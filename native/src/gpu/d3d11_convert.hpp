// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#if defined(UAV_ENABLE_GPU) && defined(_WIN32)

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11VideoDevice;
struct ID3D11VideoContext;
struct ID3D11VideoProcessor;
struct ID3D11VideoProcessorEnumerator;

namespace uav::gpu {

class D3d11Converter {
public:
    D3d11Converter() = default;
    ~D3d11Converter();
    D3d11Converter(const D3d11Converter&) = delete;
    D3d11Converter& operator=(const D3d11Converter&) = delete;

    bool init(ID3D11Device* device = nullptr, ID3D11DeviceContext* ctx = nullptr);

    ID3D11Texture2D* convert(ID3D11Texture2D* nv12, int array_slice,
                             int w, int h, int colorspace, int color_range);

    ID3D11Texture2D* convert_cpu_nv12(const uint8_t* y, int ystride,
                                      const uint8_t* uv, int uvstride,
                                      int w, int h, int colorspace, int color_range);

    bool readback(std::vector<uint8_t>& out_rgba);

    ID3D11Texture2D* target() const { return rgba_; }
    int target_width() const { return out_w_; }
    int target_height() const { return out_h_; }
    ID3D11Device* device() const { return dev_; }
    const char* last_error() const { return err_.c_str(); }

private:
    bool ensure_processor(int w, int h);
    bool ensure_target(int w, int h);
    bool ensure_nv12_owned(int w, int h);
    bool blt(ID3D11Texture2D* nv12, int array_slice, int w, int h,
             int colorspace, int color_range);
    void set_error(const char* e) { err_ = e ? e : "error"; }

    ID3D11Device*        dev_ = nullptr;  bool owns_dev_ = false;
    ID3D11DeviceContext* ctx_ = nullptr;  bool owns_ctx_ = false;

    ID3D11VideoDevice*   vdevice_  = nullptr;
    ID3D11VideoContext*  vcontext_ = nullptr;
    ID3D11VideoProcessorEnumerator* vpenum_ = nullptr;
    ID3D11VideoProcessor* vp_ = nullptr;
    int vp_w_ = 0, vp_h_ = 0;

    ID3D11Texture2D* rgba_ = nullptr;  int out_w_ = 0, out_h_ = 0;
    ID3D11Texture2D* nv12_owned_ = nullptr;  int nv_w_ = 0, nv_h_ = 0;
    ID3D11Texture2D* staging_ = nullptr;     int st_w_ = 0, st_h_ = 0;

    std::string err_;
};

}

#endif
