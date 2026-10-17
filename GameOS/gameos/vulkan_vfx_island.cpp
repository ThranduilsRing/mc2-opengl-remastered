// vulkan_vfx_island.cpp — VK-FIELD-VFX-1.
//
// Renders the GPU-particle billboard stream into the SHARED scene GBuffer color0
// as translucent, depth-tested (reverse-Z GEQUAL, NO depth write) colored soft
// billboards, so weapon fire (tracers / muzzle flashes / impact glows) survives
// the MC2_VK_SHARED_GBUFFER umbrella. Structurally a thin water island: it borrows
// the shared engine VK device + the same shared color0/normal/depth VkImageResources
// the terrain/water islands co-write, LOADs them, blends the particle billboards on
// top honoring the shared depth, and STOREs — no offscreen target, no composite, no
// GL<->VK interop.
//
// SCOPE (honest): textured — each group samples its sprite page (imported once per
// gos texture handle from the texels the GL path uploads) times the per-particle
// tint. Two blend pipelines (alpha + additive) mirror the GL particle bridge's
// per-group blend. DEFERRED remainder (documented): the tube-ribbon beam path
// (gos_tube_ribbon_flush_deferred), and the gosFX MESH shapes
// (gos_vfx_mesh_flush). The whole TU compiles to nothing unless
// MC2_VULKAN_ISLAND is defined (same discipline as the other island TUs).

#ifdef MC2_VULKAN_ISLAND

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>
#include "Gates/gate.h"
#include "vk_render_pass_timer.h"
#include "Gates/gate_cold.h"   // str<>()

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include "vk_image_resource.h"
#include "vk_island_util.h"
#include "vk_owned.h"
#include "vulkan_context.h"
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"
#include "vk_shared_gbuffer_gate.h"
#include "vulkan_vfx_island.h"
#include "texture_decode.h"   // gos_GetTextureTexels

#include <tracy/Tracy.hpp>   // VK-ISLAND-TRACY-1: per-frame CPU attribution zones

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <cstddef>   // offsetof (M2 layout asserts)
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ── Health snapshot (mirrors the water island's; stderr summary at destroy) ──
struct VfxIslandHealth {
    int   vulkanAvailable = 0;
    int   buildEnabled    = 1;
    char  deviceName[256] = {0};
    unsigned long attempted   = 0;
    unsigned long usedVulkan  = 0;
    char  fallbackReason[256] = {0};
    unsigned long vkDraws     = 0;   // per-group draws issued (last frame)
    unsigned long particles   = 0;   // particles uploaded (last frame)
    unsigned long groups      = 0;
};
VfxIslandHealth g_health;

void setFallback(const char* reason) {
    std::strncpy(g_health.fallbackReason, reason ? reason : "",
                 sizeof(g_health.fallbackReason) - 1);
    g_health.fallbackReason[sizeof(g_health.fallbackReason) - 1] = '\0';
}

void vlog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vk_island_util::vlogv("[VK_VFX] ", fmt, ap);
    va_end(ap);
}

std::string spv_dir() { return vk_island_util::spvDir(); }

VkShaderModule load_spv(VkDevice device, std::string_view path) {
    return vk_island_util::loadSpv(device, path, vlog);
}

// std140 UBO mirror — LOCKSTEP with vfx_particle.vert (VfxVsParams).
struct VfxVsParams {
    float worldToClipGL[16];  // column_major; SAME raw 16 floats as GL GL_FALSE upload   (off 0)
    float camRight[4];        // xyz + pad   (off 64)
    float camUp[4];           // xyz + pad   (off 80)
};
static_assert(sizeof(VfxVsParams) == 96, "VfxVsParams std140 size (vfx_particle.vert)");
static_assert(offsetof(VfxVsParams, camRight) == 64 && offsetof(VfxVsParams, camUp) == 80, "VfxVsParams offsets");

// Push constants: the group's atlas sub-rect — LOCKSTEP with VfxGroupPush in vfx_particle.vert.
struct VfxGroupPush {
    float    uvOffset[2];
    float    uvSize[2];
    uint32_t atlasColumns;
    uint32_t oneOne;
};
static_assert(sizeof(VfxGroupPush) == 24, "VfxGroupPush push-constant size (vfx_particle.vert)");

constexpr uint32_t kMaxSpritePages = 64;   // descriptor-set budget for the sprite pages

// One sprite page. The view is declared after the image so it is released first; `source` keeps the texel
// vector alive so its address stays a valid change marker (a new upload publishes a new vector).
struct SpritePage {
    mc2vk::OwnedImage img;
    mc2vk::OwnedImageView view;
    VkDescriptorSet set = VK_NULL_HANDLE;   // allocated from the island's sprite pool
    std::shared_ptr<const std::vector<uint8_t>> source;
};

struct VfxIsland : mc2vk::IslandState {
    VkInstance       instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys     = VK_NULL_HANDLE;
    VkDevice         device   = VK_NULL_HANDLE;
    VkQueue          queue    = VK_NULL_HANDLE;
    uint32_t         gfxFamily = 0;
    VmaAllocator     allocator = VK_NULL_HANDLE;

    VkImageResource* sColorRes  = nullptr;
    VkImageResource* sNormalRes = nullptr;
    VkImageResource* sDepthRes  = nullptr;

    // The objects the island owns, in creation order: the destructor releases them in reverse (fence first, buffers last), so the island is released
    // by deleting it, after the GPU is idle. The descriptor set and the command buffer belong to their pool.
    mc2vk::OwnedBuffer ubo;
    mc2vk::OwnedBuffer ssbo;
    VkDeviceSize     ssboBytes = 0;
    mc2vk::OwnedRenderPass rpass;
    mc2vk::OwnedFramebuffer fb;
    mc2vk::OwnedDescriptorSetLayout dsl;
    mc2vk::OwnedDescriptorPool pool;
    VkDescriptorSet  set   = VK_NULL_HANDLE;
    mc2vk::OwnedShaderModule vert, frag;
    mc2vk::OwnedSampler samp;
    mc2vk::OwnedDescriptorSetLayout spriteDsl;
    mc2vk::OwnedDescriptorPool spritePool;
    std::unordered_map<uint32_t, SpritePage> pages;   // keyed on gos texture handle; after the pool so pages release first
    // Texel vector whose import failed, per handle: the same vector is not retried (a replaced texture publishes a new one).
    // A transient failure (staging or device OOM) is cached like a bad texture: that handle stays undrawn until its texels are replaced.
    std::unordered_map<uint32_t, std::shared_ptr<const std::vector<uint8_t>>> failedSource;
    bool pageCapLogged = false;
    mc2vk::OwnedPipelineLayout playout;
    mc2vk::OwnedPipeline alphaPipe;
    mc2vk::OwnedPipeline addPipe;
    mc2vk::OwnedPipeline oneOnePipe;   // GL OneOne: (ONE, ONE), alpha ignored
    mc2vk::OwnedCommandPool cpool;
    VkCommandBuffer  cbuf  = VK_NULL_HANDLE;
    mc2vk::OwnedFence fence;

    int  w = 0, h = 0;
    bool inited = false;
    bool disabled = false;
};

bool make_buffer(VfxIsland* s, VkDeviceSize size, VkBufferUsageFlags usage, mc2vk::OwnedBuffer& out, std::string_view name) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size; bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    if (!out.create(s->allocator, s->device, bci, aci, name)) {
        vlog("vmaCreateBuffer failed (size=%llu).", (unsigned long long)size); return false;
    }
    return true;
}

void write_descriptors(VfxIsland* s) {
    if (s->set == VK_NULL_HANDLE) return;
    VkDescriptorBufferInfo uboInfo{ s->ubo.get(),  0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo ssbInfo{ s->ssbo.get(), 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[2]{};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = s->set;
    w[0].dstBinding = 0; w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &uboInfo;
    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = s->set;
    w[1].dstBinding = 1; w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &ssbInfo;
    vkUpdateDescriptorSets(s->device, 2, w, 0, nullptr);
}

// Grow the particle SSBO to at least `bytes`; re-point the descriptor. Never shrinks.
bool ensure_ssbo(VfxIsland* s, VkDeviceSize bytes) {
    if (bytes == 0) bytes = 64;
    if (bytes <= s->ssboBytes && s->ssbo) return true;
    // No device idle: the island retires every frame inline (it waits its own fence right after the submit, VKIsl.Vfx.InlineFenceWait), and no other
    // island reads this buffer or its set, so nothing in flight can still use the old one.
    s->ssbo.reset(); s->ssboBytes = 0;
    VkDeviceSize cap = bytes + bytes / 2;   // 1.5x headroom
    if (!make_buffer(s, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, s->ssbo, "vfx.ssbo")) return false;
    s->ssboBytes = cap;
    write_descriptors(s);
    return true;
}

// Copy one level of RGBA8 / BC7 texels into `page.img` and point `page.set` at it. The copy is queued in the context's upload batch (no CPU wait).
bool upload_page(VfxIsland* s, SpritePage& page, const GosTexels& t) {
    // Sprite texels are sampled as stored (BC7 sRGB read as UNORM): the MLR sprites the VFX arm must match are
    // blended on the displayed values, and decoding to linear here crushed the soft halos to near black.
    const VkFormat fmt = mc2vk::texelFormat(t.vkFormat == mc2tex::kVkFormatBc7Srgb ? mc2tex::kVkFormatBc7Unorm : t.vkFormat);
    mc2vk::VulkanContext& uctx = mc2vk::getOrCreateEngineVulkanContext();
    const mc2vk::UploadBatch::Span stg = uctx.uploads().stage(uctx, t.bytes->size());
    if (!stg) return false;
    std::memcpy(stg.ptr, t.bytes->data(), t.bytes->size());
    if (!page.img.createSampled2D(s->allocator, s->device, fmt, t.width, t.height, 1, 0, "vfx.sprite")) return false;

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = page.img.get(); vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!mc2vk::create(page.view, s->device, [&](VkImageView* h) { return vkCreateImageView(s->device, &vci, nullptr, h); })) return false;

    VkDescriptorImageInfo ii{ s->samp.get(), page.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = page.set; w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
    vkUpdateDescriptorSets(s->device, 1, &w, 0, nullptr);
    // The copy is queued, not waited for: it runs in the upload batch ahead of this frame's VFX submit (or the next submit of any island), and ends
    // with the barrier that makes the page readable by the fragment shader.
    {
        const VkImage img = page.img.get();
        const VkBuffer src = stg.buffer;
        const VkDeviceSize srcOff = stg.offset;
        const uint32_t pw = t.width, ph = t.height;
        uctx.uploads().record([img, src, srcOff, pw, ph](VkCommandBuffer cb) {
            VkImageMemoryBarrier bar{};
            bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bar.image = img;
            bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
            VkBufferImageCopy r{};
            r.bufferOffset = srcOff;
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            r.imageExtent = {pw, ph, 1};
            vkCmdCopyBufferToImage(cb, src, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
            bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        });
    }
    return true;
}

// The sprite page for a gos texture handle: imported on first sight and again when the handle's texels are
// replaced; null while the texture is not resident (the group is skipped this frame, as the GL path does).
// Call before recording the frame.
const SpritePage* get_page(VfxIsland* s, uint32_t handle) {
    if (handle == 0 || handle == 0xffffffffu) return nullptr;
    const GosTexels t = gos_GetTextureTexels(handle);
    if (t.status != GosTexels::Status::Ready || !t.bytes || t.width == 0 || t.height == 0 || t.width > 8192 || t.height > 8192) return nullptr;
    auto it = s->pages.find(handle);
    if (it != s->pages.end() && it->second.source == t.bytes) return &it->second;

    const auto failed = s->failedSource.find(handle);
    if (failed != s->failedSource.end() && failed->second == t.bytes) return nullptr;

    SpritePage page;
    if (it != s->pages.end()) {
        page.set = it->second.set;   // sets live as long as the pool; the replacement reuses the slot
    } else {
        if (s->pages.size() >= kMaxSpritePages) {
            if (!s->pageCapLogged) {
                s->pageCapLogged = true;
                vlog("sprite page cap reached (%u): groups on further textures are not drawn.", kMaxSpritePages);
            }
            return nullptr;
        }
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = s->spritePool.get(); dsai.descriptorSetCount = 1;
        const VkDescriptorSetLayout dsl = s->spriteDsl.get();
        dsai.pSetLayouts = &dsl;
        if (vkAllocateDescriptorSets(s->device, &dsai, &page.set) != VK_SUCCESS) return nullptr;
    }
    if (!upload_page(s, page, t)) {
        vlog("sprite import failed: handle=%u %ux%u.", handle, t.width, t.height);
        s->failedSource[handle] = t.bytes;
        if (it == s->pages.end()) vkFreeDescriptorSets(s->device, s->spritePool.get(), 1, &page.set);   // the failed vector is not retried, and the slot returns to the pool
        return nullptr;
    }
    page.source = t.bytes;
    vlog("sprite page cached: handle=%u %ux%u format=%u (%zu/%u).", handle, t.width, t.height, t.vkFormat,
         s->pages.size() + (it == s->pages.end() ? 1u : 0u), kMaxSpritePages);
    SpritePage& slot = s->pages[handle];
    slot = std::move(page);
    return &slot;
}

// VfxDrawGroup::additive values (LOCKSTEP with ADDITIVE_* in shaders/vulkan/vfx_particle.vert).
enum : uint32_t { VFX_ADD_ALPHA = 0, VFX_ADD_SRC_ALPHA = 1, VFX_ADD_ONE_ONE = 2 };

mc2vk::OwnedPipeline make_pipeline(VfxIsland* s, int w, int h, bool additive, bool oneOne = false) {
    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;  // procedural
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;   // double-sided
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // reverse-Z GEQUAL depth TEST, NO write (translucent) — matches the GL particle bridge.
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    // color0: straight-alpha (FS emits straight rgb,a; src-alpha factor premults in HW).
    //   alpha    = SRC_ALPHA / ONE_MINUS_SRC_ALPHA  (GL alpha groups)
    //   additive = SRC_ALPHA / ONE                  (GL additive groups)
    VkPipelineColorBlendAttachmentState cba[2]{};
    cba[0].blendEnable = VK_TRUE;
    cba[0].srcColorBlendFactor = oneOne ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
    cba[0].dstColorBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba[0].colorBlendOp = VK_BLEND_OP_ADD;
    cba[0].srcAlphaBlendFactor = oneOne ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
    cba[0].dstAlphaBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba[0].alphaBlendOp = VK_BLEND_OP_ADD;
    cba[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba[1].blendEnable = VK_FALSE;   // shared normal target left untouched
    cba[1].colorWriteMask = 0;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 2; cb.pAttachments = cba;
    VkViewport vp{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {(uint32_t)w, (uint32_t)h}};
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &scissor;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = s->vert.get(); stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = s->frag.get(); stages[1].pName = "main";
    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2; gp.pStages = stages;
    gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps; gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds; gp.pColorBlendState = &cb;
    gp.layout = s->playout.get(); gp.renderPass = s->rpass.get(); gp.subpass = 0;
    mc2vk::OwnedPipeline pipe;
    if (!mc2vk::create(pipe, s->device, [&](VkPipeline* out) { return vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, out); })) {
        vlog("vkCreateGraphicsPipelines(%s) failed.", additive ? "additive" : "alpha");
        return {};
    }
    mc2vk::nameObject(s->device, pipe.get(), additive ? "vfx.pipe.additive" : "vfx.pipe.alpha");
    return pipe;
}

bool init_island(VfxIsland* s, int w, int h) {
    s->w = w; s->h = h;
    ++g_health.attempted;

    if (!mc2vk::ensureEngineVulkanContext()) { setFallback("engine_vk_context_birth_failed"); return false; }
    mc2vk::VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    if (!ctx.valid()) { setFallback("engine_vk_context_not_valid"); return false; }
    s->instance = ctx.instance(); s->phys = ctx.physicalDevice(); s->device = ctx.device();
    s->queue = ctx.graphicsQueue(); s->gfxFamily = ctx.graphicsFamily(); s->allocator = ctx.allocator();
    if (s->device == VK_NULL_HANDLE || s->allocator == VK_NULL_HANDLE) {
        setFallback("engine_vk_device_or_allocator_null"); return false;
    }
    g_health.vulkanAvailable = 1;
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(s->phys, &props);
        std::strncpy(g_health.deviceName, props.deviceName, sizeof(g_health.deviceName) - 1);
    }

    // Borrow the shared scene GBuffer (color0 + normal + depth). Not ready => decline.
    s->sColorRes  = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
    s->sNormalRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_NORMAL);
    s->sDepthRes  = mc2SharedSceneDepthGetResource();
    if (!s->sColorRes || !s->sNormalRes || !s->sDepthRes) { setFallback("shared_gbuffer_not_ready"); return false; }
    // Not-ready guard: struct present but view/format not yet valid (boot warmup race).
    if (s->sColorRes->view.get() == VK_NULL_HANDLE || s->sColorRes->format == VK_FORMAT_UNDEFINED ||
        s->sNormalRes->view.get() == VK_NULL_HANDLE || s->sDepthRes->view.get() == VK_NULL_HANDLE ||
        s->sDepthRes->format == VK_FORMAT_UNDEFINED) {
        setFallback("shared_gbuffer_views_not_ready"); return false;
    }

    // UBO + initial particle SSBO.
    if (!make_buffer(s, sizeof(VfxVsParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, s->ubo, "vfx.ubo")) return false;
    if (!ensure_ssbo(s, 64 * 1024)) return false;   // ~1024 particles to start

    // Render pass: color0 (LOAD, blend, STORE) + normal (LOAD, STORE, writemask 0) +
    // depth (LOAD, GEQUAL test, NO write, STORE unchanged).
    {
        VkAttachmentDescription att[3]{};
        att[0].format = s->sColorRes->format; att[0].samples = VK_SAMPLE_COUNT_1_BIT;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[0].finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[1].format = s->sNormalRes->format; att[1].samples = VK_SAMPLE_COUNT_1_BIT;
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[1].finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[2].format = s->sDepthRes->format; att[2].samples = VK_SAMPLE_COUNT_1_BIT;
        att[2].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        att[2].finalLayout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorRefs[2]{
            {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
            {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
        VkAttachmentReference depthRef{2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 2; sub.pColorAttachments = colorRefs;
        sub.pDepthStencilAttachment = &depthRef;
        // Incoming external dep — order this frame's blend AFTER the upstream opaque/water
        // writes to the shared color+depth (same WAW pattern as the water island). Depth is
        // READ only here (no write), so dst depth access is READ.
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        deps[1].srcSubpass = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        deps[1].dstAccessMask = 0;
        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 3; rpci.pAttachments = att;
        rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        if (!mc2vk::create(s->rpass, s->device, [&](VkRenderPass* o) { return vkCreateRenderPass(s->device, &rpci, nullptr, o); })) { vlog("vkCreateRenderPass failed."); return false; }
        VkImageView fbViews[3] = { s->sColorRes->view.get(), s->sNormalRes->view.get(), s->sDepthRes->view.get() };
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->rpass.get(); fci.attachmentCount = 3; fci.pAttachments = fbViews;
        fci.width = (uint32_t)w; fci.height = (uint32_t)h; fci.layers = 1;
        if (!mc2vk::create(s->fb, s->device, [&](VkFramebuffer* o) { return vkCreateFramebuffer(s->device, &fci, nullptr, o); })) { vlog("vkCreateFramebuffer failed."); return false; }
    }

    // Descriptor set: UBO(0, vert) + particle SSBO(1, vert).
    {
        VkDescriptorSetLayoutBinding b[2]{};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 2; lci.pBindings = b;
        if (!mc2vk::create(s->dsl, s->device, [&](VkDescriptorSetLayout* o) { return vkCreateDescriptorSetLayout(s->device, &lci, nullptr, o); })) { vlog("vkCreateDescriptorSetLayout failed."); return false; }
        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sizes[0].descriptorCount = 1;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sizes[1].descriptorCount = 1;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 1; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
        if (!mc2vk::create(s->pool, s->device, [&](VkDescriptorPool* o) { return vkCreateDescriptorPool(s->device, &pci, nullptr, o); })) { vlog("vkCreateDescriptorPool failed."); return false; }
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = s->pool.get(); dsai.descriptorSetCount = 1; const VkDescriptorSetLayout dslHandle = s->dsl.get(); dsai.pSetLayouts = &dslHandle;
        if (vkAllocateDescriptorSets(s->device, &dsai, &s->set) != VK_SUCCESS) { vlog("vkAllocateDescriptorSets failed."); return false; }
        write_descriptors(s);
    }

    // Pipelines (alpha + additive).
    {
        s->vert = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "vfx_particle.vert.spv"));
        s->frag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "vfx_particle.frag.spv"));
        if (!s->vert || !s->frag) { setFallback("spv_load_failed"); return false; }
        // Sprite pages: set 1 = one combined sampler (LINEAR, no mips -- the FS samples level 0 like the GL path).
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        if (!mc2vk::create(s->samp, s->device, [&](VkSampler* o) { return vkCreateSampler(s->device, &sci, nullptr, o); })) { vlog("vkCreateSampler failed."); return false; }
        VkDescriptorSetLayoutBinding sb{};
        sb.binding = 0; sb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sb.descriptorCount = 1; sb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo slci{};
        slci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        slci.bindingCount = 1; slci.pBindings = &sb;
        if (!mc2vk::create(s->spriteDsl, s->device, [&](VkDescriptorSetLayout* o) { return vkCreateDescriptorSetLayout(s->device, &slci, nullptr, o); })) { vlog("vkCreateDescriptorSetLayout(sprite) failed."); return false; }
        VkDescriptorPoolSize spriteSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxSpritePages };
        VkDescriptorPoolCreateInfo spci{};
        spci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        spci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        spci.maxSets = kMaxSpritePages; spci.poolSizeCount = 1; spci.pPoolSizes = &spriteSize;
        if (!mc2vk::create(s->spritePool, s->device, [&](VkDescriptorPool* o) { return vkCreateDescriptorPool(s->device, &spci, nullptr, o); })) { vlog("vkCreateDescriptorPool(sprite) failed."); return false; }

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        const VkDescriptorSetLayout dslHandles[2] = { s->dsl.get(), s->spriteDsl.get() };
        plci.setLayoutCount = 2; plci.pSetLayouts = dslHandles;
        const VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(VfxGroupPush) };
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if (!mc2vk::create(s->playout, s->device, [&](VkPipelineLayout* o) { return vkCreatePipelineLayout(s->device, &plci, nullptr, o); })) { vlog("vkCreatePipelineLayout failed."); return false; }
        s->alphaPipe = make_pipeline(s, w, h, /*additive=*/false);
        s->addPipe   = make_pipeline(s, w, h, /*additive=*/true);
        s->oneOnePipe = make_pipeline(s, w, h, /*additive=*/true, /*oneOne=*/true);
        if (!s->alphaPipe || !s->addPipe || !s->oneOnePipe) { setFallback("pipeline_create_failed"); return false; }
    }

    // Command pool + one command buffer + fence (inline-wait each frame).
    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s->gfxFamily;
        if (!mc2vk::create(s->cpool, s->device, [&](VkCommandPool* o) { return vkCreateCommandPool(s->device, &pci, nullptr, o); })) { vlog("vkCreateCommandPool failed."); return false; }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = s->cpool.get(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(s->device, &cai, &s->cbuf) != VK_SUCCESS) { vlog("vkAllocateCommandBuffers failed."); return false; }
        VkFenceCreateInfo fnci{}; fnci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (!mc2vk::create(s->fence, s->device, [&](VkFence* o) { return vkCreateFence(s->device, &fnci, nullptr, o); })) { vlog("vkCreateFence failed."); return false; }
    }

    s->inited = true;
    setFallback("");
    vlog("island initialized %dx%d (shared scene GBuffer co-write; procedural billboard VS; "
         "GEQUAL reverse-Z depth-test no-write; alpha+additive blend; textured sprite pages).", w, h);
    return true;
}

// Gate: ON under the umbrella (content that would VANISH otherwise).
bool vfxGateOn() {
    const bool s_on = mc2vkgate::umbrellaOn();   // per-call: VK-FAILSOFT-DISARM-1
    return s_on;
}

}  // namespace

// ===========================================================================
// PUBLIC ENTRY POINTS
// ===========================================================================
extern "C" int mc2_vk_vfx_island_enabled() { return vfxGateOn() ? 1 : 0; }

extern "C" int mc2_vk_vfx_island_run(
    const void* particles, uint32_t particleCount,
    const VfxParticleDraw* draws, uint32_t drawCount,
    const float* worldToClipGL, const float* camRight, const float* camUp,
    int w, int h) {
    if (!vfxGateOn()) return 0;
    if (particleCount == 0 || drawCount == 0 || particles == nullptr || draws == nullptr) return 0;
    if (w <= 0 || h <= 0 || worldToClipGL == nullptr) return 0;
    ZoneScopedN("VKIsl.Vfx.Run");   // VK-ISLAND-TRACY-1 per-call total (CPU; incl. ALWAYS-inline fence wait)

    // The island lives in the engine context's slot: the context releases it ahead of the device whatever path ends the process.
    mc2vk::VulkanContext& slotCtx = mc2vk::getOrCreateEngineVulkanContext();
    std::unique_ptr<mc2vk::IslandState>& slot = slotCtx.islandSlot(mc2vk::IslandSlot::Vfx);
    if (slot) {
        const VfxIsland* parked = static_cast<const VfxIsland*>(slot.get());
        if (parked->w != w || parked->h != h) slotCtx.retireIsland(mc2vk::IslandSlot::Vfx);
    }
    if (!slot) {
        auto fresh = std::make_unique<VfxIsland>();
        if (!init_island(fresh.get(), w, h)) {
            vlog("init failed (%s) — VFX declined this frame.", g_health.fallbackReason);
            slotCtx.waitIdle("vfx_init_failure");   // objects made before the failure may have been submitted
            return 0;
        }
        slot = std::move(fresh);
    }
    VfxIsland* s = static_cast<VfxIsland*>(slot.get());
    if (s->disabled || !s->inited) return 0;

    // Upload the whole particle array (64B/record) + frame constants.
    const VkDeviceSize needBytes = (VkDeviceSize)particleCount * 64u;
    if (!ensure_ssbo(s, needBytes)) { s->disabled = true; return 0; }
    {
        ZoneScopedN("VKIsl.Vfx.Feed");   // VK-ISLAND-TRACY-1: particle SSBO staging
        if (void* p = s->ssbo.map()) {
            std::memcpy(p, particles, (size_t)needBytes);
            s->ssbo.unmap();
        }
    }
    {
        if (void* p = s->ubo.map()) {
            VfxVsParams up{};
            std::memcpy(up.worldToClipGL, worldToClipGL, sizeof(up.worldToClipGL));
            if (camRight) { up.camRight[0]=camRight[0]; up.camRight[1]=camRight[1]; up.camRight[2]=camRight[2]; }
            if (camUp)    { up.camUp[0]=camUp[0];       up.camUp[1]=camUp[1];       up.camUp[2]=camUp[2]; }
            else          { up.camUp[1]=1.0f; }
            if (!camRight) up.camRight[0]=1.0f;
            std::memcpy(p, &up, sizeof(up));
            s->ubo.unmap();
        }
    }

    // Resolve every group's sprite page first: an import records and waits on its own submit.
    std::vector<const SpritePage*> groupPage(drawCount, nullptr);
    for (uint32_t gi = 0; gi < drawCount; ++gi) {
        if (draws[gi].count != 0) groupPage[gi] = get_page(s, draws[gi].texture);
    }
    if (s->disabled) return 0;

    // Record: one renderpass, per-group pipeline + sub-range draw.
    vkResetCommandBuffer(s->cbuf, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(s->cbuf, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }
    vk_render_pass_timer::Begin(s->cbuf, gos_render_pass_timer::Pass_AlphaVfx);
    vk_render_pass_timer::CancelUnlessSubmitted timerGuard(gos_render_pass_timer::Pass_AlphaVfx);
    VkClearValue clears[3]{};   // all LOAD; ignored
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = s->rpass.get(); rbi.framebuffer = s->fb.get();
    rbi.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
    rbi.clearValueCount = 3; rbi.pClearValues = clears;
    vkCmdBeginRenderPass(s->cbuf, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindDescriptorSets(s->cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &s->set, 0, nullptr);
    unsigned long groupsDrawn = 0;
    VkPipeline bound = VK_NULL_HANDLE;
    for (uint32_t gi = 0; gi < drawCount; ++gi) {
        const VfxParticleDraw& d = draws[gi];
        if (d.count == 0) continue;
        if ((uint64_t)d.start + d.count > particleCount) continue;   // clamp bad ranges
        if (!groupPage[gi]) continue;   // no resident texture: skipped, as the GL path does
        VkPipeline want = d.additive == VFX_ADD_ONE_ONE ? s->oneOnePipe.get() : d.additive == VFX_ADD_SRC_ALPHA ? s->addPipe.get() : s->alphaPipe.get();
        if (want != bound) { vkCmdBindPipeline(s->cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, want); bound = want; }
        vkCmdBindDescriptorSets(s->cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 1, 1, &groupPage[gi]->set, 0, nullptr);
        const VfxGroupPush push{ {d.u0, d.v0}, {d.us, d.vs}, d.atlasColumns, d.additive == VFX_ADD_ONE_ONE ? 1u : 0u };
        vkCmdPushConstants(s->cbuf, s->playout.get(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        vkCmdDraw(s->cbuf, d.count * 6u, 1, d.start * 6u, 0);
        ++groupsDrawn;
    }
    vkCmdEndRenderPass(s->cbuf);
    vk_render_pass_timer::End(s->cbuf, gos_render_pass_timer::Pass_AlphaVfx);
    if (vkEndCommandBuffer(s->cbuf) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

    if (groupsDrawn == 0) return 0;   // nothing drawable this frame: the recorded buffer is not submitted (the guard cancels its bracket)

    const VkFence fence = s->fence.get();
    vkResetFences(s->device, 1, &fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &s->cbuf;
    { ZoneScopedN("VKIsl.Vfx.Submit");   // VK-ISLAND-TRACY-1: vkQueueSubmit
      if (mc2vk::submitGraphics(si, fence) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; } }
    timerGuard.submitted();
    { ZoneScopedN("VKIsl.Vfx.InlineFenceWait");   // VK-ISLAND-TRACY-1: ALWAYS-inline retire (never fence-deferred)
      if (vkWaitForFences(s->device, 1, &fence, VK_TRUE, 5ull*1000*1000*1000) != VK_SUCCESS) {
          setFallback("fence_timeout"); s->disabled = true; return 0;
      } }

    g_health.vkDraws = groupsDrawn;
    g_health.particles = particleCount;
    g_health.groups = drawCount;
    ++g_health.usedVulkan;
    static bool s_firstOk = false;
    if (!s_firstOk) {
        s_firstOk = true;
        vlog("first frame OK: particles=%u groups=%u draws=%lu (in-place color0 co-write, "
             "shared depth GEQUAL test, no depth write, no composite).",
             particleCount, drawCount, groupsDrawn);
    }
    return 1;
}

extern "C" void mc2_vk_vfx_island_destroy() {
    vlog("[VK_VFX] vulkan_available=%d used_vulkan=%lu vk_draws=%lu particles=%lu groups=%lu "
         "fallback_reason=\"%s\"",
         g_health.vulkanAvailable, g_health.usedVulkan, g_health.vkDraws,
         g_health.particles, g_health.groups, g_health.fallbackReason);
    mc2vk::getOrCreateEngineVulkanContext().retireIsland(mc2vk::IslandSlot::Vfx);
}

#endif  // MC2_VULKAN_ISLAND
