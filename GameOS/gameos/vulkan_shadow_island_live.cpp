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
//     (ensureEngineVulkanContext + getOrCreateEngineVulkanContext). NO
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
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include "../../RenderCore/gpu_abi/GpuStaticPropInstance.h"  // single-source 112B instance record
#include "vk_image_resource.h"
#include "vk_island_util.h"
#include "vk_owned.h"
#include "vulkan_context.h"
#include "vulkan_shadow_island_live.h"
#include "vk_shared_gbuffer_gate.h"

#include <tracy/Tracy.hpp>   // VK-ISLAND-TRACY-1: per-frame CPU attribution zones

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstddef>   // offsetof (M2 layout asserts)
#include <vector>
#include <memory>
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
    vk_island_util::vlogv("[VK_SHADOW_ISLAND] ", fmt, ap);
    va_end(ap);
}

bool shadowInteropGateRequested() {
    static int state = -1;
    if (state == -1) {
        state = mc2::gate::get<"MC2_VK_SHADOW_INTEROP">();
    }
    return state == 1;
}

bool csmFullMapInteropGateRequested() {
    static int state = -1;
    if (state == -1) {
        state = mc2::gate::get<"MC2_VK_CSM_FULLMAP_INTEROP">();
    }
    return state == 1;
}

bool csmArrayInteropGateRequested() {
    static int state = -1;
    if (state == -1) {
        state = mc2::gate::get<"MC2_VK_CSM_ARRAY_INTEROP">();
    }
    return state == 1;
}

// The island is unconditionally direct-chain-native. Kept as a named constant
// documenting that the depth images are non-exportable and consumed via VkImageView only.
constexpr bool kShadowDirectChain = true;

std::string spv_dir() {
    std::string d = mc2::gate::str<"MC2_VULKAN_SPV_DIR">();   // row default shaders/vulkan, empty reads the default
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
    return d;
}

VkShaderModule load_spv(VkDevice device, std::string_view path) {
    return vk_island_util::loadSpv(device, path, vlog);
}


// std140 per-frame UBO (LOCKSTEP with shadow_prop.vert ShadowFrameUBO).
struct ShadowFrameUBO {
    float lightSpaceMatrix[16]; // getDynamicLightSpaceMatrix() — GL_FALSE   (off 0)
    int32_t instBase;           //                                            (off 64)
    int32_t _pad0, _pad1, _pad2;
};
static_assert(sizeof(ShadowFrameUBO) == 80, "ShadowFrameUBO std140 size (shadow_prop.vert)");
static_assert(offsetof(ShadowFrameUBO, instBase) == 64, "ShadowFrameUBO instBase offset");

const VkFormat kDepthFmt = VK_FORMAT_D32_SFLOAT;

// Per-type packet (subset of GpuStaticPropPacket the draw loop needs). Copied once
// per map from the batcher's accessor.
struct TypeRun {
    uint32_t typeID;
    uint32_t firstPacket;
    uint32_t packetCount;
};

// The persistent island state. The engine context owns it while it is parked (see IslandSlot::Shadow), so the objects below are released by the
// context ahead of the allocator and the device they were made on.
struct ShadowIsland : mc2vk::IslandState {
    bool disabled = false;
    bool inited   = false;
    bool useInterop = false;
    // The island renders VK-owned (non-exportable) products for direct VK consumers;
    // useInterop stays false (no GL view/semaphore machinery) but the producers MUST
    // still run. The run entry points gate on (useInterop || directChain). Without
    // this, the resource-strict stack silently fell back to the terrain island's
    // per-frame GL CSM depth readback (8192^2 x layers glGetTexImage ~= 125 ms/frame).
    bool directChain = false;

    int size = 0;   // square shadow map side (getDynamicShadowMapSize())

    // Borrowed from the shared engine context (NOT owned).
    VkInstance       instance  = VK_NULL_HANDLE;
    VkPhysicalDevice phys      = VK_NULL_HANDLE;
    VkDevice         device    = VK_NULL_HANDLE;
    uint32_t         gfxFamily = UINT32_MAX;
    VkQueue          queue     = VK_NULL_HANDLE;
    VmaAllocator     allocator = VK_NULL_HANDLE; // borrowed from ctx

    // VK-PRODUCED D32 depth (the shadow map). Non-exportable; consumed via VkImageView. The image resources release their image in the destructor below.
    VkImageResource depthResource{};   // D32, depth attachment + sampled
    VkImageResource csmFullMapResource{};
    int               csmFullMapSize = 0;
    int               csmArraySize = 0;
    uint32_t          csmArrayLayers = 0;
    uint32_t          csmArrayRenderedMask = 0;

    uint32_t      vertexStride = 0;
    bool          geomUploaded = false;
    VkDeviceSize  instCapBytes = 0;

    // Per-type packets + type runs (copied once per map).
    std::vector<Mc2ShadowPacket> packets;  // firstIndex/indexCount/baseVertex per packet
    std::vector<TypeRun>         typeRuns;  // firstPacket/packetCount per typeID (indexed by typeID)

    VkDescriptorSet   set    = VK_NULL_HANDLE;   // allocated from `pool`, freed with it
    VkCommandBuffer   cbuf[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // allocated from `cpool`, freed with it
    bool descNeedsRewrite = true;

    // OWNED objects, declared in creation order: the destructor releases them in reverse, so a framebuffer goes before the view and render pass it uses.
    mc2vk::OwnedBuffer vbo;   // VK-owned copies of the shared static-prop VBO/IBO (P8a (B))
    mc2vk::OwnedBuffer ibo;
    mc2vk::OwnedBuffer instBuf;   // per-frame instance SSBO (memcpy of the GpuStaticPropInstance vector)
    mc2vk::OwnedBuffer ubo;
    mc2vk::OwnedImageView   depthAttView;   // depth-attachment view of the shared image
    mc2vk::OwnedRenderPass  rpass;
    mc2vk::OwnedFramebuffer fb;
    mc2vk::OwnedImageView   csmFullMapDepthAttView;
    mc2vk::OwnedFramebuffer csmFullMapFb;
    mc2vk::OwnedImage       csmArrayImg;
    mc2vk::OwnedImageView   csmArrayView;
    std::vector<mc2vk::OwnedImageView>   csmArrayLayerViews;
    std::vector<mc2vk::OwnedFramebuffer> csmArrayFbs;
    mc2vk::OwnedFence       csmArrayFence;
    mc2vk::OwnedDescriptorSetLayout dsl;
    mc2vk::OwnedDescriptorPool      pool;
    mc2vk::OwnedPipelineLayout      playout;
    mc2vk::OwnedShaderModule        vert;
    mc2vk::OwnedShaderModule        frag;
    mc2vk::OwnedPipeline            pipe;
    mc2vk::OwnedCommandPool         cpool;

    ShadowIsland() = default;
    // The views and framebuffers over the two image resources go first; the resources hold their images with raw allocations and are released here.
    ~ShadowIsland() override {
        fb.reset(); depthAttView.reset();
        csmFullMapFb.reset(); csmFullMapDepthAttView.reset();
        csmArrayFbs.clear(); csmArrayLayerViews.clear(); csmArrayView.reset();
        if (device != VK_NULL_HANDLE) {
            vkImageResourceDestroy(&depthResource, device);
            vkImageResourceDestroy(&csmFullMapResource, device);
        }
    }
};

std::unique_ptr<mc2vk::IslandState>& islandSlot() {
    return mc2vk::getOrCreateEngineVulkanContext().islandSlot(mc2vk::IslandSlot::Shadow);
}
ShadowIsland* parkedIsland() { return static_cast<ShadowIsland*>(islandSlot().get()); }

// VK-RUNG8 item-2 (boot-level native-or-decline), mirrors the mech/terrain islands: a
// PERMANENT init failure (spv/pipeline/renderpass/buffer creation past the context +
// gate readiness gates) declines the shadow island for the SESSION with ONE receipt line
// (GL owns the dynamic shadow pass) instead of re-newing + re-init'ing every frame. A
// TRANSIENT readiness gate (VK context not born / gate-off / geometry not finalized) keeps
// the per-frame fail-soft-retry. g_shadowInitReachedBuild flips to 1 once init passes the
// context + gate readiness gates (i.e. any later failure is a permanent build failure).
bool g_shadowSessionDeclined  = false;
int  g_shadowInitReachedBuild = 0;

bool make_buffer(ShadowIsland* s, mc2vk::OwnedBuffer& out, VkDeviceSize size, VkBufferUsageFlags usage,
                 bool hostVisible, std::string_view name, int i0 = -1, int i1 = -1) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size ? size : 16; bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = hostVisible ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : 0;
    if (!out.create(s->allocator, s->device, bci, aci, name, i0, i1)) {
        vlog("vmaCreateBuffer failed."); return false;
    }
    return true;
}

// Writes the instances into a mapped instance buffer in the sorted order. A mapped allocation is only aligned to its buffer's offset alignment, which can be
// less than the instance's alignas(16): a typed struct copy lets the compiler use aligned 16-byte stores and the process faults (access violation at
// 0xFFFF...) when the allocator hands out a 4- or 8-aligned offset, so the bytes go through memcpy. The caller names the buffer's capacity: a longer order is
// refused (and logged) instead of writing past the allocation.
void copy_sorted_instances(void* mapped, VkDeviceSize capBytes, const GpuStaticPropInstance* insts, const std::vector<uint32_t>& order) {
    if (static_cast<VkDeviceSize>(order.size()) * sizeof(GpuStaticPropInstance) > capBytes) {
        vlog("instance re-stage refused: %zu instances do not fit the %llu-byte buffer.", order.size(), static_cast<unsigned long long>(capBytes));
        return;
    }
    uint8_t* dst = static_cast<uint8_t*>(mapped);
    for (size_t k = 0; k < order.size(); ++k)
        std::memcpy(dst + k * sizeof(GpuStaticPropInstance), &insts[order[k]], sizeof(GpuStaticPropInstance));
}

void write_descriptors(ShadowIsland* s) {
    if (s->set == VK_NULL_HANDLE) return;
    VkDescriptorBufferInfo uboInfo{ s->ubo.get(), 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo instInfo{ s->instBuf.get(), 0, VK_WHOLE_SIZE };
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
    if (s->instBuf && bytes <= s->instCapBytes) return true;
    if (s->instBuf) {
        mc2vk::getOrCreateEngineVulkanContext().waitIdle("shadow_inst_grow");
        s->instBuf.reset();
    }
    VkDeviceSize cap = bytes + bytes / 2 + 4096;
    if (!make_buffer(s, s->instBuf, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, "shadow.instBuf"))
        return false;
    s->instCapBytes = cap;
    write_descriptors(s);
    return true;
}

// One-time init: borrow the shared device, build the exportable D32 shadow image +
// its GL interop view (GL_DEPTH_COMPONENT32F, compare-mode), depth-attachment view,
// depth-only render pass + framebuffer, instance/UBO buffers, pipeline, cmd pool.
bool init_island(ShadowIsland* s, int size) {
    s->size = size;
    ++g_health.attempted;
    g_shadowInitReachedBuild = 0;   // reset; flips to 1 after readiness gates pass

    if (!mc2vk::ensureEngineVulkanContext()) {
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

    // Readiness gates (context + gate) passed: any failure past this point is a PERMANENT
    // build failure -> the island declines for the session (item-2 boot-decline).
    g_shadowInitReachedBuild = 1;

    // ---- VK-native D32 depth image. SAMPLED (native screenfx samples the VkImageView)
    // + DEPTH_STENCIL_ATTACHMENT (VK renders into it). Non-exportable (VK-RUNG8-ITEM6). ----
    const bool directChain = kShadowDirectChain;
    bool ok = vkImageResourceCreate(&s->depthResource, s->device, s->phys,
                                    (uint32_t)size, (uint32_t)size, kDepthFmt,
                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT);
    if (ok) mc2vk::nameObject(s->device, s->depthResource.image.get(), "shadow.depthResource");
    if (!ok) {
        vkImageResourceDestroy(&s->depthResource, s->device);
        setFallback("depth_resource_setup_failed");
        return false; // P8a is native-only; no readback fallback path
    }

    s->useInterop = !directChain;      // false (item-4: no GL<->VK semaphore machinery)
    s->directChain = directChain;      // true
    g_health.directChain = 1;
    g_health.interopActive = 0;        // item-4: no interop handoff
    g_health.vk2glSemActive = 0;       // item-4: no vk2gl semaphore
    vlog("depth producer ACTIVE — VK-native D32 shadow map (direct-chain; GL<->VK handoff deleted).");

    // ---- Depth-attachment view of the shared image (the interop view's own
    // .view is the SAMPLED view; the render pass needs a DEPTH-aspect attachment
    // view — same image, depth aspect). ----
    {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = s->depthResource.image.get();
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = kDepthFmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (!mc2vk::create(s->depthAttView, s->device, [&](auto* h) { return vkCreateImageView(s->device, &vi, nullptr, h); })) {
            vlog("depth attachment view failed."); return false;
        }
    }

    // ---- Instance SSBO + UBO. ----
    if (!ensure_inst_capacity(s, 256 * 1024)) return false;  // ~2340 instances to start
    if (!make_buffer(s, s->ubo, sizeof(ShadowFrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, "shadow.ubo")) return false;

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
        if (!mc2vk::create(s->rpass, s->device, [&](auto* h) { return vkCreateRenderPass(s->device, &rpci, nullptr, h); })) {
            vlog("vkCreateRenderPass failed."); return false;
        }
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        const VkImageView attView = s->depthAttView.get();
        fci.renderPass = s->rpass.get(); fci.attachmentCount = 1; fci.pAttachments = &attView;
        fci.width = (uint32_t)size; fci.height = (uint32_t)size; fci.layers = 1;
        if (!mc2vk::create(s->fb, s->device, [&](auto* h) { return vkCreateFramebuffer(s->device, &fci, nullptr, h); })) {
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
        if (!mc2vk::create(s->dsl, s->device, [&](auto* h) { return vkCreateDescriptorSetLayout(s->device, &lci, nullptr, h); })) {
            vlog("vkCreateDescriptorSetLayout failed."); return false;
        }
        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sizes[0].descriptorCount = 1;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sizes[1].descriptorCount = 1;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 1; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
        if (!mc2vk::create(s->pool, s->device, [&](auto* h) { return vkCreateDescriptorPool(s->device, &pci, nullptr, h); })) {
            vlog("vkCreateDescriptorPool failed."); return false;
        }
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        const VkDescriptorSetLayout dslHandle = s->dsl.get();
        dsai.descriptorPool = s->pool.get(); dsai.descriptorSetCount = 1; dsai.pSetLayouts = &dslHandle;
        if (vkAllocateDescriptorSets(s->device, &dsai, &s->set) != VK_SUCCESS) {
            vlog("vkAllocateDescriptorSets failed."); return false;
        }
        write_descriptors(s);
    }

    // ---- Pipeline: depth-only, one vec3 position stream, instanced, LESS forward-Z,
    // depthBias set dynamically each frame (matches glPolygonOffset). ----
    {
        s->vert = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "shadow_prop.vert.spv"));
        s->frag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "shadow_prop.frag.spv"));
        if (!s->vert || !s->frag) { setFallback("spv_load_failed"); return false; }

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        const VkDescriptorSetLayout setLayout = s->dsl.get();
        plci.setLayoutCount = 1; plci.pSetLayouts = &setLayout;
        if (!mc2vk::create(s->playout, s->device, [&](auto* h) { return vkCreatePipelineLayout(s->device, &plci, nullptr, h); })) {
            vlog("vkCreatePipelineLayout failed."); return false;
        }

        VkVertexInputBindingDescription vib{};
        vib.binding = 0; vib.stride = mc2_static_prop_vertex_stride();  // pipeline is baked before geometry upload: s->vertexStride is still 0 here
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
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = s->vert.get(); stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = s->frag.get(); stages[1].pName = "main";
        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2; gp.pStages = stages;
        gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds; gp.pColorBlendState = &cb;
        gp.pDynamicState = &dsci;
        gp.layout = s->playout.get(); gp.renderPass = s->rpass.get(); gp.subpass = 0;
        if (!mc2vk::create(s->pipe, s->device, [&](auto* h) { return vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h); })) {
            vlog("vkCreateGraphicsPipelines failed."); return false;
        }
        mc2vk::nameObject(s->device, s->pipe.get(), "shadow.pipe");
    }

    // ---- Command pool + 2 command buffers (per parity). ----
    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s->gfxFamily;
        if (!mc2vk::create(s->cpool, s->device, [&](auto* h) { return vkCreateCommandPool(s->device, &pci, nullptr, h); })) {
            vlog("vkCreateCommandPool failed."); return false;
        }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = s->cpool.get(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 2;
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
    if (!s || size <= 0 || !s->rpass) return false;
    if (s->csmFullMapResource.image.get() != VK_NULL_HANDLE && s->csmFullMapSize == size)
        return true;

    mc2vk::getOrCreateEngineVulkanContext().waitIdle("shadow_csm_fullmap_rebuild");
    s->csmFullMapFb.reset();   // the framebuffer and the view go before the image they cover
    s->csmFullMapDepthAttView.reset();
    if (s->csmFullMapResource.image.get() != VK_NULL_HANDLE)
        vkImageResourceDestroy(&s->csmFullMapResource, s->device);
    s->csmFullMapResource = {};
    s->csmFullMapSize = 0;

    // VK-RUNG8-ITEM6: non-exportable; consumed via VkImageView (csmFullMapDepthAttView).
    bool ok = vkImageResourceCreate(&s->csmFullMapResource, s->device, s->phys,
                                    (uint32_t)size, (uint32_t)size, kDepthFmt,
                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT);
    if (ok) mc2vk::nameObject(s->device, s->csmFullMapResource.image.get(), "shadow.csmFullMap");
    if (!ok) {
        vkImageResourceDestroy(&s->csmFullMapResource, s->device);
        setFallback("csm_fullmap_resource_setup_failed");
        return false;
    }

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = s->csmFullMapResource.image.get();
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = kDepthFmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (!mc2vk::create(s->csmFullMapDepthAttView, s->device, [&](auto* h) { return vkCreateImageView(s->device, &vi, nullptr, h); })) {
        setFallback("csm_fullmap_att_view_failed");
        return false;
    }

    VkFramebufferCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    const VkImageView fullMapView = s->csmFullMapDepthAttView.get();
    fci.renderPass = s->rpass.get();
    fci.attachmentCount = 1;
    fci.pAttachments = &fullMapView;
    fci.width = (uint32_t)size;
    fci.height = (uint32_t)size;
    fci.layers = 1;
    if (!mc2vk::create(s->csmFullMapFb, s->device, [&](auto* h) { return vkCreateFramebuffer(s->device, &fci, nullptr, h); })) {
        setFallback("csm_fullmap_framebuffer_failed");
        return false;
    }
    s->csmFullMapSize = size;
    vlog("CSM full-map target initialized %dx%d (exportable D32, direct terrain binding 9).",
         size, size);
    return true;
}

bool ensure_csm_array_target(ShadowIsland* s, int size, uint32_t layers) {
    if (!s || size <= 0 || layers == 0 || layers > 31 || !s->rpass) return false;
    if (s->csmArrayImg && s->csmArraySize == size && s->csmArrayLayers == layers)
        return true;

    mc2vk::getOrCreateEngineVulkanContext().waitIdle("shadow_csm_array_rebuild");
    s->csmArrayFbs.clear();   // framebuffers and views go before the image they cover
    s->csmArrayLayerViews.clear();
    s->csmArrayView.reset();
    s->csmArrayImg.reset();
    s->csmArrayFence.reset();
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
    if (!s->csmArrayImg.create(s->allocator, s->device, ici, aci, "shadow.csmArray")) {
        setFallback("csm_array_image_failed");
        return false;
    }

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = s->csmArrayImg.get();
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vci.format = kDepthFmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers};
    if (!mc2vk::create(s->csmArrayView, s->device, [&](auto* h) { return vkCreateImageView(s->device, &vci, nullptr, h); })) {
        setFallback("csm_array_view_failed");
        return false;
    }

    s->csmArrayLayerViews.resize(layers);
    s->csmArrayFbs.resize(layers);
    for (uint32_t layer = 0; layer < layers; ++layer) {
        VkImageViewCreateInfo lvi{};
        lvi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        lvi.image = s->csmArrayImg.get();
        lvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        lvi.format = kDepthFmt;
        lvi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, layer, 1};
        if (!mc2vk::create(s->csmArrayLayerViews[layer], s->device, [&](auto* h) { return vkCreateImageView(s->device, &lvi, nullptr, h); })) {
            setFallback("csm_array_layer_view_failed");
            return false;
        }
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        const VkImageView layerView = s->csmArrayLayerViews[layer].get();
        fci.renderPass = s->rpass.get();
        fci.attachmentCount = 1;
        fci.pAttachments = &layerView;
        fci.width = (uint32_t)size;
        fci.height = (uint32_t)size;
        fci.layers = 1;
        if (!mc2vk::create(s->csmArrayFbs[layer], s->device, [&](auto* h) { return vkCreateFramebuffer(s->device, &fci, nullptr, h); })) {
            setFallback("csm_array_framebuffer_failed");
            return false;
        }
    }
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (!mc2vk::create(s->csmArrayFence, s->device, [&](auto* h) { return vkCreateFence(s->device, &fci, nullptr, h); })) {
        setFallback("csm_array_fence_failed");
        return false;
    }
    s->csmArraySize = size;
    s->csmArrayLayers = layers;
    vlog("CSM array target initialized %dx%d layers=%u (VK D32 array, direct terrain binding 8).",
         size, size, layers);
    return true;
}

void release_geometry(ShadowIsland* s) {
    if (s->vbo || s->ibo) mc2vk::getOrCreateEngineVulkanContext().waitIdle("shadow_geometry_release");
    s->vbo.reset();
    s->ibo.reset();
    s->geomUploaded = false;
    s->packets.clear();
    s->typeRuns.clear();
}

// Copy the shared static-prop VBO/IBO into VK-owned buffers ONCE per map (P8a
// feed (B): VK owns a copy — simplest working feed). Also caches the per-type
// packet ranges. Returns false if the geometry feed is unavailable this frame.
bool upload_geometry_once(ShadowIsland* s) {
    if (s->geomUploaded) return true;
    release_geometry(s);   // drop any half-built buffers from a failed earlier attempt
    Mc2ShadowGeomFeed feed{};
    if (!mc2_shadow_island_get_geometry_feed(&feed)) return false;
    if (feed.vboBytes == 0 || feed.iboBytes == 0 || feed.glVbo == 0 || feed.glIbo == 0) return false;

    s->vertexStride = feed.vertexStride;
    if (feed.vertexStride != mc2_static_prop_vertex_stride())
        vlog("ERROR: feed vertexStride %u != pipeline stride %u (shadow geometry will explode).", feed.vertexStride, mc2_static_prop_vertex_stride());

    if (!make_buffer(s, s->vbo, feed.vboBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true, "shadow.vbo")) return false;
    if (!make_buffer(s, s->ibo, feed.iboBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true, "shadow.ibo")) return false;
    // The CPU staging was freed at finalize; pull the immutable GL buffer bytes ONCE
    // (map-load, not per-frame) via glGetBufferSubData into a scratch, then memcpy
    // into the VK buffers. This one-time geometry copy is NOT the shadow-map surface,
    // so it does not touch the per-frame zero-copy shadow-map invariant.
    std::vector<uint8_t> scratch;
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, s->vbo.allocation(), &p) == VK_SUCCESS) {
          scratch.resize((size_t)feed.vboBytes);
          glBindBuffer(GL_ARRAY_BUFFER, feed.glVbo);
          while (glGetError() != GL_NO_ERROR) {}
          glGetBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)feed.vboBytes, scratch.data());
          { GLenum ge = glGetError(); if (ge != GL_NO_ERROR) vlog("glGetBufferSubData(vbo) readback GL error 0x%x.", (unsigned)ge); }
          glBindBuffer(GL_ARRAY_BUFFER, 0);
          std::memcpy(p, scratch.data(), (size_t)feed.vboBytes);
          vmaFlushAllocation(s->allocator, s->vbo.allocation(), 0, VK_WHOLE_SIZE); vmaUnmapMemory(s->allocator, s->vbo.allocation());
      } }
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, s->ibo.allocation(), &p) == VK_SUCCESS) {
          scratch.resize((size_t)feed.iboBytes);
          glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, feed.glIbo);
          while (glGetError() != GL_NO_ERROR) {}
          glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, (GLsizeiptr)feed.iboBytes, scratch.data());
          { GLenum ge = glGetError(); if (ge != GL_NO_ERROR) vlog("glGetBufferSubData(ibo) readback GL error 0x%x.", (unsigned)ge); }
          glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
          std::memcpy(p, scratch.data(), (size_t)feed.iboBytes);
          vmaFlushAllocation(s->allocator, s->ibo.allocation(), 0, VK_WHOLE_SIZE); vmaUnmapMemory(s->allocator, s->ibo.allocation());
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

// ===========================================================================
// MC2_VK_SHADOW_MATRIX_DIAG (Trace gate, default OFF). Evidence-only: matrices as
// received by the island per map, GL/VK depth readbacks -> normalized gray PNG.
// ===========================================================================
bool diagOn() {
    static const bool on = mc2::gate::get<"MC2_VK_SHADOW_MATRIX_DIAG">();
    return on;
}
struct DiagRecv { bool have = false; int size = 0; int layers = 0; float m[16] = {0}; };
DiagRecv g_diagDyn, g_diagFull, g_diagArr[4];
bool g_diagTerrainArm = false;
void diagRecordDyn(int size, const float* m) {
    if (!diagOn()) return;
    g_diagDyn.have = true; g_diagDyn.size = size; std::memcpy(g_diagDyn.m, m, 64);
}
void diagRecordFull(int size, const float* m) {
    if (!diagOn()) return;
    g_diagFull.have = true; g_diagFull.size = size; std::memcpy(g_diagFull.m, m, 64);
}
void diagRecordArr(int size, int layers, int layer, const float* m) {
    if (!diagOn() || layer < 0 || layer >= 4) return;
    g_diagArr[layer].have = true; g_diagArr[layer].size = size; g_diagArr[layer].layers = layers;
    std::memcpy(g_diagArr[layer].m, m, 64);
}
void diagPrintMat(const char* tag, const float* m, int n = 16) {
    char buf[1024]; int o = 0;
    for (int i = 0; i < n; ++i) o += std::snprintf(buf + o, sizeof(buf) - o, "%s%.6g", i ? "," : "", m[i]);
    std::fprintf(stderr, "[VK_SHADOW_MATRIX v1] %s = [%s]\n", tag, buf);
}
uint32_t diagCrc(const unsigned char* p, size_t n, uint32_t crc) {
    static uint32_t table[256]; static bool init = false;
    if (!init) { for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        table[i] = c; } init = true; }
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}
void diagBe32(std::vector<unsigned char>& o, uint32_t v) {
    o.push_back((unsigned char)(v >> 24)); o.push_back((unsigned char)(v >> 16));
    o.push_back((unsigned char)(v >> 8));  o.push_back((unsigned char)v);
}
void diagChunk(std::vector<unsigned char>& o, const char tag[4], const unsigned char* d, size_t n) {
    diagBe32(o, (uint32_t)n); size_t st = o.size(); o.insert(o.end(), tag, tag + 4);
    if (n) o.insert(o.end(), d, d + n);
    diagBe32(o, diagCrc(o.data() + st, 4 + n, 0));
}
bool diagWriteGrayPng(const char* path, int w, int h, const std::vector<unsigned char>& g) {
    std::vector<unsigned char> z; z.push_back(0x78); z.push_back(0x01);
    const size_t rowB = 1 + (size_t)w, total = rowB * (size_t)h;
    uint32_t a = 1, b = 0; size_t off = 0;
    std::vector<unsigned char> blk;
    while (off < total) {
        size_t n = std::min<size_t>(65535, total - off);
        blk.resize(n);
        for (size_t i = 0; i < n; ++i) {
            size_t pos = off + i, y = pos / rowB, x = pos % rowB;
            blk[i] = x == 0 ? 0 : g[y * (size_t)w + (x - 1)];
            a = (a + blk[i]) % 65521; b = (b + a) % 65521;
        }
        bool last = off + n >= total;
        z.push_back(last ? 1 : 0);
        z.push_back((unsigned char)(n & 0xFF)); z.push_back((unsigned char)(n >> 8));
        z.push_back((unsigned char)(~n & 0xFF)); z.push_back((unsigned char)((~n >> 8) & 0xFF));
        z.insert(z.end(), blk.begin(), blk.end());
        off += n;
    }
    diagBe32(z, (b << 16) | a);
    std::vector<unsigned char> out; const unsigned char sig[8] = {137,80,78,71,13,10,26,10};
    out.insert(out.end(), sig, sig + 8);
    unsigned char ih[13] = {0};
    ih[0]=(unsigned char)(w>>24); ih[1]=(unsigned char)(w>>16); ih[2]=(unsigned char)(w>>8); ih[3]=(unsigned char)w;
    ih[4]=(unsigned char)(h>>24); ih[5]=(unsigned char)(h>>16); ih[6]=(unsigned char)(h>>8); ih[7]=(unsigned char)h;
    ih[8] = 8; ih[9] = 0;
    diagChunk(out, "IHDR", ih, 13); diagChunk(out, "IDAT", z.data(), z.size()); diagChunk(out, "IEND", nullptr, 0);
    FILE* f = std::fopen(path, "wb"); if (!f) return false;
    std::fwrite(out.data(), 1, out.size(), f); std::fclose(f); return true;
}
// Normalize [min,max] -> gray, log stats, write PNG. Depth 1.0 = cleared background.
// Row 0 of the buffer = texture row 0 (v=0) for BOTH GL and VK readbacks (no flip applied).
void diagDumpDepth(const char* mapName, const char* side, const std::vector<float>& d, int dim) {
    if (d.empty()) { std::fprintf(stderr, "[VK_SHADOW_MATRIX v1] dump map=%s side=%s EMPTY\n", mapName, side); return; }
    float mn = 1e30f, mx = -1e30f; size_t covered = 0; int x0 = dim, y0 = dim, x1 = -1, y1 = -1;
    for (size_t i = 0; i < d.size(); ++i) {
        float v = d[i]; if (v < mn) mn = v; if (v > mx) mx = v;
        if (v < 0.9999f) { ++covered; int x = (int)(i % dim), y = (int)(i / dim);
            if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    }
    std::fprintf(stderr, "[VK_SHADOW_MATRIX v1] dump map=%s side=%s dim=%d min=%.6f max=%.6f covered(<0.9999)=%zu (%.4f%%) bbox=[%d,%d]-[%d,%d]\n",
                 mapName, side, dim, mn, mx, covered, 100.0 * (double)covered / (double)d.size(), x0, y0, x1, y1);
    std::vector<unsigned char> g(d.size());
    const float span = (mx > mn) ? (mx - mn) : 1.0f;
    for (size_t i = 0; i < d.size(); ++i) g[i] = (unsigned char)std::lround(255.0f * (d[i] - mn) / span);
    char path[128]; std::snprintf(path, sizeof(path), "vk_shadowdiag_%s_%s.png", mapName, side);
    std::fprintf(stderr, "[VK_SHADOW_MATRIX v1] wrote %s ok=%d\n", path, diagWriteGrayPng(path, dim, dim, g) ? 1 : 0);
    std::fflush(stderr);
}
// One-shot GL readback (DSA only, no binding changes). layer<0 => plain 2D.
void diagGlRead(GLuint tex, int dim, int layer, std::vector<float>& out) {
    out.clear(); if (!tex || dim <= 0 || !glGetTextureSubImage) return;
    while (glGetError() != GL_NO_ERROR) {}
    GLint pb = 0; glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pb);
    if (pb) glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    out.resize((size_t)dim * dim);
    glGetTextureSubImage(tex, 0, 0, 0, layer < 0 ? 0 : layer, dim, dim, 1, GL_DEPTH_COMPONENT, GL_FLOAT,
                         (GLsizei)(out.size() * sizeof(float)), out.data());
    if (glGetError() != GL_NO_ERROR) { out.clear(); }
    if (pb) glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pb);
}
// One-shot VK readback of a D32 image layer (layout READ_ONLY_OPTIMAL in/out).
void diagVkRead(ShadowIsland* s, VkImage img, uint32_t layer, int dim, std::vector<float>& out) {
    out.clear(); if (!s || !img || dim <= 0 || !s->cpool) return;
    const VkDeviceSize bytes = (VkDeviceSize)dim * dim * 4;
    mc2vk::OwnedBuffer staging;
    if (!make_buffer(s, staging, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, "shadow.diagReadback")) return;
    const VkBuffer buf = staging.get(); const VmaAllocation al = staging.allocation();
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = s->cpool.get(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(s->device, &cai, &cb) != VK_SUCCESS) return;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier br{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    br.srcQueueFamilyIndex = br.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; br.image = img;
    br.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, layer, 1};
    br.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL; br.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    br.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; br.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &br);
    VkBufferImageCopy rg{}; rg.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, layer, 1};
    rg.imageExtent = {(uint32_t)dim, (uint32_t)dim, 1};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &rg);
    br.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; br.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    br.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; br.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &br);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    if (vkQueueSubmit(s->queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS) {
        vkQueueWaitIdle(s->queue);
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, al, &p) == VK_SUCCESS) {
            vmaInvalidateAllocation(s->allocator, al, 0, VK_WHOLE_SIZE);
            out.resize((size_t)dim * dim); std::memcpy(out.data(), p, (size_t)bytes);
            vmaUnmapMemory(s->allocator, al);
        }
    }
    vkFreeCommandBuffers(s->device, s->cpool.get(), 1, &cb);
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
    if (size <= 0 || !lightSpace16 || !instances || instanceCount == 0) return 0;
    ZoneScopedN("VKIsl.Shadow.Run");   // VK-ISLAND-TRACY-1 per-frame island total (CPU)

    using clk = std::chrono::steady_clock;
    auto usSince = [](clk::time_point t0){ return std::chrono::duration<double, std::micro>(clk::now() - t0).count(); };
    const clk::time_point tTotal0 = clk::now();

    // Item-2 boot-level native-or-decline: a prior PERMANENT init failure declined this
    // island for the whole session -> GL owns the dynamic shadow pass, no per-frame churn.
    if (g_shadowSessionDeclined) return 0;
    // Lazy init / rebuild on size change.
    mc2vk::VulkanContext& slotCtx = mc2vk::getOrCreateEngineVulkanContext();
    if (const ShadowIsland* parked = parkedIsland(); parked && parked->size != size) slotCtx.retireIsland(mc2vk::IslandSlot::Shadow);
    if (!islandSlot()) {
        auto fresh = std::make_unique<ShadowIsland>();
        if (!init_island(fresh.get(), size)) {
            const bool permanent = (g_shadowInitReachedBuild != 0);
            slotCtx.waitIdle("shadow_init_failure");   // objects made before the failure may have been submitted
            fresh.reset();
            if (permanent) {
                // Build-phase failure won't recover this session: latch decline + ONE
                // receipt line (vs. re-init'ing every frame). GL renders the shadow pass.
                g_shadowSessionDeclined = true;
                vlog("[VK_SHADOW_ISLAND] boot-level decline: permanent init failure (%s) -- "
                     "GL owns the dynamic shadow pass for this session (no per-frame retry).",
                     g_health.fallbackReason);
            } else {
                // Transient readiness gate (VK context / gate not ready): fail-soft-retry.
                static bool s_transientLogged = false;
                if (!s_transientLogged) {
                    vlog("init not ready (%s) — GL fallback (will retry).", g_health.fallbackReason);
                    s_transientLogged = true;
                }
            }
            return 0;
        }
        islandSlot() = std::move(fresh);
    }
    ShadowIsland* s = parkedIsland();
    // VK-TERRAIN-STAGING-PERF-1: direct-chain mode has no GL interop machinery
    // (useInterop=false) but the VK producer must still run for direct consumers.
    if (s->disabled || !s->inited || !(s->useInterop || s->directChain)) return 0;

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
    const VkDeviceSize instBytes = (VkDeviceSize)instanceCount * sizeof(GpuStaticPropInstance);
    if (!ensure_inst_capacity(s, instBytes)) { s->disabled = true; return 0; }
    {
        ZoneScopedN("VKIsl.Shadow.Feed");   // VK-ISLAND-TRACY-1: caster instance staging
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->instBuf.allocation(), &p) == VK_SUCCESS) {
            std::memcpy(p, instances, (size_t)instBytes);
            vmaFlushAllocation(s->allocator, s->instBuf.allocation(), 0, VK_WHOLE_SIZE); vmaUnmapMemory(s->allocator, s->instBuf.allocation());
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
        diagRecordDyn(size, lightSpace16);
        ShadowFrameUBO u{};
        std::memcpy(u.lightSpaceMatrix, lightSpace16, sizeof(u.lightSpaceMatrix));
        u.instBase = 0;
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->ubo.allocation(), &p) == VK_SUCCESS) {
            std::memcpy(p, &u, sizeof(u)); vmaFlushAllocation(s->allocator, s->ubo.allocation(), 0, VK_WHOLE_SIZE); vmaUnmapMemory(s->allocator, s->ubo.allocation());
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
    VkFrameSync fs{};
    { ZoneScopedN("VKIsl.Shadow.FenceWait");   // VK-ISLAND-TRACY-1: parity-slot fence wait (beginFrame)
      fs = vkImageResourceBeginFrame(&s->depthResource); }
    VkCommandBuffer cb = s->cbuf[fs.parity & 1u];

    // ---- 4) Sort caster instances by typeID into contiguous runs on the CPU side.
    // The GL path sorts dynShadowSet by typeID and draws each type-run instanced.
    // We reproduce that: build (typeID, firstInstance, instCount) runs. The instance
    // buffer we just uploaded is in the CALLER's order; to match GL we must sort. But
    // the GL path sorts a COPY too (drawDynamicPropShadows builds `sorted`), so the
    // depth result is order-independent (depth-LESS, deterministic). We sort indices
    // here to group by type for the correct per-type geometry. ----
    const GpuStaticPropInstance* insts =
        reinterpret_cast<const GpuStaticPropInstance*>(instances);
    std::vector<uint32_t> order(instanceCount);
    {
    ZoneScopedN("VKIsl.Shadow.SortRestage");   // VK-ISLAND-TRACY-1: typeID sort + sorted re-stage
    for (uint32_t k = 0; k < instanceCount; ++k) order[k] = k;
    std::sort(order.begin(), order.end(),
        [insts](uint32_t a, uint32_t b){ return insts[a].typeID < insts[b].typeID; });
    // Re-stage the instance buffer in sorted order so gl_InstanceIndex within a run
    // maps to the right transforms (firstInstance addresses the run start).
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->instBuf.allocation(), &p) == VK_SUCCESS) {
            copy_sorted_instances(p, s->instCapBytes, insts, order);
            vmaFlushAllocation(s->allocator, s->instBuf.allocation(), 0, VK_WHOLE_SIZE); vmaUnmapMemory(s->allocator, s->instBuf.allocation());
        }
    }
    }   // VK-ISLAND-TRACY-1: close VKIsl.Shadow.SortRestage

    // ---- 5) Record: depth-only render pass, per-type instanced caster draws. ----
    const clk::time_point tRender0 = clk::now();
    ZoneNamedN(zShadowRec, "VKIsl.Shadow.RecordSubmit", true);   // VK-ISLAND-TRACY-1: record+submit+inline wait (to fn return)
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};   // forward-Z clear to 1.0 (GL glClearDepth(1.0))
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = s->rpass.get(); rbi.framebuffer = s->fb.get();
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

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe.get());
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &s->set, 0, nullptr);
    VkDeviceSize vboOff = 0;
    { const VkBuffer vertexBuf = s->vbo.get(); vkCmdBindVertexBuffers(cb, 0, 1, &vertexBuf, &vboOff); }
    vkCmdBindIndexBuffer(cb, s->ibo.get(), 0, VK_INDEX_TYPE_UINT32);

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

    // ---- 6) Submit (VK-RUNG8 item-4: direct-chain only). The submit signals/waits no
    // GL<->VK semaphores. VK produces the depth (GL never wrote it), the fence below
    // orders CPU access, and the same-frame VkImageView is exposed to native consumers.
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 0; si.pWaitSemaphores = nullptr; si.pWaitDstStageMask = nullptr;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    { ZoneScopedN("VKIsl.Shadow.Submit");   // VK-ISLAND-TRACY-1: vkQueueSubmit
      if (vkQueueSubmit(s->queue, 1, &si, fs.fence) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; } }
    // WAW FIX (sync-val): fully retire THIS frame's VK submit before the next frame
    // records/submits, so the two parity command buffers never have overlapping
    // in-flight writes to the shared depth image on the queue (water's pattern).
    // VK-ISLAND-TRACY-1: NOTE — this wait is ALWAYS inline (the dyn-shadow island was
    // never fence-deferred; direct consumers sample the view same-frame). Zone it so
    // the CPU block is attributable.
    { ZoneScopedN("VKIsl.Shadow.InlineFenceWait");
      if (vkWaitForFences(s->device, 1, &fs.fence, VK_TRUE, 5ull*1000*1000*1000) != VK_SUCCESS) {
          setFallback("fence_timeout"); s->disabled = true; return 0;
      } }
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
    ++g_health.usedVulkan;   // item-4: never usedInterop (direct-chain only)
    g_health.totalUs = usSince(tTotal0);
    if (!s_firstOk) {
        vlog("first VK shadow map produced: instCount=%u typeRuns=%lu view=%p (direct VK consumers only).",
             instanceCount, g_health.typeRuns, (void*)s->depthResource.view.get());
        s_firstOk = true;
    }
    // VK-SHADOW-DIRECT-CONSUME-1: fence-waited above, layout already transitioned to
    // DEPTH_STENCIL_READ_ONLY_OPTIMAL by the render pass -- safe for a same-frame-later
    // VK consumer (the terrain island) to sample s->depthResource.view directly.
    g_dynShadowReadyThisFrame = true;
    // item-4: always direct-chain -> return 0; native VK consumers use the ready view
    // accessor. (The retained exportable GL view is the dormant item-5 substrate.)
    return 0u;
}

extern "C" unsigned int mc2_vk_shadow_island_run_csm_fullmap(
    int size, const float* lightSpace16,
    const void* instances, unsigned int instanceCount,
    float biasFactor, float biasUnits) {
    g_csmFullMapReadyThisFrame = false;
    if (!shadowInteropGateRequested() || !csmFullMapInteropGateRequested()) return 0;
    if (size <= 0 || !lightSpace16 || !instances || instanceCount == 0) return 0;
    ShadowIsland* s = parkedIsland();
    if (!s || s->disabled || !s->inited
        || !(s->useInterop || s->directChain)) return 0;
    ZoneScopedN("VKIsl.Shadow.CsmFullMap");   // VK-ISLAND-TRACY-1 per-frame total (CPU; incl. inline fence wait)
    if (!upload_geometry_once(s)) { setFallback("geometry_not_ready"); return 0; }
    if (!ensure_csm_fullmap_target(s, size)) return 0;

    const VkDeviceSize instBytes = (VkDeviceSize)instanceCount * sizeof(GpuStaticPropInstance);
    if (!ensure_inst_capacity(s, instBytes)) { s->disabled = true; return 0; }
    const GpuStaticPropInstance* insts =
        reinterpret_cast<const GpuStaticPropInstance*>(instances);
    std::vector<uint32_t> order(instanceCount);
    for (uint32_t k = 0; k < instanceCount; ++k) order[k] = k;
    std::sort(order.begin(), order.end(),
        [insts](uint32_t a, uint32_t b){ return insts[a].typeID < insts[b].typeID; });
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->instBuf.allocation(), &p) == VK_SUCCESS) {
            copy_sorted_instances(p, s->instCapBytes, insts, order);
            vmaFlushAllocation(s->allocator, s->instBuf.allocation(), 0, VK_WHOLE_SIZE); vmaUnmapMemory(s->allocator, s->instBuf.allocation());
        }
    }
    {
        diagRecordFull(size, lightSpace16);
        ShadowFrameUBO u{};
        std::memcpy(u.lightSpaceMatrix, lightSpace16, sizeof(u.lightSpaceMatrix));
        u.instBase = 0;
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->ubo.allocation(), &p) == VK_SUCCESS) {
            std::memcpy(p, &u, sizeof(u));
            vmaFlushAllocation(s->allocator, s->ubo.allocation(), 0, VK_WHOLE_SIZE); vmaUnmapMemory(s->allocator, s->ubo.allocation());
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
    rbi.renderPass = s->rpass.get();
    rbi.framebuffer = s->csmFullMapFb.get();
    rbi.renderArea = {{0, 0}, {(uint32_t)size, (uint32_t)size}};
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{0.0f, 0.0f, (float)size, (float)size, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {(uint32_t)size, (uint32_t)size}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    vkCmdSetDepthBias(cb, biasUnits, 0.0f, biasFactor);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe.get());
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &s->set, 0, nullptr);
    VkDeviceSize vboOff = 0;
    { const VkBuffer vertexBuf = s->vbo.get(); vkCmdBindVertexBuffers(cb, 0, 1, &vertexBuf, &vboOff); }
    vkCmdBindIndexBuffer(cb, s->ibo.get(), 0, VK_INDEX_TYPE_UINT32);

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

    // item-4: direct-chain only. The submit signals/waits no GL<->VK semaphores.
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (vkQueueSubmit(s->queue, 1, &si, fs.fence) != VK_SUCCESS) { setFallback("csm_fullmap_submit_failed"); s->disabled = true; return 0; }
    { ZoneScopedN("VKIsl.Shadow.CsmFullMapWait");   // VK-ISLAND-TRACY-1: ALWAYS-inline retire (never fence-deferred)
      if (vkWaitForFences(s->device, 1, &fs.fence, VK_TRUE, 5ull*1000*1000*1000) != VK_SUCCESS) {
          setFallback("csm_fullmap_fence_timeout"); s->disabled = true; return 0;
      } }

    g_health.instRecords = instanceCount;
    g_health.typeRuns = typeRuns;
    ++g_health.usedVulkan;   // item-4: never usedInterop (direct-chain only)
    static bool s_firstOk = false;
    if (!s_firstOk) {
        vlog("first VK CSM full-map produced: instCount=%u typeRuns=%lu view=%p (direct VK consumers only).",
             instanceCount, typeRuns, (void*)s->csmFullMapResource.view.get());
        s_firstOk = true;
    }
    g_csmFullMapReadyThisFrame = true;
    return 0u;   // native; consumers use mc2_vk_shadow_island_get_csm_fullmap_view()
}

extern "C" unsigned int mc2_vk_shadow_island_run_csm_array_layer(
    int size, int layers, int layerIndex, const float* lightSpace16,
    const void* instances, unsigned int instanceCount,
    float biasFactor, float biasUnits) {
    if (layerIndex == 0) {
        g_csmArrayReadyThisFrame = false;
        if (ShadowIsland* parked = parkedIsland()) parked->csmArrayRenderedMask = 0;
    }
    if (!shadowInteropGateRequested() || !csmArrayInteropGateRequested()) return 0;
    if (size <= 0 || layers <= 0 || layerIndex < 0 || layerIndex >= layers) return 0;
    if (!lightSpace16 || !instances || instanceCount == 0) return 0;
    ShadowIsland* s = parkedIsland();
    if (!s || s->disabled || !s->inited
        || !(s->useInterop || s->directChain)) return 0;
    ZoneScopedN("VKIsl.Shadow.CsmArrayLayer");   // VK-ISLAND-TRACY-1 per-layer total (CPU; incl. 2 inline fence waits)
    const uint32_t layerCount = (uint32_t)layers;
    const uint32_t layer = (uint32_t)layerIndex;
    if (!upload_geometry_once(s)) { setFallback("geometry_not_ready"); return 0; }
    if (!ensure_csm_array_target(s, size, layerCount)) return 0;

    const VkDeviceSize instBytes = (VkDeviceSize)instanceCount * sizeof(GpuStaticPropInstance);
    if (!ensure_inst_capacity(s, instBytes)) { s->disabled = true; return 0; }
    const GpuStaticPropInstance* insts =
        reinterpret_cast<const GpuStaticPropInstance*>(instances);
    std::vector<uint32_t> order(instanceCount);
    for (uint32_t k = 0; k < instanceCount; ++k) order[k] = k;
    std::sort(order.begin(), order.end(),
        [insts](uint32_t a, uint32_t b){ return insts[a].typeID < insts[b].typeID; });
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->instBuf.allocation(), &p) == VK_SUCCESS) {
            copy_sorted_instances(p, s->instCapBytes, insts, order);
            vmaFlushAllocation(s->allocator, s->instBuf.allocation(), 0, VK_WHOLE_SIZE); vmaUnmapMemory(s->allocator, s->instBuf.allocation());
        }
    }
    {
        diagRecordArr(size, layers, layerIndex, lightSpace16);
        ShadowFrameUBO u{};
        std::memcpy(u.lightSpaceMatrix, lightSpace16, sizeof(u.lightSpaceMatrix));
        u.instBase = 0;
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->ubo.allocation(), &p) == VK_SUCCESS) {
            std::memcpy(p, &u, sizeof(u));
            vmaFlushAllocation(s->allocator, s->ubo.allocation(), 0, VK_WHOLE_SIZE); vmaUnmapMemory(s->allocator, s->ubo.allocation());
        }
    }

    { ZoneScopedN("VKIsl.Shadow.CsmArrayPreWait");   // VK-ISLAND-TRACY-1: retire prior layer's submit (ALWAYS inline)
      const VkFence arrayFence = s->csmArrayFence.get();
      if (vkWaitForFences(s->device, 1, &arrayFence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
          setFallback("csm_array_fence_wait_failed"); s->disabled = true; return 0;
      } }
    { const VkFence arrayFence = s->csmArrayFence.get(); vkResetFences(s->device, 1, &arrayFence); }
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
    rbi.renderPass = s->rpass.get();
    rbi.framebuffer = s->csmArrayFbs[layer].get();
    rbi.renderArea = {{0, 0}, {(uint32_t)size, (uint32_t)size}};
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    // CSM array pass uses a NON-flipped viewport {0,0,size,size}. Verified 2026-10-09 by
    // csm0 depth-dump orientation vs GL after stride fix c26b1eb2a; the 8184a1af8 July
    // measurement predated the stride fix. A flipped viewport Y-mirrors the depth and
    // streaks terrain. Live consumer: shaders/vulkan/screen_shadow_csm.frag.
    VkViewport vp{0.0f, 0.0f, (float)size, (float)size, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {(uint32_t)size, (uint32_t)size}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    vkCmdSetDepthBias(cb, biasUnits, 0.0f, biasFactor);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe.get());
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &s->set, 0, nullptr);
    VkDeviceSize vboOff = 0;
    { const VkBuffer vertexBuf = s->vbo.get(); vkCmdBindVertexBuffers(cb, 0, 1, &vertexBuf, &vboOff); }
    vkCmdBindIndexBuffer(cb, s->ibo.get(), 0, VK_INDEX_TYPE_UINT32);

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
    if (vkQueueSubmit(s->queue, 1, &si, s->csmArrayFence.get()) != VK_SUCCESS) {
        setFallback("csm_array_submit_failed"); s->disabled = true; return 0;
    }
    { ZoneScopedN("VKIsl.Shadow.CsmArrayWait");   // VK-ISLAND-TRACY-1: ALWAYS-inline retire of this layer's submit
      const VkFence arrayFence = s->csmArrayFence.get();
      if (vkWaitForFences(s->device, 1, &arrayFence, VK_TRUE, 5ull*1000*1000*1000) != VK_SUCCESS) {
          setFallback("csm_array_fence_timeout"); s->disabled = true; return 0;
      } }

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
    return (parkedIsland() && g_dynShadowReadyThisFrame)
        ? reinterpret_cast<void*>(parkedIsland()->depthResource.view.get()) : nullptr;
}
extern "C" void* mc2_vk_shadow_island_get_dyn_shadow_sampler() {
    // The shadow island itself has no VkSampler (GL owned all sampling before this
    // slice); the terrain island's own compare-mode PCF sampler is reused by the
    // caller instead. Kept as a stub for API symmetry / future direct-VK sampler use.
    return nullptr;
}
extern "C" int mc2_vk_shadow_island_dyn_shadow_ready() {
    return (parkedIsland() && g_dynShadowReadyThisFrame) ? 1 : 0;
}

extern "C" void* mc2_vk_shadow_island_get_csm_fullmap_view() {
    return (parkedIsland() && g_csmFullMapReadyThisFrame)
        ? reinterpret_cast<void*>(parkedIsland()->csmFullMapResource.view.get()) : nullptr;
}
extern "C" void* mc2_vk_shadow_island_get_csm_fullmap_image() {
    return (parkedIsland() && g_csmFullMapReadyThisFrame)
        ? reinterpret_cast<void*>(parkedIsland()->csmFullMapResource.image.get()) : nullptr;
}
extern "C" int mc2_vk_shadow_island_csm_fullmap_ready() {
    return (parkedIsland() && g_csmFullMapReadyThisFrame) ? 1 : 0;
}

extern "C" void* mc2_vk_shadow_island_get_csm_array_view() {
    return (parkedIsland() && g_csmArrayReadyThisFrame)
        ? reinterpret_cast<void*>(parkedIsland()->csmArrayView.get()) : nullptr;
}
extern "C" void* mc2_vk_shadow_island_get_csm_array_image() {
    return (parkedIsland() && g_csmArrayReadyThisFrame)
        ? reinterpret_cast<void*>(parkedIsland()->csmArrayImg.get()) : nullptr;
}
extern "C" int mc2_vk_shadow_island_csm_array_ready() {
    return (parkedIsland() && g_csmArrayReadyThisFrame) ? 1 : 0;
}

// MC2_VK_SHADOW_MATRIX_DIAG public entries.
// Matrix qualifiers (all verified by grep: NO row_major/column_major anywhere in the shadow-related VK
// shaders): shadow_prop.vert ShadowFrameUBO std140 default (column-major) mat4 lightSpaceMatrix fed RAW
// (same as GL's glUniformMatrix4fv GL_FALSE); terrain_tier1.frag UBO dynamicLightSpaceMatrix /
// csmCascadeMatrices[3] std140 default column-major, fed RAW memcpy of the same GL arrays.
extern "C" int mc2_vk_shadow_matrix_diag_enabled() { return diagOn() ? 1 : 0; }
extern "C" int mc2_vk_shadow_matrix_diag_take_terrain_arm() {
    if (!g_diagTerrainArm) return 0;
    g_diagTerrainArm = false; return 1;
}
extern "C" void mc2_vk_shadow_matrix_diag_tick(const float* glDynMat, int dynSize, unsigned glDynTex,
                                               const float* glCascMats, int csmN, int arrSize,
                                               unsigned glArrTex, unsigned glFullTex, int fullSize,
                                               float fullTexelWorld) {
    if (!diagOn()) return;
    static int s_tick = 0; ++s_tick;
    if (s_tick != 600 && s_tick != 1200) return;
    const bool dump = (s_tick == 600);
    std::fprintf(stderr, "[VK_SHADOW_MATRIX v1] ==== tick=%d csmN=%d dynSize=%d arrSize=%d fullSize=%d fullTexelWorld=%.6g (all mats: 16 floats raw memory order, GLSL column-major load) ====\n",
                 s_tick, csmN, dynSize, arrSize, fullSize, fullTexelWorld);
    if (glDynMat) diagPrintMat("GL dynamic(getDynamicLightSpaceMatrix@tick)", glDynMat);
    for (int i = 0; glCascMats && i < csmN && i < 4; ++i) {
        char tg[96]; std::snprintf(tg, sizeof(tg), "GL cascade[%d]%s", i, i == csmN - 1 ? " (=full-map)" : "");
        diagPrintMat(tg, glCascMats + i * 16);
    }
    if (g_diagDyn.have)  { char tg[64]; std::snprintf(tg, sizeof(tg), "VK island recv dyn size=%d", g_diagDyn.size); diagPrintMat(tg, g_diagDyn.m); }
    if (g_diagFull.have) { char tg[64]; std::snprintf(tg, sizeof(tg), "VK island recv fullmap size=%d", g_diagFull.size); diagPrintMat(tg, g_diagFull.m); }
    for (int i = 0; i < 4; ++i) if (g_diagArr[i].have) {
        char tg[80]; std::snprintf(tg, sizeof(tg), "VK island recv arrayLayer[%d] size=%d layers=%d", i, g_diagArr[i].size, g_diagArr[i].layers);
        diagPrintMat(tg, g_diagArr[i].m);
    }
    g_diagTerrainArm = true;   // terrain island prints its UBO sampling matrices next
    if (ShadowIsland* parked = parkedIsland(); dump && parked && parked->inited) {
        ShadowIsland* s = parked; std::vector<float> d;
        diagGlRead(glDynTex, dynSize, -1, d);       diagDumpDepth("dyn", "gl", d, dynSize);
        diagGlRead(glArrTex, arrSize, 0, d);        diagDumpDepth("csm0", "gl", d, arrSize);
        diagGlRead(glFullTex, fullSize, -1, d);     diagDumpDepth("full", "gl", d, fullSize);
        if (g_dynShadowReadyThisFrame) { diagVkRead(s, s->depthResource.image.get(), 0, s->size, d); diagDumpDepth("dyn", "vk", d, s->size); }
        else std::fprintf(stderr, "[VK_SHADOW_MATRIX v1] VK dyn map not ready this frame\n");
        if (g_csmArrayReadyThisFrame) { diagVkRead(s, s->csmArrayImg.get(), 0, s->csmArraySize, d); diagDumpDepth("csm0", "vk", d, s->csmArraySize); }
        else std::fprintf(stderr, "[VK_SHADOW_MATRIX v1] VK csm array not ready this frame\n");
        if (g_csmFullMapReadyThisFrame) { diagVkRead(s, s->csmFullMapResource.image.get(), 0, s->csmFullMapSize, d); diagDumpDepth("full", "vk", d, s->csmFullMapSize); }
        else std::fprintf(stderr, "[VK_SHADOW_MATRIX v1] VK csm fullmap not ready this frame\n");
    }
    std::fflush(stderr);
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

// Static-prop feed invalidated (map unload/load): drop VK-owned caster geometry so the
// next map re-uploads instead of drawing stale/freed vertex data.
extern "C" void mc2_vk_shadow_island_invalidate_geometry() {
    ShadowIsland* parked = parkedIsland();
    if (!parked) return;
    release_geometry(parked);
    vlog("caster geometry invalidated (map boundary).");
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
    mc2vk::getOrCreateEngineVulkanContext().retireIsland(mc2vk::IslandSlot::Shadow);
}

#endif // MC2_VULKAN_ISLAND
