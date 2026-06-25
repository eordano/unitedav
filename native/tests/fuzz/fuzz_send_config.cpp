// SPDX-License-Identifier: Apache-2.0

#include "unitedav_send.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <process.h>
#else
#  include <unistd.h>
#endif

namespace {

long this_pid() {
#if defined(_WIN32)
    return (long)::_getpid();
#else
    return (long)::getpid();
#endif
}

std::string unique_webm_path() {
    static unsigned long counter = 0;
    std::string dir;
    if (const char* env = std::getenv("TMPDIR"); env && env[0]) {
        dir = env;
    } else {
        std::error_code ec;
        std::filesystem::path p = std::filesystem::temp_directory_path(ec);
        dir = ec ? std::string(".") : p.string();
    }
    char leaf[128];
    std::snprintf(leaf, sizeof(leaf), "uav_fuzz_send_%ld_%lu.webm",
                  this_pid(), counter++);
    return (std::filesystem::path(dir) / leaf).string();
}

uint32_t take_u32(const uint8_t* d, size_t len, size_t off) {
    uint32_t v = 0;
    for (size_t i = 0; i < 4; ++i)
        if (off + i < len) v |= (uint32_t)d[off + i] << (8 * i);
    return v;
}

int clamp_dim(uint32_t raw, int lo, int hi) {
    int span = hi - lo + 1;
    return lo + (int)(raw % (uint32_t)span);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    UAVSendConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));

    cfg.video_codec   = (int)(take_u32(data, size, 0) % 5);
    cfg.audio_codec   = (int)(take_u32(data, size, 4) % 3);

    cfg.width         = clamp_dim(take_u32(data, size, 8),  2, 96) & ~1;
    cfg.height        = clamp_dim(take_u32(data, size, 12), 2, 96) & ~1;
    cfg.fps           = clamp_dim(take_u32(data, size, 16), 1, 60);
    cfg.video_bitrate = clamp_dim(take_u32(data, size, 20), 0, 1'000'000);
    cfg.sample_rate   = (take_u32(data, size, 24) & 1) ? 44100 : 48000;
    cfg.channels      = (int)(1 + (take_u32(data, size, 28) & 1));
    cfg.audio_bitrate = clamp_dim(take_u32(data, size, 32), 0, 256'000);

    if (cfg.width < 2)  cfg.width = 2;
    if (cfg.height < 2) cfg.height = 2;

    UAVSender* s = uav_send_create();
    if (!s) return 0;

    std::string path = unique_webm_path();
    int rc = uav_send_open(s, path.c_str(), &cfg);
    if (rc != UAV_SEND_OK) {
        uav_send_close(s);
        uav_send_destroy(s);
        std::remove(path.c_str());
        return 0;
    }

    {
        const int w = cfg.width, h = cfg.height;
        std::vector<uint8_t> rgba((size_t)w * (size_t)h * 4);
        for (size_t i = 0; i < rgba.size(); ++i)
            rgba[i] = size ? data[i % size] : (uint8_t)i;
        int stride = w * 4 + (int)(take_u32(data, size, 36) % 64);
        if (((take_u32(data, size, 40)) & 1) == 0) stride = w * 4;
        if ((size_t)stride * (size_t)h > rgba.size()) {
            rgba.assign((size_t)stride * (size_t)h, 0);
            for (size_t i = 0; i < rgba.size(); ++i)
                rgba[i] = size ? data[i % size] : (uint8_t)i;
        }
        (void)uav_send_push_video(s, rgba.data(), w, h, stride, 0.0);
    }

    {
        const int ch = cfg.channels > 0 ? cfg.channels : 2;
        const int frames = 256;
        std::vector<float> pcm((size_t)frames * (size_t)ch);
        for (size_t i = 0; i < pcm.size(); ++i) {
            uint8_t b = size ? data[(i + 7) % size] : (uint8_t)i;
            pcm[i] = ((float)b / 127.5f) - 1.0f;
        }
        const int srate = cfg.sample_rate > 0 ? cfg.sample_rate : 48000;
        (void)uav_send_push_audio(s, pcm.data(), frames, ch, srate, 0.0);
    }

    uav_send_close(s);
    uav_send_destroy(s);
    std::remove(path.c_str());
    return 0;
}
