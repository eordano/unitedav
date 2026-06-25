// SPDX-License-Identifier: Apache-2.0
#include "d3d11_convert.hpp"

#if defined(UAV_ENABLE_GPU) && defined(_WIN32)

#include <d3d11.h>
#include <dxgi.h>
#include <cstring>

namespace uav::gpu {

namespace {

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

enum { COL_SPC_BT709 = 1, COL_SPC_FCC = 4, COL_SPC_BT470BG = 5,
       COL_SPC_SMPTE170M = 6 };
enum { COL_RANGE_JPEG = 2 };

UINT ycbcr_matrix(int colorspace, int h) {
    switch (colorspace) {
        case COL_SPC_BT709:      return 1;
        case COL_SPC_BT470BG:
        case COL_SPC_SMPTE170M:
        case COL_SPC_FCC:        return 0;
        default:                 return (h <= 576) ? 0u : 1u;
    }
}

}

D3d11Converter::~D3d11Converter() {
    release(staging_); release(rgba_); release(nv12_owned_);
    release(vp_); release(vpenum_);
    release(vcontext_); release(vdevice_);
    if (owns_ctx_) release(ctx_); else ctx_ = nullptr;
    if (owns_dev_) release(dev_); else dev_ = nullptr;
}

bool D3d11Converter::init(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    if (dev_) return true;
    if (device) {
        dev_ = device; dev_->AddRef(); owns_dev_ = false;
        if (ctx) { ctx_ = ctx; ctx_->AddRef(); owns_ctx_ = false; }
        else { dev_->GetImmediateContext(&ctx_); owns_ctx_ = true; }
    } else {
        const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL got{};
        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
            for (UINT i = 0; !dev_; ++i) {
                IDXGIAdapter1* ad = nullptr;
                if (factory->EnumAdapters1(i, &ad) == DXGI_ERROR_NOT_FOUND) break;
                ID3D11Device* d = nullptr; ID3D11DeviceContext* c = nullptr;
                HRESULT hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                               want, 2, D3D11_SDK_VERSION, &d, &got, &c);
                if (SUCCEEDED(hr)) {
                    ID3D11VideoDevice* vd = nullptr;
                    if (SUCCEEDED(d->QueryInterface(__uuidof(ID3D11VideoDevice),
                                                    reinterpret_cast<void**>(&vd))) && vd) {
                        vd->Release();
                        dev_ = d; ctx_ = c; owns_dev_ = owns_ctx_ = true;
                    } else { if (c) c->Release(); d->Release(); }
                }
                ad->Release();
            }
            factory->Release();
        }
        if (!dev_) {
            HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 2,
                                           D3D11_SDK_VERSION, &dev_, &got, &ctx_);
            if (FAILED(hr)) { set_error("D3D11CreateDevice failed (no video-capable adapter)"); return false; }
            owns_dev_ = owns_ctx_ = true;
        }
    }

    if (FAILED(dev_->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(&vdevice_)))) {
        set_error("ID3D11VideoDevice unavailable"); return false;
    }
    if (FAILED(ctx_->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void**>(&vcontext_)))) {
        set_error("ID3D11VideoContext unavailable"); return false;
    }
    return true;
}

bool D3d11Converter::ensure_processor(int w, int h) {
    if (vp_ && vp_w_ == w && vp_h_ == h) return true;
    release(vp_); release(vpenum_); vp_w_ = vp_h_ = 0;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = w;  cd.InputHeight = h;
    cd.OutputWidth = w; cd.OutputHeight = h;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (FAILED(vdevice_->CreateVideoProcessorEnumerator(&cd, &vpenum_))) {
        set_error("CreateVideoProcessorEnumerator failed"); return false;
    }
    if (FAILED(vdevice_->CreateVideoProcessor(vpenum_, 0, &vp_))) {
        set_error("CreateVideoProcessor failed"); return false;
    }
    vp_w_ = w; vp_h_ = h;
    return true;
}

bool D3d11Converter::ensure_target(int w, int h) {
    if (rgba_ && out_w_ == w && out_h_ == h) return true;
    release(rgba_); out_w_ = out_h_ = 0;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &rgba_))) { set_error("CreateTexture2D(rgba) failed"); return false; }
    out_w_ = w; out_h_ = h;
    return true;
}

bool D3d11Converter::ensure_nv12_owned(int w, int h) {
    if (nv12_owned_ && nv_w_ == w && nv_h_ == h) return true;
    release(nv12_owned_); nv_w_ = nv_h_ = 0;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = 0;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &nv12_owned_))) { set_error("CreateTexture2D(nv12) failed"); return false; }
    nv_w_ = w; nv_h_ = h;
    return true;
}

bool D3d11Converter::blt(ID3D11Texture2D* nv12, int array_slice, int w, int h,
                         int colorspace, int color_range) {
    if (!ensure_processor(w, h) || !ensure_target(w, h)) return false;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.FourCC = 0;
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.MipSlice = 0;
    ivd.Texture2D.ArraySlice = (UINT)array_slice;
    ID3D11VideoProcessorInputView* iview = nullptr;
    if (FAILED(vdevice_->CreateVideoProcessorInputView(nv12, vpenum_, &ivd, &iview))) {
        set_error("CreateVideoProcessorInputView failed"); return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ovd.Texture2D.MipSlice = 0;
    ID3D11VideoProcessorOutputView* oview = nullptr;
    if (FAILED(vdevice_->CreateVideoProcessorOutputView(rgba_, vpenum_, &ovd, &oview))) {
        iview->Release(); set_error("CreateVideoProcessorOutputView failed"); return false;
    }

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs{};
    in_cs.YCbCr_Matrix = ycbcr_matrix(colorspace, h);
    in_cs.Nominal_Range = (color_range == COL_RANGE_JPEG)
        ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
        : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    vcontext_->VideoProcessorSetStreamColorSpace(vp_, 0, &in_cs);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs{};
    out_cs.RGB_Range = 0;
    vcontext_->VideoProcessorSetOutputColorSpace(vp_, &out_cs);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = iview;
    HRESULT hr = vcontext_->VideoProcessorBlt(vp_, oview, 0, 1, &stream);

    oview->Release(); iview->Release();
    if (FAILED(hr)) { set_error("VideoProcessorBlt failed"); return false; }
    return true;
}

ID3D11Texture2D* D3d11Converter::convert(ID3D11Texture2D* nv12, int array_slice,
                                         int w, int h, int colorspace, int color_range) {
    if (!dev_ || !vdevice_) { set_error("not initialized"); return nullptr; }
    if (!nv12) { set_error("null nv12 surface"); return nullptr; }
    return blt(nv12, array_slice, w, h, colorspace, color_range) ? rgba_ : nullptr;
}

ID3D11Texture2D* D3d11Converter::convert_cpu_nv12(const uint8_t* y, int ystride,
                                                  const uint8_t* uv, int uvstride,
                                                  int w, int h, int colorspace, int color_range) {
    if (!dev_ || !vdevice_) { set_error("not initialized"); return nullptr; }
    if (!y || !uv) { set_error("null nv12 planes"); return nullptr; }
    if (!ensure_nv12_owned(w, h)) return nullptr;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Texture2D* stage = nullptr;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &stage))) { set_error("CreateTexture2D(nv12 staging) failed"); return nullptr; }

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(stage, 0, D3D11_MAP_WRITE, 0, &m))) { release(stage); set_error("Map(nv12 staging) failed"); return nullptr; }
    auto* base = static_cast<uint8_t*>(m.pData);
    for (int r = 0; r < h; ++r)
        std::memcpy(base + (size_t)r * m.RowPitch, y + (size_t)r * ystride, (size_t)w);
    uint8_t* cbase = base + (size_t)m.RowPitch * h;
    for (int r = 0; r < h / 2; ++r)
        std::memcpy(cbase + (size_t)r * m.RowPitch, uv + (size_t)r * uvstride, (size_t)w);
    ctx_->Unmap(stage, 0);
    ctx_->CopyResource(nv12_owned_, stage);
    release(stage);

    return blt(nv12_owned_, 0, w, h, colorspace, color_range) ? rgba_ : nullptr;
}

bool D3d11Converter::readback(std::vector<uint8_t>& out) {
    if (!rgba_ || out_w_ <= 0 || out_h_ <= 0) { set_error("no converted frame"); return false; }
    const int w = out_w_, h = out_h_;
    if (!staging_ || st_w_ != w || st_h_ != h) {
        release(staging_); st_w_ = st_h_ = 0;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &staging_))) { set_error("CreateTexture2D(rgba staging) failed"); return false; }
        st_w_ = w; st_h_ = h;
    }
    ctx_->CopyResource(staging_, rgba_);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(staging_, 0, D3D11_MAP_READ, 0, &m))) { set_error("Map(rgba staging) failed"); return false; }
    out.assign((size_t)w * h * 4, 0);
    const auto* src = static_cast<const uint8_t*>(m.pData);
    for (int r = 0; r < h; ++r)
        std::memcpy(out.data() + (size_t)r * w * 4, src + (size_t)r * m.RowPitch, (size_t)w * 4);
    ctx_->Unmap(staging_, 0);
    return true;
}

}

#endif
