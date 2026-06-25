// SPDX-License-Identifier: Apache-2.0
// UnitedAV — Linux zero-copy GPU present path: headless EGL/GLES3 +
// VAAPI->dmabuf->EGLImage import + NV12->RGBA conversion shader.
#pragma once

#if defined(UAV_ENABLE_GPU) && defined(UAV_HAVE_FFMPEG) && defined(__linux__)

#include <cstdint>
#include <vector>

struct AVFrame;

namespace uav::gpu {

// Imports a VAAPI surface zero-copy and renders it to an RGBA8 texture. Not
// thread-safe: all calls must be on the thread that called init() (where the EGL
// context is made current).
class GlConverter {
public:
    GlConverter() = default;
    ~GlConverter();
    GlConverter(const GlConverter&) = delete;
    GlConverter& operator=(const GlConverter&) = delete;

    bool init(const char* render_node = nullptr);

    // Import the VAAPI `frame` zero-copy and return the RGBA8 output texture name
    // (owned by this converter; reused, do not delete). 0 on failure.
    uint32_t convert(const AVFrame* frame, int w, int h);

    // Read the last convert() output back to tightly-packed RGBA (top row first).
    bool readback(std::vector<uint8_t>& out);

    int width()  const { return out_w_; }
    int height() const { return out_h_; }

    const char* last_error() const { return err_; }

private:
    struct Plane;
    bool ensure_target(int w, int h);
    void destroy_target();
    void set_error(const char* e) { err_ = e; }

    void* dpy_ = nullptr;         // EGLDisplay
    void* ctx_ = nullptr;         // EGLContext
    void* gbm_ = nullptr;         // struct gbm_device*
    int   drm_fd_ = -1;

    unsigned program_ = 0;
    unsigned vao_ = 0;            // GLES3 requires a bound VAO
    unsigned fbo_ = 0;
    unsigned out_tex_ = 0;
    int      out_w_ = 0, out_h_ = 0;

    int  loc_texY_ = -1, loc_texUV_ = -1, loc_yuv2rgb_ = -1, loc_yoff_ = -1;

    bool initialized_ = false;
    const char* err_ = "";
};

} // namespace uav::gpu

#endif
