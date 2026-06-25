// SPDX-License-Identifier: Apache-2.0
#include "unitedav.h"
#include "player.hpp"

#include <new>

using uav::Player;

namespace {
inline Player* self(UAVPlayer* p) { return reinterpret_cast<Player*>(p); }
}

extern "C" {

UAV_API uint32_t UAV_CALL uav_abi_version(void) { return UAV_ABI_VERSION; }

UAV_API UAVPlayer* UAV_CALL uav_create(void) {
    return reinterpret_cast<UAVPlayer*>(new (std::nothrow) Player());
}

UAV_API void UAV_CALL uav_destroy(UAVPlayer* p) {
    delete self(p);
}

UAV_API int32_t UAV_CALL uav_open(UAVPlayer* p, const char* url) {
    if (!p || !url) return UAV_ERR_INVALID;
    return self(p)->open(url);
}

UAV_API int32_t UAV_CALL uav_close(UAVPlayer* p) {
    if (!p) return UAV_ERR_INVALID;
    return self(p)->close();
}

UAV_API int32_t UAV_CALL uav_play(UAVPlayer* p)  { return p ? self(p)->play()  : UAV_ERR_INVALID; }
UAV_API int32_t UAV_CALL uav_pause(UAVPlayer* p) { return p ? self(p)->pause() : UAV_ERR_INVALID; }
UAV_API int32_t UAV_CALL uav_stop(UAVPlayer* p)  { return p ? self(p)->stop()  : UAV_ERR_INVALID; }

UAV_API int32_t UAV_CALL uav_seek(UAVPlayer* p, double seconds) {
    return p ? self(p)->seek(seconds) : UAV_ERR_INVALID;
}
UAV_API int32_t UAV_CALL uav_set_looping(UAVPlayer* p, int32_t loop) {
    return p ? self(p)->set_looping(loop != 0) : UAV_ERR_INVALID;
}
UAV_API int32_t UAV_CALL uav_set_rate(UAVPlayer* p, float rate) {
    return p ? self(p)->set_rate(rate) : UAV_ERR_INVALID;
}
UAV_API int32_t UAV_CALL uav_set_volume(UAVPlayer* p, float volume) {
    return p ? self(p)->set_volume(volume) : UAV_ERR_INVALID;
}
UAV_API int32_t UAV_CALL uav_set_muted(UAVPlayer* p, int32_t muted) {
    return p ? self(p)->set_muted(muted != 0) : UAV_ERR_INVALID;
}

UAV_API int32_t UAV_CALL uav_get_state(UAVPlayer* p) {
    return p ? static_cast<int32_t>(self(p)->state()) : UAV_STATE_ERROR;
}
UAV_API double UAV_CALL uav_get_position(UAVPlayer* p) {
    return p ? self(p)->position() : 0.0;
}
UAV_API int32_t UAV_CALL uav_get_info(UAVPlayer* p, UAVMediaInfo* out) {
    if (!p || !out) return UAV_ERR_INVALID;
    return self(p)->get_info(*out);
}
UAV_API int32_t UAV_CALL uav_last_error(UAVPlayer* p) {
    return p ? self(p)->last_error() : UAV_ERR_INVALID;
}

UAV_API int32_t UAV_CALL uav_acquire_frame(UAVPlayer* p, int64_t last_frame_id, UAVVideoFrame* out) {
    if (!p || !out) return UAV_ERR_INVALID;
    return self(p)->acquire_frame(last_frame_id, *out);
}
UAV_API void UAV_CALL uav_release_frame(UAVPlayer* p) {
    if (p) self(p)->release_frame();
}

UAV_API int32_t UAV_CALL uav_read_audio(UAVPlayer* p, float* dst, int32_t frames, int32_t channels, int32_t sample_rate) {
    if (!p || !dst || frames <= 0 || channels <= 0) return 0;
    return self(p)->read_audio(dst, frames, channels, sample_rate);
}

} // extern "C"
