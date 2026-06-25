// SPDX-License-Identifier: Apache-2.0
/*
 * UnitedAV — GPU / hardware-decode ABI additions (DRAFT). Not yet wired into
 * CMake or the C# layer; the CPU ABI in unitedav.h remains the supported path.
 */
#ifndef UNITEDAV_GPU_H
#define UNITEDAV_GPU_H

#include "unitedav.h"   /* UAVPlayer, UAV_API, UAV_CALL, UAVResult, UAVPixelFormat */

#ifdef __cplusplus
extern "C" {
#endif

#define UAV_GPU_ABI_VERSION 1

/* AUTO = HW if available, else CPU. */
typedef enum UAVDecodeMode {
    UAV_DECODE_AUTO = 0,
    UAV_DECODE_CPU  = 1,
    UAV_DECODE_GPU  = 2
} UAVDecodeMode;

typedef enum UAVGraphicsApi {
    UAV_GFX_UNKNOWN = 0,
    UAV_GFX_D3D11   = 1,
    UAV_GFX_D3D12   = 2,
    UAV_GFX_METAL   = 3,
    UAV_GFX_VULKAN  = 4,
    UAV_GFX_OPENGL  = 5
} UAVGraphicsApi;

/* `native_handle` is the platform texture wrapped by Texture2D.CreateExternalTexture:
 *   D3D11 -> ID3D11Texture2D*, D3D12 -> ID3D12Resource*, Metal -> id<MTLTexture>,
 *   Vulkan -> VkImage (as uintptr), OpenGL -> GLuint name (as uintptr).
 * `version` bumps whenever native_handle changes so C# knows to re-wrap. */
typedef struct UAVGpuTexture {
    void*   native_handle;
    int32_t width;
    int32_t height;
    int32_t format;     /* UAVPixelFormat */
    int64_t version;
    int64_t frame_id;   /* monotonic */
} UAVGpuTexture;

/* Called from UnityPluginLoad(IUnityInterfaces*); pass the same pointer Unity
 * gives the plugin. */
UAV_API void UAV_CALL uav_gpu_set_unity_interfaces(void* unity_interfaces);

UAV_API int32_t UAV_CALL uav_gpu_graphics_api(void); /* UAVGraphicsApi */

/* Returns a UnityRenderingEventAndData fn pointer (void (*)(int, void*)) for
 * CommandBuffer.IssuePluginEventAndData; `data` must be the UAVPlayer*. NULL if
 * the GPU path is unavailable. */
UAV_API void* UAV_CALL uav_gpu_render_event(void);

/* Call before uav_open. UAV_ERR_UNSUPPORTED if GPU requested but no device. */
UAV_API int32_t UAV_CALL uav_set_decode_mode(UAVPlayer* p, int32_t mode);

/* What this player resolved to after open (AUTO -> CPU or GPU). */
UAV_API int32_t UAV_CALL uav_get_decode_mode(UAVPlayer* p); /* UAVDecodeMode */

/* UAV_OK -> `out` filled; UAV_ERR_NO_STREAM -> nothing new; UAV_ERR_UNSUPPORTED
 * -> CPU path (use uav_acquire_frame). The texture stays valid until the next
 * render-event for this player, so the native side must not destroy a published
 * texture without bumping `version` first. */
UAV_API int32_t UAV_CALL uav_gpu_acquire_texture(UAVPlayer* p, UAVGpuTexture* out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* UNITEDAV_GPU_H */
