// SPDX-License-Identifier: Apache-2.0
#pragma once

#if defined(UAV_ENABLE_GPU) && defined(UAV_HAVE_FFMPEG) && defined(__linux__)

#include <cstdint>
#include <vector>

struct AVFrame;

namespace uav::gpu {

class VkConverter {
public:
    VkConverter() = default;
    ~VkConverter();
    VkConverter(const VkConverter&) = delete;
    VkConverter& operator=(const VkConverter&) = delete;

    bool init(const char* render_node = nullptr);

    uint64_t convert(const AVFrame* frame, int w, int h);

    bool readback(std::vector<uint8_t>& out);

    int width()  const { return out_w_; }
    int height() const { return out_h_; }

    const char* last_error() const { return err_; }

private:
    struct Impl;
    void set_error(const char* e) { err_ = e; }

    Impl* impl_ = nullptr;
    int   out_w_ = 0, out_h_ = 0;
    const char* err_ = "";
};

}

#endif
