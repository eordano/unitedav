# native/src/gpu — hardware decode (DRAFT)

Draft scaffold for the GPU path. **Not yet wired into CMake or the C# layer** —
these files are intentionally inert so they don't disturb the CPU path. The ABI
lives in `native/include/unitedav_gpu.h`.

## Files
| File | Role |
|------|------|
| `../include/unitedav_gpu.h` | Draft GPU C ABI: decode-mode select, device handshake, render-event ptr, `uav_gpu_acquire_texture`. |
| `unity_render.cpp` | Unity native rendering plugin: `UnityPluginLoad`, `IUnityGraphics` device events, render-thread event callback. |
| `gpu_backend.hpp` | Per-API backend interface (D3D11/Metal/Vulkan/GL): make RGBA target, convert NV12/P010→RGBA on Unity's device. |
| `hwaccel.hpp` / `hwaccel.cpp` | FFmpeg HW decode: pick hw pixel format, create/derive `AVHWDeviceContext`, expose decoded surfaces as `HwSurface`. |

## Key design invariant
The consumer-facing texture contract is **identical** to the CPU path: present an
sRGB RGBA `Texture` with `RequiresVerticalFlip() == true`. YUV→RGB happens
internally on the GPU, so consumer code is unchanged across CPU/GPU.

## Unity PluginAPI headers — NOT vendored (license)
Unity's PluginAPI headers (`IUnityInterface.h`, `IUnityGraphics.h`,
`IUnityRenderingExtensions.h`, …) ship under the **Unity Companion License**,
which only grants use in Unity-dependent contexts and is not redistributable under
this project's Apache-2.0. They are therefore **not vendored**. To build the live
Unity rendering plugin (`unity_render.cpp`), point the build at a local Unity
install via `-DUAV_UNITY_PLUGIN_API_DIR=<dir>`:
- macOS: `<Unity>.app/Contents/Resources/PluginAPI`
- Linux/Windows: `<Unity>/Editor/Data/PluginAPI`

Unset → `unity_render.cpp` compiles to an inert stub (C ABI symbols only), so the
default OSS build is unaffected.

## Wiring steps
1. `CMakeLists.txt`: `option(UAV_ENABLE_GPU ...)` + `UAV_UNITY_PLUGIN_API_DIR`
   path; when ON, add the gpu/ sources + (if the dir is set) the Unity include dir
   and `UAV_HAVE_UNITY_PLUGIN_API`; per-platform graphics libs (d3d11/dxgi,
   Metal/CoreVideo, va/EGL/Vulkan). **Done** for the GL/OpenGLCore path.
2. `unity_render.cpp`: `UnityPluginLoad`/`UnityPluginUnload`,
   `IUnityGraphics` device events (resolve active renderer → `UAVGraphicsApi`),
   and a `UnityRenderingEventAndData` render-event callback that presents the
   decoded GL texture zero-copy. **Done** for OpenGLCore/OpenGLES (the testable
   path on Linux); the C# side wraps the published GL texture name with
   `Texture2D.CreateExternalTexture` and re-wraps on `version` bump.
3. Next backend: **D3D11** (`gpu/d3d11_backend.cpp`) +
   D3D11VA zero-copy via Unity's `ID3D11Device`; NV12→RGBA pixel shader.
4. Extend the decoder (`decode/decoder.cpp`) to try `HwDecode::enable()` when
   decode mode is GPU/AUTO, keep a GPU frame queue, and fall back to software on
   failure — reusing the same worker/clock/state machine.
5. C# (`unity/`): `uav_gpu_set_unity_interfaces` from a load hook;
   per-frame `CommandBuffer.IssuePluginEventAndData(uav_gpu_render_event(), …)`;
   wrap `uav_gpu_acquire_texture` handle with `Texture2D.CreateExternalTexture`,
   re-wrap on `version` change; expose via `ITextureProducer`.
6. Validate GPU output **against the CPU path as the oracle** (pixel diff), then
   per platform: D3D11 → VideoToolbox → VAAPI.
