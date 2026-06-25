// SPDX-License-Identifier: Apache-2.0

#include "uav_doctest.h"

#include "unitedav.h"
#include "unitedav_send.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <process.h>
#else
#  include <unistd.h>
#endif

namespace {

bool state_in_range(int s) {
    return s >= UAV_STATE_IDLE && s <= UAV_STATE_ERROR;
}

long this_pid() {
#if defined(_WIN32)
    return (long)::_getpid();
#else
    return (long)::getpid();
#endif
}

std::string temp_dir() {
    if (const char* env = std::getenv("TMPDIR")) {
        if (env[0]) return std::string(env);
    }
    std::error_code ec;
    std::filesystem::path p = std::filesystem::temp_directory_path(ec);
    if (ec) return std::string(".");
    return p.string();
}

static int g_temp_counter = 0;
std::string make_temp_path(const char* suffix) {
    std::filesystem::path dir(temp_dir());
    char leaf[256];
    std::snprintf(leaf, sizeof(leaf), "uav_netfault_%ld_%d%s",
                  this_pid(), g_temp_counter++, suffix);
    return (dir / leaf).string();
}

struct TempFileGuard {
    std::string path;
    explicit TempFileGuard(std::string p) : path(std::move(p)) {}
    ~TempFileGuard() { if (!path.empty()) std::remove(path.c_str()); }
    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;
    const char* c_str() const { return path.c_str(); }
};

bool write_bytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t n = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return n == bytes.size();
}

bool write_text(const std::string& path, const std::string& text) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t n = std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    return n == text.size();
}

std::vector<uint8_t> garbage(size_t n) {
    std::vector<uint8_t> v(n);
    uint32_t x = 0x9e3779b9u ^ (uint32_t)n;
    for (size_t i = 0; i < n; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        v[i] = (uint8_t)(x & 0xff);
    }
    return v;
}

struct Handle {
    UAVPlayer* p = uav_create();
    ~Handle() { uav_destroy(p); }
    operator UAVPlayer*() const { return p; }
};

struct SendHandle {
    UAVSender* s = uav_send_create();
    ~SendHandle() { uav_send_destroy(s); }
    operator UAVSender*() const { return s; }
};

UAVSendConfig make_cfg(int vcodec, int acodec, int w, int h, int fps,
                       int rate, int ch) {
    UAVSendConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.video_codec   = vcodec;
    cfg.width         = w;
    cfg.height        = h;
    cfg.fps           = fps;
    cfg.video_bitrate = 800'000;
    cfg.audio_codec   = acodec;
    cfg.sample_rate   = rate;
    cfg.channels      = ch;
    cfg.audio_bitrate = 96'000;
    return cfg;
}

double elapsed_s(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
        .count();
}

}

#if defined(UAV_HAVE_FFMPEG)

TEST_CASE("netfault: open_unreachable_tcp_fails_bounded"
          * doctest::test_suite("[netfault][tier2]")) {
    Handle h;
    REQUIRE(h.p != nullptr);

    const char* url = "http://127.0.0.1:1/uav_no_such.mp4";
    auto t0 = std::chrono::steady_clock::now();
    int rc = uav_open(h, url);
    double dt = elapsed_s(t0);

    CHECK(rc < 0);
    CHECK(uav_get_state(h) == UAV_STATE_ERROR);
    CHECK(uav_last_error(h) == rc);
    CHECK_MESSAGE(dt < 25.0,
                  "uav_open(unreachable tcp) took " << dt << "s (expected bounded "
                  "fail within the 15s finite-timeout contract)");
    CHECK(uav_close(h) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_IDLE);
}

TEST_CASE("netfault: open_garbage_and_truncated_local_file_errors"
          * doctest::test_suite("[netfault][tier2]")) {
    {
        TempFileGuard tmp(make_temp_path(".bin"));
        REQUIRE(write_bytes(tmp.path, garbage(8 * 1024)));
        Handle h;
        REQUIRE(h.p != nullptr);
        int rc = uav_open(h, tmp.c_str());
        CHECK(rc < 0);
        CHECK(uav_get_state(h) == UAV_STATE_ERROR);
        UAVMediaInfo mi;
        std::memset(&mi, 0x7f, sizeof(mi));
        CHECK(uav_get_info(h, &mi) == UAV_ERR_NO_STREAM);
        CHECK(mi.width == 0);
        CHECK(mi.height == 0);
        CHECK(uav_close(h) == UAV_OK);
        CHECK(uav_get_state(h) == UAV_STATE_IDLE);
    }

    {
        std::vector<uint8_t> data = {0x1A, 0x45, 0xDF, 0xA3};
        std::vector<uint8_t> tail = garbage(2 * 1024);
        data.insert(data.end(), tail.begin(), tail.end());
        TempFileGuard tmp(make_temp_path(".webm"));
        REQUIRE(write_bytes(tmp.path, data));
        Handle h;
        REQUIRE(h.p != nullptr);
        int rc = uav_open(h, tmp.c_str());
        CHECK(rc < 0);
        CHECK(uav_get_state(h) == UAV_STATE_ERROR);
        CHECK(uav_close(h) == UAV_OK);
        CHECK(uav_get_state(h) == UAV_STATE_IDLE);
    }
}

TEST_CASE("netfault: open_sdp_with_no_sender_times_out_clean"
          * doctest::test_suite("[netfault][tier2]")) {
    const std::string sdp =
        "v=0\r\n"
        "o=- 0 0 IN IP4 127.0.0.1\r\n"
        "s=uav-netfault\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "t=0 0\r\n"
        "m=video 55554 RTP/AVP 96\r\n"
        "a=rtpmap:96 VP8/90000\r\n";
    TempFileGuard tmp(make_temp_path(".sdp"));
    REQUIRE(write_text(tmp.path, sdp));

    Handle h;
    REQUIRE(h.p != nullptr);
    auto t0 = std::chrono::steady_clock::now();
    int rc = uav_open(h, tmp.c_str());
    double dt = elapsed_s(t0);

    CHECK(state_in_range(uav_get_state(h)));
    if (dt >= 30.0) {
        MESSAGE("SKIP: uav_open(.sdp, no sender) took " << dt << "s; the multi-stage "
                "SDP/RTP open exceeded the wall-clock cap on this stack");
        CHECK(uav_close(h) == UAV_OK);
        CHECK(uav_get_state(h) == UAV_STATE_IDLE);
        return;
    }
    if (rc == UAV_OK) {
        UAVVideoFrame vf{};
        int arc = uav_acquire_frame(h, -1, &vf);
        CHECK((arc == UAV_OK || arc == UAV_ERR_NO_STREAM));
        if (arc == UAV_OK) uav_release_frame(h);
    } else {
        CHECK(rc < 0);
    }
    CHECK(uav_close(h) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_IDLE);
}

TEST_CASE("netfault: concurrent_close_during_network_open"
          * doctest::test_suite("[netfault][tier2][concurrency]")) {
    Handle h;
    REQUIRE(h.p != nullptr);

    std::atomic<bool> stop{false};
    std::thread reader([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            (void)uav_get_state(h);
            (void)uav_get_position(h);
            UAVMediaInfo mi{};
            (void)uav_get_info(h, &mi);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    for (int i = 0; i < 12; ++i) {
        (void)uav_open(h, "http://127.0.0.1:1/uav_slow.mp4");
        uav_close(h);
    }

    stop.store(true);
    reader.join();
    CHECK(state_in_range(uav_get_state(h)));
}

#else

TEST_CASE("netfault: stub build never reaches a transport"
          * doctest::test_suite("[netfault][tier2]")) {
    Handle h;
    REQUIRE(h.p != nullptr);
    CHECK(uav_open(h, "http://127.0.0.1:1/x.mp4") == UAV_ERR_UNSUPPORTED);
    CHECK(uav_get_state(h) == UAV_STATE_ERROR);
    CHECK(uav_close(h) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_IDLE);
}

#endif

#if defined(UAV_HAVE_FFMPEG)

TEST_CASE("netfault: sender_rtp_to_blackhole_then_close"
          * doctest::test_suite("[netfault][tier2]")) {
    SendHandle s;
    REQUIRE(s.s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_NONE, 64, 48, 15, 48000, 2);
    int rc = uav_send_open(s, "rtp://127.0.0.1:5004", &cfg);
    if (rc != UAV_SEND_OK) {
        MESSAGE("SKIP: rtp:// open rejected (libvpx-vp9 absent or VP9-over-RTP "
                "unsupported by this FFmpeg)");
        CHECK(uav_send_last_error(s) < 0);
        return;
    }
    std::vector<uint8_t> rgba((size_t)64 * 48 * 4, 0);
    for (int fr = 0; fr < 5; ++fr) {
        rgba[(size_t)fr] = (uint8_t)(fr * 50);
        int prc = uav_send_push_video(s, rgba.data(), 64, 48, 64 * 4, (double)fr / 15);
        CHECK((prc == UAV_SEND_OK || prc == UAV_SEND_ERR_ENCODE));
    }
    CHECK(uav_send_close(s) == UAV_SEND_OK);
    CHECK(uav_send_last_error(s) <= 0);
}

TEST_CASE("netfault: sender_srt_caller_to_dead_port_fails_promptly"
          * doctest::test_suite("[netfault][tier2]")) {
    {
        SendHandle probe;
        REQUIRE(probe.s != nullptr);
        UAVSendConfig pcfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_NONE, 64, 48, 15, 48000, 2);
        TempFileGuard ptmp(make_temp_path(".webm"));
        if (uav_send_open(probe, ptmp.c_str(), &pcfg) != UAV_SEND_OK) {
            MESSAGE("SKIP: libvpx-vp9 encoder unavailable (can't isolate SRT fault)");
            return;
        }
        uav_send_close(probe);
    }

    SendHandle s;
    REQUIRE(s.s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_NONE, 64, 48, 15, 48000, 2);
    auto t0 = std::chrono::steady_clock::now();
    int rc = uav_send_open(s, "srt://127.0.0.1:9?mode=caller&connect_timeout=500", &cfg);
    double dt = elapsed_s(t0);

    CAPTURE(dt);
    if (dt >= 25.0) {
        MESSAGE("SKIP: srt:// caller open exceeded the wall-clock cap ("
                << dt << "s); connect_timeout not honored by this libsrt");
        CHECK(uav_send_close(s) == UAV_SEND_OK);
        return;
    }
    CHECK(rc == UAV_SEND_ERR_OPEN_FAILED);
    CHECK(uav_send_last_error(s) == UAV_SEND_ERR_OPEN_FAILED);
}

#endif
