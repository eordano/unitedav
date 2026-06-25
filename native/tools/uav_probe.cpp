// SPDX-License-Identifier: Apache-2.0

#include "unitedav.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>

static const char* state_name(int s) {
    switch (s) {
        case UAV_STATE_IDLE:      return "IDLE";
        case UAV_STATE_OPENING:   return "OPENING";
        case UAV_STATE_READY:     return "READY";
        case UAV_STATE_PLAYING:   return "PLAYING";
        case UAV_STATE_PAUSED:    return "PAUSED";
        case UAV_STATE_BUFFERING: return "BUFFERING";
        case UAV_STATE_FINISHED:  return "FINISHED";
        case UAV_STATE_ERROR:     return "ERROR";
        default:                  return "?";
    }
}

// Output dir for the sampled PPM frames. Hardcoding "/tmp" fails on native
// Windows (resolves to C:\tmp, often absent). Honor UAV_PROBE_OUTDIR, then the
// platform temp env vars, else the current directory.
static const char* probe_outdir() {
    const char* d;
    if ((d = std::getenv("UAV_PROBE_OUTDIR")) && *d) return d;
    if ((d = std::getenv("TMPDIR")) && *d) return d;
    if ((d = std::getenv("TEMP"))   && *d) return d;
    if ((d = std::getenv("TMP"))    && *d) return d;
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

static int run_stress(const char* url, int cycles) {
    std::printf("STRESS: %d open/seek/close cycles on %s\n", cycles, url);
    UAVPlayer* p = uav_create();
    if (!p) { std::fprintf(stderr, "uav_create failed\n"); return 1; }

    std::atomic<bool> stop{false};
    std::thread reader([&]() {
        int64_t last_id = -1;
        std::vector<float> abuf(1024 * 2);
        while (!stop.load(std::memory_order_relaxed)) {
            UAVVideoFrame vf{};
            if (uav_acquire_frame(p, last_id, &vf) == UAV_OK) {
                volatile uint8_t sink = 0;
                if (vf.data && vf.width > 0 && vf.height > 0)
                    sink = vf.data[((size_t)vf.height - 1) * vf.stride + (vf.width - 1) * 4];
                (void)sink;
                last_id = vf.frame_id;
                uav_release_frame(p);
            }
            uav_read_audio(p, abuf.data(), 1024, 2, 48000);
            (void)uav_get_state(p);
            (void)uav_get_position(p);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    for (int i = 0; i < cycles; ++i) {
        int rc = uav_open(p, url);
        if (rc == UAV_OK) {
            uav_play(p);
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            uav_seek(p, 0.5);
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            uav_seek(p, 0.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            uav_pause(p);
        }
        uav_close(p);
    }

    stop.store(true);
    reader.join();
    uav_destroy(p);
    std::printf("STRESS: done\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <url-or-path> [--stress N]\n", argv[0]);
        return 1;
    }
    const char* url = argv[1];

    if (argc >= 4 && std::strcmp(argv[2], "--stress") == 0) {
        return run_stress(url, std::atoi(argv[3]));
    }

    std::printf("uav_abi_version = %u\n", uav_abi_version());

    UAVPlayer* p = uav_create();
    if (!p) { std::fprintf(stderr, "uav_create failed\n"); return 1; }

    int rc = uav_open(p, url);
    std::printf("uav_open(%s) -> %d (state=%s, last_error=%d)\n",
                url, rc, state_name(uav_get_state(p)), uav_last_error(p));
    if (rc != UAV_OK) {
        uav_destroy(p);
        std::printf("OPEN FAILED (expected for bogus input)\n");
        return 2;
    }

    UAVMediaInfo info{};
    if (uav_get_info(p, &info) == UAV_OK) {
        std::printf("media: video=%d audio=%d %dx%d fps=%.3f dur=%.3f ach=%d arate=%d\n",
                    info.has_video, info.has_audio, info.width, info.height,
                    info.frame_rate, info.duration, info.audio_channels,
                    info.audio_sample_rate);
    } else {
        std::printf("uav_get_info: no info yet\n");
    }

    uav_play(p);
    std::printf("playing... state=%s\n", state_name(uav_get_state(p)));

    int64_t last_id = -1;
    int frames_written = 0;
    int video_polls = 0;
    const int max_video_polls = 2000;
    const char* outdir = probe_outdir();

    const int   ach = (info.audio_channels > 0) ? info.audio_channels : 2;
    const int   arate = (info.audio_sample_rate > 0) ? info.audio_sample_rate : 48000;
    const int   chunk_frames = 1024;
    std::vector<float> abuf((size_t)chunk_frames * ach);
    double amin = 1e30, amax = -1e30, asumsq = 0.0;
    long long asamples = 0;
    long long aframes_total = 0;

    while ((frames_written < 3 || asamples == 0) && video_polls < max_video_polls) {
        UAVVideoFrame vf{};
        if (info.has_video && uav_acquire_frame(p, last_id, &vf) == UAV_OK) {
            if (vf.frame_id != last_id && frames_written < 3 && vf.data && vf.width > 0) {
                char path[512];
                std::snprintf(path, sizeof(path), "%s/uav_frame_%d.ppm", outdir, frames_written);
                if (write_ppm(path, vf.data, vf.width, vf.height, vf.stride)) {
                    std::printf("  frame_id=%lld %dx%d stride=%d pts=%.3f -> %s\n",
                                (long long)vf.frame_id, vf.width, vf.height, vf.stride, vf.pts, path);
                    frames_written++;
                }
            }
            last_id = vf.frame_id;
            uav_release_frame(p);
        }

        if (info.has_audio) {
            int got = uav_read_audio(p, abuf.data(), chunk_frames, ach, arate);
            aframes_total += got;
            for (int i = 0; i < got * ach; ++i) {
                double v = abuf[i];
                if (v < amin) amin = v;
                if (v > amax) amax = v;
                asumsq += v * v;
                asamples++;
            }
        }

        video_polls++;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        if (frames_written >= 3 && (!info.has_audio || asamples > arate)) break;
        if (uav_get_state(p) == UAV_STATE_FINISHED) break;
    }

    std::printf("video frames written: %d (polls=%d)\n", frames_written, video_polls);
    std::printf("position=%.3f state=%s\n", uav_get_position(p), state_name(uav_get_state(p)));

    if (info.has_audio) {
        double rms = (asamples > 0) ? std::sqrt(asumsq / (double)asamples) : 0.0;
        std::printf("audio: frames=%lld samples=%lld min=%.5f max=%.5f rms=%.5f\n",
                    aframes_total, asamples, (asamples ? amin : 0.0),
                    (asamples ? amax : 0.0), rms);
    } else {
        std::printf("audio: none\n");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::printf("final position=%.3f state=%s\n",
                uav_get_position(p), state_name(uav_get_state(p)));

    uav_destroy(p);
    std::printf("destroyed OK\n");

    bool ok = (info.has_video ? frames_written > 0 : true);
    return ok ? 0 : 1;
}
