// SPDX-License-Identifier: Apache-2.0
// Vulkan zero-copy GPU convert probe: VAAPI HW decode -> VkConverter (dmabuf
// import -> compute NV12->RGBA -> readback) compared against a swscale SW oracle.
// PASS if overall mean|d| < 6.0. Mirrors uav_gpu_probe.cpp.

#include "../src/gpu/vk_convert.hpp"
#include "../src/gpu/hwaccel.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static std::string temp_dir() {
    if (const char* o = std::getenv("UAV_PROBE_OUTDIR")) return o;
    if (const char* t = std::getenv("TMPDIR")) return t;
    if (const char* t = std::getenv("TEMP")) return t;
    return "/tmp";
}

static bool write_ppm(const char* path, const uint8_t* rgba, int w, int h, int stride) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> row((size_t)w * 3);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = rgba + (size_t)y * stride;
        for (int x = 0; x < w; ++x) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <clip> [num_frames]\n", argv[0]);
        return 1;
    }
    const char* url = argv[1];
    const int want_frames = (argc >= 3) ? std::atoi(argv[2]) : 3;
    const std::string outdir = temp_dir();

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, url, nullptr, nullptr) < 0) {
        std::fprintf(stderr, "open failed: %s\n", url); return 1;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        std::fprintf(stderr, "find_stream_info failed\n"); avformat_close_input(&fmt); return 1;
    }
    int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) { std::fprintf(stderr, "no video stream\n"); avformat_close_input(&fmt); return 1; }

    const AVCodec* dec = avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
    if (!dec) { std::fprintf(stderr, "no decoder\n"); avformat_close_input(&fmt); return 1; }
    AVCodecContext* vdec = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(vdec, fmt->streams[vs]->codecpar);

    uav::HwDecode hw;
    if (!hw.enable(vdec, AV_HWDEVICE_TYPE_VAAPI, nullptr)) {
        std::fprintf(stderr, "FAIL: VAAPI hw decode not available for this codec/host\n");
        avcodec_free_context(&vdec); avformat_close_input(&fmt); return 1;
    }
    if (avcodec_open2(vdec, dec, nullptr) < 0) {
        std::fprintf(stderr, "avcodec_open2 failed\n");
        avcodec_free_context(&vdec); avformat_close_input(&fmt); return 1;
    }
    std::printf("VAAPI hardware decode enabled (codec=%s)\n", dec->name);

    uav::gpu::VkConverter conv;
    if (!conv.init(nullptr)) {
        std::fprintf(stderr, "FAIL: VkConverter::init: %s\n", conv.last_error());
        avcodec_free_context(&vdec); avformat_close_input(&fmt); return 1;
    }
    std::printf("Vulkan dmabuf->compute converter up (renderD128)\n");

    AVPacket* pkt = av_packet_alloc();
    AVFrame*  hwf = av_frame_alloc();
    AVFrame*  swf = av_frame_alloc();
    SwsContext* sws = nullptr;
    int sws_w = 0, sws_h = 0, sws_fmt = -1;

    double sum_mean = 0.0; int max_abs = 0; long long total_px = 0;
    int converted = 0;
    bool any_hw_frame = false;

    auto process = [&](AVFrame* f) -> bool {
        if (!hw.is_hw_frame(f)) return false;
        any_hw_frame = true;
        const int w = f->width, h = f->height;

        uint64_t tex = conv.convert(f, w, h);
        if (!tex) { std::fprintf(stderr, "convert failed: %s\n", conv.last_error()); return false; }
        std::vector<uint8_t> gpu;
        if (!conv.readback(gpu)) { std::fprintf(stderr, "readback failed: %s\n", conv.last_error()); return false; }

        av_frame_unref(swf);
        if (av_hwframe_transfer_data(swf, f, 0) < 0) { std::fprintf(stderr, "hwframe_transfer (oracle) failed\n"); return false; }
        if (!sws || sws_w != w || sws_h != h || sws_fmt != swf->format) {
            if (sws) sws_freeContext(sws);
            sws = sws_getContext(w, h, (AVPixelFormat)swf->format, w, h, AV_PIX_FMT_RGBA,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
            sws_w = w; sws_h = h; sws_fmt = swf->format;
        }
        std::vector<uint8_t> sw((size_t)w * h * 4, 0);
        uint8_t* dd[4] = { sw.data(), nullptr, nullptr, nullptr };
        int dl[4] = { w * 4, 0, 0, 0 };
        sws_scale(sws, swf->data, swf->linesize, 0, h, dd, dl);

        long long cdsum[3] = {0,0,0}; int cdmax[3] = {0,0,0};
        for (int i = 0; i < w * h; ++i) {
            for (int c = 0; c < 3; ++c) {
                int dv = std::abs((int)gpu[i*4+c] - (int)sw[i*4+c]);
                cdsum[c] += dv; if (dv > cdmax[c]) cdmax[c] = dv; if (dv > max_abs) max_abs = dv;
            }
        }
        double mean = (double)(cdsum[0]+cdsum[1]+cdsum[2]) / (3.0 * w * h);
        sum_mean += mean; total_px += w * h;
        std::printf("  frame %d  %dx%d  cs=%d range=%d  mean|d|=%.3f  "
                    "max|d| R=%d G=%d B=%d\n",
                    converted, w, h, f->colorspace, f->color_range, mean,
                    cdmax[0], cdmax[1], cdmax[2]);

        char p1[512], p2[512];
        std::snprintf(p1, sizeof(p1), "%s/uav_vk_%d.ppm", outdir.c_str(), converted);
        std::snprintf(p2, sizeof(p2), "%s/uav_sw_%d.ppm", outdir.c_str(), converted);
        write_ppm(p1, gpu.data(), w, h, w * 4);
        write_ppm(p2, sw.data(), w, h, w * 4);
        converted++;
        return true;
    };

    bool eof = false;
    while (converted < want_frames && !eof) {
        int rc = av_read_frame(fmt, pkt);
        if (rc < 0) { avcodec_send_packet(vdec, nullptr); eof = true; }
        else if (pkt->stream_index != vs) { av_packet_unref(pkt); continue; }
        else { avcodec_send_packet(vdec, pkt); av_packet_unref(pkt); }

        while (converted < want_frames) {
            int r = avcodec_receive_frame(vdec, hwf);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF || r < 0) break;
            process(hwf);
            av_frame_unref(hwf);
        }
    }

    if (sws) sws_freeContext(sws);
    av_frame_free(&hwf);
    av_frame_free(&swf);
    av_packet_free(&pkt);
    avcodec_free_context(&vdec);
    avformat_close_input(&fmt);

    if (!any_hw_frame) {
        std::fprintf(stderr, "FAIL: decoder produced no VAAPI (on-GPU) frames\n");
        return 1;
    }
    if (converted == 0) {
        std::fprintf(stderr, "FAIL: converted 0 frames\n");
        return 1;
    }

    const double overall_mean = sum_mean / converted;
    std::printf("\nSUMMARY: converted %d frame(s)  overall mean|d|=%.3f  global max|d|=%d\n",
                converted, overall_mean, max_abs);
    std::printf("PPMs: %s/uav_vk_*.ppm (GPU)  %s/uav_sw_*.ppm (SW oracle)\n",
                outdir.c_str(), outdir.c_str());

    const double kThresh = 6.0;
    if (overall_mean > kThresh) {
        std::fprintf(stderr, "FAIL: mean diff %.3f exceeds threshold %.1f\n", overall_mean, kThresh);
        return 3;
    }
    std::printf("PASS: zero-copy GPU convert matches SW oracle (mean|d|=%.3f < %.1f)\n",
                overall_mean, kThresh);
    return 0;
}
