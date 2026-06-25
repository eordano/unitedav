// SPDX-License-Identifier: Apache-2.0
#include "vk_convert.hpp"

#if defined(UAV_ENABLE_GPU) && defined(UAV_HAVE_FFMPEG) && defined(__linux__)

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixfmt.h>
}

#include <vulkan/vulkan.h>
#include <shaderc/shaderc.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/sysmacros.h>

namespace uav::gpu {

namespace {

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
            return (h <= 576) ? Kr_Kb{0.299f, 0.114f} : Kr_Kb{0.2126f, 0.0722f};
    }
}

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

    mat_colmajor[0] = ys;   mat_colmajor[1] = ys;   mat_colmajor[2] = ys;
    mat_colmajor[3] = 0.0f; mat_colmajor[4] = g_cb; mat_colmajor[5] = b_cb;
    mat_colmajor[6] = r_cr; mat_colmajor[7] = g_cr; mat_colmajor[8] = 0.0f;
}

const char* kComp = R"(#version 450
layout(local_size_x = 16, local_size_y = 16) in;
layout(binding = 0) uniform sampler2D texY;
layout(binding = 1) uniform sampler2D texUV;
layout(binding = 2, rgba8) uniform writeonly image2D outImg;
layout(push_constant) uniform PC {
    vec4 c0;   // column 0 of the 3x3 (yuv->rgb), .xyz used
    vec4 c1;   // column 1
    vec4 c2;   // column 2
    vec4 yoff; // .xyz used
} pc;
void main() {
    ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz  = imageSize(outImg);
    if (gid.x >= sz.x || gid.y >= sz.y) return;
    vec2 uv = (vec2(gid) + vec2(0.5)) / vec2(sz);   // gid.y=0 -> image top row
    float y    = texture(texY,  uv).r;
    vec2  cbcr = texture(texUV, uv).rg;
    vec3  ycc  = vec3(y, cbcr.x, cbcr.y) - pc.yoff.xyz;
    mat3  m    = mat3(pc.c0.xyz, pc.c1.xyz, pc.c2.xyz); // columns
    vec3  rgb  = m * ycc;
    imageStore(outImg, gid, vec4(clamp(rgb, 0.0, 1.0), 1.0));
}
)";

struct PushConst {
    float c0[4]; float c1[4]; float c2[4]; float yoff[4];
};

}


struct VkConverter::Impl {
    VkInstance       instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys     = VK_NULL_HANDLE;
    VkDevice         device   = VK_NULL_HANDLE;
    uint32_t         qfam     = 0;
    VkQueue          queue    = VK_NULL_HANDLE;
    VkCommandPool    cmdPool  = VK_NULL_HANDLE;

    VkShaderModule        shader   = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsLayout = VK_NULL_HANDLE;
    VkPipelineLayout      pipeLayout = VK_NULL_HANDLE;
    VkPipeline            pipeline = VK_NULL_HANDLE;
    VkDescriptorPool      descPool = VK_NULL_HANDLE;
    VkSampler             sampler  = VK_NULL_HANDLE;

    VkImage        outImg  = VK_NULL_HANDLE;
    VkDeviceMemory outMem  = VK_NULL_HANDLE;
    VkImageView    outView = VK_NULL_HANDLE;
    int out_w = 0, out_h = 0;

    int32_t mem_props_count = 0;
    VkPhysicalDeviceMemoryProperties memProps{};

    PFN_vkGetMemoryFdPropertiesKHR pGetMemoryFdPropertiesKHR = nullptr;

    ~Impl() {
        if (device) vkDeviceWaitIdle(device);
        destroy_target();
        if (sampler)   vkDestroySampler(device, sampler, nullptr);
        if (descPool)  vkDestroyDescriptorPool(device, descPool, nullptr);
        if (pipeline)  vkDestroyPipeline(device, pipeline, nullptr);
        if (pipeLayout) vkDestroyPipelineLayout(device, pipeLayout, nullptr);
        if (dsLayout)  vkDestroyDescriptorSetLayout(device, dsLayout, nullptr);
        if (shader)    vkDestroyShaderModule(device, shader, nullptr);
        if (cmdPool)   vkDestroyCommandPool(device, cmdPool, nullptr);
        if (device)    vkDestroyDevice(device, nullptr);
        if (instance)  vkDestroyInstance(instance, nullptr);
    }

    void destroy_target() {
        if (outView) { vkDestroyImageView(device, outView, nullptr); outView = VK_NULL_HANDLE; }
        if (outImg)  { vkDestroyImage(device, outImg, nullptr); outImg = VK_NULL_HANDLE; }
        if (outMem)  { vkFreeMemory(device, outMem, nullptr); outMem = VK_NULL_HANDLE; }
        out_w = out_h = 0;
    }

    int find_mem_type(uint32_t typeBits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & want) == want)
                return (int)i;
        }
        return -1;
    }
};

VkConverter::~VkConverter() { delete impl_; impl_ = nullptr; }

bool VkConverter::init(const char* render_node) {
    if (impl_) return true;
    if (!render_node) render_node = "/dev/dri/renderD128";

    Impl* d = new Impl();

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "unitedav-vk-probe";
    app.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    const char* instExts[] = {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = instExts;
    if (vkCreateInstance(&ici, nullptr, &d->instance) != VK_SUCCESS) {
        delete d; set_error("vkCreateInstance failed"); return false;
    }

    struct stat node_st;
    bool have_node_dev = (stat(render_node, &node_st) == 0);

    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(d->instance, &ndev, nullptr);
    std::vector<VkPhysicalDevice> devs(ndev);
    vkEnumeratePhysicalDevices(d->instance, &ndev, devs.data());

    const char* reqDevExts[] = {
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    };
    const int kReqExtCount = 4;

    auto has_all_exts = [&](VkPhysicalDevice pd) -> bool {
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> avail(ne);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, avail.data());
        for (int i = 0; i < kReqExtCount; ++i) {
            bool found = false;
            for (auto& e : avail) if (!std::strcmp(e.extensionName, reqDevExts[i])) { found = true; break; }
            if (!found) return false;
        }
        return true;
    };
    auto matches_node = [&](VkPhysicalDevice pd) -> bool {
        if (!have_node_dev) return false;
        VkPhysicalDeviceDrmPropertiesEXT drm{};
        drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 p2{};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &drm;
        vkGetPhysicalDeviceProperties2(pd, &p2);
        if (drm.hasRender) {
            dev_t rd = makedev((unsigned)drm.renderMajor, (unsigned)drm.renderMinor);
            if (rd == node_st.st_rdev) return true;
        }
        return false;
    };

    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    for (auto pd : devs) {
        if (matches_node(pd) && has_all_exts(pd)) { chosen = pd; break; }
    }
    if (chosen == VK_NULL_HANDLE) {
        for (auto pd : devs) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(pd, &props);
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
            if (has_all_exts(pd)) { chosen = pd; break; }
        }
    }
    if (chosen == VK_NULL_HANDLE) {
        delete d; set_error("no Vulkan device with dmabuf-import extensions"); return false;
    }
    d->phys = chosen;
    vkGetPhysicalDeviceMemoryProperties(d->phys, &d->memProps);

    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(d->phys, &props);
        std::fprintf(stderr, "[uav-vk] device: %s (type=%d apiVer=%u.%u)\n",
                     props.deviceName, props.deviceType,
                     VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion));
    }

    uint32_t nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &nqf, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(nqf);
    vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &nqf, qfs.data());
    int qfam = -1;
    for (uint32_t i = 0; i < nqf; ++i)
        if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfam = (int)i; break; }
    if (qfam < 0) { delete d; set_error("no compute queue family"); return false; }
    d->qfam = (uint32_t)qfam;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = d->qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = kReqExtCount;
    dci.ppEnabledExtensionNames = reqDevExts;
    if (vkCreateDevice(d->phys, &dci, nullptr, &d->device) != VK_SUCCESS) {
        delete d; set_error("vkCreateDevice failed"); return false;
    }
    vkGetDeviceQueue(d->device, d->qfam, 0, &d->queue);

    d->pGetMemoryFdPropertiesKHR = (PFN_vkGetMemoryFdPropertiesKHR)
        vkGetDeviceProcAddr(d->device, "vkGetMemoryFdPropertiesKHR");
    if (!d->pGetMemoryFdPropertiesKHR) {
        delete d; set_error("vkGetMemoryFdPropertiesKHR missing"); return false;
    }

    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = d->qfam;
    if (vkCreateCommandPool(d->device, &cpi, nullptr, &d->cmdPool) != VK_SUCCESS) {
        delete d; set_error("vkCreateCommandPool failed"); return false;
    }

    shaderc_compiler_t comp = shaderc_compiler_initialize();
    shaderc_compile_options_t opts = shaderc_compile_options_initialize();
    shaderc_compile_options_set_optimization_level(opts, shaderc_optimization_level_performance);
    shaderc_compilation_result_t res = shaderc_compile_into_spv(
        comp, kComp, std::strlen(kComp), shaderc_glsl_compute_shader,
        "vk_nv12_rgba.comp", "main", opts);
    if (shaderc_result_get_compilation_status(res) != shaderc_compilation_status_success) {
        std::fprintf(stderr, "[uav-vk] shaderc: %s\n", shaderc_result_get_error_message(res));
        shaderc_result_release(res);
        shaderc_compile_options_release(opts);
        shaderc_compiler_release(comp);
        delete d; set_error("compute shader compile failed"); return false;
    }
    {
        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = shaderc_result_get_length(res);
        smci.pCode = reinterpret_cast<const uint32_t*>(shaderc_result_get_bytes(res));
        VkResult r = vkCreateShaderModule(d->device, &smci, nullptr, &d->shader);
        shaderc_result_release(res);
        shaderc_compile_options_release(opts);
        shaderc_compiler_release(comp);
        if (r != VK_SUCCESS) { delete d; set_error("vkCreateShaderModule failed"); return false; }
    }

    VkDescriptorSetLayoutBinding binds[3]{};
    binds[0].binding = 0; binds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[0].descriptorCount = 1; binds[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    binds[1].binding = 1; binds[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[1].descriptorCount = 1; binds[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    binds[2].binding = 2; binds[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    binds[2].descriptorCount = 1; binds[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dsli{};
    dsli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsli.bindingCount = 3; dsli.pBindings = binds;
    if (vkCreateDescriptorSetLayout(d->device, &dsli, nullptr, &d->dsLayout) != VK_SUCCESS) {
        delete d; set_error("descriptor set layout failed"); return false;
    }

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0; pcr.size = sizeof(PushConst);
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1; pli.pSetLayouts = &d->dsLayout;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(d->device, &pli, nullptr, &d->pipeLayout) != VK_SUCCESS) {
        delete d; set_error("pipeline layout failed"); return false;
    }

    VkComputePipelineCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = d->shader;
    cpci.stage.pName = "main";
    cpci.layout = d->pipeLayout;
    if (vkCreateComputePipelines(d->device, VK_NULL_HANDLE, 1, &cpci, nullptr, &d->pipeline) != VK_SUCCESS) {
        delete d; set_error("compute pipeline failed"); return false;
    }

    VkDescriptorPoolSize ps[2]{};
    ps[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ps[0].descriptorCount = 2;
    ps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          ps[1].descriptorCount = 1;
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = 1; dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(d->device, &dpi, nullptr, &d->descPool) != VK_SUCCESS) {
        delete d; set_error("descriptor pool failed"); return false;
    }

    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.unnormalizedCoordinates = VK_FALSE;
    if (vkCreateSampler(d->device, &sci, nullptr, &d->sampler) != VK_SUCCESS) {
        delete d; set_error("sampler failed"); return false;
    }

    impl_ = d;
    err_ = "";
    return true;
}

namespace {

struct ImportedPlane {
    VkImage        image = VK_NULL_HANDLE;
    VkDeviceMemory mem   = VK_NULL_HANDLE;
    VkImageView    view  = VK_NULL_HANDLE;
};

}

uint64_t VkConverter::convert(const AVFrame* frame, int w, int h) {
    if (!impl_) { set_error("not initialized"); return 0; }
    if (!frame || frame->format != AV_PIX_FMT_VAAPI) {
        set_error("frame is not AV_PIX_FMT_VAAPI"); return 0;
    }
    Impl* d = impl_;

    AVFrame* drm = av_frame_alloc();
    if (!drm) { set_error("av_frame_alloc failed"); return 0; }
    drm->format = AV_PIX_FMT_DRM_PRIME;
    if (av_hwframe_map(drm, frame, AV_HWFRAME_MAP_READ) < 0) {
        av_frame_free(&drm); set_error("av_hwframe_map to DRM_PRIME failed"); return 0;
    }
    auto* desc = reinterpret_cast<AVDRMFrameDescriptor*>(drm->data[0]);
    if (!desc || desc->nb_layers < 1) {
        av_frame_free(&drm); set_error("empty DRM descriptor"); return 0;
    }

    struct PlaneDesc { int fd; uint32_t offset, pitch; uint64_t modifier; };
    PlaneDesc planes[2];
    int nplanes = 0;
    for (int li = 0; li < desc->nb_layers && nplanes < 2; ++li) {
        const AVDRMLayerDescriptor& L = desc->layers[li];
        for (int pi = 0; pi < L.nb_planes && nplanes < 2; ++pi) {
            const AVDRMPlaneDescriptor& P = L.planes[pi];
            const AVDRMObjectDescriptor& O = desc->objects[P.object_index];
            planes[nplanes++] = PlaneDesc{ O.fd, (uint32_t)P.offset,
                                           (uint32_t)P.pitch, O.format_modifier };
        }
    }
    if (nplanes < 2) { av_frame_free(&drm); set_error("expected 2 NV12 planes"); return 0; }

    const VkFormat planeFmt[2] = { VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM };

    ImportedPlane imp[2]{};
    bool ok = true;

    auto cleanup_imports = [&]() {
        for (int i = 0; i < 2; ++i) {
            if (imp[i].view) vkDestroyImageView(d->device, imp[i].view, nullptr);
            if (imp[i].image) vkDestroyImage(d->device, imp[i].image, nullptr);
            if (imp[i].mem) vkFreeMemory(d->device, imp[i].mem, nullptr);
        }
    };

    for (int i = 0; i < 2 && ok; ++i) {
        const uint32_t pw = (i == 0) ? (uint32_t)w : (uint32_t)((w + 1) / 2);
        const uint32_t ph = (i == 0) ? (uint32_t)h : (uint32_t)((h + 1) / 2);

        VkSubresourceLayout planeLayout{};
        planeLayout.offset = planes[i].offset;
        planeLayout.size = 0;
        planeLayout.rowPitch = planes[i].pitch;
        planeLayout.arrayPitch = 0;
        planeLayout.depthPitch = 0;

        VkImageDrmFormatModifierExplicitCreateInfoEXT modInfo{};
        modInfo.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
        modInfo.drmFormatModifier = planes[i].modifier;
        modInfo.drmFormatModifierPlaneCount = 1;
        modInfo.pPlaneLayouts = &planeLayout;

        VkExternalMemoryImageCreateInfo extImg{};
        extImg.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        extImg.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        extImg.pNext = &modInfo;

        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.pNext = &extImg;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = planeFmt[i];
        ici.extent = { pw, ph, 1 };
        ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(d->device, &ici, nullptr, &imp[i].image) != VK_SUCCESS) {
            set_error("vkCreateImage (dmabuf plane) failed"); ok = false; break;
        }

        int dupfd = dup(planes[i].fd);
        if (dupfd < 0) { set_error("dup(dmabuf fd) failed"); ok = false; break; }

        VkMemoryFdPropertiesKHR fdProps{};
        fdProps.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
        if (d->pGetMemoryFdPropertiesKHR(d->device,
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dupfd, &fdProps) != VK_SUCCESS) {
            close(dupfd); set_error("vkGetMemoryFdPropertiesKHR failed"); ok = false; break;
        }

        VkImageMemoryRequirementsInfo2 mri{};
        mri.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
        mri.image = imp[i].image;
        VkMemoryRequirements2 mreq{};
        mreq.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
        vkGetImageMemoryRequirements2(d->device, &mri, &mreq);

        uint32_t typeBits = mreq.memoryRequirements.memoryTypeBits & fdProps.memoryTypeBits;
        int memType = d->find_mem_type(typeBits, 0);
        if (memType < 0) { close(dupfd); set_error("no importable memory type for dmabuf"); ok = false; break; }

        VkImportMemoryFdInfoKHR importInfo{};
        importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
        importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        importInfo.fd = dupfd;

        VkMemoryDedicatedAllocateInfo dedicated{};
        dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.image = imp[i].image;
        importInfo.pNext = &dedicated;

        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.pNext = &importInfo;
        mai.allocationSize = mreq.memoryRequirements.size;
        mai.memoryTypeIndex = (uint32_t)memType;
        if (vkAllocateMemory(d->device, &mai, nullptr, &imp[i].mem) != VK_SUCCESS) {
            close(dupfd); set_error("vkAllocateMemory (import) failed"); ok = false; break;
        }

        if (vkBindImageMemory(d->device, imp[i].image, imp[i].mem, 0) != VK_SUCCESS) {
            set_error("vkBindImageMemory (dmabuf) failed"); ok = false; break;
        }

        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = imp[i].image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = planeFmt[i];
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(d->device, &vci, nullptr, &imp[i].view) != VK_SUCCESS) {
            set_error("vkCreateImageView (plane) failed"); ok = false; break;
        }
    }

    if (ok && (!d->outImg || d->out_w != w || d->out_h != h)) {
        d->destroy_target();
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = { (uint32_t)w, (uint32_t)h, 1 };
        ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(d->device, &ici, nullptr, &d->outImg) != VK_SUCCESS) {
            set_error("vkCreateImage (output) failed"); ok = false;
        }
        if (ok) {
            VkMemoryRequirements mreq{};
            vkGetImageMemoryRequirements(d->device, d->outImg, &mreq);
            int mt = d->find_mem_type(mreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (mt < 0) mt = d->find_mem_type(mreq.memoryTypeBits, 0);
            VkMemoryAllocateInfo mai{};
            mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mai.allocationSize = mreq.size; mai.memoryTypeIndex = (uint32_t)mt;
            if (vkAllocateMemory(d->device, &mai, nullptr, &d->outMem) != VK_SUCCESS) {
                set_error("vkAllocateMemory (output) failed"); ok = false;
            }
        }
        if (ok && vkBindImageMemory(d->device, d->outImg, d->outMem, 0) != VK_SUCCESS) {
            set_error("vkBindImageMemory (output) failed"); ok = false;
        }
        if (ok) {
            VkImageViewCreateInfo vci{};
            vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vci.image = d->outImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = VK_FORMAT_R8G8B8A8_UNORM;
            vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            if (vkCreateImageView(d->device, &vci, nullptr, &d->outView) != VK_SUCCESS) {
                set_error("vkCreateImageView (output) failed"); ok = false;
            }
        }
        if (ok) { d->out_w = w; d->out_h = h; out_w_ = w; out_h_ = h; }
    } else if (ok) {
        out_w_ = w; out_h_ = h;
    }

    VkDescriptorSet dset = VK_NULL_HANDLE;
    if (ok) {
        VkDescriptorSetAllocateInfo dsa{};
        dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsa.descriptorPool = d->descPool;
        dsa.descriptorSetCount = 1; dsa.pSetLayouts = &d->dsLayout;
        if (vkAllocateDescriptorSets(d->device, &dsa, &dset) != VK_SUCCESS) {
            set_error("vkAllocateDescriptorSets failed"); ok = false;
        }
    }
    if (ok) {
        VkDescriptorImageInfo iY{};
        iY.sampler = d->sampler; iY.imageView = imp[0].view;
        iY.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo iUV{};
        iUV.sampler = d->sampler; iUV.imageView = imp[1].view;
        iUV.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo iOut{};
        iOut.imageView = d->outView; iOut.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[3]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = dset; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo = &iY;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = dset; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[1].pImageInfo = &iUV;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = dset; writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[2].pImageInfo = &iOut;
        vkUpdateDescriptorSets(d->device, 3, writes, 0, nullptr);
    }

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (ok) {
        VkCommandBufferAllocateInfo cba{};
        cba.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cba.commandPool = d->cmdPool; cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cba.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(d->device, &cba, &cmd) != VK_SUCCESS) {
            set_error("vkAllocateCommandBuffers failed"); ok = false;
        }
    }
    if (ok) {
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);

        VkImageMemoryBarrier inB[2]{};
        for (int i = 0; i < 2; ++i) {
            inB[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            inB[i].srcAccessMask = 0;
            inB[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            inB[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            inB[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            inB[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
            inB[i].dstQueueFamilyIndex = d->qfam;
            inB[i].image = imp[i].image;
            inB[i].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        }
        VkImageMemoryBarrier outB{};
        outB.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        outB.srcAccessMask = 0;
        outB.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        outB.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        outB.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        outB.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        outB.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        outB.image = d->outImg;
        outB.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        VkImageMemoryBarrier pre[3] = { inB[0], inB[1], outB };
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 3, pre);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->pipeLayout,
                                0, 1, &dset, 0, nullptr);

        PushConst pc{};
        float mat[9], yoff[3];
        const bool full = (frame->color_range == AVCOL_RANGE_JPEG);
        make_matrix(coeffs_for(frame->colorspace, h), full, mat, yoff);
        pc.c0[0]=mat[0]; pc.c0[1]=mat[1]; pc.c0[2]=mat[2];
        pc.c1[0]=mat[3]; pc.c1[1]=mat[4]; pc.c1[2]=mat[5];
        pc.c2[0]=mat[6]; pc.c2[1]=mat[7]; pc.c2[2]=mat[8];
        pc.yoff[0]=yoff[0]; pc.yoff[1]=yoff[1]; pc.yoff[2]=yoff[2];
        vkCmdPushConstants(cmd, d->pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(pc), &pc);

        vkCmdDispatch(cmd, (uint32_t)((w + 15) / 16), (uint32_t)((h + 15) / 16), 1);

        vkEndCommandBuffer(cmd);

        VkFenceCreateInfo fci{}; fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        vkCreateFence(d->device, &fci, nullptr, &fence);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        if (vkQueueSubmit(d->queue, 1, &si, fence) != VK_SUCCESS) {
            set_error("vkQueueSubmit failed"); ok = false;
        } else {
            vkWaitForFences(d->device, 1, &fence, VK_TRUE, UINT64_MAX);
        }
        vkDestroyFence(d->device, fence, nullptr);
    }

    if (cmd) vkFreeCommandBuffers(d->device, d->cmdPool, 1, &cmd);
    if (dset) vkFreeDescriptorSets(d->device, d->descPool, 1, &dset);
    cleanup_imports();
    av_frame_free(&drm);

    if (!ok) return 0;
    err_ = "";
    return (uint64_t)d->outImg;
}

bool VkConverter::readback(std::vector<uint8_t>& out) {
    if (!impl_ || !impl_->outImg || out_w_ <= 0 || out_h_ <= 0) {
        set_error("no converted frame"); return false;
    }
    Impl* d = impl_;
    const int w = out_w_, h = out_h_;
    const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    out.assign((size_t)bytes, 0);

    VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory bufMem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d->device, &bci, nullptr, &buf) != VK_SUCCESS) {
        set_error("readback: vkCreateBuffer failed"); return false;
    }
    VkMemoryRequirements mreq{};
    vkGetBufferMemoryRequirements(d->device, buf, &mreq);
    int mt = d->find_mem_type(mreq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) { vkDestroyBuffer(d->device, buf, nullptr); set_error("readback: no host-visible memory"); return false; }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mreq.size; mai.memoryTypeIndex = (uint32_t)mt;
    if (vkAllocateMemory(d->device, &mai, nullptr, &bufMem) != VK_SUCCESS) {
        vkDestroyBuffer(d->device, buf, nullptr); set_error("readback: vkAllocateMemory failed"); return false;
    }
    vkBindBufferMemory(d->device, buf, bufMem, 0);

    VkCommandBufferAllocateInfo cba{};
    cba.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cba.commandPool = d->cmdPool; cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(d->device, &cba, &cmd);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = d->outImg;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageOffset = { 0, 0, 0 };
    region.imageExtent = { (uint32_t)w, (uint32_t)h, 1 };
    vkCmdCopyImageToBuffer(cmd, d->outImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);

    vkEndCommandBuffer(cmd);

    VkFenceCreateInfo fci{}; fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(d->device, &fci, nullptr, &fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    bool ok = (vkQueueSubmit(d->queue, 1, &si, fence) == VK_SUCCESS);
    if (ok) vkWaitForFences(d->device, 1, &fence, VK_TRUE, UINT64_MAX);

    if (ok) {
        void* mapped = nullptr;
        if (vkMapMemory(d->device, bufMem, 0, bytes, 0, &mapped) == VK_SUCCESS) {
            std::memcpy(out.data(), mapped, (size_t)bytes);
            vkUnmapMemory(d->device, bufMem);
        } else { ok = false; set_error("readback: vkMapMemory failed"); }
    } else {
        set_error("readback: vkQueueSubmit failed");
    }

    vkDestroyFence(d->device, fence, nullptr);
    vkFreeCommandBuffers(d->device, d->cmdPool, 1, &cmd);
    vkDestroyBuffer(d->device, buf, nullptr);
    vkFreeMemory(d->device, bufMem, nullptr);
    return ok;
}

}

#endif
