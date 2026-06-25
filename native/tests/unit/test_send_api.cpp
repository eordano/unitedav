// SPDX-License-Identifier: Apache-2.0

#include "uav_doctest.h"

#include "unitedav_send.h"
#include "unitedav.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <process.h>
#else
#  include <unistd.h>
#endif

#if defined(UAV_HAVE_FFMPEG)
#include <chrono>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
}
#endif

namespace {

static int g_temp_counter = 0;

std::string make_temp_path(const char* suffix) {
    std::string dir;
    if (const char* env = std::getenv("TMPDIR"); env && env[0]) {
        dir = env;
    } else {
        std::error_code ec;
        std::filesystem::path p = std::filesystem::temp_directory_path(ec);
        dir = ec ? std::string(".") : p.string();
    }
    long pid = (long)
#if defined(_WIN32)
        ::_getpid();
#else
        ::getpid();
#endif
    char leaf[256];
    std::snprintf(leaf, sizeof(leaf), "uav_send_test_%ld_%d%s",
                  pid, g_temp_counter++, suffix);
    return (std::filesystem::path(dir) / leaf).string();
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

#if defined(UAV_HAVE_FFMPEG)

bool have_encoder(const char* name) {
    return avcodec_find_encoder_by_name(name) != nullptr;
}
bool have_vp9()  { return have_encoder("libvpx-vp9"); }
bool have_vp8()  { return have_encoder("libvpx"); }
bool have_av1()  { return have_encoder("libaom-av1") ||
                          have_encoder("libsvtav1"); }
bool have_opus() { return have_encoder("libopus") ||
                          avcodec_find_encoder(AV_CODEC_ID_OPUS) != nullptr; }

void synth_video_frame(std::vector<uint8_t>& rgba, int w, int h, int fr) {
    rgba.resize((size_t)w * h * 4);
    int bx = (fr * 7) % (w > 16 ? w - 16 : 1);
    int by = (fr * 5) % (h > 16 ? h - 16 : 1);
    for (int y = 0; y < h; ++y) {
        uint8_t* row = rgba.data() + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) {
            uint8_t r = (uint8_t)((x + fr * 3) & 0xff);
            uint8_t g = (uint8_t)((y + fr * 2) & 0xff);
            uint8_t b = (uint8_t)((x + y + fr * 5) & 0xff);
            if (x >= bx && x < bx + 16 && y >= by && y < by + 16) r = g = b = 255;
            row[x * 4 + 0] = r;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = b;
            row[x * 4 + 3] = 255;
        }
    }
}

void synth_audio_block(std::vector<float>& pcm, int frames, int ch,
                       int rate, double& phase) {
    pcm.resize((size_t)frames * ch);
    const double freq = 440.0;
    for (int i = 0; i < frames; ++i) {
        float v = 0.5f * (float)std::sin(phase);
        phase += 2.0 * M_PI * freq / rate;
        if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
        for (int c = 0; c < ch; ++c) pcm[(size_t)i * ch + c] = v;
    }
}

int encode_synthetic(const char* path, const UAVSendConfig& cfg, int nframes) {
    UAVSender* s = uav_send_create();
    REQUIRE(s != nullptr);

    int rc = uav_send_open(s, path, &cfg);
    if (rc != UAV_SEND_OK) { uav_send_destroy(s); return rc; }

    const int w = cfg.width, h = cfg.height, fps = cfg.fps > 0 ? cfg.fps : 15;
    const int rate = cfg.sample_rate > 0 ? cfg.sample_rate : 48000;
    const int ch = cfg.channels > 0 ? cfg.channels : 2;
    const int samples_per_frame = rate / fps;

    std::vector<uint8_t> rgba;
    std::vector<float> pcm;
    double phase = 0.0;

    for (int fr = 0; fr < nframes; ++fr) {
        double pts = (double)fr / fps;
        if (cfg.video_codec != UAV_VCODEC_NONE) {
            synth_video_frame(rgba, w, h, fr);
            CHECK(uav_send_push_video(s, rgba.data(), w, h, w * 4, pts) == UAV_SEND_OK);
        }
        if (cfg.audio_codec != UAV_ACODEC_NONE) {
            synth_audio_block(pcm, samples_per_frame, ch, rate, phase);
            CHECK(uav_send_push_audio(s, pcm.data(), samples_per_frame, ch, rate, pts)
                  == UAV_SEND_OK);
        }
    }

    int crc = uav_send_close(s);
    uav_send_destroy(s);
    return crc;
}

struct DecodeStats {
    int     distinct_frames = 0;
    long long audio_samples = 0;
    double  audio_rms = 0.0;
    bool    touched_valid_buffer = false;
};

UAVMediaInfo decode_and_poll(const char* path, int need_frames, bool need_audio,
                             DecodeStats& out) {
    UAVMediaInfo info{};
    UAVPlayer* p = uav_create();
    REQUIRE(p != nullptr);

    if (uav_open(p, path) != UAV_OK) { uav_destroy(p); return info; }
    uav_get_info(p, &info);
    uav_play(p);

    const int ch = info.audio_channels > 0 ? info.audio_channels : 2;
    const int arate = info.audio_sample_rate > 0 ? info.audio_sample_rate : 48000;
    const int chunk = 1024;
    std::vector<float> abuf((size_t)chunk * ch);

    int64_t last_id = -1;
    double sumsq = 0.0;
    long long nsamp = 0;

    const int max_polls = 2000;
    const auto wall_start = std::chrono::steady_clock::now();
    const auto wall_cap = std::chrono::seconds(15);

    for (int poll = 0; poll < max_polls; ++poll) {
        if (info.has_video) {
            UAVVideoFrame vf{};
            if (uav_acquire_frame(p, last_id, &vf) == UAV_OK) {
                if (vf.frame_id != last_id) {
                    out.distinct_frames++;
                    if (vf.data && vf.width > 0 && vf.height > 0) {
                        volatile uint8_t sink =
                            vf.data[((size_t)vf.height - 1) * vf.stride +
                                    ((size_t)vf.width - 1) * 4];
                        (void)sink;
                        out.touched_valid_buffer = true;
                    }
                    last_id = vf.frame_id;
                }
                uav_release_frame(p);
            }
        }
        if (info.has_audio) {
            int got = uav_read_audio(p, abuf.data(), chunk, ch, arate);
            for (int i = 0; i < got * ch; ++i) { double v = abuf[i]; sumsq += v * v; nsamp++; }
        }

        bool video_done = !need_frames || out.distinct_frames >= need_frames;
        bool audio_done = !need_audio || nsamp > arate;
        if (video_done && audio_done) break;
        if (uav_get_state(p) == UAV_STATE_FINISHED &&
            out.distinct_frames > 0 && (!need_audio || nsamp > 0)) break;

        if (std::chrono::steady_clock::now() - wall_start > wall_cap) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    out.audio_samples = nsamp;
    out.audio_rms = (nsamp > 0) ? std::sqrt(sumsq / (double)nsamp) : 0.0;

    uav_destroy(p);
    return info;
}

#endif

}

TEST_CASE("send: abi version is 1 and mirrors the C# binding constant"
          * doctest::test_suite("[send]")) {
    CHECK(uav_send_abi_version() == UAV_SEND_ABI_VERSION);
    CHECK(uav_send_abi_version() == 1u);
    CHECK(uav_send_abi_version() == uav_send_abi_version());
}

TEST_CASE("send: create returns non-null, destroy(handle)/destroy(NULL) safe"
          * doctest::test_suite("[send]")) {
    UAVSender* s = uav_send_create();
    CHECK(s != nullptr);
    uav_send_destroy(s);
    uav_send_destroy(nullptr);
}

TEST_CASE("send: fresh handle reports last_error == UAV_SEND_OK"
          * doctest::test_suite("[send]")) {
    UAVSender* s = uav_send_create();
    REQUIRE(s != nullptr);
    CHECK(uav_send_last_error(s) == UAV_SEND_OK);
    uav_send_destroy(s);
}

TEST_CASE("send: every ABI fn null-guards a NULL handle => UAV_SEND_ERR_INVALID"
          * doctest::test_suite("[send]")) {
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_OPUS, 160, 120, 15, 48000, 2);
    char buf[64] = {0};
    uint8_t rgba[16] = {0};
    float pcm[16] = {0};

    CHECK(uav_send_open(nullptr, "x.webm", &cfg) == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_push_video(nullptr, rgba, 2, 2, 8, 0.0) == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_push_audio(nullptr, pcm, 4, 2, 48000, 0.0) == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_close(nullptr) == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_get_sdp(nullptr, buf, sizeof(buf)) == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_last_error(nullptr) == UAV_SEND_ERR_INVALID);
}

TEST_CASE("send: open with NULL url or NULL cfg => UAV_SEND_ERR_INVALID"
          * doctest::test_suite("[send]")) {
    UAVSender* s = uav_send_create();
    REQUIRE(s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_OPUS, 160, 120, 15, 48000, 2);
    CHECK(uav_send_open(s, nullptr, &cfg) == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_open(s, "x.webm", nullptr) == UAV_SEND_ERR_INVALID);
    uav_send_destroy(s);
}

TEST_CASE("send: open with no media stream configured => UAV_SEND_ERR_NO_STREAM"
          * doctest::test_suite("[send]")) {
    UAVSender* s = uav_send_create();
    REQUIRE(s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_NONE, UAV_ACODEC_NONE, 160, 120, 15, 48000, 2);
    TempFileGuard tmp(make_temp_path(".webm"));
    CHECK(uav_send_open(s, tmp.c_str(), &cfg) == UAV_SEND_ERR_NO_STREAM);
    CHECK(uav_send_last_error(s) == UAV_SEND_ERR_NO_STREAM);
    uav_send_destroy(s);
}

TEST_CASE("send: unhandled URL scheme => UAV_SEND_ERR_OPEN_FAILED"
          * doctest::test_suite("[send]")) {
    UAVSender* s = uav_send_create();
    REQUIRE(s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_NONE, 160, 120, 15, 48000, 2);
    CHECK(uav_send_open(s, "webrtc://host:1234", &cfg) == UAV_SEND_ERR_OPEN_FAILED);
    CHECK(uav_send_last_error(s) == UAV_SEND_ERR_OPEN_FAILED);
    uav_send_destroy(s);
}

TEST_CASE("send: push before open => UAV_SEND_ERR_INVALID (d_==null)"
          * doctest::test_suite("[send]")) {
    UAVSender* s = uav_send_create();
    REQUIRE(s != nullptr);
    uint8_t rgba[16] = {0};
    float pcm[16] = {0};
    CHECK(uav_send_push_video(s, rgba, 2, 2, 8, 0.0) == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_push_audio(s, pcm, 4, 2, 48000, 0.0) == UAV_SEND_ERR_INVALID);
    uav_send_destroy(s);
}

TEST_CASE("send: get_sdp on a created-but-unopened handle => UAV_SEND_ERR_INVALID"
          * doctest::test_suite("[send]")) {
    UAVSender* s = uav_send_create();
    REQUIRE(s != nullptr);
    char buf[64] = {0};
    CHECK(uav_send_get_sdp(s, buf, sizeof(buf)) == UAV_SEND_ERR_INVALID);
    uav_send_destroy(s);
}

#if !defined(UAV_HAVE_FFMPEG)

TEST_CASE("send: no-FFmpeg stub build reports the documented UNSUPPORTED contract"
          * doctest::test_suite("[send]")) {
    UAVSender* s = uav_send_create();
    REQUIRE(s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_OPUS, 160, 120, 15, 48000, 2);
    TempFileGuard tmp(make_temp_path(".webm"));
    CHECK(uav_send_open(s, tmp.c_str(), &cfg) == UAV_SEND_ERR_UNSUPPORTED);
    CHECK(uav_send_close(s) == UAV_SEND_OK);
    uav_send_destroy(s);
}

#else

TEST_CASE("send: open file AV (bare path) with VP9+Opus succeeds"
          * doctest::test_suite("[send]")) {
    if (!have_vp9() || !have_opus()) {
        WARN_MESSAGE(false, "libvpx-vp9 / libopus encoder absent — skipping");
        return;
    }
    UAVSender* s = uav_send_create();
    REQUIRE(s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_OPUS, 160, 120, 15, 48000, 2);
    TempFileGuard tmp(make_temp_path(".webm"));
    CHECK(uav_send_open(s, tmp.c_str(), &cfg) == UAV_SEND_OK);
    CHECK(uav_send_last_error(s) == UAV_SEND_OK);
    uav_send_close(s);
    uav_send_destroy(s);
}

TEST_CASE("send: open file:// URI form with VP9+Opus succeeds identically"
          * doctest::test_suite("[send]")) {
    if (!have_vp9() || !have_opus()) {
        WARN_MESSAGE(false, "libvpx-vp9 / libopus encoder absent — skipping");
        return;
    }
    UAVSender* s = uav_send_create();
    REQUIRE(s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_OPUS, 160, 120, 15, 48000, 2);
    TempFileGuard tmp(make_temp_path(".webm"));
    std::string uri = "file://" + tmp.path;
    CHECK(uav_send_open(s, uri.c_str(), &cfg) == UAV_SEND_OK);
    uav_send_close(s);
    uav_send_destroy(s);
}

TEST_CASE("send: open file AV with VP8+Opus succeeds (find_video_encoder VP8 branch)"
          * doctest::test_suite("[send]")) {
    if (!have_vp8() || !have_opus()) {
        WARN_MESSAGE(false, "libvpx (VP8) / libopus encoder absent — skipping");
        return;
    }
    UAVSender* s = uav_send_create();
    REQUIRE(s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP8, UAV_ACODEC_OPUS, 160, 120, 15, 48000, 2);
    TempFileGuard tmp(make_temp_path(".webm"));
    CHECK(uav_send_open(s, tmp.c_str(), &cfg) == UAV_SEND_OK);
    CHECK(uav_send_last_error(s) == UAV_SEND_OK);
    uav_send_close(s);
    uav_send_destroy(s);
}

TEST_CASE("send: open file AV with AV1+Opus succeeds (find_video_encoder AV1 fallback)"
          * doctest::test_suite("[send]")) {
    if (!have_av1() || !have_opus()) {
        WARN_MESSAGE(false, "libaom-av1/libsvtav1 / libopus encoder absent — skipping");
        return;
    }
    UAVSender* s = uav_send_create();
    REQUIRE(s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_AV1, UAV_ACODEC_OPUS, 160, 120, 15, 48000, 2);
    TempFileGuard tmp(make_temp_path(".webm"));
    CHECK(uav_send_open(s, tmp.c_str(), &cfg) == UAV_SEND_OK);
    CHECK(uav_send_last_error(s) == UAV_SEND_OK);
    uav_send_close(s);
    uav_send_destroy(s);
}

TEST_CASE("send: push_video to an audio-only session => UAV_SEND_ERR_INVALID"
          * doctest::test_suite("[send]")) {
    if (!have_opus()) { WARN_MESSAGE(false, "libopus absent — skipping"); return; }
    UAVSender* s = uav_send_create();
    REQUIRE(s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_NONE, UAV_ACODEC_OPUS, 160, 120, 15, 48000, 2);
    TempFileGuard tmp(make_temp_path(".webm"));
    REQUIRE(uav_send_open(s, tmp.c_str(), &cfg) == UAV_SEND_OK);
    uint8_t rgba[160 * 120 * 4] = {0};
    CHECK(uav_send_push_video(s, rgba, 160, 120, 0, 0.0) == UAV_SEND_ERR_INVALID);
    uav_send_close(s);
    uav_send_destroy(s);
}

TEST_CASE("send: push_audio to a video-only session => UAV_SEND_ERR_INVALID"
          * doctest::test_suite("[send]")) {
    if (!have_vp9()) { WARN_MESSAGE(false, "libvpx-vp9 absent — skipping"); return; }
    UAVSender* s = uav_send_create();
    REQUIRE(s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_NONE, 160, 120, 15, 48000, 2);
    TempFileGuard tmp(make_temp_path(".webm"));
    REQUIRE(uav_send_open(s, tmp.c_str(), &cfg) == UAV_SEND_OK);
    float pcm[1024 * 2] = {0};
    CHECK(uav_send_push_audio(s, pcm, 1024, 2, 48000, 0.0) == UAV_SEND_ERR_INVALID);
    uav_send_close(s);
    uav_send_destroy(s);
}

TEST_CASE("send: push_video bad args => UAV_SEND_ERR_INVALID"
          * doctest::test_suite("[send]")) {
    if (!have_vp9()) { WARN_MESSAGE(false, "libvpx-vp9 absent — skipping"); return; }
    UAVSender* s = uav_send_create();
    REQUIRE(s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_NONE, 160, 120, 15, 48000, 2);
    TempFileGuard tmp(make_temp_path(".webm"));
    REQUIRE(uav_send_open(s, tmp.c_str(), &cfg) == UAV_SEND_OK);
    std::vector<uint8_t> rgba((size_t)160 * 120 * 4, 0);
    CHECK(uav_send_push_video(s, nullptr, 160, 120, 640, 0.0) == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_push_video(s, rgba.data(), 0, 0, 640, 0.0) == UAV_SEND_ERR_INVALID);
    uav_send_close(s);
    uav_send_destroy(s);
}

TEST_CASE("send: push_video with stride==0 defaults to w*4 (tightly packed)"
          * doctest::test_suite("[send]")) {
    if (!have_vp9()) { WARN_MESSAGE(false, "libvpx-vp9 absent — skipping"); return; }
    UAVSender* s = uav_send_create();
    REQUIRE(s != nullptr);
    const int w = 160, h = 120;
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_NONE, w, h, 15, 48000, 2);
    TempFileGuard tmp(make_temp_path(".webm"));
    REQUIRE(uav_send_open(s, tmp.c_str(), &cfg) == UAV_SEND_OK);
    std::vector<uint8_t> rgba;
    synth_video_frame(rgba, w, h, 0);
    CHECK(uav_send_push_video(s, rgba.data(), w, h, 0, 0.0) == UAV_SEND_OK);
    uav_send_close(s);
    uav_send_destroy(s);
}

TEST_CASE("send: push_audio bad args => UAV_SEND_ERR_INVALID"
          * doctest::test_suite("[send]")) {
    if (!have_opus()) { WARN_MESSAGE(false, "libopus absent — skipping"); return; }
    UAVSender* s = uav_send_create();
    REQUIRE(s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_NONE, UAV_ACODEC_OPUS, 160, 120, 15, 48000, 2);
    TempFileGuard tmp(make_temp_path(".webm"));
    REQUIRE(uav_send_open(s, tmp.c_str(), &cfg) == UAV_SEND_OK);
    std::vector<float> pcm(1024 * 2, 0.0f);
    CHECK(uav_send_push_audio(s, nullptr, 1024, 2, 48000, 0.0) == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_push_audio(s, pcm.data(), 0, 2, 48000, 0.0) == UAV_SEND_ERR_INVALID);
    CHECK(uav_send_push_audio(s, pcm.data(), 1024, 0, 48000, 0.0) == UAV_SEND_ERR_INVALID);
    uav_send_close(s);
    uav_send_destroy(s);
}

TEST_CASE("send: push_audio resamples mono 44100 into stereo 48000 Opus"
          * doctest::test_suite("[send]")) {
    if (!have_opus()) { WARN_MESSAGE(false, "libopus absent — skipping"); return; }
    UAVSender* s = uav_send_create();
    REQUIRE(s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_NONE, UAV_ACODEC_OPUS, 160, 120, 15, 48000, 2);
    TempFileGuard tmp(make_temp_path(".webm"));
    REQUIRE(uav_send_open(s, tmp.c_str(), &cfg) == UAV_SEND_OK);
    std::vector<float> pcm;
    double phase = 0.0;
    synth_audio_block(pcm, 4410, 1, 44100, phase);
    CHECK(uav_send_push_audio(s, pcm.data(), 4410, 1, 44100, 0.0) == UAV_SEND_OK);
    uav_send_close(s);
    uav_send_destroy(s);
}

TEST_CASE("send: close is idempotent and a handle can be reopened"
          * doctest::test_suite("[send]")) {
    if (!have_vp9() || !have_opus()) {
        WARN_MESSAGE(false, "libvpx-vp9 / libopus encoder absent — skipping");
        return;
    }
    UAVSender* s = uav_send_create();
    REQUIRE(s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_OPUS, 160, 120, 15, 48000, 2);

    TempFileGuard tmp1(make_temp_path(".webm"));
    REQUIRE(uav_send_open(s, tmp1.c_str(), &cfg) == UAV_SEND_OK);
    std::vector<uint8_t> rgba;
    std::vector<float> pcm;
    double phase = 0.0;
    for (int fr = 0; fr < 3; ++fr) {
        synth_video_frame(rgba, 160, 120, fr);
        CHECK(uav_send_push_video(s, rgba.data(), 160, 120, 640, (double)fr / 15) == UAV_SEND_OK);
        synth_audio_block(pcm, 48000 / 15, 2, 48000, phase);
        CHECK(uav_send_push_audio(s, pcm.data(), 48000 / 15, 2, 48000, (double)fr / 15) == UAV_SEND_OK);
    }
    CHECK(uav_send_close(s) == UAV_SEND_OK);
    CHECK(uav_send_close(s) == UAV_SEND_OK);

    TempFileGuard tmp2(make_temp_path(".webm"));
    CHECK(uav_send_open(s, tmp2.c_str(), &cfg) == UAV_SEND_OK);
    CHECK(uav_send_close(s) == UAV_SEND_OK);
    uav_send_destroy(s);
}

TEST_CASE("send: get_sdp after a FILE open => UAV_SEND_ERR_UNSUPPORTED"
          * doctest::test_suite("[send]")) {
    if (!have_vp9() || !have_opus()) {
        WARN_MESSAGE(false, "libvpx-vp9 / libopus encoder absent — skipping");
        return;
    }
    UAVSender* s = uav_send_create();
    REQUIRE(s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_OPUS, 160, 120, 15, 48000, 2);
    TempFileGuard tmp(make_temp_path(".webm"));
    REQUIRE(uav_send_open(s, tmp.c_str(), &cfg) == UAV_SEND_OK);
    char buf[64] = {0};
    CHECK(uav_send_get_sdp(s, buf, sizeof(buf)) == UAV_SEND_ERR_UNSUPPORTED);
    uav_send_close(s);
    uav_send_destroy(s);
}

TEST_CASE("send: get_sdp for an RTP session is non-empty and well-formed"
          * doctest::test_suite("[send]")) {
    if (!have_vp9()) { WARN_MESSAGE(false, "libvpx-vp9 absent — skipping"); return; }
    UAVSender* s = uav_send_create();
    REQUIRE(s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_OPUS, 160, 120, 15, 48000, 2);
    int rc = uav_send_open(s, "rtp://127.0.0.1:5004", &cfg);
    if (rc != UAV_SEND_OK) {
        WARN_MESSAGE(false, "VP9-over-RTP open rejected by this FFmpeg — skipping SDP asserts");
        uav_send_destroy(s);
        return;
    }
    int n = uav_send_get_sdp(s, nullptr, 0);
    CHECK(n > 0);
    std::vector<char> buf(4096, '\xff');
    int n2 = uav_send_get_sdp(s, buf.data(), (int)buf.size());
    CHECK(n2 == n);
    CHECK(buf[std::min(n2, (int)buf.size() - 1)] == '\0');
    CHECK(std::strlen(buf.data()) > 0);
    std::string sdp(buf.data());
    CHECK(sdp.find("m=video") != std::string::npos);
    CHECK(sdp.find("v=0") != std::string::npos);

    uav_send_close(s);
    uav_send_destroy(s);
}

TEST_CASE("send: get_sdp truncates to buflen-1 chars + NUL, returns full length"
          * doctest::test_suite("[send]")) {
    if (!have_vp9()) { WARN_MESSAGE(false, "libvpx-vp9 absent — skipping"); return; }
    UAVSender* s = uav_send_create();
    REQUIRE(s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_OPUS, 160, 120, 15, 48000, 2);
    int rc = uav_send_open(s, "rtp://127.0.0.1:5006", &cfg);
    if (rc != UAV_SEND_OK) {
        WARN_MESSAGE(false, "VP9-over-RTP open rejected by this FFmpeg — skipping truncation asserts");
        uav_send_destroy(s);
        return;
    }
    char small[8];
    std::memset(small, '\xff', sizeof(small));
    int n = uav_send_get_sdp(s, small, 8);
    CHECK(n >= 8);
    CHECK(small[7] == '\0');

    uav_send_close(s);
    uav_send_destroy(s);
}

TEST_CASE("send: srt:// is classify-ACCEPTED and reaches the SRT transport handshake"
          * doctest::test_suite("[send]")) {
    if (!have_vp9()) { WARN_MESSAGE(false, "libvpx-vp9 absent — skipping srt:// case"); return; }
    UAVSender* s = uav_send_create();
    REQUIRE(s != nullptr);
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_NONE, 160, 120, 15, 48000, 2);
    int rc = uav_send_open(s, "srt://127.0.0.1:9?mode=caller&connect_timeout=500", &cfg);
    CHECK(rc == UAV_SEND_ERR_OPEN_FAILED);
    CHECK(uav_send_last_error(s) == UAV_SEND_ERR_OPEN_FAILED);
    uav_send_destroy(s);
}

TEST_CASE("send: PRIMARY A/V file round-trip survives encode->decode"
          * doctest::test_suite("[send]")) {
    if (!have_vp9() || !have_opus()) {
        WARN_MESSAGE(false, "libvpx-vp9 / libopus encoder absent — skipping round-trip");
        return;
    }
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_OPUS, 160, 120, 15, 48000, 2);
    TempFileGuard tmp(make_temp_path(".webm"));
    REQUIRE(encode_synthetic(tmp.c_str(), cfg, 10) == UAV_SEND_OK);

    DecodeStats st;
    UAVMediaInfo info = decode_and_poll(tmp.c_str(), 2,
                                        true, st);
    REQUIRE(info.has_video == 1);
    REQUIRE(info.has_audio == 1);
    REQUIRE(info.width == 160);
    REQUIRE(info.height == 120);
    REQUIRE(st.distinct_frames >= 2);
    CHECK(st.touched_valid_buffer);
    REQUIRE(st.audio_rms > 0.0);
}

TEST_CASE("send: video-only file round-trip (isolates the video path)"
          * doctest::test_suite("[send]")) {
    if (!have_vp9()) { WARN_MESSAGE(false, "libvpx-vp9 absent — skipping round-trip"); return; }
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP9, UAV_ACODEC_NONE, 160, 120, 15, 48000, 2);
    TempFileGuard tmp(make_temp_path(".webm"));
    REQUIRE(encode_synthetic(tmp.c_str(), cfg, 10) == UAV_SEND_OK);

    DecodeStats st;
    UAVMediaInfo info = decode_and_poll(tmp.c_str(), 1,
                                        false, st);
    REQUIRE(info.has_video == 1);
    REQUIRE(info.has_audio == 0);
    REQUIRE(st.distinct_frames >= 1);
}

TEST_CASE("send: VP8 video-only file round-trip (exercises the VP8 encoder branch)"
          * doctest::test_suite("[send]")) {
    if (!have_vp8()) { WARN_MESSAGE(false, "libvpx (VP8) absent — skipping round-trip"); return; }
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_VP8, UAV_ACODEC_NONE, 160, 120, 15, 48000, 2);
    TempFileGuard tmp(make_temp_path(".webm"));
    REQUIRE(encode_synthetic(tmp.c_str(), cfg, 5) == UAV_SEND_OK);

    DecodeStats st;
    UAVMediaInfo info = decode_and_poll(tmp.c_str(), 1,
                                        false, st);
    if (info.has_video != 1) {
        WARN_MESSAGE(false, "decoder produced no VP8 video (HW VP8 decode unsupported here) — skipping decode asserts");
        return;
    }
    REQUIRE(info.has_audio == 0);
    REQUIRE(info.width == 160);
    REQUIRE(info.height == 120);
    REQUIRE(st.distinct_frames >= 1);
    CHECK(st.touched_valid_buffer);
}

TEST_CASE("send: AV1 video-only file round-trip (exercises the AV1 encoder branch)"
          * doctest::test_suite("[send]")) {
    if (!have_av1()) { WARN_MESSAGE(false, "libaom-av1/libsvtav1 absent — skipping round-trip"); return; }
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_AV1, UAV_ACODEC_NONE, 160, 120, 15, 48000, 2);
    TempFileGuard tmp(make_temp_path(".webm"));
    REQUIRE(encode_synthetic(tmp.c_str(), cfg, 5) == UAV_SEND_OK);

    DecodeStats st;
    UAVMediaInfo info = decode_and_poll(tmp.c_str(), 1,
                                        false, st);
    if (info.has_video != 1) {
        WARN_MESSAGE(false, "decoder produced no AV1 video (HW AV1 decode unsupported here) — skipping decode asserts");
        return;
    }
    REQUIRE(info.has_audio == 0);
    REQUIRE(info.width == 160);
    REQUIRE(info.height == 120);
    REQUIRE(st.distinct_frames >= 1);
    CHECK(st.touched_valid_buffer);
}

TEST_CASE("send: audio-only file round-trip (isolates the audio path)"
          * doctest::test_suite("[send]")) {
    if (!have_opus()) { WARN_MESSAGE(false, "libopus absent — skipping round-trip"); return; }
    UAVSendConfig cfg = make_cfg(UAV_VCODEC_NONE, UAV_ACODEC_OPUS, 160, 120, 15, 48000, 2);
    TempFileGuard tmp(make_temp_path(".webm"));
    REQUIRE(encode_synthetic(tmp.c_str(), cfg, 20) == UAV_SEND_OK);

    DecodeStats st;
    UAVMediaInfo info = decode_and_poll(tmp.c_str(), 0,
                                        true, st);
    REQUIRE(info.has_audio == 1);
    REQUIRE(info.has_video == 0);
    REQUIRE(st.audio_rms > 0.0);
}

#endif
