// SPDX-License-Identifier: Apache-2.0
#include "unitedav_send.h"
#include "sender.hpp"

#include <new>

using uav::Sender;

namespace {
inline Sender* self(UAVSender* s) { return reinterpret_cast<Sender*>(s); }
}

extern "C" {

UAV_API uint32_t UAV_CALL uav_send_abi_version(void) { return UAV_SEND_ABI_VERSION; }

UAV_API UAVSender* UAV_CALL uav_send_create(void) {
    return reinterpret_cast<UAVSender*>(new (std::nothrow) Sender());
}

UAV_API void UAV_CALL uav_send_destroy(UAVSender* s) {
    delete self(s);
}

UAV_API int32_t UAV_CALL uav_send_open(UAVSender* s, const char* url, const UAVSendConfig* cfg) {
    if (!s || !url || !cfg) return UAV_SEND_ERR_INVALID;
    return self(s)->open(url, *cfg);
}

UAV_API int32_t UAV_CALL uav_send_push_video(UAVSender* s, const uint8_t* rgba,
                                             int32_t w, int32_t h, int32_t stride,
                                             double pts_seconds) {
    if (!s) return UAV_SEND_ERR_INVALID;
    return self(s)->push_video(rgba, w, h, stride, pts_seconds);
}

UAV_API int32_t UAV_CALL uav_send_push_audio(UAVSender* s, const float* interleaved,
                                             int32_t frames, int32_t channels,
                                             int32_t sample_rate, double pts_seconds) {
    if (!s) return UAV_SEND_ERR_INVALID;
    return self(s)->push_audio(interleaved, frames, channels, sample_rate, pts_seconds);
}

UAV_API int32_t UAV_CALL uav_send_close(UAVSender* s) {
    if (!s) return UAV_SEND_ERR_INVALID;
    return self(s)->close();
}

UAV_API int32_t UAV_CALL uav_send_last_error(UAVSender* s) {
    return s ? self(s)->last_error() : UAV_SEND_ERR_INVALID;
}

UAV_API int32_t UAV_CALL uav_send_get_sdp(UAVSender* s, char* buf, int32_t buflen) {
    if (!s) return UAV_SEND_ERR_INVALID;
    return self(s)->get_sdp(buf, buflen);
}

} // extern "C"
