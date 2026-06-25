// SPDX-License-Identifier: Apache-2.0
// Windows D3D11 zero-copy verifier: decode via d3d11va into an NV12 GPU surface,
// convert NV12->RGBA on the GPU (D3d11Converter), read back, and compare to a
// software oracle (av_hwframe_transfer_data + swscale). Mirrors uav_gpu_probe.
// Falls back to software decode (CPU NV12 upload) if d3d11va is unavailable, so
// the converter is still verified.

#include "../src/gpu/d3d11_convert.hpp"
#include "../src/gpu/hwaccel.hpp"

#include <d3d11.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libswscale/swscale.h>
}

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static const char* outdir() {
    const char* d;
    if ((d = std::getenv("UAV_PROBE_OUTDIR")) && *d) return d;
    if ((d = std::getenv("TEMP")) && *d) return d;
    if ((d = std::getenv("TMP"))  && *d) return d;
    return ".";
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

// Mean/max abs RGB diff vs an RGBA oracle; updates running totals + writes PPMs.
static void compare(const std::vector<uint8_t>& gpu, const std::vector<uint8_t>& sw,
                    int w, int h, int idx, int cs, int range,
                    double& sum_mean, int& max_abs, int& converted) {
    long long cdsum[3] = {0,0,0}; int cdmax[3] = {0,0,0};
    for (int i = 0; i < w * h; ++i)
        for (int c = 0; c < 3; ++c) {
            int d = std::abs((int)gpu[i*4+c] - (int)sw[i*4+c]);
            cdsum[c] += d; if (d > cdmax[c]) cdmax[c] = d; if (d > max_abs) max_abs = d;
        }
    double mean = (double)(cdsum[0]+cdsum[1]+cdsum[2]) / (3.0 * w * h);
    sum_mean += mean;
    std::printf("  frame %d  %dx%d  cs=%d range=%d  mean|d|=%.3f  max|d| R=%d G=%d B=%d\n",
                idx, w, h, cs, range, mean, cdmax[0], cdmax[1], cdmax[2]);
    char p1[512], p2[512];
    std::snprintf(p1, sizeof(p1), "%s/uav_d3d11_%d.ppm", outdir(), idx);
    std::snprintf(p2, sizeof(p2), "%s/uav_d3d11_sw_%d.ppm", outdir(), idx);
    write_ppm(p1, gpu.data(), w, h, w * 4);
    write_ppm(p2, sw.data(),  w, h, w * 4);
    converted++;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <clip> [num_frames]\n", argv[0]); return 1; }
    const char* url = argv[1];
    const int want_frames = (argc >= 3) ? std::atoi(argv[2]) : 3;

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, url, nullptr, nullptr) < 0) { std::fprintf(stderr, "open failed: %s\n", url); return 1; }
    if (avformat_find_stream_info(fmt, nullptr) < 0) { std::fprintf(stderr, "find_stream_info failed\n"); avformat_close_input(&fmt); return 1; }
    int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) { std::fprintf(stderr, "no video stream\n"); avformat_close_input(&fmt); return 1; }
    const AVCodec* dec = avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
    if (!dec) { std::fprintf(stderr, "no decoder\n"); avformat_close_input(&fmt); return 1; }

    AVCodecContext* vdec = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(vdec, fmt->streams[vs]->codecpar);

    uav::HwDecode hw;
    const bool hw_ok = hw.enable(vdec, AV_HWDEVICE_TYPE_D3D11VA, nullptr);
    if (avcodec_open2(vdec, dec, nullptr) < 0) {
        std::fprintf(stderr, "avcodec_open2 failed\n");
        avcodec_free_context(&vdec); avformat_close_input(&fmt); return 1;
    }

    uav::gpu::D3d11Converter conv;
    AVD3D11VADeviceContext* d3d = nullptr;
    if (hw_ok && vdec->hw_device_ctx) {
        auto* dctx = reinterpret_cast<AVHWDeviceContext*>(vdec->hw_device_ctx->data);
        d3d = reinterpret_cast<AVD3D11VADeviceContext*>(dctx->hwctx);
    }
    if (d3d) {
        if (!conv.init(d3d->device, d3d->device_context)) {
            std::fprintf(stderr, "FAIL: D3d11Converter::init(ffmpeg device): %s\n", conv.last_error());
            avcodec_free_context(&vdec); avformat_close_input(&fmt); return 1;
        }
        std::printf("D3D11 zero-copy: d3d11va decode + converter share FFmpeg's device\n");
    } else {
        if (!conv.init(nullptr)) {
            std::fprintf(stderr, "FAIL: D3d11Converter::init(standalone): %s\n", conv.last_error());
            avcodec_free_context(&vdec); avformat_close_input(&fmt); return 1;
        }
        std::printf("D3D11 fallback: software decode + CPU-NV12 upload (d3d11va unavailable)\n");
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame*  f   = av_frame_alloc();
    AVFrame*  swf = av_frame_alloc();
    SwsContext* sws_rgba = nullptr; int sr_w = 0, sr_h = 0, sr_fmt = -1;
    SwsContext* sws_nv12 = nullptr; int sn_w = 0, sn_h = 0, sn_fmt = -1;

    double sum_mean = 0.0; int max_abs = 0; int converted = 0;
    bool any_hw = false; bool eof = false;

    auto lock   = [&]{ if (d3d) d3d->lock(d3d->lock_ctx); };
    auto unlock = [&]{ if (d3d) d3d->unlock(d3d->lock_ctx); };

    auto process = [&](AVFrame* fr) -> void {
        const int w = fr->width, h = fr->height;
        std::vector<uint8_t> gpu, sw((size_t)w * h * 4, 0);

        if (d3d && hw.is_hw_frame(fr)) {
            any_hw = true;
            auto* tex = reinterpret_cast<ID3D11Texture2D*>(fr->data[0]);
            const int slice = (int)(intptr_t)fr->data[1];
            lock();
            ID3D11Texture2D* rgba = conv.convert(tex, slice, w, h, fr->colorspace, fr->color_range);
            bool rb = rgba && conv.readback(gpu);
            unlock();
            if (!rb) { std::fprintf(stderr, "convert/readback failed: %s\n", conv.last_error()); return; }

            // Oracle: download the same HW surface and swscale NV12->RGBA.
            av_frame_unref(swf);
            lock();
            int trc = av_hwframe_transfer_data(swf, fr, 0);
            unlock();
            if (trc < 0) { std::fprintf(stderr, "hwframe_transfer (oracle) failed\n"); return; }
            if (!sws_rgba || sr_w != w || sr_h != h || sr_fmt != swf->format) {
                if (sws_rgba) sws_freeContext(sws_rgba);
                sws_rgba = sws_getContext(w, h, (AVPixelFormat)swf->format, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                sr_w = w; sr_h = h; sr_fmt = swf->format;
            }
            uint8_t* dd[4] = { sw.data(), nullptr, nullptr, nullptr }; int dl[4] = { w*4, 0,0,0 };
            sws_scale(sws_rgba, swf->data, swf->linesize, 0, h, dd, dl);
        } else {
            // Fallback: this is a CPU frame. Build NV12 for the converter + RGBA oracle.
            if (!sws_nv12 || sn_w != w || sn_h != h || sn_fmt != fr->format) {
                if (sws_nv12) sws_freeContext(sws_nv12);
                sws_nv12 = sws_getContext(w, h, (AVPixelFormat)fr->format, w, h, AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
                sn_w = w; sn_h = h; sn_fmt = fr->format;
            }
            std::vector<uint8_t> yb((size_t)w * h), uvb((size_t)w * (h/2));
            uint8_t* nd[4] = { yb.data(), uvb.data(), nullptr, nullptr }; int nl[4] = { w, w, 0, 0 };
            sws_scale(sws_nv12, fr->data, fr->linesize, 0, h, nd, nl);

            ID3D11Texture2D* rgba = conv.convert_cpu_nv12(yb.data(), w, uvb.data(), w, w, h, fr->colorspace, fr->color_range);
            if (!rgba || !conv.readback(gpu)) { std::fprintf(stderr, "convert/readback failed: %s\n", conv.last_error()); return; }

            if (!sws_rgba || sr_w != w || sr_h != h || sr_fmt != fr->format) {
                if (sws_rgba) sws_freeContext(sws_rgba);
                sws_rgba = sws_getContext(w, h, (AVPixelFormat)fr->format, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                sr_w = w; sr_h = h; sr_fmt = fr->format;
            }
            uint8_t* dd[4] = { sw.data(), nullptr, nullptr, nullptr }; int dl[4] = { w*4, 0,0,0 };
            sws_scale(sws_rgba, fr->data, fr->linesize, 0, h, dd, dl);
        }
        compare(gpu, sw, w, h, converted, fr->colorspace, fr->color_range, sum_mean, max_abs, converted);
    };

    while (converted < want_frames && !eof) {
        int rc = av_read_frame(fmt, pkt);
        if (rc < 0) { avcodec_send_packet(vdec, nullptr); eof = true; }
        else if (pkt->stream_index != vs) { av_packet_unref(pkt); continue; }
        else { avcodec_send_packet(vdec, pkt); av_packet_unref(pkt); }
        while (converted < want_frames) {
            int r = avcodec_receive_frame(vdec, f);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF || r < 0) break;
            process(f);
            av_frame_unref(f);
        }
    }

    if (sws_rgba) sws_freeContext(sws_rgba);
    if (sws_nv12) sws_freeContext(sws_nv12);
    av_frame_free(&f); av_frame_free(&swf); av_packet_free(&pkt);
    avcodec_free_context(&vdec); avformat_close_input(&fmt);

    if (converted == 0) { std::fprintf(stderr, "FAIL: converted 0 frames\n"); return 1; }
    const double overall = sum_mean / converted;
    std::printf("\nSUMMARY: converted %d frame(s)  overall mean|d|=%.3f  global max|d|=%d  (%s)\n",
                converted, overall, max_abs, any_hw ? "d3d11va zero-copy" : "cpu-nv12 fallback");
    std::printf("PPMs in %s: uav_d3d11_*.ppm (GPU)  uav_d3d11_sw_*.ppm (oracle)\n", outdir());

    const double kThresh = 6.0;
    if (overall > kThresh) { std::fprintf(stderr, "FAIL: mean diff %.3f exceeds %.1f\n", overall, kThresh); return 3; }
    std::printf("PASS: D3D11 NV12->RGBA GPU convert matches SW oracle (mean|d|=%.3f < %.1f)%s\n",
                overall, kThresh, any_hw ? " [zero-copy]" : " [converter-only]");
    return 0;
}
