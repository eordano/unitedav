// SPDX-License-Identifier: Apache-2.0
// UnitedAV — FFmpeg hardware-decode helper (graphics-API-independent).
#pragma once

#include <cstdint>

#if defined(UAV_HAVE_FFMPEG)

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/hwcontext.h>
}
struct AVCodecContext;
struct AVFrame;
struct AVBufferRef;

namespace uav {

// Platform-tagged handle to a decoded HW frame surface (graphics-API-free).
struct HwSurface {
    void*   plane0 = nullptr; // ID3D11Texture2D* / CVPixelBufferRef / VASurface / VkImage
    void*   plane1 = nullptr;
    int32_t index  = 0;       // texture-array slice (D3D11VA) else 0
    int32_t fourcc = 0;       // AV_PIX_FMT_* of the HW frame's sw_format
    int32_t width = 0, height = 0;
    int32_t color_space = 0;  // AVColorSpace
    int32_t color_range = 0;  // AVColorRange
};

// Platform-default hw device type, honoring UAV_HWDECODE ("none" disables; a
// named type forces it; unset/"auto" -> platform default).
enum AVHWDeviceType uav_default_hw_type();

// Ordered HW device types to try for auto decode, most-preferred first. Writes up
// to `max` entries to `out`; returns the count.
int uav_hw_candidates(enum AVHWDeviceType* out, int max);

class HwDecode {
public:
    HwDecode() = default;
    ~HwDecode();
    HwDecode(const HwDecode&) = delete;
    HwDecode& operator=(const HwDecode&) = delete;

    // MUST be called AFTER avcodec_parameters_to_context and BEFORE avcodec_open2.
    // Returns false (leaving `vdec` untouched for software decode) if the codec
    // has no matching HW config or the device can't be created.
    bool enable(AVCodecContext* vdec, enum AVHWDeviceType type, const char* device);

    bool active() const { return active_; }
    enum AVPixelFormat hw_pix_fmt() const { return hw_pix_fmt_; }

    bool is_hw_frame(const AVFrame* f) const;

    // Download a HW frame to system memory into `dst` (reused across calls).
    AVFrame* download(const AVFrame* hw_frame, AVFrame* dst) const;

    bool to_surface(const AVFrame* hw_frame, HwSurface& out) const;

private:
    AVBufferRef*       hw_device_ctx_ = nullptr;
    enum AVPixelFormat hw_pix_fmt_    = AV_PIX_FMT_NONE;
    bool               active_        = false;
};

} // namespace uav

#endif // UAV_HAVE_FFMPEG
