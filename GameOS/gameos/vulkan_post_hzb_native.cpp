// Same-frame Vulkan depth pyramid and render-only static-prop occlusion.
#include "gos_postprocess.h"
#include "vulkan_shadow_island_live.h"
#include "vulkan_color_prop_island.h"
#include "../../RenderCore/hzb_prop_bounds.h"
#include <bit>
#include <algorithm>
#include <cmath>
#include <cfloat>

using mc2hzb::PropBounds;
using mc2hzb::ProjectedProp;
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()

#ifdef MC2_VULKAN_ISLAND

#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include "vk_image_resource.h"
#include "vk_island_util.h"
#include "vulkan_context.h"
#include "vk_gpu_profile.h"   // VK-GPU-ZONES: MC2_VK_GPU_SCOPE
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
    int directChain = 1;
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

std::string spv_dir() { return vk_island_util::spvDir(); }

VkShaderModule load_spv(VkDevice device, std::string_view path) {
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

    VkCommandBuffer cbuf[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // allocated from `cpool`, freed with it

    // One reduction level: its image, view, framebuffer and UBO are owned (declared so the UBO goes first, the image last).
    struct Level {
        int w = 0, h = 0;
        VkDescriptorSet set = VK_NULL_HANDLE;   // allocated from `pool`, freed with it
        mc2vk::OwnedImage image;
        mc2vk::OwnedImageView view;
        mc2vk::OwnedFramebuffer fb;
        mc2vk::OwnedBuffer ubo;
    };

    // OWNED objects, declared in creation order: the destructor releases them in reverse (destroy_state idles the GPU first).
    mc2vk::OwnedImageView depthSampleView;
    mc2vk::OwnedSampler   depthSampler;
    mc2vk::OwnedRenderPass rpass;
    std::vector<Level> levels;   // framebuffers made against `rpass`, so released before it
    mc2vk::OwnedDescriptorSetLayout dsl;
    mc2vk::OwnedDescriptorPool pool;
    mc2vk::OwnedShaderModule vert;
    mc2vk::OwnedShaderModule frag;
    mc2vk::OwnedPipelineLayout playout;
    mc2vk::OwnedPipeline pipe;
    mc2vk::OwnedCommandPool cpool;
    std::vector<PropBounds> propBounds;
    uint32_t boundsGeneration = UINT32_MAX;
    uint32_t cullCapacity = 0, previousCount = 0;
    uint64_t measuredFrames = 0, totalCulled = 0, totalTested = 0;
    mc2vk::OwnedBuffer projected, visibility;
    mc2vk::OwnedDescriptorSetLayout cullDsl;
    mc2vk::OwnedDescriptorPool cullPool;
    mc2vk::OwnedShaderModule cullShader;
    mc2vk::OwnedPipelineLayout cullLayout;
    mc2vk::OwnedPipeline cullPipe;
    std::vector<VkDescriptorSet> cullSets;
    uint64_t ticket = 0;   // timeline value of this pass's last submit (0 = never): waited at the NEXT run, not inline after the submit
};

namespace {

bool make_buffer(gosPostProcess::VkPostHzbNative* s, VkDeviceSize size,
                 VkBufferUsageFlags usage, mc2vk::OwnedBuffer& out,
                 std::string_view name, int i0 = -1, int i1 = -1) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    if (!out.create(s->allocator, s->device, bci, aci, name, i0, i1)) {
        vlog("vmaCreateBuffer failed.");
        return false;
    }
    return true;
}

// Releases the owned objects (the state's destructor does it, once the GPU is idle) and resets the state; the borrowed handles are not touched.
void destroy_state(gosPostProcess::VkPostHzbNative* s) {
    if (!s) return;
    if (s->device != VK_NULL_HANDLE) vkDeviceWaitIdle(s->device);
    const bool disabled = s->disabled;
    *s = gosPostProcess::VkPostHzbNative{};
    s->disabled = disabled;
}

// Geometry is cached per map; all eight corners of the actual local mesh AABB
// enclose every packet, including off-centre building meshes and rotated props.
bool cache_prop_bounds(gosPostProcess::VkPostHzbNative* s, uint32_t generation) {
    if (s->boundsGeneration == generation) return true;
    Mc2ShadowGeomFeed geom{};
    const void *vertices = nullptr, *indices = nullptr;
    uint64_t vb = 0, ib = 0;
    if (!mc2_shadow_island_get_geometry_feed(&geom) ||
        !mc2_static_prop_geometry_cpu(&vertices, &vb, &indices, &ib) ||
        !vertices || !indices || geom.vertexStride < 12 || vb != geom.vboBytes || ib != geom.iboBytes) return false;
    s->propBounds.assign(geom.typeCount, {});
    const auto* ix = static_cast<const uint32_t*>(indices);
    const auto* vx = static_cast<const uint8_t*>(vertices);
    for (uint32_t t = 0; t < geom.typeCount; ++t) {
        PropBounds b{{FLT_MAX, FLT_MAX, FLT_MAX}, {-FLT_MAX, -FLT_MAX, -FLT_MAX}, false};
        bool invalid = false;
        for (uint32_t p = 0; p < geom.typePacketCount[t]; ++p) {
            const uint64_t pk = uint64_t(geom.typeFirstPacket[t]) + p;
            if (pk >= geom.packetCount) { invalid = true; break; }
            const auto& packet = geom.packets[pk];
            if (uint64_t(packet.firstIndex) + packet.indexCount > ib / sizeof(uint32_t)) { invalid = true; break; }
            for (uint32_t k = 0; k < packet.indexCount; ++k) {
                const uint64_t vi = uint64_t(packet.baseVertex) + ix[packet.firstIndex + k];
                if (vi >= vb / geom.vertexStride) { invalid = true; break; }
                float pos[3];
                std::memcpy(pos, vx + vi * geom.vertexStride, sizeof(pos));
                for (int a = 0; a < 3; ++a) {
                    if (!std::isfinite(pos[a])) invalid = true;
                    b.lo[a] = (std::min)(b.lo[a], pos[a]); b.hi[a] = (std::max)(b.hi[a], pos[a]);
                }
                b.valid = true;
            }
        }
        b.valid = b.valid && !invalid;
        s->propBounds[t] = b;
    }
    s->boundsGeneration = generation;
    return true;
}


bool build_prop_cull(gosPostProcess::VkPostHzbNative& state, uint32_t count) {
    auto* s = &state;
    if (s->cullCapacity >= count) return true;
    // The color pass can still be reading the old mask after the HZB ticket.
    // Only capacity growth replaces its storage/descriptors; retire every reader.
    vkDeviceWaitIdle(s->device);
    s->previousCount = 0; // the retired mask no longer supplies telemetry
    s->cullCapacity = (std::max)(1024u, std::bit_ceil(count));
    s->cullPool.reset(); s->projected.reset(); s->visibility.reset();
    if (!make_buffer(s, uint64_t(s->cullCapacity)*sizeof(ProjectedProp), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, s->projected, "hzb.projected") ||
        !make_buffer(s, uint64_t(s->cullCapacity+1)*sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, s->visibility, "hzb.visibility")) return false;
    if (!s->cullDsl) {
        VkDescriptorSetLayoutBinding bindings[3] = {};
        for (uint32_t i = 0; i < 3; ++i) {
            bindings[i].binding = i; bindings[i].descriptorCount = 1; bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        VkDescriptorSetLayoutCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        di.bindingCount = 3; di.pBindings = bindings;
        if (!mc2vk::create(s->cullDsl, s->device, [&](VkDescriptorSetLayout* p){return vkCreateDescriptorSetLayout(s->device,&di,nullptr,p);})) return false;
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
        const VkDescriptorSetLayout layout = s->cullDsl.get();
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1; li.pSetLayouts = &layout; li.pushConstantRangeCount = 1; li.pPushConstantRanges = &push;
        if (!mc2vk::create(s->cullLayout,s->device,[&](VkPipelineLayout* p){return vkCreatePipelineLayout(s->device,&li,nullptr,p);})) return false;
        s->cullShader = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir()+"hzb_cull.comp.spv"));
        if (!s->cullShader) return false;
        VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pi.stage.module = s->cullShader.get(); pi.stage.pName = "main"; pi.layout = s->cullLayout.get();
        if (!mc2vk::create(s->cullPipe,s->device,[&](VkPipeline* p){return mc2vk::infra::compute(s->device,VK_NULL_HANDLE,1,&pi,nullptr,p);})) return false;
    }
    const uint32_t levels = uint32_t(s->mipCount);
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, levels}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, levels*2}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = levels; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
    if (!mc2vk::create(s->cullPool,s->device,[&](VkDescriptorPool* p){return vkCreateDescriptorPool(s->device,&pci,nullptr,p);})) return false;
    s->cullSets.resize(levels);
    for (uint32_t i = 0; i < levels; ++i) {
        const VkDescriptorSetLayout layout = s->cullDsl.get();
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = s->cullPool.get(); ai.descriptorSetCount = 1; ai.pSetLayouts = &layout;
        if (vkAllocateDescriptorSets(s->device,&ai,&s->cullSets[i]) != VK_SUCCESS) return false;
        VkDescriptorImageInfo image{s->depthSampler.get(),s->levels[i].view.get(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo buffers[] = {{s->projected.get(),0,VK_WHOLE_SIZE},{s->visibility.get(),0,VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[3] = {};
        for (uint32_t j = 0; j < 3; ++j) {
            writes[j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[j].dstSet = s->cullSets[i]; writes[j].dstBinding = j; writes[j].descriptorCount = 1;
            writes[j].descriptorType = j==0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if (j==0) writes[j].pImageInfo = &image; else writes[j].pBufferInfo = &buffers[j-1];
        }
        vkUpdateDescriptorSets(s->device,3,writes,0,nullptr);
    }
    return true;
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
    if (static_cast<int>(s->colorRes->w) != w || static_cast<int>(s->colorRes->h) != h ||
        static_cast<int>(s->normalRes->w) != w || static_cast<int>(s->normalRes->h) != h ||
        static_cast<int>(s->depthRes->w) != w || static_cast<int>(s->depthRes->h) != h) {
        setFallback("shared_scene_depth_size_mismatch");
        return false;
    }
    g_health.substrateLive = 1;

    VkResult r;

    {
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = s->depthRes->image.get();
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = s->depthRes->format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (!mc2vk::create(s->depthSampleView, s->device, [&](VkImageView* h) { r = vkCreateImageView(s->device, &vci, nullptr, h); return r; })) {
            vlog("depth sample view create failed (%d).", static_cast<int>(r));
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
        if (!mc2vk::create(s->depthSampler, s->device, [&](VkSampler* h) { r = vkCreateSampler(s->device, &sci, nullptr, h); return r; })) {
            vlog("vkCreateSampler failed (%d).", static_cast<int>(r));
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
        dep.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        dep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 1;
        rpci.pAttachments = &att;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sub;
        rpci.dependencyCount = 1;
        rpci.pDependencies = &dep;
        if (!mc2vk::create(s->rpass, s->device, [&](VkRenderPass* h) { r = vkCreateRenderPass(s->device, &rpci, nullptr, h); return r; })) {
            vlog("vkCreateRenderPass failed (%d).", static_cast<int>(r));
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
            if (!lv.image.create(s->allocator, s->device, ici, aci, "posthzb.level", level)) {
                vlog("vmaCreateImage(level %d) failed.", level);
                return false;
            }

            VkImageViewCreateInfo vci{};
            vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vci.image = lv.image.get();
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = kLevelFmt;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (!mc2vk::create(lv.view, s->device, [&](VkImageView* h) { r = vkCreateImageView(s->device, &vci, nullptr, h); return r; })) {
                vlog("vkCreateImageView(level %d) failed (%d).", level, static_cast<int>(r));
                return false;
            }

            VkFramebufferCreateInfo fci{};
            fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fci.renderPass = s->rpass.get();
            fci.attachmentCount = 1;
            const VkImageView levelView = lv.view.get();
            fci.pAttachments = &levelView;
            fci.width = static_cast<uint32_t>(lw);
            fci.height = static_cast<uint32_t>(lh);
            fci.layers = 1;
            if (!mc2vk::create(lv.fb, s->device, [&](VkFramebuffer* h) { r = vkCreateFramebuffer(s->device, &fci, nullptr, h); return r; })) {
                vlog("vkCreateFramebuffer(level %d) failed (%d).", level, static_cast<int>(r));
                return false;
            }

            if (!make_buffer(s, sizeof(HzbLevelParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             lv.ubo, "posthzb.levelUbo", level)) {
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
            if (!lv.ubo.upload(&params, sizeof(params))) {
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
        if (!mc2vk::create(s->dsl, s->device, [&](VkDescriptorSetLayout* h) { r = vkCreateDescriptorSetLayout(s->device, &lci, nullptr, h); return r; })) {
            vlog("vkCreateDescriptorSetLayout failed (%d).", static_cast<int>(r));
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
        if (!mc2vk::create(s->pool, s->device, [&](VkDescriptorPool* h) { r = vkCreateDescriptorPool(s->device, &pci, nullptr, h); return r; })) {
            vlog("vkCreateDescriptorPool failed (%d).", static_cast<int>(r));
            return false;
        }

        const VkDescriptorSetLayout dslHandle = s->dsl.get();
        for (int level = 0; level < mipCount; ++level) {
            auto& lv = s->levels[static_cast<size_t>(level)];
            VkDescriptorSetAllocateInfo dsai{};
            dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsai.descriptorPool = s->pool.get();
            dsai.descriptorSetCount = 1;
            dsai.pSetLayouts = &dslHandle;
            r = vkAllocateDescriptorSets(s->device, &dsai, &lv.set);
            if (r != VK_SUCCESS) {
                vlog("vkAllocateDescriptorSets(level %d) failed (%d).", level, static_cast<int>(r));
                return false;
            }

            VkDescriptorImageInfo srcInfo{};
            srcInfo.sampler = s->depthSampler.get();
            srcInfo.imageView = (level == 0) ? s->depthSampleView.get()
                                             : s->levels[static_cast<size_t>(level) - 1].view.get();
            srcInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkDescriptorBufferInfo uboInfo{lv.ubo.get(), 0, VK_WHOLE_SIZE};
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

    s->vert = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "hzb_reduce.vert.spv"));
    s->frag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "hzb_reduce.frag.spv"));
    if (!s->vert || !s->frag) {
        setFallback("spv_load_failed");
        return false;
    }

    {
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        const VkDescriptorSetLayout dslHandle = s->dsl.get();
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &dslHandle;
        if (!mc2vk::create(s->playout, s->device, [&](VkPipelineLayout* h) { r = vkCreatePipelineLayout(s->device, &plci, nullptr, h); return r; })) {
            vlog("vkCreatePipelineLayout failed (%d).", static_cast<int>(r));
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = s->vert.get();
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = s->frag.get();
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
        gp.layout = s->playout.get();
        gp.renderPass = s->rpass.get();
        gp.subpass = 0;
        const bool pipeOk = mc2vk::create(s->pipe, s->device, [&](VkPipeline* h) { r = mc2vk::infra::graphics(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h); return r; });
        mc2vk::nameObject(s->device, s->pipe.get(), "posthzb.pipe");
        if (!pipeOk) {
            vlog("vkCreateGraphicsPipelines failed (%d).", static_cast<int>(r));
            return false;
        }
    }

    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s->gfxFamily;
        if (!mc2vk::create(s->cpool, s->device, [&](VkCommandPool* h) { r = vkCreateCommandPool(s->device, &pci, nullptr, h); return r; })) {
            vlog("vkCreateCommandPool failed (%d).", static_cast<int>(r));
            return false;
        }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = s->cpool.get();
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 2;
        r = vkAllocateCommandBuffers(s->device, &cai, s->cbuf);
        if (r != VK_SUCCESS) {
            vlog("vkAllocateCommandBuffers failed (%d).", static_cast<int>(r));
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

// The owner's deleter: idles the GPU and releases the owned objects before the state itself goes.
void gosPostProcess::VkPostHzbNativeDeleter::operator()(VkPostHzbNative* s) const {
    destroy_state(s);
    delete s;
}

void gosPostProcess::prewarmVkHzb() {
    if(!hzbEnabled_ || width_<=0 || height_<=0) return;
    if(!vkPostHzbNative_) vkPostHzbNative_.reset(std::make_unique<VkPostHzbNative>().release());
    auto* s=vkPostHzbNative_.get();
    int levels=1; for(int edge=(std::max)(width_,height_);edge>1;edge=(edge+1)/2) ++levels;
    if(!s->inited && !build_state(s,width_,height_,levels)) { destroy_state(s); return; }
    if(!build_prop_cull(*s,1024)) destroy_state(s);
}

bool gosPostProcess::runVkPostHzbNative(const Mc2ColorPropFeed& feed, uint32_t generation, void** maskBuffer)
{
    g_health.runtimeGate = hzbEnabled_ ? 1 : 0;
    if (maskBuffer) *maskBuffer = nullptr;
    if (!hzbEnabled_ || !maskBuffer || !feed.instances112 || !feed.count || feed.count > 131072) return false;
    // Small feeds cannot amortize the pyramid and dispatches. A null mask means
    // the color pass keeps every instance, including after a larger prior frame.
    if (feed.count < 1024) {
        setFallback("low_candidate_count");
        return false;
    }
    int levels = 1;
    for (int size = (std::max)(width_, height_); size > 1; size = (size+1)/2) ++levels;
    hzbW_ = width_; hzbH_ = height_; hzbMipCount_ = levels;
    if (!vkPostHzbNative_) vkPostHzbNative_.reset(new (std::nothrow) VkPostHzbNative());
    VkPostHzbNative* s = vkPostHzbNative_.get();
    if (!s) {
        setFallback("alloc_failed");
        return false;
    }
    if (s->disabled) return false;

    // The previous submit of this pass must have completed before its command buffer is reused. A timeline ticket waited here (a frame after the submit,
    // normally already complete) replaces the fence the pass used to wait inline right after every submit.
    {
        const std::chrono::steady_clock::time_point tWait0 = std::chrono::steady_clock::now();
        if (!mc2vk::getOrCreateEngineVulkanContext().wait(mc2vk::GpuTicket{s->ticket, 0}, 5ull * 1000 * 1000 * 1000, "post_prev_submit")) {
            setFallback("fence_timeout"); s->disabled = true; return false;
        }
        g_health.waitUsLast = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - tWait0).count();
        g_health.waitUsAccum += g_health.waitUsLast;
        ++g_health.waitSamples;
        if ((g_health.waitSamples % 600) == 1)
            vlog("[QWI-WAIT] wait_us_last=%.1f wait_us_avg=%.1f samples=%lu",
                 g_health.waitUsLast,
                 g_health.waitUsAccum / static_cast<double>(g_health.waitSamples), g_health.waitSamples);
    }
    if (!s->inited || s->width != width_ || s->height != height_ || s->mipCount != hzbMipCount_) {
        if (s->inited) destroy_state(s);
        if (!build_state(s, width_, height_, hzbMipCount_)) {
            vlog("init FAILED (reason=%s) -- disabling for this process, culling keeps every prop.",
                 g_health.fallbackReason[0] ? g_health.fallbackReason : "unknown");
            destroy_state(s);
            s->disabled = true;
            return false;
        }
    }

    if (!cache_prop_bounds(s, generation) || !build_prop_cull(*s, feed.count)) {
        setFallback("cull_resources_failed"); s->disabled = true; return false;
    }
    if (s->previousCount) {
        vmaInvalidateAllocation(s->allocator, s->visibility.allocation(), 0, sizeof(uint32_t));
        if (const void* p = s->visibility.map()) {
            const uint32_t culled = *static_cast<const uint32_t*>(p);
            s->visibility.unmap();
            ++s->measuredFrames; s->totalCulled += culled; s->totalTested += s->previousCount;
            if (s->measuredFrames <= 3 || s->measuredFrames % 120 == 0)
                vlog("[CULL] tested=%u culled=%u kept=%u total_tested=%llu total_culled=%llu frames=%llu same_frame=1 render_only=1",
                    s->previousCount, culled, s->previousCount-culled,
                    static_cast<unsigned long long>(s->totalTested), static_cast<unsigned long long>(s->totalCulled), static_cast<unsigned long long>(s->measuredFrames));
        }
    }
    std::vector<ProjectedProp> projected(feed.count);
    const auto* instances = static_cast<const GpuStaticPropInstance*>(feed.instances112);
    for (uint32_t i = 0; i < feed.count; ++i)
        projected[i] = mc2hzb::project_prop(instances[i], feed.worldToClipGL, feed.objectDepthBias, s->propBounds, width_, height_, s->mipCount);
    if (!s->projected.upload(projected.data(), projected.size()*sizeof(ProjectedProp))) {
        setFallback("bounds_upload_failed"); s->disabled = true; return false;
    }

    ++g_health.attempted;
    using clk = std::chrono::steady_clock;
    const clk::time_point t0 = clk::now();

    // VK-RUNG8 item-4: direct-chain only — engage only when upstream is direct-chain,
    // else decline to the GL HZB pass. No GL<->VK semaphore handoff.
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

    barrier(s->depthRes->image.get(), VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    for (int level = 0; level < s->mipCount; ++level) {
        auto& lv = s->levels[static_cast<size_t>(level)];
        VkRenderPassBeginInfo rbi{};
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = s->rpass.get();
        rbi.framebuffer = lv.fb.get();
        rbi.renderArea = {{0, 0}, {static_cast<uint32_t>(lv.w), static_cast<uint32_t>(lv.h)}};
        rbi.clearValueCount = 0;
        MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Post.HzbBuild");
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe.get());
        VkViewport vp{0.0f, 0.0f, static_cast<float>(lv.w), static_cast<float>(lv.h), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {static_cast<uint32_t>(lv.w), static_cast<uint32_t>(lv.h)}};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &lv.set, 0, nullptr);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
        gpuScope.end();
    }

    // Order the prior color pass's mask reads before this frame's reset.
    auto memoryBarrier = [&](VkPipelineStageFlags src, VkPipelineStageFlags dst, VkAccessFlags from, VkAccessFlags to) {
        VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; m.srcAccessMask = from; m.dstAccessMask = to;
        vkCmdPipelineBarrier(cb,src,dst,0,1,&m,0,nullptr,0,nullptr);
    };
    memoryBarrier(VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(cb,s->visibility.get(),0,sizeof(uint32_t),0);
    memoryBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    MC2_VK_GPU_SCOPE(gpuCull, cb, "VKIsl.Post.HzbCull");
    mc2vk::infra::bind(cb,VK_PIPELINE_BIND_POINT_COMPUTE,s->cullPipe.get());
    for (uint32_t level = 0; level < uint32_t(s->mipCount); ++level) {
        const uint32_t push[] = {feed.count,level};
        vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,s->cullLayout.get(),0,1,&s->cullSets[level],0,nullptr);
        vkCmdPushConstants(cb,s->cullLayout.get(),VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);
        vkCmdDispatch(cb,(feed.count+63)/64,1,1);
        memoryBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    gpuCull.end();
    memoryBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                  VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_HOST_READ_BIT);

    barrier(s->depthRes->image.get(), VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

    if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
        setFallback("record_failed");
        s->disabled = true;
        return false;
    }

    // VK-RUNG8 item-4: direct-chain only — the submit waits/signals no GL<->VK
    // semaphores; the images stay Vulkan-owned for the following native pass.
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (mc2vk::getOrCreateEngineVulkanContext().submit(si, VK_NULL_HANDLE, &s->ticket) != VK_SUCCESS) {
        setFallback("submit_failed");
        s->disabled = true;
        return false;
    }

    s->previousCount = feed.count;
    *maskBuffer = reinterpret_cast<void*>(s->visibility.get());
    ++hzbBuildCount_;
    g_health.copyDepthOps = 0;
    g_health.readbackOps = 0;
    g_health.vkDraws = static_cast<unsigned long>(s->mipCount);
    ++g_health.usedVulkan;
    g_health.renderUs = std::chrono::duration<double, std::micro>(clk::now() - t0).count();
    setFallback("");
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
             g_health.waitSamples ? g_health.waitUsAccum / static_cast<double>(g_health.waitSamples) : 0.0,
             g_health.waitSamples);
    }
    if (!vkPostHzbNative_) return;
    vkPostHzbNative_.reset();
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
