// SPDX-License-Identifier: Apache-2.0
// UnitedAV — FFmpeg encode + mux + send pipeline for one session. Not internally
// synchronized: all calls for a handle must come from one thread.
#pragma once

#include "unitedav_send.h"

#include <cstdint>
#include <memory>
#include <string>

namespace uav {

class Sender {
public:
    Sender();
    ~Sender();

    int32_t open(const std::string& url, const UAVSendConfig& cfg);
    int32_t push_video(const uint8_t* rgba, int w, int h, int stride, double pts_seconds);
    int32_t push_audio(const float* interleaved, int frames, int channels,
                       int sample_rate, double pts_seconds);
    int32_t close();
    int32_t last_error() const { return last_error_; }
    int32_t get_sdp(char* buf, int buflen) const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
    int32_t last_error_ = UAV_SEND_OK;
};

} // namespace uav
