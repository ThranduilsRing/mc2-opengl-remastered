// vulkan_sky_island.cpp -- VK-PARITY-F3: HDRI equirect sky co-write. See header.
// Structure follows vulkan_legacy_shape_island.cpp (shared-GBuffer LOAD pass,
// parity-fenced submit) and vulkan_water_island.cpp ensure_hdri (copy-once import).
#ifdef MC2_VULKAN_ISLAND

#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include "vk_image_resource.h"
#include "vk_island_util.h"
#include "vk_one_shot.h"
#include "vulkan_context.h"
#include "vk_barrier_batch_vk.h"
#include "vk_gpu_profile.h"   // VK-GPU-ZONES: MC2_VK_GPU_SCOPE
#include "vulkan_sky_island.h"
#include "vk_hdri_source.h"
#include "vk_shared_gbuffer_gate.h"
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <vector>
#include <string>
#include <utility>
#include <functional>
#include <tracy/Tracy.hpp>
#include "../../mclib/battle_bench_wait.h"

namespace {

struct Health { unsigned long attempted = 0, usedVulkan = 0; char fallbackReason[128] = {0}; };
Health g_health;
void setFallback(const char* r) {
    std::strncpy(g_health.fallbackReason, r ? r : "", sizeof(g_health.fallbackReason) - 1);
    g_health.fallbackReason[sizeof(g_health.fallbackReason) - 1] = 0;
}
void vlog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vk_island_util::vlogv("[VK_SKY] ", fmt, ap);
    va_end(ap);
}

std::string spv_dir() { return vk_island_util::spvDir(); }

VkShaderModule load_spv(VkDevice device, std::string_view path) {
    return vk_island_util::loadSpv(device, path, vlog, true);
}

// LOCKSTEP with hdri_sky.frag SkyPC (push constant).
struct SkyPC {
    float invProj[16];
    float rotC0[4], rotC1[4], rotC2[4];
    float skyYaw;
};
static_assert(sizeof(SkyPC) == 116, "SkyPC lockstep");

// Owned objects are declared in creation order and released in reverse by the destructor, while the device is alive (the engine context releases the
// parked island ahead of the device).
struct Island : mc2vk::IslandState {
    bool inited = false, disabled = false;
    int  waitStrikes = 0;
    int  w = 0, h = 0;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE; uint32_t gfxFamily = UINT32_MAX;
    VkQueue queue = VK_NULL_HANDLE; VmaAllocator allocator = VK_NULL_HANDLE;
    struct VkImageResource* colorRes = nullptr;
    struct VkImageResource* depthRes = nullptr;
    VkDescriptorSet set = VK_NULL_HANDLE;   // allocated from `pool`, freed with it
    VkCommandBuffer cbuf[mc2vk::kMaxFramesInFlight] = {};           // allocated from `cpool`, freed with it
    mc2vk::GpuTicket submitted[mc2vk::kMaxFramesInFlight];          // per engine frame slot: the last submit that used cbuf[slot]
    uint64_t hdriGeneration = 0; bool hdriAttempted = false, hdriReal = false;   // the CPU HDRI generation imported

    mc2vk::OwnedCommandPool cpool;
    mc2vk::OwnedSampler samp;
    mc2vk::OwnedRenderPass rpass;
    mc2vk::OwnedFramebuffer fb;
    mc2vk::OwnedDescriptorSetLayout dsl;
    mc2vk::OwnedDescriptorPool pool;
    mc2vk::OwnedShaderModule vert, frag;
    mc2vk::OwnedPipelineLayout playout;
    mc2vk::OwnedPipeline pipe;
    mc2vk::OwnedImage hdriImg;          // declared before its view: the view is released first
    mc2vk::OwnedImageView hdriView;
};

bool g_sessionDeclined = false;
int  g_initReachedBuild = 0;

struct Pending { bool valid = false; float invProj[16] = {}; float rot[9] = {}; float yaw = 0.f; uint64_t hdriGeneration = 0; };
Pending g_pending;

// The HDRI is replaced when the GL texture changes: the caller idles the GPU first.
void destroy_hdri(Island* s) {
    s->hdriView.reset();
    s->hdriImg.reset();
}

void update_set(Island* s) {
    VkDescriptorImageInfo ii{};
    ii.sampler = s->samp.get(); ii.imageView = s->hdriView.get(); ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = s->set; w.dstBinding = 0;
    w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
    vkUpdateDescriptorSets(s->device, 1, &w, 0, nullptr);
}

bool submit_once(Island* s, const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = s->cpool.get(); cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(s->device, &cbi, &cb) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    record(cb);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    const bool ok = mc2vk::submitAndWait(mc2vk::getOrCreateEngineVulkanContext(), si, 30ull * 1000 * 1000 * 1000, "sky copy-once");
    vkFreeCommandBuffers(s->device, s->cpool.get(), 1, &cb);
    return ok;
}

// Copy-once HDRI import from the engine's CPU copy (the water island's ensure_hdri does the same). Called before recording.
// On failure/absence a 1x1 black dummy is bound (sky = black, the same as GL's "no HDRI" baseline).
bool ensure_hdri(Island* s, uint64_t generation) {
    // Keyed on the CPU copy's load generation: a reloaded HDRI can come back under the same GL name.
    if (s->hdriAttempted && s->hdriGeneration == generation) return static_cast<bool>(s->hdriView);
    s->hdriAttempted = true; s->hdriGeneration = generation; s->hdriReal = false;
    if (s->hdriView || s->hdriImg) { mc2vk::getOrCreateEngineVulkanContext().waitIdle("hdri_replace"); destroy_hdri(s); }

    VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    int w = 1, h = 1; uint32_t mips = 1;
    std::vector<std::vector<unsigned char>> levels;
    std::vector<std::pair<int,int>> dims;
    bool haveReal = false;
    if (generation != 0) {
        uint64_t gotGeneration = 0;
        const char* reason = "";
        haveReal = mc2vk::hdriFromCpu(s->phys, fmt, levels, dims, gotGeneration, reason);
        if (haveReal) { w = dims[0].first; h = dims[0].second; mips = static_cast<uint32_t>(levels.size()); }
        else vlog("HDRI not imported (%s) -- black sky.", reason);
    }
    if (!haveReal) {
        fmt = VK_FORMAT_R16G16B16A16_SFLOAT; w = 1; h = 1; mips = 1;
        levels.assign(1, std::vector<unsigned char>(8, 0));
        dims.assign(1, {1, 1});
    }

    // The image and its staging copy are admitted by OwnedImage/OwnedBuffer (residency ledger). A refused real HDRI degrades to the 1x1
    // black dummy, the same as an absent one, instead of failing the island.
    auto createImage = [&]() -> bool {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
        ii.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
        ii.mipLevels = mips; ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
        if (!s->hdriImg.create(s->allocator, s->device, ii, aci, "sky.hdri")) return false;
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = s->hdriImg.get(); vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
        return mc2vk::create(s->hdriView, s->device, [&](VkImageView* h) { return vkCreateImageView(s->device, &vi, nullptr, h); });
    };
    auto degradeToDummy = [&]() {
        s->hdriView.reset(); s->hdriImg.reset();
        haveReal = false; fmt = VK_FORMAT_R16G16B16A16_SFLOAT; w = 1; h = 1; mips = 1;
        levels.assign(1, std::vector<unsigned char>(8, 0));
        dims.assign(1, {1, 1});
    };
    std::vector<VkDeviceSize> offs;
    mc2vk::OwnedBuffer staging;   // released on return; submit_once has waited for the copy by then
    auto createStaging = [&]() -> bool {
        offs.assign(levels.size(), 0);
        VkDeviceSize total = 0;
        for (size_t i = 0; i < levels.size(); ++i) { offs[i] = total; total += static_cast<VkDeviceSize>(((levels[i].size() + 15) & ~size_t(15))); }
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bci.size = total ? total : 16; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO; aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        return staging.create(s->allocator, s->device, bci, aci, "sky.staging");
    };
    bool admitted = createImage() && createStaging();
    if (!admitted) {
        if (!haveReal) return false;
        vlog("HDRI (image or staging copy) refused by residency or allocation -- black sky.");
        degradeToDummy();
        if (!createImage() || !createStaging()) { s->hdriView.reset(); s->hdriImg.reset(); return false; }
    }
    {
        void* p = staging.map();
        if (!p) return false;
        for (size_t i = 0; i < levels.size(); ++i) std::memcpy(static_cast<unsigned char*>(p) + offs[i], levels[i].data(), levels[i].size());
        staging.unmap();
    }
    const bool ok = submit_once(s, [&](VkCommandBuffer cb) {
        const auto& bctx = mc2vk::getOrCreateEngineVulkanContext();
        const VkImageSubresourceRange hdriRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
        mc2vk::sync::cmdImageBarrier(bctx, cb, s->hdriImg.get(), hdriRange,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, 0,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        for (uint32_t i = 0; i < mips; ++i) {
            VkBufferImageCopy c{};
            c.bufferOffset = offs[i];
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
            c.imageExtent = {static_cast<uint32_t>(dims[i].first), static_cast<uint32_t>(dims[i].second), 1};
            vkCmdCopyBufferToImage(cb, staging.get(), s->hdriImg.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        }
        mc2vk::sync::cmdImageBarrier(bctx, cb, s->hdriImg.get(), hdriRange,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    if (!ok) { vlog("HDRI upload submit failed."); return false; }
    s->hdriReal = haveReal;
    mc2vk::getOrCreateEngineVulkanContext().waitIdle("hdri_descriptors");
    update_set(s);
    vlog("HDRI %s import: %dx%d fmt=%d mips=%u source_version=%llu (copy-once).", haveReal ? "REAL" : "DUMMY", w, h, static_cast<int>(fmt), mips, static_cast<unsigned long long>(generation));
    return true;
}

bool init_island(Island* s, int w, int h) {
    s->w = w; s->h = h;
    ++g_health.attempted;
    g_initReachedBuild = 0;
    if (!mc2vk::ensureEngineVulkanContext()) { setFallback("engine_vk_context_birth_failed"); return false; }
    mc2vk::VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    if (!ctx.valid()) { setFallback("engine_vk_context_not_valid"); return false; }
    s->phys = ctx.physicalDevice(); s->device = ctx.device(); s->queue = ctx.graphicsQueue();
    s->gfxFamily = ctx.graphicsFamily(); s->allocator = ctx.allocator();
    if (s->device == VK_NULL_HANDLE || s->allocator == VK_NULL_HANDLE) { setFallback("device_or_allocator_null"); return false; }
    s->colorRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
    s->depthRes = mc2SharedSceneDepthGetResource();
    if (!s->colorRes || !s->depthRes) { setFallback("shared_gbuffer_not_ready"); return false; }
    g_initReachedBuild = 1;

    {
        VkCommandPoolCreateInfo cpi{};
        cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpi.queueFamilyIndex = s->gfxFamily;
        if (!mc2vk::create(s->cpool, s->device, [&](VkCommandPool* h) { return vkCreateCommandPool(s->device, &cpi, nullptr, h); })) { setFallback("cmdpool_failed"); return false; }
        VkCommandBufferAllocateInfo cbi{};
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbi.commandPool = s->cpool.get(); cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = mc2vk::kMaxFramesInFlight;
        if (vkAllocateCommandBuffers(s->device, &cbi, s->cbuf) != VK_SUCCESS) { setFallback("cmdbuf_failed"); return false; }
    }
    {   // sampler: same as the water island's HDRI sampler (linear, clamp, full mip range)
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        if (!mc2vk::create(s->samp, s->device, [&](VkSampler* h) { return vkCreateSampler(s->device, &sci, nullptr, h); })) { setFallback("sampler_failed"); return false; }
    }
    {   // render pass: LOAD color0 + depth (depth is read-only in use; stored unchanged)
        VkImageResource* colorRes = s->colorRes;
        VkImageResource* depthRes = reinterpret_cast<VkImageResource*>(s->depthRes);
        VkAttachmentDescription att[2]{};
        att[0].format = colorRes->format; att[0].samples = VK_SAMPLE_COUNT_1_BIT;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[1].format = depthRes->format; att[1].samples = VK_SAMPLE_COUNT_1_BIT;
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[1].initialLayout = att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &colorRef; sub.pDepthStencilAttachment = &depthRef;
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT; deps[1].dstAccessMask = 0;
        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 2; rpci.pAttachments = att; rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        if (!mc2vk::create(s->rpass, s->device, [&](VkRenderPass* h) { return vkCreateRenderPass(s->device, &rpci, nullptr, h); })) { setFallback("renderpass_failed"); return false; }
        VkImageView fbViews[2] = { colorRes->view.get(), depthRes->view.get() };
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->rpass.get(); fci.attachmentCount = 2; fci.pAttachments = fbViews;
        fci.width = static_cast<uint32_t>(w); fci.height = static_cast<uint32_t>(h); fci.layers = 1;
        if (!mc2vk::create(s->fb, s->device, [&](VkFramebuffer* h) { return vkCreateFramebuffer(s->device, &fci, nullptr, h); })) { setFallback("framebuffer_failed"); return false; }
    }
    {   // descriptor: binding 0 = HDRI sampler
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; lci.bindingCount = 1; lci.pBindings = &b;
        if (!mc2vk::create(s->dsl, s->device, [&](VkDescriptorSetLayout* h) { return vkCreateDescriptorSetLayout(s->device, &lci, nullptr, h); })) { setFallback("dsl_failed"); return false; }
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pci.maxSets = 1; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
        if (!mc2vk::create(s->pool, s->device, [&](VkDescriptorPool* h) { return vkCreateDescriptorPool(s->device, &pci, nullptr, h); })) { setFallback("pool_failed"); return false; }
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        const VkDescriptorSetLayout dslHandle = s->dsl.get();
        dsai.descriptorPool = s->pool.get(); dsai.descriptorSetCount = 1; dsai.pSetLayouts = &dslHandle;
        if (MC2_VK_ALLOC_SETS(s->device, &dsai, &s->set) != VK_SUCCESS) { setFallback("set_failed"); return false; }
    }
    {   // pipeline
        s->vert = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "hdri_sky.vert.spv"));
        s->frag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "hdri_sky.frag.spv"));
        if (!s->vert || !s->frag) { setFallback("spv_load_failed"); return false; }
        VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SkyPC)};
        const VkDescriptorSetLayout dslHandle = s->dsl.get();
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; plci.pSetLayouts = &dslHandle; plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if (!mc2vk::create(s->playout, s->device, [&](VkPipelineLayout* h) { return vkCreatePipelineLayout(s->device, &plci, nullptr, h); })) { setFallback("playout_failed"); return false; }
        VkPipelineVertexInputStateCreateInfo vin{}; vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vps.viewportCount = 1; vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsci{};
        dsci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dsci.dynamicStateCount = 2; dsci.pDynamicStates = dyn;
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = s->vert.get(); stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = s->frag.get(); stages[1].pName = "main";
        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_FALSE; ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2; gp.pStages = stages; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
        gp.pDepthStencilState = &ds; gp.pColorBlendState = &cb; gp.pDynamicState = &dsci;
        gp.layout = s->playout.get(); gp.renderPass = s->rpass.get(); gp.subpass = 0;
        if (!mc2vk::create(s->pipe, s->device, [&](VkPipeline* h) { return mc2vk::infra::graphics(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h); })) { setFallback("pipeline_failed"); return false; }
        mc2vk::nameObject(s->device, s->pipe.get(), "sky.pipe");
    }
    s->inited = true; setFallback("");
    vlog("island initialized %dx%d (HDRI equirect sky -> shared color0; GEQUAL z=0, no depth write).", w, h);
    return true;
}

} // namespace

extern "C" int mc2_vk_sky_island_enabled(void) { return g_sessionDeclined ? 0 : 1; }

extern "C" void mc2_vk_sky_submit(const float* invProj16, const float* invViewRot9, float skyYaw, uint64_t hdriGeneration) {
    if (!invProj16 || !invViewRot9) return;
    std::memcpy(g_pending.invProj, invProj16, sizeof(g_pending.invProj));
    std::memcpy(g_pending.rot, invViewRot9, sizeof(g_pending.rot));
    g_pending.yaw = skyYaw; g_pending.hdriGeneration = hdriGeneration; g_pending.valid = true;
}

extern "C" int mc2_vk_sky_island_prewarm(int w,int h) {
    auto& ctx=mc2vk::getOrCreateEngineVulkanContext();
    if(!ctx.valid() || w<=0 || h<=0) return 0;
    auto& slot=ctx.islandSlot(mc2vk::IslandSlot::Sky);
    if(!slot) {
        auto fresh=std::make_unique<Island>();
        if(!init_island(fresh.get(),w,h)) { ctx.waitIdle("sky_prewarm_failure"); return 0; }
        slot=std::move(fresh);
    }
    return ensure_hdri(static_cast<Island*>(slot.get()), mc2vk::hdriCpuGeneration()) ? 1 : 0;
}

extern "C" int mc2_vk_sky_island_run(int w, int h) {
    ZoneScopedN("VK-sky-island");
    Pending p = g_pending;
    g_pending.valid = false;   // consume: a frame with no sky submit draws no sky
    if (g_sessionDeclined || !p.valid || w <= 0 || h <= 0) return 0;

    // The island lives in the engine context's slot: the context releases it ahead of the device whatever path ends the process.
    mc2vk::VulkanContext& slotCtx = mc2vk::getOrCreateEngineVulkanContext();
    std::unique_ptr<mc2vk::IslandState>& slot = slotCtx.islandSlot(mc2vk::IslandSlot::Sky);
    if (slot) {
        const Island* parked = static_cast<const Island*>(slot.get());
        if (parked->w != w || parked->h != h) slotCtx.retireIsland(mc2vk::IslandSlot::Sky);
    }
    if (!slot) {
        auto fresh = std::make_unique<Island>();
        if (!init_island(fresh.get(), w, h)) {
            const bool permanent = (g_initReachedBuild != 0);
            slotCtx.waitIdle("sky_init_failure");   // objects made before the failure may have been submitted
            if (permanent) { g_sessionDeclined = true; vlog("permanent init failure (%s) -- flat clear sky.", g_health.fallbackReason); }
            return 0;
        }
        slot = std::move(fresh);
    }
    Island* s = static_cast<Island*>(slot.get());
    if (s->disabled || !s->inited) return 0;
    if (!ensure_hdri(s, p.hdriGeneration) || !s->hdriView) { setFallback("hdri_import_failed"); return 0; }

    // GPU-FRONTIER-1: the slot follows the engine frame and the wait is on this island's own previous submit at that slot. The shared scene-color
    // fence ring this replaces flipped per island call, so every island waited on a submit made earlier in the SAME frame.
    mc2vk::VulkanContext& rctx = mc2vk::getOrCreateEngineVulkanContext();
    const uint32_t par = rctx.frameContext().slot;
    { ZoneScopedN("VKIsl.Sky.FenceWait"); mc2::battlebench::WaitScope gpuWait1(mc2::battlebench::WaitGpuMainNs);
      if (!vk_island_util::waitStrike(s->waitStrikes, s->disabled, rctx.wait(s->submitted[par], 5000000000ull, "sky submission slot"), "VK_SKY")) { setFallback("submission_slot_timeout"); vlog("LOUD: submission slot wait timed out (5 s); skipping this frame, will retry next frame."); return 0; } }

    VkCommandBuffer cb = s->cbuf[par];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; rbi.renderPass = s->rpass.get(); rbi.framebuffer = s->fb.get();
    rbi.renderArea = {{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
    MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Sky");
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe.get());
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &s->set, 0, nullptr);
    SkyPC pc{};
    std::memcpy(pc.invProj, p.invProj, sizeof(pc.invProj));
    for (int i = 0; i < 3; ++i) { pc.rotC0[i] = p.rot[i]; pc.rotC1[i] = p.rot[3 + i]; pc.rotC2[i] = p.rot[6 + i]; }
    pc.skyYaw = p.yaw;
    vkCmdPushConstants(cb, s->playout.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    mc2::battlebench::add(mc2::battlebench::DrawSky), vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    gpuScope.end();
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    { ZoneScopedN("VKIsl.Sky.Submit");
      if (mc2vk::submitGraphics(si, VK_NULL_HANDLE) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; } }
    s->submitted[par] = rctx.tracker().now();
    ++g_health.usedVulkan;
    static bool s_firstOk = false;
    if (!s_firstOk) { vlog("first HDRI sky co-write (hdriReal=%d).", s->hdriReal ? 1 : 0); s_firstOk = true; }
    return 1;
}

extern "C" void mc2_vk_sky_island_destroy(void) {
    vlog("used_vulkan=%lu fallback_reason=\"%s\"", g_health.usedVulkan, g_health.fallbackReason);
    mc2vk::getOrCreateEngineVulkanContext().retireIsland(mc2vk::IslandSlot::Sky);
}

#endif // MC2_VULKAN_ISLAND
