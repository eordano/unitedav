// SPDX-License-Identifier: Apache-2.0
#include "decoder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(UAV_HAVE_FFMPEG)

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include "../gpu/hwaccel.hpp"

namespace uav {

namespace {
constexpr int    kPixFmt          = AV_PIX_FMT_RGBA;
constexpr double kAudioBufSeconds = 2.0;
constexpr double kVideoLeadCap    = 0.05;
constexpr double kMaxStarveLead   = 0.15;  // see pace_video anti-starvation rule
constexpr int64_t kDefaultOpenTimeoutMs = 15000;
using clock_t_ = std::chrono::steady_clock;

int64_t open_timeout_ms() {
    if (const char* e = std::getenv("UAV_OPEN_TIMEOUT_MS")) {
        char* end = nullptr;
        long v = std::strtol(e, &end, 10);
        if (end != e && v > 0) return (int64_t)v;
    }
    return kDefaultOpenTimeoutMs;
}

double tb_to_sec(int64_t ts, AVRational tb) {
    if (ts == AV_NOPTS_VALUE) return 0.0;
    return static_cast<double>(ts) * av_q2d(tb);
}
} // namespace

struct Decoder::Impl {
    Decoder* owner = nullptr;

    AVFormatContext* fmt = nullptr;
    int video_stream = -1;
    int audio_stream = -1;

    AVCodecContext* vdec = nullptr;
    AVCodecContext* adec = nullptr;

    HwDecode hw;
    AVFrame* hw_sw_frame = nullptr;

    SwsContext* sws = nullptr;
    int sws_w = 0, sws_h = 0, sws_srcfmt = -1;

    // swr_* fields are mutated only under audio_mtx; swr_out_rate is also read
    // lock-free by master_clock(), hence atomic.
    SwrContext* swr = nullptr;
    int swr_out_ch = 0;
    std::atomic<int> swr_out_rate{0};
    int swr_in_rate = 0;
    int swr_in_fmt = -1;
    AVChannelLayout swr_in_layout{};

    int     mi_width = 0, mi_height = 0;
    double  mi_fps = 0.0;
    double  mi_duration = 0.0;
    int     mi_audio_ch = 0;
    int     mi_audio_rate = 0;

    // Three slots so the worker can always pick a write slot that is neither the
    // reader-locked one nor the last-published one, keeping a borrowed reader
    // pointer valid until release_frame(). All slot/pts/vbuf state under video_mtx.
    static constexpr int  kVideoSlots = 3;
    std::mutex            video_mtx;
    std::vector<uint8_t>  vbuf[kVideoSlots];
    int                   vbuf_w = 0, vbuf_h = 0, vbuf_stride = 0;
    int                   ready_slot = -1;
    int                   locked_slot = -1;
    std::atomic<int64_t>  latest_frame_id{0};
    double                latest_pts = 0.0;

    bool                  have_published_pts = false;
    double                last_published_pts = 0.0;

    std::mutex            audio_mtx;
    std::vector<float>    aring;
    size_t                aring_cap = 0;
    size_t                aread = 0;
    size_t                awrite = 0;
    size_t                afill = 0;
    std::atomic<int64_t>  audio_played_frames{0};
    std::atomic<double>   audio_clock_base{0.0};
    bool                  audio_anchored = false;

    std::thread           worker;
    std::mutex            ctrl_mtx;
    std::condition_variable ctrl_cv;
    std::atomic<bool>     quit{false};
    std::atomic<bool>     paused{true};
    std::atomic<bool>     eof{false};

    int64_t               io_budget_ms = kDefaultOpenTimeoutMs;
    std::atomic<int64_t>  io_deadline{0}; // steady_clock ns; 0 = disarmed

    void arm_io_deadline() {
        io_deadline.store((clock_t_::now() + std::chrono::milliseconds(io_budget_ms))
                              .time_since_epoch().count(),
                          std::memory_order_relaxed);
    }
    static int io_interrupt_cb(void* opaque) {
        auto* self = static_cast<Impl*>(opaque);
        if (!self) return 0;
        if (self->quit.load(std::memory_order_relaxed)) return 1;
        int64_t dl = self->io_deadline.load(std::memory_order_relaxed);
        if (dl != 0 &&
            clock_t_::now().time_since_epoch().count() >= dl) {
            return 1;
        }
        return 0;
    }

    std::atomic<bool>     seek_pending{false};
    std::atomic<double>   seek_target{0.0};

    clock_t_::time_point  wall_start;
    double                wall_pts_base = 0.0;
    bool                  wall_started = false;

    bool has_video() const { return video_stream >= 0; }
    bool has_audio() const { return audio_stream >= 0; }

    void teardown() {
        if (worker.joinable()) {
            {
                std::lock_guard<std::mutex> lk(ctrl_mtx);
                quit.store(true);
                paused.store(false);
            }
            ctrl_cv.notify_all();
            worker.join();
        }
        if (sws) { sws_freeContext(sws); sws = nullptr; }
        if (swr) { swr_free(&swr); }
        av_channel_layout_uninit(&swr_in_layout);
        if (hw_sw_frame) av_frame_free(&hw_sw_frame);
        if (vdec) avcodec_free_context(&vdec);
        if (adec) avcodec_free_context(&adec);
        if (fmt)  avformat_close_input(&fmt);
    }
    ~Impl() { teardown(); }

    void worker_loop();
    int  decode_first_video_frame();
    bool publish_video(AVFrame* f);
    void publish_audio(AVFrame* f);
    void ensure_swr(int out_ch, int out_rate);
    void flush_buffers_after_seek();
    void reset_clocks();
    double master_clock();

    void decode_and_present_video(AVFrame* frm);
    void decode_audio(AVFrame* frm);
    void drain_video(AVFrame* frm);
    void drain_audio(AVFrame* frm);
    void pace_video(double frame_pts);
};

Decoder::Decoder() = default;

Decoder::~Decoder() { close(); }

void Decoder::set_rate(float rate) {
    if (rate <= 0.0f) rate = 1.0f;
    rate_.store(rate);
}

void Decoder::set_volume(float volume) {
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    volume_.store(volume);
}

int32_t Decoder::open(const std::string& url) {
    close();

    state_.store(UAV_STATE_OPENING);
    last_error_.store(UAV_OK);

    std::shared_ptr<Impl> session = std::make_shared<Impl>();
    if (!session) { last_error_.store(UAV_ERR_NOMEM); state_.store(UAV_STATE_ERROR); return UAV_ERR_NOMEM; }
    Impl* d = session.get();
    d->owner = this;

    auto fail = [&](int32_t code) {
        last_error_.store(code);
        state_.store(UAV_STATE_ERROR);
        return code;
    };

    d->io_budget_ms = open_timeout_ms();
    d->fmt = avformat_alloc_context();
    if (!d->fmt) return fail(UAV_ERR_NOMEM);
    d->fmt->interrupt_callback.callback = &Impl::io_interrupt_cb;
    d->fmt->interrupt_callback.opaque   = d;
    d->arm_io_deadline();

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "reconnect", "1", 0);
    av_dict_set(&opts, "reconnect_streamed", "1", 0);
    av_dict_set(&opts, "reconnect_delay_max", "4", 0);
    {
        const int64_t us = (int64_t)d->io_budget_ms * 1000;
        char usbuf[32];
        std::snprintf(usbuf, sizeof(usbuf), "%lld", (long long)us);
        av_dict_set(&opts, "rw_timeout", usbuf, 0);
        av_dict_set(&opts, "timeout",    usbuf, 0);
        av_dict_set(&opts, "stimeout",   usbuf, 0);
    }
    {
        const bool is_sdp = url.size() >= 4 &&
                            (url.compare(url.size() - 4, 4, ".sdp") == 0);
        const bool is_rtp = url.compare(0, 6, "rtp://") == 0 ||
                            url.compare(0, 7, "rtsp://") == 0;
        if (is_sdp || is_rtp) {
            av_dict_set(&opts, "protocol_whitelist", "file,crypto,data,udp,rtp,rtcp,tcp", 0);
        }
    }
    int rc = avformat_open_input(&d->fmt, url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (rc < 0 || !d->fmt) return fail(UAV_ERR_OPEN_FAILED);

    // For live RTP/SDP the codec params come from the SDP; cap stream analysis.
    if (d->fmt->iformat && d->fmt->iformat->name &&
        (std::strstr(d->fmt->iformat->name, "sdp") ||
         std::strstr(d->fmt->iformat->name, "rtp") ||
         std::strstr(d->fmt->iformat->name, "rtsp"))) {
        d->fmt->probesize       = 65536;
        d->fmt->max_analyze_duration = (int64_t)(0.5 * AV_TIME_BASE);
    }

    d->arm_io_deadline();
    if (avformat_find_stream_info(d->fmt, nullptr) < 0)
        return fail(UAV_ERR_OPEN_FAILED);

    d->video_stream = av_find_best_stream(d->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    d->audio_stream = av_find_best_stream(d->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (d->video_stream < 0 && d->audio_stream < 0)
        return fail(UAV_ERR_NO_STREAM);

    if (d->video_stream >= 0) {
        AVStream* st = d->fmt->streams[d->video_stream];
        const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) { d->video_stream = -1; }
        else {
            d->vdec = avcodec_alloc_context3(dec);
            if (!d->vdec) return fail(UAV_ERR_NOMEM);
            if (avcodec_parameters_to_context(d->vdec, st->codecpar) < 0) return fail(UAV_ERR_DECODE);
#if defined(UAV_TSAN)
            // Single-threaded under TSan to avoid FFmpeg frame-threading false positives.
            d->vdec->thread_count = 1;
#else
            d->vdec->thread_count = 0;
#endif
            enum AVHWDeviceType cands[6];
            const int ncands = uav_hw_candidates(cands, 6);
            bool hw_on = false;
            for (int ci = 0; ci < ncands && !hw_on; ++ci) {
                if (d->hw.enable(d->vdec, cands[ci], nullptr)) {
                    av_log(d->vdec, AV_LOG_INFO, "[uav] hardware decode enabled: %s\n",
                           av_hwdevice_get_type_name(cands[ci]));
                    hw_on = true;
                }
            }
            if (!hw_on) av_log(d->vdec, AV_LOG_INFO, "[uav] software decode\n");
            if (avcodec_open2(d->vdec, dec, nullptr) < 0) return fail(UAV_ERR_DECODE);

            d->mi_width  = d->vdec->width;
            d->mi_height = d->vdec->height;
            AVRational fr = av_guess_frame_rate(d->fmt, st, nullptr);
            d->mi_fps = (fr.num && fr.den) ? av_q2d(fr) : 0.0;
        }
    }

    if (d->audio_stream >= 0) {
        AVStream* st = d->fmt->streams[d->audio_stream];
        const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) { d->audio_stream = -1; }
        else {
            d->adec = avcodec_alloc_context3(dec);
            if (!d->adec) return fail(UAV_ERR_NOMEM);
            if (avcodec_parameters_to_context(d->adec, st->codecpar) < 0) return fail(UAV_ERR_DECODE);
            if (avcodec_open2(d->adec, dec, nullptr) < 0) return fail(UAV_ERR_DECODE);
            d->mi_audio_ch   = d->adec->ch_layout.nb_channels;
            d->mi_audio_rate = d->adec->sample_rate;
        }
    }

    if (d->video_stream < 0 && d->audio_stream < 0)
        return fail(UAV_ERR_NO_STREAM);

    if (d->fmt->duration != AV_NOPTS_VALUE && d->fmt->duration > 0)
        d->mi_duration = static_cast<double>(d->fmt->duration) / (double)AV_TIME_BASE;
    else
        d->mi_duration = 0.0;

    if (d->has_audio()) {
        int rate = d->mi_audio_rate > 0 ? d->mi_audio_rate : 48000;
        int ch   = d->mi_audio_ch   > 0 ? d->mi_audio_ch   : 2;
        d->aring_cap = (size_t)std::ceil(kAudioBufSeconds * rate) * ch;
        d->aring.assign(d->aring_cap, 0.0f);
    }

    // Decode the first video frame so READY means "presentable".
    if (d->has_video()) {
        d->arm_io_deadline();
        int frc = d->decode_first_video_frame();
        if (frc != UAV_OK) {
            if (!d->has_audio()) return fail(frc);
        }
    }

    d->paused.store(true);
    d->quit.store(false);
    d->worker = std::thread([d]() { d->worker_loop(); });

    {
        std::lock_guard<std::mutex> lk(session_mtx_);
        d_ = session;
    }
    state_.store(UAV_STATE_READY);

    return UAV_OK;
}

void Decoder::close() {
    // Teardown happens here unless a reader is mid-borrow, in which case its
    // release_frame() drops the last ref.
    std::shared_ptr<Impl> dead;
    {
        std::lock_guard<std::mutex> lk(session_mtx_);
        dead = std::move(d_);
        d_.reset();
    }
    if (!dead) {
        state_.store(UAV_STATE_IDLE);
        last_error_.store(UAV_OK);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(dead->ctrl_mtx);
        dead->quit.store(true);
        dead->paused.store(false);
    }
    dead->ctrl_cv.notify_all();

    state_.store(UAV_STATE_IDLE);
    last_error_.store(UAV_OK);
}

void Decoder::play() {
    std::shared_ptr<Impl> sp = session();
    Impl* d = sp.get();
    if (!d) return;
    UAVState s = state_.load();
    if (s == UAV_STATE_IDLE || s == UAV_STATE_ERROR) return;
    if (s == UAV_STATE_FINISHED) {
        d->seek_target.store(0.0);
        d->seek_pending.store(true);
    }
    {
        std::lock_guard<std::mutex> lk(d->ctrl_mtx);
        d->reset_clocks();
        d->paused.store(false);
    }
    d->ctrl_cv.notify_all();
    state_.store(UAV_STATE_PLAYING);
}

void Decoder::pause() {
    std::shared_ptr<Impl> sp = session();
    Impl* d = sp.get();
    if (!d) return;
    if (state_.load() == UAV_STATE_PLAYING) {
        d->paused.store(true);
        state_.store(UAV_STATE_PAUSED);
    }
}

void Decoder::stop() {
    std::shared_ptr<Impl> sp = session();
    Impl* d = sp.get();
    if (!d) return;
    d->paused.store(true);
    d->seek_target.store(0.0);
    d->seek_pending.store(true);
    d->ctrl_cv.notify_all();
    UAVState s = state_.load();
    if (s != UAV_STATE_IDLE && s != UAV_STATE_ERROR)
        state_.store(UAV_STATE_PAUSED);
}

int32_t Decoder::seek(double seconds) {
    std::shared_ptr<Impl> sp = session();
    Impl* d = sp.get();
    if (!d) return UAV_ERR_INVALID;
    if (seconds < 0.0) seconds = 0.0;
    d->seek_target.store(seconds);
    d->seek_pending.store(true);
    d->ctrl_cv.notify_all();
    if (state_.load() == UAV_STATE_FINISHED) state_.store(UAV_STATE_PAUSED);
    return UAV_OK;
}

double Decoder::position() const {
    std::shared_ptr<Impl> sp = session();
    Impl* d = sp.get();
    if (!d) return 0.0;
    return d->master_clock();
}

int32_t Decoder::get_info(UAVMediaInfo& out) const {
    out = UAVMediaInfo{};
    std::shared_ptr<Impl> sp = session();
    Impl* d = sp.get();
    if (!d || state_.load() < UAV_STATE_READY) return UAV_ERR_NO_STREAM;
    out.has_video        = d->has_video() ? 1 : 0;
    out.has_audio        = d->has_audio() ? 1 : 0;
    out.width            = d->mi_width;
    out.height           = d->mi_height;
    out.frame_rate       = d->mi_fps;
    out.duration         = d->mi_duration;
    out.audio_channels   = d->mi_audio_ch;
    out.audio_sample_rate= d->mi_audio_rate;
    return UAV_OK;
}

int32_t Decoder::acquire_frame(int64_t last_frame_id, UAVVideoFrame& out) {
    out = UAVVideoFrame{};
    std::shared_ptr<Impl> sp = session();
    Impl* d = sp.get();
    if (!d || !d->has_video()) return UAV_ERR_NO_STREAM;

    d->video_mtx.lock();
    if (d->ready_slot < 0 || d->latest_frame_id.load() <= last_frame_id) {
        d->video_mtx.unlock();
        return UAV_ERR_NO_STREAM;
    }
    // Hold video_mtx and pin the session until release_frame() so the borrowed
    // slot buffer outlives a racing close()/destroy(). One borrow at a time.
    held_session_ = sp;
    d->locked_slot = d->ready_slot;
    out.data     = d->vbuf[d->locked_slot].data();
    out.width    = d->vbuf_w;
    out.height   = d->vbuf_h;
    out.stride   = d->vbuf_stride;
    out.format   = UAV_PIX_RGBA32;
    out.frame_id = d->latest_frame_id.load();
    out.pts      = d->latest_pts;
    return UAV_OK;
}

void Decoder::release_frame() {
    std::shared_ptr<Impl> sp = held_session_;
    if (!sp) return;
    Impl* d = sp.get();
    if (d->locked_slot >= 0) {
        d->locked_slot = -1;
        d->video_mtx.unlock();
    }
    held_session_.reset();
}

int32_t Decoder::read_audio(float* dst, int32_t frames, int32_t channels, int32_t sample_rate) {
    std::shared_ptr<Impl> sp = session();
    Impl* d = sp.get();
    const size_t want = (size_t)frames * (size_t)channels;
    if (!d || !d->has_audio()) {
        std::memset(dst, 0, sizeof(float) * want);
        return 0;
    }

    const bool muted = muted_.load();
    const float gain = muted ? 0.0f : volume_.load();

    int32_t got_frames = 0;
    {
        std::lock_guard<std::mutex> lk(d->audio_mtx);
        if (channels != d->swr_out_ch || sample_rate != d->swr_out_rate.load()) {
            d->ensure_swr(channels, sample_rate);
        }
        if (d->aring_cap > 0 && d->swr_out_ch > 0 && d->swr_out_ch == channels) {
            size_t avail_frames = d->afill / (size_t)d->swr_out_ch;
            size_t take_frames  = std::min<size_t>(avail_frames, (size_t)frames);
            size_t take_samples = take_frames * (size_t)channels;
            for (size_t i = 0; i < take_samples; ++i) {
                dst[i] = d->aring[d->aread] * gain;
                d->aread = (d->aread + 1) % d->aring_cap;
            }
            d->afill -= take_samples;
            got_frames = (int32_t)take_frames;
            d->audio_played_frames.fetch_add((int64_t)take_frames);
        }
    }

    if ((size_t)got_frames < (size_t)frames) {
        std::memset(dst + (size_t)got_frames * channels, 0,
                    sizeof(float) * ((size_t)frames - (size_t)got_frames) * (size_t)channels);
    }
    return got_frames;
}

void Decoder::Impl::reset_clocks() {
    // Caller holds ctrl_mtx. Re-pace the wall clock from now so a resume after
    // pause does not jump the no-audio clock forward by the paused duration.
    if (wall_started) {
        wall_pts_base = std::chrono::duration<double>(clock_t_::now() - wall_start).count()
                            * (double)owner->rate_.load() + wall_pts_base;
    }
    wall_start = clock_t_::now();
    wall_started = false;
}

double Decoder::Impl::master_clock() {
    const int out_rate = swr_out_rate.load();
    if (has_audio() && out_rate > 0) {
        double played = static_cast<double>(audio_played_frames.load()) / (double)out_rate;
        return audio_clock_base.load() + played;
    }
    std::lock_guard<std::mutex> lk(ctrl_mtx);
    if (!wall_started) return wall_pts_base;
    double elapsed = std::chrono::duration<double>(clock_t_::now() - wall_start).count();
    return wall_pts_base + elapsed * (double)owner->rate_.load();
}

void Decoder::Impl::ensure_swr(int out_ch, int out_rate) {
    // Caller holds audio_mtx.
    if (swr && out_ch == swr_out_ch && out_rate == swr_out_rate.load()) return;
    if (swr) swr_free(&swr);

    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, out_ch);

    swr_alloc_set_opts2(&swr,
        &out_layout, AV_SAMPLE_FMT_FLT, out_rate,
        &adec->ch_layout, adec->sample_fmt, adec->sample_rate,
        0, nullptr);
    av_channel_layout_uninit(&out_layout);

    if (swr && swr_init(swr) < 0) {
        swr_free(&swr);
    }
    swr_out_ch   = out_ch;
    swr_out_rate.store(out_rate);

    int rate = out_rate > 0 ? out_rate : 48000;
    aring_cap = (size_t)std::ceil(kAudioBufSeconds * rate) * (size_t)std::max(out_ch, 1);
    aring.assign(aring_cap, 0.0f);
    aread = awrite = afill = 0;
    // Re-anchor the clock so position() stays monotonic across a format change.
    audio_clock_base.store(audio_clock_base.load() + static_cast<double>(audio_played_frames.load()) / (double)std::max(rate, 1));
    audio_played_frames.store(0);
}

int Decoder::Impl::decode_first_video_frame() {
    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frm = av_frame_alloc();
    if (!pkt || !frm) { if (pkt) av_packet_free(&pkt); if (frm) av_frame_free(&frm); return UAV_ERR_NOMEM; }

    int result = UAV_ERR_DECODE;
    bool done = false;
    int guard = 0;
    while (!done && guard++ < 100000) {
        int rc = av_read_frame(fmt, pkt);
        if (rc < 0) {
            avcodec_send_packet(vdec, nullptr);
        } else if (pkt->stream_index != video_stream) {
            av_packet_unref(pkt);
            continue;
        } else {
            avcodec_send_packet(vdec, pkt);
            av_packet_unref(pkt);
        }
        while (true) {
            int r2 = avcodec_receive_frame(vdec, frm);
            if (r2 == AVERROR(EAGAIN)) break;
            if (r2 == AVERROR_EOF || r2 < 0) { done = true; break; }
            if (publish_video(frm)) result = UAV_OK;
            av_frame_unref(frm);
            done = true;
            break;
        }
        if (rc < 0 && !done) { done = true; }
    }

    av_packet_free(&pkt);
    av_frame_free(&frm);

    // Rewind so the worker decodes from the beginning when it runs.
    av_seek_frame(fmt, video_stream, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(vdec);
    if (adec) avcodec_flush_buffers(adec);
    return result;
}

bool Decoder::Impl::publish_video(AVFrame* f) {
    if (hw.is_hw_frame(f)) {
        if (!hw_sw_frame) hw_sw_frame = av_frame_alloc();
        AVFrame* sw = hw_sw_frame ? hw.download(f, hw_sw_frame) : nullptr;
        if (!sw) return false;
        f = sw;
    }
    const int w = f->width, h = f->height;
    if (w <= 0 || h <= 0) return false;

    const double pts = tb_to_sec(
        f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts,
        fmt->streams[video_stream]->time_base);

    if (!sws || sws_w != w || sws_h != h || sws_srcfmt != f->format) {
        if (sws) sws_freeContext(sws);
        sws = sws_getContext(w, h, (AVPixelFormat)f->format,
                             w, h, (AVPixelFormat)kPixFmt,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
        sws_w = w; sws_h = h; sws_srcfmt = f->format;
        if (!sws) return false;
    }

    const int stride = w * 4;
    const size_t bytes = (size_t)stride * h;

    std::lock_guard<std::mutex> lk(video_mtx);

    // Never publish a frame whose pts is not strictly newer than the last
    // published one (drops the re-decoded pre-roll frame).
    if (have_published_pts && pts <= last_published_pts) {
        return false;
    }

    int slot = -1;
    for (int i = 0; i < kVideoSlots; ++i) {
        if (i != locked_slot && i != ready_slot) { slot = i; break; }
    }
    if (slot < 0) return false;

    if (vbuf[slot].size() != bytes) vbuf[slot].assign(bytes, 0);
    uint8_t* dstdata[4] = { vbuf[slot].data(), nullptr, nullptr, nullptr };
    int      dstline[4] = { stride, 0, 0, 0 };
    sws_scale(sws, f->data, f->linesize, 0, h, dstdata, dstline);

    vbuf_w = w; vbuf_h = h; vbuf_stride = stride;
    ready_slot = slot;
    latest_pts = pts;
    last_published_pts = pts;
    have_published_pts = true;
    latest_frame_id.fetch_add(1);
    return true;
}

void Decoder::Impl::publish_audio(AVFrame* f) {
    std::lock_guard<std::mutex> lk(audio_mtx);
    if (!swr) {
        ensure_swr(std::max(mi_audio_ch, 1), mi_audio_rate > 0 ? mi_audio_rate : 48000);
    }
    if (!swr) return;

    int out_count = (int)av_rescale_rnd(
        swr_get_delay(swr, adec->sample_rate) + f->nb_samples,
        swr_out_rate.load(), adec->sample_rate, AV_ROUND_UP);
    if (out_count <= 0) return;
    if (aring_cap == 0 || swr_out_ch <= 0) return;

    std::vector<float> tmp((size_t)out_count * swr_out_ch);
    uint8_t* outp[1] = { reinterpret_cast<uint8_t*>(tmp.data()) };
    int converted = swr_convert(swr, outp, out_count,
                                (const uint8_t**)f->extended_data, f->nb_samples);
    if (converted <= 0) return;

    size_t produced = (size_t)converted * swr_out_ch;
    for (size_t i = 0; i < produced; ++i) {
        aring[awrite] = tmp[i];
        awrite = (awrite + 1) % aring_cap;
        if (afill < aring_cap) {
            afill++;
        } else {
            aread = (aread + 1) % aring_cap;
        }
    }
}

void Decoder::Impl::flush_buffers_after_seek() {
    if (vdec) avcodec_flush_buffers(vdec);
    if (adec) avcodec_flush_buffers(adec);
    {
        std::lock_guard<std::mutex> lk(audio_mtx);
        aread = awrite = afill = 0;
        audio_played_frames.store(0);
        audio_anchored = false;
    }
    {
        std::lock_guard<std::mutex> lk(video_mtx);
        have_published_pts = false;
    }
    eof.store(false);
}

void Decoder::Impl::worker_loop() {
    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frm = av_frame_alloc();
    if (!pkt || !frm) {
        if (pkt) av_packet_free(&pkt);
        if (frm) av_frame_free(&frm);
        owner->last_error_.store(UAV_ERR_NOMEM);
        owner->state_.store(UAV_STATE_ERROR);
        return;
    }

    auto do_seek = [&](double target) {
        int64_t ts = (int64_t)(target * AV_TIME_BASE);
        arm_io_deadline();
        avformat_seek_file(fmt, -1, INT64_MIN, ts, ts, AVSEEK_FLAG_BACKWARD);
        flush_buffers_after_seek();
        audio_clock_base.store(target);
        {
            std::lock_guard<std::mutex> lk(ctrl_mtx);
            wall_pts_base = target;
            wall_started = false;
        }
    };

    while (!quit.load()) {
        if (seek_pending.exchange(false)) {
            do_seek(seek_target.load());
        }

        if (paused.load()) {
            std::unique_lock<std::mutex> lk(ctrl_mtx);
            ctrl_cv.wait_for(lk, std::chrono::milliseconds(20), [&] {
                return quit.load() || !paused.load() || seek_pending.load();
            });
            continue;
        }

        bool ring_full = false;
        if (has_audio()) {
            std::lock_guard<std::mutex> lk(audio_mtx);
            ring_full = afill > (aring_cap * 3) / 4;
        }

        if (ring_full) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // Re-arm before each blocking read so healthy playback is never cut short.
        arm_io_deadline();
        int rc = av_read_frame(fmt, pkt);
        if (rc == AVERROR(EAGAIN)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (rc < 0) {
            if (has_video()) { avcodec_send_packet(vdec, nullptr); drain_video(frm); }
            if (has_audio()) { avcodec_send_packet(adec, nullptr); drain_audio(frm); }

            if (owner->looping_.load()) {
                do_seek(0.0);
                continue;
            }
            // Wait for the audio ring to drain (trailing audio), then FINISH.
            bool drained = true;
            if (has_audio()) {
                std::lock_guard<std::mutex> lk(audio_mtx);
                drained = (afill == 0);
            }
            if (drained) {
                owner->state_.store(UAV_STATE_FINISHED);
                paused.store(true);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            continue;
        }

        if (pkt->stream_index == video_stream) {
            avcodec_send_packet(vdec, pkt);
            av_packet_unref(pkt);
            decode_and_present_video(frm);
        } else if (pkt->stream_index == audio_stream) {
            avcodec_send_packet(adec, pkt);
            av_packet_unref(pkt);
            decode_audio(frm);
        } else {
            av_packet_unref(pkt);
        }
    }

    av_packet_free(&pkt);
    av_frame_free(&frm);
}

void Decoder::Impl::pace_video(double frame_pts) {
    // Anti-starvation invariant: with audio present the master clock advances
    // only as the audio reader consumes samples. Sleeping here while an actively
    // draining reader has emptied the ring would stop audio decode and stall the
    // clock forever. So when audio is present, a reader is draining
    // (audio_played_frames > 0), the lead is small, and the ring is below
    // low-water, return early to let the loop refill audio. A caller that pulls
    // video but never reads audio keeps strict pacing.
    while (!quit.load() && !paused.load() && !seek_pending.load()) {
        double now = master_clock();
        double lead = frame_pts - now;
        if (lead <= 0.0) return;
        if (has_audio() && audio_played_frames.load() > 0 && lead <= kMaxStarveLead) {
            std::lock_guard<std::mutex> lk(audio_mtx);
            if (aring_cap > 0 && afill < aring_cap / 4) return;
        }
        if (lead > 1.0) lead = 1.0;
        double slice = std::min(lead, kVideoLeadCap);
        std::this_thread::sleep_for(std::chrono::duration<double>(slice));
    }
}

void Decoder::Impl::decode_and_present_video(AVFrame* frm) {
    while (true) {
        int r = avcodec_receive_frame(vdec, frm);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF || r < 0) break;

        double pts = tb_to_sec(frm->best_effort_timestamp != AV_NOPTS_VALUE
                                   ? frm->best_effort_timestamp : frm->pts,
                               fmt->streams[video_stream]->time_base);

        if (!has_audio()) {
            std::lock_guard<std::mutex> lk(ctrl_mtx);
            if (!wall_started) {
                wall_start = clock_t_::now();
                wall_pts_base = pts;
                wall_started = true;
            }
        }

        pace_video(pts);
        publish_video(frm);
        av_frame_unref(frm);
    }
}

void Decoder::Impl::decode_audio(AVFrame* frm) {
    while (true) {
        int r = avcodec_receive_frame(adec, frm);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF || r < 0) break;
        // Anchor the clock to the first decoded audio pts after a (re)start/seek.
        if (!audio_anchored) {
            double apts = tb_to_sec(frm->best_effort_timestamp != AV_NOPTS_VALUE
                                        ? frm->best_effort_timestamp : frm->pts,
                                    fmt->streams[audio_stream]->time_base);
            audio_clock_base.store(apts);
            audio_played_frames.store(0);
            audio_anchored = true;
        }
        publish_audio(frm);
        av_frame_unref(frm);
    }
}

void Decoder::Impl::drain_video(AVFrame* frm) {
    while (true) {
        int r = avcodec_receive_frame(vdec, frm);
        if (r < 0) break;
        publish_video(frm);
        av_frame_unref(frm);
    }
}

void Decoder::Impl::drain_audio(AVFrame* frm) {
    while (true) {
        int r = avcodec_receive_frame(adec, frm);
        if (r < 0) break;
        publish_audio(frm);
        av_frame_unref(frm);
    }
}

} // namespace uav

#else // !UAV_HAVE_FFMPEG

namespace uav {

struct Decoder::Impl {};

Decoder::Decoder() = default;
Decoder::~Decoder() = default;

int32_t Decoder::open(const std::string&) {
    last_error_.store(UAV_ERR_UNSUPPORTED);
    state_.store(UAV_STATE_ERROR);
    return UAV_ERR_UNSUPPORTED;
}
void Decoder::close() { state_.store(UAV_STATE_IDLE); last_error_.store(UAV_OK); }
void Decoder::play() {}
void Decoder::pause() {}
void Decoder::stop() {}
int32_t Decoder::seek(double) { return UAV_ERR_UNSUPPORTED; }
void Decoder::set_rate(float) {}
void Decoder::set_volume(float) {}
double Decoder::position() const { return 0.0; }
int32_t Decoder::get_info(UAVMediaInfo& out) const { out = UAVMediaInfo{}; return UAV_ERR_NO_STREAM; }
int32_t Decoder::acquire_frame(int64_t, UAVVideoFrame& out) { out = UAVVideoFrame{}; return UAV_ERR_NO_STREAM; }
void Decoder::release_frame() {}
int32_t Decoder::read_audio(float* dst, int32_t frames, int32_t channels, int32_t) {
    if (dst && frames > 0 && channels > 0) std::memset(dst, 0, sizeof(float) * (size_t)frames * channels);
    return 0;
}

} // namespace uav

#endif // UAV_HAVE_FFMPEG
