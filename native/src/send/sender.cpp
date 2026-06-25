// SPDX-License-Identifier: Apache-2.0
#include "sender.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(UAV_HAVE_FFMPEG)

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace uav {

namespace {

constexpr AVPixelFormat kEncPixFmt = AV_PIX_FMT_YUV420P;
constexpr int64_t kDefaultOpenTimeoutMs = 15000;

int64_t open_timeout_ms() {
    if (const char* e = std::getenv("UAV_OPEN_TIMEOUT_MS")) {
        char* end = nullptr;
        long v = std::strtol(e, &end, 10);
        if (end != e && v > 0) return (int64_t)v;
    }
    return kDefaultOpenTimeoutMs;
}

enum class Transport { File, Rtp, Srt };

bool classify(const std::string& url, Transport& t, const char*& muxer,
              std::string& out_url) {
    auto starts = [&](const char* p) {
        return url.compare(0, std::strlen(p), p) == 0;
    };
    if (starts("rtp://")) {
        t = Transport::Rtp;  muxer = "rtp";    out_url = url; return true;
    }
    if (starts("srt://")) {
        t = Transport::Srt;  muxer = "matroska"; out_url = url; return true;
    }
    if (starts("file://")) {
        t = Transport::File; muxer = nullptr;  out_url = url.substr(7); return true;
    }
    if (url.find("://") == std::string::npos) {
        t = Transport::File; muxer = nullptr;  out_url = url; return true;
    }
    return false;
}

const AVCodec* find_video_encoder(int codec) {
    switch (codec) {
        case UAV_VCODEC_VP9: return avcodec_find_encoder_by_name("libvpx-vp9");
        case UAV_VCODEC_VP8: return avcodec_find_encoder_by_name("libvpx");
        case UAV_VCODEC_AV1: {
            const AVCodec* c = avcodec_find_encoder_by_name("libaom-av1");
            if (!c) c = avcodec_find_encoder_by_name("libsvtav1");
            return c;
        }
        default: return nullptr;
    }
}

} // namespace

struct Sender::Impl {
    Transport transport = Transport::File;

    AVFormatContext* fmt = nullptr;
    bool header_written  = false;
    bool avio_opened     = false;   // we own fmt->pb and must avio_closep it

    AVStream*       vst   = nullptr;
    AVCodecContext* venc  = nullptr;
    SwsContext*     sws   = nullptr;
    int sws_srcw = 0, sws_srch = 0, sws_srcfmt = -1;
    AVFrame*        vframe = nullptr;
    int64_t         vframe_count = 0;
    int cfg_w = 0, cfg_h = 0, cfg_fps = 30;

    AVStream*       ast   = nullptr;
    AVCodecContext* aenc  = nullptr;
    SwrContext*     swr   = nullptr;
    int swr_in_rate = 0, swr_in_ch = 0;
    AVFrame*        aframe = nullptr;
    int             aframe_fill = 0;
    int64_t         asample_count = 0;
    std::vector<float> aresample_buf;

    AVPacket*       pkt = nullptr;

    int64_t io_budget_ms = kDefaultOpenTimeoutMs;
    std::atomic<int64_t> io_deadline{0}; // steady_clock ns; 0 = disarmed

    void arm_io_deadline() {
        io_deadline.store(
            (std::chrono::steady_clock::now() + std::chrono::milliseconds(io_budget_ms))
                .time_since_epoch().count(),
            std::memory_order_relaxed);
    }
    static int io_interrupt_cb(void* opaque) {
        auto* self = static_cast<Impl*>(opaque);
        if (!self) return 0;
        int64_t dl = self->io_deadline.load(std::memory_order_relaxed);
        if (dl != 0 &&
            std::chrono::steady_clock::now().time_since_epoch().count() >= dl)
            return 1;
        return 0;
    }

    bool has_video() const { return vst != nullptr; }
    bool has_audio() const { return ast != nullptr; }

    void teardown() {
        if (sws)    { sws_freeContext(sws); sws = nullptr; }
        if (swr)    { swr_free(&swr); }
        if (vframe) av_frame_free(&vframe);
        if (aframe) av_frame_free(&aframe);
        if (pkt)    av_packet_free(&pkt);
        if (venc)   avcodec_free_context(&venc);
        if (aenc)   avcodec_free_context(&aenc);
        if (fmt) {
            if (avio_opened && fmt->pb) avio_closep(&fmt->pb);
            avformat_free_context(fmt);
            fmt = nullptr;
        }
        vst = ast = nullptr;
        header_written = false;
        avio_opened = false;
    }
    ~Impl() { teardown(); }

    int open_video(const UAVSendConfig& cfg, const char* muxer);
    int open_audio(const UAVSendConfig& cfg);
    int write_encoded(AVCodecContext* enc, AVStream* st, AVFrame* frame);
    int encode_audio_block();
};

Sender::Sender() = default;
Sender::~Sender() { close(); }

int Sender::Impl::open_video(const UAVSendConfig& cfg, const char* /*muxer*/) {
    const AVCodec* enc = find_video_encoder(cfg.video_codec);
    if (!enc) return UAV_SEND_ERR_UNSUPPORTED;

    venc = avcodec_alloc_context3(enc);
    if (!venc) return UAV_SEND_ERR_NOMEM;

    cfg_w   = cfg.width  > 0 ? cfg.width  : 640;
    cfg_h   = cfg.height > 0 ? cfg.height : 480;
    cfg_fps = cfg.fps    > 0 ? cfg.fps    : 30;

    venc->width      = cfg_w;
    venc->height     = cfg_h;
    venc->pix_fmt    = kEncPixFmt;
    venc->time_base  = AVRational{1, cfg_fps};
    venc->framerate  = AVRational{cfg_fps, 1};
    // Short GOP (~3/sec) for live transports so late joiners decode quickly.
    venc->gop_size   = (transport == Transport::File) ? cfg_fps
                                                      : std::max(cfg_fps / 3, 1);
    venc->bit_rate   = cfg.video_bitrate > 0 ? cfg.video_bitrate : 1'500'000;
    venc->thread_count = 0;

    if (cfg.video_codec == UAV_VCODEC_VP9 || cfg.video_codec == UAV_VCODEC_VP8) {
        av_opt_set(venc->priv_data, "deadline", "realtime", 0);
        av_opt_set(venc->priv_data, "cpu-used", "8", 0);
        if (cfg.video_codec == UAV_VCODEC_VP9)
            av_opt_set(venc->priv_data, "row-mt", "1", 0);
    } else if (cfg.video_codec == UAV_VCODEC_AV1) {
        av_opt_set(venc->priv_data, "usage", "realtime", 0);
        av_opt_set(venc->priv_data, "cpu-used", "8", 0);
    }

    if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
        venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // VP9 RTP packetization is draft-spec; the RTP muxer needs experimental compliance.
    if (transport == Transport::Rtp)
        venc->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    if (avcodec_open2(venc, enc, nullptr) < 0) return UAV_SEND_ERR_OPEN_FAILED;

    vst = avformat_new_stream(fmt, nullptr);
    if (!vst) return UAV_SEND_ERR_NOMEM;
    vst->time_base = venc->time_base;
    if (avcodec_parameters_from_context(vst->codecpar, venc) < 0)
        return UAV_SEND_ERR_OPEN_FAILED;

    vframe = av_frame_alloc();
    if (!vframe) return UAV_SEND_ERR_NOMEM;
    vframe->format = kEncPixFmt;
    vframe->width  = cfg_w;
    vframe->height = cfg_h;
    if (av_frame_get_buffer(vframe, 0) < 0) return UAV_SEND_ERR_NOMEM;

    return UAV_SEND_OK;
}

int Sender::Impl::open_audio(const UAVSendConfig& cfg) {
    const AVCodec* enc = avcodec_find_encoder_by_name("libopus");
    if (!enc) enc = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    if (!enc) return UAV_SEND_ERR_UNSUPPORTED;

    aenc = avcodec_alloc_context3(enc);
    if (!aenc) return UAV_SEND_ERR_NOMEM;

    int rate = cfg.sample_rate > 0 ? cfg.sample_rate : 48000;
    int ch   = cfg.channels    > 0 ? cfg.channels    : 2;

    aenc->sample_rate = rate;
    av_channel_layout_default(&aenc->ch_layout, ch);
    aenc->sample_fmt = AV_SAMPLE_FMT_FLT;
    {
        const enum AVSampleFormat* sfmts = nullptr;
        if (avcodec_get_supported_config(aenc, enc, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                         0, (const void**)&sfmts, nullptr) >= 0 &&
            sfmts && sfmts[0] != AV_SAMPLE_FMT_NONE) {
            aenc->sample_fmt = sfmts[0];
        }
    }
    aenc->bit_rate    = cfg.audio_bitrate > 0 ? cfg.audio_bitrate : 96000;
    aenc->time_base   = AVRational{1, rate};

    if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
        aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(aenc, enc, nullptr) < 0) return UAV_SEND_ERR_OPEN_FAILED;

    ast = avformat_new_stream(fmt, nullptr);
    if (!ast) return UAV_SEND_ERR_NOMEM;
    ast->time_base = aenc->time_base;
    if (avcodec_parameters_from_context(ast->codecpar, aenc) < 0)
        return UAV_SEND_ERR_OPEN_FAILED;

    int frame_size = aenc->frame_size > 0 ? aenc->frame_size : rate / 50;

    aframe = av_frame_alloc();
    if (!aframe) return UAV_SEND_ERR_NOMEM;
    aframe->format      = aenc->sample_fmt;
    aframe->sample_rate = rate;
    av_channel_layout_copy(&aframe->ch_layout, &aenc->ch_layout);
    aframe->nb_samples  = frame_size;
    if (av_frame_get_buffer(aframe, 0) < 0) return UAV_SEND_ERR_NOMEM;
    aframe_fill = 0;

    return UAV_SEND_OK;
}

int32_t Sender::open(const std::string& url, const UAVSendConfig& cfg) {
    close();

    if (cfg.video_codec == UAV_VCODEC_NONE && cfg.audio_codec == UAV_ACODEC_NONE) {
        last_error_ = UAV_SEND_ERR_NO_STREAM;
        return last_error_;
    }

    d_ = std::make_unique<Impl>();
    Impl* d = d_.get();

    auto fail = [&](int code) {
        last_error_ = code;
        d_.reset();
        return code;
    };

    Transport t; const char* muxer = nullptr; std::string out_url;
    if (!classify(url, t, muxer, out_url)) return fail(UAV_SEND_ERR_OPEN_FAILED);
    d->transport = t;

    int rc = avformat_alloc_output_context2(&d->fmt, nullptr, muxer, out_url.c_str());
    if (rc < 0 || !d->fmt) return fail(UAV_SEND_ERR_OPEN_FAILED);

    d->io_budget_ms = open_timeout_ms();
    d->fmt->interrupt_callback.callback = &Impl::io_interrupt_cb;
    d->fmt->interrupt_callback.opaque   = d;

    if (t == Transport::Rtp)
        d->fmt->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    // The standard RTP muxer carries one media stream per session; keep video if
    // both A and V are requested. File/srt stream both.
    bool want_video = cfg.video_codec != UAV_VCODEC_NONE;
    bool want_audio = cfg.audio_codec != UAV_ACODEC_NONE;
    if (t == Transport::Rtp && want_video && want_audio) {
        want_audio = false;
    }

    if (want_video) { int r = d->open_video(cfg, muxer); if (r != UAV_SEND_OK) return fail(r); }
    if (want_audio) { int r = d->open_audio(cfg);        if (r != UAV_SEND_OK) return fail(r); }

    if (!d->has_video() && !d->has_audio()) return fail(UAV_SEND_ERR_NO_STREAM);

    d->pkt = av_packet_alloc();
    if (!d->pkt) return fail(UAV_SEND_ERR_NOMEM);

    // Open the byte stream unless the muxer manages its own (rtp: AVFMT_NOFILE).
    if (!(d->fmt->oformat->flags & AVFMT_NOFILE)) {
        d->arm_io_deadline();
        AVDictionary* avio_opts = nullptr;
        {
            const int64_t us = (int64_t)d->io_budget_ms * 1000;
            char usbuf[32];
            std::snprintf(usbuf, sizeof(usbuf), "%lld", (long long)us);
            av_dict_set(&avio_opts, "rw_timeout", usbuf, 0);
            av_dict_set(&avio_opts, "timeout",    usbuf, 0);
        }
        AVIOInterruptCB icb{ &Impl::io_interrupt_cb, d };
        int orc = avio_open2(&d->fmt->pb, out_url.c_str(), AVIO_FLAG_WRITE,
                             &icb, &avio_opts);
        av_dict_free(&avio_opts);
        if (orc < 0) return fail(UAV_SEND_ERR_OPEN_FAILED);
        d->avio_opened = true;
    }

    AVDictionary* mux_opts = nullptr;
    if (t == Transport::Srt) {
        av_dict_set(&mux_opts, "live", "1", 0);
        av_dict_set(&mux_opts, "cluster_time_limit", "200", 0);
    }
    d->arm_io_deadline();
    int wrc = avformat_write_header(d->fmt, &mux_opts);
    av_dict_free(&mux_opts);
    if (wrc < 0) return fail(UAV_SEND_ERR_OPEN_FAILED);
    d->header_written = true;

    last_error_ = UAV_SEND_OK;
    return UAV_SEND_OK;
}

int Sender::Impl::write_encoded(AVCodecContext* enc, AVStream* st, AVFrame* frame) {
    int rc = avcodec_send_frame(enc, frame);
    if (rc < 0) return UAV_SEND_ERR_ENCODE;
    while (true) {
        rc = avcodec_receive_packet(enc, pkt);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) return UAV_SEND_ERR_ENCODE;
        pkt->stream_index = st->index;
        av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
        arm_io_deadline();
        rc = av_interleaved_write_frame(fmt, pkt);
        av_packet_unref(pkt);
        if (rc < 0) return UAV_SEND_ERR_ENCODE;
    }
    return UAV_SEND_OK;
}

int32_t Sender::push_video(const uint8_t* rgba, int w, int h, int stride,
                           double pts_seconds) {
    Impl* d = d_.get();
    if (!d || !d->has_video() || !rgba || w <= 0 || h <= 0) {
        last_error_ = UAV_SEND_ERR_INVALID;
        return last_error_;
    }
    if (stride <= 0) stride = w * 4;

    if (!d->sws || d->sws_srcw != w || d->sws_srch != h ||
        d->sws_srcfmt != AV_PIX_FMT_RGBA) {
        if (d->sws) sws_freeContext(d->sws);
        d->sws = sws_getContext(w, h, AV_PIX_FMT_RGBA,
                                d->cfg_w, d->cfg_h, kEncPixFmt,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
        d->sws_srcw = w; d->sws_srch = h; d->sws_srcfmt = AV_PIX_FMT_RGBA;
        if (!d->sws) { last_error_ = UAV_SEND_ERR_ENCODE; return last_error_; }
    }

    if (av_frame_make_writable(d->vframe) < 0) {
        last_error_ = UAV_SEND_ERR_ENCODE; return last_error_;
    }

    const uint8_t* srcdata[4] = { rgba, nullptr, nullptr, nullptr };
    int            srcline[4] = { stride, 0, 0, 0 };
    sws_scale(d->sws, srcdata, srcline, 0, h, d->vframe->data, d->vframe->linesize);

    // Drive pts from a running frame counter; if the caller's pts implies a later
    // index, advance to it so a/v stay roughly aligned.
    int64_t idx = d->vframe_count;
    if (pts_seconds > 0.0) {
        int64_t want = (int64_t)std::llround(pts_seconds * d->cfg_fps);
        if (want > idx) idx = want;
    }
    d->vframe->pts = idx;
    d->vframe_count = idx + 1;

    int r = d->write_encoded(d->venc, d->vst, d->vframe);
    last_error_ = r;
    return r;
}

int Sender::Impl::encode_audio_block() {
    if (av_frame_make_writable(aframe) < 0) return UAV_SEND_ERR_ENCODE;
    aframe->pts = asample_count;
    asample_count += aframe->nb_samples;
    int r = write_encoded(aenc, ast, aframe);
    aframe_fill = 0;
    return r;
}

int32_t Sender::push_audio(const float* interleaved, int frames, int channels,
                           int sample_rate, double /*pts_seconds*/) {
    Impl* d = d_.get();
    if (!d || !d->has_audio() || !interleaved || frames <= 0 || channels <= 0) {
        last_error_ = UAV_SEND_ERR_INVALID;
        return last_error_;
    }

    if (!d->swr || d->swr_in_rate != sample_rate || d->swr_in_ch != channels) {
        if (d->swr) swr_free(&d->swr);
        AVChannelLayout in_layout;
        av_channel_layout_default(&in_layout, channels);
        int rc = swr_alloc_set_opts2(&d->swr,
            &d->aenc->ch_layout, d->aenc->sample_fmt, d->aenc->sample_rate,
            &in_layout, AV_SAMPLE_FMT_FLT, sample_rate,
            0, nullptr);
        av_channel_layout_uninit(&in_layout);
        if (rc < 0 || !d->swr || swr_init(d->swr) < 0) {
            if (d->swr) swr_free(&d->swr);
            last_error_ = UAV_SEND_ERR_ENCODE; return last_error_;
        }
        d->swr_in_rate = sample_rate;
        d->swr_in_ch   = channels;
    }

    const int out_ch = d->aenc->ch_layout.nb_channels;
    const bool out_planar = av_sample_fmt_is_planar(d->aenc->sample_fmt);

    int max_out = (int)av_rescale_rnd(
        swr_get_delay(d->swr, sample_rate) + frames,
        d->aenc->sample_rate, sample_rate, AV_ROUND_UP);
    if (max_out <= 0) { last_error_ = UAV_SEND_OK; return UAV_SEND_OK; }

    AVFrame* tmp = av_frame_alloc();
    if (!tmp) { last_error_ = UAV_SEND_ERR_NOMEM; return last_error_; }
    tmp->format      = d->aenc->sample_fmt;
    tmp->sample_rate = d->aenc->sample_rate;
    av_channel_layout_copy(&tmp->ch_layout, &d->aenc->ch_layout);
    tmp->nb_samples  = max_out;
    int converted = -1;
    if (av_frame_get_buffer(tmp, 0) >= 0) {
        const uint8_t* inp[1] = { reinterpret_cast<const uint8_t*>(interleaved) };
        converted = swr_convert(d->swr, tmp->data, max_out, inp, frames);
    }
    if (converted < 0) {
        av_frame_free(&tmp);
        last_error_ = UAV_SEND_ERR_ENCODE; return last_error_;
    }

    // Append converted samples into aframe, encoding whenever it fills.
    const int blk = d->aframe->nb_samples;
    const int bps = av_get_bytes_per_sample(d->aenc->sample_fmt);
    int src_off = 0;
    int r = UAV_SEND_OK;
    while (src_off < converted) {
        int take = std::min(blk - d->aframe_fill, converted - src_off);
        if (out_planar) {
            for (int c = 0; c < out_ch; ++c) {
                std::memcpy(d->aframe->data[c] + (size_t)d->aframe_fill * bps,
                            tmp->data[c] + (size_t)src_off * bps,
                            (size_t)take * bps);
            }
        } else {
            std::memcpy(d->aframe->data[0] + (size_t)d->aframe_fill * out_ch * bps,
                        tmp->data[0] + (size_t)src_off * out_ch * bps,
                        (size_t)take * out_ch * bps);
        }
        d->aframe_fill += take;
        src_off        += take;
        if (d->aframe_fill == blk) {
            r = d->encode_audio_block();
            if (r != UAV_SEND_OK) break;
        }
    }

    av_frame_free(&tmp);
    last_error_ = r;
    return r;
}

int32_t Sender::close() {
    Impl* d = d_.get();
    if (!d) { last_error_ = UAV_SEND_OK; return UAV_SEND_OK; }

    int result = UAV_SEND_OK;
    if (d->header_written) {
        // Pad a partial trailing audio block with silence to a full frame.
        if (d->has_audio() && d->aframe_fill > 0) {
            const int blk = d->aframe->nb_samples;
            const int bps = av_get_bytes_per_sample(d->aenc->sample_fmt);
            const int out_ch = d->aenc->ch_layout.nb_channels;
            const bool planar = av_sample_fmt_is_planar(d->aenc->sample_fmt);
            if (av_frame_make_writable(d->aframe) >= 0) {
                if (planar) {
                    for (int c = 0; c < out_ch; ++c)
                        std::memset(d->aframe->data[c] + (size_t)d->aframe_fill * bps, 0,
                                    (size_t)(blk - d->aframe_fill) * bps);
                } else {
                    std::memset(d->aframe->data[0] + (size_t)d->aframe_fill * out_ch * bps, 0,
                                (size_t)(blk - d->aframe_fill) * out_ch * bps);
                }
                d->aframe_fill = blk;
                d->encode_audio_block();
            }
        }
        if (d->has_video()) d->write_encoded(d->venc, d->vst, nullptr);
        if (d->has_audio()) d->write_encoded(d->aenc, d->ast, nullptr);
        if (av_write_trailer(d->fmt) < 0) result = UAV_SEND_ERR_ENCODE;
    }

    d_.reset();
    last_error_ = result;
    return result;
}

int32_t Sender::get_sdp(char* buf, int buflen) const {
    const Impl* d = d_.get();
    if (!d || !d->fmt) return UAV_SEND_ERR_INVALID;
    if (d->transport != Transport::Rtp) return UAV_SEND_ERR_UNSUPPORTED;

    char sdp[4096];
    AVFormatContext* ctxs[1] = { d->fmt };
    if (av_sdp_create(ctxs, 1, sdp, sizeof(sdp)) < 0)
        return UAV_SEND_ERR_OPEN_FAILED;
    int n = (int)std::strlen(sdp);
    if (buf && buflen > 0) {
        int copy = std::min(n, buflen - 1);
        std::memcpy(buf, sdp, (size_t)copy);
        buf[copy] = '\0';
    }
    return n;
}

} // namespace uav

#else // !UAV_HAVE_FFMPEG

namespace uav {

struct Sender::Impl {};

Sender::Sender() = default;
Sender::~Sender() = default;

int32_t Sender::open(const std::string&, const UAVSendConfig&) {
    last_error_ = UAV_SEND_ERR_UNSUPPORTED; return last_error_;
}
int32_t Sender::push_video(const uint8_t*, int, int, int, double) {
    last_error_ = UAV_SEND_ERR_UNSUPPORTED; return last_error_;
}
int32_t Sender::push_audio(const float*, int, int, int, double) {
    last_error_ = UAV_SEND_ERR_UNSUPPORTED; return last_error_;
}
int32_t Sender::close() { last_error_ = UAV_SEND_OK; return UAV_SEND_OK; }
int32_t Sender::get_sdp(char*, int) const { return UAV_SEND_ERR_UNSUPPORTED; }

} // namespace uav

#endif // UAV_HAVE_FFMPEG
