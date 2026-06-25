// SPDX-License-Identifier: Apache-2.0
#include "metal_convert.hpp"

#if defined(UAV_ENABLE_GPU) && defined(__APPLE__)

#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <cstring>

namespace uav::gpu {

namespace {

// Mirrors FFmpeg's AVColorSpace / AVColorRange so this file stays FFmpeg-free.
enum { COL_SPC_BT709 = 1, COL_SPC_FCC = 4, COL_SPC_BT470BG = 5, COL_SPC_SMPTE170M = 6 };
enum { COL_RANGE_JPEG = 2 };  // full range

struct Kr_Kb { float kr, kb; };
Kr_Kb coeffs_for(int colorspace, int h) {
    switch (colorspace) {
        case COL_SPC_BT709:      return {0.2126f, 0.0722f};
        case COL_SPC_BT470BG:
        case COL_SPC_SMPTE170M:
        case COL_SPC_FCC:        return {0.299f, 0.114f};
        default:                 return (h <= 576) ? Kr_Kb{0.299f, 0.114f}
                                                   : Kr_Kb{0.2126f, 0.0722f};
    }
}

// 16-byte-aligned rows to match the kernel's `float3` constant fields.
struct MtlCM { float m0[4]; float m1[4]; float m2[4]; float yoff[4]; };

MtlCM make_cm(Kr_Kb c, bool full_range) {
    const float kr = c.kr, kb = c.kb, kg = 1.0f - kr - kb;
    float ys, yb, cs;
    if (full_range) { ys = 1.0f; yb = 0.0f; cs = 1.0f; }
    else { ys = 255.0f / (235.0f - 16.0f); yb = 16.0f / 255.0f; cs = 255.0f / (240.0f - 16.0f); }
    const float r_cr = cs * (2.0f - 2.0f * kr);
    const float b_cb = cs * (2.0f - 2.0f * kb);
    const float g_cb = -cs * (2.0f * kb * (1.0f - kb) / kg);
    const float g_cr = -cs * (2.0f * kr * (1.0f - kr) / kg);
    MtlCM m{};
    m.m0[0] = ys; m.m0[1] = 0.0f; m.m0[2] = r_cr;  // R = ys*Y' + r_cr*Cr'
    m.m1[0] = ys; m.m1[1] = g_cb; m.m1[2] = g_cr;  // G = ys*Y' + g_cb*Cb' + g_cr*Cr'
    m.m2[0] = ys; m.m2[1] = b_cb; m.m2[2] = 0.0f;  // B = ys*Y' + b_cb*Cb'
    m.yoff[0] = yb; m.yoff[1] = 0.5f; m.yoff[2] = 0.5f;
    return m;
}

const char* kKernel = R"(
#include <metal_stdlib>
using namespace metal;
struct CM { float3 m0; float3 m1; float3 m2; float3 yoff; };
kernel void nv12_to_rgba(
    texture2d<float, access::sample> texY    [[texture(0)]],
    texture2d<float, access::sample> texCbCr [[texture(1)]],
    texture2d<float, access::write>  outTex  [[texture(2)]],
    constant CM& cm [[buffer(0)]],
    uint2 gid [[thread_position_in_grid]]) {
    uint w = outTex.get_width();
    uint h = outTex.get_height();
    if (gid.x >= w || gid.y >= h) return;
    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
    float2 uv = (float2(gid) + float2(0.5)) / float2(w, h);   // uv.y=0 -> image top
    float  y    = texY.sample(s, uv).r;
    float2 cbcr = texCbCr.sample(s, uv).rg;
    float3 ycc = float3(y, cbcr.x, cbcr.y) - cm.yoff;
    float3 rgb;
    rgb.r = dot(cm.m0, ycc);
    rgb.g = dot(cm.m1, ycc);
    rgb.b = dot(cm.m2, ycc);
    outTex.write(float4(clamp(rgb, 0.0, 1.0), 1.0), gid);
}
)";

} // namespace

struct MetalConverter::Impl {
    id<MTLDevice>               device  = nil;
    id<MTLCommandQueue>         queue   = nil;
    id<MTLComputePipelineState> pipeline = nil;
    id<MTLTexture>              outTex  = nil;
    CVMetalTextureCacheRef      cache   = nullptr;
    int out_w = 0, out_h = 0;
    ~Impl() { if (cache) CFRelease(cache); }
};

MetalConverter::~MetalConverter() { delete impl_; impl_ = nullptr; }

bool MetalConverter::init() {
    if (impl_) return true;
    @autoreleasepool {
        Impl* d = new Impl();
        d->device = MTLCreateSystemDefaultDevice();
        if (!d->device) { delete d; set_error("MTLCreateSystemDefaultDevice failed"); return false; }
        d->queue = [d->device newCommandQueue];
        if (!d->queue) { delete d; set_error("newCommandQueue failed"); return false; }
        if (CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, d->device, nullptr, &d->cache) != kCVReturnSuccess) {
            delete d; set_error("CVMetalTextureCacheCreate failed"); return false;
        }
        NSError* err = nil;
        id<MTLLibrary> lib = [d->device newLibraryWithSource:@(kKernel) options:nil error:&err];
        if (!lib) { std::string m = err ? err.localizedDescription.UTF8String : "newLibraryWithSource failed";
                    delete d; set_error(m.c_str()); return false; }
        id<MTLFunction> fn = [lib newFunctionWithName:@"nv12_to_rgba"];
        if (!fn) { delete d; set_error("kernel function not found"); return false; }
        d->pipeline = [d->device newComputePipelineStateWithFunction:fn error:&err];
        if (!d->pipeline) { std::string m = err ? err.localizedDescription.UTF8String : "pipeline failed";
                            delete d; set_error(m.c_str()); return false; }
        impl_ = d;
    }
    return true;
}

void* MetalConverter::convert(void* cvpixelbuffer, int colorspace, int color_range) {
    if (!impl_) { set_error("not initialized"); return nullptr; }
    CVPixelBufferRef pb = static_cast<CVPixelBufferRef>(cvpixelbuffer);
    if (!pb) { set_error("null CVPixelBuffer"); return nullptr; }

    @autoreleasepool {
        const size_t yw = CVPixelBufferGetWidthOfPlane(pb, 0);
        const size_t yh = CVPixelBufferGetHeightOfPlane(pb, 0);
        const size_t cw = CVPixelBufferGetWidthOfPlane(pb, 1);
        const size_t ch = CVPixelBufferGetHeightOfPlane(pb, 1);
        const int w = (int)yw, h = (int)yh;

        CVMetalTextureRef cvY = nullptr, cvC = nullptr;
        if (CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, impl_->cache, pb, nullptr,
                MTLPixelFormatR8Unorm, yw, yh, 0, &cvY) != kCVReturnSuccess) {
            set_error("CVMetalTextureCache (Y) failed"); return nullptr;
        }
        if (CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, impl_->cache, pb, nullptr,
                MTLPixelFormatRG8Unorm, cw, ch, 1, &cvC) != kCVReturnSuccess) {
            CFRelease(cvY); set_error("CVMetalTextureCache (CbCr) failed"); return nullptr;
        }
        id<MTLTexture> texY = CVMetalTextureGetTexture(cvY);
        id<MTLTexture> texC = CVMetalTextureGetTexture(cvC);

        if (!impl_->outTex || impl_->out_w != w || impl_->out_h != h) {
            MTLTextureDescriptor* td =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                   width:w height:h mipmapped:NO];
            td.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModeShared;   // Apple Silicon: CPU-readable for the probe
            impl_->outTex = [impl_->device newTextureWithDescriptor:td];
            impl_->out_w = w; impl_->out_h = h;
            out_w_ = w; out_h_ = h;
        }

        MtlCM cm = make_cm(coeffs_for(colorspace, h), color_range == COL_RANGE_JPEG);

        id<MTLCommandBuffer> cb = [impl_->queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:impl_->pipeline];
        [enc setTexture:texY atIndex:0];
        [enc setTexture:texC atIndex:1];
        [enc setTexture:impl_->outTex atIndex:2];
        [enc setBytes:&cm length:sizeof(cm) atIndex:0];
        MTLSize tg = MTLSizeMake(16, 16, 1);
        MTLSize grid = MTLSizeMake((w + 15) / 16, (h + 15) / 16, 1);
        [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];

        CFRelease(cvY); CFRelease(cvC);
        CVMetalTextureCacheFlush(impl_->cache, 0);

        if (cb.status == MTLCommandBufferStatusError) { set_error("compute command buffer error"); return nullptr; }
        err_ = "";
        return (__bridge void*)impl_->outTex;
    }
}

bool MetalConverter::readback(std::vector<uint8_t>& out) {
    if (!impl_ || !impl_->outTex || out_w_ <= 0 || out_h_ <= 0) { set_error("no converted frame"); return false; }
    const int w = out_w_, h = out_h_;
    out.assign((size_t)w * h * 4, 0);
    @autoreleasepool {
        [impl_->outTex getBytes:out.data()
                    bytesPerRow:(NSUInteger)w * 4
                     fromRegion:MTLRegionMake2D(0, 0, w, h)
                    mipmapLevel:0];
    }
    return true;
}

void* MetalConverter::target() const {
    return impl_ ? (__bridge void*)impl_->outTex : nullptr;
}

} // namespace uav::gpu

#endif // UAV_ENABLE_GPU && __APPLE__
