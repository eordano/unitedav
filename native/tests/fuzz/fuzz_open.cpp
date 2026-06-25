// SPDX-License-Identifier: Apache-2.0

#include "unitedav.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#  include <io.h>
#  include <process.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace {

std::string make_temp_path() {
    static std::atomic<uint64_t> counter{0};
    std::string base;
    if (const char* tmp = std::getenv("TMPDIR"); tmp && tmp[0]) {
        base = tmp;
    } else {
        std::error_code ec;
        std::filesystem::path p = std::filesystem::temp_directory_path(ec);
        base = ec ? std::string(".") : p.string();
    }
    long pid = (long)
#if defined(_WIN32)
        ::_getpid();
#else
        ::getpid();
#endif
    unsigned long long n = counter.fetch_add(1, std::memory_order_relaxed);
    char leaf[128];
    std::snprintf(leaf, sizeof(leaf), "uav_fuzz_open_%ld_%llu.bin", pid, n);
    return (std::filesystem::path(base) / leaf).string();
}

bool write_all(const std::string& path, const uint8_t* data, size_t size) {
#if defined(_WIN32)
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = (size == 0) || (std::fwrite(data, 1, size, f) == size);
    std::fflush(f);
    std::fclose(f);
    if (!ok) std::remove(path.c_str());
    return ok;
#else
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    size_t off = 0;
    bool ok = true;
    while (off < size) {
        ssize_t w = ::write(fd, data + off, size - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        off += (size_t)w;
    }
    if (ok) ::fsync(fd);
    ::close(fd);
    if (!ok) ::unlink(path.c_str());
    return ok;
#endif
}

struct TempFileGuard {
    std::string path;
    explicit TempFileGuard(std::string p) : path(std::move(p)) {}
    ~TempFileGuard() {
        if (!path.empty()) std::remove(path.c_str());
    }
    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;
};

void run_decode_loop(UAVPlayer* p, const uint8_t* data, size_t size) {
    uav_play(p);

    int64_t last_id = 0;
    float audio_buf[1024 * 2];

    constexpr int kMaxIters = 64;
    for (int i = 0; i < kMaxIters; ++i) {
        int state = uav_get_state(p);
        if (state == UAV_STATE_FINISHED || state == UAV_STATE_ERROR) break;

        UAVVideoFrame vf;
        if (uav_acquire_frame(p, last_id, &vf) == UAV_OK) {
            if (vf.data && vf.height > 0 && vf.stride > 0) {
                volatile uint8_t sink = vf.data[0];
                size_t lastoff = (size_t)vf.stride * (size_t)(vf.height - 1);
                sink ^= vf.data[lastoff];
                (void)sink;
            }
            if (vf.frame_id > last_id) last_id = vf.frame_id;
            uav_release_frame(p);
        }

        uav_read_audio(p, audio_buf, 1024, 2, 48000);

        if (size > 0 && (i & 7) == 0) {
            unsigned char b = data[(size_t)i % size];
            double secs = (double)b * 0.05;
            uav_seek(p, secs);
            UAVMediaInfo info;
            uav_get_info(p, &info);
            (void)uav_get_position(p);
        }
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string path = make_temp_path();
    TempFileGuard guard(path);

    const bool wrote = write_all(path, data, size);

    UAVPlayer* p = uav_create();
    if (!p) return 0;

    if (wrote) {
        int rc = uav_open(p, path.c_str());
        if (rc == UAV_OK) {
            run_decode_loop(p, data, size);
        }
    }

    uav_close(p);
    uav_destroy(p);

    return 0;
}
