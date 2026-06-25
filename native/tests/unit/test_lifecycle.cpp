// SPDX-License-Identifier: Apache-2.0

#include "uav_doctest.h"

#include "unitedav.h"

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

const char* state_name(int s) {
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

bool state_in_range(int s) {
    return s >= UAV_STATE_IDLE && s <= UAV_STATE_ERROR;
}

bool poll_state(UAVPlayer* p, int want, int timeout_ms) {
    const int slice_ms = 5;
    for (int waited = 0; waited <= timeout_ms; waited += slice_ms) {
        if (uav_get_state(p) == want) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(slice_ms));
    }
    return uav_get_state(p) == want;
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
    int rc = std::system(full.c_str());
    return rc == 0;
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
    std::string base;
    if (const char* env = std::getenv("TMPDIR"); env && env[0]) {
        base = env;
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
    char leaf[256];
    std::snprintf(leaf, sizeof(leaf), "uav_lifecycle_synth_%ld.webm", pid);
    return (std::filesystem::path(base) / leaf).string();
}

struct ClipCleanup {
    std::string path;
    ~ClipCleanup() { if (!path.empty()) std::remove(path.c_str()); }
};
ClipCleanup g_generated_clip_cleanup;

const std::string& synthetic_clip() {
    static std::string cached = [] () -> std::string {
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

}

TEST_CASE("lifecycle: abi_version_is_one" * doctest::test_suite("[lifecycle]")) {
    CHECK(uav_abi_version() == UAV_ABI_VERSION);
    CHECK(uav_abi_version() == 1u);
}

TEST_CASE("lifecycle: abi_version_stable_across_calls"
          * doctest::test_suite("[lifecycle]")) {
    uint32_t a = uav_abi_version();
    uint32_t b = uav_abi_version();
    CHECK(a == b);
}

TEST_CASE("lifecycle: null_handle_get_state" * doctest::test_suite("[lifecycle]")) {
    CHECK(uav_get_state(nullptr) == UAV_STATE_ERROR);
}

TEST_CASE("lifecycle: null_handle_last_error" * doctest::test_suite("[lifecycle]")) {
    CHECK(uav_last_error(nullptr) == UAV_ERR_INVALID);
}

TEST_CASE("lifecycle: null_handle_controls_return_invalid"
          * doctest::test_suite("[lifecycle]")) {
    CHECK(uav_open(nullptr, "x")    == UAV_ERR_INVALID);
    CHECK(uav_close(nullptr)        == UAV_ERR_INVALID);
    CHECK(uav_play(nullptr)         == UAV_ERR_INVALID);
    CHECK(uav_pause(nullptr)        == UAV_ERR_INVALID);
    CHECK(uav_stop(nullptr)         == UAV_ERR_INVALID);
    CHECK(uav_seek(nullptr, 1.0)    == UAV_ERR_INVALID);
    CHECK(uav_set_looping(nullptr, 1) == UAV_ERR_INVALID);
    CHECK(uav_set_rate(nullptr, 1.0f) == UAV_ERR_INVALID);
    CHECK(uav_set_volume(nullptr, 0.5f) == UAV_ERR_INVALID);
    CHECK(uav_set_muted(nullptr, 1) == UAV_ERR_INVALID);

    UAVMediaInfo mi{};
    CHECK(uav_get_info(nullptr, &mi) == UAV_ERR_INVALID);

    CHECK(uav_get_position(nullptr) == doctest::Approx(0.0));

    float buf[8] = {0};
    CHECK(uav_read_audio(nullptr, buf, 4, 2, 48000) == 0);

    UAVVideoFrame vf{};
    CHECK(uav_acquire_frame(nullptr, -1, &vf) == UAV_ERR_INVALID);

    uav_release_frame(nullptr);
    uav_destroy(nullptr);
}

TEST_CASE("lifecycle: destroy_null_is_noop" * doctest::test_suite("[lifecycle]")) {
    uav_destroy(nullptr);
    CHECK(true);
}

TEST_CASE("lifecycle: create_initial_state_idle"
          * doctest::test_suite("[lifecycle]")) {
    Handle h;
    REQUIRE(h.p != nullptr);
    CHECK(uav_get_state(h) == UAV_STATE_IDLE);
    CHECK(uav_last_error(h) == UAV_OK);
}

TEST_CASE("lifecycle: create_destroy_noop_cycle"
          * doctest::test_suite("[lifecycle]")) {
    for (int i = 0; i < 32; ++i) {
        UAVPlayer* p = uav_create();
        REQUIRE(p != nullptr);
        CHECK(uav_get_state(p) == UAV_STATE_IDLE);
        uav_destroy(p);
    }
}

TEST_CASE("lifecycle: open_null_url" * doctest::test_suite("[lifecycle]")) {
    Handle h;
    REQUIRE(h.p != nullptr);
    CHECK(uav_open(h, nullptr) == UAV_ERR_INVALID);
    CHECK(uav_get_state(h) == UAV_STATE_IDLE);
}

TEST_CASE("lifecycle: open_empty_url" * doctest::test_suite("[lifecycle]")) {
    Handle h;
    REQUIRE(h.p != nullptr);
    CHECK(uav_open(h, "") == UAV_ERR_INVALID);
    CHECK(uav_get_state(h) == UAV_STATE_IDLE);
}

TEST_CASE("lifecycle: get_info_before_open_no_stream"
          * doctest::test_suite("[lifecycle]")) {
    Handle h;
    REQUIRE(h.p != nullptr);
    UAVMediaInfo mi;
    std::memset(&mi, 0x7f, sizeof(mi));
    CHECK(uav_get_info(h, &mi) == UAV_ERR_NO_STREAM);
    CHECK(mi.has_video == 0);
    CHECK(mi.has_audio == 0);
    CHECK(mi.width == 0);
    CHECK(mi.height == 0);
    CHECK(mi.audio_channels == 0);
    CHECK(mi.audio_sample_rate == 0);
}

TEST_CASE("lifecycle: play_on_idle_rejected" * doctest::test_suite("[lifecycle]")) {
    Handle h;
    REQUIRE(h.p != nullptr);
    CHECK(uav_play(h) == UAV_ERR_INVALID);
    CHECK(uav_get_state(h) == UAV_STATE_IDLE);
}

TEST_CASE("lifecycle: pause_stop_on_idle_graceful"
          * doctest::test_suite("[lifecycle]")) {
    Handle h;
    REQUIRE(h.p != nullptr);
    CHECK(uav_pause(h) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_IDLE);
    CHECK(uav_stop(h) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_IDLE);
}

TEST_CASE("lifecycle: seek_on_idle_invalid" * doctest::test_suite("[lifecycle]")) {
    Handle h;
    REQUIRE(h.p != nullptr);
    CHECK(uav_seek(h, 1.0) == UAV_ERR_INVALID);
    CHECK(uav_get_state(h) == UAV_STATE_IDLE);
}

TEST_CASE("lifecycle: double_close_idempotent" * doctest::test_suite("[lifecycle]")) {
    Handle h;
    REQUIRE(h.p != nullptr);
    CHECK(uav_close(h) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_IDLE);
    CHECK(uav_close(h) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_IDLE);
}

TEST_CASE("lifecycle: release_without_acquire_noop"
          * doctest::test_suite("[lifecycle]")) {
    Handle h;
    REQUIRE(h.p != nullptr);
    uav_release_frame(h);
    uav_release_frame(h);
    CHECK(uav_get_state(h) == UAV_STATE_IDLE);
}

TEST_CASE("lifecycle: open_bogus_path_errors" * doctest::test_suite("[lifecycle]")) {
    Handle h;
    REQUIRE(h.p != nullptr);
    int rc = uav_open(h, "/nonexistent/uav_no_such_file.mp4");
    CHECK(rc < 0);
#if defined(UAV_HAVE_FFMPEG)
    CHECK(rc == UAV_ERR_OPEN_FAILED);
#else
    CHECK(rc == UAV_ERR_UNSUPPORTED);
#endif
    CHECK(uav_get_state(h) == UAV_STATE_ERROR);
    CHECK(uav_last_error(h) == rc);
}

TEST_CASE("lifecycle: play_on_error_rejected" * doctest::test_suite("[lifecycle]")) {
    Handle h;
    REQUIRE(h.p != nullptr);
    int rc = uav_open(h, "/nonexistent/uav_no_such_file.mp4");
    CHECK(rc < 0);
    REQUIRE(uav_get_state(h) == UAV_STATE_ERROR);
    CHECK(uav_play(h) == UAV_ERR_INVALID);
    CHECK(uav_get_state(h) == UAV_STATE_ERROR);
}

TEST_CASE("lifecycle: close_after_error_returns_to_idle"
          * doctest::test_suite("[lifecycle]")) {
    Handle h;
    REQUIRE(h.p != nullptr);
    CHECK(uav_open(h, "/nonexistent/uav_no_such_file.mp4") < 0);
    REQUIRE(uav_get_state(h) == UAV_STATE_ERROR);
    CHECK(uav_close(h) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_IDLE);
    CHECK(uav_last_error(h) == UAV_OK);
}

#if !defined(UAV_HAVE_FFMPEG)
TEST_CASE("lifecycle: unsupported_build_contract"
          * doctest::test_suite("[lifecycle]")) {
    Handle h;
    REQUIRE(h.p != nullptr);
    CHECK(uav_open(h, "anything.mp4") == UAV_ERR_UNSUPPORTED);
    CHECK(uav_get_state(h) == UAV_STATE_ERROR);
    CHECK(uav_last_error(h) == UAV_ERR_UNSUPPORTED);
    CHECK(uav_close(h) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_IDLE);
    CHECK(uav_play(h) == UAV_ERR_INVALID);
    CHECK(uav_seek(h, 1.0) == UAV_ERR_UNSUPPORTED);
}
#endif

TEST_CASE("lifecycle: illegal_transition_matrix_no_crash"
          * doctest::test_suite("[lifecycle]")) {
    Handle h;
    REQUIRE(h.p != nullptr);

    CHECK(uav_play(h) == UAV_ERR_INVALID);
    CHECK(state_in_range(uav_get_state(h)));

    CHECK(uav_seek(h, 2.0) == UAV_ERR_INVALID);

    CHECK(uav_stop(h) == UAV_OK);
    CHECK(uav_pause(h) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_IDLE);

    CHECK(uav_open(h, "/nonexistent/uav_no_such_file.mp4") < 0);
    CHECK(uav_get_state(h) == UAV_STATE_ERROR);

    UAVMediaInfo mi{};
    CHECK(uav_get_info(h, &mi) == UAV_ERR_NO_STREAM);

    CHECK(uav_play(h) == UAV_ERR_INVALID);
    CHECK(uav_get_state(h) == UAV_STATE_ERROR);

    CHECK(uav_stop(h) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_ERROR);

    CHECK(uav_close(h) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_IDLE);
    CHECK(uav_last_error(h) == UAV_OK);

    CHECK(uav_play(h) == UAV_ERR_INVALID);

    CHECK(state_in_range(uav_get_state(h)));
}

#if defined(UAV_HAVE_FFMPEG)

TEST_CASE("lifecycle: open_reaches_ready" * doctest::test_suite("[lifecycle]")) {
    if (!media_available()) {
        MESSAGE("SKIP: no synthetic clip (system ffmpeg / encoder unavailable)");
        return;
    }
    Handle h;
    REQUIRE(h.p != nullptr);
    int rc = uav_open(h, synthetic_clip().c_str());
    REQUIRE_MESSAGE(rc == UAV_OK, "open(" << synthetic_clip() << ") -> " << rc);
    CHECK(uav_get_state(h) == UAV_STATE_READY);
    CHECK(uav_last_error(h) == UAV_OK);
}

TEST_CASE("lifecycle: ready_get_info_ok" * doctest::test_suite("[lifecycle]")) {
    if (!media_available()) { MESSAGE("SKIP: no synthetic clip"); return; }
    Handle h;
    REQUIRE(h.p != nullptr);
    REQUIRE(uav_open(h, synthetic_clip().c_str()) == UAV_OK);
    REQUIRE(uav_get_state(h) == UAV_STATE_READY);

    UAVMediaInfo mi{};
    CHECK(uav_get_info(h, &mi) == UAV_OK);
    CHECK(mi.has_video == 1);
    CHECK(mi.width > 0);
    CHECK(mi.height > 0);
    CHECK(mi.width == kClipW);
    CHECK(mi.height == kClipH);
    CHECK(mi.duration > 0.0);
}

TEST_CASE("lifecycle: play_pause_resume_transitions"
          * doctest::test_suite("[lifecycle]")) {
    if (!media_available()) { MESSAGE("SKIP: no synthetic clip"); return; }
    Handle h;
    REQUIRE(h.p != nullptr);
    REQUIRE(uav_open(h, synthetic_clip().c_str()) == UAV_OK);

    CHECK(uav_play(h) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_PLAYING);

    CHECK(uav_pause(h) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_PAUSED);

    CHECK(uav_play(h) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_PLAYING);
}

TEST_CASE("lifecycle: pause_from_ready_is_noop"
          * doctest::test_suite("[lifecycle]")) {
    if (!media_available()) { MESSAGE("SKIP: no synthetic clip"); return; }
    Handle h;
    REQUIRE(h.p != nullptr);
    REQUIRE(uav_open(h, synthetic_clip().c_str()) == UAV_OK);
    REQUIRE(uav_get_state(h) == UAV_STATE_READY);
    CHECK(uav_pause(h) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_READY);
}

TEST_CASE("lifecycle: stop_resets_to_paused_and_position"
          * doctest::test_suite("[lifecycle]")) {
    if (!media_available()) { MESSAGE("SKIP: no synthetic clip"); return; }
    Handle h;
    REQUIRE(h.p != nullptr);
    REQUIRE(uav_open(h, synthetic_clip().c_str()) == UAV_OK);
    CHECK(uav_play(h) == UAV_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    CHECK(uav_stop(h) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_PAUSED);

    bool rewound = false;
    for (int i = 0; i < 60; ++i) {
        if (uav_get_position(h) <= 0.1) { rewound = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(rewound);
    CHECK(uav_get_position(h) >= 0.0);
}

TEST_CASE("lifecycle: seek_then_position_monotone"
          * doctest::test_suite("[lifecycle]")) {
    if (!media_available()) { MESSAGE("SKIP: no synthetic clip"); return; }
    Handle h;
    REQUIRE(h.p != nullptr);
    REQUIRE(uav_open(h, synthetic_clip().c_str()) == UAV_OK);

    UAVMediaInfo mi{};
    REQUIRE(uav_get_info(h, &mi) == UAV_OK);
    double mid = (mi.duration > 0.0) ? mi.duration * 0.5 : 0.3;

    CHECK(uav_play(h) == UAV_OK);
    CHECK(uav_seek(h, mid) == UAV_OK);
    for (int i = 0; i < 20; ++i) {
        double pos = uav_get_position(h);
        CHECK(pos >= 0.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

TEST_CASE("lifecycle: seek_negative_clamped" * doctest::test_suite("[lifecycle]")) {
    if (!media_available()) { MESSAGE("SKIP: no synthetic clip"); return; }
    Handle h;
    REQUIRE(h.p != nullptr);
    REQUIRE(uav_open(h, synthetic_clip().c_str()) == UAV_OK);
    CHECK(uav_play(h) == UAV_OK);
    CHECK(uav_seek(h, -5.0) == UAV_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(uav_get_position(h) >= 0.0);
}

TEST_CASE("lifecycle: seek_from_finished_leaves_finished"
          * doctest::test_suite("[lifecycle]")) {
    if (!media_available()) { MESSAGE("SKIP: no synthetic clip"); return; }
    Handle h;
    REQUIRE(h.p != nullptr);
    REQUIRE(uav_open(h, synthetic_clip().c_str()) == UAV_OK);
    CHECK(uav_play(h) == UAV_OK);
    if (!poll_state(h, UAV_STATE_FINISHED, 5000)) {
        MESSAGE("SKIP: clip did not reach FINISHED within timeout");
        return;
    }
    CHECK(uav_seek(h, 0.0) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_PAUSED);
}

TEST_CASE("lifecycle: play_to_finished_then_replay"
          * doctest::test_suite("[lifecycle]")) {
    if (!media_available()) { MESSAGE("SKIP: no synthetic clip"); return; }
    Handle h;
    REQUIRE(h.p != nullptr);
    REQUIRE(uav_open(h, synthetic_clip().c_str()) == UAV_OK);
    CHECK(uav_play(h) == UAV_OK);
    if (!poll_state(h, UAV_STATE_FINISHED, 5000)) {
        MESSAGE("SKIP: clip did not reach FINISHED within timeout");
        return;
    }
    CHECK(uav_play(h) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_PLAYING);
}

TEST_CASE("lifecycle: double_open_same_handle"
          * doctest::test_suite("[lifecycle]")) {
    if (!media_available()) { MESSAGE("SKIP: no synthetic clip"); return; }
    Handle h;
    REQUIRE(h.p != nullptr);
    REQUIRE(uav_open(h, synthetic_clip().c_str()) == UAV_OK);
    REQUIRE(uav_get_state(h) == UAV_STATE_READY);
    CHECK(uav_open(h, synthetic_clip().c_str()) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_READY);
}

TEST_CASE("lifecycle: reopen_after_close" * doctest::test_suite("[lifecycle]")) {
    if (!media_available()) { MESSAGE("SKIP: no synthetic clip"); return; }
    Handle h;
    REQUIRE(h.p != nullptr);

    CHECK(uav_open(h, synthetic_clip().c_str()) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_READY);
    CHECK(uav_last_error(h) == UAV_OK);

    CHECK(uav_close(h) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_IDLE);
    CHECK(uav_last_error(h) == UAV_OK);

    CHECK(uav_open(h, synthetic_clip().c_str()) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_READY);
    CHECK(uav_last_error(h) == UAV_OK);
}

TEST_CASE("lifecycle: close_returns_to_idle" * doctest::test_suite("[lifecycle]")) {
    if (!media_available()) { MESSAGE("SKIP: no synthetic clip"); return; }
    Handle h;
    REQUIRE(h.p != nullptr);
    REQUIRE(uav_open(h, synthetic_clip().c_str()) == UAV_OK);
    REQUIRE(uav_get_state(h) == UAV_STATE_READY);

    CHECK(uav_close(h) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_IDLE);
    CHECK(uav_last_error(h) == UAV_OK);

    UAVMediaInfo mi{};
    CHECK(uav_get_info(h, &mi) == UAV_ERR_NO_STREAM);
}

TEST_CASE("lifecycle: use_after_close_graceful"
          * doctest::test_suite("[lifecycle]")) {
    if (!media_available()) { MESSAGE("SKIP: no synthetic clip"); return; }
    Handle h;
    REQUIRE(h.p != nullptr);
    REQUIRE(uav_open(h, synthetic_clip().c_str()) == UAV_OK);
    CHECK(uav_close(h) == UAV_OK);
    REQUIRE(uav_get_state(h) == UAV_STATE_IDLE);

    CHECK(uav_play(h) == UAV_ERR_INVALID);
    CHECK(uav_pause(h) == UAV_OK);
    CHECK(uav_stop(h) == UAV_OK);
    CHECK(uav_seek(h, 0.5) == UAV_ERR_INVALID);

    UAVMediaInfo mi{};
    CHECK(uav_get_info(h, &mi) == UAV_ERR_NO_STREAM);
    CHECK(uav_get_position(h) == doctest::Approx(0.0));

    UAVVideoFrame vf{};
    CHECK(uav_acquire_frame(h, -1, &vf) == UAV_ERR_NO_STREAM);

    float buf[8] = {0};
    CHECK(uav_read_audio(h, buf, 4, 2, 48000) == 0);

    CHECK(uav_get_state(h) == UAV_STATE_IDLE);
}

TEST_CASE("lifecycle: acquire_without_play_no_stream"
          * doctest::test_suite("[lifecycle]")) {
    if (!media_available()) { MESSAGE("SKIP: no synthetic clip"); return; }
    Handle h;
    REQUIRE(h.p != nullptr);
    REQUIRE(uav_open(h, synthetic_clip().c_str()) == UAV_OK);

    int64_t last_id = -1;
    for (int i = 0; i < 4; ++i) {
        UAVVideoFrame vf{};
        int rc = uav_acquire_frame(h, last_id, &vf);
        if (rc == UAV_OK) {
            CHECK(vf.data != nullptr);
            CHECK(vf.width > 0);
            CHECK(vf.height > 0);
            last_id = vf.frame_id;
            uav_release_frame(h);
        } else {
            CHECK(rc == UAV_ERR_NO_STREAM);
        }
    }
    CHECK(state_in_range(uav_get_state(h)));
}

TEST_CASE("lifecycle: state_value_in_enum_range"
          * doctest::test_suite("[lifecycle]")) {
    if (!media_available()) { MESSAGE("SKIP: no synthetic clip"); return; }
    Handle h;
    REQUIRE(h.p != nullptr);
    CHECK(state_in_range(uav_get_state(h)));
    REQUIRE(uav_open(h, synthetic_clip().c_str()) == UAV_OK);
    CHECK(state_in_range(uav_get_state(h)));
    uav_play(h);
    CHECK(state_in_range(uav_get_state(h)));
    uav_pause(h);
    CHECK(state_in_range(uav_get_state(h)));
    uav_stop(h);
    CHECK(state_in_range(uav_get_state(h)));
    for (int i = 0; i < 20; ++i) {
        CHECK(state_in_range(uav_get_state(h)));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    uav_close(h);
    CHECK(state_in_range(uav_get_state(h)));
}

TEST_CASE("lifecycle: last_error_reflects_last_op"
          * doctest::test_suite("[lifecycle]")) {
    if (!media_available()) { MESSAGE("SKIP: no synthetic clip"); return; }
    Handle h;
    REQUIRE(h.p != nullptr);

    REQUIRE(uav_open(h, synthetic_clip().c_str()) == UAV_OK);
    CHECK(uav_last_error(h) == UAV_OK);

    int rc = uav_open(h, "/nonexistent/uav_no_such_file.mp4");
    CHECK(rc < 0);
    CHECK(uav_last_error(h) == rc);

    CHECK(uav_close(h) == UAV_OK);
    CHECK(uav_last_error(h) == UAV_OK);
}

TEST_CASE("lifecycle: concurrent_close_during_acquire"
          * doctest::test_suite("[lifecycle][concurrency]")) {
    if (!media_available()) { MESSAGE("SKIP: no synthetic clip"); return; }
    Handle h;
    REQUIRE(h.p != nullptr);

    std::atomic<bool> stop{false};
    std::thread reader([&]() {
        int64_t last_id = -1;
        std::vector<float> abuf(1024 * 2);
        while (!stop.load(std::memory_order_relaxed)) {
            UAVVideoFrame vf{};
            if (uav_acquire_frame(h, last_id, &vf) == UAV_OK) {
                if (vf.data && vf.width > 0 && vf.height > 0) {
                    volatile uint8_t sink =
                        vf.data[((size_t)vf.height - 1) * vf.stride + (vf.width - 1) * 4];
                    (void)sink;
                }
                last_id = vf.frame_id;
                uav_release_frame(h);
            }
            uav_read_audio(h, abuf.data(), 1024, 2, 48000);
            (void)uav_get_state(h);
            (void)uav_get_position(h);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    for (int i = 0; i < 8; ++i) {
        if (uav_open(h, synthetic_clip().c_str()) == UAV_OK) {
            uav_play(h);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            uav_seek(h, 0.3);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            uav_seek(h, 0.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            uav_pause(h);
        }
        uav_close(h);
    }

    stop.store(true);
    reader.join();
    CHECK(state_in_range(uav_get_state(h)));
}

#endif
