// vulkan_water_island.cpp - Vulkan water island.
// =============================================================================
// Renders the MC2 armed water fast-path mesh (base layer) in Vulkan. Tier-1
// renders in-place into the shared scene GBuffer; the old P9a side-image gate is
// retired and unreachable.
//
// SHAPE (SSAO-island seam + a real geometry draw, per VK-P9a-STEP0 note):
//   - Device: BORROWED from the shared engine VulkanContext
//     (getOrCreateEngineVulkanContext, like engine_present_vk.cpp:330-337). NO
//     second device, NO volkLoadDevice.
//   - Feed (STEP 0 decision — VK-side copy/re-pack, NOT external buffer share):
//       * recipe  = mission-static, copied ONCE at first-run into a VK SSBO from
//         WaterStream::GetRecipes()/GetRecipeCount().
//       * thin    = per-frame, CPU-staged from the caller's thin-record array
//         each frame into a VK SSBO. No fenced GL buffer crosses the seam -> the
//         GL ring-3 fence (id 43) is irrelevant to VK; R2 sync hazard dissolves.
//   - Pipeline (THIS tree's PipelineRegistry WaterArmed row, :227-249):
//       reverse-Z GEQUAL, depth-TEST on, depth-WRITE on, cull NONE, AlphaBlend
//       SRC_ALPHA/ONE_MINUS_SRC_ALPHA, MRT (color0 MainColor + color1 GBuffer1).
//       MVP fudge lives in the VS (clip.z += 0.00025*clip.w), byte-copied.
//   - Output: the VK pass LOADs scene color0+normal+depth from the shared scene
//     GBuffer, alpha-blends water OVER color0, preserves normal, writes depth, and
//     hands the same shared resources back to GL. Forbidden COLOR-op counters and
//     retired depth-IN counters MUST stay 0.
//
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
#include "vulkan_context.h"
#include "gos_terrain_water_stream.h"

#include <tracy/Tracy.hpp>   // VK-ISLAND-TRACY-1: per-frame CPU attribution zones

// RUNG 6 (VK-WATER-TIER1): in-place shared scene GBuffer co-write — the water
// draw renders directly into the SAME scene color0(MRT0)+normal(MRT1)+depth
// images GL authored (no offscreen water target, no GL composite pass, no
// glGetTexImage depth readback). Same substrate + accessors the terrain/prop/
// mech Tier-1 islands use.
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"
#include "vk_shared_gbuffer_gate.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include <chrono>

// ---------------------------------------------------------------------------
// Health snapshot (mirrors the SSAO island's) — read by debug_state_dump.cpp.
// ---------------------------------------------------------------------------
// VK-PARITY-WATER-SHORE-1: shore data producers (gos_terrain_lod_chunk.cpp, C++ linkage).
unsigned int gos_TerrainLodChunk_ShorelineMaskTex();
unsigned int gos_TerrainLodChunk_WaterShoreHeightTex();
unsigned int gos_TerrainLodChunk_WaterShoreDataGeneration();

namespace {

struct WaterIslandHealth {
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
    unsigned long usedInterop = 0;
    unsigned long thinRecords = 0;   // records fed this frame
    unsigned long recipeCount = 0;   // recipe rows uploaded once
    double copyColorInUs = 0.0, copyDepthInUs = 0.0, drawUs = 0.0, colorOutUs = 0.0, totalUs = 0.0;
};
WaterIslandHealth g_health;

void setFallback(const char* reason) {
    std::strncpy(g_health.fallbackReason, reason ? reason : "",
                 sizeof(g_health.fallbackReason) - 1);
    g_health.fallbackReason[sizeof(g_health.fallbackReason) - 1] = '\0';
}

// Forbidden COLOR-op counters (blocker #5). Any of these firing on the interop
// path means a COLOR copy slipped through -> fail the frame. DEPTH-in copies are
// EXPECTED (GL scene depth import) and counted SEPARATELY so they never trip the
// color assert.
struct InteropForbidden {
    unsigned long vkCmdCopyBufferToImage = 0;  // COLOR staging upload (must be 0)
    unsigned long vkCmdCopyImageToBuffer = 0;  // COLOR readback       (must be 0)
    unsigned long glGetTexImage          = 0;  // COLOR pull           (must be 0)
    unsigned long glTexSubImage2D        = 0;  // COLOR writeback      (must be 0)
};
InteropForbidden g_forbidden;
unsigned long g_depthInGetTexImage       = 0;  // EXPECTED (scene depth import)
unsigned long g_depthInCopyBufferToImage = 0;  // EXPECTED (staging -> depth image)

void vlog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::fprintf(stderr, "[VK_WATER_ISLAND] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

// MC2_VK_WATER_TIER1 (rung 6, VK full-ownership ladder): render the armed water
// BASE layer IN-PLACE into the shared scene GBuffer (color0+normal+depth) like
// the terrain/prop/mech Tier-1 islands — no offscreen target, no GL composite,
// no depth readback.
//
// VK-FIELD-ENV-CONTENT-STACK-ARM-1 (2026-07-11): now resolved via mc2vkgate::subGate
// so MC2_VK_SHARED_GBUFFER=1 arms it (explicit "1"/"0" still wins). Water is BASE scene
// content, not an enhancement -- post-VK-RUNG8-ITEM8 the native VK composite is the sole
// present-image producer and there is no per-layer GL fallback, so an unarmed water layer
// VANISHES (the field bug). This ALSO reconciles a latent inconsistency: gos_postprocess.cpp
// and vk_shared_gbuffer_gate.h::postDirectChainUpstreamOn() already read MC2_VK_WATER_TIER1
// via subGate, while this producer read it via envTriState==1 -- so under the umbrella the
// post chain believed water was armed while the island declined. Now both agree. The strict
// proof armed WATER_TIER1=1 and passed its parity check.
bool waterTier1Requested() {
    const bool s_on = mc2vkgate::subGate("MC2_VK_WATER_TIER1");   // per-call: VK-FAILSOFT-DISARM-1 (water must return to GL after fail-soft)
    return s_on;
}

// MC2_VK_WATER_PERF — fine-grained per-sub-step microsecond breakdown of the VK
// water interop path (additive diagnostic; zero cost when OFF). Emits a
// [VK_WATER_PERF] line per frame with the readback/stage/depth-in/sem/submit/
// fence-wait/composite split so the ~11ms Tracy zone can be attributed.
bool waterPerfGateRequested() {
    static int state = -1;
    if (state == -1) {
        const char* e = std::getenv("MC2_VK_WATER_PERF");
        state = (e && e[0] == '1') ? 1 : 0;
    }
    return state == 1;
}

// VK-WATER-FENCE-DEFER-1 (cluster-A pilot, ONE island): defer the island's retire
// wait to the parity slot's next vkImageResourceBeginFrame instead of waiting the
// submit fence inline. Default-ON (the per-parity staging pairs make it correct by
// construction); MC2_VK_WATER_FENCE_DEFER=0 is the killswitch restoring the inline
// wait — the staging stays per-parity either way (superset-safe).
bool waterFenceDeferEnabled() {
    static int state = -1;
    if (state == -1)
        state = (mc2vkgate::envTriState("MC2_VK_WATER_FENCE_DEFER") == 0) ? 0 : 1;
    return state == 1;
}

// MC2_VK_WATER_DIRECT_CHAIN -- deletion-readiness proof gate. When the prior
// terrain/prop/mech owners have kept the shared GBuffer Vulkan-owned, water can
// render in-place without another GL/VK semaphore round trip.
// VK-RUNG8-ITEM5: MC2_VK_WATER_DIRECT_CHAIN is RETIRED. Water co-write is now
// unconditionally direct-chain-native (the GL<->VK semaphore-handoff arm + the
// shared-scene GL-import half are deleted). Parseable no-op: setting it explicitly
// prints one deprecation note and changes nothing.
bool waterDirectChainRequested() {
    static bool s_noted = false;
    if (!s_noted) {
        s_noted = true;
        const char* v = std::getenv("MC2_VK_WATER_DIRECT_CHAIN");
        if (v && v[0])
            vlog("[VK_WATER_TIER1] MC2_VK_WATER_DIRECT_CHAIN is deprecated "
                 "(water is unconditionally direct-chain-native); ignoring explicit value.");
    }
    return true;
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

// std140 UBO mirrors (LOCKSTEP with water_geom.vert/.frag). ------------------
struct WaterVsParams {
    float worldToClipGL[16]; // column_major; SAME raw 16 floats as GL GL_FALSE upload
    float p0[4];             // waterElevation, alphaDepth, mapTopLeftX, mapTopLeftY
    float p1[4];             // frameCos, maxMinUV, uvScale, uvOffsetX
    float p2[4];             // uvOffsetY, fudge, alphaEdgeByte, alphaMiddleByte
    float p3[4];             // alphaDeepByte, pad, pad, pad
};
struct WaterFsParams {
    float fog_color[4];      // off 0
    float f0[4];             // off 16  time, camX, camY, camZ
    float f1[4];             // off 32  alphaDepth, ABSORPTION_DENSITY, WATER_MAX_ALPHA, RIPPLE_GAIN
    float f2[4];             // off 48  GLINT_GAIN, u_waterReflStrength, u_skyYaw, u_waterHdriLod
    float f3[4];             // off 64  screenW, screenH, pad, pad (u_waterScreenSize)
    float DEEP_COLOR[4];     // off 80
    float SHALLOW_COLOR[4];  // off 96
    // VK-PARITY-WATER-SHORE-1 (WATER-SHORE-EDGE-1) -- LOCKSTEP with water_geom.frag.
    float f4[4];             // off 112 u_shoreMode, u_hasShoreHeight, u_hasShoreMask, u_shoreFeatherWu
    float f5[4];             // off 128 u_shoreHeightBounds
    float f6[4];             // off 144 u_shoreMaskBounds
    float f7[4];             // off 160 waterElevation, pad x3
    // VK-PARITY-F3: LOCKSTEP with water_geom.frag f8/f9.
    float f8[4];             // off 176 edgeFeather, edgeFeatherStrength, halfMap, skyTintStrength
    float f9[4];             // off 192 skyTintColor.xyz, projAspect
};

// The persistent island state.
struct WaterIsland {
    bool disabled = false;
    bool inited   = false;
    bool useInterop = false;
    bool tier1    = false;   // rung 6: in-place shared scene GBuffer co-write

    // Borrowed shared scene GBuffer (tier1 only; NOT owned — postprocess createFBOs
    // owns lifetime; a resize rebuilds them AND resizes this island, which refetches).
    VkImageResource* sColorRes  = nullptr;
    VkImageResource* sNormalRes = nullptr;
    VkImageResource* sDepthRes  = nullptr;
    // VK-RUNG8-ITEM5: shared scene is VK-only (no GL interop view). Water co-writes
    // direct-chain-native only; the former s*View handoff handles are deleted.

    int w = 0, h = 0;

    // Borrowed from the shared engine context (NOT owned).
    VkInstance       instance  = VK_NULL_HANDLE;
    VkPhysicalDevice phys      = VK_NULL_HANDLE;
    VkDevice         device    = VK_NULL_HANDLE;
    uint32_t         gfxFamily = UINT32_MAX;
    VkQueue          queue     = VK_NULL_HANDLE;
    VmaAllocator     allocator = VK_NULL_HANDLE; // borrowed from ctx

    // Feed buffers (VK-side copies of the GL water feed).
    // VK-WATER-FENCE-DEFER-1 (cluster-A pilot): the per-frame-WRITTEN staging
    // (thin SSBO, vsUbo, fsUbo) and the descriptor set that binds them are
    // PER-PARITY-SLOT pairs. The inline post-submit fence wait is retired; the
    // retire guarantee moves to vkImageResourceBeginFrame's slot-fence wait,
    // which now runs BEFORE staging — slot p's previous submit is retired before
    // buffers[p] are rewritten and before cbuf[p]/set[p] are reused. Recipe
    // (mission-static after upload) and HDRI (copy-once) stay single-instance:
    // their rare (re)writes are vkDeviceWaitIdle-guarded.
    VkBuffer      recipeBuf = VK_NULL_HANDLE; VmaAllocation recipeAlloc = VK_NULL_HANDLE;
    VkDeviceSize  recipeBytes = 0;
    bool          recipeUploaded = false;
    VkBuffer      thinBuf[2]   = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VmaAllocation thinAlloc[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceSize  thinCapBytes[2] = {0, 0};   // grows to high-water-mark
    VkBuffer      vsUbo[2]     = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VmaAllocation vsUboAlloc[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkBuffer      fsUbo[2]     = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VmaAllocation fsUboAlloc[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

    // RUNG-6.5 WATER-REFLECTION: copy-once HDRI equirect import (binding 2).
    // Copied ONCE from the GL HDRI texture (BC6H mip0 sidecar in the field
    // config, or RGBA16F + full mip chain on the tinyexr fallback path) — NOT a
    // per-frame copy. hdriRealImported=false -> a 1x1 black dummy is bound and
    // the island patches fsParams' u_waterHdriLod slot to -1 (SH-L2 fallback,
    // same semantics GL uses when isHdriReady()==false).
    VkImage       hdriImage = VK_NULL_HANDLE; VmaAllocation hdriAlloc = VK_NULL_HANDLE;
    VkImageView   hdriView  = VK_NULL_HANDLE;
    VkSampler     hdriSampler = VK_NULL_HANDLE;
    bool          hdriRealImported = false;   // real GL HDRI bytes present (not dummy)
    bool          hdriAttempted    = false;   // import tried once (success or fail)
    unsigned int  hdriGlTexId      = 0;       // GL tex id the import came from

    // VK-PARITY-WATER-SHORE-1: copy-once imports of the GL water-shore textures
    // (binding 3 = R32F hi-res visual height, binding 4 = RGBA8 shoreline mask).
    // Re-imported only when gos_TerrainLodChunk_WaterShoreDataGeneration() changes
    // (new mission). A 1x1 dummy keeps the statically-used binding valid when the
    // GL texture is absent; has*=false makes the FS degrade 2 -> 1 -> 0 like GL.
    VkImage       shHImage = VK_NULL_HANDLE; VmaAllocation shHAlloc = VK_NULL_HANDLE;
    VkImageView   shHView  = VK_NULL_HANDLE;
    VkImage       shMImage = VK_NULL_HANDLE; VmaAllocation shMAlloc = VK_NULL_HANDLE;
    VkImageView   shMView  = VK_NULL_HANDLE;
    bool          shHReal = false, shMReal = false;   // real data imported (not dummy)
    bool          shoreAttempted = false;
    unsigned int  shoreGen = 0;

    VkRenderPass      rpass  = VK_NULL_HANDLE;
    VkFramebuffer     fb     = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool  pool   = VK_NULL_HANDLE;
    VkDescriptorSet   set[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // per parity slot (VK-WATER-FENCE-DEFER-1)
    VkPipelineLayout  playout = VK_NULL_HANDLE;
    VkPipeline        pipe   = VK_NULL_HANDLE;
    VkShaderModule    vert   = VK_NULL_HANDLE;
    VkShaderModule    frag   = VK_NULL_HANDLE;

    VkCommandPool     cpool  = VK_NULL_HANDLE;
    VkCommandBuffer   cbuf[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkFence           fence  = VK_NULL_HANDLE;

    bool descNeedsThinRewrite = true; // thinBuf realloc -> descriptor rewrite
};

void destroy_island(WaterIsland* s) {
    if (!s) return;
    VkDevice d = s->device;
    if (d != VK_NULL_HANDLE) vkDeviceWaitIdle(d);
    if (s->pipe)    vkDestroyPipeline(d, s->pipe, nullptr);
    if (s->playout) vkDestroyPipelineLayout(d, s->playout, nullptr);
    if (s->vert)    vkDestroyShaderModule(d, s->vert, nullptr);
    if (s->frag)    vkDestroyShaderModule(d, s->frag, nullptr);
    if (s->dsl)     vkDestroyDescriptorSetLayout(d, s->dsl, nullptr);
    if (s->pool)    vkDestroyDescriptorPool(d, s->pool, nullptr);
    if (s->fb)      vkDestroyFramebuffer(d, s->fb, nullptr);
    if (s->rpass)   vkDestroyRenderPass(d, s->rpass, nullptr);
    if (s->shHView)  vkDestroyImageView(d, s->shHView, nullptr);
    if (s->shHImage) vmaDestroyImage(s->allocator, s->shHImage, s->shHAlloc);
    if (s->shMView)  vkDestroyImageView(d, s->shMView, nullptr);
    if (s->shMImage) vmaDestroyImage(s->allocator, s->shMImage, s->shMAlloc);
    if (s->hdriSampler) vkDestroySampler(d, s->hdriSampler, nullptr);
    if (s->hdriView)   vkDestroyImageView(d, s->hdriView, nullptr);
    if (s->hdriImage)  vmaDestroyImage(s->allocator, s->hdriImage, s->hdriAlloc);
    if (s->recipeBuf) vmaDestroyBuffer(s->allocator, s->recipeBuf, s->recipeAlloc);
    for (int i = 0; i < 2; ++i) {   // VK-WATER-FENCE-DEFER-1: per-parity pairs
        if (s->thinBuf[i]) vmaDestroyBuffer(s->allocator, s->thinBuf[i], s->thinAlloc[i]);
        if (s->vsUbo[i])   vmaDestroyBuffer(s->allocator, s->vsUbo[i], s->vsUboAlloc[i]);
        if (s->fsUbo[i])   vmaDestroyBuffer(s->allocator, s->fsUbo[i], s->fsUboAlloc[i]);
    }
    if (s->cpool)     vkDestroyCommandPool(d, s->cpool, nullptr);
    if (s->fence)     vkDestroyFence(d, s->fence, nullptr);
    // NOTE: instance/phys/device/queue/allocator are BORROWED — never destroyed here.
}

WaterIsland* g_island = nullptr;

bool make_buffer(WaterIsland* s, VkDeviceSize size, VkBufferUsageFlags usage,
                 VkBuffer* buf, VmaAllocation* alloc) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size; bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    if (vmaCreateBuffer(s->allocator, &bci, &aci, buf, alloc, nullptr) != VK_SUCCESS) {
        vlog("vmaCreateBuffer failed."); return false;
    }
    return true;
}

// Write the 4-binding descriptor set (0=vsUbo, 1=fsUbo, 5=recipe, 6=thin).
// Called at init (after the set is allocated) and whenever a buffer is (re)allocated.
// No-op until the descriptor SET exists — ensure_thin_capacity / recipe realloc can
// run during init BEFORE vkAllocateDescriptorSets, and writing to a NULL set is a
// validation error + crash (dstSet == VK_NULL_HANDLE). The post-alloc write_descriptors
// in init_island covers the initial bind; later reallocs re-write with a valid set.
void write_descriptors(WaterIsland* s) {
    // VK-WATER-FENCE-DEFER-1: write BOTH parity sets (set[i] binds buffers[i]).
    // Callers (re)allocating any bound buffer are vkDeviceWaitIdle-guarded, so no
    // in-flight command buffer can be using either set during the update.
    for (int i = 0; i < 2; ++i) {
        if (s->set[i] == VK_NULL_HANDLE) continue;   // set not allocated yet (early init path)
        VkDescriptorBufferInfo vsInfo{ s->vsUbo[i], 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo fsInfo{ s->fsUbo[i], 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo recInfo{ s->recipeBuf, 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo thinInfo{ s->thinBuf[i], 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet w[4]{};
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = s->set[i];
        w[0].dstBinding = 0; w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &vsInfo;
        w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = s->set[i];
        w[1].dstBinding = 1; w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[1].pBufferInfo = &fsInfo;
        w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[2].dstSet = s->set[i];
        w[2].dstBinding = 5; w[2].descriptorCount = 1;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[2].pBufferInfo = &recInfo;
        w[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[3].dstSet = s->set[i];
        w[3].dstBinding = 6; w[3].descriptorCount = 1;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[3].pBufferInfo = &thinInfo;
        // Binding 2 (HDRI equirect) is written separately by ensure_hdri() once the
        // image exists — it may not yet at init time (write of a NULL view is invalid).
        VkDescriptorImageInfo hdriInfo{};
        VkWriteDescriptorSet w4{};
        if (s->hdriView != VK_NULL_HANDLE && s->hdriSampler != VK_NULL_HANDLE) {
            hdriInfo = { s->hdriSampler, s->hdriView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            w4.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w4.dstSet = s->set[i];
            w4.dstBinding = 2; w4.descriptorCount = 1;
            w4.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w4.pImageInfo = &hdriInfo;
            vkUpdateDescriptorSets(s->device, 1, &w4, 0, nullptr);
        }
        vkUpdateDescriptorSets(s->device, 4, w, 0, nullptr);
        // VK-PARITY-WATER-SHORE-1: bindings 3/4 (shore height / mask), once imported.
        if (s->shHView != VK_NULL_HANDLE && s->shMView != VK_NULL_HANDLE &&
            s->hdriSampler != VK_NULL_HANDLE) {
            VkDescriptorImageInfo shInfo[2] = {
                { s->hdriSampler, s->shHView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                { s->hdriSampler, s->shMView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
            VkWriteDescriptorSet ws[2]{};
            for (int k = 0; k < 2; ++k) {
                ws[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; ws[k].dstSet = s->set[i];
                ws[k].dstBinding = 3 + k; ws[k].descriptorCount = 1;
                ws[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                ws[k].pImageInfo = &shInfo[k];
            }
            vkUpdateDescriptorSets(s->device, 2, ws, 0, nullptr);
        }
    }
}

// Grow slot `par`'s thin SSBO to at least `bytes`; rewrite descriptors on realloc.
bool ensure_thin_capacity(WaterIsland* s, uint32_t par, VkDeviceSize bytes) {
    if (s->thinBuf[par] != VK_NULL_HANDLE && bytes <= s->thinCapBytes[par]) return true;
    if (s->thinBuf[par] != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(s->device); // simple + safe for a realloc (rare, grows once)
        vmaDestroyBuffer(s->allocator, s->thinBuf[par], s->thinAlloc[par]);
        s->thinBuf[par] = VK_NULL_HANDLE;
    }
    VkDeviceSize cap = bytes + bytes / 2 + 4096; // headroom
    if (!make_buffer(s, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &s->thinBuf[par], &s->thinAlloc[par]))
        return false;
    s->thinCapBytes[par] = cap;
    write_descriptors(s);
    return true;
}

// RUNG-6.5: one-shot transient command submit (import-time only, fence-waited).
bool submit_once(WaterIsland* s, const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = s->cpool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(s->device, &cai, &cb) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    bool ok = (vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS);
    if (ok) { record(cb); ok = (vkEndCommandBuffer(cb) == VK_SUCCESS); }
    if (ok) {
        VkFenceCreateInfo fci{}; fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence f = VK_NULL_HANDLE;
        ok = (vkCreateFence(s->device, &fci, nullptr, &f) == VK_SUCCESS);
        if (ok) {
            VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1; si.pCommandBuffers = &cb;
            ok = (vkQueueSubmit(s->queue, 1, &si, f) == VK_SUCCESS) &&
                 (vkWaitForFences(s->device, 1, &f, VK_TRUE, 5ull*1000*1000*1000) == VK_SUCCESS);
            vkDestroyFence(s->device, f, nullptr);
        }
    }
    vkFreeCommandBuffers(s->device, s->cpool, 1, &cb);
    return ok;
}

// RUNG-6.5 WATER-REFLECTION: COPY-ONCE HDRI equirect import (NOT per-frame).
// Mirrors gos_hdri.cpp's two upload paths:
//   - BC6H .ktx2 sidecar (field default, MC2_HDRI_BC6H on): single mip 0
//     (GL_TEXTURE_MAX_LEVEL=0), byte-copied via glGetCompressedTexImage ->
//     VK_FORMAT_BC6H_{U,S}FLOAT_BLOCK. GL's textureLod at LOD 4 clamps to mip 0
//     on this texture, so a 1-mip VK image is exact parity.
//   - RGBA16F tinyexr fallback: full glGenerateMipmap chain, each level
//     byte-copied via glGetTexImage(GL_HALF_FLOAT) -> VK_FORMAT_R16G16B16A16_SFLOAT.
// COST (honest): one-time ~24MB (BC6H 4K mip0) or ~85MB (RGBA16F 4K + chain) CPU
// staging + one upload submit at first armed-water frame after HDRI ready. Zero
// per-frame cost afterwards. Fail-soft: bind a 1x1 black dummy + patch the FS
// UBO's u_waterHdriLod to -1 (SH-L2 fallback) — same semantics as GL !isHdriReady().
// Not counted in g_forbidden: those counters guard per-frame scene-surface
// copies; this is a one-time content-texture import (same class as the prop
// island's copy-once albedo array).
bool ensure_hdri(WaterIsland* s, unsigned int hdriGlTex) {
    if (s->hdriAttempted && s->hdriGlTexId == hdriGlTex) return s->hdriView != VK_NULL_HANDLE;
    s->hdriAttempted = true;
    s->hdriGlTexId   = hdriGlTex;
    s->hdriRealImported = false;
    // Re-import (tex id changed, e.g. dummy -> real after late HDRI load):
    // drop the old image first. Cheap + rare; idle the device so no pending
    // command buffer still references the outgoing view.
    if (s->hdriView != VK_NULL_HANDLE || s->hdriImage != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(s->device);
        if (s->hdriView)  { vkDestroyImageView(s->device, s->hdriView, nullptr); s->hdriView = VK_NULL_HANDLE; }
        if (s->hdriImage) { vmaDestroyImage(s->allocator, s->hdriImage, s->hdriAlloc); s->hdriImage = VK_NULL_HANDLE; }
    }

    if (s->hdriSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(s->device, &sci, nullptr, &s->hdriSampler) != VK_SUCCESS) {
            vlog("hdri: vkCreateSampler failed."); return false;
        }
    }

    // Gather GL-side facts + bytes.
    VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    int w = 1, h = 1;
    uint32_t mips = 1;
    std::vector<std::vector<unsigned char>> levels;   // per-mip raw bytes
    std::vector<std::pair<int,int>> levelDims;
    bool haveReal = false;
    if (hdriGlTex != 0) {
        GLint prevTex = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
        glBindTexture(GL_TEXTURE_2D, (GLuint)hdriGlTex);
        GLint ifmt = 0, compressed = 0, w0 = 0, h0 = 0, maxLevel = 1000;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED, &compressed);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &w0);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h0);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &maxLevel);
        if (w0 > 0 && h0 > 0) {
            if (compressed &&
                (ifmt == GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT ||
                 ifmt == GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT)) {
                // BC6H path — respect the device feature (MoltenVK/Metal may lack BC).
                VkPhysicalDeviceFeatures feats{};
                vkGetPhysicalDeviceFeatures(s->phys, &feats);
                if (feats.textureCompressionBC) {
                    fmt = (ifmt == GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT)
                        ? VK_FORMAT_BC6H_SFLOAT_BLOCK : VK_FORMAT_BC6H_UFLOAT_BLOCK;
                    GLint sz = 0;
                    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0,
                        GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &sz);
                    if (sz > 0) {
                        levels.emplace_back((size_t)sz);
                        glGetCompressedTexImage(GL_TEXTURE_2D, 0, levels[0].data());
                        levelDims.emplace_back(w0, h0);
                        w = w0; h = h0; mips = 1;
                        haveReal = (glGetError() == GL_NO_ERROR);
                    }
                } else {
                    vlog("hdri: device lacks textureCompressionBC — SH-L2 fallback.");
                }
            } else if (ifmt == GL_RGBA16F) {
                // RGBA16F + full glGenerateMipmap chain.
                fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
                w = w0; h = h0;
                int lw = w0, lh = h0;
                for (int lvl = 0; lvl <= maxLevel; ++lvl) {
                    GLint tw = 0;
                    glGetTexLevelParameteriv(GL_TEXTURE_2D, lvl, GL_TEXTURE_WIDTH, &tw);
                    if (tw <= 0) break;
                    lw = (w0 >> lvl) > 0 ? (w0 >> lvl) : 1;
                    lh = (h0 >> lvl) > 0 ? (h0 >> lvl) : 1;
                    levels.emplace_back((size_t)lw * lh * 8);
                    glGetTexImage(GL_TEXTURE_2D, lvl, GL_RGBA, GL_HALF_FLOAT,
                                  levels.back().data());
                    levelDims.emplace_back(lw, lh);
                    if (lw == 1 && lh == 1) break;
                }
                mips = (uint32_t)levels.size();
                haveReal = (mips > 0) && (glGetError() == GL_NO_ERROR);
            } else {
                vlog("hdri: unexpected GL internal format 0x%x — SH-L2 fallback.", ifmt);
            }
        }
        glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    }
    if (!haveReal) {
        // 1x1 black RGBA16F dummy (keeps the statically-used binding valid;
        // shader never reads it because the island patches u_waterHdriLod to -1).
        fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
        w = 1; h = 1; mips = 1;
        levels.assign(1, std::vector<unsigned char>(8, 0));
        levelDims.assign(1, {1, 1});
    }

    // Create image + view.
    {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
        ii.extent = {(uint32_t)w, (uint32_t)h, 1};
        ii.mipLevels = mips; ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
        if (vmaCreateImage(s->allocator, &ii, &aci, &s->hdriImage, &s->hdriAlloc, nullptr) != VK_SUCCESS) {
            vlog("hdri: vmaCreateImage failed."); return false;
        }
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = s->hdriImage; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
        if (vkCreateImageView(s->device, &vi, nullptr, &s->hdriView) != VK_SUCCESS) {
            vlog("hdri: vkCreateImageView failed."); return false;
        }
    }

    // Staging buffer (one allocation, all mips at 16B-aligned offsets).
    std::vector<VkDeviceSize> offs(levels.size());
    VkDeviceSize total = 0;
    for (size_t i = 0; i < levels.size(); ++i) {
        offs[i] = total;
        total += (VkDeviceSize)((levels[i].size() + 15) & ~size_t(15));
    }
    VkBuffer staging = VK_NULL_HANDLE; VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    if (!make_buffer(s, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging, &stagingAlloc)) return false;
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, stagingAlloc, &p) != VK_SUCCESS) {
            vmaDestroyBuffer(s->allocator, staging, stagingAlloc); return false;
        }
        for (size_t i = 0; i < levels.size(); ++i)
            std::memcpy((unsigned char*)p + offs[i], levels[i].data(), levels[i].size());
        vmaUnmapMemory(s->allocator, stagingAlloc);
    }

    const bool ok = submit_once(s, [&](VkCommandBuffer cb) {
        VkImageMemoryBarrier bar{};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.srcAccessMask = 0; bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = s->hdriImage;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        for (uint32_t i = 0; i < mips; ++i) {
            VkBufferImageCopy c{};
            c.bufferOffset = offs[i];
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
            c.imageExtent = {(uint32_t)levelDims[i].first, (uint32_t)levelDims[i].second, 1};
            vkCmdCopyBufferToImage(cb, staging, s->hdriImage,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        }
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
    });
    vmaDestroyBuffer(s->allocator, staging, stagingAlloc);
    if (!ok) { vlog("hdri: upload submit failed."); return false; }

    s->hdriRealImported = haveReal;
    // VK-WATER-FENCE-DEFER-1: the descriptor update below used to be safe because
    // the previous frame's fence was waited INLINE (no cbuf using the set could be
    // pending). With the deferred retire, a prior water submit may still be in
    // flight — device-idle first (rare: at most twice per island, dummy -> real).
    vkDeviceWaitIdle(s->device);
    write_descriptors(s);   // binding 2 now valid
    vlog("hdri %s import: %dx%d fmt=%d mips=%u glTex=%u (copy-once, zero per-frame cost).",
         haveReal ? "REAL" : "DUMMY", w, h, (int)fmt, mips, hdriGlTex);
    return true;
}

// VK-PARITY-WATER-SHORE-1: copy-once import of one GL shore texture (R32F height
// or RGBA8 mask) into a sampled VK image. Mirrors ensure_hdri's pattern (GL
// read-back on the render thread's current context, one staging upload). When
// glTex is 0 (or the read fails) a 1x1 zero dummy is created instead and
// *real is false. Caller device-idles beforehand.
bool import_shore_tex(WaterIsland* s, unsigned int glTex, bool isFloat,
                      VkImage* img, VmaAllocation* alloc, VkImageView* view, bool* real) {
    const VkFormat fmt = isFloat ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    const size_t bpp = 4;
    std::vector<unsigned char> bytes;
    int w = 1, h = 1;
    *real = false;
    if (glTex != 0) {
        GLint prevTex = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
        GLint prevPack = 4; glGetIntegerv(GL_PACK_ALIGNMENT, &prevPack);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glBindTexture(GL_TEXTURE_2D, (GLuint)glTex);
        GLint w0 = 0, h0 = 0, ifmt = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &w0);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h0);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
        const GLint wantFmt = isFloat ? GL_R32F : GL_RGBA8;
        if (w0 > 0 && h0 > 0 && ifmt == wantFmt) {
            bytes.resize((size_t)w0 * (size_t)h0 * bpp);
            if (isFloat) glGetTexImage(GL_TEXTURE_2D, 0, GL_RED,  GL_FLOAT,         bytes.data());
            else         glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes.data());
            if (glGetError() == GL_NO_ERROR) { w = w0; h = h0; *real = true; }
        }
        glPixelStorei(GL_PACK_ALIGNMENT, prevPack);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    }
    if (!*real) { w = 1; h = 1; bytes.assign(bpp, 0); }

    if (*view)  { vkDestroyImageView(s->device, *view, nullptr); *view = VK_NULL_HANDLE; }
    if (*img)   { vmaDestroyImage(s->allocator, *img, *alloc); *img = VK_NULL_HANDLE; }
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
    ii.extent = {(uint32_t)w, (uint32_t)h, 1};
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(s->allocator, &ii, &aci, img, alloc, nullptr) != VK_SUCCESS) {
        vlog("shore: vmaCreateImage failed."); return false;
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = *img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(s->device, &vi, nullptr, view) != VK_SUCCESS) {
        vlog("shore: vkCreateImageView failed."); return false;
    }
    VkBuffer staging = VK_NULL_HANDLE; VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    if (!make_buffer(s, (VkDeviceSize)bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging, &stagingAlloc)) return false;
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, stagingAlloc, &p) != VK_SUCCESS) {
            vmaDestroyBuffer(s->allocator, staging, stagingAlloc); return false;
        }
        std::memcpy(p, bytes.data(), bytes.size());
        vmaUnmapMemory(s->allocator, stagingAlloc);
    }
    const VkImage dst = *img;
    const bool ok = submit_once(s, [&](VkCommandBuffer cb) {
        VkImageMemoryBarrier bar{};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.srcAccessMask = 0; bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = dst;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
        vkCmdCopyBufferToImage(cb, staging, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
    });
    vmaDestroyBuffer(s->allocator, staging, stagingAlloc);
    if (!ok) { vlog("shore: upload submit failed."); return false; }
    return true;
}

// (Re)import both shore textures when the GL-side data generation changes.
// Requires hdriSampler (created by ensure_hdri) -- shared linear/clamp sampler.
bool ensure_shore(WaterIsland* s) {
    const unsigned int gen = gos_TerrainLodChunk_WaterShoreDataGeneration();
    if (s->shoreAttempted && s->shoreGen == gen) return true;
    s->shoreAttempted = true;
    s->shoreGen = gen;
    vkDeviceWaitIdle(s->device);
    // R32_SFLOAT linear filtering is an optional format feature; without it the
    // height sidecar is treated as absent (FS degrades 2 -> 1, same as a missing
    // sidecar on GL).
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(s->phys, VK_FORMAT_R32_SFLOAT, &fp);
    const bool r32Linear = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    if (!r32Linear) vlog("shore: R32_SFLOAT lacks linear filtering -- height shore disabled (mode-1 fallback).");
    const unsigned int hTex = r32Linear ? gos_TerrainLodChunk_WaterShoreHeightTex() : 0u;
    if (!import_shore_tex(s, hTex, true,  &s->shHImage, &s->shHAlloc, &s->shHView, &s->shHReal)) return false;
    if (!import_shore_tex(s, gos_TerrainLodChunk_ShorelineMaskTex(), false,
                          &s->shMImage, &s->shMAlloc, &s->shMView, &s->shMReal)) return false;
    write_descriptors(s);
    vlog("shore import: height=%s mask=%s gen=%u (copy-once, zero per-frame cost).",
         s->shHReal ? "REAL" : "DUMMY", s->shMReal ? "REAL" : "DUMMY", gen);
    return true;
}

// One-time init: borrow the shared device, build the exportable color seam,
// gbuffer1 + depth images, recipe/thin/UBO buffers, render pass, pipeline.
bool init_island(WaterIsland* s, int w, int h) {
    s->w = w; s->h = h;
    ++g_health.attempted;

    // VK-P9a: bring up the shared engine device on-demand, DECOUPLED from present.
    // If MC2_RENDER_BACKEND=vulkan already born it, this is a no-op; otherwise it
    // inits the SAME shared device (one volkLoadDevice, interop exts, NO swapchain)
    // so the water region runs on the GL-present path (SSAO model, shared device).
    // Fail-soft -> GL water draw.
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

    // ---- RUNG 6 (tier1): borrow the SHARED scene GBuffer (color0+normal+depth)
    // the GL scene renders into. Fail-soft: not ready => GL water draw.
    // VK-RUNG8-ITEM5: fire the one-shot MC2_VK_WATER_DIRECT_CHAIN deprecation note if
    // the retired gate was explicitly set (result ignored — always direct-chain-native).
    (void)waterDirectChainRequested();

    s->tier1 = waterTier1Requested();
    if (!s->tier1) {
        setFallback("gate_off");
        return false;
    }
    s->sColorRes  = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
    s->sNormalRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_NORMAL);
    s->sDepthRes  = mc2SharedSceneDepthGetResource();
    // VK-RUNG8-ITEM5: direct-chain-native only. The shared GBuffer is VK-only and
    // left Vulkan-owned by the upstream first-owner; only the VkImageResources are
    // required (no GL interop view, no GL<->VK semaphore handoff).
    if (!s->sColorRes || !s->sNormalRes || !s->sDepthRes) {
        setFallback("shared_gbuffer_not_ready");
        return false;
    }
    s->useInterop = false;
    g_health.interopActive = 0;
    g_health.vk2glSemActive = 0;
    vlog("TIER-1 shared scene GBuffer seam ACTIVE - in-place water co-write "
         "(color0+normal+depth, no composite, direct-chain native, no interop).");
    // ---- UBOs (per parity slot, VK-WATER-FENCE-DEFER-1). ----
    for (int i = 0; i < 2; ++i) {
        if (!make_buffer(s, sizeof(WaterVsParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &s->vsUbo[i], &s->vsUboAlloc[i])) return false;
        if (!make_buffer(s, sizeof(WaterFsParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &s->fsUbo[i], &s->fsUboAlloc[i])) return false;
    }

    // ---- Recipe SSBO (mission-static, copied once at first run). Sized to the
    // current recipe count; if empty now, deferred to run() first frame. ----
    {
        const uint32_t rc = WaterStream::GetRecipeCount();
        // 64B/recipe (WaterRecipe). Allocate at least 1 to keep the descriptor valid.
        s->recipeBytes = (VkDeviceSize)(rc ? rc : 1) * 64;
        if (!make_buffer(s, s->recipeBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &s->recipeBuf, &s->recipeAlloc)) return false;
    }
    // Thin SSBOs (per parity slot) — start with a small ring; grows in run().
    if (!ensure_thin_capacity(s, 0, 64 * 1024)) return false; // ~1365 records to start
    if (!ensure_thin_capacity(s, 1, 64 * 1024)) return false;

    // ---- Render pass: color0 (shared, LOAD scene color, alpha-blend, STORE) +
    // color1 (gbuffer1, CLEAR, STORE local) + depth (LOAD imported, GEQUAL, write). ----
    {
        VkAttachmentDescription att[3]{};
        // RUNG 6 tier1: LOAD-preserve pass over the shared scene GBuffer. Water
        // runs after terrain/props/mechs wrote scene depth and must GEQUAL-test
        // against it, exactly like the GL water draw does.
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

        // Incoming + outgoing external deps — interop cross-frame WAW serialize (SSAO
        // P1.5 pattern). WAW FIX: the shared color image is written every frame (CLEAR +
        // water draw + STORE) across two parity slots; without an INCOMING external dep
        // sync-val flags a WRITE_AFTER_WRITE between frame N's storeOp and frame N+1's
        // color write (the GL semaphore serializes it functionally, but sync-val can't
        // see across the GL boundary). deps[0]: order THIS frame's color+depth writes
        // AFTER any prior color/depth write on the shared/local images. srcAccessMask
        // includes COLOR_ATTACHMENT_WRITE so the prior store is a visible hazard source.
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dependencyFlags = 0;
        // deps[1]: OUTGOING — the color store completes before the cross-API signal
        // hands the image back to GL (dst = BOTTOM_OF_PIPE, no dstAccess).
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
        if (vkCreateRenderPass(s->device, &rpci, nullptr, &s->rpass) != VK_SUCCESS) {
            vlog("vkCreateRenderPass failed."); return false;
        }
        VkImageView fbViews[3] = {
            s->sColorRes->view,   // shared scene color0 (MRT0)
            s->sNormalRes->view,  // shared scene normal (MRT1)
            s->sDepthRes->view    // shared scene depth
        };
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->rpass; fci.attachmentCount = 3; fci.pAttachments = fbViews;
        fci.width = (uint32_t)w; fci.height = (uint32_t)h; fci.layers = 1;
        if (vkCreateFramebuffer(s->device, &fci, nullptr, &s->fb) != VK_SUCCESS) {
            vlog("vkCreateFramebuffer failed."); return false;
        }
    }

    // ---- Descriptor layout/pool/set (2 UBO + 2 SSBO), both stages. ----
    {
        VkDescriptorSetLayoutBinding b[7]{};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[2].binding = 5; b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[3].binding = 6; b[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[3].descriptorCount = 1; b[3].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        // RUNG-6.5: HDRI equirect (copy-once import), FS-only.
        b[4].binding = 2; b[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[4].descriptorCount = 1; b[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // VK-PARITY-WATER-SHORE-1: shore height (3) + shore mask (4), FS-only.
        for (int k = 0; k < 2; ++k) {
            b[5 + k].binding = 3 + k; b[5 + k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[5 + k].descriptorCount = 1; b[5 + k].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 7; lci.pBindings = b;
        if (vkCreateDescriptorSetLayout(s->device, &lci, nullptr, &s->dsl) != VK_SUCCESS) {
            vlog("vkCreateDescriptorSetLayout failed."); return false;
        }
        // VK-WATER-FENCE-DEFER-1: TWO sets (one per parity slot), so pool sizes double.
        VkDescriptorPoolSize sizes[3]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sizes[0].descriptorCount = 4;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sizes[1].descriptorCount = 4;
        sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[2].descriptorCount = 6;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 2; pci.poolSizeCount = 3; pci.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(s->device, &pci, nullptr, &s->pool) != VK_SUCCESS) {
            vlog("vkCreateDescriptorPool failed."); return false;
        }
        VkDescriptorSetLayout layouts[2] = { s->dsl, s->dsl };
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = s->pool; dsai.descriptorSetCount = 2; dsai.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(s->device, &dsai, s->set) != VK_SUCCESS) {
            vlog("vkAllocateDescriptorSets failed."); return false;
        }
        write_descriptors(s);
    }

    // ---- Pipeline (procedural VS, no vertex input; GEQUAL depth on+write on;
    // cull none; MRT alpha-blend SRC_ALPHA/ONE_MINUS_SRC_ALPHA on both color atts). ----
    {
        s->vert = load_spv(s->device, spv_dir() + "water_geom.vert.spv");
        s->frag = load_spv(s->device, spv_dir() + "water_geom.frag.spv");
        if (s->vert == VK_NULL_HANDLE || s->frag == VK_NULL_HANDLE) { setFallback("spv_load_failed"); return false; }

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; plci.pSetLayouts = &s->dsl;
        if (vkCreatePipelineLayout(s->device, &plci, nullptr, &s->playout) != VK_SUCCESS) {
            vlog("vkCreatePipelineLayout failed."); return false;
        }

        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO; // count=0 (procedural)
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Depth: reverse-Z GEQUAL, test on, WRITE ON (PipelineRegistry WaterArmed row).
        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

        // RUNG 6 tier1 blend: in-place over the LOADed scene color. The FS emits
        // premultiplied (col*a, a), producing the same RGB result as GL's
        // straight-alpha SRC_ALPHA/ONE_MINUS_SRC_ALPHA water pass.
        VkPipelineColorBlendAttachmentState cba[2]{};
        cba[0].blendEnable = VK_TRUE;
        cba[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba[0].colorBlendOp = VK_BLEND_OP_ADD;
        cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba[0].alphaBlendOp = VK_BLEND_OP_ADD;
        cba[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        // Attachment 1 = shared scene normal. GL's water GBuffer1 output has src.a
        // 0, so under GL's SRC_ALPHA blend it contributes nothing; writeMask=0 is
        // the exact no-op without requiring independentBlend.
        cba[1].blendEnable = VK_FALSE;
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
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = s->vert; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = s->frag; stages[1].pName = "main";
        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2; gp.pStages = stages;
        gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds; gp.pColorBlendState = &cb;
        gp.layout = s->playout; gp.renderPass = s->rpass; gp.subpass = 0;
        if (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, &s->pipe) != VK_SUCCESS) {
            vlog("vkCreateGraphicsPipelines failed."); return false;
        }
    }

    // ---- Command pool + 2 command buffers (per parity) + fence. ----
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
        VkFenceCreateInfo fnci{}; fnci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(s->device, &fnci, nullptr, &s->fence) != VK_SUCCESS) {
            vlog("vkCreateFence failed."); return false;
        }
    }

    s->inited = true;
    setFallback("");
    vlog("island initialized %dx%d (borrowed shared device; shared scene GBuffer; "
         "procedural water VS; GEQUAL reverse-Z depth-write-on; "
         "MRT alpha-blend).", w, h);
    return true;
}

} // namespace

// ===========================================================================
// PUBLIC ENTRY POINTS (consumed under #ifdef MC2_VULKAN_ISLAND from
// gameos_graphics.cpp:renderWaterFastPath + debug_state_dump.cpp).
// ===========================================================================

// Health accessor for debug_state_dump.cpp.
extern "C" void mc2_render_backend_water_region_health(
    int* vulkanAvailable, int* buildEnabled, const char** deviceName,
    unsigned long* validationErrors, unsigned long* attempted, unsigned long* usedVulkan,
    const char** fallbackReason, unsigned long* vkDraws,
    int* interopActive, int* vk2glSemActive, unsigned long* usedInterop,
    unsigned long* thinRecords, unsigned long* recipeCount,
    unsigned long* forbiddenCopyBufToImg, unsigned long* forbiddenCopyImgToBuf,
    unsigned long* forbiddenGetTexImage, unsigned long* forbiddenTexSubImage,
    unsigned long* depthInGetTexImage, unsigned long* depthInCopyBufToImg,
    double* copyColorInUs, double* copyDepthInUs, double* drawUs, double* colorOutUs, double* totalUs) {
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
    if (thinRecords)      *thinRecords      = g_health.thinRecords;
    if (recipeCount)      *recipeCount      = g_health.recipeCount;
    if (forbiddenCopyBufToImg) *forbiddenCopyBufToImg = g_forbidden.vkCmdCopyBufferToImage;
    if (forbiddenCopyImgToBuf) *forbiddenCopyImgToBuf = g_forbidden.vkCmdCopyImageToBuffer;
    if (forbiddenGetTexImage)  *forbiddenGetTexImage  = g_forbidden.glGetTexImage;
    if (forbiddenTexSubImage)  *forbiddenTexSubImage  = g_forbidden.glTexSubImage2D;
    if (depthInGetTexImage)    *depthInGetTexImage    = g_depthInGetTexImage;
    if (depthInCopyBufToImg)   *depthInCopyBufToImg   = g_depthInCopyBufferToImage;
    if (copyColorInUs)    *copyColorInUs    = g_health.copyColorInUs;
    if (copyDepthInUs)    *copyDepthInUs    = g_health.copyDepthInUs;
    if (drawUs)           *drawUs           = g_health.drawUs;
    if (colorOutUs)       *colorOutUs       = g_health.colorOutUs;
    if (totalUs)          *totalUs          = g_health.totalUs;
}

// True when the Tier-1 gate requests the VK water region (the caller then SKIPS the GL
// MDI draw and calls mc2_vk_water_island_run instead — mutually exclusive).
extern "C" int mc2_vk_water_island_enabled() {
    return waterTier1Requested() ? 1 : 0;
}

// Per-frame entry. Inputs (all from the GL side, at water-render time):
//   sceneColorTex/sceneDepthTex — GL scene MainColor + depth textures (w x h).
//   mvp16 — the 16 raw floats GL uploads (glUniformMatrix4fv GL_FALSE) for u_worldToClipGL.
//   thinRecords/thinCount — the CPU-staged per-frame thin-record array (48B each).
//   recipes/recipeCount — mission-static recipe array (64B each; uploaded once).
//   vsP/fsP — the packed uniform params (LOCKSTEP with water_geom.{vert,frag}).
// Returns 1 if the VK water pass rendered+composited this frame; 0 = fell back
// (caller must then run the GL water draw, i.e. treat 0 as "not handled").
extern "C" int mc2_vk_water_island_run(
    unsigned int sceneColorTex, unsigned int sceneDepthTex, int w, int h,
    const void* thinRecords, unsigned int thinCount,
    const void* recipes, unsigned int recipeCount,
    const void* vsParams, const void* fsParams,
    unsigned int hdriGlTex) {
    if (!waterTier1Requested()) return 0;
    if (thinCount == 0 || recipeCount == 0 || w <= 0 || h <= 0) return 0;
    ZoneScopedN("VKIsl.Water.Run");   // VK-ISLAND-TRACY-1 per-frame island total (CPU)
    (void)sceneColorTex;
    (void)sceneDepthTex;

    using clk = std::chrono::steady_clock;
    auto usSince = [](clk::time_point t0){ return std::chrono::duration<double, std::micro>(clk::now() - t0).count(); };
    const clk::time_point tTotal0 = clk::now();
    const bool perf = waterPerfGateRequested();
    // Fine-grained sub-step timers (only meaningful when `perf`; cheap to always fill).
    double us_stage = 0.0;      // recipe/thin/UBO memcpy staging (steps 1-3)
    double us_depth_readback=0.0; // glGetTexImage(DEPTH) GL->CPU stall (step 5)
    double us_depth_stage = 0.0;  // memcpy depth scratch -> VK staging (step 5)
    double us_record = 0.0;     // command-buffer record (step 7)
    double us_submit = 0.0;     // vkQueueSubmit (step 8)
    double us_sem_wait = 0.0;   // glInteropWaitFromVk (step 8)
    double us_fence_wait = 0.0; // vkWaitForFences inline stall (step 8)
    double us_composite = 0.0;  // GL composite draw (step 9)

    // Lazy init / rebuild on size change.
    if (g_island && (g_island->w != w || g_island->h != h)) {
        destroy_island(g_island); delete g_island; g_island = nullptr;
    }
    if (!g_island) {
        g_island = new WaterIsland();
        if (!init_island(g_island, w, h)) {
            vlog("init failed (%s) — GL fallback.", g_health.fallbackReason);
            destroy_island(g_island); delete g_island; g_island = nullptr;
            return 0;
        }
    }
    WaterIsland* s = g_island;
    // VK-OWNERSHIP-GAP-FIX (item-4 correctness): direct-chain sets useInterop=false
    // (init:640) but the old guard '|| !s->useInterop' bailed here, so under the strict
    // umbrella water returned 0 and GL silently drew water into the VK-owned GBuffer
    // (used_vulkan=0). Enter the native path when direct-chain OR hybrid interop is armed.
    if (s->disabled || !s->inited) return 0;  // VK-RUNG8-ITEM5: direct-chain-native only

    // RUNG-6.5: COPY-ONCE HDRI import (or 1x1 dummy) — the FS statically uses
    // binding 2, so it must be valid before the first draw. Re-runs only if the
    // GL HDRI texture id changes (e.g. late HDRI load: first frames import the
    // dummy at glTex=0, then the real texture once ready). Safe to update the
    // descriptor here: ensure_hdri device-idles before its descriptor update
    // (VK-WATER-FENCE-DEFER-1 — the old "previous frame's fence was waited
    // inline" guarantee is gone under the deferred retire).
    if (!ensure_hdri(s, hdriGlTex)) {
        setFallback("hdri_binding_setup_failed"); s->disabled = true; return 0;
    }

    // VK-PARITY-WATER-SHORE-1: shore height/mask copy-once import (binding 3/4).
    if (!ensure_shore(s)) {
        setFallback("shore_binding_setup_failed"); s->disabled = true; return 0;
    }

    // Snapshot forbidden COLOR counters (prove none fire on the interop path).
    const unsigned long f0a = g_forbidden.vkCmdCopyBufferToImage;
    const unsigned long f0b = g_forbidden.vkCmdCopyImageToBuffer;
    const unsigned long f0c = g_forbidden.glGetTexImage;
    const unsigned long f0d = g_forbidden.glTexSubImage2D;

    // VK-RUNG8-ITEM5: direct-chain-native only (the trailing non-tier1 path is dead).
    if (!s->tier1) return 0;

    // VK-WATER-FENCE-DEFER-1 (cluster-A pilot): begin the shared-slot frame BEFORE
    // staging. The slot-fence wait inside beginFrame IS the deferred retire of the
    // submit that last used this parity slot (normally an earlier island / last
    // frame — near-free by now), and it protects everything indexed [par] below:
    // staging buffers, descriptor set, command buffer. The inline post-submit wait
    // this replaces is kept only under the MC2_VK_WATER_FENCE_DEFER=0 killswitch.
    const clk::time_point tFence0 = clk::now();
    VkFrameSync fsC{};
    { ZoneScopedN("VKIsl.Water.FenceWait");   // VK-ISLAND-TRACY-1: deferred retire of the parity slot
      fsC = vkImageResourceBeginFrame(s->sColorRes); }
    us_fence_wait = usSince(tFence0);
    const uint32_t par = fsC.parity & 1u;

    const clk::time_point tStage0 = clk::now();
    {
    ZoneScopedN("VKIsl.Water.Feed");   // VK-ISLAND-TRACY-1: recipe/thin/UBO staging (steps 1-3)
    // ---- 1) Upload recipe SSBO once (mission-static). ----
    if (!s->recipeUploaded) {
        const VkDeviceSize need = (VkDeviceSize)recipeCount * 64;
        if (need > s->recipeBytes) {
            vkDeviceWaitIdle(s->device);
            vmaDestroyBuffer(s->allocator, s->recipeBuf, s->recipeAlloc);
            if (!make_buffer(s, need, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &s->recipeBuf, &s->recipeAlloc)) { s->disabled = true; return 0; }
            s->recipeBytes = need;
            write_descriptors(s);
        }
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->recipeAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, recipes, (size_t)recipeCount * 64);
            vmaUnmapMemory(s->allocator, s->recipeAlloc);
        }
        s->recipeUploaded = true;
        g_health.recipeCount = recipeCount;
    }

    // ---- 2) Upload the per-frame thin records into slot `par` (CPU-staged). ----
    const VkDeviceSize thinBytes = (VkDeviceSize)thinCount * 48;
    if (!ensure_thin_capacity(s, par, thinBytes)) { s->disabled = true; return 0; }
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->thinAlloc[par], &p) == VK_SUCCESS) {
            std::memcpy(p, thinRecords, (size_t)thinBytes);
            vmaUnmapMemory(s->allocator, s->thinAlloc[par]);
        }
    }
    g_health.thinRecords = thinCount;

    // ---- 3) Upload slot `par`'s UBOs. ----
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->vsUboAlloc[par], &p) == VK_SUCCESS) {
            WaterVsParams vsLocal;
            std::memcpy(&vsLocal, vsParams, sizeof(WaterVsParams));
            // Shore mode 3: flat water plane in the VS only when the hi-res height sidecar
            // is really imported (mirrors GL u_shoreFlat = mode==3 && shTex != 0).
            vsLocal.p3[1] = (((int)(((const WaterFsParams*)fsParams)->f4[0] + 0.5f) == 3) && s->shHReal) ? 1.0f : 0.0f;
            std::memcpy(p, &vsLocal, sizeof(WaterVsParams)); vmaUnmapMemory(s->allocator, s->vsUboAlloc[par]);
        }
        if (vmaMapMemory(s->allocator, s->fsUboAlloc[par], &p) == VK_SUCCESS) {
            WaterFsParams fsLocal;
            std::memcpy(&fsLocal, fsParams, sizeof(WaterFsParams));
            // RUNG-6.5 fail-soft: if the real HDRI bytes aren't imported (dummy
            // bound), force u_waterHdriLod (f2.w, offset 60) to -1 so the FS
            // takes the SH-L2 fallback — same semantics as GL !isHdriReady().
            if (!s->hdriRealImported) fsLocal.f2[3] = -1.0f;
            // VK-PARITY-WATER-SHORE-1: GL sets u_hasShoreHeight = (mode==2 && tex)
            // and u_hasShoreMask = (mode>=1 && tex); the island owns the textures.
            {
                const int shMode = (int)(fsLocal.f4[0] + 0.5f);
                fsLocal.f4[1] = (shMode >= 2 && s->shHReal) ? 1.0f : 0.0f;
                fsLocal.f4[2] = (shMode >= 1 && s->shMReal) ? 1.0f : 0.0f;
            }
            std::memcpy(p, &fsLocal, sizeof(WaterFsParams));
            vmaUnmapMemory(s->allocator, s->fsUboAlloc[par]);
        }
    }
    }   // VK-ISLAND-TRACY-1: close VKIsl.Water.Feed

    // ============================ RUNG 6 TIER-1 PATH ============================
    // In-place co-write into the SHARED scene GBuffer. No color/depth import, no
    // composite. Frame-sync protocol COPIED from vulkan_terrain_island.cpp:2495-2603,
    // with the beginFrame hoisted above staging (VK-WATER-FENCE-DEFER-1).
    {
        us_stage = usSince(tStage0);
        g_health.copyColorInUs = 0.0;
        g_health.copyDepthInUs = 0.0;

        VkCommandBuffer cb = s->cbuf[par];

        const clk::time_point tRender0 = clk::now();
        {
        ZoneScopedN("VKIsl.Water.Record");   // VK-ISLAND-TRACY-1: command-buffer record
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

        // All attachments LOAD -> clear values ignored (array must still cover them).
        VkClearValue clears[3]{};
        VkRenderPassBeginInfo rbi{};
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = s->rpass; rbi.framebuffer = s->fb;
        rbi.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
        rbi.clearValueCount = 3; rbi.pClearValues = clears;
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout, 0, 1, &s->set[par], 0, nullptr);
        vkCmdDraw(cb, thinCount * 6u, 1, 0, 0);
        vkCmdEndRenderPass(cb);
        if (vkEndCommandBuffer(cb) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }
        }   // VK-ISLAND-TRACY-1: close VKIsl.Water.Record
        us_record = usSince(tRender0);

        // VK-RUNG8-ITEM5: direct-chain-native submit — waits/signals NO GL<->VK
        // semaphores (shared GBuffer stays Vulkan-owned); color fence orders CPU.
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 0; si.pWaitSemaphores = nullptr; si.pWaitDstStageMask = nullptr;
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 0; si.pSignalSemaphores = nullptr;
        const clk::time_point tSubmit0 = clk::now();
        { ZoneScopedN("VKIsl.Water.Submit");   // VK-ISLAND-TRACY-1: vkQueueSubmit
          if (vkQueueSubmit(s->queue, 1, &si, fsC.fence) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; } }
        us_submit = usSince(tSubmit0);

        us_sem_wait = 0.0;

        // VK-WATER-FENCE-DEFER-1: the inline retire wait is GONE — the submit's
        // fence (the shared parity-slot fence) is waited at the slot's next
        // vkImageResourceBeginFrame, i.e. the deferred retire that beginFrame at
        // the top of this function already timed into us_fence_wait. Everything
        // this frame's submit uses is per-parity ([par]) or static, so nothing is
        // rewritten before that wait. GPU-side ordering against the downstream
        // post/composite passes never depended on this CPU wait (their command
        // buffers' entry barriers cover earlier submission-order work).
        // Killswitch MC2_VK_WATER_FENCE_DEFER=0 restores the inline wait.
        if (!waterFenceDeferEnabled()) {
            ZoneScopedN("VKIsl.Water.InlineFenceWait");   // VK-ISLAND-TRACY-1: killswitch-only inline retire
            const clk::time_point tInline0 = clk::now();
            if (vkWaitForFences(s->device, 1, &fsC.fence, VK_TRUE, 5ull*1000*1000*1000) != VK_SUCCESS) {
                setFallback("fence_timeout"); s->disabled = true; return 0;
            }
            us_fence_wait = usSince(tInline0);
        }
        g_health.drawUs = usSince(tRender0);
        g_health.colorOutUs = 0.0;   // no composite in tier1

        // HARD assert: no COLOR-side readback/copy fired (tier1 must be zero-copy on
        // color AND depth — there is no expected depth-in either).
        const bool tier1ReadbackFired =
            (g_forbidden.vkCmdCopyBufferToImage != f0a) || (g_forbidden.vkCmdCopyImageToBuffer != f0b) ||
            (g_forbidden.glGetTexImage != f0c) || (g_forbidden.glTexSubImage2D != f0d);
        if (tier1ReadbackFired) {
            vlog("FATAL: a COLOR-side readback/copy fired on the tier1 co-write path — "
                 "zero-copy invariant violated; failing frame + disabling.");
            setFallback("interop_did_readback"); s->disabled = true; return 0;
        }

        g_health.vkDraws = 1;
        ++g_health.usedVulkan;
        // VK-RUNG8-ITEM5: never interop (direct-chain-native only).
        g_health.totalUs = usSince(tTotal0);
        static bool s_tier1FirstOk = false;
        if (!s_tier1FirstOk) {
            vlog("first TIER-1 in-place water GBuffer co-write: thin=%u recipes=%u "
                 "(color0 premult-over blend, normal untouched (writemask 0, GL a=0 parity), "
                 "shared depth GEQUAL test+write; no composite, no depth readback).",
                 thinCount, recipeCount);
            s_tier1FirstOk = true;
        }
        if (perf) {
            std::fprintf(stderr,
                "[VK_WATER_PERF] tier1=1 thin=%u recipes=%u stage_us=%.1f record_us=%.1f "
                "submit_us=%.1f sem_wait_us=%.1f fence_wait_us=%.1f island_total_us=%.1f\n",
                thinCount, recipeCount, us_stage, us_record, us_submit, us_sem_wait,
                us_fence_wait, g_health.totalUs);
            std::fflush(stderr);
        }
        return 1;
    }
    return 0;
}

// Teardown (called from gosPostProcess::destroy or renderer shutdown).
extern "C" void mc2_vk_water_island_destroy() {
    vlog("[VK_WATER_TIER1] vulkan_available=%d used_vulkan=%lu used_interop=%lu interop_active=%d "
         "vk2gl_sem=%d validation_errors=%lu thin_records=%lu recipe_count=%lu fallback_reason=\"%s\" "
         "vk_draws=%lu forbidden_copyBufToImg=%lu forbidden_copyImgToBuf=%lu forbidden_getTexImage=%lu "
         "forbidden_texSubImage=%lu depthin_getTexImage=%lu depthin_copyBufToImg=%lu "
         "copy_color_in_us=%.1f copy_depth_in_us=%.1f draw_us=%.1f color_out_us=%.1f total_us=%.1f",
         g_health.vulkanAvailable, g_health.usedVulkan, g_health.usedInterop, g_health.interopActive,
         g_health.vk2glSemActive, g_health.validationErrors, g_health.thinRecords, g_health.recipeCount,
         g_health.fallbackReason, g_health.vkDraws,
         g_forbidden.vkCmdCopyBufferToImage, g_forbidden.vkCmdCopyImageToBuffer,
         g_forbidden.glGetTexImage, g_forbidden.glTexSubImage2D,
         g_depthInGetTexImage, g_depthInCopyBufferToImage,
         g_health.copyColorInUs, g_health.copyDepthInUs, g_health.drawUs, g_health.colorOutUs, g_health.totalUs);
    if (!g_island) return;
    destroy_island(g_island);
    delete g_island; g_island = nullptr;
}

#endif // MC2_VULKAN_ISLAND
