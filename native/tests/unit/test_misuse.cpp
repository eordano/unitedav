// SPDX-License-Identifier: Apache-2.0

#include "uav_doctest.h"

#include "unitedav.h"
#include "unitedav_send.h"

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

constexpr int kClipW = 160;
constexpr int kClipH = 120;

bool file_nonempty(const std::string& path) {
    if (path.empty()) return false;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fclose(f);
    return sz > 0;
}

bool run_quiet(const std::string& cmd) {
    std::string full = cmd + " >/dev/null 2>&1";
    return std::system(full.c_str()) == 0;
}

long this_pid() {
#if defined(_WIN32)
    return (long)::_getpid();
#else
    return (long)::getpid();
#endif
}

std::string temp_dir() {
    if (const char* env = std::getenv("TMPDIR"); env && env[0]) return std::string(env);
    std::error_code ec;
    std::filesystem::path p = std::filesystem::temp_directory_path(ec);
    return ec ? std::string(".") : p.string();
}

std::string reusable_clip_path() {
    std::vector<std::string> dirs;
    if (const char* env = std::getenv("UAV_TEST_MEDIA_DIR"); env && env[0])
        dirs.emplace_back(env);
    dirs.emplace_back("../tests/media/out");
    dirs.emplace_back("tests/media/out");
    for (const auto& d : dirs) {
        std::string p = d + "/uav_lifecycle_synth.webm";
        if (file_nonempty(p)) return p;
    }
    return std::string();
}

std::string generated_clip_path() {
    char leaf[256];
    std::snprintf(leaf, sizeof(leaf), "uav_misuse_synth_%ld.webm", this_pid());
    return (std::filesystem::path(temp_dir()) / leaf).string();
}

struct ClipCleanup {
    std::string path;
    ~ClipCleanup() { if (!path.empty()) std::remove(path.c_str()); }
};
ClipCleanup g_generated_clip_cleanup;

const std::string& synthetic_clip() {
    static std::string cached = []() -> std::string {
        if (std::string reuse = reusable_clip_path(); !reuse.empty())
            return reuse;
        std::string path = generated_clip_path();
        g_generated_clip_cleanup.path = path;
        if (!run_quiet("ffmpeg -version")) return std::string();
        char cmd[640];
        std::snprintf(cmd, sizeof(cmd),
            "ffmpeg -y -nostdin "
            "-f lavfi -i testsrc2=size=%dx%d:rate=15 "
            "-f lavfi -i sine=frequency=440:sample_rate=48000 "
            "-t 1 "
            "-c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 200k "
            "-c:a libopus -b:a 32k "
            "-f webm \"%s\"",
            kClipW, kClipH, path.c_str());
        if (run_quiet(cmd) && file_nonempty(path)) return path;
        std::snprintf(cmd, sizeof(cmd),
            "ffmpeg -y -nostdin "
            "-f lavfi -i testsrc2=size=%dx%d:rate=15 "
            "-f lavfi -i sine=frequency=440:sample_rate=48000 "
            "-t 1 "
            "-c:v libvpx -deadline realtime -cpu-used 8 -b:v 200k "
            "-c:a libvorbis -b:a 64k "
            "-f webm \"%s\"",
            kClipW, kClipH, path.c_str());
        if (run_quiet(cmd) && file_nonempty(path)) return path;
        return std::string();
    }();
    return cached;
}

#if defined(UAV_HAVE_FFMPEG)
bool media_available() { return !synthetic_clip().empty(); }
#endif

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

static int g_temp_counter = 0;
std::string make_temp_path(const char* suffix) {
    char leaf[256];
    std::snprintf(leaf, sizeof(leaf), "uav_misuse_%ld_%d%s",
                  this_pid(), g_temp_counter++, suffix);
    return (std::filesystem::path(temp_dir()) / leaf).string();
}
struct TempFileGuard {
    std::string path;
    explicit TempFileGuard(std::string p) : path(std::move(p)) {}
    ~TempFileGuard() { if (!path.empty()) std::remove(path.c_str()); }
    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;
    const char* c_str() const { return path.c_str(); }
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

}

TEST_CASE("misuse: acquire_release_acquire_round_trip_releases_lock_once"
          * doctest::test_suite("[misuse]")) {
#if defined(UAV_HAVE_FFMPEG)
    if (!media_available()) { MESSAGE("SKIP: no synthetic clip"); return; }
    Handle h;
    REQUIRE(h.p != nullptr);
    REQUIRE(uav_open(h, synthetic_clip().c_str()) == UAV_OK);
    REQUIRE(uav_play(h) == UAV_OK);

    int64_t last_id = -1;
    for (int i = 0; i < 8; ++i) {
        UAVVideoFrame vf{};
        int rc = uav_acquire_frame(h, last_id, &vf);
        if (rc == UAV_OK) {
            CHECK(vf.data != nullptr);
            last_id = vf.frame_id;
            uav_release_frame(h);
        } else {
            CHECK(rc == UAV_ERR_NO_STREAM);
        }
        CHECK(state_in_range(uav_get_state(h)));
    }
    CHECK(state_in_range(uav_get_state(h)));
#else
    MESSAGE("SKIP: no-FFmpeg build has no video stream to borrow");
    CHECK(true);
#endif
}

TEST_CASE("misuse: unbalanced_release_frame_is_safe_noop"
          * doctest::test_suite("[misuse]")) {
    {
        Handle h;
        REQUIRE(h.p != nullptr);
        uav_release_frame(h);
        uav_release_frame(h);
        CHECK(state_in_range(uav_get_state(h)));
    }
#if defined(UAV_HAVE_FFMPEG)
    if (media_available()) {
        Handle h;
        REQUIRE(h.p != nullptr);
        REQUIRE(uav_open(h, synthetic_clip().c_str()) == UAV_OK);
        uav_release_frame(h);
        CHECK(uav_close(h) == UAV_OK);
        uav_release_frame(h);
        CHECK(uav_get_state(h) == UAV_STATE_IDLE);
    } else {
        MESSAGE("SKIP (opened-handle leg): no synthetic clip");
    }
#endif
}

TEST_CASE("misuse: get_info_out_struct_poisoned_then_zeroed"
          * doctest::test_suite("[misuse]")) {
    Handle h;
    REQUIRE(h.p != nullptr);
    UAVMediaInfo mi;
    std::memset(&mi, 0x7f, sizeof(mi));
    CHECK(uav_get_info(h, &mi) == UAV_ERR_NO_STREAM);
    CHECK(mi.has_video == 0);
    CHECK(mi.has_audio == 0);
    CHECK(mi.width == 0);
    CHECK(mi.height == 0);
    CHECK(mi.frame_rate == doctest::Approx(0.0));
    CHECK(mi.duration == doctest::Approx(0.0));
    CHECK(mi.audio_channels == 0);
    CHECK(mi.audio_sample_rate == 0);
}

TEST_CASE("misuse: double_destroy_and_null_handle_guard_table"
          * doctest::test_suite("[misuse]")) {
    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);
    uav_destroy(p);
    uav_destroy(nullptr);

    CHECK(uav_open(nullptr, "x") == UAV_ERR_INVALID);
    CHECK(uav_close(nullptr) == UAV_ERR_INVALID);
    CHECK(uav_play(nullptr) == UAV_ERR_INVALID);
    CHECK(uav_pause(nullptr) == UAV_ERR_INVALID);
    CHECK(uav_stop(nullptr) == UAV_ERR_INVALID);
    CHECK(uav_seek(nullptr, 1.0) == UAV_ERR_INVALID);
    CHECK(uav_set_looping(nullptr, 1) == UAV_ERR_INVALID);
    CHECK(uav_set_rate(nullptr, 1.0f) == UAV_ERR_INVALID);
    CHECK(uav_set_volume(nullptr, 0.5f) == UAV_ERR_INVALID);
    CHECK(uav_set_muted(nullptr, 1) == UAV_ERR_INVALID);
    CHECK(uav_get_state(nullptr) == UAV_STATE_ERROR);
    CHECK(uav_last_error(nullptr) == UAV_ERR_INVALID);
    CHECK(uav_get_position(nullptr) == doctest::Approx(0.0));
    UAVMediaInfo mi{};
    CHECK(uav_get_info(nullptr, &mi) == UAV_ERR_INVALID);
    UAVVideoFrame vf{};
    CHECK(uav_acquire_frame(nullptr, -1, &vf) == UAV_ERR_INVALID);
    float buf[8] = {0};
    CHECK(uav_read_audio(nullptr, buf, 4, 2, 48000) == 0);
    uav_release_frame(nullptr);
}

TEST_CASE("misuse: send_push_before_open_and_after_close"
          * doctest::test_suite("[misuse]")) {
    uint8_t rgba[4 * 4 * 4] = {0};
    float pcm[256 * 2] = {0};

    {
        SendHandle s;
        REQUIRE(s.s != nullptr);
        CHECK(uav_send_push_video(s, rgba, 4, 4, 16, 0.0) == UAV_SEND_ERR_INVALID);
        CHECK(uav_send_push_audio(s, pcm, 256, 2, 48000, 0.0) == UAV_SEND_ERR_INVALID);
        char buf[64] = {0};
        CHECK(uav_send_get_sdp(s, buf, sizeof(buf)) == UAV_SEND_ERR_INVALID);
        CHECK(uav_send_close(s) == UAV_SEND_OK);
    }

#if defined(UAV_HAVE_FFMPEG)
    {
        SendHandle s;
        REQUIRE(s.s != nullptr);
        UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_OPUS, 64, 48, 15, 48000, 2);
        TempFileGuard tmp(make_temp_path(".webm"));
        int rc = uav_send_open(s, tmp.c_str(), &cfg);
        if (rc != UAV_SEND_OK) {
            MESSAGE("SKIP (after-close leg): VP9/Opus encoder unavailable");
            return;
        }
        CHECK(uav_send_close(s) == UAV_SEND_OK);
        CHECK(uav_send_push_video(s, rgba, 4, 4, 16, 0.0) == UAV_SEND_ERR_INVALID);
        CHECK(uav_send_push_audio(s, pcm, 256, 2, 48000, 0.0) == UAV_SEND_ERR_INVALID);
        char buf[64] = {0};
        CHECK(uav_send_get_sdp(s, buf, sizeof(buf)) == UAV_SEND_ERR_INVALID);
    }
#endif
}

#if defined(UAV_HAVE_FFMPEG)

TEST_CASE("misuse: acquire_release_across_close_pins_session"
          * doctest::test_suite("[misuse]")) {
    if (!media_available()) { MESSAGE("SKIP: no synthetic clip"); return; }
    Handle h;
    REQUIRE(h.p != nullptr);
    REQUIRE(uav_open(h, synthetic_clip().c_str()) == UAV_OK);
    REQUIRE(uav_play(h) == UAV_OK);

    UAVVideoFrame vf{};
    bool got = false;
    for (int i = 0; i < 400 && !got; ++i) {
        if (uav_acquire_frame(h, -1, &vf) == UAV_OK) { got = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!got) {
        MESSAGE("SKIP: no presentable frame within timeout (decode too slow here)");
        return;
    }
    REQUIRE(vf.data != nullptr);
    REQUIRE(vf.width > 0);
    REQUIRE(vf.height > 0);

    CHECK(uav_close(h) == UAV_OK);

    volatile uint8_t sink =
        vf.data[((size_t)vf.height - 1) * (size_t)vf.stride +
                ((size_t)vf.width - 1) * 4];
    (void)sink;

    uav_release_frame(h);
    CHECK(state_in_range(uav_get_state(h)));
}

TEST_CASE("misuse: read_audio_with_hostile_args"
          * doctest::test_suite("[misuse]")) {
    if (!media_available()) { MESSAGE("SKIP: no synthetic clip"); return; }

    {
        Handle h;
        REQUIRE(h.p != nullptr);
        float dst[64] = {0};
        CHECK(uav_read_audio(h, dst, 16, 2, 48000) == 0);
    }

    Handle h;
    REQUIRE(h.p != nullptr);
    REQUIRE(uav_open(h, synthetic_clip().c_str()) == UAV_OK);
    REQUIRE(uav_play(h) == UAV_OK);

    const int kFrames = 512;
    int channel_seq[] = {1, 2, 8, 2, 1, 8, 2};
    int rate_seq[]    = {48000, 44100, 48000, -1, 0, 48000, 22050};

    for (int call = 0; call < 32; ++call) {
        int ch   = channel_seq[call % (int)(sizeof(channel_seq) / sizeof(int))];
        int rate = rate_seq[call % (int)(sizeof(rate_seq) / sizeof(int))];
        std::vector<float> dst((size_t)kFrames * (size_t)ch, -7.0f);
        int got = uav_read_audio(h, dst.data(), kFrames, ch, rate);
        CHECK(got >= 0);
        CHECK(got <= kFrames);
    }
    CHECK(state_in_range(uav_get_state(h)));
}

#endif

#if defined(UAV_HAVE_FFMPEG)

TEST_CASE("misuse: send_open_close_open_reopen_cycle_no_leak"
          * doctest::test_suite("[misuse]")) {
    SendHandle s;
    REQUIRE(s.s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_OPUS, 64, 48, 15, 48000, 2);

    {
        TempFileGuard probe(make_temp_path(".webm"));
        if (uav_send_open(s, probe.c_str(), &cfg) != UAV_SEND_OK) {
            MESSAGE("SKIP: libvpx-vp9/libopus encoder unavailable");
            return;
        }
        CHECK(uav_send_close(s) == UAV_SEND_OK);
    }

    std::vector<uint8_t> rgba((size_t)64 * 48 * 4, 0);
    std::vector<float> pcm((size_t)(48000 / 15) * 2, 0.0f);

    for (int iter = 0; iter < 16; ++iter) {
        TempFileGuard tmp(make_temp_path(".webm"));
        REQUIRE(uav_send_open(s, tmp.c_str(), &cfg) == UAV_SEND_OK);
        for (int fr = 0; fr < 3; ++fr) {
            rgba[(size_t)fr * 4 % rgba.size()] = (uint8_t)(fr * 40);
            double pts = (double)fr / 15.0;
            CHECK(uav_send_push_video(s, rgba.data(), 64, 48, 64 * 4, pts) == UAV_SEND_OK);
            CHECK(uav_send_push_audio(s, pcm.data(), 48000 / 15, 2, 48000, pts) == UAV_SEND_OK);
        }
        CHECK(uav_send_close(s) == UAV_SEND_OK);
    }
    CHECK(uav_send_last_error(s) == UAV_SEND_OK);
}

TEST_CASE("misuse: send_push_geometry_mismatch_is_rescaled_not_oob"
          * doctest::test_suite("[misuse]")) {
    SendHandle s;
    REQUIRE(s.s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_NONE, 160, 120, 15, 48000, 2);
    TempFileGuard tmp(make_temp_path(".webm"));
    if (uav_send_open(s, tmp.c_str(), &cfg) != UAV_SEND_OK) {
        MESSAGE("SKIP: libvpx-vp9 encoder unavailable");
        return;
    }

    auto push_geom = [&](int w, int h, int pad, double pts) {
        const int stride = w * 4 + pad;
        std::vector<uint8_t> src((size_t)stride * (size_t)h);
        for (size_t i = 0; i < src.size(); ++i) src[i] = (uint8_t)(i & 0xff);
        CHECK(uav_send_push_video(s, src.data(), w, h, stride, pts) == UAV_SEND_OK);
    };

    push_geom(64, 48, 0, 0.0);
    push_geom(64, 48, 16, 1.0 / 15);
    push_geom(320, 200, 0, 2.0 / 15);
    push_geom(320, 200, 32, 3.0 / 15);
    push_geom(160, 120, 8, 4.0 / 15);

    CHECK(uav_send_close(s) == UAV_SEND_OK);
    CHECK(uav_send_last_error(s) == UAV_SEND_OK);
}

#endif
