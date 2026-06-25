// SPDX-License-Identifier: Apache-2.0

#include "oracle_ref.hpp"

#include <cmath>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace uav_oracle {

namespace {
double tb_to_sec(int64_t ts, AVRational tb) {
    if (ts == AV_NOPTS_VALUE) return 0.0;
    return ts * av_q2d(tb);
}
} // namespace

struct Reference::Impl {
    AVFormatContext* fmt = nullptr;
    AVCodecContext*  vdec = nullptr;
    AVCodecContext*  adec = nullptr;
    SwsContext*      sws = nullptr;
    SwrContext*      swr = nullptr;
    int sws_w = 0, sws_h = 0, sws_fmt = -1;

    ~Impl() {
        if (sws) sws_freeContext(sws);
        if (swr) swr_free(&swr);
        if (vdec) avcodec_free_context(&vdec);
        if (adec) avcodec_free_context(&adec);
        if (fmt)  avformat_close_input(&fmt);
    }
};

Reference::~Reference() { delete d_; }

bool Reference::open(const std::string& path) {
    d_ = new Impl();
    if (avformat_open_input(&d_->fmt, path.c_str(), nullptr, nullptr) < 0 || !d_->fmt) {
        last_error_ = "avformat_open_input failed: " + path;
        return false;
    }
    if (avformat_find_stream_info(d_->fmt, nullptr) < 0) {
        last_error_ = "avformat_find_stream_info failed";
        return false;
    }
    video_stream_ = av_find_best_stream(d_->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audio_stream_ = av_find_best_stream(d_->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (video_stream_ < 0 && audio_stream_ < 0) {
        last_error_ = "no decodable stream";
        return false;
    }

    if (video_stream_ >= 0) {
        AVStream* st = d_->fmt->streams[video_stream_];
        const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) { video_stream_ = -1; }
        else {
            d_->vdec = avcodec_alloc_context3(dec);
            avcodec_parameters_to_context(d_->vdec, st->codecpar);
            d_->vdec->thread_count = 1;
            if (avcodec_open2(d_->vdec, dec, nullptr) < 0) {
                last_error_ = "video avcodec_open2 failed";
                video_stream_ = -1;
            } else {
                colorimetry_.colorspace  = (int)d_->vdec->colorspace;
                colorimetry_.range       = (int)d_->vdec->color_range;
                colorimetry_.transfer    = (int)d_->vdec->color_trc;
                colorimetry_.primaries   = (int)d_->vdec->color_primaries;
                colorimetry_.src_pix_fmt = (int)d_->vdec->pix_fmt;
                const AVPixFmtDescriptor* pd = av_pix_fmt_desc_get(d_->vdec->pix_fmt);
                if (pd) colorimetry_.bit_depth = pd->comp[0].depth;
            }
        }
    }

    if (audio_stream_ >= 0) {
        AVStream* st = d_->fmt->streams[audio_stream_];
        const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) { audio_stream_ = -1; }
        else {
            d_->adec = avcodec_alloc_context3(dec);
            avcodec_parameters_to_context(d_->adec, st->codecpar);
            if (avcodec_open2(d_->adec, dec, nullptr) < 0) {
                last_error_ = "audio avcodec_open2 failed";
                audio_stream_ = -1;
            }
        }
    }

    if (video_stream_ < 0 && audio_stream_ < 0) {
        if (last_error_.empty()) last_error_ = "no usable decoder";
        return false;
    }
    return true;
}

std::vector<RefFrame> Reference::decode_video(int max_frames) {
    std::vector<RefFrame> out;
    if (!d_ || video_stream_ < 0 || max_frames <= 0) return out;

    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frm = av_frame_alloc();
    if (!pkt || !frm) { if (pkt) av_packet_free(&pkt); if (frm) av_frame_free(&frm); return out; }

    bool colorimetry_from_frame = false;
    auto emit = [&](AVFrame* f) {
        const int w = f->width, h = f->height;
        if (w <= 0 || h <= 0) return;
        if (!colorimetry_from_frame) {
            if (f->colorspace != AVCOL_SPC_UNSPECIFIED) colorimetry_.colorspace = (int)f->colorspace;
            if (f->color_range != AVCOL_RANGE_UNSPECIFIED) colorimetry_.range = (int)f->color_range;
            if (f->color_trc  != AVCOL_TRC_UNSPECIFIED) colorimetry_.transfer = (int)f->color_trc;
            if (f->color_primaries != AVCOL_PRI_UNSPECIFIED) colorimetry_.primaries = (int)f->color_primaries;
            colorimetry_.src_pix_fmt = f->format;
            const AVPixFmtDescriptor* pd = av_pix_fmt_desc_get((AVPixelFormat)f->format);
            if (pd) colorimetry_.bit_depth = pd->comp[0].depth;
            colorimetry_from_frame = true;
        }
        // Scaler must stay byte-identical to decoder.cpp: AV_PIX_FMT_RGBA, SWS_BILINEAR.
        if (!d_->sws || d_->sws_w != w || d_->sws_h != h || d_->sws_fmt != f->format) {
            if (d_->sws) sws_freeContext(d_->sws);
            d_->sws = sws_getContext(w, h, (AVPixelFormat)f->format,
                                     w, h, AV_PIX_FMT_RGBA,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
            d_->sws_w = w; d_->sws_h = h; d_->sws_fmt = f->format;
        }
        if (!d_->sws) return;
        RefFrame rf;
        rf.width = w; rf.height = h; rf.stride = w * 4;
        rf.rgba.assign((size_t)rf.stride * h, 0);
        rf.pts = tb_to_sec(f->best_effort_timestamp != AV_NOPTS_VALUE
                               ? f->best_effort_timestamp : f->pts,
                           d_->fmt->streams[video_stream_]->time_base);
        uint8_t* dd[4] = { rf.rgba.data(), nullptr, nullptr, nullptr };
        int      dl[4] = { rf.stride, 0, 0, 0 };
        sws_scale(d_->sws, f->data, f->linesize, 0, h, dd, dl);
        out.push_back(std::move(rf));
    };

    bool done = false;
    int guard = 0;
    while (!done && (int)out.size() < max_frames && guard++ < 1000000) {
        int rc = av_read_frame(d_->fmt, pkt);
        if (rc < 0) {
            avcodec_send_packet(d_->vdec, nullptr);
        } else if (pkt->stream_index != video_stream_) {
            av_packet_unref(pkt);
            continue;
        } else {
            avcodec_send_packet(d_->vdec, pkt);
            av_packet_unref(pkt);
        }
        while ((int)out.size() < max_frames) {
            int r2 = avcodec_receive_frame(d_->vdec, frm);
            if (r2 == AVERROR(EAGAIN)) break;
            if (r2 == AVERROR_EOF || r2 < 0) { done = true; break; }
            emit(frm);
            av_frame_unref(frm);
        }
        if (rc < 0) done = true;
    }

    av_packet_free(&pkt);
    av_frame_free(&frm);
    return out;
}

RefAudio Reference::decode_audio(int out_channels, int out_rate, double min_seconds) {
    RefAudio ra;
    if (!d_ || audio_stream_ < 0 || out_channels <= 0 || out_rate <= 0) return ra;
    ra.sample_rate = out_rate;
    ra.channel_count = out_channels;
    ra.channels.assign((size_t)out_channels, {});

    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, out_channels);
    if (swr_alloc_set_opts2(&d_->swr,
            &out_layout, AV_SAMPLE_FMT_FLT, out_rate,
            &d_->adec->ch_layout, d_->adec->sample_fmt, d_->adec->sample_rate,
            0, nullptr) < 0 || !d_->swr || swr_init(d_->swr) < 0) {
        av_channel_layout_uninit(&out_layout);
        if (d_->swr) swr_free(&d_->swr);
        last_error_ = "swr init failed";
        return ra;
    }
    av_channel_layout_uninit(&out_layout);

    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frm = av_frame_alloc();
    if (!pkt || !frm) { if (pkt) av_packet_free(&pkt); if (frm) av_frame_free(&frm); return ra; }

    const size_t want_frames = (size_t)std::ceil(min_seconds * out_rate);

    auto convert = [&](AVFrame* f) {
        int out_count = (int)av_rescale_rnd(
            swr_get_delay(d_->swr, d_->adec->sample_rate) + (f ? f->nb_samples : 0),
            out_rate, d_->adec->sample_rate, AV_ROUND_UP);
        if (out_count <= 0) return;
        std::vector<float> tmp((size_t)out_count * out_channels);
        uint8_t* outp[1] = { reinterpret_cast<uint8_t*>(tmp.data()) };
        const uint8_t** in = f ? (const uint8_t**)f->extended_data : nullptr;
        int in_n = f ? f->nb_samples : 0;
        int conv = swr_convert(d_->swr, outp, out_count, in, in_n);
        if (conv <= 0) return;
        for (int i = 0; i < conv; ++i)
            for (int c = 0; c < out_channels; ++c)
                ra.channels[c].push_back(tmp[(size_t)i * out_channels + c]);
    };

    bool done = false;
    int guard = 0;
    while (!done && ra.channels[0].size() < want_frames && guard++ < 1000000) {
        int rc = av_read_frame(d_->fmt, pkt);
        if (rc < 0) {
            avcodec_send_packet(d_->adec, nullptr);
        } else if (pkt->stream_index != audio_stream_) {
            av_packet_unref(pkt);
            continue;
        } else {
            avcodec_send_packet(d_->adec, pkt);
            av_packet_unref(pkt);
        }
        while (true) {
            int r2 = avcodec_receive_frame(d_->adec, frm);
            if (r2 == AVERROR(EAGAIN)) break;
            if (r2 == AVERROR_EOF || r2 < 0) { done = true; break; }
            convert(frm);
            av_frame_unref(frm);
        }
        if (rc < 0) {
            convert(nullptr);
            done = true;
        }
    }

    av_packet_free(&pkt);
    av_frame_free(&frm);
    return ra;
}

const char* colorspace_name(int v) {
    switch (v) {
        case AVCOL_SPC_RGB:         return "rgb";
        case AVCOL_SPC_BT709:       return "bt709";
        case AVCOL_SPC_UNSPECIFIED: return "unspecified";
        case AVCOL_SPC_FCC:         return "fcc";
        case AVCOL_SPC_BT470BG:     return "bt470bg/bt601";
        case AVCOL_SPC_SMPTE170M:   return "smpte170m/bt601";
        case AVCOL_SPC_SMPTE240M:   return "smpte240m";
        case AVCOL_SPC_BT2020_NCL:  return "bt2020nc";
        case AVCOL_SPC_BT2020_CL:   return "bt2020c";
        default:                    return "other";
    }
}

const char* colorrange_name(int v) {
    switch (v) {
        case AVCOL_RANGE_MPEG: return "mpeg/limited";
        case AVCOL_RANGE_JPEG: return "jpeg/full";
        default:               return "unspecified";
    }
}

const char* colortrc_name(int v) {
    switch (v) {
        case AVCOL_TRC_BT709:        return "bt709";
        case AVCOL_TRC_UNSPECIFIED:  return "unspecified";
        case AVCOL_TRC_GAMMA22:      return "gamma22";
        case AVCOL_TRC_GAMMA28:      return "gamma28";
        case AVCOL_TRC_SMPTE170M:    return "smpte170m";
        case AVCOL_TRC_IEC61966_2_1: return "srgb/iec61966-2-1";
        case AVCOL_TRC_SMPTE2084:    return "smpte2084/pq";
        case AVCOL_TRC_ARIB_STD_B67: return "hlg";
        default:                     return "other";
    }
}

} // namespace uav_oracle
