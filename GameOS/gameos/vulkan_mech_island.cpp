// vulkan_mech_island.cpp — VK-P14 MECH TIER-1: the FIRST VK-native SKINNED mech region.
// =============================================================================
// Co-writes the GL mech batcher's visible instances IN-PLACE into the SHARED scene
// GBuffer (MRT0 color + MRT1 normal + shared D32S8 depth), VK-skinned. Structure =
// vulkan_color_prop_island.cpp's TIER-1 co-write path (shared-GBuffer LOAD-preserve
// renderpass, GEQUAL reverse-Z, no composite), with the static-prop 40B vertex +
// modelMatrix instance swapped for:
//   - the mech 48B vertex stream (GpuMechVertex: pos/normal/uv/boneIdx u8[4]/
//     boneWeights u8[4]/tangentOct/aRGBLight), copied ONCE per map;
//   - a per-frame BONE PALETTE SSBO (GpuMechBone 64B) memcpy'd each frame;
//   - a per-frame INSTANCE SSBO (GpuMechInstance 64B);
//   - a VS that does LINEAR-BLEND SKINNING over 4 bone weights (mech.vert:156-180),
//     then the Stuff->MC2 axis swap worldMC2 = vec3(-x,z,y) (mech.vert:178).
//
// SUB-SLICE 1 (this file): skinning + FLAT color. This is the P-conjugation /
// axis-swap silhouette proof — verify the mech is not gliding/mirrored/in-sky
// BEFORE adding material + calc_light lighting. Those are gated follow-ons:
//   MC2_VK_MECH_MATERIAL  — copy-once single 2D albedo (slot-0 paint scheme) sampled
//                           per run (single-material probe; per-actor VK-descriptor
//                           cache DEFERRED — see the run() material note).
//   MC2_VK_MECH_LIGHTING  — port calc_light + feed the LightsData UBO (DEFERRED;
//                           needs the whole ObjectLights SSBO mirrored to VK).
//
// Gate MC2_VK_MECH_TIER1 (requires MC2_VK_SHARED_SCENE_DEPTH), default-OFF -> GL
// mech color flush byte-identical. The feed (instances + bone palette + shared
// VBO/IBO + per-run slot-0 GL texture) comes from gos_mech_batcher.cpp via the
// Mc2Mech*Feed accessors. NO fenced GL buffer crosses.
//
// The whole TU compiles to nothing unless MC2_VULKAN_ISLAND is defined.
// =============================================================================

#ifdef MC2_VULKAN_ISLAND

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>
#include "Gates/gate.h"
#include "vk_render_pass_timer.h"
#include "Gates/gate_cold.h"   // str<>()
#include "Gates/render_options.h"   // options.cfg SunAxisFix

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include "vk_image_resource.h"
#include "vk_island_util.h"
#include "vulkan_context.h"
#include "vulkan_mech_island.h"
#include "texture_decode.h"              // gos_GetTextureTexels: material texels without a GL readback
#include "../../RenderCore/gpu_abi/GpuMechFrameUBO.h"  // single-source per-frame UBO
#include "vk_shared_gbuffer_gate.h"      // VK-P14 GATE-CONSOLIDATION-1 umbrella resolver
#include "vk_shared_light_feed.h"        // VK-LIGHTFEED-DEDUP-1: shared per-frame LightsData SSBO
#include "vk_gpu_timestamps.h"           // VK-DRAW-PERF-1: MC2_VK_GPU_TIMESTAMPS GPU segment timers
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"   // shared scene color/normal images
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"   // shared scene depth image

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <vector>
#include <string>
#include <chrono>
#include <tracy/Tracy.hpp>   // VK-FLIP-FIELD-REPORT-1: coarse per-region frame zones

namespace {

struct MechIslandHealth {
    int   vulkanAvailable = 0;
    int   buildEnabled    = 1;
    char  deviceName[256] = {0};
    unsigned long attempted   = 0;
    unsigned long usedVulkan  = 0;
    char  fallbackReason[256] = {0};
    unsigned long vkDraws     = 0;
    int   interopActive  = 0;
    int   vk2glSemActive = 0;
    unsigned long instRecords = 0;
    unsigned long boneRecords = 0;
    unsigned long runs        = 0;
    unsigned long geomVerts   = 0;
    double stageUs = 0.0, drawUs = 0.0, totalUs = 0.0;
};
MechIslandHealth g_health;

struct InteropForbidden {
    unsigned long glGetTexImage = 0;   // color CPU pull (must be 0)
    unsigned long glReadPixels  = 0;   // color CPU pull (must be 0)
};
InteropForbidden g_forbidden;

void setFallback(const char* reason) {
    std::strncpy(g_health.fallbackReason, reason ? reason : "",
                 sizeof(g_health.fallbackReason) - 1);
    g_health.fallbackReason[sizeof(g_health.fallbackReason) - 1] = '\0';
}

void vlog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vk_island_util::vlogv("[VK_MECH_ISLAND] ", fmt, ap);
    va_end(ap);
}

bool mechGateRequested() {
    static int state = -1;
    if (state == -1) {
        // Umbrella member: MC2_VK_SHARED_GBUFFER=1 arms it.
        state = mc2vkgate::umbrellaOn() ? 1 : 0;   // require shared depth
    }
    return state == 1;
}

bool mechFenceDeferEnabled() {
    static int state = -1;
    if (state == -1)
        state = mc2::gate::get<"MC2_VK_MECH_FENCE_DEFER">() ? 1 : 0;
    return state == 1;
}

bool mechPerfGateRequested() {
    static int state = -1;
    if (state == -1) {
        state = mc2::gate::get<"MC2_VK_MECH_PERF">();
    }
    return state == 1;
}

// The mech island always renders direct-chain (terrain leaves the shared GBuffer
// Vulkan-owned); there is no GL<->VK semaphore-handoff co-write path.

// MC2_VK_MECH_MATERIAL (follow-on): copy-once single 2D albedo for the covered
// stock mech type. DEFERRED general case = per-frame slot0 handle -> VK-descriptor
// cache. Default-OFF -> flat color.
bool mechMaterialRequested() {
    // Umbrella member.
    static int state = -1;
    if (state == -1) state = mc2vkgate::umbrellaOn() ? 1 : 0;
    return state == 1;
}

// MC2_VK_MECH_LIGHTING (follow-on): per-vertex calc_light Lambert (matches the GL
// default mech path). Default-OFF -> flat-white lit (v_litColor = 1). Independent of
// MC2_VK_MECH_MATERIAL, but only visible when material is on (albedo * litColor).
bool mechLightingRequested() {
    // Umbrella member.
    static int state = -1;
    if (state == -1) state = mc2vkgate::umbrellaOn() ? 1 : 0;
    return state == 1;
}

// VK-RUNG8 item-9: the native
// CPU geometry + LightsData feed is unconditional under the VK mech island.
// The env name MC2_VK_MECH_FEED_NATIVE is still parsed + noted as deprecated in gos_mech_batcher.cpp; there is no per-frame gate to query here.


std::string spv_dir() { return vk_island_util::spvDir(); }

VkShaderModule load_spv(VkDevice device, std::string_view path) {
    return vk_island_util::loadSpv(device, path, vlog);
}

// The per-frame std140 UBO (MechFrameUBO) is single-sourced in
// RenderCore/gpu_abi/GpuMechFrameUBO.h; its generated block feeds both mech shaders.

// VK-PARITY-MECH-1: object light-pool SSBO cap (GpuTerrainLight 48B; GL caps 256).
const uint32_t kMaxPoolLights = 256;

const VkFormat kColorFmt = VK_FORMAT_R16G16B16A16_SFLOAT;

struct MechIsland : mc2vk::IslandState {
    bool disabled = false;
    bool inited   = false;

    int w = 0, h = 0;

    // Borrowed from the shared engine context (NOT owned).
    VkInstance       instance  = VK_NULL_HANDLE;
    VkPhysicalDevice phys      = VK_NULL_HANDLE;
    VkDevice         device    = VK_NULL_HANDLE;
    uint32_t         gfxFamily = UINT32_MAX;
    VkQueue          queue     = VK_NULL_HANDLE;
    VmaAllocator     allocator = VK_NULL_HANDLE;

    // Shared-GBuffer co-write objects (over the SAME shared color/normal/depth
    // images the GL scene renders terrain into). Images are BORROWED.
    struct VkImageResource* colorRes  = nullptr;
    struct VkImageResource* normalRes = nullptr;
    struct VkImageResource* depthRes  = nullptr;
    // VK-RUNG8-ITEM5: shared scene is VK-only (no GL interop view). Mech is
    // direct-chain-native only; the former colorView/normalView/depthView are gone.

    // Copy-once mech geometry (VK-owned copies of the shared mech VBO/IBO).
    uint32_t vertexStride = 48;
    bool     geomUploaded = false;
    uint32_t geomGen = 0;   // generation of the feed the buffers were filled from

    // Per-frame data (UBO, instance + bone snapshots, LightsData, night pool) lives in the engine frame ring; every scheme set binds it with dynamic
    // offsets (bindings 0, 1, 2, 4, 5). ringBound[par][binding] = the ring buffer that binding of every scheme set of that parity points at; a frame
    // that allocated from another buffer (the ring grew) or a set created since rewrites it.
    VkBuffer ringBound[2][8] = {};

    // VK-MECH-MATERIAL-LIGHTING-1: per-frame LightsData span (binding 4) for calc_light.
    bool     wantLighting  = false;   // MC2_VK_MECH_LIGHTING gate
    bool     lightingReady = false;   // LightsData copied this frame (>=1 ObjectLights)

    // VK-PARITY-MECH-1: per-frame object light-pool SSBO (binding 5, FS) = CPU copy
    // of the GL binding-28 pool array (GetObjectLightPoolParams().pool).

    // Descriptors: 0=UBO(VS/FS), 1=instance SSBO(VS), 2=bone SSBO(VS), 3=albedo(FS), 4=LightsData(VS),
    // 5=pool SSBO(FS), 6=AO map(FS), 7=normal map(FS)  (5-7: VK-PARITY-MECH-1).

    // MC2_VK_MECH_MATERIAL: per-actor paint-scheme VK-descriptor cache. Each distinct
    // gos texture (slot-0 paint scheme, VK-PARITY-MECH-1: + per-type AO / normal map)
    // gets ONE copy-once VkImage/view (texture cache); each distinct (albedo, ao,
    // normal) triple gets a descriptor set (set cache; bindings 3/6/7 differ). The
    // draw loop binds the set matching the run, so a mission with >1 distinct scheme
    // shows the right scheme on each mech. A dummy 1x1 white texture (handle sentinel
    // 0) backs runs with no texture.
    bool        wantMaterial  = false;
    bool        materialReady = false;   // >=1 real scheme imported
    struct TexEntry {
        uint32_t      gosHandle = 0;             // 0 = dummy white
        mc2vk::OwnedImage img;
        mc2vk::OwnedImageView view;              // declared after the image: released first
    };
    std::vector<TexEntry> textures;      // [0] = dummy white, always present
    struct SchemeEntry {
        uint32_t albedo = 0, ao = 0, nrm = 0;    // gos handles (0 = dummy)
        VkDescriptorSet set[2] = {};
    };
    std::vector<SchemeEntry> schemes;    // [0] = all-dummy set, always present
    static const uint32_t    kMaxSchemes  = 64;   // descriptor-pool budget (sets)
    static const uint32_t    kMaxTextures = 96;   // copy-once texture budget

    VkCommandBuffer cbuf[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // allocated from `cpool`, freed with it

    // VK-DRAW-PERF-1: per-pass GPU timestamp timer (3 stamps: pass-start / draws-start /
    // pass-end). Gated + fail-soft inside init(); no-op unless MC2_VK_GPU_TIMESTAMPS set.
    mc2vkts::GpuTimer gpuTimer;

    // Owned objects are declared in creation order and released in reverse by the destructor, while the device is alive (the engine context releases the
    // parked island ahead of the device). The timer owns its query pool and is torn down first.
    mc2vk::OwnedCommandPool cpool;
    mc2vk::OwnedRenderPass rpass;
    mc2vk::OwnedFramebuffer fb;
    mc2vk::OwnedDescriptorSetLayout dsl;
    mc2vk::OwnedDescriptorPool pool;
    mc2vk::OwnedSampler albedoSamp;
    mc2vk::OwnedShaderModule vert, frag;
    mc2vk::OwnedPipelineLayout playout;
    mc2vk::OwnedPipeline pipe;
    mc2vk::OwnedBuffer vbo, ibo;

    ~MechIsland() override { gpuTimer.destroy(); }
};

// The island lives in the engine context's slot: the context releases it ahead of the device whatever path ends the process.
std::unique_ptr<mc2vk::IslandState>& islandSlot() {
    return mc2vk::getOrCreateEngineVulkanContext().islandSlot(mc2vk::IslandSlot::Mech);
}
MechIsland* parkedIsland() { return static_cast<MechIsland*>(islandSlot().get()); }

// VK-RUNG8 item-2 (boot-level native-or-decline), mirrors the terrain island: a
// PERMANENT init failure (spv/pipeline/renderpass/buffer creation past the context +
// shared-GBuffer readiness gates) declines the mech island for the SESSION with ONE
// receipt line (GL owns mechs from frame 0) instead of re-newing + re-init'ing every
// frame. A TRANSIENT readiness gate (VK context / shared GBuffer not born yet at boot)
// keeps the item-9 fail-hard-with-retry (GL draws this frame, retry next). g_mech-
// InitReachedBuild flips to 1 once init passes the readiness gates.
static bool g_mechSessionDeclined  = false;
static int  g_mechInitReachedBuild = 0;

// gpuReadHeavy: a host-written buffer the GPU reads many times per frame (the night pool, walked by every mech fragment) is written
// sequentially so VMA can place it in device-local, host-visible memory instead of system memory read across the bus.
bool make_buffer(MechIsland* s, mc2vk::OwnedBuffer& out, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                 std::string_view name, int i0 = -1, int i1 = -1, bool gpuReadHeavy = false) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size ? size : 16; bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = !hostVisible ? 0
              : gpuReadHeavy ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                             : VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    if (!out.create(s->allocator, s->device, bci, aci, name, i0, i1)) {
        vlog("buffer creation failed (%.*s).", static_cast<int>(name.size()), name.data()); return false;
    }
    return true;
}

// Write the five per-frame BUFFER bindings (0 UBO / 1 inst / 2 bone / 4 lights / 5 pool) of ONE descriptor set at the frame ring's current buffer, and mark
// the set's parity as needing a rewrite at its next frame (the frame knows which buffer its span came from). binding-3 (albedo) is written separately
// per scheme by bind_scheme_images (3/6/7).
constexpr VkDeviceSize kUboRange = sizeof(MechFrameUBO);

void write_ring_bindings(MechIsland* s, VkDescriptorSet set) {
    if (set == VK_NULL_HANDLE) return;
    const VkBuffer ring = mc2vk::getOrCreateEngineVulkanContext().frameRingBuffer();
    if (ring == VK_NULL_HANDLE) return;
    const VkDeviceSize range = mc2vk::getOrCreateEngineVulkanContext().frameRingRange();
    mc2vk::writeDynamicBinding(s->device, set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, ring, kUboRange);
    for (const uint32_t b : {1u, 2u, 4u, 5u})
        mc2vk::writeDynamicBinding(s->device, set, b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, ring, range);
}

// Point the ring bindings of EVERY cached scheme set's [par] slot at the buffers this frame's spans live in. Safe after beginFrame (the parity fence
// retired every [par] consumer); the [1-par] sets, possibly in flight, are untouched and follow at their own frame. A binding already pointing at its
// buffer is skipped.
void sync_ring_bindings(MechIsland* s, uint32_t par, const mc2vk::FrameAlloc& mainSpan, const mc2vk::FrameAlloc& lightSpan) {
    struct Want { uint32_t binding; VkDescriptorType type; VkBuffer buf; VkDeviceSize range; };
    const VkBuffer mainBuf = mainSpan.buffer;
    const VkBuffer lightBuf = lightSpan ? lightSpan.buffer : mainSpan.buffer;
    const VkDeviceSize lightRange = lightSpan ? lightSpan.range : mainSpan.range;
    const Want want[5] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, mainBuf, kUboRange},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, mainBuf, mainSpan.range},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, mainBuf, mainSpan.range},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, lightBuf, lightRange},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, mainBuf, mainSpan.range},
    };
    for (const Want& w : want) {
        if (w.buf == VK_NULL_HANDLE || s->ringBound[par][w.binding] == w.buf) continue;
        for (const auto& e : s->schemes)
            if (e.set[par] != VK_NULL_HANDLE) mc2vk::writeDynamicBinding(s->device, e.set[par], w.binding, w.type, w.buf, w.range);
        s->ringBound[par][w.binding] = w.buf;
    }
}

// Point a scheme set's binding-3 (albedo) / 6 (AO) / 7 (normal) at its images + the
// shared sampler (REPEAT/LINEAR; the FS samples all three with textureLod(...,0)).
void bind_scheme_images(MechIsland* s, VkDescriptorSet set, VkImageView albedo,
                        VkImageView ao, VkImageView nrm) {
    if (set == VK_NULL_HANDLE || albedo == VK_NULL_HANDLE || ao == VK_NULL_HANDLE ||
        nrm == VK_NULL_HANDLE || !s->albedoSamp) return;
    VkDescriptorImageInfo info[3]{};
    const VkImageView views[3] = { albedo, ao, nrm };
    const uint32_t    binds[3] = { 3u, 6u, 7u };
    VkWriteDescriptorSet w[3]{};
    for (int k = 0; k < 3; ++k) {
        info[k].sampler = s->albedoSamp.get(); info[k].imageView = views[k];
        info[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[k].dstSet = set;
        w[k].dstBinding = binds[k]; w[k].descriptorCount = 1;
        w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[k].pImageInfo = &info[k];
    }
    vkUpdateDescriptorSets(s->device, 3, w, 0, nullptr);
}

// MC2_VK_MECH_MATERIAL: one-shot command helpers + copy-once single-2D albedo import.
VkCommandBuffer cp_begin(MechIsland* s) {
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = s->cpool.get(); cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(s->device, &cbi, &cb) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}
void cp_end(MechIsland* s, VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    // VK-QWI-TO-FENCE-1: transient per-submit fence instead of a queue-wide drain
    // (copy-once helper, not per-frame; fail-soft to QWI if fence creation fails).
    mc2vk::OwnedFence fence;
    VkFenceCreateInfo fci{}; fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    mc2vk::create(fence, s->device, [&](VkFence* h) { return vkCreateFence(s->device, &fci, nullptr, h); });   // empty on failure: the queue idle below stands in
    mc2vk::submitGraphics(si, fence.get());
    if (fence) {
        const VkFence f = fence.get();
        vkWaitForFences(s->device, 1, &f, VK_TRUE, 5ull * 1000 * 1000 * 1000);
    } else {
        vkQueueWaitIdle(s->queue);
    }
    vkFreeCommandBuffers(s->device, s->cpool.get(), 1, &cb);
}

bool make_albedo_sampler(MechIsland* s) {
    if (s->albedoSamp) return true;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    return mc2vk::create(s->albedoSamp, s->device, [&](VkSampler* h) { return vkCreateSampler(s->device, &sci, nullptr, h); });
}

// Upload one level of RGBA8 or BC7 texels, as they are, into a fresh VkImage+view of `fmt` stored in the
// given TexEntry (copy-once texture cache slot).
bool upload_scheme_image(MechIsland* s, MechIsland::TexEntry& e,
                         uint32_t w, uint32_t h, VkFormat fmt, const void* bytes, VkDeviceSize byteCount) {
    mc2vk::OwnedBuffer stg;   // released on return; cp_end has waited for the copy by then
    if (!make_buffer(s, stg, byteCount, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, "mech.staging")) return false;
    { void* p = stg.map();
      if (!p) return false;
      std::memcpy(p, bytes, (size_t)byteCount); stg.unmap(); }

    if (!e.img.createSampled2D(s->allocator, s->device, fmt, w, h, 1, 0, "mech.texture")) return false;
    VkCommandBuffer cb = cp_begin(s);
    if (cb == VK_NULL_HANDLE) return false;
    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bar.image = e.img.get();
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bar.srcAccessMask = 0; bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cb, stg.get(), e.img.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    cp_end(s, cb);

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = e.img.get(); vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return mc2vk::create(e.view, s->device, [&](VkImageView* h) { return vkCreateImageView(s->device, &vci, nullptr, h); });
}

// Allocate a descriptor set pair for a scheme (all 8 bindings), write buffer bindings
// + its three images. Returns false on failure.
bool alloc_scheme_sets(MechIsland* s, VkImageView albedo, VkImageView ao, VkImageView nrm,
                       VkDescriptorSet out[2]) {
    VkDescriptorSetLayout layouts[2] = {s->dsl.get(), s->dsl.get()};
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = s->pool.get(); dsai.descriptorSetCount = 2; dsai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(s->device, &dsai, out) != VK_SUCCESS) return false;
    for (uint32_t par = 0; par < 2; ++par) {
        write_ring_bindings(s, out[par]);
        for (VkBuffer& b : s->ringBound[par]) b = VK_NULL_HANDLE;   // the next frame of this parity rewrites its sets at the buffers its spans live in
        bind_scheme_images(s, out[par], albedo, ao, nrm);
    }
    return true;
}

// Create the always-present dummy white texture (textures[0]) + all-dummy set (schemes[0]).
bool ensure_dummy_scheme(MechIsland* s) {
    if (!make_albedo_sampler(s)) return false;
    if (!s->schemes.empty()) return true;
    if (s->textures.empty()) {
        MechIsland::TexEntry t; t.gosHandle = 0;
        const uint8_t px[4] = {255, 255, 255, 255};   // white -> flat FS uses vArgb only
        if (!upload_scheme_image(s, t, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, px, sizeof(px))) return false;
        s->textures.push_back(std::move(t));
    }
    MechIsland::SchemeEntry e;
    VkImageView v = s->textures[0].view.get();
    if (!alloc_scheme_sets(s, v, v, v, e.set)) return false;
    s->schemes.push_back(e);
    return true;
}

// Copy-once import of a MATERIAL texture (paint scheme, AO or normal map) from the handle's
// decoded texels, keyed on the gos handle. Not a colour surface, so g_forbidden is untouched.
// Returns the dummy white view on 0 / failure / budget; sets *pending when the handle has no
// texels yet, so the caller does not cache a set built on the dummy.
VkImageView get_tex_view(MechIsland* s, uint32_t gosHandle, bool* pending) {
    VkImageView dummy = s->textures.empty() ? VK_NULL_HANDLE : s->textures[0].view.get();
    if (gosHandle == 0) return dummy;
    for (auto& t : s->textures) if (t.gosHandle == gosHandle) return t.view.get();
    if (s->textures.size() >= MechIsland::kMaxTextures) return dummy;
    const GosTexels texels = gos_GetTextureTexels(gosHandle);
    if (texels.status == GosTexels::Status::Pending) { *pending = true; return dummy; }
    if (texels.status == GosTexels::Status::Unavailable) return dummy;   // no CPU texels: the set caches the dummy
    const uint32_t w = texels.width, h = texels.height;
    if (w == 0 || h == 0 || w > 8192 || h > 8192) return dummy;
    MechIsland::TexEntry t; t.gosHandle = gosHandle;
    if (!upload_scheme_image(s, t, w, h, mc2vk::texelFormat(texels.vkFormat), texels.bytes->data(),
                             (VkDeviceSize)texels.bytes->size()))
        return dummy;
    const VkImageView cachedView = t.view.get();
    s->textures.push_back(std::move(t));
    vlog("texture cached: %ux%u handle=%u (texture %zu/%u).", w, h, gosHandle,
         s->textures.size() - 1, MechIsland::kMaxTextures);
    return cachedView;
}

// Per-run scheme-set lookup keyed on the (albedo, ao, normal) gos handles, importing any new
// texture copy-once on first sight. Called for every run BEFORE recording, so the draw-loop
// lookup is a pure cache hit.
VkDescriptorSet get_scheme_set(MechIsland* s, uint32_t albedo, uint32_t ao, uint32_t nrm,
                               uint32_t par) {
    VkDescriptorSet dummySet = s->schemes.empty() ? VK_NULL_HANDLE : s->schemes[0].set[par];
    if (albedo == 0 && ao == 0 && nrm == 0) return dummySet;
    for (auto& e : s->schemes)
        if (e.albedo == albedo && e.ao == ao && e.nrm == nrm) return e.set[par];
    if (s->schemes.size() >= MechIsland::kMaxSchemes) return dummySet;  // budget: draws unlit-ish
    bool pending = false;
    VkImageView va = get_tex_view(s, albedo, &pending);
    VkImageView vo = get_tex_view(s, ao, &pending);
    VkImageView vn = get_tex_view(s, nrm, &pending);
    if (pending) return dummySet;   // a texture is not resident yet: retry next frame
    MechIsland::SchemeEntry e; e.albedo = albedo; e.ao = ao; e.nrm = nrm;
    if (!alloc_scheme_sets(s, va, vo, vn, e.set)) return dummySet;
    s->schemes.push_back(e);
    if (albedo != 0) s->materialReady = true;
    vlog("scheme set cached: albedo=%u ao=%u normal=%u (set %zu/%u).", albedo, ao, nrm,
         s->schemes.size() - 1, MechIsland::kMaxSchemes);
    return e.set[par];
}

bool init_island(MechIsland* s, int w, int h) {
    s->w = w; s->h = h;
    ++g_health.attempted;
    g_mechInitReachedBuild = 0;  // reset; flips to 1 after readiness gates pass

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
    g_health.vulkanAvailable = 1;
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(s->phys, &props);
        std::strncpy(g_health.deviceName, props.deviceName, sizeof(g_health.deviceName) - 1);
    }

    s->wantMaterial = mechMaterialRequested();
    s->wantLighting = mechLightingRequested();

    // Fetch the SHARED scene color/normal/depth exportable images the GL scene renders
    // terrain into. Fail-soft: any missing => GL fallback (the GL mech flush runs).
    s->colorRes  = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
    s->normalRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_NORMAL);
    s->depthRes  = mc2SharedSceneDepthGetResource();
    if (!s->colorRes || !s->normalRes || !s->depthRes) {
        setFallback("shared_gbuffer_not_ready"); return false;
    }
    // VK-RUNG8 item-4/item-5: direct-chain-native only. The shared GBuffer is
    // VK-only and left Vulkan-owned by the terrain first-owner, so the mech island
    // issues no GL<->VK semaphore handoff and has no interop/vk2gl semaphore state.
    g_health.interopActive = 0;
    g_health.vk2glSemActive = 0;
    // Readiness gates (context + shared GBuffer) passed: any failure past this point is a
    // PERMANENT build failure -> the island declines for the session (item-2).
    g_mechInitReachedBuild = 1;

    // Command pool + 2 command buffers (parity).
    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s->gfxFamily;
        if (!mc2vk::create(s->cpool, s->device, [&](VkCommandPool* h) { return vkCreateCommandPool(s->device, &pci, nullptr, h); })) { vlog("cmd pool failed."); return false; }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = s->cpool.get(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 2;
        if (vkAllocateCommandBuffers(s->device, &cai, s->cbuf) != VK_SUCCESS) { vlog("cmd bufs failed."); return false; }
    }

    // VK-DRAW-PERF-1: 3-stamp GPU timer (pass-start / draws-start / pass-end).
    // No-op unless MC2_VK_GPU_TIMESTAMPS is set; fail-soft if timestamps unsupported.
    if (mc2vkts::timestampsEnabled()) {
        if (s->gpuTimer.init(s->device, s->phys, s->gfxFamily, 3))
            vlog("GPU timestamps ON (period=%.2f ns): pass segments = setup / mech draws.",
                 s->gpuTimer.periodNs);
        else
            vlog("GPU timestamps requested but unsupported on this queue (validBits=0 or period<=0).");
    }

    // The per-scheme descriptor sets (incl. the dummy white schemes[0]) are created in
    // the descriptor-pool block below (needs s->pool). binding3 is always valid there.

    // Render pass: 2 color LOAD (preserve GL terrain in shared color/normal) + shared
    // depth LOAD (D32S8, GEQUAL test+write). NO clear. Same shape as the prop island Tier-1.
    {
        VkImageResource* colorRes  = reinterpret_cast<VkImageResource*>(s->colorRes);
        VkImageResource* normalRes = reinterpret_cast<VkImageResource*>(s->normalRes);
        VkImageResource* depthRes  = reinterpret_cast<VkImageResource*>(s->depthRes);
        VkAttachmentDescription att[3]{};
        att[0].format = colorRes->format;  att[0].samples = VK_SAMPLE_COUNT_1_BIT;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[1].format = normalRes->format; att[1].samples = VK_SAMPLE_COUNT_1_BIT;
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; att[1].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[2].format = depthRes->format;  att[2].samples = VK_SAMPLE_COUNT_1_BIT;
        att[2].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; att[2].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

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
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT; deps[1].dstAccessMask = 0;

        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 3; rpci.pAttachments = att;
        rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        if (!mc2vk::create(s->rpass, s->device, [&](VkRenderPass* h) { return vkCreateRenderPass(s->device, &rpci, nullptr, h); })) { setFallback("renderpass_failed"); return false; }
        VkImageView fbViews[3] = { colorRes->view.get(), normalRes->view.get(), depthRes->view.get() };
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->rpass.get(); fci.attachmentCount = 3; fci.pAttachments = fbViews;
        fci.width = (uint32_t)w; fci.height = (uint32_t)h; fci.layers = 1;
        if (!mc2vk::create(s->fb, s->device, [&](VkFramebuffer* h) { return vkCreateFramebuffer(s->device, &fci, nullptr, h); })) { setFallback("framebuffer_failed"); return false; }
    }

    // Descriptor layout: 0=UBO(VS/FS), 1=instance SSBO(VS), 2=bone SSBO(VS), 3=albedo(FS).
    {
        VkDescriptorSetLayoutBinding b[8]{};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[2].binding = 2; b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[3].binding = 3; b[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[3].descriptorCount = 1; b[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[4].binding = 4; b[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;   // LightsData
        b[4].descriptorCount = 1; b[4].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        // VK-PARITY-MECH-1: 5 = object light-pool SSBO, 6 = AO map, 7 = normal map (FS).
        b[5].binding = 5; b[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        b[5].descriptorCount = 1; b[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[6].binding = 6; b[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[6].descriptorCount = 1; b[6].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[7].binding = 7; b[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[7].descriptorCount = 1; b[7].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 8; lci.pBindings = b;
        if (!mc2vk::create(s->dsl, s->device, [&](VkDescriptorSetLayout* h) { return vkCreateDescriptorSetLayout(s->device, &lci, nullptr, h); })) { vlog("dsl failed."); return false; }
        // Pool sized for kMaxSchemes descriptor sets (one per distinct paint scheme):
        // each set = 1 UBO + 4 SSBO (inst/bone/lights/pool) + 3 samplers (albedo/AO/normal).
        const uint32_t N = MechIsland::kMaxSchemes;
        VkDescriptorPoolSize sizes[3]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; sizes[0].descriptorCount = 2 * N;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC; sizes[1].descriptorCount = 8 * N;  // inst+bone+lights+pool, two parities
        sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[2].descriptorCount = 6 * N;  // albedo+AO+normal, two parities
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 2 * N; pci.poolSizeCount = 3; pci.pPoolSizes = sizes;
        if (!mc2vk::create(s->pool, s->device, [&](VkDescriptorPool* h) { return vkCreateDescriptorPool(s->device, &pci, nullptr, h); })) { vlog("pool failed."); return false; }
        // Always-present dummy white scheme (schemes[0]) = binding-3 fallback for runs
        // with no slot-0 paint texture (and the material-OFF flat-fill path).
        if (!ensure_dummy_scheme(s)) { setFallback("dummy_scheme_failed"); return false; }
    }

    // Pipeline: mech 48B vertex stream, instanced, GEQUAL reverse-Z test+write, cull NONE,
    // 2 GBuffer outputs, no blend (opaque in-place overwrite).
    {
        s->vert = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "mech_tier1.vert.spv"));
        s->frag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "mech_tier1.frag.spv"));
        if (!s->vert || !s->frag) { setFallback("spv_load_failed"); return false; }

        // VK-PARITY-MECH-1: per-run materialFlags (ALPHA_TEST discard, mech.frag:177).
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; pcr.offset = 0; pcr.size = 4;
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        const VkDescriptorSetLayout dslHandle = s->dsl.get();
        plci.setLayoutCount = 1; plci.pSetLayouts = &dslHandle;
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if (!mc2vk::create(s->playout, s->device, [&](VkPipelineLayout* h) { return vkCreatePipelineLayout(s->device, &plci, nullptr, h); })) { vlog("playout failed."); return false; }

        // Mirror GpuMechVertex (48B): loc0 pos vec3@0, loc1 normal vec3@12, loc2 uv vec2@24,
        // loc3 boneIndices uvec4 (u8x4)@32, loc4 boneWeights vec4 (u8x4 normalized)@36,
        // loc5 tangentOct (s16x2 norm)@40, loc6 aRGBLight uint@44.
        VkVertexInputBindingDescription vib{};
        vib.binding = 0; vib.stride = mc2_mech_vertex_stride();  // pipeline is baked before geometry upload (s->vertexStride may be 0)
        vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription via[7]{};
        via[0].location = 0; via[0].binding = 0; via[0].format = VK_FORMAT_R32G32B32_SFLOAT; via[0].offset = 0;
        via[1].location = 1; via[1].binding = 0; via[1].format = VK_FORMAT_R32G32B32_SFLOAT; via[1].offset = 12;
        via[2].location = 2; via[2].binding = 0; via[2].format = VK_FORMAT_R32G32_SFLOAT;    via[2].offset = 24;
        via[3].location = 3; via[3].binding = 0; via[3].format = VK_FORMAT_R8G8B8A8_UINT;    via[3].offset = 32; // uvec4
        via[4].location = 4; via[4].binding = 0; via[4].format = VK_FORMAT_R8G8B8A8_UNORM;   via[4].offset = 36; // vec4 [0,1]
        via[5].location = 5; via[5].binding = 0; via[5].format = VK_FORMAT_R16G16_SNORM;     via[5].offset = 40; // vec2 [-1,1]
        via[6].location = 6; via[6].binding = 0; via[6].format = VK_FORMAT_R32_UINT;         via[6].offset = 44;
        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vib;
        vin.vertexAttributeDescriptionCount = 7; vin.pVertexAttributeDescriptions = via;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1; vps.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;   // GL mech color pass does not hard-cull
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;   // reverse-Z, matches GL mech opaque

        VkPipelineColorBlendAttachmentState cba[2]{};
        for (int k = 0; k < 2; ++k) {
            cba[k].blendEnable = VK_FALSE;
            cba[k].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        }
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 2; cb.pAttachments = cba;

        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsci{};
        dsci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dsci.dynamicStateCount = 2; dsci.pDynamicStates = dyn;

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
        if (!mc2vk::create(s->pipe, s->device, [&](VkPipeline* h) { return vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h); })) {
            setFallback("pipeline_failed"); return false;
        }
        mc2vk::nameObject(s->device, s->pipe.get(), "mech.pipe");
    }

    s->inited = true;
    setFallback("");
    vlog("island initialized %dx%d (borrowed shared device; skinned mech co-write into "
         "shared color(MRT0)+normal(MRT1)+depth; GEQUAL reverse-Z; material=%d lighting=%d).",
         w, h, s->wantMaterial ? 1 : 0, s->wantLighting ? 1 : 0);
    return true;
}

// Copy the shared mech VBO/IBO into VK-owned buffers ONCE per map.
bool upload_geometry_once(MechIsland* s) {
    Mc2MechGeomFeed feed{};
    const bool haveFeed = mc2_mech_island_get_geometry_feed(&feed) != 0;
    if (s->geomUploaded && (!haveFeed || feed.geomGen == s->geomGen)) return true;
    if (!haveFeed) return false;
    // A null source means a normals rebuild raced the accessor: decline before the waitIdle and the buffer destroy, so the old
    // buffers stay and no device stall is taken for a retry.
    if (feed.vboBytes == 0 || feed.iboBytes == 0 || !feed.vboCpu || !feed.iboCpu) return false;
    if (s->geomUploaded) {
        // A new map (or a normals rebuild) replaced the batcher's vertex and index sets: the draw runs index into the NEW
        // set, so a buffer kept from the previous map is read past its end (VUID-vkCmdDrawIndexed-robustBufferAccess2-08798).
        // Frames in flight may still read the old buffers.
        mc2vk::getOrCreateEngineVulkanContext().waitIdle("mech_geometry_regen");
        s->vbo.reset();
        s->ibo.reset();
        s->geomUploaded = false;
    }
    // The bytes come from the batcher's CPU copy (the exact bytes the GL buffers were created from).
    if (feed.vertexStride != mc2_mech_vertex_stride())
        vlog("ERROR: mech feed vertexStride %u != pipeline stride %u (mech geometry will explode).", feed.vertexStride, mc2_mech_vertex_stride());
    s->vertexStride = feed.vertexStride;

    // A failed copy leaves both buffers empty, so the next frame's retry starts from nothing instead of drawing unfilled buffers.
    if (!s->vbo.createFilled(s->allocator, s->device, feed.vboCpu, feed.vboBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "mech.vbo") ||
        !s->ibo.createFilled(s->allocator, s->device, feed.iboCpu, feed.iboBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "mech.ibo")) {
        vlog("mech geometry upload failed (buffer creation or map); retrying next frame.");
        s->vbo.reset();
        s->ibo.reset();
        return false;
    }
    s->geomGen = feed.geomGen;
    g_health.geomVerts = (unsigned long)(feed.vboBytes / (feed.vertexStride ? feed.vertexStride : 1));
    s->geomUploaded = true;
    vlog("mech geometry uploaded once: vbo=%llu B ibo=%llu B stride=%u source=cpu-native.",
         (unsigned long long)feed.vboBytes, (unsigned long long)feed.iboBytes, feed.vertexStride);
    return true;
}

} // namespace

// ===========================================================================
// PUBLIC ENTRY POINTS.
// ===========================================================================

extern "C" int mc2_vk_mech_island_enabled() {
    return mechGateRequested() ? 1 : 0;
}

extern "C" int mc2_vk_mech_island_run(int w, int h) {
    ZoneScopedN("VK-mech-island");   // VK-FLIP-FIELD-REPORT-1 coarse region zone
    if (!mechGateRequested()) return 0;
    if (w <= 0 || h <= 0) return 0;

    using clk = std::chrono::steady_clock;
    auto usSince = [](clk::time_point t0){ return std::chrono::duration<double, std::micro>(clk::now() - t0).count(); };
    const clk::time_point tTotal0 = clk::now();
    const bool perf = mechPerfGateRequested();
    double us_fence_wait = 0.0;

    Mc2MechFrameFeed feed{};
    if (!mc2_mech_island_get_frame_feed(&feed)) { setFallback("no_frame_feed"); return 0; }
    if (!feed.instances64 || feed.instanceCount == 0 || !feed.runs || feed.runCount == 0) {
        setFallback("no_mech_instances"); return 0;
    }

    // VK-MECH-CAPTURE-DETERMINISM-1: frame-stamped cull-parity witness. feed.instanceCount
    // is s_vkMechInstSnapshot.size() == the GL batcher's totalInstances for THIS frame
    // (assigned in the same flush, gos_mech_batcher.cpp:2796) so it is directly comparable
    // to GL's [MECHBATCHER v1] event=summary instances=. Gated by the same stats env so
    // controls stay clean (no default-on spam). extern frame counter (tgl.cpp).
    {
        static const bool s_cullWitness = mc2::gate::get<"MC2_MECH_BATCHER_STATS">();
        if (s_cullWitness) {
            // Ordinal = per-drawn-frame (usedVulkan+1); the GL batcher builds one feed
            // per frame the island draws, so the ordered sequence of vk_instances must
            // equal the batcher's instances= sequence (equal every frame by construction).
            std::fprintf(stderr, "[VK_MECH_ISLAND] event=cull_witness seq=%lu vk_instances=%u runs=%u\n",
                         g_health.usedVulkan + 1, feed.instanceCount, feed.runCount);
        }
    }

    // Item-2 boot-level native-or-decline: a prior PERMANENT init failure declined this
    // island for the whole session -> GL owns mechs, no per-frame re-init churn.
    if (g_mechSessionDeclined) return 0;
    mc2vk::VulkanContext& slotCtx = mc2vk::getOrCreateEngineVulkanContext();
    if (const MechIsland* parked = parkedIsland(); parked && (parked->w != w || parked->h != h)) slotCtx.retireIsland(mc2vk::IslandSlot::Mech);
    if (!islandSlot()) {
        auto fresh = std::make_unique<MechIsland>();
        if (!init_island(fresh.get(), w, h)) {
            const bool permanent = (g_mechInitReachedBuild != 0);
            slotCtx.waitIdle("mech_init_failure");   // objects made before the failure may have been submitted
            if (permanent) {
                // Build-phase failure won't recover this session: latch decline + ONE
                // receipt line (vs. re-init'ing every frame). GL renders mechs from frame 0.
                g_mechSessionDeclined = true;
                vlog("[VK_MECH_ISLAND] boot-level decline: permanent init failure (%s) -- "
                     "GL owns mechs for this session (no per-frame retry).",
                     g_health.fallbackReason);
            } else {
                // Transient readiness gate (VK context / shared GBuffer not born yet):
                // keep item-9 fail-hard-with-retry -- GL draws this frame, retry next.
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
    MechIsland* s = parkedIsland();
    if (s->disabled || !s->inited) return 0;

    if (!upload_geometry_once(s)) { setFallback("geometry_not_ready"); return 0; }

    // MC2_VK_MECH_MATERIAL: per-actor paint-scheme cache. Import each distinct run's
    // slot-0 paint scheme copy-once here (before recording), so get_scheme_set in the
    // draw loop is a pure cache lookup (no GL calls inside the VK command buffer).
    if (s->wantMaterial) {
        for (uint32_t r = 0; r < feed.runCount; ++r) {
            const Mc2MechDrawRun& rr = feed.runs[r];
            get_scheme_set(s, rr.slot0Tex, rr.aoTex, rr.normalTex, 0);
        }
    }

    const unsigned long f0b = g_forbidden.glGetTexImage;
    const unsigned long f0c = g_forbidden.glReadPixels;

    // Per-frame data goes into the engine frame ring BEFORE beginFrame: a ring failure after beginFrame would reset the acquired shared fence and could
    // strand it unsignalled. The ring slot belongs to the frame, not to the island's parity, so staging here races nothing.
    mc2vk::VulkanContext& rctx = mc2vk::getOrCreateEngineVulkanContext();
    const VkDeviceSize ringAlign = rctx.frameAlignment();
    const VkDeviceSize instBytes = (VkDeviceSize)feed.instanceCount * 64ull;
    const VkDeviceSize boneBytes = (VkDeviceSize)(feed.boneCount ? feed.boneCount : 1) * 64ull;
    const bool nativeLight = feed.lightDataCpu && feed.lightDataCpuBytes >= 3600ull;
    int poolN = (feed.poolLights48 && feed.numPoolLights > 0) ? feed.numPoolLights : 0;
    if (poolN > (int)kMaxPoolLights) poolN = (int)kMaxPoolLights;
    const VkDeviceSize instOff = mc2vk::alignUp(sizeof(MechFrameUBO), ringAlign);
    const VkDeviceSize boneOff = mc2vk::alignUp(instOff + (instBytes ? instBytes : 64ull), ringAlign);
    const VkDeviceSize poolOff = mc2vk::alignUp(boneOff + boneBytes, ringAlign);
    const mc2vk::FrameAlloc mainSpan = rctx.frameAlloc(poolOff + (VkDeviceSize)(poolN > 0 ? poolN : 1) * 48ull);
    if (!mainSpan) { setFallback("frame_ring_alloc_failed"); s->disabled = true; return 0; }
    uint8_t* const mainPtr = static_cast<uint8_t*>(mainSpan.ptr);

    // VK-MECH-FENCE-DEFER-1: retire the parity slot before ANY per-frame CPU
    // staging that the slot's prior submit may still read.
    VkImageResource* colorRes = reinterpret_cast<VkImageResource*>(s->colorRes);
    const clk::time_point tFence0 = clk::now();
    VkFrameSync fsC{};
    { ZoneScopedN("VKIsl.Mech.FenceWait");   // VK-ISLAND-TRACY-1: deferred retire of the parity slot
      fsC = vkImageResourceBeginFrame(colorRes); }
    us_fence_wait = usSince(tFence0);
    const uint32_t par = fsC.parity & 1u;

    const clk::time_point tStage0 = clk::now();
    // Upload instance + bone snapshots into the frame span.
    { ZoneScopedN("VK-mech-instance-bone-upload");   // VK-FLIP-FIELD-REPORT-1: per-frame skinning feed staging
      if (instBytes) std::memcpy(mainPtr + instOff, feed.instances64, (size_t)instBytes);
      if (feed.boneCount) std::memcpy(mainPtr + boneOff, feed.bones64, (size_t)(feed.boneCount * 64ull)); }
    g_health.instRecords = feed.instanceCount;
    g_health.boneRecords = feed.boneCount;

    // VK-MECH-MATERIAL-LIGHTING-1: per-frame copy of the CPU LightsData array (the byte source of the GL SSBO upload) into a frame-ring span (binding 4)
    // so calc_light reads the same ObjectLights[] the GL mech VS does. If the CPU array is not ready this frame, lighting is simply not marked ready
    // (the shader falls back to flat-lit via u.lightingReady=0). Binding 4 still names a valid span (the main one) then.
    s->lightingReady = false;
    mc2vk::FrameAlloc lightSpan;
    if (s->wantLighting && nativeLight) {
        // VK-LIGHTFEED-DEDUP-1: the shared once-per-frame LightsData span first (ONE copy total across the mech + prop islands; the copy is zoned
        // VKIsl.Shared.LightFeed inside acquire). Killswitch MC2_VK_LIGHTFEED_DEDUP=0 or any acquire fail-soft (mid-frame light-count growth, ring
        // failure) copies into this island's own span below (zone VKIsl.Mech.LightFeed).
        lightSpan = mc2vkSharedLightFeedAcquire(feed.lightDataCpu, feed.lightDataCpuBytes);
        if (!lightSpan) {
            ZoneScopedN("VKIsl.Mech.LightFeed");   // VK-ISLAND-TRACY-1: per-frame LightsData CPU->VK copy (own-span path)
            lightSpan = rctx.frameAlloc((VkDeviceSize)feed.lightDataCpuBytes);
            if (lightSpan) std::memcpy(lightSpan.ptr, feed.lightDataCpu, (size_t)feed.lightDataCpuBytes);
        }
        s->lightingReady = static_cast<bool>(lightSpan);
    }

    // Upload UBO.
    { MechFrameUBO u{};
      std::memcpy(u.worldToClipGL, feed.worldToClipGL, sizeof(u.worldToClipGL));
      u.objectDepthBias = feed.objectDepthBias;
      u.materialReady = (s->wantMaterial && s->materialReady) ? 1 : 0;
      u.lightingReady = s->lightingReady ? 1 : 0;
      u.sunAxisMc2Fix = mc2::options::current().sunAxisFix ? 1 : 0;   // same option the GL mech flush reads
      u.fogColorRGB[0] = feed.fogColorRGB[0];
      u.fogColorRGB[1] = feed.fogColorRGB[1];
      u.fogColorRGB[2] = feed.fogColorRGB[2];
      u.fogColorRGB[3] = 0.0f;
      // VK-PARITY-MECH-1: same producers as the GL mech flush uniforms.
      for (int k = 0; k < 4; ++k) u.cameraWorldPos[k] = feed.cameraWorldPos[k];
      u.shadeA[0] = feed.ambientStrength;  u.shadeA[1] = feed.specularStrength;
      u.shadeA[2] = feed.metalRoughness;   u.shadeA[3] = feed.glassRoughness;
      u.shadeB[0] = feed.glassLumaThresh;  u.shadeB[1] = feed.glassMaxChanThresh;
      u.shadeB[2] = feed.importedGamma;    u.shadeB[3] = feed.importedAlbedoScale;
      u.shadeC[0] = feed.aoStrength;       u.shadeC[1] = feed.normalStrength;
      if (poolN > 0) std::memcpy(mainPtr + poolOff, feed.poolLights48, static_cast<size_t>(poolN) * 48u);
      u.poolF[0] = feed.poolMoverDim; u.poolF[1] = feed.poolMoverScale; u.poolF[2] = feed.poolObjScale;
      u.poolI[0] = poolN;             u.poolI[1] = feed.poolSoft;
      u.poolI[2] = feed.poolMoverMax; u.poolI[3] = feed.poolCompress;
      u.miscI[0] = feed.specDebugMask;
      std::memcpy(mainPtr, &u, sizeof(u)); }
    // Every scheme set of this parity follows the buffers the spans came from; the draw passes the offsets.
    sync_ring_bindings(s, par, mainSpan, lightSpan);
    const uint32_t dynOffsets[5] = {
        static_cast<uint32_t>(mainSpan.offset),
        static_cast<uint32_t>(mainSpan.offset + instOff),
        static_cast<uint32_t>(mainSpan.offset + boneOff),
        static_cast<uint32_t>(lightSpan ? lightSpan.offset : mainSpan.offset),
        static_cast<uint32_t>(mainSpan.offset + poolOff)};
    g_health.stageUs = usSince(tStage0);

    // ---- Shared-GBuffer co-write (VK-RUNG8 item-4: direct-chain only). Terrain left
    // the shared color/normal/depth Vulkan-owned in their attachment layouts, so there
    // is NO GL<->VK semaphore handoff — begin a frame only on the color parity and rely
    // on the submit fence below for CPU ordering before the next island. ----
    VkCommandBuffer cb = s->cbuf[par];

    const clk::time_point tRender0 = clk::now();
    ZoneNamedN(zMechRec, "VK-mech-record-submit", true);   // VK-FLIP-FIELD-REPORT-1: CPU record+queue-submit (to fn return)
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }
    vk_render_pass_timer::Begin(cb, gos_render_pass_timer::Pass_Mechs);
    vk_render_pass_timer::CancelUnlessSubmitted timerGuard(gos_render_pass_timer::Pass_Mechs);

    // VK-DRAW-PERF-1: reset the query pool (outside the render pass) + stamp pass-start.
    s->gpuTimer.reset(cb);
    s->gpuTimer.write(cb, 0);

    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = s->rpass.get(); rbi.framebuffer = s->fb.get();
    rbi.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
    rbi.clearValueCount = 0; rbi.pClearValues = nullptr;   // all LOAD
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {(uint32_t)w, (uint32_t)h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe.get());
    VkDeviceSize vboOff = 0;
    const VkBuffer vertexBuffer = s->vbo.get();
    vkCmdBindVertexBuffers(cb, 0, 1, &vertexBuffer, &vboOff);
    vkCmdBindIndexBuffer(cb, s->ibo.get(), 0, VK_INDEX_TYPE_UINT32);

    // VK-DRAW-PERF-1: stamp draws-start (gap 0->1 = setup + binds).
    s->gpuTimer.write(cb, 1);

    // One indexed instanced draw per run. firstInstance = run.instanceBase so the VS's
    // gl_InstanceIndex indexes the instance SSBO snapshot exactly as the GL VS's
    // u_instanceBase + gl_InstanceID did. Per run: bind the descriptor set for this run's
    // paint scheme (per-actor cache) so the right albedo (binding 3) is sampled.
    VkDescriptorSet dummySet = s->schemes.empty() ? VK_NULL_HANDLE : s->schemes[0].set[par];
    VkDescriptorSet boundSet = VK_NULL_HANDLE;
    unsigned long runs = 0;
    for (uint32_t r = 0; r < feed.runCount; ++r) {
        const Mc2MechDrawRun& run = feed.runs[r];
        if (run.indexCount == 0 || run.instanceCount == 0) continue;
        VkDescriptorSet set = s->wantMaterial
            ? get_scheme_set(s, run.slot0Tex, run.aoTex, run.normalTex, par) : dummySet;
        if (set == VK_NULL_HANDLE) set = dummySet;
        if (set != boundSet) {
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &set, 5, dynOffsets);
            boundSet = set;
        }
        // VK-PARITY-MECH-1: per-run ALPHA_TEST flag (mech.frag u_materialFlags).
        const uint32_t mflags = run.materialFlags;
        vkCmdPushConstants(cb, s->playout.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &mflags);
        vkCmdDrawIndexed(cb, run.indexCount, run.instanceCount, run.firstIndex,
                         run.baseVertex, run.instanceBase);
        ++runs;
    }
    g_health.runs = runs;

    vkCmdEndRenderPass(cb);
    // VK-DRAW-PERF-1: stamp pass-end (gap 1->2 = mech instanced draws).
    s->gpuTimer.write(cb, 2);
    vk_render_pass_timer::End(cb, gos_render_pass_timer::Pass_Mechs);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

    // VK-RUNG8 item-4: direct-chain only — the submit waits/signals no GL<->VK
    // semaphores; the fence below orders CPU access before the next island records
    // against these shared images.
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    { ZoneScopedN("VKIsl.Mech.Submit");   // VK-ISLAND-TRACY-1: vkQueueSubmit
      if (mc2vk::submitGraphics(si, fsC.fence) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; } }
    timerGuard.submitted();

    if (!mechFenceDeferEnabled()) {
        ZoneScopedN("VKIsl.Mech.InlineFenceWait");   // VK-ISLAND-TRACY-1: killswitch-only inline retire
        const clk::time_point tInline0 = clk::now();
        if (vkWaitForFences(s->device, 1, &fsC.fence, VK_TRUE, 5ull*1000*1000*1000) != VK_SUCCESS) {
            setFallback("fence_timeout"); s->disabled = true; return 0;
        }
        us_fence_wait = usSince(tInline0);
    }
    g_health.drawUs = usSince(tRender0);
    s->gpuTimer.resolve();   // VK-DRAW-PERF-1: fence signalled -> fold GPU segment times

    const bool readbackFired = (g_forbidden.glGetTexImage != f0b) || (g_forbidden.glReadPixels != f0c);
    if (readbackFired) {
        vlog("FATAL: a color readback fired on the co-write path — failing frame + disabling.");
        setFallback("interop_did_readback"); s->disabled = true; return 0;
    }

    g_health.vkDraws = 1;
    ++g_health.usedVulkan;
    g_health.totalUs = usSince(tTotal0);
    static bool s_firstOk = false;
    if (!s_firstOk) {
        vlog("first skinned mech co-write: instCount=%u boneCount=%u runs=%lu "
             "(co-written into shared color(MRT0)+normal(MRT1)+depth; no composite).",
             feed.instanceCount, feed.boneCount, g_health.runs);
        s_firstOk = true;
    }
    if (perf) {
        std::fprintf(stderr,
            "[VK_MECH_PERF] tier1=1 inst=%u bones=%u runs=%lu stage_us=%.1f "
            "record_submit_us=%.1f fence_wait_us=%.1f island_total_us=%.1f\n",
            feed.instanceCount, feed.boneCount, g_health.runs, g_health.stageUs,
            g_health.drawUs, us_fence_wait, g_health.totalUs);
        std::fflush(stderr);
    }
    return 1;
}

extern "C" void mc2_render_backend_mech_region_health(
    int* vulkanAvailable, int* buildEnabled, const char** deviceName,
    unsigned long* attempted, unsigned long* usedVulkan, const char** fallbackReason,
    unsigned long* vkDraws, int* interopActive, int* vk2glSemActive,
    unsigned long* instRecords, unsigned long* boneRecords, unsigned long* runs,
    unsigned long* geomVerts,
    unsigned long* forbiddenGetTexImage, unsigned long* forbiddenReadPixels,
    double* stageUs, double* drawUs, double* totalUs) {
    if (vulkanAvailable)  *vulkanAvailable  = g_health.vulkanAvailable;
    if (buildEnabled)     *buildEnabled     = g_health.buildEnabled;
    if (deviceName)       *deviceName       = g_health.deviceName;
    if (attempted)        *attempted        = g_health.attempted;
    if (usedVulkan)       *usedVulkan       = g_health.usedVulkan;
    if (fallbackReason)   *fallbackReason   = g_health.fallbackReason;
    if (vkDraws)          *vkDraws          = g_health.vkDraws;
    if (interopActive)    *interopActive    = g_health.interopActive;
    if (vk2glSemActive)   *vk2glSemActive   = g_health.vk2glSemActive;
    if (instRecords)      *instRecords      = g_health.instRecords;
    if (boneRecords)      *boneRecords      = g_health.boneRecords;
    if (runs)             *runs             = g_health.runs;
    if (geomVerts)        *geomVerts        = g_health.geomVerts;
    if (forbiddenGetTexImage) *forbiddenGetTexImage = g_forbidden.glGetTexImage;
    if (forbiddenReadPixels)  *forbiddenReadPixels  = g_forbidden.glReadPixels;
    if (stageUs)          *stageUs          = g_health.stageUs;
    if (drawUs)           *drawUs           = g_health.drawUs;
    if (totalUs)          *totalUs          = g_health.totalUs;
}

extern "C" void mc2_vk_mech_island_destroy() {
    vlog("[VK_MECH_TIER1] vulkan_available=%d used_vulkan=%lu interop_active=%d vk2gl_sem=%d "
         "inst_records=%lu bone_records=%lu runs=%lu geom_verts=%lu fallback_reason=\"%s\" "
         "vk_draws=%lu forbidden_getTexImage=%lu forbidden_readPixels=%lu "
         "stage_us=%.1f draw_us=%.1f total_us=%.1f",
         g_health.vulkanAvailable, g_health.usedVulkan, g_health.interopActive, g_health.vk2glSemActive,
         g_health.instRecords, g_health.boneRecords, g_health.runs, g_health.geomVerts,
         g_health.fallbackReason, g_health.vkDraws, g_forbidden.glGetTexImage, g_forbidden.glReadPixels,
         g_health.stageUs, g_health.drawUs, g_health.totalUs);
    // VK-DRAW-PERF-1: GPU-timestamp segment breakdown (only when MC2_VK_GPU_TIMESTAMPS
    // armed and supported; gap1 = setup+binds, gap2 = mech instanced draws).
    const MechIsland* parked = parkedIsland();
    if (parked && parked->gpuTimer.supported && parked->gpuTimer.samples > 0) {
        vlog("GPU_TIMESTAMPS samples=%llu gpu_pass_us=%.1f gpu_setup_us=%.1f gpu_draw_us=%.1f "
             "(cpu_draw_us=%.1f incl. record+submit+fence-wait)",
             (unsigned long long)parked->gpuTimer.samples,
             parked->gpuTimer.avgTotalUs(),
             parked->gpuTimer.avgGapUs(1),
             parked->gpuTimer.avgGapUs(2),
             g_health.drawUs);
    }
    mc2vk::getOrCreateEngineVulkanContext().retireIsland(mc2vk::IslandSlot::Mech);
}

#endif // MC2_VULKAN_ISLAND
