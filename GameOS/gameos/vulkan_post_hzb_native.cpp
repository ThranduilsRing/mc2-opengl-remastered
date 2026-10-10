// VK-POST-HZB-NATIVE-1 (rung 8.4) -- the HZB pyramid rendered natively on the
// shared engine VulkanContext, sampling the shared scene depth directly and
// writing one separate R32F image per ceil-ladder level.
//
// GATE: MC2_VK_POST_HZB - explicit OPT-IN (an Exact1 row: only "1" arms it), default-OFF. Highest precedence
// at the HZB site when MC2_HZB_BUILD=1. The native path is fail-soft: on any
// not-ready condition it declines and the caller falls back to the unchanged
// GL runHzbReduce() body. MC2_HZB_PROBE=1 intentionally forces the native path
// to decline so the probe keeps reading the fresh GL pyramid instead of stale
// GL textures.

#include "gos_postprocess.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()

#ifdef MC2_VULKAN_ISLAND

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include "vk_image_resource.h"
#include "vk_island_util.h"
#include "vulkan_context.h"
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"
#include "vk_shared_gbuffer_gate.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>
#include <new>

namespace {

void vlog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vk_island_util::vlogv512("[VK_POST_HZB_NATIVE] ", fmt, ap);
    va_end(ap);
}

struct HzbNativeHealth {
    int runtimeGate = 0;
    int substrateLive = 0;
    int directChain = 0;
    unsigned long attempted = 0;
    unsigned long usedVulkan = 0;
    unsigned long usedInterop = 0;
    unsigned long levels = 0;
    unsigned long vkDraws = 0;
    unsigned long copyDepthOps = 0;
    unsigned long readbackOps = 0;
    char fallbackReason[64] = {0};
    double renderUs = 0.0;
    // VK-QWI-TO-FENCE-1: water-style us_fence_wait telemetry around the per-frame
    // CPU-blocking queue wait (QWI before the conversion, per-submit fence after).
    double waitUsLast = 0.0;
    double waitUsAccum = 0.0;
    unsigned long waitSamples = 0;
};
HzbNativeHealth g_health;

void setFallback(const char* reason) {
    std::strncpy(g_health.fallbackReason, reason ? reason : "",
                 sizeof(g_health.fallbackReason) - 1);
    g_health.fallbackReason[sizeof(g_health.fallbackReason) - 1] = '\0';
}

std::string spv_dir() {
    std::string d = mc2::gate::str<"MC2_VULKAN_SPV_DIR">();   // row default shaders/vulkan, empty reads the default
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
    return d;
}

VkShaderModule load_spv(VkDevice device, const std::string& path) {
    return vk_island_util::loadSpv(device, path, vlog);
}

// std140 POD, matching shaders/vulkan/hzb_reduce.frag.
struct HzbLevelParams {
    float srcTexel[2];
    int   reduce;
    int   _pad0;
};
static_assert(sizeof(HzbLevelParams) == 16, "HzbLevelParams std140 offsets drifted");

const VkFormat kLevelFmt = VK_FORMAT_R32_SFLOAT;

} // namespace

struct gosPostProcess::VkPostHzbNative {
    bool disabled = false;
    bool inited = false;
    int  width = 0, height = 0, mipCount = 0;

    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice         device = VK_NULL_HANDLE;
    uint32_t         gfxFamily = UINT32_MAX;
    VkQueue          queue = VK_NULL_HANDLE;
    VmaAllocator     allocator = VK_NULL_HANDLE;

    VkImageResource* colorRes = nullptr;
    VkImageResource* normalRes = nullptr;
    VkImageResource* depthRes = nullptr;
    // VK-RUNG8-ITEM5: shared scene is VK-only (no GL view); direct-chain only.

    VkImageView   depthSampleView = VK_NULL_HANDLE;
    VkSampler     depthSampler = VK_NULL_HANDLE;

    struct Level {
        int w = 0, h = 0;
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer fb = VK_NULL_HANDLE;
        VkBuffer ubo = VK_NULL_HANDLE;
        VmaAllocation uboAlloc = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    std::vector<Level> levels;

    VkRenderPass  rpass = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkPipelineLayout playout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkShaderModule vert = VK_NULL_HANDLE;
    VkShaderModule frag = VK_NULL_HANDLE;
    VkCommandPool cpool = VK_NULL_HANDLE;
    VkCommandBuffer cbuf[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkFence submitFence = VK_NULL_HANDLE; // VK-QWI-TO-FENCE-1: per-submit retire fence
};

namespace {

bool make_buffer(gosPostProcess::VkPostHzbNative* s, VkDeviceSize size,
                 VkBufferUsageFlags usage, VkBuffer* buf, VmaAllocation* alloc) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    if (vmaCreateBuffer(s->allocator, &bci, &aci, buf, alloc, nullptr) != VK_SUCCESS) {
        vlog("vmaCreateBuffer failed.");
        return false;
    }
    return true;
}

void destroy_state(gosPostProcess::VkPostHzbNative* s) {
    if (!s) return;
    VkDevice d = s->device;
    if (d != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(d);
        if (s->pipe) vkDestroyPipeline(d, s->pipe, nullptr);
        if (s->playout) vkDestroyPipelineLayout(d, s->playout, nullptr);
        if (s->vert) vkDestroyShaderModule(d, s->vert, nullptr);
        if (s->frag) vkDestroyShaderModule(d, s->frag, nullptr);
        if (s->pool) vkDestroyDescriptorPool(d, s->pool, nullptr);
        if (s->dsl) vkDestroyDescriptorSetLayout(d, s->dsl, nullptr);
        if (s->rpass) vkDestroyRenderPass(d, s->rpass, nullptr);
        if (s->depthSampler) vkDestroySampler(d, s->depthSampler, nullptr);
        if (s->depthSampleView) vkDestroyImageView(d, s->depthSampleView, nullptr);
        if (s->submitFence) vkDestroyFence(d, s->submitFence, nullptr);   // VK-QWI-TO-FENCE-1
        if (s->cpool) vkDestroyCommandPool(d, s->cpool, nullptr);
        if (s->allocator != VK_NULL_HANDLE) {
            for (auto& lv : s->levels) {
                if (lv.ubo) vmaDestroyBuffer(s->allocator, lv.ubo, lv.uboAlloc);
                if (lv.fb) vkDestroyFramebuffer(d, lv.fb, nullptr);
                if (lv.view) vkDestroyImageView(d, lv.view, nullptr);
                if (lv.image) vmaDestroyImage(s->allocator, lv.image, lv.alloc);
            }
        }
    }
    const bool disabled = s->disabled;
    *s = gosPostProcess::VkPostHzbNative{};
    s->disabled = disabled;
}

bool build_state(gosPostProcess::VkPostHzbNative* s, int w, int h, int mipCount) {
    s->width = w;
    s->height = h;
    s->mipCount = mipCount;

    if (!mc2vk::ensureEngineVulkanContext()) {
        setFallback("engine_vk_context_birth_failed");
        return false;
    }
    mc2vk::VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    if (!ctx.valid()) {
        setFallback("engine_vk_context_not_valid");
        return false;
    }
    s->phys = ctx.physicalDevice();
    s->device = ctx.device();
    s->queue = ctx.graphicsQueue();
    s->gfxFamily = ctx.graphicsFamily();
    s->allocator = ctx.allocator();
    if (s->device == VK_NULL_HANDLE || s->allocator == VK_NULL_HANDLE) {
        setFallback("engine_vk_device_or_allocator_null");
        return false;
    }

    s->colorRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
    s->normalRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_NORMAL);
    s->depthRes = mc2SharedSceneDepthGetResource();
    // VK-RUNG8 item-4/item-5: direct-chain-native only. Only the VK-only shared
    // VkImageResources are required; there is no GL interop view.
    if (!s->colorRes || !s->normalRes || !s->depthRes) {
        setFallback("shared_scene_depth_not_ready");
        return false;
    }
    if ((int)s->colorRes->w != w || (int)s->colorRes->h != h ||
        (int)s->normalRes->w != w || (int)s->normalRes->h != h ||
        (int)s->depthRes->w != w || (int)s->depthRes->h != h) {
        setFallback("shared_scene_depth_size_mismatch");
        return false;
    }
    g_health.substrateLive = 1;

    VkResult r;

    {
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = s->depthRes->image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = s->depthRes->format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        r = vkCreateImageView(s->device, &vci, nullptr, &s->depthSampleView);
        if (r != VK_SUCCESS) {
            vlog("depth sample view create failed (%d).", (int)r);
            return false;
        }
    }
    {
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_NEAREST;
        sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        r = vkCreateSampler(s->device, &sci, nullptr, &s->depthSampler);
        if (r != VK_SUCCESS) {
            vlog("vkCreateSampler failed (%d).", (int)r);
            return false;
        }
    }

    // Render pass: one R32F color attachment, STORE, finalLayout SHADER_READ_ONLY.
    {
        VkAttachmentDescription att{};
        att.format = kLevelFmt;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkSubpassDependency dep{};
        dep.srcSubpass = 0;
        dep.dstSubpass = VK_SUBPASS_EXTERNAL;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 1;
        rpci.pAttachments = &att;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sub;
        rpci.dependencyCount = 1;
        rpci.pDependencies = &dep;
        r = vkCreateRenderPass(s->device, &rpci, nullptr, &s->rpass);
        if (r != VK_SUCCESS) {
            vlog("vkCreateRenderPass failed (%d).", (int)r);
            return false;
        }
    }

    s->levels.resize(static_cast<size_t>(mipCount));
    {
        int lw = w;
        int lh = h;
        for (int level = 0; level < mipCount; ++level) {
            auto& lv = s->levels[static_cast<size_t>(level)];
            lv.w = lw;
            lv.h = lh;

            VkImageCreateInfo ici{};
            ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = kLevelFmt;
            ici.extent = {static_cast<uint32_t>(lw), static_cast<uint32_t>(lh), 1};
            ici.mipLevels = 1;
            ici.arrayLayers = 1;
            ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            if (vmaCreateImage(s->allocator, &ici, &aci, &lv.image, &lv.alloc, nullptr) != VK_SUCCESS) {
                vlog("vmaCreateImage(level %d) failed.", level);
                return false;
            }

            VkImageViewCreateInfo vci{};
            vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vci.image = lv.image;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = kLevelFmt;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            r = vkCreateImageView(s->device, &vci, nullptr, &lv.view);
            if (r != VK_SUCCESS) {
                vlog("vkCreateImageView(level %d) failed (%d).", level, (int)r);
                return false;
            }

            VkFramebufferCreateInfo fci{};
            fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fci.renderPass = s->rpass;
            fci.attachmentCount = 1;
            fci.pAttachments = &lv.view;
            fci.width = static_cast<uint32_t>(lw);
            fci.height = static_cast<uint32_t>(lh);
            fci.layers = 1;
            r = vkCreateFramebuffer(s->device, &fci, nullptr, &lv.fb);
            if (r != VK_SUCCESS) {
                vlog("vkCreateFramebuffer(level %d) failed (%d).", level, (int)r);
                return false;
            }

            if (!make_buffer(s, sizeof(HzbLevelParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             &lv.ubo, &lv.uboAlloc)) {
                return false;
            }

            HzbLevelParams params{};
            if (level == 0) {
                params.srcTexel[0] = 1.0f / static_cast<float>(w);
                params.srcTexel[1] = 1.0f / static_cast<float>(h);
                params.reduce = 0;
            } else {
                const auto& prev = s->levels[static_cast<size_t>(level) - 1];
                params.srcTexel[0] = 1.0f / static_cast<float>(prev.w);
                params.srcTexel[1] = 1.0f / static_cast<float>(prev.h);
                params.reduce = 1;
            }
            void* p = nullptr;
            if (vmaMapMemory(s->allocator, lv.uboAlloc, &p) == VK_SUCCESS) {
                std::memcpy(p, &params, sizeof(params));
                vmaUnmapMemory(s->allocator, lv.uboAlloc);
            } else {
                vlog("vmaMapMemory(ubo level %d) failed.", level);
                return false;
            }

            lw = (lw + 1) / 2;
            lh = (lh + 1) / 2;
            if (lw < 1) lw = 1;
            if (lh < 1) lh = 1;
        }
    }

    {
        VkDescriptorSetLayoutBinding binds[2]{};
        binds[0].binding = 0;
        binds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[0].descriptorCount = 1;
        binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binds[1].binding = 1;
        binds[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binds[1].descriptorCount = 1;
        binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 2;
        lci.pBindings = binds;
        r = vkCreateDescriptorSetLayout(s->device, &lci, nullptr, &s->dsl);
        if (r != VK_SUCCESS) {
            vlog("vkCreateDescriptorSetLayout failed (%d).", (int)r);
            return false;
        }
        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[0].descriptorCount = static_cast<uint32_t>(mipCount);
        sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[1].descriptorCount = static_cast<uint32_t>(mipCount);
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = static_cast<uint32_t>(mipCount);
        pci.poolSizeCount = 2;
        pci.pPoolSizes = sizes;
        r = vkCreateDescriptorPool(s->device, &pci, nullptr, &s->pool);
        if (r != VK_SUCCESS) {
            vlog("vkCreateDescriptorPool failed (%d).", (int)r);
            return false;
        }

        for (int level = 0; level < mipCount; ++level) {
            auto& lv = s->levels[static_cast<size_t>(level)];
            VkDescriptorSetAllocateInfo dsai{};
            dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsai.descriptorPool = s->pool;
            dsai.descriptorSetCount = 1;
            dsai.pSetLayouts = &s->dsl;
            r = vkAllocateDescriptorSets(s->device, &dsai, &lv.set);
            if (r != VK_SUCCESS) {
                vlog("vkAllocateDescriptorSets(level %d) failed (%d).", level, (int)r);
                return false;
            }

            VkDescriptorImageInfo srcInfo{};
            srcInfo.sampler = s->depthSampler;
            srcInfo.imageView = (level == 0) ? s->depthSampleView
                                             : s->levels[static_cast<size_t>(level) - 1].view;
            srcInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkDescriptorBufferInfo uboInfo{lv.ubo, 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet writes[2]{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = lv.set;
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].pImageInfo = &srcInfo;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = lv.set;
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[1].pBufferInfo = &uboInfo;
            vkUpdateDescriptorSets(s->device, 2, writes, 0, nullptr);
        }
    }

    s->vert = load_spv(s->device, spv_dir() + "hzb_reduce.vert.spv");
    s->frag = load_spv(s->device, spv_dir() + "hzb_reduce.frag.spv");
    if (s->vert == VK_NULL_HANDLE || s->frag == VK_NULL_HANDLE) {
        setFallback("spv_load_failed");
        return false;
    }

    {
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &s->dsl;
        r = vkCreatePipelineLayout(s->device, &plci, nullptr, &s->playout);
        if (r != VK_SUCCESS) {
            vlog("vkCreatePipelineLayout failed (%d).", (int)r);
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = s->vert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = s->frag;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1;
        vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable = VK_FALSE;
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dsci{};
        dsci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dsci.dynamicStateCount = 2;
        dsci.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2;
        gp.pStages = stages;
        gp.pVertexInputState = &vin;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pColorBlendState = &cb;
        gp.pDynamicState = &dsci;
        gp.layout = s->playout;
        gp.renderPass = s->rpass;
        gp.subpass = 0;
        r = vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, &s->pipe);
        if (r != VK_SUCCESS) {
            vlog("vkCreateGraphicsPipelines failed (%d).", (int)r);
            return false;
        }
    }

    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s->gfxFamily;
        r = vkCreateCommandPool(s->device, &pci, nullptr, &s->cpool);
        if (r != VK_SUCCESS) {
            vlog("vkCreateCommandPool failed (%d).", (int)r);
            return false;
        }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = s->cpool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 2;
        r = vkAllocateCommandBuffers(s->device, &cai, s->cbuf);
        if (r != VK_SUCCESS) {
            vlog("vkAllocateCommandBuffers failed (%d).", (int)r);
            return false;
        }
    }

    s->inited = true;
    setFallback("");
    g_health.levels = static_cast<unsigned long>(mipCount);
    vlog("initialized %dx%d with %d ceil-ladder levels on the shared engine context "
         "(zero depth ingress/readback egress on the normal path).", w, h, mipCount);
    return true;
}

} // namespace

bool gosPostProcess::vkPostHzbNativeEnabled()
{
    static int state = -1;
    if (state == -1) {
        state = mc2::gate::get<"MC2_VK_POST_HZB">() ? 1 : 0;
        g_health.runtimeGate = state;
        if (state == 1) {
            vlog("MC2_VK_POST_HZB=1 -- rung-8.4 native HZB gate ON "
                 "(lazy init at the HZB site; fail-soft to GL runHzbReduce).");
        }
    }
    return state == 1;
}

// Native HZB runs only in direct-chain, engaged when upstream left the shared GBuffer
// Vulkan-owned (postDirectChainUpstreamOn); otherwise it declines and the GL HZB pass runs.
static bool vkPostHzbDirectChainEnabled()
{
    static int state = -1;
    if (state == -1) {
        state = mc2vkgate::postDirectChainUpstreamOn() ? 1 : 0;
        g_health.directChain = state;
        if (state == 1)
            vlog("native HZB direct-chain: upstream left the shared GBuffer VK-owned; "
                 "no HZB GL<->VK handoff.");
    }
    return state == 1;
}

bool gosPostProcess::runVkPostHzbNative()
{
    if (!hzbEnabled_) return false;
    if (!hzbReduceProg_ || !hzbReduceProg_->is_valid()) return false;
    if (!hzbLevelTex_[0] || !hzbFBO_ || hzbMipCount_ < 1) return false;
    if (sceneDepthTex_ == 0) return false;
    if (hzbProbeEnabled_) {
        setFallback("probe_enabled");
        return false;
    }
    {
        static int seenW = -1;
        static int seenH = -1;
        static int seenMips = -1;
        if (seenW != width_ || seenH != height_ || seenMips != hzbMipCount_) {
            seenW = width_;
            seenH = height_;
            seenMips = hzbMipCount_;
            return false;
        }
    }

    if (!vkPostHzbNative_) vkPostHzbNative_ = new (std::nothrow) VkPostHzbNative();
    VkPostHzbNative* s = vkPostHzbNative_;
    if (!s) {
        setFallback("alloc_failed");
        return false;
    }
    if (s->disabled) return false;
    if (!s->inited || s->width != width_ || s->height != height_ || s->mipCount != hzbMipCount_) {
        if (s->inited) destroy_state(s);
        if (!build_state(s, width_, height_, hzbMipCount_)) {
            vlog("init FAILED (reason=%s) -- disabling for this process, GL HZB runs.",
                 g_health.fallbackReason[0] ? g_health.fallbackReason : "unknown");
            destroy_state(s);
            s->disabled = true;
            return false;
        }
    }

    ++g_health.attempted;
    using clk = std::chrono::steady_clock;
    const clk::time_point t0 = clk::now();

    // VK-RUNG8 item-4: direct-chain only — engage only when upstream is direct-chain,
    // else decline to the GL HZB pass. No GL<->VK semaphore handoff.
    if (!vkPostHzbDirectChainEnabled()) { setFallback("upstream_not_direct_chain"); return false; }
    VkCommandBuffer cb = s->cbuf[0u];

    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
        setFallback("record_failed");
        s->disabled = true;
        return false;
    }

    auto barrier = [&](VkImage img, VkImageAspectFlags aspect, VkImageLayout oldL, VkImageLayout newL,
                       VkAccessFlags srcA, VkAccessFlags dstA,
                       VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = oldL;
        b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {aspect, 0, 1, 0, 1};
        b.srcAccessMask = srcA;
        b.dstAccessMask = dstA;
        vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };

    barrier(s->depthRes->image, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    for (int level = 0; level < s->mipCount; ++level) {
        auto& lv = s->levels[static_cast<size_t>(level)];
        VkRenderPassBeginInfo rbi{};
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = s->rpass;
        rbi.framebuffer = lv.fb;
        rbi.renderArea = {{0, 0}, {static_cast<uint32_t>(lv.w), static_cast<uint32_t>(lv.h)}};
        rbi.clearValueCount = 0;
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);
        VkViewport vp{0.0f, 0.0f, static_cast<float>(lv.w), static_cast<float>(lv.h), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {static_cast<uint32_t>(lv.w), static_cast<uint32_t>(lv.h)}};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout, 0, 1, &lv.set, 0, nullptr);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
    }

    barrier(s->depthRes->image, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_ACCESS_SHADER_READ_BIT, 0,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
        setFallback("record_failed");
        s->disabled = true;
        return false;
    }

    // VK-RUNG8 item-4: direct-chain only — the submit waits/signals no GL<->VK
    // semaphores; the images stay Vulkan-owned for the following native pass.
    // VK-QWI-TO-FENCE-1: dedicated per-submit fence instead of the queue-wide drain
    // (see vulkan_post_fog_native.cpp for the full rationale). Fail-soft to QWI if
    // fence creation ever fails.
    if (s->submitFence == VK_NULL_HANDLE) {
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(s->device, &fci, nullptr, &s->submitFence) != VK_SUCCESS)
            s->submitFence = VK_NULL_HANDLE;
    }
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (vkQueueSubmit(s->queue, 1, &si, s->submitFence) != VK_SUCCESS) {
        setFallback("submit_failed");
        s->disabled = true;
        return false;
    }

    // Slot-recycle guard: this frame's command buffer must be retired before reuse
    // next frame (per-submit fence; QWI only as the fence-creation fail-soft).
    {
        const clk::time_point tWait0 = clk::now();
        if (s->submitFence != VK_NULL_HANDLE) {
            if (vkWaitForFences(s->device, 1, &s->submitFence, VK_TRUE,
                                5ull * 1000 * 1000 * 1000) != VK_SUCCESS) {
                setFallback("fence_timeout");
                s->disabled = true;
                return false;
            }
            vkResetFences(s->device, 1, &s->submitFence);
        } else if (vkQueueWaitIdle(s->queue) != VK_SUCCESS) {
            setFallback("queue_wait_idle_failed");
            s->disabled = true;
            return false;
        }
        g_health.waitUsLast = std::chrono::duration<double, std::micro>(clk::now() - tWait0).count();
        g_health.waitUsAccum += g_health.waitUsLast;
        ++g_health.waitSamples;
        if ((g_health.waitSamples % 600) == 1)
            vlog("[QWI-WAIT] wait_us_last=%.1f wait_us_avg=%.1f samples=%lu",
                 g_health.waitUsLast,
                 g_health.waitUsAccum / (double)g_health.waitSamples, g_health.waitSamples);
    }

    g_health.copyDepthOps = 0;
    g_health.readbackOps = 0;
    g_health.vkDraws = static_cast<unsigned long>(s->mipCount);
    ++g_health.usedVulkan;
    g_health.renderUs = std::chrono::duration<double, std::micro>(clk::now() - t0).count();
    return true;
}

void gosPostProcess::destroyVkPostHzbNative()
{
    if (g_health.runtimeGate) {
        vlog("[HEALTH] runtime_gate=%d substrate_live=%d attempted=%lu used_vulkan=%lu "
             "direct_chain=%d used_interop=%lu levels=%lu vk_draws=%lu copy_depth_ops=%lu readback_ops=%lu "
             "fallback_reason=\"%s\" render_us_last=%.1f wait_us_last=%.1f wait_us_avg=%.1f wait_samples=%lu",
             g_health.runtimeGate, g_health.substrateLive, g_health.attempted,
             g_health.usedVulkan, g_health.directChain, g_health.usedInterop,
             g_health.levels, g_health.vkDraws,
             g_health.copyDepthOps, g_health.readbackOps, g_health.fallbackReason,
             g_health.renderUs, g_health.waitUsLast,
             g_health.waitSamples ? g_health.waitUsAccum / (double)g_health.waitSamples : 0.0,
             g_health.waitSamples);
    }
    if (!vkPostHzbNative_) return;
    destroy_state(vkPostHzbNative_);
    delete vkPostHzbNative_;
    vkPostHzbNative_ = nullptr;
}

extern "C" void mc2_vk_post_hzb_native_health(
    int* runtimeGate, int* substrateLive,
    unsigned long* attempted, unsigned long* usedVulkan,
    unsigned long* levels, unsigned long* vkDraws,
    unsigned long* copyDepthOps, unsigned long* readbackOps,
    const char** fallbackReason, double* renderUs)
{
    if (runtimeGate)    *runtimeGate = g_health.runtimeGate;
    if (substrateLive)  *substrateLive = g_health.substrateLive;
    if (attempted)      *attempted = g_health.attempted;
    if (usedVulkan)     *usedVulkan = g_health.usedVulkan;
    if (levels)         *levels = g_health.levels;
    if (vkDraws)        *vkDraws = g_health.vkDraws;
    if (copyDepthOps)   *copyDepthOps = g_health.copyDepthOps;
    if (readbackOps)    *readbackOps = g_health.readbackOps;
    if (fallbackReason) *fallbackReason = g_health.fallbackReason;
    if (renderUs)       *renderUs = g_health.renderUs;
}

#endif // MC2_VULKAN_ISLAND
