// SPDX-License-Identifier: Apache-2.0
// UnitedAV — Player: owns one media session behind the C ABI (wraps uav::Decoder).
#pragma once

#include "unitedav.h"
#include "decode/decoder.hpp"

#include <atomic>
#include <string>

namespace uav {

class Player {
public:
    Player();
    ~Player();

    int32_t  open(const char* url);
    int32_t  close();
    int32_t  play();
    int32_t  pause();
    int32_t  stop();
    int32_t  seek(double seconds);
    int32_t  set_looping(bool loop);
    int32_t  set_rate(float rate);
    int32_t  set_volume(float volume);
    int32_t  set_muted(bool muted);

    UAVState state() const { return decoder_.state(); }
    double   position() const { return decoder_.position(); }
    int32_t  get_info(UAVMediaInfo& out) const { return decoder_.get_info(out); }
    int32_t  last_error() const { return decoder_.last_error(); }

    int32_t  acquire_frame(int64_t last_frame_id, UAVVideoFrame& out) {
        return decoder_.acquire_frame(last_frame_id, out);
    }
    void     release_frame() { decoder_.release_frame(); }
    int32_t  read_audio(float* dst, int32_t frames, int32_t channels, int32_t sample_rate) {
        return decoder_.read_audio(dst, frames, channels, sample_rate);
    }

private:
    Decoder     decoder_;
    std::string url_;

    // Atomic: ABI setters may run on a different thread than open(), which
    // re-reads these after decoder_.open() to re-apply pre-open control state.
    std::atomic<bool>  looping_{false};
    std::atomic<float> rate_{1.0f};
    std::atomic<float> volume_{1.0f};
    std::atomic<bool>  muted_{false};
};

} // namespace uav
