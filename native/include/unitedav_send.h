// SPDX-License-Identifier: Apache-2.0
/*
 * UnitedAV — sender C ABI (libavformat transports: file / RTP / SRT). Stable
 * cdecl, mirroring unitedav.h.
 *
 * Threading: a UAVSender is NOT internally synchronized. All calls for a handle
 * must come from one thread or be externally serialized.
 */
#ifndef UNITEDAV_SEND_H
#define UNITEDAV_SEND_H

#include <stdint.h>

#if defined(_WIN32)
#  define UAV_API __declspec(dllexport)
#  define UAV_CALL
#else
#  define UAV_API __attribute__((visibility("default")))
#  define UAV_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define UAV_SEND_ABI_VERSION 1

typedef struct UAVSender UAVSender;

/* 0 == OK. Mirrors unitedav.h's UAVResult numbering. */
typedef enum UAVSendResult {
    UAV_SEND_OK              = 0,
    UAV_SEND_ERR_INVALID     = -1,
    UAV_SEND_ERR_OPEN_FAILED = -2,
    UAV_SEND_ERR_NO_STREAM   = -3,
    UAV_SEND_ERR_ENCODE      = -4,
    UAV_SEND_ERR_UNSUPPORTED = -5,
    UAV_SEND_ERR_NOMEM       = -6
} UAVSendResult;

typedef enum UAVVideoCodec {
    UAV_VCODEC_NONE = 0,
    UAV_VCODEC_VP9  = 1,  /* libvpx-vp9 (default) */
    UAV_VCODEC_VP8  = 2,  /* libvpx */
    UAV_VCODEC_AV1  = 3   /* libaom-av1 */
} UAVVideoCodec;

typedef enum UAVAudioCodec {
    UAV_ACODEC_NONE = 0,
    UAV_ACODEC_OPUS = 1   /* libopus (default) */
} UAVAudioCodec;

/* Zero-initialize then fill what you need; a codec of *_NONE disables that media. */
typedef struct UAVSendConfig {
    int32_t video_codec;   /* UAVVideoCodec */
    int32_t width;         /* 0 => from the first pushed frame */
    int32_t height;        /* 0 => from the first pushed frame */
    int32_t fps;           /* <=0 => 30   */
    int32_t video_bitrate; /* bits/sec; <=0 => default */

    int32_t audio_codec;   /* UAVAudioCodec */
    int32_t sample_rate;   /* Hz; <=0 => 48000 */
    int32_t channels;      /* 1 or 2; <=0 => 2 */
    int32_t audio_bitrate; /* bits/sec; <=0 => 96000 */
} UAVSendConfig;

UAV_API uint32_t   UAV_CALL uav_send_abi_version(void);
UAV_API UAVSender* UAV_CALL uav_send_create(void);
UAV_API void       UAV_CALL uav_send_destroy(UAVSender* s);

/* `url` scheme picks transport + container: file:///path or bare path -> webm/
 * matroska; rtp://host:port -> RTP (A+V multiplexed into one stream; SDP via
 * uav_send_get_sdp); srt://host:port -> MPEG-TS over SRT. Call once before any
 * push. */
UAV_API int32_t UAV_CALL uav_send_open(UAVSender* s, const char* url, const UAVSendConfig* cfg);

/* Push one RGBA8888 video frame (tightly packed if stride == w*4, else stride is
 * bytes per row). `pts_seconds` is on the sender's timeline (monotonic). */
UAV_API int32_t UAV_CALL uav_send_push_video(UAVSender* s, const uint8_t* rgba,
                                             int32_t w, int32_t h, int32_t stride,
                                             double pts_seconds);

/* Push `frames` of interleaved float audio (~[-1,1]). `channels`/`sample_rate`
 * describe the INPUT layout; resampled to the Opus layout. Timing is driven by
 * the running sample count, so `pts_seconds` is informational. */
UAV_API int32_t UAV_CALL uav_send_push_audio(UAVSender* s, const float* interleaved,
                                             int32_t frames, int32_t channels,
                                             int32_t sample_rate, double pts_seconds);

/* Flush encoders, write the trailer, close avio/format context. Idempotent;
 * the handle may be reopened afterward. */
UAV_API int32_t UAV_CALL uav_send_close(UAVSender* s);

UAV_API int32_t UAV_CALL uav_send_last_error(UAVSender* s);

/* Copy the RTP session SDP into `buf` (NUL-terminated, truncated to `buflen`).
 * Returns the byte count that WOULD be written (excluding NUL), or a negative
 * UAV_SEND_ERR_* code. Only meaningful for rtp:// outputs. */
UAV_API int32_t UAV_CALL uav_send_get_sdp(UAVSender* s, char* buf, int32_t buflen);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* UNITEDAV_SEND_H */
