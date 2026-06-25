// SPDX-License-Identifier: Apache-2.0
// UnitedAV — FFmpeg demux/decode pipeline for one media session.
#pragma once

#include "unitedav.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace uav {

class Decoder {
public:
    Decoder();
    ~Decoder();

    int32_t open(const std::string& url);
    void     close();

    void play();
    void pause();
    void stop();
    int32_t seek(double seconds);

    void set_looping(bool loop) { looping_.store(loop); }
    void set_rate(float rate);
    void set_volume(float volume);
    void set_muted(bool muted) { muted_.store(muted); }

    UAVState state() const { return state_.load(); }
    int32_t  last_error() const { return last_error_.load(); }
    double   position() const;
    int32_t  get_info(UAVMediaInfo& out) const;

    // Returns the latest frame if newer than last_frame_id; holds the read lock
    // until release_frame().
    int32_t  acquire_frame(int64_t last_frame_id, UAVVideoFrame& out);
    void     release_frame();

    int32_t  read_audio(float* dst, int32_t frames, int32_t channels, int32_t sample_rate);

private:
    struct Impl;

    // shared_ptr-owned so an ABI getter can pin the Impl alive for a whole call
    // across a concurrent close(); teardown runs when the last reference drops.
    // acquire_frame stows its pin in held_session_ so the borrowed buffer
    // outlives the acquire->release window.
    std::shared_ptr<Impl>  d_;
    std::shared_ptr<Impl>  held_session_;
    mutable std::mutex     session_mtx_;

    std::shared_ptr<Impl> session() const {
        std::lock_guard<std::mutex> lk(session_mtx_);
        return d_;
    }

    std::atomic<UAVState> state_{UAV_STATE_IDLE};
    std::atomic<int32_t>  last_error_{UAV_OK};
    std::atomic<bool>     looping_{false};
    std::atomic<bool>     muted_{false};
    std::atomic<float>    volume_{1.0f};
    std::atomic<float>    rate_{1.0f};
};

} // namespace uav
