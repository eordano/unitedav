// SPDX-License-Identifier: Apache-2.0
#include "egl_vaapi.hpp"

#if defined(UAV_ENABLE_GPU) && defined(UAV_HAVE_FFMPEG) && defined(__linux__)

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixfmt.h>
}

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <fcntl.h>
#include <unistd.h>
#include <gbm.h>
#include <drm_fourcc.h>

#include <cstdio>
#include <cstring>

namespace uav::gpu {

namespace {

PFNEGLGETPLATFORMDISPLAYEXTPROC      p_eglGetPlatformDisplayEXT = nullptr;
PFNEGLCREATEIMAGEKHRPROC             p_eglCreateImageKHR = nullptr;
PFNEGLDESTROYIMAGEKHRPROC            p_eglDestroyImageKHR = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC  p_glEGLImageTargetTexture2DOES = nullptr;

bool load_egl_procs() {
    p_eglGetPlatformDisplayEXT = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
        eglGetProcAddress("eglGetPlatformDisplayEXT");
    p_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)
        eglGetProcAddress("eglCreateImageKHR");
    p_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)
        eglGetProcAddress("eglDestroyImageKHR");
    p_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    return p_eglCreateImageKHR && p_eglDestroyImageKHR &&
           p_glEGLImageTargetTexture2DOES;
}

GLuint compile(GLenum type, const char* src, const char** err) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        static char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[uav-gpu] shader compile error: %s\n", log);
        if (err) *err = "shader compile failed";
        glDeleteShader(s);
        return 0;
    }
    return s;
}

// Fullscreen-triangle vertex shader; v_uv has the image top-left at (0,0). See
// readback() for the orientation contract.
const char* kVS = R"(#version 300 es
out vec2 v_uv;
void main() {
    vec2 pos = vec2(float((gl_VertexID & 1) << 2) - 1.0,
                    float((gl_VertexID & 2) << 1) - 1.0);
    v_uv = vec2((pos.x + 1.0) * 0.5, (pos.y + 1.0) * 0.5);
    gl_Position = vec4(pos, 0.0, 1.0);
}
)";

const char* kFS = R"(#version 300 es
precision highp float;
in  vec2 v_uv;
out vec4 o_color;
uniform sampler2D u_texY;
uniform sampler2D u_texUV;
uniform mat3      u_yuv2rgb;
uniform vec3      u_yoff;
void main() {
    float y  = texture(u_texY,  v_uv).r;
    vec2  uv = texture(u_texUV, v_uv).rg;
    vec3  ycc = vec3(y, uv.x, uv.y) - u_yoff;
    vec3  rgb = u_yuv2rgb * ycc;
    o_color = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";

struct Kr_Kb { float kr, kb; };
Kr_Kb coeffs_for(int colorspace, int h) {
    switch (colorspace) {
        case AVCOL_SPC_BT709:      return {0.2126f, 0.0722f};
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
        case AVCOL_SPC_FCC:        return {0.299f, 0.114f};
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:  return {0.2627f, 0.0593f};
        case AVCOL_SPC_UNSPECIFIED:
        default:
            // swscale's heuristic: SD (<=576 lines) -> BT.601, else BT.709.
            return (h <= 576) ? Kr_Kb{0.299f, 0.114f} : Kr_Kb{0.2126f, 0.0722f};
    }
}

// Build the 3x3 YUV->RGB matrix (column-major for glUniformMatrix3fv) and the
// pre-subtract offset. Normalized [0,1] in/out.
void make_matrix(Kr_Kb c, bool full_range, float mat_colmajor[9], float yoff[3]) {
    const float kr = c.kr, kb = c.kb, kg = 1.0f - kr - kb;

    float ys, yb, cs;
    if (full_range) {
        ys = 1.0f; yb = 0.0f; cs = 1.0f;
    } else {
        ys = 255.0f / (235.0f - 16.0f);
        yb = 16.0f / 255.0f;
        cs = 255.0f / (240.0f - 16.0f);
    }
    yoff[0] = yb; yoff[1] = 0.5f; yoff[2] = 0.5f;

    const float r_cr = cs * (2.0f - 2.0f * kr);
    const float b_cb = cs * (2.0f - 2.0f * kb);
    const float g_cb = -cs * (2.0f * kb * (1.0f - kb) / kg);
    const float g_cr = -cs * (2.0f * kr * (1.0f - kr) / kg);

    // glUniformMatrix3fv is column-major: m[col*3 + row].
    mat_colmajor[0] = ys;   mat_colmajor[1] = ys;   mat_colmajor[2] = ys;
    mat_colmajor[3] = 0.0f; mat_colmajor[4] = g_cb; mat_colmajor[5] = b_cb;
    mat_colmajor[6] = r_cr; mat_colmajor[7] = g_cr; mat_colmajor[8] = 0.0f;
}

uint32_t plane_fourcc(int plane_index) {
    return plane_index == 0 ? DRM_FORMAT_R8 : DRM_FORMAT_GR88;
}

} // namespace

struct GlConverter::Plane {
    EGLImageKHR img = EGL_NO_IMAGE_KHR;
    GLuint      tex = 0;
};

GlConverter::~GlConverter() {
    // Make the context current so GL deletes hit the right context.
    if (dpy_ && ctx_) {
        eglMakeCurrent(static_cast<EGLDisplay>(dpy_), EGL_NO_SURFACE,
                       EGL_NO_SURFACE, static_cast<EGLContext>(ctx_));
    }
    destroy_target();
    if (program_) { glDeleteProgram(program_); program_ = 0; }
    if (vao_)     { glDeleteVertexArrays(1, &vao_); vao_ = 0; }

    if (dpy_) {
        EGLDisplay d = static_cast<EGLDisplay>(dpy_);
        eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (ctx_) eglDestroyContext(d, static_cast<EGLContext>(ctx_));
        eglTerminate(d);
    }
    ctx_ = nullptr; dpy_ = nullptr;
    if (gbm_) { gbm_device_destroy(static_cast<struct gbm_device*>(gbm_)); gbm_ = nullptr; }
    if (drm_fd_ >= 0) { close(drm_fd_); drm_fd_ = -1; }
}

bool GlConverter::init(const char* render_node) {
    if (initialized_) return true;
    if (!render_node) render_node = "/dev/dri/renderD128";

    drm_fd_ = open(render_node, O_RDWR | O_CLOEXEC);
    if (drm_fd_ < 0) { set_error("open(render node) failed"); return false; }
    gbm_ = gbm_create_device(drm_fd_);
    if (!gbm_) { set_error("gbm_create_device failed"); return false; }

    p_eglGetPlatformDisplayEXT = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
        eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = EGL_NO_DISPLAY;
    if (p_eglGetPlatformDisplayEXT) {
        dpy = p_eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm_, nullptr);
        if (dpy == EGL_NO_DISPLAY)
            dpy = p_eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA,
                                             EGL_DEFAULT_DISPLAY, nullptr);
    }
    if (dpy == EGL_NO_DISPLAY) { set_error("eglGetPlatformDisplay failed"); return false; }
    dpy_ = dpy;

    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) { set_error("eglInitialize failed"); return false; }

    const char* exts = eglQueryString(dpy, EGL_EXTENSIONS);
    if (!exts || !std::strstr(exts, "EGL_EXT_image_dma_buf_import")) {
        set_error("EGL_EXT_image_dma_buf_import missing"); return false;
    }
    if (!exts || !std::strstr(exts, "EGL_KHR_surfaceless_context")) {
        set_error("EGL_KHR_surfaceless_context missing"); return false;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) { set_error("eglBindAPI(GLES) failed"); return false; }
    if (!load_egl_procs()) { set_error("EGL/GL dmabuf-import procs missing"); return false; }

    // A config is needed even for a surfaceless context with most drivers.
    const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg = nullptr; EGLint ncfg = 0;
    if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg < 1) {
        cfg = (EGLConfig)EGL_NO_CONFIG_KHR;
    }

    const EGLint ctx_attrs[] = { EGL_CONTEXT_MAJOR_VERSION, 3,
                                 EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT) { set_error("eglCreateContext failed"); return false; }
    ctx_ = ctx;

    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        set_error("eglMakeCurrent(surfaceless) failed"); return false;
    }

    const char* cerr = nullptr;
    GLuint vs = compile(GL_VERTEX_SHADER, kVS, &cerr);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFS, &cerr);
    if (!vs || !fs) { if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs);
                      set_error(cerr ? cerr : "shader compile failed"); return false; }
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (!linked) { set_error("program link failed"); return false; }

    loc_texY_    = glGetUniformLocation(program_, "u_texY");
    loc_texUV_   = glGetUniformLocation(program_, "u_texUV");
    loc_yuv2rgb_ = glGetUniformLocation(program_, "u_yuv2rgb");
    loc_yoff_    = glGetUniformLocation(program_, "u_yoff");

    glGenVertexArrays(1, &vao_);  // GLES3 core requires a bound VAO to draw

    initialized_ = true;
    err_ = "";
    return true;
}

bool GlConverter::ensure_target(int w, int h) {
    if (out_tex_ && out_w_ == w && out_h_ == h) return true;
    destroy_target();
    glGenTextures(1, &out_tex_);
    glBindTexture(GL_TEXTURE_2D, out_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out_tex_, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) { destroy_target(); set_error("FBO incomplete"); return false; }
    out_w_ = w; out_h_ = h;
    return true;
}

void GlConverter::destroy_target() {
    if (fbo_)     { glDeleteFramebuffers(1, &fbo_); fbo_ = 0; }
    if (out_tex_) { glDeleteTextures(1, &out_tex_); out_tex_ = 0; }
    out_w_ = out_h_ = 0;
}

uint32_t GlConverter::convert(const AVFrame* frame, int w, int h) {
    if (!initialized_) { set_error("not initialized"); return 0; }
    if (!frame || frame->format != AV_PIX_FMT_VAAPI) {
        set_error("frame is not AV_PIX_FMT_VAAPI"); return 0;
    }

    // Zero-copy: map the VAAPI surface to DRM_PRIME (metadata map; no CPU copy).
    AVFrame* drm = av_frame_alloc();
    if (!drm) { set_error("av_frame_alloc failed"); return 0; }
    drm->format = AV_PIX_FMT_DRM_PRIME;
    if (av_hwframe_map(drm, frame, AV_HWFRAME_MAP_READ) < 0) {
        av_frame_free(&drm);
        set_error("av_hwframe_map to DRM_PRIME failed");
        return 0;
    }
    auto* desc = reinterpret_cast<AVDRMFrameDescriptor*>(drm->data[0]);
    if (!desc || desc->nb_layers < 1) {
        av_frame_free(&drm); set_error("empty DRM descriptor"); return 0;
    }

    const EGLDisplay dpy = static_cast<EGLDisplay>(dpy_);

    // NV12 may arrive as one layer with two planes or two single-plane layers;
    // flatten to an ordered (fd, offset, pitch) list: plane 0 Y, plane 1 UV.
    struct PlaneDesc { int fd; uint32_t offset, pitch; };
    PlaneDesc planes[2];
    int nplanes = 0;
    for (int li = 0; li < desc->nb_layers && nplanes < 2; ++li) {
        const AVDRMLayerDescriptor& L = desc->layers[li];
        for (int pi = 0; pi < L.nb_planes && nplanes < 2; ++pi) {
            const AVDRMPlaneDescriptor& P = L.planes[pi];
            const AVDRMObjectDescriptor& O = desc->objects[P.object_index];
            planes[nplanes++] = PlaneDesc{ O.fd, (uint32_t)P.offset, (uint32_t)P.pitch };
        }
    }
    if (nplanes < 2) { av_frame_free(&drm); set_error("expected 2 NV12 planes"); return 0; }

    // fds stay owned by `drm`; EGLImage import dups them, so do not close them here.
    Plane gp[2];
    bool ok = true;
    for (int i = 0; i < 2 && ok; ++i) {
        const int pw = (i == 0) ? w : (w + 1) / 2;
        const int ph = (i == 0) ? h : (h + 1) / 2;
        const EGLint attrs[] = {
            EGL_WIDTH,                     pw,
            EGL_HEIGHT,                    ph,
            EGL_LINUX_DRM_FOURCC_EXT,      (EGLint)plane_fourcc(i),
            EGL_DMA_BUF_PLANE0_FD_EXT,     planes[i].fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)planes[i].offset,
            EGL_DMA_BUF_PLANE0_PITCH_EXT,  (EGLint)planes[i].pitch,
            EGL_NONE
        };
        gp[i].img = p_eglCreateImageKHR(dpy, EGL_NO_CONTEXT,
                                        EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer)nullptr, attrs);
        if (gp[i].img == EGL_NO_IMAGE_KHR) { set_error("eglCreateImageKHR (plane) failed"); ok = false; break; }

        glGenTextures(1, &gp[i].tex);
        glBindTexture(GL_TEXTURE_2D, gp[i].tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)gp[i].img);
        if (glGetError() != GL_NO_ERROR) { set_error("glEGLImageTargetTexture2DOES failed"); ok = false; }
    }

    if (ok) ok = ensure_target(w, h);

    if (ok) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glViewport(0, 0, w, h);
        glUseProgram(program_);
        glBindVertexArray(vao_);

        float mat[9], yoff[3];
        const bool full = (frame->color_range == AVCOL_RANGE_JPEG);
        make_matrix(coeffs_for(frame->colorspace, h), full, mat, yoff);
        glUniformMatrix3fv(loc_yuv2rgb_, 1, GL_FALSE, mat);
        glUniform3fv(loc_yoff_, 1, yoff);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gp[0].tex);
        glUniform1i(loc_texY_, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, gp[1].tex);
        glUniform1i(loc_texUV_, 1);

        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFinish();

        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (glGetError() != GL_NO_ERROR) { set_error("draw failed"); ok = false; }
    }

    for (int i = 0; i < 2; ++i) {
        if (gp[i].tex) glDeleteTextures(1, &gp[i].tex);
        if (gp[i].img != EGL_NO_IMAGE_KHR) p_eglDestroyImageKHR(dpy, gp[i].img);
    }
    av_frame_free(&drm);   // closes the dmabuf fds + unmaps the DRM_PRIME map

    if (!ok) return 0;
    err_ = "";
    return out_tex_;
}

bool GlConverter::readback(std::vector<uint8_t>& out) {
    if (!out_tex_ || out_w_ <= 0 || out_h_ <= 0) { set_error("no converted frame"); return false; }
    const int w = out_w_, h = out_h_;
    out.assign((size_t)w * h * 4, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    // The shader maps the image top row to framebuffer row 0, so this yields a
    // top-row-first buffer; no row reversal needed.
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (glGetError() != GL_NO_ERROR) { set_error("glReadPixels failed"); return false; }
    return true;
}

} // namespace uav::gpu

#endif
