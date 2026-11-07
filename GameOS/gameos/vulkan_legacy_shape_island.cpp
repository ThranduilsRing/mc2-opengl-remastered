#include "../../RenderCore/TextureService.h"
// vulkan_legacy_shape_island.cpp -- VK-PARITY-F1: VK owner of the legacy MLR
// solid-object vertex loop (ground vehicles etc.). See the header for the why;
// structure = vulkan_mech_island.cpp's shared-GBuffer co-write path (LOAD-preserve
// MRT0 color + MRT1 normal + shared D32S8 depth, GEQUAL reverse-Z, no composite,
// ticket-retired submit slots) with a much simpler feed:
//   * geometry: the node's CPU gos_VERTEX array (pre-transformed screen-space,
//     CPU-lit) is memcpy'd into the engine frame ring;
//   * textures: copy-once import of the handle's decoded texels, keyed on the mc2::textures::ready() node;
//   * no lights / no instancing: vertex colour is already CPU-lit.
// The whole TU compiles to nothing unless MC2_VULKAN_ISLAND is defined.

#ifdef MC2_VULKAN_ISLAND

#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include "ui_texture.h"
#include "texture_decode.h"   // gos_GetTextureTexels

#include <vk_mem_alloc.h>

#include "vk_image_resource.h"
#include "vk_island_util.h"
#include "vk_one_shot.h"
#include "vulkan_context.h"
#include "vk_gpu_profile.h"   // VK-GPU-ZONES: MC2_VK_GPU_SCOPE
#include "vk_texture_residency.h"
#include "vulkan_legacy_shape_island.h"
#include "vulkan_mech_island.h"   // mech-lab panel: mc2_vk_mech_preview_flush
#include "../../RenderCore/gpu_abi/GpuLegacyFrameUBO.h"  // single-source per-frame UBO
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
#include <unordered_map>
#include <unordered_set>
#include <tracy/Tracy.hpp>
#include "../../mclib/battle_bench_wait.h"

const float* gos_GetProj2ScreenMat4();             // gosRenderer::projection_ (row-major, GL_TRUE)

namespace {

struct Health {
    unsigned long attempted = 0, usedVulkan = 0, nodesDrawn = 0, vertsDrawn = 0, texUploads = 0;
    char fallbackReason[128] = {0};
};
Health g_health;

void setFallback(const char* r) {
    std::strncpy(g_health.fallbackReason, r ? r : "", sizeof(g_health.fallbackReason) - 1);
    g_health.fallbackReason[sizeof(g_health.fallbackReason) - 1] = 0;
}

void vlog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vk_island_util::vlogv("[VK_LEGACY_SHAPE] ", fmt, ap);
    va_end(ap);
}

std::string spv_dir() { return vk_island_util::spvDir(); }

VkShaderModule load_spv(VkDevice device, std::string_view path) {
    return vk_island_util::loadSpv(device, path, vlog);
}

// The per-frame std140 UBO (LegacyFrameUBO) is single-sourced in
// RenderCore/gpu_abi/GpuLegacyFrameUBO.h; its generated block feeds both legacy shaders.

const uint32_t kVertexStride = 32;       // gos_VERTEX: x,y,z,rhw, argb, frgb, u, v
const uint32_t kMaxTextures  = 192;
// Solid, blob-shadow and alpha draws can each submit in both in-flight frames.
const uint32_t kSubmissionSlots = 6;

// The view is declared after the image so it is released first.
struct TexEntry {
    mc2vk::ResidentTextureRef texture;
    VkImageView bound[kSubmissionSlots] = {};
    VkDescriptorSet set[kSubmissionSlots] = {};   // allocated from the island's pool, freed with it
};

// Owned objects are declared in creation order and released in reverse by the destructor, while the device is alive (the engine context releases the
// parked island ahead of the device).
struct Island : mc2vk::IslandState {
    bool inited = false, disabled = false, preview = false, previewPublished = false;
    mc2vk::GpuTicket previewTicket;
    mc2vk::ResidentTextureRef previewColor;
    mc2vk::OwnedImage previewNormal, previewDepth;
    mc2vk::OwnedImageView previewNormalView, previewDepthView;
    int  w = 0, h = 0;
    VkInstance instance = VK_NULL_HANDLE; VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE; uint32_t gfxFamily = UINT32_MAX;
    VkQueue queue = VK_NULL_HANDLE; VmaAllocator allocator = VK_NULL_HANDLE;

    struct VkImageResource* colorRes = nullptr;
    struct VkImageResource* normalRes = nullptr;
    struct VkImageResource* depthRes = nullptr;

    // Per-frame data (the frame UBO, then the vertex stream) lives in the engine frame ring; every texture set binds the UBO with a dynamic offset.
    // ringBound[par] = the ring buffer binding 0 of every [par] set currently points at; a frame whose span came from another buffer (the ring grew), or a
    // set created since, re-points them after the submission slot retired.
    VkBuffer ringBound[kSubmissionSlots] = {};
    VkCommandBuffer cbuf[kSubmissionSlots] = {};   // allocated from `cpool`, freed with it
    mc2vk::GpuTicket submitted[kSubmissionSlots];
    uint32_t nextSlot = 0;

    mc2vk::OwnedCommandPool cpool[kSubmissionSlots];   // one pool per slot: vkResetCommandPool per frame instead of per-cb reset
    mc2vk::OwnedSampler samp;
    mc2vk::OwnedRenderPass rpass;
    mc2vk::OwnedFramebuffer fb;
    mc2vk::OwnedDescriptorSetLayout dsl;
    mc2vk::OwnedDescriptorPool pool;
    TexEntry dummyTex;                                  // 1x1 white (untextured / failed import)
    std::unordered_map<uint32_t, TexEntry> texs;        // keyed on mc2::textures::ready() node
    mc2vk::OwnedShaderModule vert, frag;
    mc2vk::OwnedPipelineLayout playout;
    mc2vk::OwnedPipeline pipes[MC2_LEGACY_PASS_COUNT];
};

bool g_previewDispatch=false, g_previewActive=false;
int g_previewW=800,g_previewH=600;
std::vector<Mc2LegacyMlrDraw> g_previewPending;
std::vector<std::vector<uint8_t>> g_previewVertices;

// The island lives in the engine context's slot: the context releases it ahead of the device whatever path ends the process.
std::unique_ptr<mc2vk::IslandState>& islandSlot() {
    return mc2vk::getOrCreateEngineVulkanContext().islandSlot(g_previewDispatch ? mc2vk::IslandSlot::Preview : mc2vk::IslandSlot::LegacyShape);
}
Island* parkedIsland() { return static_cast<Island*>(islandSlot().get()); }
bool g_sessionDeclined = false;
int  g_initReachedBuild = 0;
std::vector<Mc2LegacyMlrDraw> g_pending;

// One level of RGBA8 or BC7 texels, copied as they are into a sampled image of `fmt`.
bool upload_image(Island* s, TexEntry& e, uint32_t w, uint32_t h, VkFormat fmt, const void* bytes, VkDeviceSize byteCount) {
    // Explicit legacy producer boundary: CPU/gos bytes stay legacy-owned; the service owns their Vulkan image.
    auto& textures=mc2vk::getOrCreateEngineVulkanContext().textures();
    e.texture=textures.acquireLegacy(w,h,fmt,bytes,static_cast<size_t>(byteCount));
    return static_cast<bool>(e.texture);
}

// Per-submission descriptor set for one texture: 0 = frame UBO, 1 = albedo.
bool alloc_tex_sets(Island* s, TexEntry& t) {
    VkDescriptorSetLayout layouts[kSubmissionSlots];
    for (auto& layout : layouts) layout = s->dsl.get();
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = s->pool.get(); dsai.descriptorSetCount = kSubmissionSlots; dsai.pSetLayouts = layouts;
    if (MC2_VK_ALLOC_SETS(s->device, &dsai, t.set) != VK_SUCCESS) return false;
    for (uint32_t par = 0; par < kSubmissionSlots; ++par) {
        VkDescriptorImageInfo ii{};
        ii.sampler = s->samp.get(); ii.imageView = mc2vk::getOrCreateEngineVulkanContext().textures().view(t.texture.get()); ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w[1]{};
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = t.set[par]; w[0].dstBinding = 1;
        w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &ii;
        vkUpdateDescriptorSets(s->device, 1, w, 0, nullptr);
        t.bound[par]=ii.imageView;
        // Binding 0 follows the ring buffer when this submission slot is reused.
        if (const VkBuffer ring = mc2vk::getOrCreateEngineVulkanContext().frameRingBuffer())
            mc2vk::writeDynamicBinding(s->device, t.set[par], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, ring, sizeof(LegacyFrameUBO));
        s->ringBound[par] = VK_NULL_HANDLE;
    }
    return true;
}

// World GV/variant materials share the mech source/repaint adapter, including mip regeneration.
// Preview uses the same image identity with its sampler clamped to level zero.
TexEntry* get_tex(Island* s,uint32_t texNode,uint32_t gosHandle) {
    if (!gosHandle || gosHandle==0xffffffffu) return &s->dummyTex;
    auto& textures=mc2vk::getOrCreateEngineVulkanContext().textures();
    const auto handle=RenderCore::uiTextureHandle(gos_GetMaterialTextureId(gosHandle));
    if (!handle.isValid()) return &s->dummyTex;
    if (auto it=s->texs.find(texNode);it!=s->texs.end()) {
        if (it->second.texture.get()!=handle) {
            it->second.texture=textures.retain(handle);
            for (auto& bound : it->second.bound) bound = VK_NULL_HANDLE; // recycled VkImageView names still require fresh descriptors
        }
        return &it->second;
    }
    if (s->texs.size()>=kMaxTextures) return &s->dummyTex;
    TexEntry entry;entry.texture=textures.retain(handle);
    if (!entry.texture || !alloc_tex_sets(s,entry)) return &s->dummyTex;
    ++g_health.texUploads;
    return &s->texs.emplace(texNode,std::move(entry)).first->second;
}

bool init_island(Island* s, int w, int h) {
    s->w = w; s->h = h;
    ++g_health.attempted;
    g_initReachedBuild = 0;

    if (!mc2vk::ensureEngineVulkanContext()) { setFallback("engine_vk_context_birth_failed"); return false; }
    mc2vk::VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    if (!ctx.valid()) { setFallback("engine_vk_context_not_valid"); return false; }
    s->instance  = ctx.instance();
    s->phys      = ctx.physicalDevice();
    s->device    = ctx.device();
    s->queue     = ctx.graphicsQueue();
    s->gfxFamily = ctx.graphicsFamily();
    s->allocator = ctx.allocator();
    if (s->device == VK_NULL_HANDLE || s->allocator == VK_NULL_HANDLE) { setFallback("engine_vk_device_or_allocator_null"); return false; }

    s->colorRes  = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
    s->normalRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_NORMAL);
    s->depthRes  = mc2SharedSceneDepthGetResource();
    if (!s->preview && (!s->colorRes || !s->normalRes || !s->depthRes)) { setFallback("shared_gbuffer_not_ready"); return false; }
    if (s->preview) {
        s->previewColor=ctx.textures().createRenderTarget(static_cast<uint32_t>(w),static_cast<uint32_t>(h));
        if (!s->previewColor) return false;
        if (!s->previewNormal.createSampled2D(ctx.allocator(),s->device,VK_FORMAT_R8G8B8A8_UNORM,w,h,1,
                                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,"ui.preview.normal") ||
            !s->previewDepth.createSampled2D(ctx.allocator(),s->device,VK_FORMAT_D32_SFLOAT,w,h,1,
                                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,"ui.preview.depth")) return false;
        auto view=[&](VkImage image,VkFormat format,VkImageAspectFlags aspect,mc2vk::OwnedImageView& out) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=image;
            vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=format; vi.subresourceRange={aspect,0,1,0,1};
            return mc2vk::create(out,s->device,[&](VkImageView* v){return vkCreateImageView(s->device,&vi,nullptr,v);});
        };
        if (!view(s->previewNormal.get(),VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_ASPECT_COLOR_BIT,s->previewNormalView) ||
            !view(s->previewDepth.get(),VK_FORMAT_D32_SFLOAT,VK_IMAGE_ASPECT_DEPTH_BIT,s->previewDepthView)) return false;
    }
    g_initReachedBuild = 1;   // readiness gates passed: failures past here are PERMANENT

    {   // command pool + per-submission command buffers
        VkCommandPoolCreateInfo cpi{};
        cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpi.flags = 0;
        cpi.queueFamilyIndex = s->gfxFamily;
        for (uint32_t i = 0; i < kSubmissionSlots; ++i) {
            if (!mc2vk::create(s->cpool[i], s->device, [&](VkCommandPool* h) { return vkCreateCommandPool(s->device, &cpi, nullptr, h); })) { setFallback("cmdpool_failed"); return false; }
            VkCommandBufferAllocateInfo cbi{};
            cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cbi.commandPool = s->cpool[i].get(); cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(s->device, &cbi, &s->cbuf[i]) != VK_SUCCESS) { setFallback("cmdbuf_failed"); return false; }
        }
    }

    {   // World materials use full regenerated mips; preview stays clamped to level zero.
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; sci.maxLod=s->preview ? 0.0f : VK_LOD_CLAMP_NONE;
        if (!s->preview) mc2vk::infra::materialSampler(sci);
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        if (!mc2vk::create(s->samp, s->device, [&](VkSampler* h) { return vkCreateSampler(s->device, &sci, nullptr, h); })) { setFallback("sampler_failed"); return false; }
    }

    {   // render pass + framebuffer: LOAD color0/normal/depth (same as the mech island)
        VkImageResource* colorRes  = reinterpret_cast<VkImageResource*>(s->colorRes);
        VkImageResource* normalRes = reinterpret_cast<VkImageResource*>(s->normalRes);
        VkImageResource* depthRes  = reinterpret_cast<VkImageResource*>(s->depthRes);
        VkAttachmentDescription att[3]{};
        att[0].format = s->preview ? VK_FORMAT_R8G8B8A8_UNORM : colorRes->format;  att[0].samples = VK_SAMPLE_COUNT_1_BIT;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[1].format = s->preview ? VK_FORMAT_R8G8B8A8_UNORM : normalRes->format; att[1].samples = VK_SAMPLE_COUNT_1_BIT;
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; att[1].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[2].format = s->preview ? VK_FORMAT_D32_SFLOAT : depthRes->format;  att[2].samples = VK_SAMPLE_COUNT_1_BIT;
        att[2].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; att[2].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        if (s->preview) {
            for (auto& a : att) { a.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; a.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; }
            att[0].finalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        VkAttachmentReference colorRefs[2] = {
            {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL} };
        VkAttachmentReference depthRef{2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 2; sub.pColorAttachments = colorRefs;
        sub.pDepthStencilAttachment = &depthRef;

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
        deps[1].dstStageMask = s->preview ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        deps[1].dstAccessMask = s->preview ? VK_ACCESS_SHADER_READ_BIT : 0;

        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 3; rpci.pAttachments = att;
        rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        if (!mc2vk::create(s->rpass, s->device, [&](VkRenderPass* h) { return vkCreateRenderPass(s->device, &rpci, nullptr, h); })) { setFallback("renderpass_failed"); return false; }
        VkImageView fbViews[3] = {s->preview ? ctx.textures().view(s->previewColor.get()) : colorRes->view.get(),
            s->preview ? s->previewNormalView.get() : normalRes->view.get(),
            s->preview ? s->previewDepthView.get() : depthRes->view.get()};
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->rpass.get(); fci.attachmentCount = 3; fci.pAttachments = fbViews;
        fci.width = static_cast<uint32_t>(w); fci.height = static_cast<uint32_t>(h); fci.layers = 1;
        if (!mc2vk::create(s->fb, s->device, [&](VkFramebuffer* h) { return vkCreateFramebuffer(s->device, &fci, nullptr, h); })) { setFallback("framebuffer_failed"); return false; }
    }

    {   // descriptor layout + pool, dummy white texture
        VkDescriptorSetLayoutBinding b[2]{};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 2; lci.pBindings = b;
        if (!mc2vk::create(s->dsl, s->device, [&](VkDescriptorSetLayout* h) { return vkCreateDescriptorSetLayout(s->device, &lci, nullptr, h); })) { setFallback("dsl_failed"); return false; }
        const uint32_t N = kMaxTextures + 1;   // + dummy
        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; sizes[0].descriptorCount = kSubmissionSlots * N;
        sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[1].descriptorCount = kSubmissionSlots * N;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = kSubmissionSlots * N; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
        if (!mc2vk::create(s->pool, s->device, [&](VkDescriptorPool* h) { return vkCreateDescriptorPool(s->device, &pci, nullptr, h); })) { setFallback("pool_failed"); return false; }

        const uint8_t px[4] = {255, 255, 255, 255};
        if (!upload_image(s, s->dummyTex, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, px, sizeof(px)) || !alloc_tex_sets(s, s->dummyTex)) {
            setFallback("dummy_tex_failed"); return false;
        }
    }

    {   // pipeline
        s->vert = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "legacy_shape_tier1.vert.spv"));
        s->frag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "legacy_shape_tier1.frag.spv"));
        if (!s->vert || !s->frag) { setFallback("spv_load_failed"); return false; }

        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; pcr.offset = 0; pcr.size = 4;
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        const VkDescriptorSetLayout dslHandle = s->dsl.get();
        plci.setLayoutCount = 1; plci.pSetLayouts = &dslHandle;
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if (!mc2vk::create(s->playout, s->device, [&](VkPipelineLayout* h) { return vkCreatePipelineLayout(s->device, &plci, nullptr, h); })) { setFallback("playout_failed"); return false; }

        // gos_VERTEX (32B): pos vec4 (x,y,z,rhw)@0, argb u8x4 unorm@16, frgb u8x4 unorm@20, uv vec2@24.
        VkVertexInputBindingDescription vib{};
        vib.binding = 0; vib.stride = kVertexStride; vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription via[4]{};
        via[0].location = 0; via[0].binding = 0; via[0].format = VK_FORMAT_R32G32B32A32_SFLOAT; via[0].offset = 0;
        via[1].location = 1; via[1].binding = 0; via[1].format = VK_FORMAT_R8G8B8A8_UNORM;      via[1].offset = 16;
        via[2].location = 2; via[2].binding = 0; via[2].format = VK_FORMAT_R8G8B8A8_UNORM;      via[2].offset = 20;
        via[3].location = 3; via[3].binding = 0; via[3].format = VK_FORMAT_R32G32_SFLOAT;       via[3].offset = 24;
        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vib;
        vin.vertexAttributeDescriptionCount = 4; vin.pVertexAttributeDescriptions = via;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1; vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsci{};
        dsci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dsci.dynamicStateCount = 2; dsci.pDynamicStates = dyn;
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = s->vert.get(); stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = s->frag.get(); stages[1].pName = "main";

        // One pipeline per legacy loop (gosRenderer state each loop sets in renderLists):
        //   SOLID : OneZero (no blend), depth write, ZCompare 1 = GEQUAL  (reverse-Z)
        //   ALPHA : AlphaInvAlpha,      no write,    GEQUAL
        //   SHADOW: AlphaInvAlpha,      no write,    ZCompare 2 = GREATER
        //   ADD   : OneOne,             no write,    GEQUAL
        // Blended passes mask MRT1 (GBuffer normal) off so they never smear the normal buffer.
        for (int k = 0; k < MC2_LEGACY_PASS_COUNT; ++k) {
            VkPipelineDepthStencilStateCreateInfo ds{};
            ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            ds.depthTestEnable = VK_TRUE;
            ds.depthWriteEnable = (k == MC2_LEGACY_PASS_SOLID) ? VK_TRUE : VK_FALSE;
            ds.depthCompareOp = (k == MC2_LEGACY_PASS_SHADOW) ? VK_COMPARE_OP_GREATER
                                                              : VK_COMPARE_OP_GREATER_OR_EQUAL;
            VkPipelineColorBlendAttachmentState cba[2]{};
            const VkColorComponentFlags all = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            cba[0].colorWriteMask = all;
            cba[1].colorWriteMask = (k == MC2_LEGACY_PASS_SOLID) ? all : 0;
            if (k == MC2_LEGACY_PASS_ALPHA || k == MC2_LEGACY_PASS_SHADOW) {
                cba[0].blendEnable = VK_TRUE;
                cba[0].srcColorBlendFactor = cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                cba[0].dstColorBlendFactor = cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                cba[0].colorBlendOp = cba[0].alphaBlendOp = VK_BLEND_OP_ADD;
            } else if (k == MC2_LEGACY_PASS_ADD) {
                cba[0].blendEnable = VK_TRUE;
                cba[0].srcColorBlendFactor = cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                cba[0].dstColorBlendFactor = cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                cba[0].colorBlendOp = cba[0].alphaBlendOp = VK_BLEND_OP_ADD;
            }
            VkPipelineColorBlendStateCreateInfo cb{};
            cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            cb.attachmentCount = 2; cb.pAttachments = cba;
            VkGraphicsPipelineCreateInfo gp{};
            gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            gp.stageCount = 2; gp.pStages = stages;
            gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
            gp.pViewportState = &vps; gp.pRasterizationState = &rs;
            gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds; gp.pColorBlendState = &cb;
            gp.pDynamicState = &dsci;
            gp.layout = s->playout.get(); gp.renderPass = s->rpass.get(); gp.subpass = 0;
            if (!mc2vk::create(s->pipes[k], s->device, [&](VkPipeline* h) { return mc2vk::infra::graphics(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h); })) {
                setFallback("pipeline_failed"); return false;
            }
            mc2vk::nameObjectAt(s->device, s->pipes[k].get(), "legacyshape.pipe", static_cast<int>(k));
        }
    }

    s->inited = true;
    setFallback("");
    vlog("island initialized %dx%d (legacy MLR solid loop -> shared color(MRT0)+normal(MRT1)+depth; GEQUAL reverse-Z).", w, h);
    return true;
}

} // namespace

// ===========================================================================
// PUBLIC ENTRY POINTS.
// ===========================================================================

extern "C" int mc2_vk_legacy_shape_island_enabled() {
    return g_sessionDeclined ? 0 : 1;
}

extern "C" void mc2_vk_legacy_shape_submit(const Mc2LegacyMlrDraw* d) {
    if (d && d->vertices && d->vertexCount >= 3) g_pending.push_back(*d);
}

extern "C" int mc2_vk_legacy_shape_island_prewarm(int w,int h) {
    auto& ctx=mc2vk::getOrCreateEngineVulkanContext();
    if(!ctx.valid() || w<=0 || h<=0) return 0;
    auto& slot=ctx.islandSlot(mc2vk::IslandSlot::LegacyShape);
    if(slot) return 1;
    auto fresh=std::make_unique<Island>();
    if(!init_island(fresh.get(),w,h)) { ctx.waitIdle("legacy_shape_prewarm_failure"); return 0; }
    slot=std::move(fresh);
    return 1;
}

extern "C" int mc2_vk_legacy_shape_island_run(int w, int h, const Mc2LegacyMlrFrame* frame) {
    ZoneScopedN("VK-legacy-shape-island");
    // Always consume the queue (even on failure) so it can never grow across frames.
    std::vector<Mc2LegacyMlrDraw> draws;
    draws.swap(g_previewDispatch ? g_previewPending : g_pending);
    if ((!g_previewDispatch && g_sessionDeclined) || !frame) return 0;
    if ((!g_previewDispatch && draws.empty()) || w <= 0 || h <= 0) return 0;

    mc2vk::VulkanContext& slotCtx = mc2vk::getOrCreateEngineVulkanContext();
    if (const Island* parked = parkedIsland(); parked && (parked->w != w || parked->h != h ||
        (parked->preview && !RenderCore::textureService().isLive(parked->previewColor.get())))) slotCtx.retireIsland(g_previewDispatch ? mc2vk::IslandSlot::Preview : mc2vk::IslandSlot::LegacyShape);
    if (!islandSlot()) {
        auto fresh = std::make_unique<Island>();
        fresh->preview=g_previewDispatch;
        if (!init_island(fresh.get(), w, h)) {
            const bool permanent = (g_initReachedBuild != 0);
            slotCtx.waitIdle("legacy_shape_init_failure");   // objects made before the failure may have been submitted
            if (permanent) {
                if (!g_previewDispatch) g_sessionDeclined = true;
                vlog("boot-level decline: permanent init failure (%s) -- legacy shapes will not draw under VK.",
                     g_health.fallbackReason);
            } else {
                static bool s_logged = false;
                if (!s_logged) { vlog("init not ready (%s) -- will retry.", g_health.fallbackReason); s_logged = true; }
            }
            return 0;
        }
        islandSlot() = std::move(fresh);
    }
    Island* s = parkedIsland();
    if (s->disabled || !s->inited) return 0;

    // Resolve textures BEFORE recording; total the vertex bytes.
    struct Resolved { TexEntry* t; const Mc2LegacyMlrDraw* d; VkDeviceSize byteOff; };
    std::vector<Resolved> list;
    list.reserve(draws.size());
    VkDeviceSize totalBytes = 0;
    for (const Mc2LegacyMlrDraw& d : draws) {
        TexEntry* t = get_tex(s, d.texNode, d.gosTexHandle);
        list.push_back({t ? t : &s->dummyTex, &d, totalBytes});
        totalBytes += static_cast<VkDeviceSize>(d.vertexCount) * kVertexStride;
    }

    // The frame UBO and vertex stream belong to the engine frame ring, independently of the submission slot.
    mc2vk::VulkanContext& rctx = mc2vk::getOrCreateEngineVulkanContext();
    const VkDeviceSize vtxOff = mc2vk::alignUp(sizeof(LegacyFrameUBO), rctx.frameAlignment());
    const mc2vk::FrameAlloc span = rctx.frameAlloc(vtxOff + (totalBytes ? totalBytes : kVertexStride));
    if (!span) { s->disabled = true; setFallback("frame_ring_alloc_failed"); return 0; }

    if (s->preview) {
        // The UI samples this target after the preview submit; wait the preceding sealed frame before overwriting it.
        const auto frameNo=rctx.tracker().frameNo();
        const auto sampled=frameNo>1 ? rctx.tracker().frameTicket(frameNo-1) : mc2vk::GpuTicket{};
        if (!rctx.wait(mc2vk::GpuTicket::both(sampled,s->previewTicket),5000000000ull,"preview target reuse")) return 0;
    }
    // The GBuffer's two fences are shared by all islands. Cycling them here can
    // wait on terrain/props submitted earlier in THIS frame. Only this slot's
    // prior legacy submit must retire before its command buffer and sets change;
    // render-pass dependencies still order the shared attachment accesses.
    const uint32_t par = s->nextSlot;
    {
        ZoneScopedN("VKIsl.Legacy.FenceWait"); mc2::battlebench::WaitScope gpuWait1(mc2::battlebench::WaitGpuMainNs);
        if (!rctx.wait(s->submitted[par], 5000000000ull, "legacy submission slot")) {
            setFallback("submission_slot_timeout"); s->disabled = true; return 0;
        }
    }

    {   // per-frame vertex stream + UBO
        uint8_t* const base = static_cast<uint8_t*>(span.ptr);
        for (const Resolved& r : list)
            std::memcpy(base + vtxOff + r.byteOff, r.d->vertices, static_cast<size_t>(r.d->vertexCount) * kVertexStride);
        LegacyFrameUBO u{};
        const float* proj = gos_GetProj2ScreenMat4();
        if (proj) std::memcpy(u.proj, proj, sizeof(u.proj));
        if (s->preview) { std::memset(u.proj,0,sizeof(u.proj)); u.proj[0]=2.0f/800; u.proj[3]=-1;
            u.proj[5]=-2.0f/600; u.proj[7]=1; u.proj[10]=1; u.proj[15]=1; }
        for (int k = 0; k < 4; ++k) u.fogColor[k] = frame->fogColor[k];
        std::memcpy(base, &u, sizeof(u));
    }
    for (const auto& r : list) {
        const auto view=rctx.textures().view(r.t->texture.get());
        if (r.t->bound[par]==view) continue;
        VkDescriptorImageInfo ii{s->samp.get(),view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet=r.t->set[par]; w.dstBinding=1; w.descriptorCount=1;
        w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo=&ii;
        vkUpdateDescriptorSets(s->device,1,&w,0,nullptr); r.t->bound[par]=view;
    }
    // Every texture set of this slot follows the ring buffer the span came from; the draw passes the UBO offset.
    if (s->ringBound[par] != span.buffer) {
        for (auto& kv : s->texs)
            if (kv.second.set[par] != VK_NULL_HANDLE) mc2vk::writeDynamicBinding(s->device, kv.second.set[par], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, span.buffer, sizeof(LegacyFrameUBO));
        if (s->dummyTex.set[par] != VK_NULL_HANDLE) mc2vk::writeDynamicBinding(s->device, s->dummyTex.set[par], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, span.buffer, sizeof(LegacyFrameUBO));
        s->ringBound[par] = span.buffer;
    }
    const uint32_t uboOffset = static_cast<uint32_t>(span.offset);

    VkCommandBuffer cb = s->cbuf[par];
    vkResetCommandPool(s->device, s->cpool[par].get(), 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = s->rpass.get(); rbi.framebuffer = s->fb.get();
    rbi.renderArea = {{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
    VkClearValue clears[3]{}; clears[0].color.float32[3]=1;
    clears[1].color.float32[0]=0.5f; clears[1].color.float32[1]=0.5f; clears[1].color.float32[2]=1;
    if (s->preview) { rbi.clearValueCount=3; rbi.pClearValues=clears; }
    MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Legacy");
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    VkDeviceSize vertexOffset = span.offset + vtxOff;
    uint32_t boundPass = 0xffffffffu;
    vkCmdBindVertexBuffers(cb, 0, 1, &span.buffer, &vertexOffset);

    VkDescriptorSet boundSet = VK_NULL_HANDLE;
    unsigned long drawn = 0, verts = 0;
    // Draw order is the blend order, so only ADJACENT entries merge: same texture set, pass and alpha-test flag, with the vertex ranges abutting.
    // One vkCmdDraw then covers exactly the vertices the separate draws would have, in the same order.
    uint32_t pendFirst = 0, pendCount = 0;
    auto flushPending = [&]() {
        if (pendCount == 0) return;
        mc2::battlebench::add(mc2::battlebench::DrawLegacy), vkCmdDraw(cb, pendCount, 1, pendFirst, 0);
        pendCount = 0;
    };
    const Resolved* prevR = nullptr;
    for (const Resolved& r : list) {
        const uint32_t firstVtx = static_cast<uint32_t>((r.byteOff / kVertexStride));
        if (prevR && pendCount != 0 && r.t->set[par] == prevR->t->set[par] && r.d->pass == prevR->d->pass && r.d->alphaTest == prevR->d->alphaTest &&
            firstVtx == pendFirst + pendCount) {
            pendCount += r.d->vertexCount; ++drawn; verts += r.d->vertexCount; prevR = &r;
            continue;
        }
        flushPending();
        VkDescriptorSet set = r.t->set[par];
        if (set != boundSet) {
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &set, 1, &uboOffset);
            boundSet = set;
        }
        const uint32_t pass = (r.d->pass < MC2_LEGACY_PASS_COUNT) ? r.d->pass : static_cast<uint32_t>(MC2_LEGACY_PASS_SOLID);
        if (pass != boundPass) {
            mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipes[pass].get());
            boundPass = pass;
        }
        // FS flags: bit0 alpha test, bit1 fog off (additive loops clear gos_State_Fog), bit2 opaque alpha.
        const uint32_t pflags = (r.d->alphaTest ? 1u : 0u) |
                                (pass == MC2_LEGACY_PASS_ADD ? 2u : 0u) |
                                (pass == MC2_LEGACY_PASS_SOLID ? 4u : 0u);
        vkCmdPushConstants(cb, s->playout.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &pflags);
        pendFirst = firstVtx; pendCount = r.d->vertexCount; prevR = &r;
        ++drawn; verts += r.d->vertexCount;
    }
    flushPending();
    vkCmdEndRenderPass(cb);
    gpuScope.end();
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    { ZoneScopedN("VKIsl.Legacy.Submit");
      if (mc2vk::submitGraphics(si, VK_NULL_HANDLE) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; } }
    s->submitted[par] = rctx.tracker().now();
    s->nextSlot = (par + 1) % kSubmissionSlots;

    if (s->preview) {
        // The lab's GPU mech (the in-game material path) LOADs over this pass in the same target. Its submit follows
        // ours on the queue, so the reuse ticket taken after it covers both.
        (void)mc2_vk_mech_preview_flush(rctx.textures().view(s->previewColor.get()),s->previewNormalView.get(),
                                        s->previewDepthView.get(),w,h);
        s->previewPublished=true; s->previewTicket=rctx.tracker().now();
    }
    g_health.nodesDrawn += drawn;
    g_health.vertsDrawn += verts;
    ++g_health.usedVulkan;
    static bool s_firstOk = false;
    if (!s_firstOk) {
        vlog("first legacy-shape co-write: nodes=%lu verts=%lu textures=%zu.", drawn, verts, s->texs.size());
        s_firstOk = true;
    }
    return 1;
}

extern "C" int mc2_vk_preview_active() { return g_previewActive ? 1 : 0; }
// Batch scope: several cameras (mech lab: mech + component picture) share one preview texture; draws accumulate
// and one pass runs when the last hold releases.
static int g_previewHold=0;
extern "C" void mc2_vk_preview_hold(int delta) {
    if (delta>0) { if (g_previewHold++==0) { g_previewPending.clear(); g_previewVertices.clear(); } return; }
    if (g_previewHold>0 && --g_previewHold==0) {
        g_previewDispatch=true;
        Mc2LegacyMlrFrame frame{};
        mc2_vk_legacy_shape_island_run(g_previewW?g_previewW:800,g_previewH?g_previewH:600,&frame);
        g_previewDispatch=false; g_previewVertices.clear();
    }
}
extern "C" void mc2_vk_preview_begin(int width,int height) {
    g_previewW=width; g_previewH=height;
    if (g_previewHold==0) { g_previewPending.clear(); g_previewVertices.clear(); }
    g_previewActive=true;
}
extern "C" void mc2_vk_preview_capture(const Mc2LegacyMlrDraw* draw) {
    if (!draw || !draw->vertices || draw->vertexCount<3) return;
    auto& bytes=g_previewVertices.emplace_back(static_cast<size_t>(draw->vertexCount)*kVertexStride);
    std::memcpy(bytes.data(),draw->vertices,bytes.size());
    auto d=*draw; d.vertices=bytes.data(); g_previewPending.push_back(d);
}
extern "C" void mc2_vk_preview_end() {
    g_previewActive=false;
    if (g_previewHold>0) return;
    g_previewDispatch=true;
    Mc2LegacyMlrFrame frame{};
    mc2_vk_legacy_shape_island_run(g_previewW,g_previewH,&frame);
    g_previewDispatch=false; g_previewVertices.clear();
}
extern "C" uint64_t mc2_vk_preview_texture() {
    auto* island=static_cast<Island*>(mc2vk::getOrCreateEngineVulkanContext().islandSlot(mc2vk::IslandSlot::Preview).get());
    return island && island->inited && island->previewPublished ? RenderCore::uiTextureId(island->previewColor.get()) : 0;
}

extern "C" void mc2_vk_legacy_shape_island_reset() {
    g_pending.clear(); g_previewPending.clear(); g_previewVertices.clear();
    auto& ctx=mc2vk::getOrCreateEngineVulkanContext();
    const auto legacy=mc2vk::IslandSlot::LegacyShape, preview=mc2vk::IslandSlot::Preview;
    auto* world=static_cast<Island*>(ctx.islandSlot(legacy).get());
    auto* bay=static_cast<Island*>(ctx.islandSlot(preview).get());
    if ((!world || !world->inited) && (!bay || !bay->inited)) return;
    ctx.waitIdle("legacy_shape_reset");
    for (const auto slot : {mc2vk::IslandSlot::LegacyShape, mc2vk::IslandSlot::Preview}) {
        auto* s=static_cast<Island*>(ctx.islandSlot(slot).get());
        if (!s || !s->inited) continue;
        const auto before=s->texs.size();
        s->texs.clear();
        // Neither pool frees individual sets: reset it and restore its dummy pair.
        if (s->pool) vkResetDescriptorPool(s->device,s->pool.get(),0);
        s->dummyTex.set[0]=s->dummyTex.set[1]=VK_NULL_HANDLE;
        s->ringBound[0]=s->ringBound[1]=VK_NULL_HANDLE;
        s->previewPublished=false;
        if (!alloc_tex_sets(s,s->dummyTex)) { s->disabled=true; vlog("reset: dummy sets failed -- island disabled."); }
        vlog("texture cache reset slot=%s entries_before=%zu entries_after=%zu",s->preview ? "preview" : "legacy",before,s->texs.size());
    }
}

extern "C" void mc2_vk_legacy_shape_island_destroy() {
    vlog("used_vulkan=%lu nodes_drawn=%lu verts_drawn=%lu tex_uploads=%lu fallback_reason=\"%s\"",
         g_health.usedVulkan, g_health.nodesDrawn, g_health.vertsDrawn, g_health.texUploads,
         g_health.fallbackReason);
    mc2vk::getOrCreateEngineVulkanContext().retireIsland(g_previewDispatch ? mc2vk::IslandSlot::Preview : mc2vk::IslandSlot::LegacyShape);
}

#endif // MC2_VULKAN_ISLAND
