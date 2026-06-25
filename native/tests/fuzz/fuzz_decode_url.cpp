// SPDX-License-Identifier: Apache-2.0

#include "unitedav.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

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

std::string unique_tmp_path() {
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
    std::snprintf(leaf, sizeof(leaf), "uav_fuzz_decode_%ld_%lu.bin",
                  this_pid(), counter++);
    return (std::filesystem::path(dir) / leaf).string();
}

bool looks_like_network(const uint8_t* d, size_t n) {
    static const char* schemes[] = {
        "rtp:", "rtsp:", "http:", "https:", "srt:", "udp:", "tcp:",
        "ftp:", "rtmp:", "tls:", "sftp:", "gopher:", "data:"};
    for (const char* sc : schemes) {
        size_t l = std::strlen(sc);
        if (n >= l && std::memcmp(d, sc, l) == 0) return true;
    }
    return false;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (looks_like_network(data, size)) return 0;

    const size_t kMaxBytes = 64 * 1024;
    size_t n = size < kMaxBytes ? size : kMaxBytes;

    std::string path = unique_tmp_path();
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return 0;
        if (n) std::fwrite(data, 1, n, f);
        std::fclose(f);
    }

    UAVPlayer* p = uav_create();
    if (!p) { std::remove(path.c_str()); return 0; }

    int rc = uav_open(p, path.c_str());
    if (rc == UAV_OK) {
        UAVMediaInfo mi{};
        (void)uav_get_info(p, &mi);
        (void)uav_get_state(p);
    } else {
        UAVMediaInfo mi{};
        (void)uav_get_info(p, &mi);
        (void)uav_get_state(p);
    }

    uav_close(p);
    uav_destroy(p);
    std::remove(path.c_str());
    return 0;
}
