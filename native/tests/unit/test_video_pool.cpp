// SPDX-License-Identifier: Apache-2.0

#include "uav_doctest.h"

#include "unitedav.h"

#if defined(UAV_HAVE_FFMPEG)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int32_t EXPECT_W = 320;
constexpr int32_t EXPECT_H = 240;

const char* kVideoFixture   = "webm__vp9__opus.webm";
const char* kNoVideoFixture = "ogg__novideo__opus.ogg";

std::string find_media(const char* name) {
    std::vector<std::string> roots;
    if (const char* env = std::getenv("UAV_TEST_MEDIA_DIR")) {
        if (env[0]) roots.emplace_back(env);
    }
    roots.emplace_back("tests/media/out");
    roots.emplace_back("../tests/media/out");
    roots.emplace_back("../../tests/media/out");
    roots.emplace_back("../../../tests/media/out");
    for (const auto& r : roots) {
        std::string path = r + "/" + name;
        if (FILE* f = std::fopen(path.c_str(), "rb")) {
            std::fclose(f);
            return path;
        }
    }
    return std::string();
}

void pump_audio(UAVPlayer* p) {
    float sink[512];
    (void)uav_read_audio(p, sink, 256, 1, 48000);
}

bool poll_acquire(UAVPlayer* p, int64_t last_id, UAVVideoFrame& out,
                  int deadline_ms = 4000) {
    auto start = std::chrono::steady_clock::now();
    for (;;) {
        if (uav_acquire_frame(p, last_id, &out) == UAV_OK) return true;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
        if (elapsed >= deadline_ms) return false;
        pump_audio(p);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

}

TEST_CASE("pool: acquire on unopened handle returns NO_STREAM, zero frame"
          * doctest::test_suite("[pool]")) {
    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);

    UAVVideoFrame vf;
    vf.data = reinterpret_cast<const uint8_t*>(0xDEADBEEF);
    vf.width = 12345; vf.height = 6789; vf.stride = 999;
    vf.format = 7; vf.frame_id = 42; vf.pts = 3.14;

    CHECK(uav_acquire_frame(p, -1, &vf) == UAV_ERR_NO_STREAM);
    CHECK(vf.data == nullptr);
    CHECK(vf.width == 0);
    CHECK(vf.height == 0);
    CHECK(vf.stride == 0);
    CHECK(vf.frame_id == 0);

    uav_release_frame(p);

    uav_destroy(p);
}

TEST_CASE("pool: audio-only fixture gates acquire on has_video"
          * doctest::test_suite("[pool]")) {
    std::string path = find_media(kNoVideoFixture);
    if (path.empty()) {
        MESSAGE("skip: media fixture not found: " << kNoVideoFixture);
        return;
    }
    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);

    if (uav_open(p, path.c_str()) != UAV_OK) {
        MESSAGE("skip: could not open audio-only fixture (decoder unavailable)");
        uav_destroy(p);
        return;
    }

    UAVMediaInfo info;
    REQUIRE(uav_get_info(p, &info) == UAV_OK);
    CHECK(info.has_video == 0);

    UAVVideoFrame vf;
    CHECK(uav_acquire_frame(p, -1, &vf) == UAV_ERR_NO_STREAM);
    CHECK(uav_acquire_frame(p, 0, &vf) == UAV_ERR_NO_STREAM);
    CHECK(uav_acquire_frame(p, 9999, &vf) == UAV_ERR_NO_STREAM);
    CHECK(vf.data == nullptr);

    uav_release_frame(p);

    uav_close(p);
    uav_destroy(p);
}

TEST_CASE("pool: READY pre-rolls a presentable first frame"
          * doctest::test_suite("[pool]")) {
    std::string path = find_media(kVideoFixture);
    if (path.empty()) {
        MESSAGE("skip: media fixture not found: " << kVideoFixture);
        return;
    }
    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);

    if (uav_open(p, path.c_str()) != UAV_OK) {
        MESSAGE("skip: could not open video fixture (decoder unavailable)");
        uav_destroy(p);
        return;
    }

    UAVMediaInfo info;
    REQUIRE(uav_get_info(p, &info) == UAV_OK);
    REQUIRE(info.has_video == 1);

    UAVVideoFrame vf;
    REQUIRE(uav_acquire_frame(p, -1, &vf) == UAV_OK);
    CHECK(vf.data != nullptr);
    CHECK(vf.width == EXPECT_W);
    CHECK(vf.height == EXPECT_H);
    CHECK(vf.stride == vf.width * 4);
    CHECK(vf.format == UAV_PIX_RGBA32);
    CHECK(vf.frame_id >= 1);
    CHECK(vf.pts == doctest::Approx(0.0).epsilon(0.05));

    uav_release_frame(p);

    uav_close(p);
    uav_destroy(p);
}

TEST_CASE("pool: asking for the id you already have yields NO_STREAM (paused)"
          * doctest::test_suite("[pool]")) {
    std::string path = find_media(kVideoFixture);
    if (path.empty()) {
        MESSAGE("skip: media fixture not found: " << kVideoFixture);
        return;
    }
    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);
    if (uav_open(p, path.c_str()) != UAV_OK) {
        MESSAGE("skip: could not open video fixture");
        uav_destroy(p);
        return;
    }

    UAVVideoFrame vf;
    REQUIRE(uav_acquire_frame(p, -1, &vf) == UAV_OK);
    const int64_t n = vf.frame_id;
    uav_release_frame(p);

    UAVVideoFrame vf2;
    CHECK(uav_acquire_frame(p, n, &vf2) == UAV_ERR_NO_STREAM);
    CHECK(vf2.data == nullptr);

    uav_close(p);
    uav_destroy(p);
}

TEST_CASE("pool: frame_id and pts are strictly monotonic across plays"
          * doctest::test_suite("[pool]")) {
    std::string path = find_media(kVideoFixture);
    if (path.empty()) {
        MESSAGE("skip: media fixture not found: " << kVideoFixture);
        return;
    }
    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);
    if (uav_open(p, path.c_str()) != UAV_OK) {
        MESSAGE("skip: could not open video fixture");
        uav_destroy(p);
        return;
    }
    REQUIRE(uav_play(p) == UAV_OK);

    int64_t last_id = -1;
    int64_t prev_id = -1;
    double  prev_pts = -1e18;
    int distinct = 0;

    const int kWant = 8;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (distinct < kWant && std::chrono::steady_clock::now() < deadline) {
        pump_audio(p);
        UAVVideoFrame vf;
        if (uav_acquire_frame(p, last_id, &vf) == UAV_OK) {
            CHECK(vf.frame_id > last_id);
            if (prev_id >= 0) CHECK(vf.frame_id > prev_id);
            if (prev_pts > -1e17) CHECK(vf.pts > prev_pts);

            prev_id = vf.frame_id;
            prev_pts = vf.pts;
            last_id = vf.frame_id;
            ++distinct;
            uav_release_frame(p);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    CHECK(distinct >= 2);

    uav_close(p);
    uav_destroy(p);
}

TEST_CASE("pool: release without acquire is a safe no-op"
          * doctest::test_suite("[pool]")) {
    std::string path = find_media(kVideoFixture);
    if (path.empty()) {
        MESSAGE("skip: media fixture not found: " << kVideoFixture);
        return;
    }
    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);
    if (uav_open(p, path.c_str()) != UAV_OK) {
        MESSAGE("skip: could not open video fixture");
        uav_destroy(p);
        return;
    }

    uav_release_frame(p);
    uav_release_frame(p);

    UAVVideoFrame vf;
    REQUIRE(uav_acquire_frame(p, -1, &vf) == UAV_OK);
    CHECK(vf.data != nullptr);
    uav_release_frame(p);

    uav_close(p);
    uav_destroy(p);
}

TEST_CASE("pool: double-release after acquire is safe (no double-unlock)"
          * doctest::test_suite("[pool]")) {
    std::string path = find_media(kVideoFixture);
    if (path.empty()) {
        MESSAGE("skip: media fixture not found: " << kVideoFixture);
        return;
    }
    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);
    if (uav_open(p, path.c_str()) != UAV_OK) {
        MESSAGE("skip: could not open video fixture");
        uav_destroy(p);
        return;
    }

    UAVVideoFrame vf;
    REQUIRE(uav_acquire_frame(p, -1, &vf) == UAV_OK);
    uav_release_frame(p);
    uav_release_frame(p);

    UAVVideoFrame vf2;
    REQUIRE(uav_acquire_frame(p, -1, &vf2) == UAV_OK);
    CHECK(vf2.data != nullptr);
    uav_release_frame(p);

    uav_close(p);
    uav_destroy(p);
}

TEST_CASE("pool: balanced acquire/release round-trips never deadlock"
          * doctest::test_suite("[pool]")) {
    std::string path = find_media(kVideoFixture);
    if (path.empty()) {
        MESSAGE("skip: media fixture not found: " << kVideoFixture);
        return;
    }
    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);
    if (uav_open(p, path.c_str()) != UAV_OK) {
        MESSAGE("skip: could not open video fixture");
        uav_destroy(p);
        return;
    }
    REQUIRE(uav_play(p) == UAV_OK);

    int64_t last_id = -1;
    int ok = 0;
    volatile uint8_t sink = 0;
    for (int i = 0; i < 200; ++i) {
        UAVVideoFrame vf;
        int r = uav_acquire_frame(p, last_id, &vf);
        if (r == UAV_OK) {
            REQUIRE(vf.data != nullptr);
            REQUIRE(vf.height > 0);
            REQUIRE(vf.stride > 0);
            const size_t bytes = (size_t)vf.stride * (size_t)vf.height;
            sink = sink ^ vf.data[0];
            sink = sink ^ vf.data[bytes - 1];
            last_id = vf.frame_id;
            ++ok;
            uav_release_frame(p);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    (void)sink;
    CHECK(ok >= 1);

    uav_close(p);
    uav_destroy(p);
}

TEST_CASE("pool: held frame is pinned while worker publishes into other slots"
          * doctest::test_suite("[pool]")) {
    std::string path = find_media(kVideoFixture);
    if (path.empty()) {
        MESSAGE("skip: media fixture not found: " << kVideoFixture);
        return;
    }
    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);
    if (uav_open(p, path.c_str()) != UAV_OK) {
        MESSAGE("skip: could not open video fixture");
        uav_destroy(p);
        return;
    }
    REQUIRE(uav_play(p) == UAV_OK);

    UAVVideoFrame vf;
    REQUIRE(poll_acquire(p, -1, vf));
    REQUIRE(vf.data != nullptr);
    REQUIRE(vf.height > 0);
    REQUIRE(vf.stride > 0);

    const uint8_t* pinned = vf.data;
    const size_t bytes = (size_t)vf.stride * (size_t)vf.height;
    const uint8_t br_before = vf.data[bytes - 1];

    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    CHECK(vf.data == pinned);
    CHECK(vf.data[bytes - 1] == br_before);

    uav_release_frame(p);

    uav_close(p);
    uav_destroy(p);
}

TEST_CASE("pool: under slot contention the pool yields complete fresh frames"
          * doctest::test_suite("[pool]")) {
    std::string path = find_media(kVideoFixture);
    if (path.empty()) {
        MESSAGE("skip: media fixture not found: " << kVideoFixture);
        return;
    }
    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);
    if (uav_open(p, path.c_str()) != UAV_OK) {
        MESSAGE("skip: could not open video fixture");
        uav_destroy(p);
        return;
    }
    REQUIRE(uav_play(p) == UAV_OK);

    bool advanced = false;
    for (int round = 0; round < 3 && !advanced; ++round) {
        UAVVideoFrame held;
        if (!poll_acquire(p, -1, held)) break;
        const int64_t held_id = held.frame_id;
        {
            auto hold_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);
            while (std::chrono::steady_clock::now() < hold_end) {
                pump_audio(p);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        uav_release_frame(p);

        UAVVideoFrame fresh;
        if (poll_acquire(p, held_id, fresh)) {
            CHECK(fresh.data != nullptr);
            CHECK(fresh.width * 4 == fresh.stride);
            CHECK(fresh.height > 0);
            CHECK(fresh.frame_id > held_id);
            uav_release_frame(p);
            advanced = true;
        }
    }
    CHECK(advanced);

    uav_close(p);
    uav_destroy(p);
}

TEST_CASE("pool: seek-to-0 republishes a fresh frame near pts 0"
          * doctest::test_suite("[pool]")) {
    std::string path = find_media(kVideoFixture);
    if (path.empty()) {
        MESSAGE("skip: media fixture not found: " << kVideoFixture);
        return;
    }
    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);
    if (uav_open(p, path.c_str()) != UAV_OK) {
        MESSAGE("skip: could not open video fixture");
        uav_destroy(p);
        return;
    }
    REQUIRE(uav_play(p) == UAV_OK);

    constexpr double kPreSeekMinPts = 0.5;
    int64_t n = -1;
    double p_pts = 0.0;
    {
        int64_t last_id = -1;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            pump_audio(p);
            UAVVideoFrame vf;
            if (uav_acquire_frame(p, last_id, &vf) == UAV_OK) {
                last_id = vf.frame_id;
                if (vf.pts >= kPreSeekMinPts) {
                    n = vf.frame_id; p_pts = vf.pts; uav_release_frame(p); break;
                }
                uav_release_frame(p);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    }
    if (n < 0) {
        MESSAGE("skip: never reached a pts>=0.5 frame within deadline");
        uav_close(p);
        uav_destroy(p);
        return;
    }
    CHECK(p_pts >= kPreSeekMinPts);

    REQUIRE(uav_seek(p, 0.0) == UAV_OK);

    bool republished = false;
    {
        bool saw_rewind = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            pump_audio(p);
            UAVVideoFrame vf;
            if (uav_acquire_frame(p, n, &vf) == UAV_OK) {
                CHECK(vf.frame_id > n);
                if (vf.pts < p_pts) {
                    saw_rewind = true;
                    if (vf.pts <= 0.5) republished = true;
                }
                uav_release_frame(p);
                if (republished) break;
                n = vf.frame_id;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        CHECK(saw_rewind);
    }
    CHECK(republished);

    uav_close(p);
    uav_destroy(p);
}

TEST_CASE("pool: leak-free across many open/close cycles"
          * doctest::test_suite("[pool]")) {
    std::string path = find_media(kVideoFixture);
    if (path.empty()) {
        MESSAGE("skip: media fixture not found: " << kVideoFixture);
        return;
    }
    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);

    if (uav_open(p, path.c_str()) != UAV_OK) {
        MESSAGE("skip: could not open video fixture (decoder unavailable)");
        uav_destroy(p);
        return;
    }
    uav_close(p);

    const int kCycles = 100;
    int frames_seen = 0;
    for (int i = 0; i < kCycles; ++i) {
        REQUIRE(uav_open(p, path.c_str()) == UAV_OK);
        REQUIRE(uav_play(p) == UAV_OK);
        UAVVideoFrame vf;
        if (poll_acquire(p, -1, vf, 2000)) {
            REQUIRE(vf.data != nullptr);
            volatile uint8_t t = vf.data[0];
            (void)t;
            ++frames_seen;
            uav_release_frame(p);
        }
        uav_close(p);
    }
    CHECK(frames_seen == kCycles);

    uav_destroy(p);
}

TEST_CASE("pool: acquire after close returns NO_STREAM, release is a no-op"
          * doctest::test_suite("[pool]")) {
    std::string path = find_media(kVideoFixture);
    if (path.empty()) {
        MESSAGE("skip: media fixture not found: " << kVideoFixture);
        return;
    }
    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);
    if (uav_open(p, path.c_str()) != UAV_OK) {
        MESSAGE("skip: could not open video fixture");
        uav_destroy(p);
        return;
    }
    REQUIRE(uav_play(p) == UAV_OK);

    UAVVideoFrame vf;
    REQUIRE(poll_acquire(p, -1, vf));
    uav_release_frame(p);

    uav_close(p);

    UAVVideoFrame vf2;
    CHECK(uav_acquire_frame(p, -1, &vf2) == UAV_ERR_NO_STREAM);
    CHECK(vf2.data == nullptr);
    uav_release_frame(p);

    uav_destroy(p);
}

TEST_CASE("pool: mid-borrow close keeps the held buffer alive (held_session_ pin)"
          * doctest::test_suite("[pool][concurrency]")) {
    std::string path = find_media(kVideoFixture);
    if (path.empty()) {
        MESSAGE("skip: media fixture not found: " << kVideoFixture);
        return;
    }
    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);
    if (uav_open(p, path.c_str()) != UAV_OK) {
        MESSAGE("skip: could not open video fixture");
        uav_destroy(p);
        return;
    }
    REQUIRE(uav_play(p) == UAV_OK);

    UAVVideoFrame vf;
    REQUIRE(poll_acquire(p, -1, vf));
    REQUIRE(vf.data != nullptr);
    REQUIRE(vf.height > 0);
    REQUIRE(vf.stride > 0);
    const size_t bytes = (size_t)vf.stride * (size_t)vf.height;
    const uint8_t first_before = vf.data[0];
    const uint8_t last_before = vf.data[bytes - 1];

    std::thread closer([p]() { uav_close(p); });

    volatile uint8_t sink = 0;
    for (int i = 0; i < 50; ++i) {
        sink = sink ^ vf.data[0];
        sink = sink ^ vf.data[bytes - 1];
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    (void)sink;
    CHECK(vf.data[0] == first_before);
    CHECK(vf.data[bytes - 1] == last_before);

    closer.join();

    uav_release_frame(p);

    uav_destroy(p);
}

TEST_CASE("pool: paused player fabricates no phantom frames"
          * doctest::test_suite("[pool]")) {
    std::string path = find_media(kVideoFixture);
    if (path.empty()) {
        MESSAGE("skip: media fixture not found: " << kVideoFixture);
        return;
    }
    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);
    if (uav_open(p, path.c_str()) != UAV_OK) {
        MESSAGE("skip: could not open video fixture");
        uav_destroy(p);
        return;
    }

    UAVVideoFrame vf;
    REQUIRE(uav_acquire_frame(p, -1, &vf) == UAV_OK);
    const int64_t f0 = vf.frame_id;
    uav_release_frame(p);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    int polls = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        UAVVideoFrame v;
        CHECK(uav_acquire_frame(p, f0, &v) == UAV_ERR_NO_STREAM);
        ++polls;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(polls >= 1);

    uav_close(p);
    uav_destroy(p);
}

#else

TEST_CASE("pool: (no-FFmpeg) acquire on an unopened handle returns NO_STREAM"
          * doctest::test_suite("[pool]")) {
    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);
    UAVVideoFrame vf;
    vf.data = reinterpret_cast<const uint8_t*>(0xDEADBEEF);
    vf.width = 1; vf.frame_id = 1;
    CHECK(uav_acquire_frame(p, -1, &vf) == UAV_ERR_NO_STREAM);
    CHECK(vf.data == nullptr);
    CHECK(vf.width == 0);
    CHECK(vf.frame_id == 0);
    uav_release_frame(p);
    uav_destroy(p);
    MESSAGE("note: FFmpeg disabled — pool decode cases compiled out");
}

#endif
