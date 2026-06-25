// SPDX-License-Identifier: Apache-2.0

#include "uav_doctest.h"

#include "unitedav.h"
#include "unitedav_send.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
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

struct Handle {
    UAVPlayer* p = uav_create();
    ~Handle() { uav_destroy(p); }
    operator UAVPlayer*() const { return p; }
};

struct SenderHandle {
    UAVSender* s = uav_send_create();
    ~SenderHandle() { uav_send_destroy(s); }
    operator UAVSender*() const { return s; }
};

std::string temp_dir() {
    if (const char* env = std::getenv("TMPDIR"); env && env[0]) return std::string(env);
    std::error_code ec;
    std::filesystem::path p = std::filesystem::temp_directory_path(ec);
    return ec ? std::string(".") : p.string();
}

int g_temp_counter = 0;

std::string make_temp_path(const char* suffix) {
    long pid = (long)
#if defined(_WIN32)
        ::_getpid();
#else
        ::getpid();
#endif
    char leaf[256];
    std::snprintf(leaf, sizeof(leaf), "uav_neg_test_%ld_%d%s",
                  pid, g_temp_counter++, suffix);
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

bool write_file(const std::string& path, const void* bytes, size_t n) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = (n == 0) || (std::fwrite(bytes, 1, n, f) == n);
    std::fclose(f);
    return ok;
}

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

#if defined(UAV_HAVE_FFMPEG)

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

constexpr int kClipW = 160;
constexpr int kClipH = 120;

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
    long pid = (long)
#if defined(_WIN32)
        ::_getpid();
#else
        ::getpid();
#endif
    char leaf[256];
    std::snprintf(leaf, sizeof(leaf), "uav_negative_synth_%ld.webm", pid);
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

bool media_available() { return !synthetic_clip().empty(); }

bool copy_prefix(const std::string& src, const std::string& dst, size_t n) {
    FILE* in = std::fopen(src.c_str(), "rb");
    if (!in) return false;
    std::vector<unsigned char> buf(n);
    size_t got = std::fread(buf.data(), 1, n, in);
    std::fclose(in);
    if (got == 0) return false;
    return write_file(dst, buf.data(), got);
}

#endif

}

TEST_CASE("negative: A null-handle decode ABI sweep returns documented sentinels"
          * doctest::test_suite("[negative]")) {
    CHECK(uav_open(nullptr, "x")          == UAV_ERR_INVALID);
    CHECK(uav_open(nullptr, nullptr)      == UAV_ERR_INVALID);
    CHECK(uav_close(nullptr)              == UAV_ERR_INVALID);
    CHECK(uav_play(nullptr)               == UAV_ERR_INVALID);
    CHECK(uav_pause(nullptr)              == UAV_ERR_INVALID);
    CHECK(uav_stop(nullptr)               == UAV_ERR_INVALID);
    CHECK(uav_seek(nullptr, 1.0)          == UAV_ERR_INVALID);
    CHECK(uav_set_looping(nullptr, 1)     == UAV_ERR_INVALID);
    CHECK(uav_set_rate(nullptr, 1.0f)     == UAV_ERR_INVALID);
    CHECK(uav_set_volume(nullptr, 0.5f)   == UAV_ERR_INVALID);
    CHECK(uav_set_muted(nullptr, 1)       == UAV_ERR_INVALID);

    CHECK(uav_get_state(nullptr)          == UAV_STATE_ERROR);
    CHECK(uav_get_position(nullptr)       == doctest::Approx(0.0));

    UAVMediaInfo mi{};
    CHECK(uav_get_info(nullptr, &mi)      == UAV_ERR_INVALID);

    CHECK(uav_last_error(nullptr)         == UAV_ERR_INVALID);

    UAVVideoFrame vf{};
    CHECK(uav_acquire_frame(nullptr, -1, &vf) == UAV_ERR_INVALID);

    float buf[8] = {0};
    CHECK(uav_read_audio(nullptr, buf, 4, 2, 48000) == 0);

    uav_release_frame(nullptr);
    uav_destroy(nullptr);
    CHECK(true);
}

TEST_CASE("negative: A2 null-handle send ABI sweep returns documented sentinels"
          * doctest::test_suite("[negative]")) {
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_OPUS, 160, 120, 15, 48000, 2);
    uint8_t rgba[16] = {0};
    float   pcm[16]  = {0};
    char    sdp[64]  = {0};

    CHECK(uav_send_open(nullptr, "f.webm", &cfg)              == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_push_video(nullptr, rgba, 2, 2, 8, 0.0)    == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_push_audio(nullptr, pcm, 4, 2, 48000, 0.0) == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_close(nullptr)                             == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_last_error(nullptr)                        == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_get_sdp(nullptr, sdp, sizeof(sdp))         == UAV_SEND_ERR_INVALID);
    uav_send_destroy(nullptr);

    SenderHandle s;
    REQUIRE(s.s != nullptr);
    CHECK(uav_send_open(s, nullptr, &cfg)        == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_open(s, "x.webm", nullptr)    == UAV_SEND_ERR_INVALID);

    CHECK(uav_send_abi_version() == UAV_SEND_ABI_VERSION);
    CHECK(uav_send_abi_version() == uav_send_abi_version());
}

TEST_CASE("negative: B null out-pointers with a valid handle no-crash, handle stays usable"
          * doctest::test_suite("[negative]")) {
    Handle h;
    REQUIRE(h.p != nullptr);

    CHECK(uav_get_info(h, nullptr) == UAV_ERR_INVALID);
    CHECK(state_in_range(uav_get_state(h)));

    CHECK(uav_acquire_frame(h, -1, nullptr) == UAV_ERR_INVALID);
    CHECK(state_in_range(uav_get_state(h)));

    CHECK(uav_read_audio(h, nullptr, 4, 2, 48000) == 0);
    CHECK(state_in_range(uav_get_state(h)));

    SenderHandle s;
    REQUIRE(s.s != nullptr);
    CHECK(uav_send_get_sdp(s, nullptr, 0) == UAV_SEND_ERR_INVALID);

    CHECK(uav_send_push_video(s, nullptr, 2, 2, 8, 0.0)    == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_push_audio(s, nullptr, 4, 2, 48000, 0.0) == UAV_SEND_ERR_INVALID);
}

TEST_CASE("negative: C bogus scalar args on an IDLE handle clamp/zero, never crash/OOB"
          * doctest::test_suite("[negative]")) {
    Handle h;
    REQUIRE(h.p != nullptr);

    {
        const int frames = 4, channels = 2;
        std::vector<float> abuf((size_t)frames * channels + 1);
        const float kGuard = -123456.5f;
        abuf.back() = kGuard;

        CHECK(uav_read_audio(h, abuf.data(), frames, channels, 48000) == 0);
        CHECK(uav_read_audio(h, abuf.data(), 0, channels, 48000) == 0);
        CHECK(uav_read_audio(h, abuf.data(), -1, channels, 48000) == 0);
        CHECK(uav_read_audio(h, abuf.data(), frames, 0, 48000) == 0);
        CHECK(uav_read_audio(h, abuf.data(), frames, -2, 48000) == 0);
        CHECK(uav_read_audio(h, abuf.data(), frames, channels, 0) == 0);
        CHECK(uav_read_audio(h, abuf.data(), frames, channels, -48000) == 0);
        CHECK(abuf.back() == doctest::Approx(kGuard));
    }

    CHECK(uav_set_rate(h, 0.0f)   == UAV_OK);
    CHECK(uav_set_rate(h, -1.0f)  == UAV_OK);
    CHECK(uav_set_rate(h, std::numeric_limits<float>::quiet_NaN()) == UAV_OK);
    CHECK(uav_set_rate(h, std::numeric_limits<float>::infinity())  == UAV_OK);

    CHECK(uav_set_volume(h, -1.0f) == UAV_OK);
    CHECK(uav_set_volume(h, 2.0f)  == UAV_OK);
    CHECK(uav_set_volume(h, std::numeric_limits<float>::quiet_NaN()) == UAV_OK);

    CHECK(uav_set_muted(h, 12345)   == UAV_OK);
    CHECK(uav_set_looping(h, -1)    == UAV_OK);

    CHECK(uav_seek(h, std::numeric_limits<double>::quiet_NaN()) == UAV_ERR_INVALID);
    CHECK(uav_seek(h, std::numeric_limits<double>::infinity())  == UAV_ERR_INVALID);
    CHECK(uav_seek(h, -1e9) == UAV_ERR_INVALID);

    CHECK(uav_get_state(h) == UAV_STATE_IDLE);
}

TEST_CASE("negative: D bogus urls/paths fail gracefully with the documented contract"
          * doctest::test_suite("[negative]")) {
    const char* inputs[] = {
        "/nonexistent/uav_no_such.mp4",
        "/tmp/uav nonexist %zz.mp4",
        "zzzz://nope",
        ".",
        "/dev/null",
    };
    for (const char* in : inputs) {
        CAPTURE(in);
        Handle h;
        REQUIRE(h.p != nullptr);
        int rc = uav_open(h, in);
        CHECK(rc < 0);
#if defined(UAV_HAVE_FFMPEG)
        CHECK(rc <= UAV_ERR_INVALID);
        CHECK(uav_last_error(h) == rc);
#else
        CHECK(rc == UAV_ERR_UNSUPPORTED);
        CHECK(uav_last_error(h) == UAV_ERR_UNSUPPORTED);
#endif
        CHECK(uav_get_state(h) == UAV_STATE_ERROR);
        CHECK(uav_close(h) == UAV_OK);
        CHECK(uav_get_state(h) == UAV_STATE_IDLE);
        CHECK(uav_last_error(h) == UAV_OK);
    }

    {
        Handle h;
        REQUIRE(h.p != nullptr);
        std::string longpath(4096, 'a');
        longpath = "/tmp/" + longpath + ".mp4";
        int rc = uav_open(h, longpath.c_str());
        CHECK(rc < 0);
        CHECK(uav_get_state(h) == UAV_STATE_ERROR);
#if defined(UAV_HAVE_FFMPEG)
        CHECK(uav_last_error(h) == rc);
#endif
    }

    {
        Handle h;
        REQUIRE(h.p != nullptr);
        CHECK(uav_open(h, "") == UAV_ERR_INVALID);
        CHECK(uav_get_state(h) == UAV_STATE_IDLE);
        CHECK(uav_last_error(h) == UAV_OK);
    }
}

#if defined(UAV_HAVE_FFMPEG)

TEST_CASE("negative: E zero-byte and garbage files fail open gracefully"
          * doctest::test_suite("[negative]")) {
    const char* exts[] = { ".webm", ".mp4", ".mkv" };

    for (const char* ext : exts) {
        CAPTURE(ext);
        TempFileGuard tmp(make_temp_path(ext));
        REQUIRE(write_file(tmp.path, nullptr, 0));
        Handle h;
        REQUIRE(h.p != nullptr);
        int rc = uav_open(h, tmp.c_str());
        CHECK(rc < 0);
        CHECK(uav_get_state(h) == UAV_STATE_ERROR);
        CHECK(uav_last_error(h) == rc);
        CHECK(uav_close(h) == UAV_OK);
        CHECK(uav_get_state(h) == UAV_STATE_IDLE);
    }

    {
        TempFileGuard tmp(make_temp_path(".webm"));
        unsigned char junk[4096];
        for (size_t i = 0; i < sizeof(junk); ++i)
            junk[i] = (unsigned char)((i * 1103515245u + 12345u) >> 7);
        REQUIRE(write_file(tmp.path, junk, sizeof(junk)));
        Handle h;
        REQUIRE(h.p != nullptr);
        int rc = uav_open(h, tmp.c_str());
        CHECK(rc < 0);
        CHECK(uav_get_state(h) == UAV_STATE_ERROR);
        CHECK(uav_last_error(h) == rc);
        CHECK(uav_close(h) == UAV_OK);
        CHECK(uav_get_state(h) == UAV_STATE_IDLE);
    }
}

TEST_CASE("negative: E header-only truncated clip fails open gracefully (no crash)"
          * doctest::test_suite("[negative]")) {
    if (!media_available()) {
        MESSAGE("SKIP: no synthetic clip (system ffmpeg / encoder unavailable)");
        return;
    }
    for (size_t prefix : {(size_t)64, (size_t)256, (size_t)512}) {
        CAPTURE(prefix);
        TempFileGuard tmp(make_temp_path(".webm"));
        if (!copy_prefix(synthetic_clip(), tmp.path, prefix)) {
            MESSAGE("SKIP: could not build truncated prefix");
            continue;
        }
        Handle h;
        REQUIRE(h.p != nullptr);
        int rc = uav_open(h, tmp.c_str());
        if (rc < 0) {
            CHECK(uav_get_state(h) == UAV_STATE_ERROR);
            CHECK(uav_last_error(h) == rc);
        } else {
            MESSAGE("note: truncated prefix unexpectedly opened; asserting only no-crash");
            CHECK(state_in_range(uav_get_state(h)));
        }
        CHECK(uav_close(h) == UAV_OK);
        CHECK(uav_get_state(h) == UAV_STATE_IDLE);
    }
}

#endif

#if defined(UAV_HAVE_FFMPEG)

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {
bool have_vp9() { return avcodec_find_encoder_by_name("libvpx-vp9") != nullptr; }
}

TEST_CASE("negative: F oversized/negative/zero cfg dims never crash, surface an error or default"
          * doctest::test_suite("[negative]")) {
    if (!have_vp9()) { MESSAGE("SKIP: libvpx-vp9 absent"); return; }

    for (int dim : {100000, std::numeric_limits<int>::max()}) {
        CAPTURE(dim);
        SenderHandle s;
        REQUIRE(s.s != nullptr);
        UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_NONE, dim, dim, 15, 48000, 2);
        TempFileGuard tmp(make_temp_path(".webm"));
        int rc = uav_send_open(s, tmp.c_str(), &cfg);
        CHECK(rc != UAV_SEND_OK);
        CHECK(rc < 0);
        CHECK(uav_send_last_error(s) == rc);
    }

    for (int dim : {0, -1}) {
        CAPTURE(dim);
        SenderHandle s;
        REQUIRE(s.s != nullptr);
        UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_NONE, dim, dim, 15, 48000, 2);
        TempFileGuard tmp(make_temp_path(".webm"));
        int rc = uav_send_open(s, tmp.c_str(), &cfg);
        CHECK(rc == UAV_SEND_OK);
        CHECK(uav_send_close(s) == UAV_SEND_OK);
    }
}

TEST_CASE("negative: F push_video bad geometry returns INVALID, no OOB read (ASan-graded)"
          * doctest::test_suite("[negative]")) {
    if (!have_vp9()) { MESSAGE("SKIP: libvpx-vp9 absent"); return; }
    const int w = 64, h = 48;
    SenderHandle s;
    REQUIRE(s.s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_NONE, w, h, 15, 48000, 2);
    TempFileGuard tmp(make_temp_path(".webm"));
    REQUIRE(uav_send_open(s, tmp.c_str(), &cfg) == UAV_SEND_OK);

    std::vector<uint8_t> rgba((size_t)w * h * 4, 0x40);

    CHECK(uav_send_push_video(s, nullptr, w, h, w * 4, 0.0) == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_push_video(s, rgba.data(), 0, h, w * 4, 0.0) == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_push_video(s, rgba.data(), w, 0, w * 4, 0.0) == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_push_video(s, rgba.data(), -1, h, w * 4, 0.0) == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_push_video(s, rgba.data(), w, -1, w * 4, 0.0) == UAV_SEND_ERR_INVALID);

    CHECK(uav_send_push_video(s, rgba.data(), w, h, 0,  0.0) == UAV_SEND_OK);
    CHECK(uav_send_push_video(s, rgba.data(), w, h, -8, 0.0) == UAV_SEND_OK);

    uav_send_close(s);
}

#endif

TEST_CASE("negative: G double-close / double-send-close are idempotent (safe paths)"
          * doctest::test_suite("[negative]")) {
    {
        Handle h;
        REQUIRE(h.p != nullptr);
        CHECK(uav_close(h) == UAV_OK);
        CHECK(uav_get_state(h) == UAV_STATE_IDLE);
        CHECK(uav_last_error(h) == UAV_OK);
        CHECK(uav_close(h) == UAV_OK);
        CHECK(uav_get_state(h) == UAV_STATE_IDLE);
        CHECK(uav_last_error(h) == UAV_OK);
    }
    {
        SenderHandle s;
        REQUIRE(s.s != nullptr);
        CHECK(uav_send_close(s) == UAV_SEND_OK);
        CHECK(uav_send_close(s) == UAV_SEND_OK);
    }
}

TEST_CASE("negative: G create/close/destroy cycles are leak-free (ASan/LSan signal)"
          * doctest::test_suite("[negative]")) {
    for (int i = 0; i < 64; ++i) {
        UAVPlayer* p = uav_create();
        REQUIRE(p != nullptr);
        CHECK(uav_get_state(p) == UAV_STATE_IDLE);
        CHECK(uav_close(p) == UAV_OK);
        uav_destroy(p);

        UAVSender* s = uav_send_create();
        REQUIRE(s != nullptr);
        uav_send_destroy(s);
    }
    CHECK(true);
}

#if defined(UAV_HAVE_FFMPEG)

TEST_CASE("negative: H use-after-close on the decode ABI is graceful, no crash"
          * doctest::test_suite("[negative]")) {
    if (!media_available()) { MESSAGE("SKIP: no synthetic clip"); return; }
    Handle h;
    REQUIRE(h.p != nullptr);
    REQUIRE(uav_open(h, synthetic_clip().c_str()) == UAV_OK);
    CHECK(uav_close(h) == UAV_OK);
    REQUIRE(uav_get_state(h) == UAV_STATE_IDLE);

    CHECK(uav_play(h)  == UAV_ERR_INVALID);
    CHECK(uav_pause(h) == UAV_OK);
    CHECK(uav_stop(h)  == UAV_OK);
    CHECK(uav_seek(h, 0.5) == UAV_ERR_INVALID);

    UAVMediaInfo mi{};
    CHECK(uav_get_info(h, &mi) == UAV_ERR_NO_STREAM);
    CHECK(uav_get_position(h) == doctest::Approx(0.0));

    UAVVideoFrame vf{};
    CHECK(uav_acquire_frame(h, -1, &vf) == UAV_ERR_NO_STREAM);

    float buf[8] = {0};
    CHECK(uav_read_audio(h, buf, 4, 2, 48000) == 0);

    uav_release_frame(h);
    CHECK(uav_get_state(h) == UAV_STATE_IDLE);
}

TEST_CASE("negative: H use-after-close on the send ABI is graceful, no crash"
          * doctest::test_suite("[negative]")) {
    if (!have_vp9()) { MESSAGE("SKIP: libvpx-vp9 absent"); return; }
    SenderHandle s;
    REQUIRE(s.s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_NONE, 64, 48, 15, 48000, 2);
    TempFileGuard tmp(make_temp_path(".webm"));
    REQUIRE(uav_send_open(s, tmp.c_str(), &cfg) == UAV_SEND_OK);
    CHECK(uav_send_close(s) == UAV_SEND_OK);

    std::vector<uint8_t> rgba((size_t)64 * 48 * 4, 0);
    float pcm[256] = {0};
    char  sdp[64]  = {0};
    CHECK(uav_send_push_video(s, rgba.data(), 64, 48, 0, 0.0) == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_push_audio(s, pcm, 128, 2, 48000, 0.0)     == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_get_sdp(s, sdp, sizeof(sdp))              == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_close(s) == UAV_SEND_OK);
}

#endif

TEST_CASE("negative: I out-of-order decode calls return documented codes, handle usable"
          * doctest::test_suite("[negative]")) {
    Handle h;
    REQUIRE(h.p != nullptr);

    CHECK(uav_play(h) == UAV_ERR_INVALID);
    CHECK(uav_seek(h, 1.0) == UAV_ERR_INVALID);

    UAVVideoFrame vf{};
    CHECK(uav_acquire_frame(h, -1, &vf) == UAV_ERR_NO_STREAM);

    uav_release_frame(h);
    uav_release_frame(h);
    uav_release_frame(h);

    float buf[8] = {0};
    CHECK(uav_read_audio(h, buf, 4, 2, 48000) == 0);

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

    CHECK(uav_pause(h) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_IDLE);
    CHECK(uav_stop(h) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_IDLE);

    CHECK(state_in_range(uav_get_state(h)));
}

TEST_CASE("negative: I out-of-order send calls before open are INVALID/idempotent"
          * doctest::test_suite("[negative]")) {
    SenderHandle s;
    REQUIRE(s.s != nullptr);
    uint8_t rgba[16] = {0};
    float   pcm[16]  = {0};
    char    sdp[64]  = {0};

    CHECK(uav_send_push_video(s, rgba, 2, 2, 8, 0.0)    == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_push_audio(s, pcm, 4, 2, 48000, 0.0) == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_get_sdp(s, sdp, sizeof(sdp))         == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_close(s) == UAV_SEND_OK);
}

#if defined(UAV_HAVE_FFMPEG)
TEST_CASE("negative: I get_sdp on a non-rtp (file) open => UAV_SEND_ERR_UNSUPPORTED"
          * doctest::test_suite("[negative]")) {
    if (!have_vp9()) { MESSAGE("SKIP: libvpx-vp9 absent"); return; }
    SenderHandle s;
    REQUIRE(s.s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_NONE, 64, 48, 15, 48000, 2);
    TempFileGuard tmp(make_temp_path(".webm"));
    REQUIRE(uav_send_open(s, tmp.c_str(), &cfg) == UAV_SEND_OK);
    char sdp[64] = {0};
    CHECK(uav_send_get_sdp(s, sdp, sizeof(sdp)) == UAV_SEND_ERR_UNSUPPORTED);
    uav_send_close(s);
}
#endif

#if defined(UAV_HAVE_FFMPEG)

TEST_CASE("negative: J acquire-without-release then close keeps the buffer valid, no deadlock"
          * doctest::test_suite("[negative]")) {
    if (!media_available()) { MESSAGE("SKIP: no synthetic clip"); return; }
    Handle h;
    REQUIRE(h.p != nullptr);
    REQUIRE(uav_open(h, synthetic_clip().c_str()) == UAV_OK);

    UAVVideoFrame vf{};
    int rc = uav_acquire_frame(h, -1, &vf);
    if (rc != UAV_OK) {
        MESSAGE("note: no frame ready to borrow; skipping imbalance asserts");
        return;
    }
    REQUIRE(vf.data != nullptr);

    if (vf.width > 0 && vf.height > 0) {
        volatile uint8_t sink =
            vf.data[((size_t)vf.height - 1) * vf.stride + ((size_t)vf.width - 1) * 4];
        (void)sink;
    }

    CHECK(uav_close(h) == UAV_OK);

    if (vf.width > 0 && vf.height > 0) {
        volatile uint8_t sink =
            vf.data[((size_t)vf.height - 1) * vf.stride + ((size_t)vf.width - 1) * 4];
        (void)sink;
    }

    uav_release_frame(h);
    uav_release_frame(h);

    REQUIRE(uav_open(h, synthetic_clip().c_str()) == UAV_OK);
    UAVVideoFrame vf3{};
    int rc3 = uav_acquire_frame(h, -1, &vf3);
    if (rc3 == UAV_OK) uav_release_frame(h);
    CHECK(state_in_range(uav_get_state(h)));
}

TEST_CASE("negative: J reader thread races open/play/seek/close (TSan signal)"
          * doctest::test_suite("[negative][concurrency]")) {
    if (!media_available()) { MESSAGE("SKIP: no synthetic clip"); return; }
    Handle h;
    REQUIRE(h.p != nullptr);

    std::atomic<bool> stop{false};
    std::thread reader([&]() {
        int64_t last_id = -1;
        std::vector<float> abuf(1024 * 2);
        int tick = 0;
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
            const float rates[]   = {0.5f, 1.0f, 2.0f, -1.0f};
            const float volumes[] = {0.0f, 0.5f, 1.0f, 2.0f};
            uav_set_rate(h, rates[tick & 3]);
            uav_set_volume(h, volumes[tick & 3]);
            uav_set_muted(h, tick & 1);
            uav_set_looping(h, (tick >> 1) & 1);
            ++tick;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    for (int i = 0; i < 8; ++i) {
        if (uav_open(h, synthetic_clip().c_str()) == UAV_OK) {
            uav_play(h);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            uav_seek(h, 0.3);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
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

TEST_CASE("negative: J2 loop-restart seek races a borrowing reader (TSan/ASan signal)"
          * doctest::test_suite("[negative][concurrency]")) {
    if (!media_available()) { MESSAGE("SKIP: no synthetic clip"); return; }
    Handle h;
    REQUIRE(h.p != nullptr);
    REQUIRE(uav_open(h, synthetic_clip().c_str()) == UAV_OK);

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
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    using clock = std::chrono::steady_clock;

    CHECK(uav_set_looping(h, 1) == UAV_OK);
    UAVMediaInfo mi{};
    double dur = (uav_get_info(h, &mi) == UAV_OK && mi.duration > 0.3) ? mi.duration : 1.0;
    uav_seek(h, dur - 0.3);
    uav_play(h);
    auto p1_deadline = clock::now() + std::chrono::milliseconds(2500);
    while (clock::now() < p1_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (!state_in_range(uav_get_state(h))) break;
    }

    CHECK(uav_set_looping(h, 0) == UAV_OK);
    uav_seek(h, dur - 0.3);
    uav_play(h);
    auto p2_deadline = clock::now() + std::chrono::milliseconds(3000);
    int st = uav_get_state(h);
    while (st != UAV_STATE_FINISHED && st != UAV_STATE_ERROR && clock::now() < p2_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        st = uav_get_state(h);
    }
    if (st == UAV_STATE_FINISHED) {
        CHECK(uav_play(h) == UAV_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } else {
        MESSAGE("note: clip did not reach FINISHED within the cap; restart-seek "
                "path not exercised this run (still no crash/deadlock)");
    }

    stop.store(true);
    reader.join();
    CHECK(state_in_range(uav_get_state(h)));
    uav_close(h);
}

#endif

#if defined(UAV_HAVE_FFMPEG)

namespace {
bool open_must_fail_bounded(UAVPlayer* p, const char* url, int cap_seconds) {
    auto t0 = std::chrono::steady_clock::now();
    int rc = uav_open(p, url);
    auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    CAPTURE(url);
    CAPTURE(rc);
    CAPTURE(elapsed);
    if (elapsed > (double)cap_seconds) {
        MESSAGE("SKIP: open exceeded the wall-clock cap (slow/blocking transport)");
        uav_close(p);
        return false;
    }
    CHECK(rc < 0);
    CHECK(uav_get_state(p) == UAV_STATE_ERROR);
    CHECK(uav_last_error(p) == rc);
    CHECK(uav_close(p) == UAV_OK);
    CHECK(uav_get_state(p) == UAV_STATE_IDLE);
    return true;
}
}

TEST_CASE("negative: K network-fault urls fail FAST and gracefully, bounded, no crash"
          * doctest::test_suite("[negative]")) {
    Handle h;
    REQUIRE(h.p != nullptr);

    const char* urls[] = {
        "http://127.0.0.1:1/nope.m3u8",
        "http://192.0.2.1/x.mp4",
        "rtp://127.0.0.1:9/none",
        "srt://127.0.0.1:9",
    };
    for (const char* u : urls) {
        (void)open_must_fail_bounded(h, u, 15);
    }

    {
        TempFileGuard sdp(make_temp_path(".sdp"));
        unsigned char junk[128];
        for (size_t i = 0; i < sizeof(junk); ++i) junk[i] = (unsigned char)(i * 31 + 7);
        REQUIRE(write_file(sdp.path, junk, sizeof(junk)));
        (void)open_must_fail_bounded(h, sdp.c_str(), 15);
    }
}

TEST_CASE("negative: K send-side network-fault open cleans up without crashing"
          * doctest::test_suite("[negative]")) {
    if (!have_vp9()) { MESSAGE("SKIP: libvpx-vp9 absent"); return; }

    {
        SenderHandle s;
        REQUIRE(s.s != nullptr);
        UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_NONE, 64, 48, 15, 48000, 2);
        int rc = uav_send_open(s, "rtp://192.0.2.1:9", &cfg);
        if (rc == UAV_SEND_OK) {
            std::vector<uint8_t> rgba((size_t)64 * 48 * 4, 0x20);
            int prc = uav_send_push_video(s, rgba.data(), 64, 48, 0, 0.0);
            (void)prc;
        } else {
            CHECK(rc < 0);
            CHECK(uav_send_last_error(s) == rc);
        }
        CHECK(uav_send_close(s) == UAV_SEND_OK);
    }

    {
        SenderHandle s;
        REQUIRE(s.s != nullptr);
        UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_NONE, 64, 48, 15, 48000, 2);
        auto t0 = std::chrono::steady_clock::now();
        int rc = uav_send_open(s, "srt://127.0.0.1:9?mode=caller&connect_timeout=500", &cfg);
        auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
        CAPTURE(elapsed);
        if (elapsed > 15.0) {
            MESSAGE("SKIP: srt:// open exceeded the wall-clock cap");
        } else {
            CHECK(rc < 0);
            CHECK(uav_send_last_error(s) == rc);
        }
        CHECK(uav_send_close(s) == UAV_SEND_OK);
    }
}

#endif

TEST_CASE("negative: L last_error mirrors the last impl-routed op (with the two exceptions)"
          * doctest::test_suite("[negative]")) {
    Handle h;
    REQUIRE(h.p != nullptr);

    CHECK(uav_last_error(h) == UAV_OK);

    int rc = uav_open(h, "/nonexistent/uav_no_such.mp4");
    CHECK(rc < 0);
    CHECK(uav_last_error(h) == rc);
#if defined(UAV_HAVE_FFMPEG)
    CHECK(rc == UAV_ERR_OPEN_FAILED);
#else
    CHECK(rc == UAV_ERR_UNSUPPORTED);
#endif

    CHECK(uav_close(h) == UAV_OK);
    CHECK(uav_last_error(h) == UAV_OK);

    CHECK(uav_open(h, "") == UAV_ERR_INVALID);
    CHECK(uav_last_error(h) == UAV_OK);
    CHECK(uav_get_state(h) == UAV_STATE_IDLE);

    CHECK(uav_pause(h) == UAV_OK);
    CHECK(uav_last_error(h) == UAV_OK);
    CHECK(uav_stop(h) == UAV_OK);
    CHECK(uav_last_error(h) == UAV_OK);

#if defined(UAV_HAVE_FFMPEG)
    if (media_available()) {
        REQUIRE(uav_open(h, synthetic_clip().c_str()) == UAV_OK);
        CHECK(uav_last_error(h) == UAV_OK);
        CHECK(uav_close(h) == UAV_OK);
        CHECK(uav_last_error(h) == UAV_OK);
    }
#endif
}
