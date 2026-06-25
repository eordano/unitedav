// SPDX-License-Identifier: Apache-2.0
// UnitedAV — per-graphics-API GPU backend interface (DRAFT).
#pragma once

#include "unitedav_gpu.h"
#include "hwaccel.hpp"
#include <cstdint>

namespace uav::gpu {

using uav::HwSurface;

class GpuBackend {
public:
    virtual ~GpuBackend() = default;

    virtual UAVGraphicsApi api() const = 0;

    // Returns the native texture handle for CreateExternalTexture, or null on
    // failure. Bumps the player's UAVGpuTexture.version.
    virtual void* ensure_target(int32_t width, int32_t height) = 0;

    // Render-thread only.
    virtual bool convert_to_rgba(const HwSurface& src, void* rgba_target) = 0;

    virtual void* target_handle() const = 0;
};

// Build the backend matching the active Unity device; null if the active API has
// no GPU backend yet (caller falls back to CPU path).
GpuBackend* create_backend_for_active_device(/* IUnityInterfaces* */ void* unity);

// Payload handed to the render-event callback (via IssuePluginEventAndData) so the
// render thread can present a player's current decoded HW surface. The decoder
// owns the lifetimes; the pointers stay valid until the matching render event
// returns. On the GL/OpenGLCore path `converter` is a uav::gpu::GlConverter* and
// `hw_frame` is the current AV_PIX_FMT_VAAPI AVFrame*.
struct UavGpuRenderPayload {
    void*   converter = nullptr;  // GlConverter* (GL path)
    void*   hw_frame  = nullptr;  // AVFrame* (AV_PIX_FMT_VAAPI)
    int32_t width     = 0;
    int32_t height    = 0;
    int64_t frame_id  = 0;
};

// Implemented in unity_render.cpp.
int32_t graphics_api();
void*   render_event_ptr();
void    set_unity_interfaces(void* unity_interfaces);
void    teardown_unity_interfaces();
bool    acquire_published(UAVGpuTexture* out);

} // namespace uav::gpu
