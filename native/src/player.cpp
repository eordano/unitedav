// SPDX-License-Identifier: Apache-2.0
#include "player.hpp"

namespace uav {

Player::Player() = default;

Player::~Player() {
    close();
}

int32_t Player::open(const char* url) {
    url_ = url ? url : "";
    if (url_.empty()) return UAV_ERR_INVALID;

    int32_t rc = decoder_.open(url_);
    if (rc != UAV_OK) return rc;

    // Re-apply control state the caller may have set before open().
    decoder_.set_looping(looping_);
    decoder_.set_rate(rate_);
    decoder_.set_volume(volume_);
    decoder_.set_muted(muted_);
    return UAV_OK;
}

int32_t Player::close() {
    decoder_.close();
    return UAV_OK;
}

int32_t Player::play() {
    UAVState s = decoder_.state();
    if (s == UAV_STATE_IDLE || s == UAV_STATE_ERROR) return UAV_ERR_INVALID;
    decoder_.play();
    return UAV_OK;
}

int32_t Player::pause() {
    decoder_.pause();
    return UAV_OK;
}

int32_t Player::stop() {
    decoder_.stop();
    return UAV_OK;
}

int32_t Player::seek(double seconds) {
    return decoder_.seek(seconds);
}

int32_t Player::set_looping(bool loop) { looping_ = loop; decoder_.set_looping(loop); return UAV_OK; }
int32_t Player::set_rate(float rate)   { rate_ = rate;    decoder_.set_rate(rate);    return UAV_OK; }
int32_t Player::set_volume(float v)    { volume_ = v;     decoder_.set_volume(v);     return UAV_OK; }
int32_t Player::set_muted(bool muted)  { muted_ = muted;  decoder_.set_muted(muted);  return UAV_OK; }

} // namespace uav
