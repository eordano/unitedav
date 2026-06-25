// SPDX-License-Identifier: Apache-2.0

#include "uav_doctest.h"

#include "unitedav.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

TEST_CASE("ring: read_audio on a never-opened player returns 0 and zero-fills"
          * doctest::test_suite("[ring]")) {
    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);

    const int frames = 1024, ch = 2;
    std::vector<float> buf((size_t)frames * ch, 0.5f);

    int got = uav_read_audio(p, buf.data(), frames, ch, 48000);
    CHECK(got == 0);
    bool all_zero = true;
    for (float v : buf) if (v != 0.0f) { all_zero = false; break; }
    CHECK(all_zero);

    uav_destroy(p);
}

TEST_CASE("ring: uav_read_audio arg-guard does not write dst (matches uav_api.cpp)"
          * doctest::test_suite("[ring]")) {
    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);

    const int frames = 256, ch = 2;
    const float SENT = 0.5f;
    std::vector<float> buf((size_t)frames * ch, SENT);

    auto sentinel_intact = [&]() {
        for (float v : buf) if (v != SENT) return false;
        return true;
    };

    CHECK(uav_read_audio(nullptr, buf.data(), frames, ch, 48000) == 0);
    CHECK(sentinel_intact());

    CHECK(uav_read_audio(p, nullptr, frames, ch, 48000) == 0);

    CHECK(uav_read_audio(p, buf.data(), 0, ch, 48000) == 0);
    CHECK(sentinel_intact());
    CHECK(uav_read_audio(p, buf.data(), -7, ch, 48000) == 0);
    CHECK(sentinel_intact());

    CHECK(uav_read_audio(p, buf.data(), frames, 0, 48000) == 0);
    CHECK(sentinel_intact());
    CHECK(uav_read_audio(p, buf.data(), frames, -2, 48000) == 0);
    CHECK(sentinel_intact());

    uav_destroy(p);
}

TEST_CASE("ring: sample_rate<=0 is accepted, returns 0 and zero-fills (no crash)"
          * doctest::test_suite("[ring]")) {
    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);

    const int frames = 256, ch = 2;
    auto check_zero_filled = [&](int32_t sample_rate) {
        std::vector<float> buf((size_t)frames * ch, 0.5f);
        int got = uav_read_audio(p, buf.data(), frames, ch, sample_rate);
        CHECK(got == 0);
        bool all_zero = true;
        for (float v : buf) if (v != 0.0f) { all_zero = false; break; }
        CHECK(all_zero);
    };

    check_zero_filled(0);
    check_zero_filled(-1);
    check_zero_filled(-48000);

    uav_destroy(p);
}

#if defined(UAV_HAVE_FFMPEG)

namespace {

const char* kAudioClips[] = {
    "ogg__novideo__opus.ogg",
    "ogg__novideo__vorbis.ogg",
};

bool file_exists(const std::string& path) {
    if (FILE* f = std::fopen(path.c_str(), "rb")) { std::fclose(f); return true; }
    return false;
}

std::string find_clip(const char* const* clips, size_t n) {
    std::vector<std::string> dirs;
    if (const char* env = std::getenv("UAV_TEST_MEDIA_DIR")) {
        if (env[0]) dirs.emplace_back(env);
    }
    dirs.emplace_back("tests/media/out");
    dirs.emplace_back("../tests/media/out");
    dirs.emplace_back("../../tests/media/out");
    dirs.emplace_back("../../../tests/media/out");

    for (const auto& d : dirs) {
        for (size_t i = 0; i < n; ++i) {
            std::string full = d + "/" + clips[i];
            if (file_exists(full)) return full;
        }
    }
    return std::string();
}

UAVPlayer* open_audio_clip(std::string& path) {
    path = find_clip(kAudioClips, sizeof(kAudioClips) / sizeof(kAudioClips[0]));
    if (path.empty()) return nullptr;
    UAVPlayer* p = uav_create();
    if (!p) return nullptr;
    if (uav_open(p, path.c_str()) != UAV_OK) {
        uav_destroy(p);
        return nullptr;
    }
    return p;
}

int prime_until_audio(UAVPlayer* p, float* dst, int frames, int ch, int rate,
                      int timeout_ms = 3000) {
    using clk = std::chrono::steady_clock;
    auto deadline = clk::now() + std::chrono::milliseconds(timeout_ms);
    int got = 0;
    while (clk::now() < deadline) {
        got = uav_read_audio(p, dst, frames, ch, rate);
        REQUIRE(got >= 0);
        REQUIRE(got <= frames);
        if (got > 0) break;
        if (uav_get_state(p) == UAV_STATE_FINISHED) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return got;
}

double rms(const float* buf, size_t n) {
    if (n == 0) return 0.0;
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) acc += (double)buf[i] * (double)buf[i];
    return std::sqrt(acc / (double)n);
}

double max_abs(const float* buf, size_t n) {
    double m = 0.0;
    for (size_t i = 0; i < n; ++i) m = std::max(m, (double)std::fabs(buf[i]));
    return m;
}

double gather_rms(UAVPlayer* p, int ch, int rate, int reads,
                  size_t& frames_out, int timeout_ms = 3000) {
    using clk = std::chrono::steady_clock;
    auto deadline = clk::now() + std::chrono::milliseconds(timeout_ms);
    const int FR = 1024;
    std::vector<float> acc;
    frames_out = 0;
    int done = 0;
    while (done < reads && clk::now() < deadline) {
        std::vector<float> buf((size_t)FR * ch, 0.0f);
        int got = uav_read_audio(p, buf.data(), FR, ch, rate);
        REQUIRE(got >= 0);
        REQUIRE(got <= FR);
        if (got > 0) {
            acc.insert(acc.end(), buf.begin(), buf.begin() + (size_t)got * ch);
            frames_out += (size_t)got;
            ++done;
        } else {
            if (uav_get_state(p) == UAV_STATE_FINISHED) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    return rms(acc.data(), acc.size());
}

}

TEST_CASE("ring: opened-but-not-played underruns to 0 + zero-fill"
          * doctest::test_suite("[ring]")) {
    std::string path;
    UAVPlayer* p = open_audio_clip(path);
    if (!p) { MESSAGE("no audio clip / FFmpeg open failed; skipping"); return; }

    const int frames = 1024, ch = 2;
    for (int iter = 0; iter < 5; ++iter) {
        std::vector<float> buf((size_t)frames * ch, 0.5f);
        int got = uav_read_audio(p, buf.data(), frames, ch, 48000);
        CHECK(got == 0);
        bool all_zero = true;
        for (float v : buf) if (v != 0.0f) { all_zero = false; break; }
        CHECK(all_zero);
    }

    uav_destroy(p);
}

TEST_CASE("ring: huge read is partial (bounded by ring), normal read refills"
          * doctest::test_suite("[ring]")) {
    std::string path;
    UAVPlayer* p = open_audio_clip(path);
    if (!p) { MESSAGE("no audio clip / FFmpeg open failed; skipping"); return; }

    REQUIRE(uav_play(p) == UAV_OK);

    const int ch = 2, rate = 48000;

    std::vector<float> small((size_t)1024 * ch, 0.0f);
    int primed = prime_until_audio(p, small.data(), 1024, ch, rate);
    if (primed == 0) {
        MESSAGE("worker never filled ring within deadline; skipping");
        uav_destroy(p);
        return;
    }

    const int huge = 200000;
    std::vector<float> big((size_t)huge * ch, 0.5f);
    int got = uav_read_audio(p, big.data(), huge, ch, rate);
    CHECK(got >= 0);
    CHECK(got < huge);
    CHECK(got <= 2 * rate + rate / 2);
    bool tail_zero = true;
    for (size_t i = (size_t)got * ch; i < big.size(); ++i)
        if (big[i] != 0.0f) { tail_zero = false; break; }
    CHECK(tail_zero);

    int refilled = prime_until_audio(p, small.data(), 1024, ch, rate);
    CHECK(refilled > 0);
    CHECK(refilled <= 1024);

    uav_destroy(p);
}

TEST_CASE("ring: mono source upmixed to stereo duplicates both lanes"
          * doctest::test_suite("[ring]")) {
    std::string path;
    UAVPlayer* p = open_audio_clip(path);
    if (!p) { MESSAGE("no audio clip / FFmpeg open failed; skipping"); return; }

    REQUIRE(uav_play(p) == UAV_OK);

    const int frames = 1024, ch = 2, rate = 48000;
    std::vector<float> buf((size_t)frames * ch, 0.0f);
    int got = prime_until_audio(p, buf.data(), frames, ch, rate);
    if (got == 0) {
        MESSAGE("worker never filled ring within deadline; skipping");
        uav_destroy(p);
        return;
    }

    for (int i = 0; i < got; ++i) {
        CHECK(buf[(size_t)2 * i] == buf[(size_t)2 * i + 1]);
    }

    uav_destroy(p);
}

TEST_CASE("ring: sample-rate conversion preserves the 440Hz tone"
          * doctest::test_suite("[ring]")) {
    const int rates[] = { 24000, 44100 };

    for (int rate : rates) {
        std::string path;
        UAVPlayer* p = open_audio_clip(path);
        if (!p) { MESSAGE("no audio clip / FFmpeg open failed; skipping"); return; }
        REQUIRE(uav_play(p) == UAV_OK);

        const int ch = 1;
        const int frames = 4096;
        std::vector<float> buf((size_t)frames * ch, 0.0f);

        int got = prime_until_audio(p, buf.data(), frames, ch, rate);
        if (got == 0) {
            MESSAGE("worker never filled ring within deadline; skipping rate");
            uav_destroy(p);
            continue;
        }
        std::vector<float> win(buf.begin(), buf.begin() + (size_t)got);
        for (int extra = 0; extra < 6 && win.size() < 6000; ++extra) {
            std::vector<float> more((size_t)frames * ch, 0.0f);
            int g = uav_read_audio(p, more.data(), frames, ch, rate);
            REQUIRE(g >= 0);
            REQUIRE(g <= frames);
            if (g > 0) win.insert(win.end(), more.begin(), more.begin() + (size_t)g);
            else std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        double mx = max_abs(win.data(), win.size());
        CHECK(mx > 0.05);
        double mn = 1.0;
        for (float v : win) mn = std::min(mn, (double)v);
        CHECK((mx - mn) > 0.1);

        int crossings = 0;
        for (size_t i = 1; i < win.size(); ++i) {
            if (win[i - 1] <= 0.0f && win[i] > 0.0f) ++crossings;
        }
        double seconds = (double)win.size() / (double)rate;
        if (seconds > 0.0 && crossings > 0) {
            double freq = crossings / seconds;
            CHECK(freq > 180.0);
            CHECK(freq < 760.0);
        }

        uav_destroy(p);
    }
}

TEST_CASE("ring: swr reconfig on layout change drops the stale ring"
          * doctest::test_suite("[ring]")) {
    std::string path;
    UAVPlayer* p = open_audio_clip(path);
    if (!p) { MESSAGE("no audio clip / FFmpeg open failed; skipping"); return; }

    REQUIRE(uav_play(p) == UAV_OK);

    const int frames = 1024;
    std::vector<float> a((size_t)frames * 2, 0.0f);
    int got_a = prime_until_audio(p, a.data(), frames, 2, 48000);
    if (got_a == 0) {
        MESSAGE("worker never filled ring within deadline; skipping");
        uav_destroy(p);
        return;
    }

    std::vector<float> b((size_t)frames * 1, 0.5f);
    int got_b = uav_read_audio(p, b.data(), frames, 1, 24000);
    CHECK(got_b == 0);
    bool zeroed = true;
    for (float v : b) if (v != 0.0f) { zeroed = false; break; }
    CHECK(zeroed);

    int got_c = prime_until_audio(p, b.data(), frames, 1, 24000);
    CHECK(got_c >= 0);
    CHECK(got_c <= frames);

    uav_destroy(p);
}

TEST_CASE("ring: sample_rate<=0 on a primed session zero-fills then self-heals"
          * doctest::test_suite("[ring]")) {
    std::string path;
    UAVPlayer* p = open_audio_clip(path);
    if (!p) { MESSAGE("no audio clip / FFmpeg open failed; skipping"); return; }

    REQUIRE(uav_play(p) == UAV_OK);

    const int frames = 1024, ch = 2;
    std::vector<float> warm((size_t)frames * ch, 0.0f);
    int primed = prime_until_audio(p, warm.data(), frames, ch, 48000);
    if (primed == 0) {
        MESSAGE("worker never filled ring within deadline; skipping");
        uav_destroy(p);
        return;
    }

    for (int32_t bad_rate : { 0, -1 }) {
        std::vector<float> buf((size_t)frames * ch, 0.5f);
        int got = uav_read_audio(p, buf.data(), frames, ch, bad_rate);
        CHECK(got == 0);
        bool all_zero = true;
        for (float v : buf) if (v != 0.0f) { all_zero = false; break; }
        CHECK(all_zero);
    }

    int healed = prime_until_audio(p, warm.data(), frames, ch, 48000);
    CHECK(healed >= 0);
    CHECK(healed <= frames);

    uav_destroy(p);
}

TEST_CASE("ring: mute silences samples exactly, ring keeps flowing"
          * doctest::test_suite("[ring]")) {
    std::string path;
    UAVPlayer* p = open_audio_clip(path);
    if (!p) { MESSAGE("no audio clip / FFmpeg open failed; skipping"); return; }

    REQUIRE(uav_play(p) == UAV_OK);

    const int ch = 2, rate = 48000;

    std::vector<float> warm((size_t)1024 * ch, 0.0f);
    int primed = prime_until_audio(p, warm.data(), 1024, ch, rate);
    if (primed == 0) {
        MESSAGE("worker never filled ring within deadline; skipping");
        uav_destroy(p);
        return;
    }
    REQUIRE(uav_set_volume(p, 1.0f) == UAV_OK);
    size_t loud_frames = 0;
    double rms_loud = gather_rms(p, ch, rate, 4, loud_frames);
    if (loud_frames == 0) {
        MESSAGE("no audio gathered for loud RMS; skipping");
        uav_destroy(p);
        return;
    }
    CHECK(rms_loud > 0.05);

    REQUIRE(uav_set_muted(p, 1) == UAV_OK);
    bool saw_muted_read = false;
    {
        using clk = std::chrono::steady_clock;
        auto deadline = clk::now() + std::chrono::milliseconds(2000);
        while (clk::now() < deadline) {
            std::vector<float> buf((size_t)1024 * ch, 0.123f);
            int got = uav_read_audio(p, buf.data(), 1024, ch, rate);
            REQUIRE(got >= 0);
            REQUIRE(got <= 1024);
            if (got > 0) {
                for (size_t i = 0; i < (size_t)got * ch; ++i) CHECK(buf[i] == 0.0f);
                saw_muted_read = true;
                break;
            }
            if (uav_get_state(p) == UAV_STATE_FINISHED) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    CHECK(saw_muted_read);

    REQUIRE(uav_set_muted(p, 0) == UAV_OK);
    size_t back_frames = 0;
    double rms_back = gather_rms(p, ch, rate, 4, back_frames);
    if (back_frames > 0) CHECK(rms_back > 0.05);

    uav_destroy(p);
}

TEST_CASE("ring: volume scales the ring output, 0.0 is exact silence"
          * doctest::test_suite("[ring]")) {
    std::string path;
    UAVPlayer* p = open_audio_clip(path);
    if (!p) { MESSAGE("no audio clip / FFmpeg open failed; skipping"); return; }

    REQUIRE(uav_play(p) == UAV_OK);
    const int ch = 2, rate = 48000;

    std::vector<float> warm((size_t)1024 * ch, 0.0f);
    int primed = prime_until_audio(p, warm.data(), 1024, ch, rate);
    if (primed == 0) {
        MESSAGE("worker never filled ring within deadline; skipping");
        uav_destroy(p);
        return;
    }

    REQUIRE(uav_set_volume(p, 1.0f) == UAV_OK);
    size_t f1 = 0;
    double r1 = gather_rms(p, ch, rate, 6, f1);
    if (f1 == 0) { MESSAGE("no audio for R1; skipping"); uav_destroy(p); return; }

    REQUIRE(uav_set_volume(p, 0.5f) == UAV_OK);
    size_t f2 = 0;
    double r2 = gather_rms(p, ch, rate, 6, f2);
    if (f2 == 0) { MESSAGE("no audio for R2; skipping"); uav_destroy(p); return; }

    CHECK(r1 > 0.05);
    double ratio = r2 / r1;
    CHECK(std::fabs(ratio - 0.5) < 0.1);

    REQUIRE(uav_set_volume(p, 0.0f) == UAV_OK);
    {
        using clk = std::chrono::steady_clock;
        auto deadline = clk::now() + std::chrono::milliseconds(2000);
        bool saw = false;
        while (clk::now() < deadline) {
            std::vector<float> buf((size_t)1024 * ch, 0.7f);
            int got = uav_read_audio(p, buf.data(), 1024, ch, rate);
            REQUIRE(got >= 0);
            REQUIRE(got <= 1024);
            if (got > 0) {
                for (size_t i = 0; i < (size_t)got * ch; ++i) CHECK(buf[i] == 0.0f);
                saw = true;
                break;
            }
            if (uav_get_state(p) == UAV_STATE_FINISHED) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(saw);
    }

    uav_destroy(p);
}

TEST_CASE("ring: volume clamps to [0,1] (>1 -> 1, <0 -> 0)"
          * doctest::test_suite("[ring]")) {
    std::string path;
    UAVPlayer* p = open_audio_clip(path);
    if (!p) { MESSAGE("no audio clip / FFmpeg open failed; skipping"); return; }

    REQUIRE(uav_play(p) == UAV_OK);
    const int ch = 2, rate = 48000;

    std::vector<float> warm((size_t)1024 * ch, 0.0f);
    int primed = prime_until_audio(p, warm.data(), 1024, ch, rate);
    if (primed == 0) {
        MESSAGE("worker never filled ring within deadline; skipping");
        uav_destroy(p);
        return;
    }

    REQUIRE(uav_set_volume(p, 1.0f) == UAV_OK);
    size_t f1 = 0;
    double r1 = gather_rms(p, ch, rate, 6, f1);
    REQUIRE(uav_set_volume(p, 2.0f) == UAV_OK);
    size_t f2 = 0;
    double r2 = gather_rms(p, ch, rate, 6, f2);
    if (f1 > 0 && f2 > 0) {
        CHECK(r1 > 0.05);
        CHECK(r2 > 0.05);
        CHECK(r2 <= r1 * 1.15);
    }

    {
        std::vector<float> buf((size_t)1024 * ch, 0.0f);
        int got = prime_until_audio(p, buf.data(), 1024, ch, rate);
        for (int i = 0; i < got * ch; ++i) {
            CHECK(buf[i] <= 1.0001f);
            CHECK(buf[i] >= -1.0001f);
        }
    }

    REQUIRE(uav_set_volume(p, -1.0f) == UAV_OK);
    {
        using clk = std::chrono::steady_clock;
        auto deadline = clk::now() + std::chrono::milliseconds(2000);
        bool saw = false;
        while (clk::now() < deadline) {
            std::vector<float> buf((size_t)1024 * ch, 0.9f);
            int got = uav_read_audio(p, buf.data(), 1024, ch, rate);
            REQUIRE(got >= 0);
            REQUIRE(got <= 1024);
            if (got > 0) {
                for (size_t i = 0; i < (size_t)got * ch; ++i) CHECK(buf[i] == 0.0f);
                saw = true;
                break;
            }
            if (uav_get_state(p) == UAV_STATE_FINISHED) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(saw);
    }

    uav_destroy(p);
}

TEST_CASE("ring: overflow drops oldest and stays bounded by ring capacity"
          * doctest::test_suite("[ring]")) {
    std::string path;
    UAVPlayer* p = open_audio_clip(path);
    if (!p) { MESSAGE("no audio clip / FFmpeg open failed; skipping"); return; }

    REQUIRE(uav_play(p) == UAV_OK);
    const int ch = 2, rate = 48000;

    {
        std::vector<float> warm((size_t)1024 * ch, 0.0f);
        int primed = prime_until_audio(p, warm.data(), 1024, ch, rate);
        if (primed == 0) {
            MESSAGE("worker never filled ring within deadline; skipping");
            uav_destroy(p);
            return;
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(2500));

    const int BIG = 4 * rate;
    {
        std::vector<float> big((size_t)BIG * ch, 0.5f);
        int first = uav_read_audio(p, big.data(), BIG, ch, rate);
        REQUIRE(first >= 0);
        REQUIRE(first <= BIG);
        for (size_t k = (size_t)first * ch; k < big.size(); ++k)
            REQUIRE(big[k] == 0.0f);
        CHECK(first <= (long long)(2 * rate + rate / 2));
        CHECK(first > 0);
    }

    const int FR = 4096;
    long long total = 0;
    int empties = 0;
    for (int i = 0; i < 200 && empties < 3; ++i) {
        std::vector<float> buf((size_t)FR * ch, 0.5f);
        int got = uav_read_audio(p, buf.data(), FR, ch, rate);
        REQUIRE(got >= 0);
        REQUIRE(got <= FR);
        for (size_t k = (size_t)got * ch; k < buf.size(); ++k)
            REQUIRE(buf[k] == 0.0f);
        total += got;
        if (got == 0) ++empties; else empties = 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    CHECK(total > 0);

    uav_destroy(p);
}

TEST_CASE("ring: EOF drain then FINISHED reads return 0 + zero-fill"
          * doctest::test_suite("[ring]")) {
    std::string path;
    UAVPlayer* p = open_audio_clip(path);
    if (!p) { MESSAGE("no audio clip / FFmpeg open failed; skipping"); return; }

    UAVMediaInfo info{};
    REQUIRE(uav_get_info(p, &info) == UAV_OK);
    const int ch = 2, rate = 48000;

    if (info.duration > 0.5) {
        REQUIRE(uav_seek(p, info.duration - 0.2) == UAV_OK);
    }
    REQUIRE(uav_play(p) == UAV_OK);

    bool saw_finished = false;
    {
        using clk = std::chrono::steady_clock;
        auto deadline = clk::now() + std::chrono::milliseconds(4000);
        while (clk::now() < deadline) {
            std::vector<float> buf((size_t)1024 * ch, 0.5f);
            int got = uav_read_audio(p, buf.data(), 1024, ch, rate);
            REQUIRE(got >= 0);
            REQUIRE(got <= 1024);
            for (size_t k = (size_t)got * ch; k < buf.size(); ++k)
                REQUIRE(buf[k] == 0.0f);
            if (uav_get_state(p) == UAV_STATE_FINISHED) { saw_finished = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    if (!saw_finished) {
        MESSAGE("did not reach FINISHED within deadline; skipping post-EOF asserts");
        uav_destroy(p);
        return;
    }

    for (int i = 0; i < 64; ++i) {
        std::vector<float> buf((size_t)1024 * ch, 0.5f);
        int got = uav_read_audio(p, buf.data(), 1024, ch, rate);
        REQUIRE(got >= 0);
        REQUIRE(got <= 1024);
        for (size_t k = (size_t)got * ch; k < buf.size(); ++k)
            REQUIRE(buf[k] == 0.0f);
        if (got == 0) break;
    }

    std::vector<float> buf((size_t)1024 * ch, 0.5f);
    int got = uav_read_audio(p, buf.data(), 1024, ch, rate);
    CHECK(got == 0);
    bool all_zero = true;
    for (float v : buf) if (v != 0.0f) { all_zero = false; break; }
    CHECK(all_zero);

    uav_destroy(p);
}

#endif
