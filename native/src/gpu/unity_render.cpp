// SPDX-License-Identifier: Apache-2.0
#include "unitedav_gpu.h"

#if defined(UAV_ENABLE_GPU)

#if defined(UAV_HAVE_UNITY_PLUGIN_API)

#include "IUnityInterface.h"
#include "IUnityGraphics.h"
#include "IUnityRenderingExtensions.h"

#include "gpu_backend.hpp"

#if defined(UAV_HAVE_FFMPEG) && defined(__linux__)
#include "egl_vaapi.hpp"
extern "C" {
#include <libavutil/frame.h>
}
#elif defined(UAV_HAVE_FFMPEG) && defined(_WIN32)
#include "d3d11_convert.hpp"
#include <d3d11.h>
extern "C" {
#include <libavutil/frame.h>
}
#elif defined(UAV_HAVE_FFMPEG) && defined(__APPLE__)
#include "metal_convert.hpp"
extern "C" {
#include <libavutil/frame.h>
}
#endif

#include <atomic>
#include <cstdint>
#include <mutex>

namespace uav::gpu {

namespace {

IUnityInterfaces*    s_unity    = nullptr;
IUnityGraphics*      s_graphics = nullptr;
std::atomic<int32_t> s_api{UAV_GFX_UNKNOWN};

std::mutex   s_tex_mutex;
UAVGpuTexture s_published{};

int32_t to_uav_api(UnityGfxRenderer r) {
    switch (r) {
        case kUnityGfxRendererOpenGLCore:
        case kUnityGfxRendererOpenGLES30: return UAV_GFX_OPENGL;
        case kUnityGfxRendererD3D11:      return UAV_GFX_D3D11;
        case kUnityGfxRendererD3D12:      return UAV_GFX_D3D12;
        case kUnityGfxRendererMetal:      return UAV_GFX_METAL;
        case kUnityGfxRendererVulkan:     return UAV_GFX_VULKAN;
        default:                          return UAV_GFX_UNKNOWN;
    }
}

void publish(void* handle, int32_t w, int32_t h, int64_t frame_id) {
    std::lock_guard<std::mutex> lk(s_tex_mutex);
    if (handle != s_published.native_handle ||
        w != s_published.width || h != s_published.height) {
        s_published.version += 1;
    }
    s_published.native_handle = handle;
    s_published.width  = w;
    s_published.height = h;
    s_published.format = 0;
    s_published.frame_id = frame_id;
}

void UNITY_INTERFACE_API on_graphics_device_event(UnityGfxDeviceEventType eventType) {
    switch (eventType) {
        case kUnityGfxDeviceEventInitialize:
        case kUnityGfxDeviceEventAfterReset:
            if (s_graphics) s_api.store(to_uav_api(s_graphics->GetRenderer()));
            break;
        case kUnityGfxDeviceEventShutdown:
            s_api.store(UAV_GFX_UNKNOWN);
            break;
        default:
            break;
    }
}

void UNITY_INTERFACE_API on_render_event(int eventId, void* data) {
    (void)eventId;
    if (!data) return;
    const int32_t api = s_api.load();

#if defined(UAV_HAVE_FFMPEG) && defined(__linux__)
    if (api != UAV_GFX_OPENGL) return;
    auto* payload = static_cast<UavGpuRenderPayload*>(data);
    if (!payload || !payload->converter || !payload->hw_frame) return;

    auto* conv  = static_cast<GlConverter*>(payload->converter);
    auto* frame = static_cast<AVFrame*>(payload->hw_frame);
    const uint32_t tex = conv->convert(frame, payload->width, payload->height);
    if (tex)
        publish(reinterpret_cast<void*>(static_cast<uintptr_t>(tex)),
                payload->width, payload->height, payload->frame_id);
#elif defined(UAV_HAVE_FFMPEG) && defined(_WIN32)
    if (api != UAV_GFX_D3D11) return;
    auto* payload = static_cast<UavGpuRenderPayload*>(data);
    if (!payload || !payload->converter || !payload->hw_frame) return;

    auto* conv  = static_cast<D3d11Converter*>(payload->converter);
    auto* frame = static_cast<AVFrame*>(payload->hw_frame);
    auto* tex   = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    const int slice = static_cast<int>(reinterpret_cast<intptr_t>(frame->data[1]));
    ID3D11Texture2D* rgba = conv->convert(tex, slice, payload->width, payload->height,
                                          frame->colorspace, frame->color_range);
    if (rgba)
        publish(reinterpret_cast<void*>(rgba),
                payload->width, payload->height, payload->frame_id);
#elif defined(UAV_HAVE_FFMPEG) && defined(__APPLE__)
    if (api != UAV_GFX_METAL) return;
    auto* payload = static_cast<UavGpuRenderPayload*>(data);
    if (!payload || !payload->converter || !payload->hw_frame) return;

    auto* conv  = static_cast<MetalConverter*>(payload->converter);
    auto* frame = static_cast<AVFrame*>(payload->hw_frame);
    void* mtl = conv->convert(reinterpret_cast<void*>(frame->data[3]),
                              frame->colorspace, frame->color_range);
    if (mtl)
        publish(mtl, payload->width, payload->height, payload->frame_id);
#else
    (void)api; (void)data;
#endif
}

}

int32_t graphics_api() { return s_api.load(); }
void*   render_event_ptr() { return reinterpret_cast<void*>(&on_render_event); }

void set_unity_interfaces(void* unity_interfaces) {
    s_unity = static_cast<IUnityInterfaces*>(unity_interfaces);
    if (!s_unity) {
        s_graphics = nullptr;
        s_api.store(UAV_GFX_UNKNOWN);
        return;
    }
    s_graphics = s_unity->Get<IUnityGraphics>();
    if (s_graphics) {
        s_graphics->RegisterDeviceEventCallback(on_graphics_device_event);
        on_graphics_device_event(kUnityGfxDeviceEventInitialize);
    }
}

void teardown_unity_interfaces() {
    if (s_graphics)
        s_graphics->UnregisterDeviceEventCallback(on_graphics_device_event);
    s_graphics = nullptr;
    s_unity = nullptr;
    s_api.store(UAV_GFX_UNKNOWN);
}

bool acquire_published(UAVGpuTexture* out) {
    if (!out) return false;
    std::lock_guard<std::mutex> lk(s_tex_mutex);
    if (!s_published.native_handle) return false;
    *out = s_published;
    return true;
}

}

extern "C" {

void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
UnityPluginLoad(IUnityInterfaces* unityInterfaces) {
    uav::gpu::set_unity_interfaces(unityInterfaces);
}

void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
UnityPluginUnload() {
    uav::gpu::teardown_unity_interfaces();
}

}

#else

#include <atomic>

namespace uav::gpu {
namespace {
std::atomic<int32_t> s_api{UAV_GFX_UNKNOWN};
void on_render_event(int, void*) {}
}
int32_t graphics_api() { return s_api.load(); }
void*   render_event_ptr() { return reinterpret_cast<void*>(&on_render_event); }
void    set_unity_interfaces(void*) {}
void    teardown_unity_interfaces() {}
bool    acquire_published(UAVGpuTexture*) { return false; }
}

#endif

extern "C" {

UAV_API void UAV_CALL uav_gpu_set_unity_interfaces(void* unity_interfaces) {
    uav::gpu::set_unity_interfaces(unity_interfaces);
}
UAV_API int32_t UAV_CALL uav_gpu_graphics_api(void) {
    return uav::gpu::graphics_api();
}
UAV_API void* UAV_CALL uav_gpu_render_event(void) {
    return uav::gpu::render_event_ptr();
}

}

#endif
