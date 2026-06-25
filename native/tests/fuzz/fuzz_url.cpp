// SPDX-License-Identifier: Apache-2.0

#include "unitedav.h"

#include <chrono>
#include <cstddef>
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

std::string tmp_dir() {
    std::string base;
    if (const char* env = std::getenv("TMPDIR"); env && env[0]) {
        base = env;
    } else {
        std::error_code ec;
        std::filesystem::path p = std::filesystem::temp_directory_path(ec);
        base = ec ? std::string(".") : p.string();
    }
    char leaf[128];
    std::snprintf(leaf, sizeof(leaf), "uav_fuzz_url_%ld_nodir", this_pid());
    return (std::filesystem::path(base) / leaf).string();
}

std::string sanitized_leaf(const uint8_t* data, size_t size, size_t cap) {
    std::string out;
    out.reserve(cap);
    for (size_t i = 0; i < size && out.size() < cap; ++i) {
        uint8_t b = data[i];
        if (b == 0) continue;
        if (b == '/' || b == '\\') continue;
        if (b < 0x20) continue;
        out.push_back((char)b);
    }
    return out;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    constexpr size_t kLeafCap = 256;

    const unsigned sel = (size ? data[0] : 0u) % 5u;
    const unsigned port = 49152u + ((size > 1 ? data[1] : 0u) << 1);

    const std::string leaf = sanitized_leaf(data, size, kLeafCap);
    const std::string base = tmp_dir();

    std::string url;
    switch (sel) {
        case 0:
            url = base + "/" + leaf;
            break;
        case 1:
            url = "file:" + base + "/" + leaf;
            break;
        case 2:
            url = base + "/" + leaf + ".sdp";
            break;
        case 3: {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "rtp://127.0.0.1:%u", port);
            url = std::string(buf) + (leaf.empty() ? "" : ("?" + leaf));
            break;
        }
        default: {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "rtsp://127.0.0.1:%u", port);
            url = std::string(buf) + "/" + leaf;
            break;
        }
    }

    UAVPlayer* p = uav_create();
    if (!p) return 0;

    // Contract: uav_open must return within a finite I/O ceiling; abort() is a hard finding.
    const auto t0 = std::chrono::steady_clock::now();
    int rc = uav_open(p, url.c_str());
    const double dt =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (dt >= 25.0) {
        std::fprintf(stderr,
                     "fuzz_url: uav_open hung %.1fs on url=\"%s\" "
                     "(violates the 15s finite-timeout contract)\n",
                     dt, url.c_str());
        std::abort();
    }

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
    return 0;
}
