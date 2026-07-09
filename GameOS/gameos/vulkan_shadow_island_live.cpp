// vulkan_shadow_island_live.cpp — VK-P8a SHADOW-INTEROP: the SECOND Vulkan GEOMETRY
// region and the FIRST VK-PRODUCES-DEPTH region (water was the first geometry region,
// GL-produces-color / VK-consumes; shadow REVERSES the direction).
// =============================================================================
// VK renders the DYNAMIC static-prop shadow-caster depth into an exportable D32
// VkImageResource; GL binds that shared D32 in place of getDynamicShadowTexture()
// and samples it via the UNCHANGED hardware sampler2DShadow calcShadow() path
// (shadow.hglsl). This lights up the vk2gl back-channel (glVkToGlSem /
// glInteropWaitFromVk) for the first time on a geometry region: VK submit SIGNALS
// vk2gl, GL WAITS it, then samples.
//
// SHAPE (mirrors the P9 water island — the proven geometry-region template):
//   - Device: BORROWED from the shared engine VulkanContext
//     (ensureEngineVulkanContextForInterop + getOrCreateEngineVulkanContext). NO
//     second device, NO volkLoadDevice. Requires the ctx was born wantInterop.
//   - Feed (STEP 0 decision, .claude/VK-P8a-STEP0-CASTER-FEED-NOTE.md): the CPU
//     GpuStaticPropInstance vector (dynShadowSet) the GL path already builds each
//     frame is memcpy'd into a VK instance SSBO (no fenced GL buffer crosses); the
//     shared static-prop VBO/IBO are copied ONCE per map into VK-owned immutable
//     buffers (P8a fallback (B): VK owns a copy — simplest working feed; import of
//     the GL buffer is a P8b optimization). Per-type packet ranges
//     (baseVertex/firstIndex/indexCount) drive the instanced draws.
//   - Pipeline: depth-only, no color attachment, depthTestEnable+depthWriteEnable,
//     depthCompareOp=LESS (forward-Z, matching the GL dynamic shadow pass — NOT
//     reverse-Z), cull NONE, depthBias set to the engine's glPolygonOffset
//     (shadowBiasFactor_/shadowBiasUnits_) EXACTLY (the STOP-watch).
//   - Output: VK renders the caster depth into the shared D32, signals vk2gl; GL
//     waits + binds the shared D32 as shadowMap (compare-mode) — zero-copy on the
//     depth surface; forbidden DEPTH-readback counters MUST stay 0.
//
// Gate MC2_VK_SHADOW_INTEROP default-OFF -> GL shadow path byte-identical.
// The whole TU compiles to nothing unless MC2_VULKAN_ISLAND is defined.
// =============================================================================

#ifdef MC2_VULKAN_ISLAND

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>
#include <GL/glew.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include "vk_image_resource.h"
#include "gl_interop_view.h"
#include "vulkan_context.h"
#include "vulkan_shadow_island_live.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>

// ---------------------------------------------------------------------------
// Health snapshot (mirrors the water island's) — read by debug_state_dump.cpp.
// ---------------------------------------------------------------------------
namespace {

struct ShadowIslandHealth {
    int   vulkanAvailable = 0;
    int   buildEnabled    = 1;   // this TU only compiles when the island is built
    char  deviceName[256] = {0};
    unsigned long validationErrors = 0;
    unsigned long attempted   = 0;
    unsigned long usedVulkan  = 0;
    char  fallbackReason[256] = {0};
    unsigned long vkDraws     = 0;
    int   interopActive  = 0;
    int   vk2glSemActive = 0;
    int   directChain    = 0;
    unsigned long usedInterop = 0;
    unsigned long instRecords = 0;   // caster instances fed this frame
    unsigned long typeRuns    = 0;   // per-type instanced draws this frame
    unsigned long geomVerts   = 0;   // shared VBO verts copied once
    double stageInstUs = 0.0, drawUs = 0.0, totalUs = 0.0;
};
ShadowIslandHealth g_health;
// VK-SHADOW-DIRECT-CONSUME-1: true only between a successful mc2_vk_shadow_island_run()
// (fence-waited, layout-transitioned) and the next call to mc2_vk_shadow_island_run()
// (which resets it before doing any work) -- so a same-frame-later VK consumer (the
// terrain island) can tell "is this frame's depth ready right now" without racing a
// stale prior-frame flag into a later frame where the shadow island declined.
bool g_dynShadowReadyThisFrame = false;
bool g_csmFullMapReadyThisFrame = false;
bool g_csmArrayReadyThisFrame = false;

void setFallback(const char* reason) {
    std::strncpy(g_health.fallbackReason, reason ? reason : "",
                 sizeof(g_health.fallbackReason) - 1);
    g_health.fallbackReason[sizeof(g_health.fallbackReason) - 1] = '\0';
}

// Forbidden DEPTH-readback counters (zero-copy invariant). VK PRODUCES the shadow
// map; GL SAMPLES it in place. Nothing should ever pull the shadow-map bytes back
// to the CPU or copy them across a staging buffer. Any of these firing on the
// interop path means the zero-copy invariant was violated -> fail the frame.
struct InteropForbidden {
    unsigned long vkCmdCopyImageToBuffer = 0;  // shadow-map readback   (must be 0)
    unsigned long glGetTexImage          = 0;  // shadow-map CPU pull   (must be 0)
    unsigned long glReadPixels           = 0;  // shadow-map CPU pull   (must be 0)
};
InteropForbidden g_forbidden;

void vlog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::fprintf(stderr, "[VK_SHADOW_ISLAND] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    va_end(ap);
}

bool shadowInteropGateRequested() {
    static int state = -1;
    if (state == -1) {
        const char* e = std::getenv("MC2_VK_SHADOW_INTEROP");
        state = (e && e[0] == '1') ? 1 : 0;
    }
    return state == 1;
}

bool csmFullMapInteropGateRequested() {
    static int state = -1;
    if (state == -1) {
        const char* e = std::getenv("MC2_VK_CSM_FULLMAP_INTEROP");
        state = (e && e[0] == '1') ? 1 : 0;
    }
    return state == 1;
}

bool csmArrayInteropGateRequested() {
    static int state = -1;
    if (state == -1) {
        const char* e = std::getenv("MC2_VK_CSM_ARRAY_INTEROP");
        state = (e && e[0] == '1') ? 1 : 0;
    }
    return state == 1;
}

bool shadowDirectChainRequested() {
    static int state = -1;
    if (state == -1) {
        const char* e = std::getenv("MC2_VK_SHADOW_DIRECT_CHAIN");
        state = (e && e[0] == '1') ? 1 : 0;
    }
    return state == 1;
}

void noteShadowDirectChain() {
    static bool logged = false;
    g_health.directChain = shadowDirectChainRequested() ? 1 : 0;
    if (g_health.directChain && !logged) {
        vlog("MC2_VK_SHADOW_DIRECT_CHAIN=1 -- VK shadow products stay VK-owned; GL semaphore wait is skipped.");
        logged = true;
    }
}

std::string spv_dir() {
    const char* env = std::getenv("MC2_VULKAN_SPV_DIR");
    std::string d = (env && *env) ? env : "shaders/vulkan";
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
    return d;
}

VkShaderModule load_spv(VkDevice device, const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { vlog("cannot open spv '%s'.", path.c_str()); return VK_NULL_HANDLE; }
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len <= 0 || (len % 4) != 0) { vlog("spv '%s' bad size %ld.", path.c_str(), len); std::fclose(f); return VK_NULL_HANDLE; }
    std::vector<uint32_t> code(static_cast<size_t>(len) / 4);
    size_t got = std::fread(code.data(), 1, static_cast<size_t>(len), f);
    std::fclose(f);
    if (got != static_cast<size_t>(len)) { vlog("spv short read '%s'.", path.c_str()); return VK_NULL_HANDLE; }
    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = static_cast<size_t>(len);
    smci.pCode = code.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &smci, nullptr, &mod) != VK_SUCCESS) {
        vlog("vkCreateShaderModule('%s') failed.", path.c_str()); return VK_NULL_HANDLE;
    }
    return mod;
}

// GpuStaticPropInstance mirror (112B, LOCKSTEP with gos_static_prop_batcher.h AND
// shaders/vulkan/shadow_prop.vert). We only READ modelMatrix in the VS, but the
// full stride must match so the memcpy'd POD indexes correctly.
struct GpuStaticPropInstanceMirror {
    float    modelMatrix[16];  // off 0
    uint32_t typeID;           // off 64
    uint32_t firstColorOffset; // off 68
    uint32_t flags;            // off 72
    uint32_t lightDataIndex;   // off 76
    float    aRGBHighlight[4]; // off 80
    float    fogRGB[4];        // off 96
};                             // 112 bytes
static_assert(sizeof(GpuStaticPropInstanceMirror) == 112, "instance mirror must be 112B");

// std140 per-frame UBO (LOCKSTEP with shadow_prop.vert ShadowFrameUBO).
struct ShadowFrameUBO {
    float lightSpaceMatrix[16]; // getDynamicLightSpaceMatrix() — GL_FALSE
    int32_t instBase;
    int32_t _pad0, _pad1, _pad2;
};

const VkFormat kDepthFmt = VK_FORMAT_D32_SFLOAT;

// Per-type packet (subset of GpuStaticPropPacket the draw loop needs). Copied once
// per map from the batcher's accessor.
struct TypeRun {
    uint32_t typeID;
    uint32_t firstPacket;
    uint32_t packetCount;
};

// The persistent island state.
struct ShadowIsland {
    bool disabled = false;
    bool inited   = false;
    bool useInterop = false;

    int size = 0;   // square shadow map side (getDynamicShadowMapSize())

    // Borrowed from the shared engine context (NOT owned).
    VkInstance       instance  = VK_NULL_HANDLE;
    VkPhysicalDevice phys      = VK_NULL_HANDLE;
    VkDevice         device    = VK_NULL_HANDLE;
    uint32_t         gfxFamily = UINT32_MAX;
    VkQueue          queue     = VK_NULL_HANDLE;
    VmaAllocator     allocator = VK_NULL_HANDLE; // borrowed from ctx

    // Shared exportable D32 depth (the shadow map GL samples). VK-PRODUCED.
    VkImageResource depthResource{};   // exportable D32, depth attachment + sampled
    GlInteropView   depthGlView{};

    // Caster geometry (VK-owned copies of the shared static-prop VBO/IBO, P8a (B)).
    VkBuffer      vbo = VK_NULL_HANDLE; VmaAllocation vboAlloc = VK_NULL_HANDLE;
    VkBuffer      ibo = VK_NULL_HANDLE; VmaAllocation iboAlloc = VK_NULL_HANDLE;
    uint32_t      vertexStride = 0;
    bool          geomUploaded = false;

    // Per-type packets + type runs (copied once per map).
    std::vector<Mc2ShadowPacket> packets;  // firstIndex/indexCount/baseVertex per packet
    std::vector<TypeRun>         typeRuns;  // firstPacket/packetCount per typeID (indexed by typeID)

    // Instance SSBO (per-frame, memcpy the GpuStaticPropInstance vector).
    VkBuffer      instBuf = VK_NULL_HANDLE; VmaAllocation instAlloc = VK_NULL_HANDLE;
    VkDeviceSize  instCapBytes = 0;
    VkBuffer      ubo = VK_NULL_HANDLE; VmaAllocation uboAlloc = VK_NULL_HANDLE;

    VkRenderPass      rpass  = VK_NULL_HANDLE;
    VkFramebuffer     fb     = VK_NULL_HANDLE;
    VkImageView       depthAttView = VK_NULL_HANDLE;  // depth-attachment view of the shared image
    int               csmFullMapSize = 0;
    VkImageResource   csmFullMapResource{};
    GlInteropView     csmFullMapGlView{};
    VkFramebuffer     csmFullMapFb = VK_NULL_HANDLE;
    VkImageView       csmFullMapDepthAttView = VK_NULL_HANDLE;
    int               csmArraySize = 0;
    uint32_t          csmArrayLayers = 0;
    VkImage           csmArrayImg = VK_NULL_HANDLE;
    VmaAllocation     csmArrayAlloc = VK_NULL_HANDLE;
    VkImageView       csmArrayView = VK_NULL_HANDLE;
    std::vector<VkImageView> csmArrayLayerViews;
    std::vector<VkFramebuffer> csmArrayFbs;
    VkFence           csmArrayFence = VK_NULL_HANDLE;
    uint32_t          csmArrayRenderedMask = 0;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool  pool   = VK_NULL_HANDLE;
    VkDescriptorSet   set    = VK_NULL_HANDLE;
    VkPipelineLayout  playout = VK_NULL_HANDLE;
    VkPipeline        pipe   = VK_NULL_HANDLE;
    VkShaderModule    vert   = VK_NULL_HANDLE;
    VkShaderModule    frag   = VK_NULL_HANDLE;

    VkCommandPool     cpool  = VK_NULL_HANDLE;
    VkCommandBuffer   cbuf[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

    bool descNeedsRewrite = true;
};

ShadowIsland* g_island = nullptr;

bool make_buffer(ShadowIsland* s, VkDeviceSize size, VkBufferUsageFlags usage,
                 VkBuffer* buf, VmaAllocation* alloc, bool hostVisible) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size ? size : 16; bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = hostVisible ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : 0;
    if (vmaCreateBuffer(s->allocator, &bci, &aci, buf, alloc, nullptr) != VK_SUCCESS) {
        vlog("vmaCreateBuffer failed."); return false;
    }
    return true;
}

void write_descriptors(ShadowIsland* s) {
    if (s->set == VK_NULL_HANDLE) return;
    VkDescriptorBufferInfo uboInfo{ s->ubo, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo instInfo{ s->instBuf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[2]{};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = s->set;
    w[0].dstBinding = 0; w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &uboInfo;
    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = s->set;
    w[1].dstBinding = 1; w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &instInfo;
    vkUpdateDescriptorSets(s->device, 2, w, 0, nullptr);
}

bool ensure_inst_capacity(ShadowIsland* s, VkDeviceSize bytes) {
    if (s->instBuf != VK_NULL_HANDLE && bytes <= s->instCapBytes) return true;
    if (s->instBuf != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(s->device);
        vmaDestroyBuffer(s->allocator, s->instBuf, s->instAlloc);
        s->instBuf = VK_NULL_HANDLE;
    }
    VkDeviceSize cap = bytes + bytes / 2 + 4096;
    if (!make_buffer(s, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &s->instBuf, &s->instAlloc, true))
        return false;
    s->instCapBytes = cap;
    write_descriptors(s);
    return true;
}

void destroy_island(ShadowIsland* s) {
    if (!s) return;
    VkDevice d = s->device;
    if (d != VK_NULL_HANDLE) vkDeviceWaitIdle(d);
    glInteropViewDestroy(&s->depthGlView);
    if (d != VK_NULL_HANDLE) vkImageResourceDestroy(&s->depthResource, d);
    if (s->depthAttView) vkDestroyImageView(d, s->depthAttView, nullptr);
    if (s->csmFullMapFb) vkDestroyFramebuffer(d, s->csmFullMapFb, nullptr);
    glInteropViewDestroy(&s->csmFullMapGlView);
    if (s->csmFullMapDepthAttView) vkDestroyImageView(d, s->csmFullMapDepthAttView, nullptr);
    if (d != VK_NULL_HANDLE) vkImageResourceDestroy(&s->csmFullMapResource, d);
    for (VkFramebuffer fbLayer : s->csmArrayFbs) {
        if (fbLayer) vkDestroyFramebuffer(d, fbLayer, nullptr);
    }
    for (VkImageView viewLayer : s->csmArrayLayerViews) {
        if (viewLayer) vkDestroyImageView(d, viewLayer, nullptr);
    }
    if (s->csmArrayView) vkDestroyImageView(d, s->csmArrayView, nullptr);
    if (s->csmArrayImg) vmaDestroyImage(s->allocator, s->csmArrayImg, s->csmArrayAlloc);
    if (s->csmArrayFence) vkDestroyFence(d, s->csmArrayFence, nullptr);
    if (s->pipe)    vkDestroyPipeline(d, s->pipe, nullptr);
    if (s->playout) vkDestroyPipelineLayout(d, s->playout, nullptr);
    if (s->vert)    vkDestroyShaderModule(d, s->vert, nullptr);
    if (s->frag)    vkDestroyShaderModule(d, s->frag, nullptr);
    if (s->dsl)     vkDestroyDescriptorSetLayout(d, s->dsl, nullptr);
    if (s->pool)    vkDestroyDescriptorPool(d, s->pool, nullptr);
    if (s->fb)      vkDestroyFramebuffer(d, s->fb, nullptr);
    if (s->rpass)   vkDestroyRenderPass(d, s->rpass, nullptr);
    if (s->vbo)     vmaDestroyBuffer(s->allocator, s->vbo, s->vboAlloc);
    if (s->ibo)     vmaDestroyBuffer(s->allocator, s->ibo, s->iboAlloc);
    if (s->instBuf) vmaDestroyBuffer(s->allocator, s->instBuf, s->instAlloc);
    if (s->ubo)     vmaDestroyBuffer(s->allocator, s->ubo, s->uboAlloc);
    if (s->cpool)   vkDestroyCommandPool(d, s->cpool, nullptr);
    // instance/phys/device/queue/allocator are BORROWED — never destroyed here.
}

// One-time init: borrow the shared device, build the exportable D32 shadow image +
// its GL interop view (GL_DEPTH_COMPONENT32F, compare-mode), depth-attachment view,
// depth-only render pass + framebuffer, instance/UBO buffers, pipeline, cmd pool.
bool init_island(ShadowIsland* s, int size) {
    s->size = size;
    ++g_health.attempted;

    if (!mc2vk::ensureEngineVulkanContextForInterop()) {
        setFallback("engine_vk_context_birth_failed"); return false;
    }
    mc2vk::VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    if (!ctx.valid()) { setFallback("engine_vk_context_not_valid"); return false; }
    s->instance  = ctx.instance();
    s->phys      = ctx.physicalDevice();
    s->device    = ctx.device();
    s->queue     = ctx.graphicsQueue();
    s->gfxFamily = ctx.graphicsFamily();
    s->allocator = ctx.allocator();
    if (s->device == VK_NULL_HANDLE || s->allocator == VK_NULL_HANDLE) {
        setFallback("engine_vk_device_or_allocator_null"); return false;
    }
    g_health.vulkanAvailable = 1;
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(s->phys, &props);
        std::strncpy(g_health.deviceName, props.deviceName, sizeof(g_health.deviceName) - 1);
        g_health.deviceName[sizeof(g_health.deviceName) - 1] = '\0';
    }

    if (!shadowInteropGateRequested()) { setFallback("gate_off"); return false; }

    // ---- Exportable shared D32 depth image. SAMPLED (GL samples it via
    // sampler2DShadow) + DEPTH_STENCIL_ATTACHMENT (VK renders into it). ----
    bool ok = vkImageResourceCreate(&s->depthResource, s->device, s->phys,
                                    (uint32_t)size, (uint32_t)size, kDepthFmt,
                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT,
                                    /*exportable*/ true);
    if (ok) ok = glInteropViewCreate(&s->depthGlView, &s->depthResource, s->phys,
                                     GL_DEPTH_COMPONENT32F);
    if (!ok) {
        glInteropViewDestroy(&s->depthGlView);
        vkImageResourceDestroy(&s->depthResource, s->device);
        setFallback("interop_setup_failed");
        return false; // P8a is interop-only; no readback fallback path
    }
    // Configure the imported GL depth texture for HARDWARE compare sampling — the
    // EXACT MC2 calcShadow convention (probe-proven over imported D32). GL consumers
    // then bind this texture as `dynamicShadowMap` and sample with texture(sampler2DShadow,...).
    glBindTexture(GL_TEXTURE_2D, s->depthGlView.glTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);   // hardware 2x2 PCF (matches GL shadow tex)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glBindTexture(GL_TEXTURE_2D, 0);

    s->useInterop = true;
    g_health.interopActive = 1;
    g_health.vk2glSemActive = s->depthGlView.vkToGlImported ? 1 : 0;
    vlog("depth interop seam ACTIVE — zero-copy exportable D32 shadow map (vk2gl_sem=%d).",
         g_health.vk2glSemActive);

    // ---- Depth-attachment view of the shared image (the interop view's own
    // .view is the SAMPLED view; the render pass needs a DEPTH-aspect attachment
    // view — same image, depth aspect). ----
    {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = s->depthResource.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = kDepthFmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(s->device, &vi, nullptr, &s->depthAttView) != VK_SUCCESS) {
            vlog("depth attachment view failed."); return false;
        }
    }

    // ---- Instance SSBO + UBO. ----
    if (!ensure_inst_capacity(s, 256 * 1024)) return false;  // ~2340 instances to start
    if (!make_buffer(s, sizeof(ShadowFrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &s->ubo, &s->uboAlloc, true)) return false;

    // ---- Depth-only render pass (single depth attachment; CLEAR to 1.0 forward-Z,
    // STORE). Interop layout: GL never writes this image, VK is sole producer; we
    // acquire in UNDEFINED (contents don't survive — we CLEAR every frame) and leave
    // in DEPTH_STENCIL_READ_ONLY so GL samples it. External deps serialize the
    // cross-frame WAW (the vk2gl/GL semaphore round-trip functionally serializes it,
    // but sync-val can't see across the GL boundary — the water WAW-fix pattern). ----
    {
        VkAttachmentDescription att{};
        att.format = kDepthFmt; att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // Cleared every frame -> prior contents irrelevant -> UNDEFINED initial (also
        // avoids a layout-transition write across the GL sampling that just happened).
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL; // GL samples next
        VkAttachmentReference depthRef{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 0;
        sub.pDepthStencilAttachment = &depthRef;

        // Cross-frame WAW: the shared depth image is written (CLEAR + caster draws +
        // STORE) every frame across two parity command buffers. deps[0] orders THIS
        // frame's depth writes AFTER any prior depth write; deps[1] retires the store
        // before the vk2gl signal hands the image to GL.
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        deps[1].dstAccessMask = 0;

        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 1; rpci.pAttachments = &att;
        rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        if (vkCreateRenderPass(s->device, &rpci, nullptr, &s->rpass) != VK_SUCCESS) {
            vlog("vkCreateRenderPass failed."); return false;
        }
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->rpass; fci.attachmentCount = 1; fci.pAttachments = &s->depthAttView;
        fci.width = (uint32_t)size; fci.height = (uint32_t)size; fci.layers = 1;
        if (vkCreateFramebuffer(s->device, &fci, nullptr, &s->fb) != VK_SUCCESS) {
            vlog("vkCreateFramebuffer failed."); return false;
        }
    }

    // ---- Descriptor layout/pool/set (UBO binding0 VS, instance SSBO binding1 VS). ----
    {
        VkDescriptorSetLayoutBinding b[2]{};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 2; lci.pBindings = b;
        if (vkCreateDescriptorSetLayout(s->device, &lci, nullptr, &s->dsl) != VK_SUCCESS) {
            vlog("vkCreateDescriptorSetLayout failed."); return false;
        }
        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sizes[0].descriptorCount = 1;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sizes[1].descriptorCount = 1;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 1; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(s->device, &pci, nullptr, &s->pool) != VK_SUCCESS) {
            vlog("vkCreateDescriptorPool failed."); return false;
        }
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = s->pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &s->dsl;
        if (vkAllocateDescriptorSets(s->device, &dsai, &s->set) != VK_SUCCESS) {
            vlog("vkAllocateDescriptorSets failed."); return false;
        }
        write_descriptors(s);
    }

    // ---- Pipeline: depth-only, one vec3 position stream, instanced, LESS forward-Z,
    // depthBias set dynamically each frame (matches glPolygonOffset). ----
    {
        s->vert = load_spv(s->device, spv_dir() + "shadow_prop.vert.spv");
        s->frag = load_spv(s->device, spv_dir() + "shadow_prop.frag.spv");
        if (s->vert == VK_NULL_HANDLE || s->frag == VK_NULL_HANDLE) { setFallback("spv_load_failed"); return false; }

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; plci.pSetLayouts = &s->dsl;
        if (vkCreatePipelineLayout(s->device, &plci, nullptr, &s->playout) != VK_SUCCESS) {
            vlog("vkCreatePipelineLayout failed."); return false;
        }

        VkVertexInputBindingDescription vib{};
        vib.binding = 0; vib.stride = s->vertexStride ? s->vertexStride : sizeof(float) * 3;
        vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription via{};
        via.location = 0; via.binding = 0; via.format = VK_FORMAT_R32G32B32_SFLOAT; via.offset = 0;
        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vib;
        vin.vertexAttributeDescriptionCount = 1; vin.pVertexAttributeDescriptions = &via;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1; vps.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;       // GL shadow pass cull none (ShadowMech/ShadowStaticProp)
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.depthBiasEnable = VK_TRUE;          // matches GL glEnable(GL_POLYGON_OFFSET_FILL)
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Forward-Z: LESS + write on (NOT reverse-Z; the GL dynamic shadow map is
        // forward-Z GL_LESS, glClearDepth(1.0)).
        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 0;   // depth-only

        // Dynamic viewport/scissor + DEPTH BIAS (set per-frame to the engine's
        // glPolygonOffset factor/units — the STOP-watch parity requirement).
        VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                  VK_DYNAMIC_STATE_DEPTH_BIAS };
        VkPipelineDynamicStateCreateInfo dsci{};
        dsci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dsci.dynamicStateCount = 3; dsci.pDynamicStates = dyn;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = s->vert; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = s->frag; stages[1].pName = "main";
        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2; gp.pStages = stages;
        gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds; gp.pColorBlendState = &cb;
        gp.pDynamicState = &dsci;
        gp.layout = s->playout; gp.renderPass = s->rpass; gp.subpass = 0;
        if (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, &s->pipe) != VK_SUCCESS) {
            vlog("vkCreateGraphicsPipelines failed."); return false;
        }
    }

    // ---- Command pool + 2 command buffers (per parity). ----
    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s->gfxFamily;
        if (vkCreateCommandPool(s->device, &pci, nullptr, &s->cpool) != VK_SUCCESS) {
            vlog("vkCreateCommandPool failed."); return false;
        }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = s->cpool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 2;
        if (vkAllocateCommandBuffers(s->device, &cai, s->cbuf) != VK_SUCCESS) {
            vlog("vkAllocateCommandBuffers failed."); return false;
        }
    }

    s->inited = true;
    setFallback("");
    vlog("island initialized %dx%d (borrowed shared device; exportable D32 shadow map, "
         "depth-only LESS forward-Z, depthBias dynamic; instanced static-prop casters).", size, size);
    return true;
}

bool ensure_csm_fullmap_target(ShadowIsland* s, int size) {
    if (!s || size <= 0 || s->rpass == VK_NULL_HANDLE) return false;
    if (s->csmFullMapResource.image != VK_NULL_HANDLE && s->csmFullMapSize == size)
        return true;

    vkDeviceWaitIdle(s->device);
    if (s->csmFullMapFb) vkDestroyFramebuffer(s->device, s->csmFullMapFb, nullptr);
    glInteropViewDestroy(&s->csmFullMapGlView);
    if (s->csmFullMapDepthAttView) vkDestroyImageView(s->device, s->csmFullMapDepthAttView, nullptr);
    if (s->csmFullMapResource.image != VK_NULL_HANDLE)
        vkImageResourceDestroy(&s->csmFullMapResource, s->device);
    s->csmFullMapResource = {};
    s->csmFullMapDepthAttView = VK_NULL_HANDLE;
    s->csmFullMapFb = VK_NULL_HANDLE;
    s->csmFullMapSize = 0;

    bool ok = vkImageResourceCreate(&s->csmFullMapResource, s->device, s->phys,
                                    (uint32_t)size, (uint32_t)size, kDepthFmt,
                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT,
                                    /*exportable*/ true);
    if (ok) ok = glInteropViewCreate(&s->csmFullMapGlView, &s->csmFullMapResource, s->phys,
                                     GL_DEPTH_COMPONENT32F);
    if (!ok) {
        glInteropViewDestroy(&s->csmFullMapGlView);
        vkImageResourceDestroy(&s->csmFullMapResource, s->device);
        setFallback("csm_fullmap_interop_setup_failed");
        return false;
    }
    glBindTexture(GL_TEXTURE_2D, s->csmFullMapGlView.glTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glBindTexture(GL_TEXTURE_2D, 0);

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = s->csmFullMapResource.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = kDepthFmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(s->device, &vi, nullptr, &s->csmFullMapDepthAttView) != VK_SUCCESS) {
        setFallback("csm_fullmap_att_view_failed");
        return false;
    }

    VkFramebufferCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fci.renderPass = s->rpass;
    fci.attachmentCount = 1;
    fci.pAttachments = &s->csmFullMapDepthAttView;
    fci.width = (uint32_t)size;
    fci.height = (uint32_t)size;
    fci.layers = 1;
    if (vkCreateFramebuffer(s->device, &fci, nullptr, &s->csmFullMapFb) != VK_SUCCESS) {
        setFallback("csm_fullmap_framebuffer_failed");
        return false;
    }
    s->csmFullMapSize = size;
    vlog("CSM full-map target initialized %dx%d (exportable D32, direct terrain binding 9).",
         size, size);
    return true;
}

bool ensure_csm_array_target(ShadowIsland* s, int size, uint32_t layers) {
    if (!s || size <= 0 || layers == 0 || layers > 31 || s->rpass == VK_NULL_HANDLE) return false;
    if (s->csmArrayImg != VK_NULL_HANDLE && s->csmArraySize == size && s->csmArrayLayers == layers)
        return true;

    vkDeviceWaitIdle(s->device);
    for (VkFramebuffer fbLayer : s->csmArrayFbs) {
        if (fbLayer) vkDestroyFramebuffer(s->device, fbLayer, nullptr);
    }
    for (VkImageView viewLayer : s->csmArrayLayerViews) {
        if (viewLayer) vkDestroyImageView(s->device, viewLayer, nullptr);
    }
    s->csmArrayFbs.clear();
    s->csmArrayLayerViews.clear();
    if (s->csmArrayView) { vkDestroyImageView(s->device, s->csmArrayView, nullptr); s->csmArrayView = VK_NULL_HANDLE; }
    if (s->csmArrayImg) { vmaDestroyImage(s->allocator, s->csmArrayImg, s->csmArrayAlloc); s->csmArrayImg = VK_NULL_HANDLE; }
    if (s->csmArrayFence) { vkDestroyFence(s->device, s->csmArrayFence, nullptr); s->csmArrayFence = VK_NULL_HANDLE; }
    s->csmArraySize = 0;
    s->csmArrayLayers = 0;
    s->csmArrayRenderedMask = 0;

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = kDepthFmt;
    ici.extent = {(uint32_t)size, (uint32_t)size, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(s->allocator, &ici, &aci, &s->csmArrayImg, &s->csmArrayAlloc, nullptr) != VK_SUCCESS) {
        setFallback("csm_array_image_failed");
        return false;
    }

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = s->csmArrayImg;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vci.format = kDepthFmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers};
    if (vkCreateImageView(s->device, &vci, nullptr, &s->csmArrayView) != VK_SUCCESS) {
        setFallback("csm_array_view_failed");
        return false;
    }

    s->csmArrayLayerViews.resize(layers, VK_NULL_HANDLE);
    s->csmArrayFbs.resize(layers, VK_NULL_HANDLE);
    for (uint32_t layer = 0; layer < layers; ++layer) {
        VkImageViewCreateInfo lvi{};
        lvi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        lvi.image = s->csmArrayImg;
        lvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        lvi.format = kDepthFmt;
        lvi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, layer, 1};
        if (vkCreateImageView(s->device, &lvi, nullptr, &s->csmArrayLayerViews[layer]) != VK_SUCCESS) {
            setFallback("csm_array_layer_view_failed");
            return false;
        }
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->rpass;
        fci.attachmentCount = 1;
        fci.pAttachments = &s->csmArrayLayerViews[layer];
        fci.width = (uint32_t)size;
        fci.height = (uint32_t)size;
        fci.layers = 1;
        if (vkCreateFramebuffer(s->device, &fci, nullptr, &s->csmArrayFbs[layer]) != VK_SUCCESS) {
            setFallback("csm_array_framebuffer_failed");
            return false;
        }
    }
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(s->device, &fci, nullptr, &s->csmArrayFence) != VK_SUCCESS) {
        setFallback("csm_array_fence_failed");
        return false;
    }
    s->csmArraySize = size;
    s->csmArrayLayers = layers;
    vlog("CSM array target initialized %dx%d layers=%u (VK D32 array, direct terrain binding 8).",
         size, size, layers);
    return true;
}

// Copy the shared static-prop VBO/IBO into VK-owned buffers ONCE per map (P8a
// feed (B): VK owns a copy — simplest working feed). Also caches the per-type
// packet ranges. Returns false if the geometry feed is unavailable this frame.
bool upload_geometry_once(ShadowIsland* s) {
    if (s->geomUploaded) return true;
    Mc2ShadowGeomFeed feed{};
    if (!mc2_shadow_island_get_geometry_feed(&feed)) return false;
    if (feed.vboBytes == 0 || feed.iboBytes == 0 || feed.glVbo == 0 || feed.glIbo == 0) return false;

    s->vertexStride = feed.vertexStride;

    if (!make_buffer(s, feed.vboBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &s->vbo, &s->vboAlloc, true)) return false;
    if (!make_buffer(s, feed.iboBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,  &s->ibo, &s->iboAlloc, true)) return false;
    // The CPU staging was freed at finalize; pull the immutable GL buffer bytes ONCE
    // (map-load, not per-frame) via glGetBufferSubData into a scratch, then memcpy
    // into the VK buffers. This one-time geometry copy is NOT the shadow-map surface,
    // so it does not touch the per-frame zero-copy shadow-map invariant.
    std::vector<uint8_t> scratch;
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, s->vboAlloc, &p) == VK_SUCCESS) {
          scratch.resize((size_t)feed.vboBytes);
          glBindBuffer(GL_ARRAY_BUFFER, feed.glVbo);
          glGetBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)feed.vboBytes, scratch.data());
          glBindBuffer(GL_ARRAY_BUFFER, 0);
          std::memcpy(p, scratch.data(), (size_t)feed.vboBytes);
          vmaUnmapMemory(s->allocator, s->vboAlloc);
      } }
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, s->iboAlloc, &p) == VK_SUCCESS) {
          scratch.resize((size_t)feed.iboBytes);
          glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, feed.glIbo);
          glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, (GLsizeiptr)feed.iboBytes, scratch.data());
          glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
          std::memcpy(p, scratch.data(), (size_t)feed.iboBytes);
          vmaUnmapMemory(s->allocator, s->iboAlloc);
      } }

    s->packets.assign(feed.packets, feed.packets + feed.packetCount);
    s->typeRuns.clear();
    s->typeRuns.resize(feed.typeCount);
    for (uint32_t t = 0; t < feed.typeCount; ++t) {
        s->typeRuns[t].typeID      = t;
        s->typeRuns[t].firstPacket = feed.typeFirstPacket[t];
        s->typeRuns[t].packetCount = feed.typePacketCount[t];
    }
    g_health.geomVerts = (unsigned long)(feed.vboBytes / (feed.vertexStride ? feed.vertexStride : 1));
    s->geomUploaded = true;
    vlog("caster geometry uploaded once: vbo=%llu B ibo=%llu B stride=%u packets=%u types=%u.",
         (unsigned long long)feed.vboBytes, (unsigned long long)feed.iboBytes,
         feed.vertexStride, feed.packetCount, feed.typeCount);
    return true;
}

} // namespace

// ===========================================================================
// PUBLIC ENTRY POINTS (consumed under #ifdef MC2_VULKAN_ISLAND from
// gos_postprocess.cpp / txmmgr.cpp / debug_state_dump.cpp).
// ===========================================================================

// True when the gate requests the VK shadow region (the caller then SKIPS the GL
// dynamic shadow WRITE draws and calls mc2_vk_shadow_island_run instead).
extern "C" int mc2_vk_shadow_island_enabled() {
    return shadowInteropGateRequested() ? 1 : 0;
}

// Per-frame entry. Inputs (all from the GL side, at dynamic-shadow-pass time):
//   size            — square shadow map side (getDynamicShadowMapSize()).
//   lightSpace16    — the 16 raw floats GL uploads (GL_FALSE) for lightSpaceMatrix.
//   instances       — the CPU GpuStaticPropInstance vector (112B each) = dynShadowSet.
//   instanceCount   — number of caster instances (already culled + ready).
//   biasFactor/biasUnits — the engine's glPolygonOffset(shadowBiasFactor_,
//                     shadowBiasUnits_) — reproduced as VK depthBias EXACTLY.
// Returns the GL texture handle of the VK-produced shared shadow map (>0) if the VK
// pass rendered+signalled this frame; 0 = fell back (caller then runs the GL path).
extern "C" unsigned int mc2_vk_shadow_island_run(
    int size, const float* lightSpace16,
    const void* instances, unsigned int instanceCount,
    float biasFactor, float biasUnits) {
    g_dynShadowReadyThisFrame = false;   // VK-SHADOW-DIRECT-CONSUME-1: reset; set true only at the tail on success
    g_csmFullMapReadyThisFrame = false;  // CSM full-map direct view must be same-frame fresh too
    g_csmArrayReadyThisFrame = false;    // CSM array direct view must be same-frame fresh too
    if (!shadowInteropGateRequested()) return 0;
    noteShadowDirectChain();
    if (size <= 0 || !lightSpace16 || !instances || instanceCount == 0) return 0;

    using clk = std::chrono::steady_clock;
    auto usSince = [](clk::time_point t0){ return std::chrono::duration<double, std::micro>(clk::now() - t0).count(); };
    const clk::time_point tTotal0 = clk::now();

    // Lazy init / rebuild on size change.
    if (g_island && g_island->size != size) {
        destroy_island(g_island); delete g_island; g_island = nullptr;
    }
    if (!g_island) {
        g_island = new ShadowIsland();
        if (!init_island(g_island, size)) {
            vlog("init failed (%s) — GL fallback.", g_health.fallbackReason);
            destroy_island(g_island); delete g_island; g_island = nullptr;
            return 0;
        }
    }
    ShadowIsland* s = g_island;
    if (s->disabled || !s->inited || !s->useInterop) return 0;

    // Caster geometry must be resident (copied once per map). If the batcher's
    // geometry isn't finalized yet, decline this frame (GL path runs).
    static bool s_firstOk = false;
    if (!upload_geometry_once(s)) { setFallback("geometry_not_ready"); return 0; }

    // Snapshot forbidden DEPTH-readback counters (prove none fire on the interop path).
    const unsigned long f0a = g_forbidden.vkCmdCopyImageToBuffer;
    const unsigned long f0b = g_forbidden.glGetTexImage;
    const unsigned long f0c = g_forbidden.glReadPixels;

    const clk::time_point tStage0 = clk::now();
    // ---- 1) Upload the per-frame caster instances (CPU-staged; no GL buffer crosses). ----
    const VkDeviceSize instBytes = (VkDeviceSize)instanceCount * sizeof(GpuStaticPropInstanceMirror);
    if (!ensure_inst_capacity(s, instBytes)) { s->disabled = true; return 0; }
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->instAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, instances, (size_t)instBytes);
            vmaUnmapMemory(s->allocator, s->instAlloc);
        }
    }
    g_health.instRecords = instanceCount;

    // ---- 2) Upload the per-frame UBO (lightSpaceMatrix; instBase set per-type below).
    // The instance SSBO is bound whole; the VS reads instBase+gl_InstanceIndex, so a
    // single UBO with instBase can't cover multiple type-runs. Instead the type-run's
    // base is folded into the vertex draw's firstInstance (VK) — see the draw loop:
    // vkCmdDraw firstInstance offsets gl_InstanceIndex, so instBase stays 0 and each
    // run passes firstInstance=runStart. Upload lightSpace + instBase=0 once. ----
    {
        ShadowFrameUBO u{};
        std::memcpy(u.lightSpaceMatrix, lightSpace16, sizeof(u.lightSpaceMatrix));
        u.instBase = 0;
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->uboAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, &u, sizeof(u)); vmaUnmapMemory(s->allocator, s->uboAlloc);
        }
    }
    g_health.stageInstUs = usSince(tStage0);

    // ---- 3) Begin frame parity. ----
    // NOTE: this is a VK-PRODUCES / GL-CONSUMES depth resource. GL NEVER writes the
    // shared depth image (only samples it later via calcShadow). So there is NO GL->VK
    // acquire here — unlike the water/color islands (GL-PRODUCES) which must signal
    // glInteropSignalToVk to hand GL-written pixels to VK. Issuing that signal here is
    // not just unnecessary, it is HARMFUL: the mid-frame glSignalSemaphoreEXT + glFlush
    // on the shared depth poisons the subsequent GL scene render into sceneFBO_ on this
    // AMD interop timeline (whole scene goes black). The only ordering the shared depth
    // needs is: VK produces -> VK signals vk2gl -> GL waits vk2gl -> GL samples. The
    // GL->VK direction (glToVkSem / fs.signal) is therefore left unused: the VK submit
    // below does NOT wait it, and the per-slot WAW hazard is covered by the frame fence
    // (vkImageResourceBeginFrame waits frameFence[parity] before reusing the slot).
    VkFrameSync fs = vkImageResourceBeginFrame(&s->depthResource);
    VkCommandBuffer cb = s->cbuf[fs.parity & 1u];

    // ---- 4) Sort caster instances by typeID into contiguous runs on the CPU side.
    // The GL path sorts dynShadowSet by typeID and draws each type-run instanced.
    // We reproduce that: build (typeID, firstInstance, instCount) runs. The instance
    // buffer we just uploaded is in the CALLER's order; to match GL we must sort. But
    // the GL path sorts a COPY too (drawDynamicPropShadows builds `sorted`), so the
    // depth result is order-independent (depth-LESS, deterministic). We sort indices
    // here to group by type for the correct per-type geometry. ----
    const GpuStaticPropInstanceMirror* insts =
        reinterpret_cast<const GpuStaticPropInstanceMirror*>(instances);
    std::vector<uint32_t> order(instanceCount);
    for (uint32_t k = 0; k < instanceCount; ++k) order[k] = k;
    std::sort(order.begin(), order.end(),
        [insts](uint32_t a, uint32_t b){ return insts[a].typeID < insts[b].typeID; });
    // Re-stage the instance buffer in sorted order so gl_InstanceIndex within a run
    // maps to the right transforms (firstInstance addresses the run start).
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->instAlloc, &p) == VK_SUCCESS) {
            GpuStaticPropInstanceMirror* dst = reinterpret_cast<GpuStaticPropInstanceMirror*>(p);
            for (uint32_t k = 0; k < instanceCount; ++k) dst[k] = insts[order[k]];
            vmaUnmapMemory(s->allocator, s->instAlloc);
        }
    }

    // ---- 5) Record: depth-only render pass, per-type instanced caster draws. ----
    const clk::time_point tRender0 = clk::now();
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};   // forward-Z clear to 1.0 (GL glClearDepth(1.0))
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = s->rpass; rbi.framebuffer = s->fb;
    rbi.renderArea = {{0, 0}, {(uint32_t)size, (uint32_t)size}};
    rbi.clearValueCount = 1; rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{0.0f, 0.0f, (float)size, (float)size, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {(uint32_t)size, (uint32_t)size}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    // DEPTH BIAS PARITY (the STOP-watch): GL uses glPolygonOffset(factor, units).
    // Vulkan vkCmdSetDepthBias(constantFactor, clamp, slopeFactor) — GL's `units`
    // maps to depthBiasConstantFactor and GL's `factor` maps to depthBiasSlopeFactor.
    vkCmdSetDepthBias(cb, biasUnits, 0.0f, biasFactor);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout, 0, 1, &s->set, 0, nullptr);
    VkDeviceSize vboOff = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &s->vbo, &vboOff);
    vkCmdBindIndexBuffer(cb, s->ibo, 0, VK_INDEX_TYPE_UINT32);

    // Per-type run: walk the sorted instance order, group by typeID, and for each run
    // issue the type's packet draws with firstInstance=runStart (so gl_InstanceIndex
    // addresses the run's transforms) — mirrors drawDynamicPropShadows exactly.
    unsigned long typeRuns = 0, drawCalls = 0;
    uint32_t i = 0;
    while (i < instanceCount) {
        const uint32_t tid = insts[order[i]].typeID;
        uint32_t j = i;
        while (j < instanceCount && insts[order[j]].typeID == tid) ++j;
        const uint32_t instCount = j - i;
        if (tid < s->typeRuns.size()) {
            const TypeRun& tr = s->typeRuns[tid];
            for (uint32_t p = 0; p < tr.packetCount; ++p) {
                const uint32_t pk = tr.firstPacket + p;
                if (pk >= s->packets.size()) break;
                const Mc2ShadowPacket& pkt = s->packets[pk];
                vkCmdDrawIndexed(cb, pkt.indexCount, instCount,
                                 pkt.firstIndex, (int32_t)pkt.baseVertex,
                                 /*firstInstance*/ i);
                ++drawCalls;
            }
            ++typeRuns;
        }
        i = j;
    }
    g_health.typeRuns = typeRuns;

    vkCmdEndRenderPass(cb);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

    // ---- 6) Submit: NO GL->VK wait (VK produces the depth; GL never wrote it).
    // Legacy interop signals VK->GL and makes GL wait before sampling; direct-chain
    // proof mode skips that handoff and exposes the same-frame VkImageView only.
    const bool directChain = shadowDirectChainRequested();
    const bool useGpuVk2Gl = !directChain && s->depthGlView.vkToGlImported && (fs.vk2gl != VK_NULL_HANDLE);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 0; si.pWaitSemaphores = nullptr; si.pWaitDstStageMask = nullptr;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    if (useGpuVk2Gl) { si.signalSemaphoreCount = 1; si.pSignalSemaphores = &fs.vk2gl; }
    if (vkQueueSubmit(s->queue, 1, &si, fs.fence) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; }
    if (useGpuVk2Gl) {
        // GL waits the VK->GL semaphore before it samples the shared depth as shadowMap.
        glInteropWaitFromVk(s->depthGlView, GL_LAYOUT_DEPTH_STENCIL_READ_ONLY_EXT, fs.parity);
    }
    // WAW FIX (sync-val): fully retire THIS frame's VK submit before the next frame
    // records/submits, so the two parity command buffers never have overlapping
    // in-flight writes to the shared depth image on the queue (water's pattern).
    if (vkWaitForFences(s->device, 1, &fs.fence, VK_TRUE, 5ull*1000*1000*1000) != VK_SUCCESS) {
        setFallback("fence_timeout"); s->disabled = true; return 0;
    }
    g_health.drawUs = usSince(tRender0);

    // ---- HARD assert: no DEPTH-side readback/copy fired on the interop path. ----
    const bool readbackFired =
        (g_forbidden.vkCmdCopyImageToBuffer != f0a) ||
        (g_forbidden.glGetTexImage != f0b) || (g_forbidden.glReadPixels != f0c);
    if (readbackFired) {
        vlog("FATAL: a DEPTH-side readback/copy fired on the interop path — zero-copy "
             "invariant violated; failing frame + disabling.");
        setFallback("interop_did_readback"); s->disabled = true; return 0;
    }

    g_health.vkDraws = 1;
    ++g_health.usedVulkan;
    if (!directChain) ++g_health.usedInterop;
    g_health.totalUs = usSince(tTotal0);
    if (!s_firstOk) {
        if (directChain) {
            vlog("first VK shadow map produced: instCount=%u typeRuns=%lu view=%p (direct VK consumers only).",
                 instanceCount, g_health.typeRuns, (void*)s->depthResource.view);
        } else {
            vlog("first VK shadow map produced: instCount=%u typeRuns=%lu tex=%u (GL now samples the "
                 "VK-produced D32 via calcShadow).", instanceCount, g_health.typeRuns, s->depthGlView.glTex);
        }
        s_firstOk = true;
    }
    // VK-SHADOW-DIRECT-CONSUME-1: fence-waited above, layout already transitioned to
    // DEPTH_STENCIL_READ_ONLY_OPTIMAL by the render pass -- safe for a same-frame-later
    // VK consumer (the terrain island) to sample s->depthResource.view directly.
    g_dynShadowReadyThisFrame = true;
    // Return the GL texture handle only for the legacy VK->GL consumer path. In
    // direct-chain mode, native VK consumers use the ready view accessor instead.
    return directChain ? 0u : s->depthGlView.glTex;
}

extern "C" unsigned int mc2_vk_shadow_island_run_csm_fullmap(
    int size, const float* lightSpace16,
    const void* instances, unsigned int instanceCount,
    float biasFactor, float biasUnits) {
    g_csmFullMapReadyThisFrame = false;
    if (!shadowInteropGateRequested() || !csmFullMapInteropGateRequested()) return 0;
    noteShadowDirectChain();
    if (size <= 0 || !lightSpace16 || !instances || instanceCount == 0) return 0;
    if (!g_island || g_island->disabled || !g_island->inited || !g_island->useInterop) return 0;
    ShadowIsland* s = g_island;
    if (!upload_geometry_once(s)) { setFallback("geometry_not_ready"); return 0; }
    if (!ensure_csm_fullmap_target(s, size)) return 0;

    const VkDeviceSize instBytes = (VkDeviceSize)instanceCount * sizeof(GpuStaticPropInstanceMirror);
    if (!ensure_inst_capacity(s, instBytes)) { s->disabled = true; return 0; }
    const GpuStaticPropInstanceMirror* insts =
        reinterpret_cast<const GpuStaticPropInstanceMirror*>(instances);
    std::vector<uint32_t> order(instanceCount);
    for (uint32_t k = 0; k < instanceCount; ++k) order[k] = k;
    std::sort(order.begin(), order.end(),
        [insts](uint32_t a, uint32_t b){ return insts[a].typeID < insts[b].typeID; });
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->instAlloc, &p) == VK_SUCCESS) {
            GpuStaticPropInstanceMirror* dst = reinterpret_cast<GpuStaticPropInstanceMirror*>(p);
            for (uint32_t k = 0; k < instanceCount; ++k) dst[k] = insts[order[k]];
            vmaUnmapMemory(s->allocator, s->instAlloc);
        }
    }
    {
        ShadowFrameUBO u{};
        std::memcpy(u.lightSpaceMatrix, lightSpace16, sizeof(u.lightSpaceMatrix));
        u.instBase = 0;
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->uboAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, &u, sizeof(u));
            vmaUnmapMemory(s->allocator, s->uboAlloc);
        }
    }

    VkFrameSync fs = vkImageResourceBeginFrame(&s->csmFullMapResource);
    VkCommandBuffer cb = s->cbuf[fs.parity & 1u];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("csm_fullmap_record_failed"); s->disabled = true; return 0; }

    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = s->rpass;
    rbi.framebuffer = s->csmFullMapFb;
    rbi.renderArea = {{0, 0}, {(uint32_t)size, (uint32_t)size}};
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{0.0f, 0.0f, (float)size, (float)size, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {(uint32_t)size, (uint32_t)size}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    vkCmdSetDepthBias(cb, biasUnits, 0.0f, biasFactor);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout, 0, 1, &s->set, 0, nullptr);
    VkDeviceSize vboOff = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &s->vbo, &vboOff);
    vkCmdBindIndexBuffer(cb, s->ibo, 0, VK_INDEX_TYPE_UINT32);

    unsigned long typeRuns = 0;
    uint32_t i = 0;
    while (i < instanceCount) {
        const uint32_t tid = insts[order[i]].typeID;
        uint32_t j = i;
        while (j < instanceCount && insts[order[j]].typeID == tid) ++j;
        const uint32_t runCount = j - i;
        if (tid < s->typeRuns.size()) {
            const TypeRun& tr = s->typeRuns[tid];
            for (uint32_t p = 0; p < tr.packetCount; ++p) {
                const uint32_t pk = tr.firstPacket + p;
                if (pk >= s->packets.size()) break;
                const Mc2ShadowPacket& pkt = s->packets[pk];
                vkCmdDrawIndexed(cb, pkt.indexCount, runCount,
                                 pkt.firstIndex, (int32_t)pkt.baseVertex,
                                 /*firstInstance*/ i);
            }
            ++typeRuns;
        }
        i = j;
    }
    vkCmdEndRenderPass(cb);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) { setFallback("csm_fullmap_record_failed"); s->disabled = true; return 0; }

    const bool directChain = shadowDirectChainRequested();
    const bool useGpuVk2Gl = !directChain && s->csmFullMapGlView.vkToGlImported && (fs.vk2gl != VK_NULL_HANDLE);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (useGpuVk2Gl) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &fs.vk2gl;
    }
    if (vkQueueSubmit(s->queue, 1, &si, fs.fence) != VK_SUCCESS) { setFallback("csm_fullmap_submit_failed"); s->disabled = true; return 0; }
    if (useGpuVk2Gl) {
        glInteropWaitFromVk(s->csmFullMapGlView, GL_LAYOUT_DEPTH_STENCIL_READ_ONLY_EXT, fs.parity);
    }
    if (vkWaitForFences(s->device, 1, &fs.fence, VK_TRUE, 5ull*1000*1000*1000) != VK_SUCCESS) {
        setFallback("csm_fullmap_fence_timeout"); s->disabled = true; return 0;
    }

    g_health.instRecords = instanceCount;
    g_health.typeRuns = typeRuns;
    ++g_health.usedVulkan;
    if (!directChain) ++g_health.usedInterop;
    static bool s_firstOk = false;
    if (!s_firstOk) {
        vlog("first VK CSM full-map produced: instCount=%u typeRuns=%lu tex=%u.",
             instanceCount, typeRuns, s->csmFullMapGlView.glTex);
        s_firstOk = true;
    }
    g_csmFullMapReadyThisFrame = true;
    return s->csmFullMapGlView.glTex;
}

extern "C" unsigned int mc2_vk_shadow_island_run_csm_array_layer(
    int size, int layers, int layerIndex, const float* lightSpace16,
    const void* instances, unsigned int instanceCount,
    float biasFactor, float biasUnits) {
    if (layerIndex == 0) {
        g_csmArrayReadyThisFrame = false;
        if (g_island) g_island->csmArrayRenderedMask = 0;
    }
    if (!shadowInteropGateRequested() || !csmArrayInteropGateRequested()) return 0;
    noteShadowDirectChain();
    if (size <= 0 || layers <= 0 || layerIndex < 0 || layerIndex >= layers) return 0;
    if (!lightSpace16 || !instances || instanceCount == 0) return 0;
    if (!g_island || g_island->disabled || !g_island->inited || !g_island->useInterop) return 0;
    ShadowIsland* s = g_island;
    const uint32_t layerCount = (uint32_t)layers;
    const uint32_t layer = (uint32_t)layerIndex;
    if (!upload_geometry_once(s)) { setFallback("geometry_not_ready"); return 0; }
    if (!ensure_csm_array_target(s, size, layerCount)) return 0;

    const VkDeviceSize instBytes = (VkDeviceSize)instanceCount * sizeof(GpuStaticPropInstanceMirror);
    if (!ensure_inst_capacity(s, instBytes)) { s->disabled = true; return 0; }
    const GpuStaticPropInstanceMirror* insts =
        reinterpret_cast<const GpuStaticPropInstanceMirror*>(instances);
    std::vector<uint32_t> order(instanceCount);
    for (uint32_t k = 0; k < instanceCount; ++k) order[k] = k;
    std::sort(order.begin(), order.end(),
        [insts](uint32_t a, uint32_t b){ return insts[a].typeID < insts[b].typeID; });
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->instAlloc, &p) == VK_SUCCESS) {
            GpuStaticPropInstanceMirror* dst = reinterpret_cast<GpuStaticPropInstanceMirror*>(p);
            for (uint32_t k = 0; k < instanceCount; ++k) dst[k] = insts[order[k]];
            vmaUnmapMemory(s->allocator, s->instAlloc);
        }
    }
    {
        ShadowFrameUBO u{};
        std::memcpy(u.lightSpaceMatrix, lightSpace16, sizeof(u.lightSpaceMatrix));
        u.instBase = 0;
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->uboAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, &u, sizeof(u));
            vmaUnmapMemory(s->allocator, s->uboAlloc);
        }
    }

    if (vkWaitForFences(s->device, 1, &s->csmArrayFence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        setFallback("csm_array_fence_wait_failed"); s->disabled = true; return 0;
    }
    vkResetFences(s->device, 1, &s->csmArrayFence);
    VkCommandBuffer cb = s->cbuf[layer & 1u];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("csm_array_record_failed"); s->disabled = true; return 0; }

    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = s->rpass;
    rbi.framebuffer = s->csmArrayFbs[layer];
    rbi.renderArea = {{0, 0}, {(uint32_t)size, (uint32_t)size}};
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{0.0f, (float)size, (float)size, -(float)size, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {(uint32_t)size, (uint32_t)size}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    vkCmdSetDepthBias(cb, biasUnits, 0.0f, biasFactor);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout, 0, 1, &s->set, 0, nullptr);
    VkDeviceSize vboOff = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &s->vbo, &vboOff);
    vkCmdBindIndexBuffer(cb, s->ibo, 0, VK_INDEX_TYPE_UINT32);

    unsigned long typeRuns = 0;
    uint32_t i = 0;
    while (i < instanceCount) {
        const uint32_t tid = insts[order[i]].typeID;
        uint32_t j = i;
        while (j < instanceCount && insts[order[j]].typeID == tid) ++j;
        const uint32_t runCount = j - i;
        if (tid < s->typeRuns.size()) {
            const TypeRun& tr = s->typeRuns[tid];
            for (uint32_t p = 0; p < tr.packetCount; ++p) {
                const uint32_t pk = tr.firstPacket + p;
                if (pk >= s->packets.size()) break;
                const Mc2ShadowPacket& pkt = s->packets[pk];
                vkCmdDrawIndexed(cb, pkt.indexCount, runCount,
                                 pkt.firstIndex, (int32_t)pkt.baseVertex,
                                 /*firstInstance*/ i);
            }
            ++typeRuns;
        }
        i = j;
    }
    vkCmdEndRenderPass(cb);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) { setFallback("csm_array_record_failed"); s->disabled = true; return 0; }

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (vkQueueSubmit(s->queue, 1, &si, s->csmArrayFence) != VK_SUCCESS) {
        setFallback("csm_array_submit_failed"); s->disabled = true; return 0;
    }
    if (vkWaitForFences(s->device, 1, &s->csmArrayFence, VK_TRUE, 5ull*1000*1000*1000) != VK_SUCCESS) {
        setFallback("csm_array_fence_timeout"); s->disabled = true; return 0;
    }

    s->csmArrayRenderedMask |= (1u << layer);
    const uint32_t readyMask = (1u << layerCount) - 1u;
    g_csmArrayReadyThisFrame = ((s->csmArrayRenderedMask & readyMask) == readyMask);
    g_health.instRecords = instanceCount;
    g_health.typeRuns = typeRuns;
    ++g_health.usedVulkan;
    static bool s_firstOk = false;
    if (g_csmArrayReadyThisFrame && !s_firstOk) {
        vlog("first VK CSM array produced: layers=%u instCount=%u typeRuns=%lu.",
             layerCount, instanceCount, typeRuns);
        s_firstOk = true;
    }
    return g_csmArrayReadyThisFrame ? 1u : 0u;
}

// VK-SHADOW-DIRECT-CONSUME-1: raw VK-side view/ready accessors for the terrain
// island. g_dynShadowReadyThisFrame is set true at the tail of a successful
// mc2_vk_shadow_island_run() (after the fence wait retired the submit, i.e. the
// depth image content + layout transition to DEPTH_STENCIL_READ_ONLY_OPTIMAL are
// already complete) and reset false at the top of every call (including a failed/
// declined one), so a stale prior-frame view is never reported ready this frame.
extern "C" void* mc2_vk_shadow_island_get_dyn_shadow_view() {
    return (g_island && g_dynShadowReadyThisFrame)
        ? reinterpret_cast<void*>(g_island->depthResource.view) : nullptr;
}
extern "C" void* mc2_vk_shadow_island_get_dyn_shadow_sampler() {
    // The shadow island itself has no VkSampler (GL owned all sampling before this
    // slice); the terrain island's own compare-mode PCF sampler is reused by the
    // caller instead. Kept as a stub for API symmetry / future direct-VK sampler use.
    return nullptr;
}
extern "C" int mc2_vk_shadow_island_dyn_shadow_ready() {
    return (g_island && g_dynShadowReadyThisFrame) ? 1 : 0;
}

extern "C" void* mc2_vk_shadow_island_get_csm_fullmap_view() {
    return (g_island && g_csmFullMapReadyThisFrame)
        ? reinterpret_cast<void*>(g_island->csmFullMapResource.view) : nullptr;
}
extern "C" int mc2_vk_shadow_island_csm_fullmap_ready() {
    return (g_island && g_csmFullMapReadyThisFrame) ? 1 : 0;
}

extern "C" void* mc2_vk_shadow_island_get_csm_array_view() {
    return (g_island && g_csmArrayReadyThisFrame)
        ? reinterpret_cast<void*>(g_island->csmArrayView) : nullptr;
}
extern "C" int mc2_vk_shadow_island_csm_array_ready() {
    return (g_island && g_csmArrayReadyThisFrame) ? 1 : 0;
}

// Health accessor (mirrors mc2_render_backend_water_region_health).
extern "C" void mc2_render_backend_shadow_region_health(
    int* vulkanAvailable, int* buildEnabled, const char** deviceName,
    unsigned long* validationErrors, unsigned long* attempted, unsigned long* usedVulkan,
    const char** fallbackReason, unsigned long* vkDraws,
    int* interopActive, int* vk2glSemActive, unsigned long* usedInterop,
    unsigned long* instRecords, unsigned long* typeRuns, unsigned long* geomVerts,
    unsigned long* forbiddenCopyImgToBuf, unsigned long* forbiddenGetTexImage,
    unsigned long* forbiddenReadPixels,
    double* stageInstUs, double* drawUs, double* totalUs) {
    if (vulkanAvailable)  *vulkanAvailable  = g_health.vulkanAvailable;
    if (buildEnabled)     *buildEnabled     = g_health.buildEnabled;
    if (deviceName)       *deviceName       = g_health.deviceName;
    if (validationErrors) *validationErrors = g_health.validationErrors;
    if (attempted)        *attempted        = g_health.attempted;
    if (usedVulkan)       *usedVulkan       = g_health.usedVulkan;
    if (fallbackReason)   *fallbackReason   = g_health.fallbackReason;
    if (vkDraws)          *vkDraws          = g_health.vkDraws;
    if (interopActive)    *interopActive    = g_health.interopActive;
    if (vk2glSemActive)   *vk2glSemActive   = g_health.vk2glSemActive;
    if (usedInterop)      *usedInterop      = g_health.usedInterop;
    if (instRecords)      *instRecords      = g_health.instRecords;
    if (typeRuns)         *typeRuns         = g_health.typeRuns;
    if (geomVerts)        *geomVerts        = g_health.geomVerts;
    if (forbiddenCopyImgToBuf) *forbiddenCopyImgToBuf = g_forbidden.vkCmdCopyImageToBuffer;
    if (forbiddenGetTexImage)  *forbiddenGetTexImage  = g_forbidden.glGetTexImage;
    if (forbiddenReadPixels)   *forbiddenReadPixels   = g_forbidden.glReadPixels;
    if (stageInstUs)      *stageInstUs      = g_health.stageInstUs;
    if (drawUs)           *drawUs           = g_health.drawUs;
    if (totalUs)          *totalUs          = g_health.totalUs;
}

// Teardown (called from gosPostProcess::destroy or renderer shutdown).
extern "C" void mc2_vk_shadow_island_destroy() {
    vlog("[VK_SHADOW_INTEROP] vulkan_available=%d used_vulkan=%lu used_interop=%lu interop_active=%d "
         "vk2gl_sem=%d direct_chain=%d validation_errors=%lu inst_records=%lu type_runs=%lu geom_verts=%lu "
         "fallback_reason=\"%s\" vk_draws=%lu forbidden_copyImgToBuf=%lu forbidden_getTexImage=%lu "
         "forbidden_readPixels=%lu stage_inst_us=%.1f draw_us=%.1f total_us=%.1f",
         g_health.vulkanAvailable, g_health.usedVulkan, g_health.usedInterop, g_health.interopActive,
         g_health.vk2glSemActive, g_health.directChain, g_health.validationErrors, g_health.instRecords, g_health.typeRuns,
         g_health.geomVerts, g_health.fallbackReason, g_health.vkDraws,
         g_forbidden.vkCmdCopyImageToBuffer, g_forbidden.glGetTexImage, g_forbidden.glReadPixels,
         g_health.stageInstUs, g_health.drawUs, g_health.totalUs);
    if (!g_island) return;
    destroy_island(g_island);
    delete g_island; g_island = nullptr;
}

#endif // MC2_VULKAN_ISLAND
