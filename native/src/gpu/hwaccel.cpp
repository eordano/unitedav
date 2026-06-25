// SPDX-License-Identifier: Apache-2.0
#include "hwaccel.hpp"

#if defined(UAV_HAVE_FFMPEG)

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace uav {

namespace {
// libavcodec get_format contract: return the HW format (stashed in ctx->opaque
// by enable()) to keep frames on the GPU, or NONE to fall back to software.
enum AVPixelFormat hw_get_format(AVCodecContext* ctx, const enum AVPixelFormat* fmts) {
    const auto want = static_cast<enum AVPixelFormat>(reinterpret_cast<intptr_t>(ctx->opaque));
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == want) return *p;
    return AV_PIX_FMT_NONE;
}
} // namespace

enum AVHWDeviceType uav_default_hw_type() {
    if (const char* env = std::getenv("UAV_HWDECODE")) {
        if (!std::strcmp(env, "none") || !std::strcmp(env, "0"))
            return AV_HWDEVICE_TYPE_NONE;
        if (std::strcmp(env, "auto") != 0 && std::strcmp(env, "1") != 0) {
            const enum AVHWDeviceType t = av_hwdevice_find_type_by_name(env);
            return t;
        }
    }
#if defined(_WIN32)
    return AV_HWDEVICE_TYPE_D3D11VA;
#elif defined(__APPLE__)
    return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#else
    return AV_HWDEVICE_TYPE_VAAPI;
#endif
}

int uav_hw_candidates(enum AVHWDeviceType* out, int max) {
    if (!out || max <= 0) return 0;
    if (const char* env = std::getenv("UAV_HWDECODE")) {
        if (!std::strcmp(env, "none") || !std::strcmp(env, "0")) return 0;
        if (std::strcmp(env, "auto") != 0 && std::strcmp(env, "1") != 0) {
            const enum AVHWDeviceType t = av_hwdevice_find_type_by_name(env);
            if (t == AV_HWDEVICE_TYPE_NONE) return 0;
            out[0] = t; return 1;
        }
    }
    int n = 0;
    auto add = [&](enum AVHWDeviceType t) { if (n < max && t != AV_HWDEVICE_TYPE_NONE) out[n++] = t; };
#if defined(_WIN32)
    add(AV_HWDEVICE_TYPE_CUDA);
    add(AV_HWDEVICE_TYPE_D3D11VA);
    add(AV_HWDEVICE_TYPE_DXVA2);
#elif defined(__APPLE__)
    add(AV_HWDEVICE_TYPE_VIDEOTOOLBOX);
#else
    add(AV_HWDEVICE_TYPE_VAAPI);
    add(AV_HWDEVICE_TYPE_CUDA);
    add(AV_HWDEVICE_TYPE_VDPAU);
#endif
    return n;
}

bool HwDecode::enable(AVCodecContext* vdec, enum AVHWDeviceType type, const char* device) {
    if (!vdec || !vdec->codec || type == AV_HWDEVICE_TYPE_NONE) return false;

    enum AVPixelFormat pix = AV_PIX_FMT_NONE;
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(vdec->codec, i);
        if (!cfg) break;
        if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
            cfg->device_type == type) { pix = cfg->pix_fmt; break; }
    }
    if (pix == AV_PIX_FMT_NONE) return false;

    if (!device) device = std::getenv("UAV_HWDEVICE");
    if (!device && type == AV_HWDEVICE_TYPE_VAAPI) device = "/dev/dri/renderD128";

    if (av_hwdevice_ctx_create(&hw_device_ctx_, type, device, nullptr, 0) < 0)
        return false;

    vdec->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
    if (!vdec->hw_device_ctx) { av_buffer_unref(&hw_device_ctx_); return false; }

    vdec->opaque     = reinterpret_cast<void*>(static_cast<intptr_t>(pix));
    vdec->get_format = hw_get_format;
    hw_pix_fmt_      = pix;
    active_          = true;
    return true;
}

bool HwDecode::is_hw_frame(const AVFrame* f) const {
    return active_ && f && f->format == hw_pix_fmt_;
}

AVFrame* HwDecode::download(const AVFrame* hw_frame, AVFrame* dst) const {
    if (!is_hw_frame(hw_frame) || !dst) return nullptr;
    av_frame_unref(dst);
    if (av_hwframe_transfer_data(dst, hw_frame, 0) < 0) return nullptr;
    av_frame_copy_props(dst, hw_frame);
    return dst;
}

bool HwDecode::to_surface(const AVFrame* hw_frame, HwSurface& out) const {
    if (!is_hw_frame(hw_frame)) return false;
    out = HwSurface{};
    out.width  = hw_frame->width;
    out.height = hw_frame->height;
    out.color_space = hw_frame->colorspace;
    out.color_range = hw_frame->color_range;
    const AVHWFramesContext* fctx = hw_frame->hw_frames_ctx
        ? reinterpret_cast<const AVHWFramesContext*>(hw_frame->hw_frames_ctx->data)
        : nullptr;
    out.fourcc = fctx ? static_cast<int32_t>(fctx->sw_format) : static_cast<int32_t>(AV_PIX_FMT_NV12);
    // data[0]/data[1] carry the platform handle (D3D11VA: texture* + array index;
    // VAAPI: surface id); the graphics backend interprets it.
    out.plane0 = hw_frame->data[0];
    out.index  = static_cast<int32_t>(reinterpret_cast<intptr_t>(hw_frame->data[1]));
    return true;
}

HwDecode::~HwDecode() {
    if (hw_device_ctx_) av_buffer_unref(&hw_device_ctx_);
}

} // namespace uav

#endif // UAV_HAVE_FFMPEG
