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
// per-group blend. Native geometry also covers tube ribbons and effect meshes.
// The whole TU compiles to nothing unless
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
#include "vulkan_vfx_geometry.h"
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
    unsigned long nativeMeshDraws = 0, nativeTubeDraws = 0, nativeVegetationDraws = 0;
};
VfxIslandHealth g_health;
GosTexels vegetationAtlas;

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
    mc2vk::OwnedRenderPass rpass;
    mc2vk::OwnedFramebuffer fb;
    mc2vk::OwnedDescriptorSetLayout dsl;
    mc2vk::OwnedDescriptorPool pool;
    // One set per parity slot; both bind the engine frame ring with dynamic offsets (0 = frame constants, 1 = particles). ringBound[par][binding] = the ring
    // buffer that binding of set[par] currently points at; a frame whose span came from another buffer (the ring grew) re-points it after the slot's last
    // submit completed.
    VkDescriptorSet  set[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkBuffer         ringBound[2][2] = {};
    mc2vk::OwnedShaderModule vert, frag;
    mc2vk::OwnedSampler samp;
    mc2vk::OwnedSampler vegetationSampler;
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
    mc2vk::OwnedPipeline vegetationPipe;
    mc2vk::OwnedPipeline oneOnePipe;   // GL OneOne: (ONE, ONE), alpha ignored
    mc2vk::OwnedCommandPool cpool;
    VkCommandBuffer  cbuf[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // one per parity slot
    uint64_t         ticket[2] = {0, 0};                           // timeline value of the slot's last submit (0 = never submitted)
    uint32_t         nextSlot = 0;

    int  w = 0, h = 0;
    bool inited = false;
    bool disabled = false;
};

// The ring bindings of one parity set: 0 = frame constants (uniform, explicit range), 1 = particles (storage, the ring's dynamic range).
void write_ring_bindings(VfxIsland* s, uint32_t par, VkBuffer buf, VkDeviceSize range) {
    if (buf == VK_NULL_HANDLE || s->set[par] == VK_NULL_HANDLE) return;
    if (s->ringBound[par][0] != buf) {
        mc2vk::writeDynamicBinding(s->device, s->set[par], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, buf, sizeof(VfxVsParams));
        s->ringBound[par][0] = buf;
    }
    if (s->ringBound[par][1] != buf) {
        mc2vk::writeDynamicBinding(s->device, s->set[par], 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, buf, range);
        s->ringBound[par][1] = buf;
    }
}

// A sprite page that left the cache (its texels were replaced): the descriptor set returns to the sprite pool and the image goes once the frames that
// may sample them have completed.
struct RetiredSprite {
    VkDevice dev = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    SpritePage page;
    RetiredSprite(VkDevice d, VkDescriptorPool p, SpritePage&& pg) : dev(d), pool(p), page(std::move(pg)) {}
    RetiredSprite(RetiredSprite&& o) noexcept : dev(o.dev), pool(o.pool), page(std::move(o.page)) { o.page.set = VK_NULL_HANDLE; }
    RetiredSprite(const RetiredSprite&) = delete;
    ~RetiredSprite() { if (page.set != VK_NULL_HANDLE) vkFreeDescriptorSets(dev, pool, 1, &page.set); }
};

// Copy one level of RGBA8 / BC7 texels into `page.img` and point `page.set` at it. The copy is queued in the context's upload batch (no CPU wait).
bool upload_page(VfxIsland* s, SpritePage& page, const GosTexels& t, bool vegetation) {
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

    VkDescriptorImageInfo ii{ vegetation ? s->vegetationSampler.get() : s->samp.get(), page.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
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
    if (handle == 0xffffffffu) return nullptr;
    GosTexels t;
    if (handle == kVfxVegetationAtlas) t = vegetationAtlas;
    else if (handle == 0) {
        static const auto white = std::make_shared<const std::vector<uint8_t>>(4, 255u);
        t.status = GosTexels::Status::Ready;
        t.width = t.height = 1;
        t.vkFormat = mc2tex::kVkFormatRgba8Unorm;
        t.bytes = white;
    } else t = gos_GetTextureTexels(handle);
    if (t.status != GosTexels::Status::Ready || !t.bytes || t.width == 0 || t.height == 0 || t.width > 8192 || t.height > 8192) return nullptr;
    auto it = s->pages.find(handle);
    if (it != s->pages.end() && it->second.source == t.bytes) return &it->second;

    const auto failed = s->failedSource.find(handle);
    if (failed != s->failedSource.end() && failed->second == t.bytes) return nullptr;

    SpritePage page;
    // A replacement takes a fresh set: the old page may still be sampled by a frame in flight, so neither its set nor its image is touched; the old
    // page retires below. (A new page needs a free slot of the pool; a replacement frees one when the old page retires, so it may use the same budget.)
    if (it == s->pages.end() && s->pages.size() >= kMaxSpritePages) {
        if (!s->pageCapLogged) {
            s->pageCapLogged = true;
            vlog("sprite page cap reached (%u): groups on further textures are not drawn.", kMaxSpritePages);
        }
        return nullptr;
    }
    {
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = s->spritePool.get(); dsai.descriptorSetCount = 1;
        const VkDescriptorSetLayout dsl = s->spriteDsl.get();
        dsai.pSetLayouts = &dsl;
        if (vkAllocateDescriptorSets(s->device, &dsai, &page.set) != VK_SUCCESS) return nullptr;
    }
    if (!upload_page(s, page, t, handle == kVfxVegetationAtlas)) {
        vlog("sprite import failed: handle=%u %ux%u.", handle, t.width, t.height);
        s->failedSource[handle] = t.bytes;
        vkFreeDescriptorSets(s->device, s->spritePool.get(), 1, &page.set);   // the failed vector is not retried, and the slot returns to the pool
        page.set = VK_NULL_HANDLE;
        return nullptr;
    }
    if (it != s->pages.end())
        mc2vk::getOrCreateEngineVulkanContext().retire(RetiredSprite(s->device, s->spritePool.get(), std::move(it->second)));
    page.source = t.bytes;
    vlog("sprite page cached: handle=%u %ux%u format=%u (%zu/%u).", handle, t.width, t.height, t.vkFormat,
         s->pages.size() + (it == s->pages.end() ? 1u : 0u), kMaxSpritePages);
    SpritePage& slot = s->pages[handle];
    slot = std::move(page);
    return &slot;
}

// VfxDrawGroup::additive values (LOCKSTEP with ADDITIVE_* in shaders/vulkan/vfx_particle.vert).
enum : uint32_t { VFX_ADD_ALPHA = 0, VFX_ADD_SRC_ALPHA = 1, VFX_ADD_ONE_ONE = 2 };

mc2vk::OwnedPipeline make_pipeline(VfxIsland* s, int w, int h, bool additive, bool oneOne = false, bool vegetation = false) {
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
    ds.depthCompareOp = vegetation ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_GREATER_OR_EQUAL;
    // color0: straight-alpha (FS emits straight rgb,a; src-alpha factor premults in HW).
    //   alpha    = SRC_ALPHA / ONE_MINUS_SRC_ALPHA  (GL alpha groups)
    //   additive = SRC_ALPHA / ONE                  (GL additive groups)
    VkPipelineColorBlendAttachmentState cba[2]{};
    cba[0].blendEnable = vegetation ? VK_FALSE : VK_TRUE;
    cba[0].srcColorBlendFactor = oneOne ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
    cba[0].dstColorBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba[0].colorBlendOp = VK_BLEND_OP_ADD;
    cba[0].srcAlphaBlendFactor = oneOne ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
    cba[0].dstAlphaBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba[0].alphaBlendOp = VK_BLEND_OP_ADD;
    cba[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba[1].blendEnable = VK_FALSE;   // shared normal target left untouched
    cba[1].colorWriteMask = vegetation ? cba[0].colorWriteMask : 0;
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
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 2; lci.pBindings = b;
        if (!mc2vk::create(s->dsl, s->device, [&](VkDescriptorSetLayout* o) { return vkCreateDescriptorSetLayout(s->device, &lci, nullptr, o); })) { vlog("vkCreateDescriptorSetLayout failed."); return false; }
        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; sizes[0].descriptorCount = 2;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC; sizes[1].descriptorCount = 2;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 2; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
        if (!mc2vk::create(s->pool, s->device, [&](VkDescriptorPool* o) { return vkCreateDescriptorPool(s->device, &pci, nullptr, o); })) { vlog("vkCreateDescriptorPool failed."); return false; }
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        const VkDescriptorSetLayout dslHandles2[2] = { s->dsl.get(), s->dsl.get() };
        dsai.descriptorPool = s->pool.get(); dsai.descriptorSetCount = 2; dsai.pSetLayouts = dslHandles2;
        if (vkAllocateDescriptorSets(s->device, &dsai, s->set) != VK_SUCCESS) { vlog("vkAllocateDescriptorSets failed."); return false; }
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
        sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        if (!mc2vk::create(s->vegetationSampler, s->device, [&](VkSampler* o) { return vkCreateSampler(s->device, &sci, nullptr, o); })) return false;

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
        s->vegetationPipe = make_pipeline(s, w, h, false, false, true);
        s->alphaPipe = make_pipeline(s, w, h, /*additive=*/false);
        s->addPipe   = make_pipeline(s, w, h, /*additive=*/true);
        s->oneOnePipe = make_pipeline(s, w, h, /*additive=*/true, /*oneOne=*/true);
        if (!s->alphaPipe || !s->addPipe || !s->oneOnePipe || !s->vegetationPipe) { setFallback("pipeline_create_failed"); return false; }
    }

    // Command pool + two command buffers (one per parity slot; the slot is reused once its last submit completed, a timeline ticket, no island fence).
    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s->gfxFamily;
        if (!mc2vk::create(s->cpool, s->device, [&](VkCommandPool* o) { return vkCreateCommandPool(s->device, &pci, nullptr, o); })) { vlog("vkCreateCommandPool failed."); return false; }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = s->cpool.get(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 2;
        if (vkAllocateCommandBuffers(s->device, &cai, s->cbuf) != VK_SUCCESS) { vlog("vkAllocateCommandBuffers failed."); return false; }
    }

    s->inited = true;
    setFallback("");
    vlog("island initialized %dx%d (shared scene GBuffer co-write; procedural billboard VS; "
         "GEQUAL reverse-Z depth-test no-write; alpha+additive blend; textured sprite pages).", w, h);
    return true;
}

}  // namespace

// ===========================================================================
// PUBLIC ENTRY POINTS
// ===========================================================================
extern "C" int mc2_vk_vfx_island_enabled() { return 1; }

extern "C" int mc2_vk_vfx_island_run(
    const void* particles, uint32_t particleCount,
    const VfxParticleDraw* draws, uint32_t drawCount,
    const float* worldToClipGL, const float* camRight, const float* camUp,
    int w, int h) {
    if (particleCount == 0 || drawCount == 0 || particles == nullptr || draws == nullptr) return 0;
    if (w <= 0 || h <= 0 || worldToClipGL == nullptr) return 0;
    ZoneScopedN("VKIsl.Vfx.Run");   // VK-ISLAND-TRACY-1 per-call total (CPU)

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

    // Per-frame data goes into the engine frame ring: frame constants (UBO) then the whole particle array (64B/record), one span. The parity slot's last
    // submit must have completed before its command buffer and set are reused: a timeline ticket, not an island fence.
    const uint32_t par = s->nextSlot;
    s->nextSlot ^= 1u;
    if (!slotCtx.wait(mc2vk::GpuTicket{s->ticket[par], 0}, 5ull * 1000 * 1000 * 1000, "vfx_slot")) { setFallback("slot_timeout"); s->disabled = true; return 0; }
    const VkDeviceSize needBytes = (VkDeviceSize)particleCount * 64u;
    const VkDeviceSize partOff = mc2vk::alignUp(sizeof(VfxVsParams), slotCtx.frameAlignment());
    const mc2vk::FrameAlloc span = slotCtx.frameAlloc(partOff + needBytes);
    if (!span) { setFallback("frame_ring_alloc_failed"); s->disabled = true; return 0; }
    {
        ZoneScopedN("VKIsl.Vfx.Feed");   // VK-ISLAND-TRACY-1: particle staging
        std::memcpy(static_cast<uint8_t*>(span.ptr) + partOff, particles, (size_t)needBytes);
    }
    {
        VfxVsParams up{};
        std::memcpy(up.worldToClipGL, worldToClipGL, sizeof(up.worldToClipGL));
        if (camRight) { up.camRight[0]=camRight[0]; up.camRight[1]=camRight[1]; up.camRight[2]=camRight[2]; }
        if (camUp)    { up.camUp[0]=camUp[0];       up.camUp[1]=camUp[1];       up.camUp[2]=camUp[2]; }
        else          { up.camUp[1]=1.0f; }
        if (!camRight) up.camRight[0]=1.0f;
        std::memcpy(span.ptr, &up, sizeof(up));
    }
    write_ring_bindings(s, par, span.buffer, span.range);
    const uint32_t dynOffsets[2] = {static_cast<uint32_t>(span.offset), static_cast<uint32_t>(span.offset + partOff)};

    // Resolve every group's sprite page first: an import records and waits on its own submit.
    std::vector<const SpritePage*> groupPage(drawCount, nullptr);
    for (uint32_t gi = 0; gi < drawCount; ++gi) {
        if (draws[gi].count != 0 && (draws[gi].texture != 0 || draws[gi].atlasColumns == kVfxGeometryAtlas))
            groupPage[gi] = get_page(s, draws[gi].texture);
        // The mesh bridge drew unresolved textures as white; preserve that fallback.
        if (!groupPage[gi] && draws[gi].atlasColumns == kVfxGeometryAtlas && draws[gi].start < particleCount &&
            (static_cast<const VfxGeometryVertex*>(particles)[draws[gi].start].flags & VfxGeometryMesh) != 0u)
            groupPage[gi] = get_page(s, 0);
    }
    if (s->disabled) return 0;

    // Record: one renderpass, per-group pipeline + sub-range draw.
    VkCommandBuffer cb = s->cbuf[par];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }
    vk_render_pass_timer::Begin(cb, gos_render_pass_timer::Pass_AlphaVfx);
    vk_render_pass_timer::CancelUnlessSubmitted timerGuard(gos_render_pass_timer::Pass_AlphaVfx);
    VkClearValue clears[3]{};   // all LOAD; ignored
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = s->rpass.get(); rbi.framebuffer = s->fb.get();
    rbi.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
    rbi.clearValueCount = 3; rbi.pClearValues = clears;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &s->set[par], 2, dynOffsets);
    unsigned long groupsDrawn = 0, meshDraws = 0, tubeDraws = 0, vegetationDraws = 0;
    VkPipeline bound = VK_NULL_HANDLE;
    for (uint32_t gi = 0; gi < drawCount; ++gi) {
        const VfxParticleDraw& d = draws[gi];
        if (d.count == 0) continue;
        if ((uint64_t)d.start + d.count > particleCount) continue;   // clamp bad ranges
        if (!groupPage[gi]) continue;   // no resident texture: skipped, as the GL path does
        VkPipeline want = d.atlasColumns == kVfxVegetationAtlas ? s->vegetationPipe.get() : d.additive == VFX_ADD_ONE_ONE ? s->oneOnePipe.get() : d.additive == VFX_ADD_SRC_ALPHA ? s->addPipe.get() : s->alphaPipe.get();
        if (want != bound) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, want); bound = want; }
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 1, 1, &groupPage[gi]->set, 0, nullptr);
        const VfxGroupPush push{ {d.u0, d.v0}, {d.us, d.vs}, d.atlasColumns, d.additive == VFX_ADD_ONE_ONE ? 1u : 0u };
        vkCmdPushConstants(cb, s->playout.get(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        const uint32_t expansion = d.atlasColumns == kVfxVegetationAtlas ? 36u : d.atlasColumns == kVfxGeometryAtlas ? 1u : 6u;
        vkCmdDraw(cb, d.count * expansion, 1, d.start * expansion, 0);
        ++groupsDrawn;
        if (d.atlasColumns == kVfxVegetationAtlas) ++vegetationDraws;
        else if (d.atlasColumns == kVfxGeometryAtlas) {
            const auto flags = static_cast<const VfxGeometryVertex*>(particles)[d.start].flags;
            if ((flags & VfxGeometryMesh) != 0u) ++meshDraws;
            if ((flags & VfxGeometryTube) != 0u) ++tubeDraws;
        }
    }
    vkCmdEndRenderPass(cb);
    vk_render_pass_timer::End(cb, gos_render_pass_timer::Pass_AlphaVfx);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

    if (groupsDrawn == 0) return 0;   // nothing drawable this frame: the recorded buffer is not submitted (the guard cancels its bracket)

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    { ZoneScopedN("VKIsl.Vfx.Submit");   // VK-ISLAND-TRACY-1: vkQueueSubmit
      uint64_t value = 0;
      if (slotCtx.submit(si, VK_NULL_HANDLE, &value) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; }
      s->ticket[par] = value; }
    timerGuard.submitted();

    if (meshDraws && !g_health.nativeMeshDraws) vlog("native_geometry kind=mesh draws=%lu", meshDraws);
    if (tubeDraws && !g_health.nativeTubeDraws) vlog("native_geometry kind=tube draws=%lu", tubeDraws);
    if (vegetationDraws && !g_health.nativeVegetationDraws) vlog("native_geometry kind=vegetation draws=%lu", vegetationDraws);
    g_health.nativeMeshDraws += meshDraws;
    g_health.nativeTubeDraws += tubeDraws;
    g_health.nativeVegetationDraws += vegetationDraws;
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

void mc2_vk_vfx_vegetation_atlas(const GosTexels& atlas) { vegetationAtlas = atlas; }

extern "C" void mc2_vk_vfx_island_destroy() {
    vlog("native_mesh_draws=%lu native_tube_draws=%lu native_vegetation_draws=%lu",
         g_health.nativeMeshDraws, g_health.nativeTubeDraws, g_health.nativeVegetationDraws);
    vlog("[VK_VFX] vulkan_available=%d used_vulkan=%lu vk_draws=%lu particles=%lu groups=%lu "
         "fallback_reason=\"%s\"",
         g_health.vulkanAvailable, g_health.usedVulkan, g_health.vkDraws,
         g_health.particles, g_health.groups, g_health.fallbackReason);
    mc2vk::getOrCreateEngineVulkanContext().retireIsland(mc2vk::IslandSlot::Vfx);
}

#endif  // MC2_VULKAN_ISLAND
