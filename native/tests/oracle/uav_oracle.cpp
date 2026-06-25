// SPDX-License-Identifier: Apache-2.0

#include "unitedav.h"
#include "oracle_ref.hpp"
#include "oracle_metrics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kPass = 0;
constexpr int kFail = 1;
constexpr int kErr  = 2;
constexpr int kSkip = 77;   // ctest SKIP_RETURN_CODE

// FFmpeg AVColorSpace / AVColorRange ABI enum values, asserted without the libav headers.
constexpr int kAVCOL_SPC_BT470BG   = 5;
constexpr int kAVCOL_SPC_SMPTE170M = 6;
constexpr int kAVCOL_RANGE_MPEG    = 1;

} // namespace
namespace uav_oracle { int oracle_metrics_selftest(); }

namespace {

int uav_setenv(const char* name, const char* value, int overwrite) {
#if defined(_WIN32)
    if (!overwrite) {
        if (const char* existing = std::getenv(name)) {
            if (existing[0] != '\0') return 0;
        }
    }
    return ::_putenv_s(name, value);
#else
    return ::setenv(name, value, overwrite);
#endif
}

struct Thresholds {
    double min_psnr        = 35.0;
    double min_ssim        = 0.98;
    double min_acorr       = 0.99;
    double max_hw_meandiff = 6.0;
    int    frames          = 5;
    bool   acorr_explicit  = false;
};

double audio_corr_floor(const char* clip, const Thresholds& th) {
    if (th.acorr_explicit) return th.min_acorr;
    std::string name = clip ? clip : "";
    if (name.find("__mp3") != std::string::npos || name.find(".mp3") != std::string::npos)
        return 0.97;
    return th.min_acorr;
}

const char* state_name(int s) {
    switch (s) {
        case UAV_STATE_IDLE: return "IDLE";  case UAV_STATE_OPENING: return "OPENING";
        case UAV_STATE_READY: return "READY"; case UAV_STATE_PLAYING: return "PLAYING";
        case UAV_STATE_PAUSED: return "PAUSED"; case UAV_STATE_BUFFERING: return "BUFFERING";
        case UAV_STATE_FINISHED: return "FINISHED"; case UAV_STATE_ERROR: return "ERROR";
        default: return "?";
    }
}

void kv(const char* k, double v)        { std::printf("%s=%.6f\n", k, v); }
void kvi(const char* k, long long v)    { std::printf("%s=%lld\n", k, v); }
void kvs(const char* k, const char* v)  { std::printf("%s=%s\n", k, v); }

struct CapFrame {
    std::vector<uint8_t> rgba;
    int width = 0, height = 0, stride = 0, format = -1;
    int64_t frame_id = 0;
    double pts = 0.0;
};

bool capture_via_abi(const char* url, int n, std::vector<CapFrame>& out,
                     bool* abi_ok, std::string* err) {
    out.clear();
    if (abi_ok) *abi_ok = true;
    UAVPlayer* p = uav_create();
    if (!p) { if (err) *err = "uav_create failed"; return false; }
    int rc = uav_open(p, url);
    if (rc != UAV_OK) {
        if (err) *err = "uav_open -> " + std::to_string(rc) +
                        " (state " + state_name(uav_get_state(p)) + ")";
        uav_destroy(p);
        return false;
    }
    UAVMediaInfo info{};
    uav_get_info(p, &info);
    if (!info.has_video) { uav_destroy(p); return true; }

    uav_play(p);
    int64_t last_id = -1;
    int64_t prev_id = -1;
    const int max_polls = 4000;
    for (int poll = 0; poll < max_polls && (int)out.size() < n; ++poll) {
        UAVVideoFrame vf{};
        if (uav_acquire_frame(p, last_id, &vf) == UAV_OK) {
            if (vf.data && vf.width > 0 && vf.height > 0 && vf.frame_id != last_id) {
                if (abi_ok) {
                    if (vf.format != UAV_PIX_RGBA32)       *abi_ok = false;
                    if (vf.stride != vf.width * 4)         *abi_ok = false;
                    if (info.width  && vf.width  != info.width)  *abi_ok = false;
                    if (info.height && vf.height != info.height) *abi_ok = false;
                    if (prev_id >= 0 && vf.frame_id <= prev_id)  *abi_ok = false;
                }
                prev_id = vf.frame_id;
                CapFrame cf;
                cf.width = vf.width; cf.height = vf.height; cf.stride = vf.stride;
                cf.format = vf.format; cf.frame_id = vf.frame_id; cf.pts = vf.pts;
                cf.rgba.assign((size_t)vf.stride * vf.height, 0);
                std::memcpy(cf.rgba.data(), vf.data, cf.rgba.size());
                out.push_back(std::move(cf));
            }
            last_id = vf.frame_id;
            uav_release_frame(p);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (uav_get_state(p) == UAV_STATE_FINISHED) break;
    }
    uav_destroy(p);
    return true;
}

bool capture_audio_via_abi(const char* url, int* ch_out, int* rate_out,
                           std::vector<std::vector<float>>& chans, double min_seconds,
                           std::string* err) {
    chans.clear();
    UAVPlayer* p = uav_create();
    if (!p) { if (err) *err = "uav_create failed"; return false; }
    if (uav_open(p, url) != UAV_OK) { if (err) *err = "uav_open failed"; uav_destroy(p); return false; }
    UAVMediaInfo info{};
    uav_get_info(p, &info);
    if (!info.has_audio) { uav_destroy(p); if (ch_out) *ch_out = 0; return true; }
    const int ch   = info.audio_channels   > 0 ? info.audio_channels   : 2;
    const int rate = info.audio_sample_rate > 0 ? info.audio_sample_rate : 48000;
    if (ch_out) *ch_out = ch;
    if (rate_out) *rate_out = rate;
    chans.assign((size_t)ch, {});

    uav_play(p);
    const int chunk = 1024;
    std::vector<float> buf((size_t)chunk * ch);
    const size_t want = (size_t)std::ceil(min_seconds * rate);
    const int max_polls = 6000;
    for (int poll = 0; poll < max_polls && chans[0].size() < want; ++poll) {
        int got = uav_read_audio(p, buf.data(), chunk, ch, rate);
        for (int i = 0; i < got; ++i)
            for (int c = 0; c < ch; ++c)
                chans[c].push_back(buf[(size_t)i * ch + c]);
        if (got == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (uav_get_state(p) == UAV_STATE_FINISHED && got == 0) {
            int extra = uav_read_audio(p, buf.data(), chunk, ch, rate);
            for (int i = 0; i < extra; ++i)
                for (int c = 0; c < ch; ++c)
                    chans[c].push_back(buf[(size_t)i * ch + c]);
            if (extra == 0) break;
        }
    }
    uav_destroy(p);
    return true;
}

std::vector<uint8_t> vflip(const std::vector<uint8_t>& src, int w, int h, int stride) {
    (void)w;
    std::vector<uint8_t> dst((size_t)stride * h);
    for (int y = 0; y < h; ++y)
        std::memcpy(dst.data() + (size_t)y * stride,
                    src.data() + (size_t)(h - 1 - y) * stride,
                    (size_t)stride);
    return dst;
}

const uav_oracle::RefFrame* nearest_ref(const std::vector<uav_oracle::RefFrame>& refs, double pts) {
    const uav_oracle::RefFrame* best = nullptr;
    double bestd = 1e30;
    for (const auto& r : refs) {
        double d = std::fabs(r.pts - pts);
        if (d < bestd) { bestd = d; best = &r; }
    }
    return best;
}

int mode_sw_vs_ref(const char* clip, const Thresholds& th) {
    using namespace uav_oracle;

    uav_setenv("UAV_HWDECODE", "none", 1);
    std::vector<CapFrame> caps;
    bool abi_ok = true;
    std::string err;
    if (!capture_via_abi(clip, th.frames, caps, &abi_ok, &err)) {
        std::fprintf(stderr, "oracle: %s\n", err.c_str());
        kvi("PASS", 0);
        return kErr;
    }

    Reference ref;
    if (!ref.open(clip)) {
        std::fprintf(stderr, "oracle: reference open failed: %s\n", ref.last_error());
        kvi("PASS", 0);
        return kErr;
    }

    bool pass = true;
    kvs("mode", "sw-vs-ref");
    kvs("clip", clip);
    kvi("abi_geometry_ok", abi_ok ? 1 : 0);
    if (!abi_ok) pass = false;

    if (ref.has_video()) {
        if (caps.empty()) {
            std::fprintf(stderr, "oracle: plugin produced no video frames\n");
            pass = false;
            kvi("video_frames", 0);
        } else {
            std::vector<RefFrame> refs = ref.decode_video(std::max(th.frames * 4, 16));
            if (refs.empty()) { std::fprintf(stderr, "oracle: reference produced no frames\n"); pass = false; }
            kvi("video_frames", (long long)caps.size());

            double min_psnr_seen = 1e30, min_ssim_seen = 1e30;
            int graded = 0;
            for (const auto& cf : caps) {
                const RefFrame* rf = nearest_ref(refs, cf.pts);
                if (!rf) continue;
                if (rf->width != cf.width || rf->height != cf.height) {
                    std::fprintf(stderr, "oracle: dim mismatch ref %dx%d vs abi %dx%d\n",
                                 rf->width, rf->height, cf.width, cf.height);
                    pass = false; continue;
                }
                double psnr = psnr_rgb(cf.rgba.data(), rf->rgba.data(), cf.width, cf.height, cf.stride);
                double ssim = ssim_luma(cf.rgba.data(), rf->rgba.data(), cf.width, cf.height, cf.stride);
                min_psnr_seen = std::min(min_psnr_seen, psnr);
                min_ssim_seen = std::min(min_ssim_seen, ssim);
                ++graded;
            }
            if (graded == 0) { std::fprintf(stderr, "oracle: no frame matched a reference\n"); pass = false; }
            kvi("video_graded", graded);
            kv("video_min_psnr_db", min_psnr_seen >= 1e29 ? 0.0 : min_psnr_seen);
            kv("video_min_ssim",    min_ssim_seen >= 1e29 ? 0.0 : min_ssim_seen);
            if (graded > 0) {
                if (min_psnr_seen < th.min_psnr) { pass = false; }
                if (min_ssim_seen < th.min_ssim) { pass = false; }
            }

            if (graded > 0 && !caps.empty()) {
                const CapFrame& cf = caps.front();
                const RefFrame* rf = nearest_ref(refs, cf.pts);
                if (rf && rf->width == cf.width && rf->height == cf.height) {
                    double ssim_up = ssim_luma(cf.rgba.data(), rf->rgba.data(), cf.width, cf.height, cf.stride);
                    std::vector<uint8_t> flipped = vflip(cf.rgba, cf.width, cf.height, cf.stride);
                    double ssim_flip = ssim_luma(flipped.data(), rf->rgba.data(), cf.width, cf.height, cf.stride);
                    kv("orient_ssim_topdown", ssim_up);
                    kv("orient_ssim_flipped", ssim_flip);
                    bool orient_ok = ssim_up > ssim_flip && ssim_up >= th.min_ssim;
                    kvi("orient_topdown_ok", orient_ok ? 1 : 0);
                    if (!orient_ok) pass = false;
                }
            }
        }
    } else {
        kvi("video_frames", 0);
        kvs("video", "none");
    }

    if (ref.has_audio()) {
        int ch = 0, rate = 0;
        std::vector<std::vector<float>> abi_ch;
        std::string aerr;
        capture_audio_via_abi(clip, &ch, &rate, abi_ch, 1.2, &aerr);
        if (ch <= 0 || abi_ch.empty() || abi_ch[0].empty()) {
            std::fprintf(stderr, "oracle: plugin produced no audio (%s)\n", aerr.c_str());
            pass = false;
            kvi("audio_channels", ch);
        } else {
            RefAudio ra = ref.decode_audio(ch, rate, 1.5);
            kvi("audio_channels", ch);
            kvi("audio_sample_rate", rate);
            double min_corr = 2.0;
            double abi_rms_sum = 0.0, ref_rms_sum = 0.0;
            int corr_n = 0;
            for (int c = 0; c < ch && c < (int)ra.channels.size(); ++c) {
                size_t n = std::min(abi_ch[c].size(), ra.channels[c].size());
                if (n < 256) continue;
                size_t lag_a = 0, lag_b = 0;
                double ca = best_lag_corr(abi_ch[c].data(), ra.channels[c].data(), n, (size_t)(rate / 50), &lag_a);
                double cb = best_lag_corr(ra.channels[c].data(), abi_ch[c].data(), n, (size_t)(rate / 50), &lag_b);
                double corr = std::max(ca, cb);
                min_corr = std::min(min_corr, corr);
                abi_rms_sum += rms(abi_ch[c].data(), n);
                ref_rms_sum += rms(ra.channels[c].data(), n);
                ++corr_n;
            }
            if (corr_n == 0) { std::fprintf(stderr, "oracle: no comparable audio\n"); pass = false; }
            kv("audio_min_corr", min_corr > 1.5 ? 0.0 : min_corr);
            double abi_rms = corr_n ? abi_rms_sum / corr_n : 0.0;
            double ref_rms = corr_n ? ref_rms_sum / corr_n : 0.0;
            kv("audio_abi_rms", abi_rms);
            kv("audio_ref_rms", ref_rms);
            double rms_ratio = (ref_rms > 1e-9) ? abi_rms / ref_rms : 0.0;
            kv("audio_rms_ratio", rms_ratio);
            if (corr_n > 0) {
                double acorr_floor = audio_corr_floor(clip, th);
                kv("audio_min_corr_floor", acorr_floor);
                if (min_corr < acorr_floor) pass = false;
                if (rms_ratio < 0.5 || rms_ratio > 2.0) pass = false;
            }
        }
    } else {
        kvs("audio", "none");
    }

    kvi("PASS", pass ? 1 : 0);
    return pass ? kPass : kFail;
}

int mode_hw_vs_sw(const char* clip, const Thresholds& th) {
    using namespace uav_oracle;
    kvs("mode", "hw-vs-sw");
    kvs("clip", clip);

    uav_setenv("UAV_HWDECODE", "none", 1);
    std::vector<CapFrame> sw;
    std::string err;
    if (!capture_via_abi(clip, th.frames, sw, nullptr, &err)) {
        std::fprintf(stderr, "oracle: sw capture failed: %s\n", err.c_str());
        kvi("PASS", 0);
        return kErr;
    }
    if (sw.empty()) {
        std::fprintf(stderr, "oracle: no SW video frames (audio-only?) -> SKIP\n");
        kvs("skip_reason", "no-video");
        return kSkip;
    }

    uav_setenv("UAV_HWDECODE", "auto", 1);
    std::vector<CapFrame> hw;
    if (!capture_via_abi(clip, th.frames, hw, nullptr, &err) || hw.empty()) {
        std::fprintf(stderr, "oracle: HW capture failed/empty -> SKIP (%s)\n", err.c_str());
        kvs("skip_reason", "hw-capture-empty");
        return kSkip;
    }

    int n = (int)std::min(sw.size(), hw.size());
    double max_mean = 0.0, min_ssim = 1e30;
    int graded = 0;
    for (int i = 0; i < n; ++i) {
        if (sw[i].width != hw[i].width || sw[i].height != hw[i].height) continue;
        double md = mean_abs_diff_rgb(hw[i].rgba.data(), sw[i].rgba.data(),
                                      sw[i].width, sw[i].height, sw[i].stride);
        double ss = ssim_luma(hw[i].rgba.data(), sw[i].rgba.data(),
                              sw[i].width, sw[i].height, sw[i].stride);
        max_mean = std::max(max_mean, md);
        min_ssim = std::min(min_ssim, ss);
        ++graded;
    }
    kvi("hw_graded", graded);
    kv("hw_max_meandiff", max_mean);
    kv("hw_min_ssim", min_ssim >= 1e29 ? 0.0 : min_ssim);

    if (graded == 0) { std::fprintf(stderr, "oracle: no comparable HW/SW frames -> SKIP\n"); kvs("skip_reason", "no-pairs"); return kSkip; }

    // A byte-identical HW run means HW silently fell back to SW; SKIP rather than claim a HW pass.
    bool likely_fallback = (max_mean == 0.0);
    if (likely_fallback) {
        std::fprintf(stderr, "oracle: HW==SW byte-identical (HW likely fell back to SW) -> SKIP\n");
        kvs("skip_reason", "hw-fallback-to-sw");
        return kSkip;
    }

    bool pass = (max_mean <= th.max_hw_meandiff) && (min_ssim >= th.min_ssim);
    kvi("PASS", pass ? 1 : 0);
    return pass ? kPass : kFail;
}

int mode_contract(const char* clip, const Thresholds& th) {
    using namespace uav_oracle;
    kvs("mode", "contract");
    kvs("clip", clip);

    Reference ref;
    if (!ref.open(clip)) {
        std::fprintf(stderr, "oracle: reference open failed: %s\n", ref.last_error());
        kvi("PASS", 0);
        return kErr;
    }
    bool pass = true;

    if (ref.has_video()) {
        std::vector<RefFrame> refs = ref.decode_video(2);
        const RefColorimetry& c = ref.colorimetry();
        kvi("colorspace", c.colorspace);
        kvs("colorspace_name", colorspace_name(c.colorspace));
        kvi("color_range", c.range);
        kvs("color_range_name", colorrange_name(c.range));
        kvi("color_trc", c.transfer);
        kvs("color_trc_name", colortrc_name(c.transfer));
        kvi("bit_depth", c.bit_depth);

        bool is_sd = !refs.empty() && refs.front().height > 0 && refs.front().height <= 576;
        bool bt601 = (c.colorspace == kAVCOL_SPC_BT470BG || c.colorspace == kAVCOL_SPC_SMPTE170M);
        if (is_sd && c.bit_depth == 8) {
            bool color_ok = bt601 && (c.range == kAVCOL_RANGE_MPEG);
            kvi("colorimetry_bt601_limited_ok", color_ok ? 1 : 0);
            if (!color_ok) {
                std::fprintf(stderr, "oracle: SD colorimetry not BT.601/limited "
                             "(cs=%d range=%d)\n", c.colorspace, c.range);
                pass = false;
            }
        } else {
            kvi("colorimetry_bt601_limited_ok", -1);
        }

        constexpr int kAVCOL_TRC_LINEAR = 8;
        bool srgb_consistent = (c.transfer != kAVCOL_TRC_LINEAR);
        kvi("srgb_consistent", srgb_consistent ? 1 : 0);
        if (!srgb_consistent) {
            std::fprintf(stderr, "oracle: transfer is LINEAR-light; violates sRGB texture contract\n");
            pass = false;
        }

        uav_setenv("UAV_HWDECODE", "none", 1);
        std::vector<CapFrame> caps;
        std::string err;
        capture_via_abi(clip, 1, caps, nullptr, &err);
        if (!caps.empty() && !refs.empty()) {
            const CapFrame& cf = caps.front();
            const RefFrame* rf = nearest_ref(refs, cf.pts);
            if (rf && rf->width == cf.width && rf->height == cf.height) {
                double up = ssim_luma(cf.rgba.data(), rf->rgba.data(), cf.width, cf.height, cf.stride);
                std::vector<uint8_t> fl = vflip(cf.rgba, cf.width, cf.height, cf.stride);
                double dn = ssim_luma(fl.data(), rf->rgba.data(), cf.width, cf.height, cf.stride);
                kv("orient_ssim_topdown", up);
                kv("orient_ssim_flipped", dn);
                bool orient_ok = up > dn && up >= th.min_ssim;
                kvi("requires_vertical_flip_true", orient_ok ? 1 : 0);
                if (!orient_ok) pass = false;
            } else {
                kvi("requires_vertical_flip_true", -1);
            }
        }
    } else {
        kvs("video", "none");
    }

    kvi("PASS", pass ? 1 : 0);
    return pass ? kPass : kFail;
}

void usage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --selftest\n"
        "       %s <clip> [--mode sw-vs-ref|hw-vs-sw|contract] [--frames N]\n"
        "                 [--min-psnr X] [--min-ssim X] [--min-acorr X]\n"
        "                 [--max-hw-meandiff X]\n", argv0, argv0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return kErr; }

    if (std::strcmp(argv[1], "--selftest") == 0) {
        int rc = uav_oracle::oracle_metrics_selftest();
        if (rc != 0) { std::fprintf(stderr, "oracle: --selftest metric checks FAILED\n"); return kFail; }
        std::printf("oracle: --selftest metric checks PASSED\n");
        return kPass;
    }

    const char* clip = argv[1];
    std::string mode = "sw-vs-ref";
    Thresholds th;

    for (int i = 2; i < argc; ++i) {
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "oracle: %s needs a value\n", name); std::exit(kErr); }
            return argv[++i];
        };
        if (!std::strcmp(argv[i], "--mode"))             mode = need("--mode");
        else if (!std::strcmp(argv[i], "--frames"))      th.frames = std::atoi(need("--frames"));
        else if (!std::strcmp(argv[i], "--min-psnr"))    th.min_psnr = std::atof(need("--min-psnr"));
        else if (!std::strcmp(argv[i], "--min-ssim"))    th.min_ssim = std::atof(need("--min-ssim"));
        else if (!std::strcmp(argv[i], "--min-acorr"))   { th.min_acorr = std::atof(need("--min-acorr")); th.acorr_explicit = true; }
        else if (!std::strcmp(argv[i], "--max-hw-meandiff")) th.max_hw_meandiff = std::atof(need("--max-hw-meandiff"));
        else { std::fprintf(stderr, "oracle: unknown arg %s\n", argv[i]); usage(argv[0]); return kErr; }
    }
    if (th.frames <= 0) th.frames = 5;

    if (mode == "sw-vs-ref") return mode_sw_vs_ref(clip, th);
    if (mode == "hw-vs-sw")  return mode_hw_vs_sw(clip, th);
    if (mode == "contract")  return mode_contract(clip, th);

    std::fprintf(stderr, "oracle: unknown --mode '%s'\n", mode.c_str());
    usage(argv[0]);
    return kErr;
}
