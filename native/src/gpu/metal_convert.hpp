// SPDX-License-Identifier: Apache-2.0
// UnitedAV — Metal NV12->RGBA GPU converter (macOS zero-copy present).
//
// The macOS analog of GlConverter (egl_vaapi.cpp) and D3d11Converter: take a
// VideoToolbox-decoded frame — a CVPixelBuffer (NV12, IOSurface-backed) — bind its
// planes as Metal textures zero-copy via a CVMetalTextureCache, convert NV12->RGBA
// with a compute kernel on the GPU, and output an RGBA MTLTexture. That texture is
// the handle C# wraps with Texture2D.CreateExternalTexture (id<MTLTexture>).
//
// Pure C++ interface (no Metal/ObjC types) so unity_render.cpp and the probe stay
// C++; the implementation lives in metal_convert.mm.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#if defined(UAV_ENABLE_GPU) && defined(__APPLE__)

namespace uav::gpu {

class MetalConverter {
public:
    MetalConverter() = default;
    ~MetalConverter();
    MetalConverter(const MetalConverter&) = delete;
    MetalConverter& operator=(const MetalConverter&) = delete;

    // Create the system default Metal device, command queue, texture cache and the
    // NV12->RGBA compute pipeline.
    bool init();

    // Convert a VideoToolbox CVPixelBuffer (passed as a CVPixelBufferRef in a
    // void*) to the RGBA target on the GPU. Returns the RGBA MTLTexture as a void*
    // (id<MTLTexture>), valid until the next convert() / destruction, or null.
    void* convert(void* cvpixelbuffer, int colorspace, int color_range);

    // Copy the RGBA target back to system memory, top row first (RGBA8). For the
    // probe / oracle comparison only — not used on the present path.
    bool readback(std::vector<uint8_t>& out_rgba);

    void* target() const;            // id<MTLTexture> as void*
    int target_width() const { return out_w_; }
    int target_height() const { return out_h_; }
    const char* last_error() const { return err_.c_str(); }

private:
    void set_error(const char* e) { err_ = e ? e : "error"; }

    struct Impl;       // holds the ObjC/Metal objects (ARC-managed in the .mm)
    Impl* impl_ = nullptr;
    int out_w_ = 0, out_h_ = 0;
    std::string err_;
};

} // namespace uav::gpu

#endif // UAV_ENABLE_GPU && __APPLE__
