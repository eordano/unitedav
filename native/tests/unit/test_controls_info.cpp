// SPDX-License-Identifier: Apache-2.0

#include "uav_doctest.h"

#include "unitedav.h"

#include <cstring>

#if defined(UAV_HAVE_FFMPEG)
#include "unitedav_send.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

constexpr int    kW        = 320;
constexpr int    kH        = 240;
constexpr int    kFps      = 30;
constexpr int    kFrames   = 60;
constexpr double kDuration = (double)kFrames / (double)kFps;
constexpr int    kChannels = 2;
constexpr int    kRate     = 48000;
constexpr double kSineHz   = 440.0;

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int wait_for_state_at_least(UAVPlayer* p, int want, int timeout_ms) {
    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    int s = uav_get_state(p);
    while (s < want && s != UAV_STATE_ERROR && clock::now() < deadline) {
        sleep_ms(5);
        s = uav_get_state(p);
    }
    return s;
}

int wait_for_state_eq(UAVPlayer* p, int want, int timeout_ms) {
    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    int s = uav_get_state(p);
    while (s != want && s != UAV_STATE_ERROR && clock::now() < deadline) {
        sleep_ms(5);
        s = uav_get_state(p);
    }
    return s;
}

static int g_tmp_counter = 0;

std::string temp_dir() {
    if (const char* env = std::getenv("TMPDIR")) {
        if (env[0]) return std::string(env);
    }
    std::error_code ec;
    std::filesystem::path p = std::filesystem::temp_directory_path(ec);
    if (ec) return std::string(".");
    return p.string();
}

std::string tmp_path(const char* name) {
    long pid = (long)
#if defined(_WIN32)
        ::_getpid();
#else
        ::getpid();
#endif
    std::filesystem::path p = std::filesystem::path(temp_dir());
    char leaf[256];
    std::snprintf(leaf, sizeof(leaf), "uav_ctrl_%ld_%d_%s",
                  pid, g_tmp_counter++, name);
    return (p / leaf).string();
}

int make_fixture(const std::string& path, bool want_audio) {
    UAVSender* s = uav_send_create();
    if (!s) return UAV_SEND_ERR_NOMEM;

    UAVSendConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.video_codec   = UAV_VCODEC_VP9;
    cfg.width         = kW;
    cfg.height        = kH;
    cfg.fps           = kFps;
    cfg.video_bitrate = 800000;
    cfg.audio_codec   = want_audio ? UAV_ACODEC_OPUS : UAV_ACODEC_NONE;
    cfg.sample_rate   = kRate;
    cfg.channels      = kChannels;
    cfg.audio_bitrate = 96000;

    int rc = uav_send_open(s, path.c_str(), &cfg);
    if (rc != UAV_SEND_OK) { uav_send_destroy(s); return rc; }

    std::vector<uint8_t> rgba((size_t)kW * kH * 4, 0);

    const int   asamples_per_frame = kRate / kFps;
    std::vector<float> apcm((size_t)asamples_per_frame * kChannels, 0.0f);
    int64_t aphase = 0;

    for (int f = 0; f < kFrames; ++f) {
        std::memset(rgba.data(), 0, rgba.size());
        const int bx = (f * 4) % (kW - 32);
        const int by = (f * 3) % (kH - 32);
        for (int y = by; y < by + 32; ++y) {
            uint8_t* row = rgba.data() + (size_t)y * kW * 4;
            for (int x = bx; x < bx + 32; ++x) {
                uint8_t* px = row + (size_t)x * 4;
                px[0] = 255; px[1] = 255; px[2] = 255; px[3] = 255;
            }
        }
        const double pts = (double)f / (double)kFps;
        rc = uav_send_push_video(s, rgba.data(), kW, kH, kW * 4, pts);
        if (rc != UAV_SEND_OK) { uav_send_destroy(s); return rc; }

        if (want_audio) {
            for (int i = 0; i < asamples_per_frame; ++i) {
                float v = 0.5f * (float)std::sin(2.0 * M_PI * kSineHz *
                                                 (double)aphase / (double)kRate);
                apcm[(size_t)i * kChannels + 0] = v;
                apcm[(size_t)i * kChannels + 1] = v;
                ++aphase;
            }
            rc = uav_send_push_audio(s, apcm.data(), asamples_per_frame,
                                     kChannels, kRate, pts);
            if (rc != UAV_SEND_OK) { uav_send_destroy(s); return rc; }
        }
    }

    rc = uav_send_close(s);
    uav_send_destroy(s);
    return rc;
}

struct Fixture {
    std::string path;
    int         rc;
    bool        usable;
    Fixture(const char* name, bool want_audio) : path(tmp_path(name)) {
        rc = make_fixture(path, want_audio);
        usable = (rc == UAV_SEND_OK);
    }
    ~Fixture() { if (usable) std::remove(path.c_str()); }
    bool unsupported() const { return rc == UAV_SEND_ERR_UNSUPPORTED; }
};

UAVPlayer* open_ready(const std::string& path, int timeout_ms = 4000) {
    UAVPlayer* p = uav_create();
    if (!p) return nullptr;
    if (uav_open(p, path.c_str()) != UAV_OK) { uav_destroy(p); return nullptr; }
    wait_for_state_at_least(p, UAV_STATE_READY, timeout_ms);
    return p;
}

double read_audio_rms(UAVPlayer* p, int frames) {
    std::vector<float> buf((size_t)frames * kChannels, 0.0f);
    int got = uav_read_audio(p, buf.data(), frames, kChannels, kRate);
    if (got <= 0) return 0.0;
    double sum = 0.0;
    const size_t n = (size_t)got * kChannels;
    for (size_t i = 0; i < n; ++i) sum += (double)buf[i] * (double)buf[i];
    return std::sqrt(sum / (double)n);
}

double play_and_measure_rms(UAVPlayer* p, int frames, int timeout_ms) {
    uav_play(p);
    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);

    int warm = 0;
    while (clock::now() < deadline) {
        if (read_audio_rms(p, frames) > 1e-9) { ++warm; break; }
        sleep_ms(20);
    }
    while (warm < 3 && clock::now() < deadline) {
        sleep_ms(20);
        if (read_audio_rms(p, frames) > 1e-9) ++warm;
    }
    auto hard = clock::now() + std::chrono::milliseconds(4000);
    double acc = 0.0;
    int n = 0;
    while (n < 4 && clock::now() < hard) {
        sleep_ms(20);
        double r = read_audio_rms(p, frames);
        if (r > 1e-9) { acc += r; ++n; }
    }
    return n > 0 ? acc / n : 0.0;
}

void drain_audio(UAVPlayer* p) {
    std::vector<float> buf((size_t)2048 * kChannels, 0.0f);
    for (int i = 0; i < 32; ++i) {
        if (uav_read_audio(p, buf.data(), 2048, kChannels, kRate) <= 0) break;
    }
}

double measure_active_rms(UAVPlayer* p, int frames, int reads = 4,
                          int budget_ms = 4000) {
    drain_audio(p);
    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(budget_ms);
    double acc = 0.0;
    int n = 0;
    while (n < reads && clock::now() < deadline) {
        sleep_ms(20);
        double r = read_audio_rms(p, frames);
        if (r > 1e-9) { acc += r; ++n; }
    }
    return n > 0 ? acc / n : 0.0;
}

bool confirm_silence(UAVPlayer* p, int frames, int budget_ms = 3000) {
    drain_audio(p);
    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(budget_ms);
    bool saw_produced = false;
    while (clock::now() < deadline) {
        sleep_ms(20);
        std::vector<float> buf((size_t)frames * kChannels, 0.5f);
        int got = uav_read_audio(p, buf.data(), frames, kChannels, kRate);
        if (got > 0) {
            saw_produced = true;
            for (size_t i = 0; i < (size_t)got * kChannels; ++i) {
                if (std::fabs(buf[i]) > 1e-6) return false;
            }
            return true;
        }
        if (uav_get_state(p) == UAV_STATE_FINISHED) break;
    }
    return saw_produced;
}

}
#endif

TEST_CASE("controls: setters/seek/info reject null handle"
          * doctest::test_suite("[controls]")) {
    CHECK(uav_set_looping(nullptr, 1) == UAV_ERR_INVALID);
    CHECK(uav_set_rate(nullptr, 1.0f) == UAV_ERR_INVALID);
    CHECK(uav_set_volume(nullptr, 1.0f) == UAV_ERR_INVALID);
    CHECK(uav_set_muted(nullptr, 1) == UAV_ERR_INVALID);
    CHECK(uav_seek(nullptr, 1.0) == UAV_ERR_INVALID);

    UAVMediaInfo info;
    std::memset(&info, 0xAB, sizeof(info));
    CHECK(uav_get_info(nullptr, &info) == UAV_ERR_INVALID);

    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);
    CHECK(uav_get_info(p, nullptr) == UAV_ERR_INVALID);
    uav_destroy(p);
}

TEST_CASE("controls: get_info before READY returns NO_STREAM and zeroes out"
          * doctest::test_suite("[controls]")) {
    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);

    UAVMediaInfo info;
    std::memset(&info, 0x5A, sizeof(info));
    CHECK(uav_get_info(p, &info) == UAV_ERR_NO_STREAM);
    CHECK(info.has_video == 0);
    CHECK(info.has_audio == 0);
    CHECK(info.width == 0);
    CHECK(info.height == 0);
    CHECK(info.frame_rate == doctest::Approx(0.0));
    CHECK(info.duration == doctest::Approx(0.0));
    CHECK(info.audio_channels == 0);
    CHECK(info.audio_sample_rate == 0);

    uav_destroy(p);
}

#if defined(UAV_HAVE_FFMPEG)

TEST_CASE("controls: fixture generator self-check (oracle gate)"
          * doctest::test_suite("[controls]")) {
    Fixture fx("selfcheck.webm", true);
    if (fx.unsupported()) {
        MESSAGE("VP9/Opus encoder unavailable in this build "
                "(uav_send_open => UAV_SEND_ERR_UNSUPPORTED); skipping fixture.");
        return;
    }
    CHECK_MESSAGE(fx.rc == UAV_SEND_OK,
                  "fixture generation failed, rc=" << fx.rc);
}

TEST_CASE("controls: uav_get_info matches the known synthetic clip"
          * doctest::test_suite("[controls]")) {
    Fixture fx("info.webm", true);
    if (fx.unsupported()) { MESSAGE("encoder unavailable; skip"); return; }
    REQUIRE(fx.rc == UAV_SEND_OK);

    UAVPlayer* p = open_ready(fx.path);
    REQUIRE(p != nullptr);
    REQUIRE(uav_get_state(p) >= UAV_STATE_READY);

    UAVMediaInfo info;
    std::memset(&info, 0, sizeof(info));
    REQUIRE(uav_get_info(p, &info) == UAV_OK);

    CHECK(info.width  == kW);
    CHECK(info.height == kH);
    CHECK(std::fabs(info.frame_rate - (double)kFps) < 0.5);
    CHECK(std::fabs(info.duration - kDuration) < 0.15);
    CHECK(info.has_video == 1);
    CHECK(info.has_audio == 1);
    CHECK(info.audio_channels == kChannels);
    CHECK(info.audio_sample_rate == kRate);

    uav_destroy(p);
}

TEST_CASE("controls: position is monotonic while playing and advances"
          * doctest::test_suite("[controls]")) {
    Fixture fx("posmono.webm", true);
    if (fx.unsupported()) { MESSAGE("encoder unavailable; skip"); return; }
    REQUIRE(fx.rc == UAV_SEND_OK);

    UAVPlayer* p = open_ready(fx.path);
    REQUIRE(p != nullptr);

    REQUIRE(uav_play(p) == UAV_OK);

    const double start = uav_get_position(p);
    double prev = start;
    std::vector<float> buf((size_t)1024 * kChannels, 0.0f);
    for (int i = 0; i < 20; ++i) {
        sleep_ms(50);
        uav_read_audio(p, buf.data(), 1024, kChannels, kRate);
        const double now = uav_get_position(p);
        CHECK(now >= prev - 1e-6);
        prev = now;
    }
    CHECK((prev - start) > 0.2);

    uav_destroy(p);
}

TEST_CASE("controls: paused clock does not advance materially"
          * doctest::test_suite("[controls]")) {
    Fixture fx("pause.webm", true);
    if (fx.unsupported()) { MESSAGE("encoder unavailable; skip"); return; }
    REQUIRE(fx.rc == UAV_SEND_OK);

    UAVPlayer* p = open_ready(fx.path);
    REQUIRE(p != nullptr);

    REQUIRE(uav_play(p) == UAV_OK);
    std::vector<float> buf((size_t)1024 * kChannels, 0.0f);
    for (int i = 0; i < 6; ++i) { sleep_ms(20); uav_read_audio(p, buf.data(), 1024, kChannels, kRate); }

    REQUIRE(uav_pause(p) == UAV_OK);
    CHECK(wait_for_state_eq(p, UAV_STATE_PAUSED, 500) == UAV_STATE_PAUSED);
    const double before = uav_get_position(p);
    sleep_ms(150);
    const double after = uav_get_position(p);
    CHECK(std::fabs(after - before) < 0.05);

    uav_destroy(p);
}

TEST_CASE("controls: seek to mid-clip lands near the target"
          * doctest::test_suite("[controls]")) {
    Fixture fx("seekmid.webm", true);
    if (fx.unsupported()) { MESSAGE("encoder unavailable; skip"); return; }
    REQUIRE(fx.rc == UAV_SEND_OK);

    UAVPlayer* p = open_ready(fx.path);
    REQUIRE(p != nullptr);

    UAVVideoFrame f0;
    int64_t base_id = 0;
    if (uav_acquire_frame(p, -1, &f0) == UAV_OK) { base_id = f0.frame_id; uav_release_frame(p); }

    REQUIRE(uav_seek(p, 1.0) == UAV_OK);
    REQUIRE(uav_play(p) == UAV_OK);

    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(6000);
    bool saw_frame = false;
    double observed_pts = -1.0;
    while (clock::now() < deadline) {
        UAVVideoFrame f;
        if (uav_acquire_frame(p, base_id, &f) == UAV_OK) {
            const double pts = f.pts;
            uav_release_frame(p);
            if (pts > 0.7) { observed_pts = pts; saw_frame = true; break; }
        }
        sleep_ms(15);
    }
    REQUIRE(saw_frame);
    CHECK(std::fabs(observed_pts - 1.0) < 0.20);
    CHECK(std::fabs(uav_get_position(p) - 1.0) < 0.20);

    uav_destroy(p);
}

TEST_CASE("controls: seek to a negative time clamps to zero"
          * doctest::test_suite("[controls]")) {
    Fixture fx("seekneg.webm", true);
    if (fx.unsupported()) { MESSAGE("encoder unavailable; skip"); return; }
    REQUIRE(fx.rc == UAV_SEND_OK);

    UAVPlayer* p = open_ready(fx.path);
    REQUIRE(p != nullptr);

    REQUIRE(uav_seek(p, 1.0) == UAV_OK);
    sleep_ms(50);
    REQUIRE(uav_seek(p, -5.0) == UAV_OK);
    REQUIRE(uav_play(p) == UAV_OK);

    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(3000);
    std::vector<float> abuf((size_t)512 * kChannels, 0.0f);
    double observed_pts = 1e9;
    bool saw = false;
    while (clock::now() < deadline) {
        UAVVideoFrame f;
        if (uav_acquire_frame(p, -1, &f) == UAV_OK) {
            observed_pts = f.pts;
            uav_release_frame(p);
            saw = true;
            break;
        }
        uav_read_audio(p, abuf.data(), 512, kChannels, kRate);
        sleep_ms(15);
    }
    REQUIRE(saw);
    CHECK(observed_pts < 0.10);

    uav_destroy(p);
}

TEST_CASE("controls: looping off plays to FINISHED"
          * doctest::test_suite("[controls]")) {
    Fixture fx("loopoff.webm", true);
    if (fx.unsupported()) { MESSAGE("encoder unavailable; skip"); return; }
    REQUIRE(fx.rc == UAV_SEND_OK);

    UAVPlayer* p = open_ready(fx.path);
    REQUIRE(p != nullptr);

    CHECK(uav_set_looping(p, 0) == UAV_OK);
    REQUIRE(uav_seek(p, kDuration - 0.3) == UAV_OK);
    REQUIRE(uav_play(p) == UAV_OK);

    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(5000);
    std::vector<float> abuf((size_t)1024 * kChannels, 0.0f);
    int st = uav_get_state(p);
    while (st != UAV_STATE_FINISHED && st != UAV_STATE_ERROR && clock::now() < deadline) {
        uav_read_audio(p, abuf.data(), 1024, kChannels, kRate);
        sleep_ms(20);
        st = uav_get_state(p);
    }
    CHECK(st == UAV_STATE_FINISHED);

    uav_destroy(p);
}

TEST_CASE("controls: looping on never latches FINISHED at EOF"
          * doctest::test_suite("[controls]")) {
    Fixture fx("loopon.webm", true);
    if (fx.unsupported()) { MESSAGE("encoder unavailable; skip"); return; }
    REQUIRE(fx.rc == UAV_SEND_OK);

    UAVPlayer* p = open_ready(fx.path);
    REQUIRE(p != nullptr);

    CHECK(uav_set_looping(p, 1) == UAV_OK);
    REQUIRE(uav_seek(p, kDuration - 0.3) == UAV_OK);
    REQUIRE(uav_play(p) == UAV_OK);

    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(4000);
    std::vector<float> abuf((size_t)1024 * kChannels, 0.0f);
    double prev_pos = -1.0;
    int wrap_count = 0;
    bool ever_finished = false;
    while (clock::now() < deadline) {
        uav_read_audio(p, abuf.data(), 1024, kChannels, kRate);
        sleep_ms(20);
        if (uav_get_state(p) == UAV_STATE_FINISHED) ever_finished = true;
        const double pos = uav_get_position(p);
        if (prev_pos > 0.3 && pos < prev_pos - 0.5) {
            if (++wrap_count >= 2) break;
        }
        prev_pos = pos;
    }
    CHECK_FALSE(ever_finished);
    CHECK(wrap_count >= 2);

    uav_destroy(p);
}

TEST_CASE("controls: set_rate accepts valid and clamps non-positive to 1.0"
          * doctest::test_suite("[controls]")) {
    UAVPlayer* probe = uav_create();
    REQUIRE(probe != nullptr);
    CHECK(uav_set_rate(probe, 2.0f)  == UAV_OK);
    CHECK(uav_set_rate(probe, 0.0f)  == UAV_OK);
    CHECK(uav_set_rate(probe, -1.0f) == UAV_OK);
    uav_destroy(probe);

    Fixture fx("rate_noaudio.webm", false);
    if (fx.unsupported()) { MESSAGE("encoder unavailable; skip"); return; }
    REQUIRE(fx.rc == UAV_SEND_OK);

    auto advance_over = [&](float rate, int window_ms) -> double {
        UAVPlayer* p = open_ready(fx.path);
        REQUIRE(p != nullptr);
        CHECK(uav_set_rate(p, rate) == UAV_OK);
        REQUIRE(uav_play(p) == UAV_OK);
        using clock = std::chrono::steady_clock;
        const double p0 = uav_get_position(p);
        auto anchor_deadline = clock::now() + std::chrono::milliseconds(4000);
        double a = p0;
        while (clock::now() < anchor_deadline) {
            sleep_ms(2);
            a = uav_get_position(p);
            if (a > p0) break;
        }
        sleep_ms(window_ms);
        const double b = uav_get_position(p);
        uav_destroy(p);
        return b - a;
    };

    const int   window = 600;
    const double d1   = advance_over(1.0f, window);
    const double d2   = advance_over(2.0f, window);
    const double d0   = advance_over(0.0f, window);

    REQUIRE(d1 > 0.01);
    CHECK((d2 / d1) > 1.4);
    CHECK((d2 / d1) < 2.6);
    CHECK((d0 / d1) > 0.6);
    CHECK((d0 / d1) < 1.4);
}

TEST_CASE("controls: volume scales audio gain and clamps to [0,1]"
          * doctest::test_suite("[controls]")) {
    Fixture fx("volume.webm", true);
    if (fx.unsupported()) { MESSAGE("encoder unavailable; skip"); return; }
    REQUIRE(fx.rc == UAV_SEND_OK);

    UAVPlayer* p = open_ready(fx.path);
    REQUIRE(p != nullptr);

    CHECK(uav_set_looping(p, 1) == UAV_OK);
    CHECK(uav_set_muted(p, 0) == UAV_OK);

    CHECK(uav_set_volume(p, 1.0f) == UAV_OK);
    const double rms_full = play_and_measure_rms(p, 4096, 2000);
    REQUIRE(rms_full > 0.1);

    CHECK(uav_set_volume(p, 0.5f) == UAV_OK);
    const double rms_half = measure_active_rms(p, 4096);
    REQUIRE(rms_half > 1e-4);
    CHECK(std::fabs(rms_half / rms_full - 0.5) < 0.1);

    CHECK(uav_set_volume(p, 0.0f) == UAV_OK);
    CHECK(confirm_silence(p, 4096));

    CHECK(uav_set_volume(p, 2.0f) == UAV_OK);
    const double rms_over = measure_active_rms(p, 4096);
    REQUIRE(rms_over > 0.1);
    CHECK(std::fabs(rms_over / rms_full - 1.0) < 0.10);

    CHECK(uav_set_volume(p, -1.0f) == UAV_OK);
    CHECK(confirm_silence(p, 4096));

    uav_destroy(p);
}

TEST_CASE("controls: mute silences audio and overrides volume"
          * doctest::test_suite("[controls]")) {
    Fixture fx("mute.webm", true);
    if (fx.unsupported()) { MESSAGE("encoder unavailable; skip"); return; }
    REQUIRE(fx.rc == UAV_SEND_OK);

    UAVPlayer* p = open_ready(fx.path);
    REQUIRE(p != nullptr);

    CHECK(uav_set_looping(p, 1) == UAV_OK);
    CHECK(uav_set_volume(p, 1.0f) == UAV_OK);
    const double rms_audible = play_and_measure_rms(p, 4096, 2000);
    REQUIRE(rms_audible > 0.1);

    CHECK(uav_set_muted(p, 1) == UAV_OK);
    CHECK(confirm_silence(p, 4096));

    CHECK(uav_set_muted(p, 0) == UAV_OK);
    const double rms_unmuted = measure_active_rms(p, 4096);
    CHECK(rms_unmuted > 0.1);

    uav_destroy(p);
}

TEST_CASE("controls: control state set before open persists across open"
          * doctest::test_suite("[controls]")) {
    Fixture fx("persist.webm", true);
    if (fx.unsupported()) { MESSAGE("encoder unavailable; skip"); return; }
    REQUIRE(fx.rc == UAV_SEND_OK);

    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);

    CHECK(uav_set_volume(p, 0.0f) == UAV_OK);
    CHECK(uav_set_muted(p, 0) == UAV_OK);
    CHECK(uav_set_looping(p, 1) == UAV_OK);
    CHECK(uav_set_rate(p, 2.0f) == UAV_OK);

    REQUIRE(uav_open(p, fx.path.c_str()) == UAV_OK);
    REQUIRE(wait_for_state_at_least(p, UAV_STATE_READY, 4000) >= UAV_STATE_READY);

    const double rms_pre = play_and_measure_rms(p, 4096, 1500);
    CHECK(rms_pre < 1e-4);

    CHECK(uav_set_volume(p, 1.0f) == UAV_OK);
    drain_audio(p);
    sleep_ms(60);
    const double rms_post = read_audio_rms(p, 4096);
    CHECK(rms_post > 0.1);

    uav_destroy(p);
}

#endif
