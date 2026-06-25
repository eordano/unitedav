// SPDX-License-Identifier: Apache-2.0
/* UnitedAV — native plugin C ABI. Stable cdecl (UAV_CALL empty on every
 * platform) for a single P/Invoke convention. */
#ifndef UNITEDAV_H
#define UNITEDAV_H

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

#define UAV_ABI_VERSION 1

typedef struct UAVPlayer UAVPlayer;

typedef enum UAVState {
    UAV_STATE_IDLE      = 0,
    UAV_STATE_OPENING   = 1,
    UAV_STATE_READY     = 2,  /* metadata + first frame available */
    UAV_STATE_PLAYING   = 3,
    UAV_STATE_PAUSED    = 4,
    UAV_STATE_BUFFERING = 5,
    UAV_STATE_FINISHED  = 6,
    UAV_STATE_ERROR     = 7
} UAVState;

typedef enum UAVResult {
    UAV_OK              = 0,
    UAV_ERR_INVALID     = -1,
    UAV_ERR_OPEN_FAILED = -2,
    UAV_ERR_NO_STREAM   = -3,
    UAV_ERR_DECODE      = -4,
    UAV_ERR_UNSUPPORTED = -5,
    UAV_ERR_NOMEM       = -6
} UAVResult;

typedef enum UAVPixelFormat {
    UAV_PIX_RGBA32 = 0,
    UAV_PIX_NV12   = 1
} UAVPixelFormat;

/* `data` is owned by the player and valid only until the matching unlock. */
typedef struct UAVVideoFrame {
    const uint8_t* data;
    int32_t        width;
    int32_t        height;
    int32_t        stride;
    int32_t        format;   /* UAVPixelFormat */
    int64_t        frame_id; /* monotonic; lets the caller skip duplicate uploads */
    double         pts;      /* seconds */
} UAVVideoFrame;

typedef struct UAVMediaInfo {
    int32_t has_video;
    int32_t has_audio;
    int32_t width;
    int32_t height;
    double  frame_rate;       /* 0 if unknown */
    double  duration;         /* seconds, 0/negative if live/unknown */
    int32_t audio_channels;
    int32_t audio_sample_rate;
} UAVMediaInfo;

UAV_API uint32_t   UAV_CALL uav_abi_version(void);
UAV_API UAVPlayer* UAV_CALL uav_create(void);
UAV_API void       UAV_CALL uav_destroy(UAVPlayer* p);

UAV_API int32_t UAV_CALL uav_open(UAVPlayer* p, const char* url);
UAV_API int32_t UAV_CALL uav_close(UAVPlayer* p);
UAV_API int32_t UAV_CALL uav_play(UAVPlayer* p);
UAV_API int32_t UAV_CALL uav_pause(UAVPlayer* p);
UAV_API int32_t UAV_CALL uav_stop(UAVPlayer* p);
UAV_API int32_t UAV_CALL uav_seek(UAVPlayer* p, double seconds);
UAV_API int32_t UAV_CALL uav_set_looping(UAVPlayer* p, int32_t loop);
UAV_API int32_t UAV_CALL uav_set_rate(UAVPlayer* p, float rate);
UAV_API int32_t UAV_CALL uav_set_volume(UAVPlayer* p, float volume); /* 0..1 */
UAV_API int32_t UAV_CALL uav_set_muted(UAVPlayer* p, int32_t muted);

UAV_API int32_t UAV_CALL uav_get_state(UAVPlayer* p);
UAV_API double  UAV_CALL uav_get_position(UAVPlayer* p);  /* seconds */
UAV_API int32_t UAV_CALL uav_get_info(UAVPlayer* p, UAVMediaInfo* out);
UAV_API int32_t UAV_CALL uav_last_error(UAVPlayer* p);

/* Fills `out` with a frame newer than `last_frame_id`; the buffer stays valid
 * until uav_release_frame. UAV_ERR_NO_STREAM when nothing new. */
UAV_API int32_t UAV_CALL uav_acquire_frame(UAVPlayer* p, int64_t last_frame_id, UAVVideoFrame* out);
UAV_API void    UAV_CALL uav_release_frame(UAVPlayer* p);

/* Pull up to `frames` interleaved float samples for `channels` into `dst`;
 * returns the frames actually written (0 on underrun). */
UAV_API int32_t UAV_CALL uav_read_audio(UAVPlayer* p, float* dst, int32_t frames, int32_t channels, int32_t sample_rate);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* UNITEDAV_H */
