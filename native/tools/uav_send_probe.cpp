// SPDX-License-Identifier: Apache-2.0

#include "unitedav_send.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static int parse_codec_v(const char* s) {
    if (!std::strcmp(s, "vp9"))  return UAV_VCODEC_VP9;
    if (!std::strcmp(s, "vp8"))  return UAV_VCODEC_VP8;
    if (!std::strcmp(s, "av1"))  return UAV_VCODEC_AV1;
    if (!std::strcmp(s, "none")) return UAV_VCODEC_NONE;
    return -1;
}
static int parse_codec_a(const char* s) {
    if (!std::strcmp(s, "opus")) return UAV_ACODEC_OPUS;
    if (!std::strcmp(s, "none")) return UAV_ACODEC_NONE;
    return -1;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <out-url> [seconds] [--sdp PATH] [--vcodec vp9|vp8|av1|none]"
            " [--acodec opus|none] [--size WxH] [--fps N]\n", argv[0]);
        return 1;
    }
    const char* url = argv[1];
    double seconds = 2.0;
    std::string sdp_path = "/tmp/up.sdp";
    int vcodec = UAV_VCODEC_VP9;
    int acodec = UAV_ACODEC_OPUS;
    int width = 640, height = 480, fps = 30;

    int argi = 2;
    if (argi < argc && argv[argi][0] != '-') { seconds = std::atof(argv[argi]); argi++; }
    for (; argi < argc; ++argi) {
        if (!std::strcmp(argv[argi], "--sdp") && argi + 1 < argc) { sdp_path = argv[++argi]; }
        else if (!std::strcmp(argv[argi], "--vcodec") && argi + 1 < argc) {
            vcodec = parse_codec_v(argv[++argi]);
            if (vcodec < 0) { std::fprintf(stderr, "bad --vcodec\n"); return 1; }
        }
        else if (!std::strcmp(argv[argi], "--acodec") && argi + 1 < argc) {
            acodec = parse_codec_a(argv[++argi]);
            if (acodec < 0) { std::fprintf(stderr, "bad --acodec\n"); return 1; }
        }
        else if (!std::strcmp(argv[argi], "--size") && argi + 1 < argc) {
            if (std::sscanf(argv[++argi], "%dx%d", &width, &height) != 2) {
                std::fprintf(stderr, "bad --size\n"); return 1;
            }
        }
        else if (!std::strcmp(argv[argi], "--fps") && argi + 1 < argc) { fps = std::atoi(argv[++argi]); }
        else { std::fprintf(stderr, "unknown arg: %s\n", argv[argi]); return 1; }
    }

    std::printf("uav_send_abi_version = %u\n", uav_send_abi_version());
    std::printf("send: url=%s %dx%d fps=%d vcodec=%d acodec=%d %.2fs\n",
                url, width, height, fps, vcodec, acodec, seconds);

    UAVSender* s = uav_send_create();
    if (!s) { std::fprintf(stderr, "uav_send_create failed\n"); return 1; }

    UAVSendConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.video_codec   = vcodec;
    cfg.width         = width;
    cfg.height        = height;
    cfg.fps           = fps;
    cfg.video_bitrate = 2'000'000;
    cfg.audio_codec   = acodec;
    cfg.sample_rate   = 48000;
    cfg.channels      = 2;
    cfg.audio_bitrate = 96000;

    int rc = uav_send_open(s, url, &cfg);
    std::printf("uav_send_open -> %d (last_error=%d)\n", rc, uav_send_last_error(s));
    if (rc != UAV_SEND_OK) { uav_send_destroy(s); return 1; }

    if (std::strncmp(url, "rtp://", 6) == 0) {
        std::vector<char> sdp(8192);
        int n = uav_send_get_sdp(s, sdp.data(), (int)sdp.size());
        if (n > 0) {
            FILE* f = std::fopen(sdp_path.c_str(), "wb");
            if (f) { std::fwrite(sdp.data(), 1, std::strlen(sdp.data()), f); std::fclose(f); }
            std::printf("wrote SDP (%d bytes) -> %s\n", n, sdp_path.c_str());
            std::printf("---SDP---\n%s\n---------\n", sdp.data());
        } else {
            std::fprintf(stderr, "uav_send_get_sdp failed (%d)\n", n);
        }
    }

    const int total_frames = (int)std::lround(seconds * fps);
    const int arate = 48000, ach = 2;
    const int samples_per_video = arate / fps;

    std::vector<uint8_t> rgba((size_t)width * height * 4);
    std::vector<float>   pcm((size_t)samples_per_video * ach);

    double aphase = 0.0;
    const double freq = 440.0;
    long long audio_idx = 0;
    int v_ok = 0, a_ok = 0;

    const bool live = (std::strncmp(url, "srt://", 6) == 0) ||
                      (std::strncmp(url, "rtp://", 6) == 0);
    const auto t0 = std::chrono::steady_clock::now();

    for (int fr = 0; fr < total_frames; ++fr) {
        double pts = (double)fr / fps;

        if (live) {
            auto due = t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                std::chrono::duration<double>(pts));
            std::this_thread::sleep_until(due);
        }

        if (vcodec != UAV_VCODEC_NONE) {
            int bx = (fr * 7) % (width  > 32 ? width  - 32 : 1);
            int by = (fr * 5) % (height > 32 ? height - 32 : 1);
            for (int y = 0; y < height; ++y) {
                uint8_t* row = rgba.data() + (size_t)y * width * 4;
                for (int x = 0; x < width; ++x) {
                    uint8_t r = (uint8_t)((x + fr * 3) & 0xff);
                    uint8_t g = (uint8_t)((y + fr * 2) & 0xff);
                    uint8_t b = (uint8_t)((x + y + fr * 5) & 0xff);
                    if (x >= bx && x < bx + 32 && y >= by && y < by + 32) {
                        r = g = b = 255;
                    }
                    row[x * 4 + 0] = r;
                    row[x * 4 + 1] = g;
                    row[x * 4 + 2] = b;
                    row[x * 4 + 3] = 255;
                }
            }
            if (uav_send_push_video(s, rgba.data(), width, height, width * 4, pts) == UAV_SEND_OK)
                v_ok++;
            else
                std::fprintf(stderr, "push_video failed at fr=%d (err=%d)\n", fr, uav_send_last_error(s));
        }

        if (acodec != UAV_ACODEC_NONE) {
            for (int i = 0; i < samples_per_video; ++i) {
                float v = 0.5f * (float)std::sin(aphase);
                aphase += 2.0 * M_PI * freq / arate;
                if (aphase > 2.0 * M_PI) aphase -= 2.0 * M_PI;
                pcm[i * 2 + 0] = v;
                pcm[i * 2 + 1] = v;
            }
            double apts = (double)audio_idx / arate;
            if (uav_send_push_audio(s, pcm.data(), samples_per_video, ach, arate, apts) == UAV_SEND_OK)
                a_ok++;
            else
                std::fprintf(stderr, "push_audio failed at fr=%d (err=%d)\n", fr, uav_send_last_error(s));
            audio_idx += samples_per_video;
        }
    }

    std::printf("pushed: video_ok=%d audio_ok=%d (of %d frames)\n", v_ok, a_ok, total_frames);

    int crc = uav_send_close(s);
    std::printf("uav_send_close -> %d (last_error=%d)\n", crc, uav_send_last_error(s));
    uav_send_destroy(s);
    std::printf("send done\n");
    return (crc == UAV_SEND_OK) ? 0 : 1;
}
