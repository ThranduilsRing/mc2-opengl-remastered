// vulkan_color_prop_island.cpp — VK-P11 STATIC-PROP COLOR: the FIRST VK-native COLOR
// GEOMETRY region rendering REAL static-prop triangle geometry.
// Renders static props in Vulkan directly into the shared scene GBuffer. The
// retired Tier-0 side-image/composite proof path has been mechanically removed;
// this island now owns only the Tier-1 in-place co-write path.
//
// Shape:
//   - Borrows the shared engine VulkanContext; no private device.
//   - Uses the shared scene color0/normal/depth exportable images.
//   - Copies static-prop geometry once per map and stages visible instances per frame.
//   - Optional material/light feeds extend parity without changing the public health ABI.
//
// The whole TU compiles to nothing unless MC2_VULKAN_ISLAND is defined.
// =============================================================================

#ifdef MC2_VULKAN_ISLAND

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <span>
#include <volk.h>
#include "Gates/gate_cold.h"   // gate::get<>() and str<>()
#include "vk_render_pass_timer.h"
#include "vk_gpu_profile.h"   // VK-GPU-ZONES: MC2_VK_GPU_SCOPE

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include "../../RenderCore/gpu_abi/GpuStaticPropInstance.h"  // single-source 112B instance record
#include "../../RenderCore/gpu_abi/GpuObjectLights.h"  // single-source per-object light record
#include "../../RenderCore/gpu_abi/GpuColorFrameUBO.h"  // single-source per-frame UBO
#include "vk_image_resource.h"
#include "vk_island_util.h"
#include "gos_visual_capture.h"      // mc2CaptureGaitFreezeActive() (extern "C")
#include "vk_one_shot.h"
#include "vulkan_context.h"
#include "vulkan_shadow_island_live.h"   // geometry and color feed contracts
#include "gos_postprocess.h"
#include "vulkan_color_prop_island.h"
#include "texture_decode.h"
#include "vk_texture_residency.h"
#include "vk_shared_gbuffer_gate.h"      // VK-P14 GATE-CONSOLIDATION-1 umbrella resolver
#include "vk_shared_light_feed.h"        // VK-LIGHTFEED-DEDUP-1: shared per-frame LightsData SSBO
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"   // TIER-1: fetch shared scene color/normal images
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"   // TIER-1: fetch shared scene depth (island accessors)

extern const float* gos_GetTerrainMVPMat4();   // live camera matrix (matrix diag only)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <cstddef>   // offsetof (M2 layout asserts)
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>
#include <tracy/Tracy.hpp>   // VK-FLIP-FIELD-REPORT-1: coarse per-region frame zones


namespace {

// ---------------------------------------------------------------------------
// Health snapshot (mirrors the shadow island's).
// ---------------------------------------------------------------------------
struct ColorPropIslandHealth {
    int   vulkanAvailable = 0;
    int   buildEnabled    = 1;
    char  deviceName[256] = {0};
    unsigned long validationErrors = 0;
    unsigned long attempted   = 0;
    unsigned long usedVulkan  = 0;
    char  fallbackReason[256] = {0};
    unsigned long vkDraws     = 0;
    int   interopActive  = 0;
    int   vk2glSemActive = 0;
    unsigned long usedInterop = 0;
    unsigned long instRecords = 0;   // color instances fed this frame
    unsigned long typeRuns    = 0;   // per-type instanced draws this frame
    unsigned long geomVerts   = 0;   // shared VBO verts copied once
    double stageInstUs = 0.0, drawUs = 0.0, totalUs = 0.0;
};
ColorPropIslandHealth g_health;

void setFallback(const char* reason) {
    std::strncpy(g_health.fallbackReason, reason ? reason : "",
                 sizeof(g_health.fallbackReason) - 1);
    g_health.fallbackReason[sizeof(g_health.fallbackReason) - 1] = '\0';
}

// Forbidden COLOR-readback counters (zero-copy invariant on the color surface).
struct InteropForbidden {
    unsigned long vkCmdCopyImageToBuffer = 0;  // color readback   (must be 0)
    unsigned long glGetTexImage          = 0;  // color CPU pull   (must be 0)
    unsigned long glReadPixels           = 0;  // color CPU pull   (must be 0)
};
InteropForbidden g_forbidden;

void vlog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vk_island_util::vlogv("[VK_COLOR_PROP_ISLAND] ", fmt, ap);
    va_end(ap);
}

// VK-PROP-FENCE-DEFER-1 (cluster-A, water-pilot template 61031785): defer the
// island's retire wait to the parity slot's next vkImageResourceBeginFrame instead
// of waiting the submit fence inline. Default-ON (per-parity inst/ubo/light staging
// + per-parity descriptor sets make it correct by construction);
// MC2_VK_PROP_FENCE_DEFER=0 is the killswitch restoring the inline wait — the
// per-parity staging holds either way (superset-safe).
bool propFenceDeferEnabled() {
    static int state = -1;
    if (state == -1)
        state = mc2::gate::get<"MC2_VK_PROP_FENCE_DEFER">() ? 1 : 0;
    return state == 1;
}

// MC2_VK_PROP_PERF — per-frame microsecond breakdown receipt ([VK_PROPS_PERF]
// line: stage/record-submit/fence-wait/total). Same class as MC2_VK_WATER_PERF.
bool propPerfGateRequested() {
    static int state = -1;
    if (state == -1) {
        state = mc2::gate::get<"MC2_VK_PROP_PERF">();
    }
    return state == 1;
}

// Prop co-write is unconditionally direct-chain-native (the GL<->VK
// semaphore-handoff arm and the shared-scene GL-import half are deleted).
bool propDirectChainRequested() {
    return true;
}

std::string spv_dir() { return vk_island_util::spvDir(); }

VkShaderModule load_spv(VkDevice device, std::string_view path) {
    return vk_island_util::loadSpv(device, path, vlog);
}


// The per-frame std140 UBO (ColorFrameUBO) is single-sourced in
// RenderCore/gpu_abi/GpuColorFrameUBO.h; its generated block feeds both prop shaders.

const VkFormat kColorFmt = VK_FORMAT_R16G16B16A16_SFLOAT; // MainColor-compatible
const VkFormat kDepthFmt = VK_FORMAT_D32_SFLOAT;          // VK-owned (non-exported)
// Night light-pool SSBO: GpuTerrainLight is 48 B; 256 is the GL object pool cap.
const int          kMaxPoolLights = 256;

// Per-type packet (subset of GpuStaticPropPacket the draw loop needs).
struct TypeRun {
    uint32_t typeID;
    uint32_t firstPacket;
    uint32_t packetCount;
};

struct ColorPropIsland : mc2vk::IslandState {
    bool disabled = false;
    bool inited   = false;
    bool useInterop = false;
    bool tier1    = false;   // TIER-1 in-place shared-GBuffer co-write

    // ---- TIER-1: shared-GBuffer co-write objects (built over the SAME shared
    // color/normal/depth images the GL scene renders terrain into). ----
    // Borrowed (NOT owned) shared-image resources/views, fetched per-frame from the
    // shared-scene-color/-depth registries. Cached at init for the framebuffer build.
    struct VkImageResource* t1_colorRes  = nullptr;
    struct VkImageResource* t1_normalRes = nullptr;
    struct VkImageResource* t1_depthRes  = nullptr;
    // VK-RUNG8-ITEM5: shared scene is VK-only (no GL interop view). Prop co-writes
    // direct-chain-native only; the former t1_*View handoff handles are deleted.

    int w = 0, h = 0;

    // Borrowed from the shared engine context (NOT owned).
    VkInstance       instance  = VK_NULL_HANDLE;
    VkPhysicalDevice phys      = VK_NULL_HANDLE;
    VkDevice         device    = VK_NULL_HANDLE;
    uint32_t         gfxFamily = UINT32_MAX;
    VkQueue          queue     = VK_NULL_HANDLE;
    VmaAllocator     allocator = VK_NULL_HANDLE;

    // Caster/color geometry (VK-owned copies of the shared static-prop VBO/IBO).
    uint32_t      vertexStride = 0;
    bool          geomUploaded = false;

    std::vector<Mc2ShadowPacket> packets;
    std::vector<TypeRun>         typeRuns;
    // VK-FLIP-FIELD-REPORT-2 BLACK-FENCE FIX: per-packet (not per-type) albedo layer,
    // group-relative (same array as typeAlbedoLayer/typeAlbedoLayerOn). Indexed by
    // global packet index (same indexing as `packets` above). -1 = no texture.
    std::vector<int32_t>        packetLayerOff;
    std::vector<int32_t>        packetLayerOn;
    // FENCE-UVSCALE-1: per-packet UV sub-region scale, same indexing as
    // packetLayerOff/On above. Default 1.0 (no crop) until the feed populates
    // it -- matches GL's PerDrawEntry.uvScaleX/Y default (gos_static_prop_batcher.cpp
    // uvScaleXByPacket/uvScaleYByPacket init).
    std::vector<float>          packetUvScaleX;
    std::vector<float>          packetUvScaleY;

    // VK-PROP-FENCE-DEFER-1 (per-parity staging audit): the per-frame CPU-written
    // buffers the submit consumes are the instance SSBO, the frame UBO and the
    // LightsData SSBO — all become PER-PARITY-SLOT pairs, staged [par] AFTER the
    // hoisted beginFrame. Geometry vbo/ibo are upload-once (fresh buffers, latched);
    // layer tables + albedo arrays are map-load writes that run right after a
    // cp_end_oneshot inline fence wait (which retires ALL earlier submission-order
    // work on the queue) — safe by construction.
    // Per-frame data (UBO, instance rows, LightsData, night pool) lives in the engine frame ring; set[par] binds it with dynamic offsets (bindings 0, 1,
    // 4, 8). ringBound[par][binding] = the ring buffer that binding of set[par] currently points at; a frame whose spans came from another buffer
    // (the ring grew), or a set rewritten by write_descriptors, is re-pointed after beginFrame retired the parity.
    VkBuffer      ringBound[2][9] = {};

    // VK-PROP-AMBIENT-IBL-1: per-frame copy of the live GL LightsData SSBO (binding 4,
    // Tier-1 only) so the vert shader's calc_light-style ambient sum reads the same
    // ObjectLights[] the GL prop VS does. wantLighting = tier1 (no separate gate --
    // this is a parity FIX, not a new opt-in feature).
    bool          wantLighting  = false;
    bool          lightingReady = false;   // LightsData copied this frame

    // VK-P11 STATIC-PROP MATERIAL: copy-once albedo 2D_ARRAY import + per-type layer
    // table + terrain sun. All optional (Tier-1 + MC2_VK_PROP_MATERIAL); fail-soft to
    // teal when unavailable (materialReady stays false).
    bool          wantMaterial   = false;   // MC2_VK_PROP_MATERIAL armed
    bool          materialReady  = false;   // albedo array imported this map
    uint32_t      albedoGlTexImported = 0;  // GL name last imported (re-import guard)
    uint32_t      albedoLayers = 0;
    VkDeviceSize  layerCapBytes = 0;
    VkDeviceSize  hotCapBytes = 0;
    uint32_t      mapGeneration = 0;        // bumped at every map change; consumers of the albedo arrays rebind on a change
    uint64_t alphaContentHash = 0;
    bool          hotReady = false;         // the hot-colour table was uploaded for the current map (cleared at a map change)
    float         terrainSunMC2[3] = {0.0f, 0.0f, 1.0f};

    // VK-P11 STATIC-PROP MATERIAL alpha-ON (class 1): second copy-once array import +
    // second per-type layer SSBO. Class-1 types (alpha-MIXED buildings) sample this
    // array with a 0.5 alpha-test discard (matches GL static_prop.frag). Fail-soft to
    // teal when unavailable (materialReadyOn stays false).
    bool          materialReadyOn = false;  // alpha-ON array imported this map
    uint32_t      albedoOnGlTexImported = 0;
    uint32_t      albedoOnLayers = 0;
    VkDeviceSize  layerOnCapBytes = 0;

    VkDescriptorSet   set[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // per parity slot (VK-PROP-FENCE-DEFER-1); set[i] binds ubo/inst/light[i]
    VkCommandBuffer   cbuf[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // allocated from `cpool`, freed with it

    // Owned objects are declared in creation order and released in reverse by the destructor, while the device is alive (the engine context releases the
    // parked island ahead of the device). An image is declared before its view so the view is released first.
    mc2vk::OwnedBuffer vbo, ibo;
    mc2vk::OwnedRenderPass t1_rpass;      // 2 color LOAD + depth LOAD
    mc2vk::OwnedFramebuffer t1_fb;        // over the 3 shared image views
    mc2vk::OwnedDescriptorSetLayout dsl;
    mc2vk::OwnedDescriptorPool pool;
    mc2vk::ResidentTextureRef albedoTexture;
    bool descriptorsDirty[2]={true,true};
    std::array<VkImageView,2> boundTextures[2]{};
    mc2vk::OwnedSampler albedoSamp;
    mc2vk::OwnedBuffer  layerBuf;         // per-type int layer SSBO
    mc2vk::ResidentTextureRef albedoOnTexture;
    mc2vk::OwnedSampler albedoOnSamp;
    mc2vk::OwnedBuffer  layerOnBuf;       // per-type int layer SSBO (class 1)
    mc2vk::OwnedBuffer  hotBuf;           // per-type hot-colour night emission colours (3 vec4 per type)
    mc2vk::OwnedShaderModule t1_vert, t1_frag;
    mc2vk::OwnedPipelineLayout playout;
    mc2vk::OwnedPipeline t1_pipe;         // depthWrite ON GEQUAL, 2 outputs, no blend
    mc2vk::OwnedPipeline t1_fringePipe;   // PROP-COVERAGE-1: depth test only, MRT0 alpha-blended, MRT1 masked
    mc2vk::OwnedCommandPool cpool;
};

// The island lives in the engine context's slot: the context releases it ahead of the device whatever path ends the process.
std::unique_ptr<mc2vk::IslandState>& islandSlot() {
    return mc2vk::getOrCreateEngineVulkanContext().islandSlot(mc2vk::IslandSlot::ColorProp);
}
ColorPropIsland* parkedIsland() { return static_cast<ColorPropIsland*>(islandSlot().get()); }


// gpuReadHeavy: a host-written buffer the GPU reads many times per frame (the night pool, walked by every prop fragment) is written
// sequentially so VMA can place it in device-local, host-visible memory instead of system memory read across the bus.
bool make_buffer(ColorPropIsland* s, mc2vk::OwnedBuffer& out, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
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

constexpr VkDeviceSize kFrameUboRange = sizeof(ColorFrameUBO);

// Binding 4 (LightsData) is declared when tier1 && wantLighting, binding 8 (night pools) when tier1.
void write_ring_bindings(ColorPropIsland* s, VkDescriptorSet set) {
    if (set == VK_NULL_HANDLE) return;
    const VkBuffer ring = mc2vk::getOrCreateEngineVulkanContext().frameRingBuffer();
    if (ring == VK_NULL_HANDLE) return;
    const VkDeviceSize range = mc2vk::getOrCreateEngineVulkanContext().frameRingRange();
    mc2vk::writeDynamicBinding(s->device, set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, ring, kFrameUboRange);
    mc2vk::writeDynamicBinding(s->device, set, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, ring, range);
    if (s->tier1 && s->wantLighting) mc2vk::writeDynamicBinding(s->device, set, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, ring, range);
    if (s->tier1) mc2vk::writeDynamicBinding(s->device, set, 8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, ring, range);
}

// Re-point set[par]'s ring bindings at the buffers this frame's spans live in (after beginFrame retired the parity). A binding already there is skipped.
void sync_ring_bindings(ColorPropIsland* s, uint32_t par, const mc2vk::FrameAlloc& mainSpan, const mc2vk::FrameAlloc& lightSpan) {
    struct Want { uint32_t binding; VkDescriptorType type; VkBuffer buf; VkDeviceSize range; bool on; };
    const VkBuffer mainBuf = mainSpan.buffer;
    const VkBuffer lightBuf = lightSpan ? lightSpan.buffer : mainSpan.buffer;
    const VkDeviceSize lightRange = lightSpan ? lightSpan.range : mainSpan.range;
    const Want want[4] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, mainBuf, kFrameUboRange, true},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, mainBuf, mainSpan.range, true},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, lightBuf, lightRange, s->tier1 && s->wantLighting},
        {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, mainBuf, mainSpan.range, s->tier1},
    };
    for (const Want& w : want) {
        if (!w.on || w.buf == VK_NULL_HANDLE || s->ringBound[par][w.binding] == w.buf) continue;
        mc2vk::writeDynamicBinding(s->device, s->set[par], w.binding, w.type, w.buf, w.range);
        s->ringBound[par][w.binding] = w.buf;
    }
}

void write_descriptors(ColorPropIsland* s, int parity = -1) {
    if (parity<0) { s->descriptorsDirty[0]=s->descriptorsDirty[1]=true; return; }
    for (int i=parity;i<=parity;++i) {
    s->descriptorsDirty[i]=false;
    if (s->set[i] == VK_NULL_HANDLE) continue;
    // The ring bindings (0 UBO, 1 instances, 4 LightsData, 8 pool) at the frame ring's current buffer; the next frame of this parity re-points them at
    // the buffer its spans live in (ringBound is cleared).
    write_ring_bindings(s, s->set[i]);
    for (VkBuffer& b : s->ringBound[i]) b = VK_NULL_HANDLE;
    VkWriteDescriptorSet w[2]{};
    uint32_t n = 0;
    // VK-P11 STATIC-PROP MATERIAL: binding2 layer SSBO (VS) + binding3 albedo array (FS).
    // Only written when the layout declared them (wantMaterial) AND the objects exist
    // (dummy or real). A valid descriptor is required for binding3 even pre-import.
    VkDescriptorBufferInfo layerInfo{};
    VkDescriptorImageInfo  albInfo{};
    VkDescriptorBufferInfo layerOnInfo{};
    VkDescriptorImageInfo  albOnInfo{};
    VkDescriptorBufferInfo hotInfo{};
    VkWriteDescriptorSet   wm[5]{};   // alpha-OFF (2/3) + alpha-ON (5/6) material writes
    uint32_t m = 0;
    if (s->wantMaterial && s->tier1 && s->layerBuf &&
        s->albedoTexture && s->albedoSamp &&
        s->layerOnBuf &&
        s->albedoOnTexture && s->albedoOnSamp) {
        layerInfo = VkDescriptorBufferInfo{ s->layerBuf.get(), 0, VK_WHOLE_SIZE };
        wm[m].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wm[m].dstSet = s->set[i];
        wm[m].dstBinding = 2; wm[m].descriptorCount = 1;
        wm[m].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wm[m].pBufferInfo = &layerInfo; ++m;
        albInfo.sampler = s->albedoSamp.get(); albInfo.imageView = s->albedoTexture.view();
        albInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        wm[m].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wm[m].dstSet = s->set[i];
        wm[m].dstBinding = 3; wm[m].descriptorCount = 1;
        wm[m].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wm[m].pImageInfo = &albInfo; ++m;
        // alpha-ON group: binding5 layer SSBO (VS) + binding6 albedo array (FS).
        layerOnInfo = VkDescriptorBufferInfo{ s->layerOnBuf.get(), 0, VK_WHOLE_SIZE };
        wm[m].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wm[m].dstSet = s->set[i];
        wm[m].dstBinding = 5; wm[m].descriptorCount = 1;
        wm[m].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wm[m].pBufferInfo = &layerOnInfo; ++m;
        albOnInfo.sampler = s->albedoOnSamp.get(); albOnInfo.imageView = s->albedoOnTexture.view();
        albOnInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        wm[m].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wm[m].dstSet = s->set[i];
        wm[m].dstBinding = 6; wm[m].descriptorCount = 1;
        wm[m].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wm[m].pImageInfo = &albOnInfo; ++m;
    }
    // Hot-colour table (binding 7, VS): declared in every tier-1 layout, bound to a one-entry placeholder until the real table lands.
    if (s->tier1 && s->hotBuf) {
        hotInfo = VkDescriptorBufferInfo{ s->hotBuf.get(), 0, VK_WHOLE_SIZE };
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set[i];
        w[n].dstBinding = 7; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[n].pBufferInfo = &hotInfo; ++n;
    }
    if (n) vkUpdateDescriptorSets(s->device, n, w, 0, nullptr);
    if (m) vkUpdateDescriptorSets(s->device, m, wm, 0, nullptr);
    }   // per-parity set loop
}

// ADJUDICATION-1 WASHOUT-RECON (extracted as a helper for VK-LIGHTFEED-DEDUP-1
// so it probes whichever bytes the VK vert shader will actually read this
// frame -- the shared ring copy's CPU source, or the island's own just-copied
// buffer): one-shot CPU-side replica of the vert shader's ambientTerm/sunColor
// scan. If this ever disagrees with a live GL-side dump of the SAME
// ObjectLights[idx] taken at GL's own draw time, the copy is stale/racing GL's
// write. Probes entries 0..7 plus the real per-instance lightDataIndex values
// actually used this frame.
void prop_light_vertex_probe(const uint8_t* base, VkDeviceSize lbytes,
                             const Mc2ColorPropFeed& feed) {
    if (!mc2::gate::get<"MC2_VK_PROP_LIGHT_VERTEX_PROBE">()) return;
    static bool s_vtx_dumped = false;
    if (s_vtx_dumped) return;
    s_vtx_dumped = true;
    const size_t stride = sizeof(GpuObjectLights);
    const size_t maxEntries = static_cast<size_t>(lbytes) / stride;
    auto scanEntry = [&](size_t idx) {
        if (idx >= maxEntries) return;
        const GpuObjectLights* ld = reinterpret_cast<const GpuObjectLights*>((base + idx * stride));
        float ambient[3] = {0, 0, 0};
        float sunColor[3] = {0, 0, 0};
        bool sunFound = false;
        int n = ld->numLights_;
        n = std::clamp(n, 0, 32);
        for (int i = 0; i < n; ++i) {
            int lt = static_cast<int>(ld->lightDir[i][3]);
            if (lt == 0) { // TG_LIGHT_AMBIENT
                ambient[0] += ld->lightColor[i][0];
                ambient[1] += ld->lightColor[i][1];
                ambient[2] += ld->lightColor[i][2];
            } else if (!sunFound && (lt == 1 || lt == 2)) {
                sunColor[0] = ld->lightColor[i][0];
                sunColor[1] = ld->lightColor[i][1];
                sunColor[2] = ld->lightColor[i][2];
                sunFound = true;
            }
        }
        std::fprintf(stderr,
            "[PROP_LIGHT_VERTEX_PROBE v1] idx=%zu numLights=%d "
            "ambient=(%.4f,%.4f,%.4f) sunColor=(%.4f,%.4f,%.4f) sunFound=%d\n",
            idx, n, ambient[0], ambient[1], ambient[2],
            sunColor[0], sunColor[1], sunColor[2], sunFound ? 1 : 0);
    };
    // Real per-instance lightDataIndex values actually used this
    // frame (first few real props), instead of a blind 0..7 scan.
    {
        const GpuStaticPropInstance* insts =
            reinterpret_cast<const GpuStaticPropInstance*>(feed.instances112);
        std::fprintf(stderr, "[PROP_LIGHT_VERTEX_PROBE v1] real inst.lightDataIndex sample: ");
        for (uint32_t ii = 0; ii < feed.count && ii < 12; ++ii) {
            std::fprintf(stderr, "[%u]=%u typeID=%u ", ii, insts[ii].lightDataIndex, insts[ii].typeID);
        }
        std::fprintf(stderr, "\n");
        std::fflush(stderr);
        // Scan the ACTUAL indices these instances use (dedup small set).
        uint32_t seen[12]; int seenCount = 0;
        for (uint32_t ii = 0; ii < feed.count && ii < 200; ++ii) {
            uint32_t li = insts[ii].lightDataIndex;
            bool dup = false;
            for (int k = 0; k < seenCount; ++k) if (seen[k] == li) { dup = true; break; }
            if (!dup && seenCount < 12) { seen[seenCount++] = li; scanEntry(static_cast<size_t>(li)); }
        }
    }
    for (size_t idx = 0; idx < maxEntries && idx < 8; ++idx) scanEntry(idx);
    std::fflush(stderr);
}

// VK-P11 STATIC-PROP MATERIAL ------------------------------------------------
// Copy-once import of the GL alpha-OFF albedo GL_TEXTURE_2D_ARRAY into a VK sampled
// array image (all layers, full mips), + a LINEAR_MIPMAP_LINEAR / CLAMP_TO_EDGE
// sampler matching the GL static-prop array. Generalizes the terrain island's
// single-layer/single-mip upload_sampled_image to arrayLayers=N + mipLevels=M:
// glGetTexImage(GL_TEXTURE_2D_ARRAY) / glGetCompressedTexImage return ALL layers of a
// level contiguously, so one vkCmdCopyBufferToImage per mip with layerCount=N copies
// the whole level. This is a ONCE-PER-MAP material import (like the terrain colormap),
// NOT the per-frame zero-copy color surface — so it does NOT touch g_forbidden.

VkCommandBuffer cp_begin_oneshot(ColorPropIsland* s) {
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = s->cpool.get(); cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(s->device, &cbi, &cb) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}
void cp_end_oneshot(ColorPropIsland* s, VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    mc2vk::submitAndWait(mc2vk::getOrCreateEngineVulkanContext(), si, 5ull * 1000 * 1000 * 1000, "prop copy-once");
    vkFreeCommandBuffers(s->device, s->cpool.get(), 1, &cb);
}

// VK-P11 STATIC-PROP MATERIAL: a single array-import target (alpha-OFF or alpha-ON).
// Both albedo groups share the same import code via this indirection.
struct AlbedoTarget {
    mc2vk::ResidentTextureRef* texture;
    mc2vk::OwnedSampler*   samp;
    uint32_t*      layers;
    uint32_t*      importedGlName;
    bool*          ready;
    const char*    tag;   // "alpha-OFF" / "alpha-ON" for logs
};
AlbedoTarget cp_target_off(ColorPropIsland* s) {
    return AlbedoTarget{ &s->albedoTexture, &s->albedoSamp,
                         &s->albedoLayers, &s->albedoGlTexImported, &s->materialReady, "alpha-OFF" };
}
AlbedoTarget cp_target_on(ColorPropIsland* s) {
    return AlbedoTarget{ &s->albedoOnTexture, &s->albedoOnSamp,
                         &s->albedoOnLayers, &s->albedoOnGlTexImported, &s->materialReadyOn, "alpha-ON" };
}

bool cp_make_albedo_sampler(ColorPropIsland* s, mc2vk::OwnedSampler* samp, uint32_t mipLevels) {
    if (*samp) return true;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;           // LINEAR_MIPMAP_LINEAR
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = VK_LOD_CLAMP_NONE; // image view bounds the immutable sampler to its supplied mip chain
    return mc2vk::create(*samp, s->device, [&](VkSampler* h) { return vkCreateSampler(s->device, &sci, nullptr, h); });
}

// Build a VK sampled 2D_ARRAY image from a contiguous byte blob laid out
// mip-major then layer-major-per-mip is NOT how GL returns it; GL returns, per mip
// level, all layers contiguously. `regions` carries one VkBufferImageCopy per mip
// (layerCount = layers, correct bufferOffset). Caller owns `bytes`.
bool cp_upload_albedo_array(ColorPropIsland* s, const AlbedoTarget& tgt,
                            VkFormat fmt, uint32_t w, uint32_t h,
                            uint32_t layers, uint32_t mips, bool compressed,
                            const void* bytes, VkDeviceSize byteCount,
                            const std::vector<VkBufferImageCopy>& regions) {
    RenderCore::KtxImage image; image.width=w; image.height=h; image.mipCount=mips;
    image.vkFormat=uint32_t(fmt); image.isCompressed=compressed; image.blockSizeBytes=compressed ? 16 : 0;
    image.pixels.assign(static_cast<const uint8_t*>(bytes),static_cast<const uint8_t*>(bytes)+byteCount);
    for (const auto& region : regions) image.mipByteOffsets.push_back(size_t(region.bufferOffset));
    uint64_t hash=1469598103934665603ull;
    for (uint8_t byte : image.pixels) { hash^=byte; hash*=1099511628211ull; }
    RenderCore::TextureKey key;
    key.source="prop-array:"+std::to_string(hash)+":"+std::to_string(w)+":"+std::to_string(h)+":"+std::to_string(layers);
    key.format=uint32_t(fmt); key.hints=mips>1 ? gosHint_MipmapFilter0 : 0;
    auto texture=mc2vk::getOrCreateEngineVulkanContext().textures().acquire(key,image,{w,h,w,h},layers,true);
    if (!texture) return false;
    if (tgt.texture == &s->albedoOnTexture) {
        // Bytes plus their interpretation identify the actual alpha-ON upload across installs/mods.
        const uint32_t shape[] = {uint32_t(fmt), w, h, layers, mips};
        for (uint32_t v : shape) { hash ^= v; hash *= 1099511628211ull; }
        s->alphaContentHash = hash;
    }
    *tgt.texture=std::move(texture); *tgt.layers=layers;
    return true;
}

// Bind a 1x1 opaque-magenta placeholder array so binding3 is a VALID descriptor even
// when the real albedo import fails/isn't ready. materialReady stays false -> the FS
// keeps flat teal (never samples the dummy).
bool cp_ensure_dummy_albedo(ColorPropIsland* s, const AlbedoTarget& tgt) {
    if (!cp_make_albedo_sampler(s, tgt.samp, 1)) return false;
    if (*tgt.texture) return true;
    const uint8_t px[4] = {255, 0, 255, 255};
    std::vector<VkBufferImageCopy> rgn(1);
    rgn[0].bufferOffset = 0;
    rgn[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rgn[0].imageExtent = {1, 1, 1};
    return cp_upload_albedo_array(s, tgt, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, false,
                                  px, sizeof(px), rgn);
}

// Copy-once import of one albedo group array from the batcher's CPU copy (every level, as GL uploaded it). `changeKey` is the GL
// array's name, used only to notice a rebuilt array (a new map). Sets *tgt.ready on success; a malformed copy is rejected with a log
// line and leaves the dummy bound (the FS keeps teal). Returns true if a NEW import landed.
bool cp_import_albedo_array(ColorPropIsland* s, const AlbedoTarget& tgt, uint32_t changeKey, const mc2tex::TextureArrayCpu* a) {
    if (changeKey == 0 || !a) return false;
    if (changeKey == *tgt.importedGlName && *tgt.ready) return false; // already
    const bool bc7 = a->vkFormat == mc2tex::kVkFormatBc7Unorm || a->vkFormat == mc2tex::kVkFormatBc7Srgb;
    if (a->width == 0 || a->height == 0 || a->layers == 0 || a->levels.empty() || (!bc7 && a->vkFormat != mc2tex::kVkFormatRgba8Unorm)) {
        vlog("%s albedo array rejected: %ux%u layers=%u levels=%zu format=%u.", tgt.tag, a->width, a->height, a->layers, a->levels.size(), a->vkFormat);
        return false;
    }
    const uint32_t mips = static_cast<uint32_t>(a->levels.size());
    std::vector<uint8_t> blob;
    std::vector<VkBufferImageCopy> regions;
    regions.reserve(mips);
    for (uint32_t m = 0; m < mips; ++m) {
        const size_t want = mc2tex::textureArrayLevelBytes(*a, m);
        if (want == 0 || a->levels[m].size() != want) {
            vlog("%s albedo array rejected: level %u holds %zu bytes, %zu expected.", tgt.tag, m, a->levels[m].size(), want);
            return false;
        }
        VkBufferImageCopy r{};
        r.bufferOffset = blob.size();
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, a->layers};
        r.imageExtent = {mc2tex::mipExtent(a->width, m), mc2tex::mipExtent(a->height, m), 1};
        regions.push_back(r);
        blob.insert(blob.end(), a->levels[m].begin(), a->levels[m].end());
    }
    const VkFormat vkFmt = static_cast<VkFormat>(a->vkFormat);

    // (Re)create sampler with the real mip count.
    if (!cp_make_albedo_sampler(s, tgt.samp, mips)) return false;

    if (!cp_upload_albedo_array(s, tgt, vkFmt, a->width, a->height, a->layers,
                                mips, bc7, blob.data(), static_cast<VkDeviceSize>(blob.size()), regions)) {
        return false;
    }
    *tgt.importedGlName = changeKey;
    *tgt.ready = true;
    vlog("%s albedo 2D_ARRAY imported: %ux%u layers=%u mips=%u fmt=%s (%zu B).",
         tgt.tag, a->width, a->height, a->layers, mips, bc7 ? "BC7" : "RGBA8", blob.size());
    return true;
}

// Upload the per-type albedo-layer table (int32 per type) into a layer SSBO
// (alpha-OFF binding2, or alpha-ON binding5).
bool cp_upload_layer_table_into(ColorPropIsland* s, mc2vk::OwnedBuffer& buf,
                                VkDeviceSize* capBytes, const int32_t* layers, uint32_t count) {
    const VkDeviceSize bytes = static_cast<VkDeviceSize>((count ? count : 1)) * sizeof(int32_t);
    if (buf && bytes > *capBytes) {
        mc2vk::getOrCreateEngineVulkanContext().waitIdle("color_prop_layer_grow");   // the old table may still be read by an in-flight submit
        buf.reset();
    }
    if (!buf) {
        if (!make_buffer(s, buf, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, "colorprop.layerTable"))
            return false;
        *capBytes = bytes;
    }
    void* p = buf.map();
    if (!p) return false;
    if (count && layers) std::memcpy(p, layers, static_cast<size_t>(count) * sizeof(int32_t));
    else { int32_t neg = -1; std::memcpy(p, &neg, sizeof(neg)); }
    buf.unmap();
    return true;
}
bool cp_upload_layer_table(ColorPropIsland* s, const int32_t* layers, uint32_t count) {
    return cp_upload_layer_table_into(s, s->layerBuf, &s->layerCapBytes, layers, count);
}
bool cp_upload_layer_table_on(ColorPropIsland* s, const int32_t* layers, uint32_t count) {
    return cp_upload_layer_table_into(s, s->layerOnBuf, &s->layerOnCapBytes, layers, count);
}

// Uploads the per-type hot-colour table (12 floats per type); a null table with count 0 binds one zeroed entry, so the descriptor is always valid and an
// unfed type simply has no night emission.
bool cp_upload_hot_table(ColorPropIsland* s, std::span<const float> colours) {
    const size_t n = colours.empty() ? 12u : colours.size();   // one zeroed entry when there is no table
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(n * sizeof(float));
    bool rebound = false;
    if (s->hotBuf && bytes > s->hotCapBytes) {
        // The placeholder grows into the real table on the first upload (a one-time stall); the old table may still be read by an in-flight submit.
        mc2vk::getOrCreateEngineVulkanContext().waitIdle("color_prop_hot_grow");
        s->hotBuf.reset();
    }
    if (!s->hotBuf) {
        if (!make_buffer(s, s->hotBuf, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, "colorprop.hotColors"))
            return false;
        s->hotCapBytes = bytes;
        rebound = true;
    }
    void* p = s->hotBuf.map();
    if (!p) return false;
    if (!colours.empty()) std::memcpy(p, colours.data(), static_cast<size_t>(bytes));
    else std::memset(p, 0, static_cast<size_t>(bytes));
    s->hotBuf.unmap();
    if (rebound && s->inited) write_descriptors(s);
    return true;
}

// One-time init.
bool init_island(ColorPropIsland* s, int w, int h) {
    s->w = w; s->h = h;
    ++g_health.attempted;

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

    // Tier-1 is the only live Vulkan prop color path.

    // VK-RUNG8-ITEM5: fire the one-shot MC2_VK_PROP_DIRECT_CHAIN deprecation note if
    // the retired gate was explicitly set (result ignored — always direct-chain-native).
    (void)propDirectChainRequested();

    s->tier1 = true;
    s->wantMaterial = true;
    // VK-PROP-AMBIENT-IBL-1: real ambient/IBL feed also requires Tier-1.
    // Not a separate opt-in gate --
    // this closes a documented parity gap in the same tier the washout fix landed in.
    s->wantLighting = s->tier1;
    if (!s->tier1) { setFallback("gate_off"); return false; }

    if (s->tier1) {
        // ---- TIER-1: fetch the SHARED scene color/normal/depth exportable images the
        // GL scene renders terrain into (built by the postprocess seams; the island
        // co-writes props into these SAME images in-place). Fail-soft: any missing =>
        // GL fallback (the caller runs the GL prop flush). ----
        s->t1_colorRes  = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
        s->t1_normalRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_NORMAL);
        s->t1_depthRes  = mc2SharedSceneDepthGetResource();
        // VK-RUNG8-ITEM5: direct-chain-native only. The shared GBuffer is VK-only and
        // left Vulkan-owned by the terrain first-owner; only the VkImageResources are
        // required (no GL interop view, no GL<->VK semaphore handoff).
        if (!s->t1_colorRes || !s->t1_normalRes || !s->t1_depthRes) {
            setFallback("tier1_shared_gbuffer_not_ready");
            return false;
        }
        // VK-FIELD-ORACLE-1 / crash-C0000005-at-0x20 FIX: the pointers above can be
        // non-null while the underlying shared images have NOT been created yet (the
        // struct exists but view==VK_NULL_HANDLE, format==VK_FORMAT_UNDEFINED). That
        // happens at frame 1 in the field env (boot/warmup race): the postprocess
        // seam allocates the shared color/normal/depth images a few frames after the
        // renderLists prop pass first fires. Building the renderpass with an UNDEFINED
        // format + a framebuffer with NULL views yields a malformed pipeline that the
        // AMD ICD dereferences (crash at vkCreateGraphicsPipelines), and the
        // non-crashing variant (stale/empty target) is exactly why props were
        // INVISIBLE under the field env. Decline until every shared attachment is
        // fully realized; run() leaves g_island null so we retry next frame (the
        // established decline+retry pattern), and once ready we init against VALID
        // views so props actually co-write into the composited shared color.
        {
            VkImageResource* cR = reinterpret_cast<VkImageResource*>(s->t1_colorRes);
            VkImageResource* nR = reinterpret_cast<VkImageResource*>(s->t1_normalRes);
            VkImageResource* dR = reinterpret_cast<VkImageResource*>(s->t1_depthRes);
            const bool ready =
                cR->view.get() != VK_NULL_HANDLE && cR->format != VK_FORMAT_UNDEFINED &&
                nR->view.get() != VK_NULL_HANDLE && nR->format != VK_FORMAT_UNDEFINED &&
                dR->view.get() != VK_NULL_HANDLE && dR->format != VK_FORMAT_UNDEFINED &&
                cR->w > 0 && cR->h > 0;
            if (!ready) {
                setFallback("tier1_shared_gbuffer_images_not_realized");
                return false;
            }
            // The shared images must also match the size the caller is rendering at;
            // building the framebuffer at a stale extent would render props into the
            // wrong region (or off the realized image). run() rebuilds on w/h change,
            // but a resource re-realized at a new size mid-session must also force a
            // fresh init -- decline here and let the next frame re-init cleanly.
            if (static_cast<int>(cR->w) != w || static_cast<int>(cR->h) != h) {
                setFallback("tier1_shared_gbuffer_size_mismatch");
                return false;
            }
        }
        s->useInterop = false;
        g_health.interopActive = 0;
        g_health.vk2glSemActive = 0;
        vlog("TIER-1 shared-GBuffer co-write ACTIVE — in-place RGBA16F color(MRT0)+normal(MRT1) "
             "+ shared D32S8 depth (direct-chain native, no interop).");
    }
    if (s->tier1) {
        VkImageResource* colorRes  = reinterpret_cast<VkImageResource*>(s->t1_colorRes);
        VkImageResource* normalRes = reinterpret_cast<VkImageResource*>(s->t1_normalRes);
        VkImageResource* depthRes  = reinterpret_cast<VkImageResource*>(s->t1_depthRes);

        VkAttachmentDescription att[3]{};
        // color0 (MRT0) — LOAD GL terrain color, STORE result.
        att[0].format = colorRes->format; att[0].samples = VK_SAMPLE_COUNT_1_BIT;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[0].finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        // color1 (MRT1 GBuffer1) — LOAD GL terrain normals (respect clearGBuffer1), STORE.
        att[1].format = normalRes->format; att[1].samples = VK_SAMPLE_COUNT_1_BIT;
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[1].finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        // depth (shared D32S8) — LOAD GL scene depth, TEST+WRITE (GEQUAL), STORE so GL
        // (and post passes) see the prop-updated depth. Stencil LOAD/STORE preserved.
        att[2].format = depthRes->format; att[2].samples = VK_SAMPLE_COUNT_1_BIT;
        att[2].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        att[2].finalLayout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorRefs[2] = {
            {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
            {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        };
        VkAttachmentReference depthRef{2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 2; sub.pColorAttachments = colorRefs;
        sub.pDepthStencilAttachment = &depthRef;

        // Cross-submit WAW/hand-off deps shape (GL semaphore provides
        // the actual ordering; these let sync-val see the LOAD-preserve co-write).
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
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        deps[1].dstAccessMask = 0;

        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 3; rpci.pAttachments = att;
        rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        if (!mc2vk::create(s->t1_rpass, s->device, [&](VkRenderPass* h) { return vkCreateRenderPass(s->device, &rpci, nullptr, h); })) {
            vlog("TIER-1 vkCreateRenderPass failed."); setFallback("tier1_renderpass_failed"); return false;
        }
        VkImageView fbViews[3] = { colorRes->view.get(), normalRes->view.get(), depthRes->view.get() };
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->t1_rpass.get(); fci.attachmentCount = 3; fci.pAttachments = fbViews;
        fci.width = static_cast<uint32_t>(w); fci.height = static_cast<uint32_t>(h); fci.layers = 1;
        if (!mc2vk::create(s->t1_fb, s->device, [&](VkFramebuffer* h) { return vkCreateFramebuffer(s->device, &fci, nullptr, h); })) {
            vlog("TIER-1 vkCreateFramebuffer failed."); setFallback("tier1_framebuffer_failed"); return false;
        }
    }

    // ---- Descriptor layout/pool/set (UBO binding0 VS, instance SSBO binding1 VS).
    // VK-P11 STATIC-PROP MATERIAL (tier1 only): binding2 = per-type albedo-layer SSBO
    // (VS), binding3 = albedo sampler2DArray (FS). Declared on the tier1 layout even
    // when material isn't ready (dummy layer SSBO + dummy 1x1 array bound) so the
    // Tier-1 shaders can always reference them; the shader gates real sampling on
    // uFrame.materialReady. base descriptor set keeps exactly 2 bindings when optional feeds are off. ----
    const bool materialBindings = s->tier1 && s->wantMaterial;
    // VK-PROP-AMBIENT-IBL-1: binding4 LightsData SSBO, declared whenever tier1 (not
    // gated on wantMaterial -- ambient/IBL parity is independent of the albedo import).
    const bool lightBinding = s->tier1 && s->wantLighting;
    {
        // Base bindings 0(UBO),1(inst SSBO); optional +4(LightsData SSBO, VS). Material
        // (tier1 + MC2_VK_PROP_MATERIAL): +2(alpha-OFF layer SSBO VS), 3(alpha-OFF albedo
        // FS), 5(alpha-ON layer SSBO VS), 6(alpha-ON albedo FS).
        VkDescriptorSetLayoutBinding b[10]{};
        uint32_t nb = 0;
        b[nb].binding = 0; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b[nb].descriptorCount = 1; b[nb].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; ++nb;
        b[nb].binding = 1; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        b[nb].descriptorCount = 1; b[nb].stageFlags = VK_SHADER_STAGE_VERTEX_BIT; ++nb;
        if (materialBindings) {
            b[nb].binding = 2; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[nb].descriptorCount = 1; b[nb].stageFlags = VK_SHADER_STAGE_VERTEX_BIT; ++nb;
            b[nb].binding = 3; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[nb].descriptorCount = 1; b[nb].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; ++nb;
        }
        if (lightBinding) {
            b[nb].binding = 4; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            b[nb].descriptorCount = 1; b[nb].stageFlags = VK_SHADER_STAGE_VERTEX_BIT; ++nb;
        }
        if (materialBindings) {
            b[nb].binding = 5; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[nb].descriptorCount = 1; b[nb].stageFlags = VK_SHADER_STAGE_VERTEX_BIT; ++nb;
            b[nb].binding = 6; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[nb].descriptorCount = 1; b[nb].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; ++nb;
        }
        if (s->tier1) {   // the hot-colour table is read by the tier-1 vertex shader in every mode
            b[nb].binding = 7; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[nb].descriptorCount = 1; b[nb].stageFlags = VK_SHADER_STAGE_VERTEX_BIT; ++nb;
            b[nb].binding = 8; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;   // night light pools
            b[nb].descriptorCount = 1; b[nb].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; ++nb;
        }
        b[nb].binding = 9; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[nb].descriptorCount = 1; b[nb].stageFlags = VK_SHADER_STAGE_VERTEX_BIT; ++nb;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = nb; lci.pBindings = b;
        if (!mc2vk::create(s->dsl, s->device, [&](VkDescriptorSetLayout* h) { return vkCreateDescriptorSetLayout(s->device, &lci, nullptr, h); })) {
            vlog("vkCreateDescriptorSetLayout failed."); return false;
        }
        // VK-PROP-FENCE-DEFER-1: TWO sets (one per parity slot), so pool sizes double.
        // Dynamic: 0 UBO; 1 + 4 (lights) + 8 (pools) SSBO. Static SSBOs: layer tables (2, 5) and the hot-colour table (7).
        VkDescriptorPoolSize sizes[4]{};
        uint32_t nsz = 0;
        sizes[nsz++] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2};
        sizes[nsz++] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 2u * (1u + (lightBinding ? 1u : 0u) + (s->tier1 ? 1u : 0u))};
        sizes[nsz++] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2u * ((materialBindings ? 2u : 0u) + (s->tier1 ? 1u : 0u) + 1u)};
        if (materialBindings) sizes[nsz++] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4u};
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 2; pci.poolSizeCount = nsz; pci.pPoolSizes = sizes;
        if (!mc2vk::create(s->pool, s->device, [&](VkDescriptorPool* h) { return vkCreateDescriptorPool(s->device, &pci, nullptr, h); })) {
            vlog("vkCreateDescriptorPool failed."); return false;
        }
        VkDescriptorSetLayout layouts[2] = { s->dsl.get(), s->dsl.get() };
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = s->pool.get(); dsai.descriptorSetCount = 2; dsai.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(s->device, &dsai, s->set) != VK_SUCCESS) {
            vlog("vkAllocateDescriptorSets failed."); return false;
        }
        if (s->tier1 && !cp_upload_hot_table(s, {})) { setFallback("hot_buf_failed"); return false; }
        write_descriptors(s);
    }

    // ---- TIER-1 Pipeline: same 40B vertex stream, but emits TWO GBuffer outputs
    // (loc0 lit color, loc1 GBuffer1 normal+0.25), reverse-Z GEQUAL depth-test+WRITE,
    // cull NONE, and NO blend on either attachment (opaque in-place overwrite — a
    // winning prop fragment fully replaces the terrain GBuffer pixel, matching GL). ----
    if (s->tier1) {
        s->t1_vert = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "color_prop_tier1.vert.spv"));
        s->t1_frag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "color_prop_tier1.frag.spv"));
        if (!s->t1_vert || !s->t1_frag) { setFallback("tier1_spv_load_failed"); return false; }

        // VK-FLIP-FIELD-REPORT-2 BLACK-FENCE FIX: per-PACKET albedo layer push constant
        // (2x int32 = layerOff, layerOn). GL's coalesce path samples texArrayLayer per
        // DRAW (PerDrawEntry), not per typeID — a multi-packet type (e.g. a fence type
        // with separate "post"/"arch" sub-mesh packets) can have DIFFERENT textures per
        // packet. The prior per-type SSBO lookup (by inst.typeID) forced every packet
        // of a type to reuse one ("first-valid-wins") layer, so non-first packets
        // sampled the WRONG BC7 layer/UV combination — this is what produced solid-black
        // fragments (wrong layer decodes to a black margin block, alpha>=0.5 so no
        // alpha-test discard masked it). Fixed here: each vkCmdDrawIndexed for a packet
        // now pushes THAT packet's own layer, consumed directly by vert/frag instead of
        // the SSBO-by-typeID path.
        // FENCE-UVSCALE-1: widened 8 -> 16 bytes (2x int32 layer + 2x float
        // uvScale). Well inside the Vulkan-guaranteed minimum
        // maxPushConstantsSize of 128 bytes (spec table 32.1) -- no need to
        // pack into half floats or move to the per-instance SSBO path.
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcr.offset = 0; pcr.size = 28; // int32 packetLayerOff, int32 packetLayerOn, float uvScaleX, float uvScaleY, int32 packetAlphaTest,
                                       // int32 coverageFringePass (PROP-COVERAGE-1)

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        const VkDescriptorSetLayout dslHandle = s->dsl.get();
        plci.setLayoutCount = 1; plci.pSetLayouts = &dslHandle;
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if (!mc2vk::create(s->playout, s->device, [&](VkPipelineLayout* h) { return vkCreatePipelineLayout(s->device, &plci, nullptr, h); })) {
            vlog("TIER-1 vkCreatePipelineLayout failed."); return false;
        }

        VkVertexInputBindingDescription vib{};
        vib.binding   = 0;
        // VK-FIELD-ORACLE-1 PROPS-INVISIBLE ROOT CAUSE: vib.stride was left 0.
        // With stride 0 every vertex index fetches byte offset 0 (vertex 0), so
        // ALL triangles collapse to a degenerate point -> the prop draws execute
        // (used_vulkan>0, 991 instances, 69 typeRuns) but rasterize ZERO fragments
        // -> props invisible under BOTH the field and strict VK envs, while the
        // shadow island (its OWN correctly-strided pipeline + light-view transform)
        // still cast prop shadows -- which is exactly the "shadows visible, geometry
        // absent" symptom. The shared prop geometry feed is a tightly-packed 40-byte
        // vertex (pos vec3 @0, normal vec3 @12, uv vec2 @24, colorSeed u32 @32,
        // typeAux u32 @36 == 40B), matching the hardcoded attribute offsets below
        // and the feed's stride=40 (asserted post-upload in upload_geometry_once).
        vib.stride    = mc2_static_prop_vertex_stride();  // batcher's kVertexStride (pipeline is baked before upload)
        vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription via[5]{};
        via[0].location = 0; via[0].binding = 0; via[0].format = VK_FORMAT_R32G32B32_SFLOAT; via[0].offset = 0;
        via[1].location = 1; via[1].binding = 0; via[1].format = VK_FORMAT_R32G32B32_SFLOAT; via[1].offset = 12;
        via[2].location = 2; via[2].binding = 0; via[2].format = VK_FORMAT_R32G32_SFLOAT;    via[2].offset = 24;
        via[3].location = 3; via[3].binding = 0; via[3].format = VK_FORMAT_R32_UINT;         via[3].offset = 32;
        via[4].location = 4; via[4].binding = 0; via[4].format = VK_FORMAT_R32_UINT;         via[4].offset = 36;
        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vib;
        vin.vertexAttributeDescriptionCount = 5; vin.pVertexAttributeDescriptions = via;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1; vps.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        // GL's StaticPropOpaque / StaticPropAlphaTest pipelines cull back faces (CCW front); the props were drawn double-sided here, so faces GL
        // culls (the inner panels of the hex lattice props) covered the faces behind them. The island projects with GL's
        // clip matrix into an unflipped viewport, which mirrors window y, so GL's CCW front arrives here clockwise.
        rs.cullMode = VK_CULL_MODE_BACK_BIT;
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Reverse-Z GEQUAL, test+WRITE on (matches GL StaticPropOpaque; props occlude
        // vs the SHARED scene depth GL authored — the point of Tier-1).
        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

        // NO blend on EITHER attachment: opaque in-place overwrite. GBuffer1 is NOT
        // alpha-blendable (premult-over would corrupt the normal+class encoding), and
        // the color pass is opaque, so a winning fragment writes rgba directly.
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
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = s->t1_vert.get(); stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = s->t1_frag.get(); stages[1].pName = "main";
        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2; gp.pStages = stages;
        gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds; gp.pColorBlendState = &cb;
        gp.pDynamicState = &dsci;
        gp.layout = s->playout.get(); gp.renderPass = s->t1_rpass.get(); gp.subpass = 0;
        if (!mc2vk::create(s->t1_pipe, s->device, [&](VkPipeline* h) { return vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h); })) {
            vlog("TIER-1 vkCreateGraphicsPipelines failed."); setFallback("tier1_pipeline_failed"); return false;
        }
        mc2vk::nameObject(s->device, s->t1_pipe.get(), "colorprop.tier1Pipe");

        // PROP-COVERAGE-1 fringe pipeline: the second pass over the alpha-tested packets that
        // blends the coverage the core pass cut (see color_prop_tier1.frag). Same shaders and
        // layout; depth TEST only (a fringe never hides what is behind it), MRT0 straight-alpha
        // over with the destination alpha kept, MRT1 (normal + shadow class, not blendable)
        // write-masked. Per-attachment state relies on independentBlend, which the device
        // profile requires.
        ds.depthWriteEnable = VK_FALSE;
        cba[0].blendEnable = VK_TRUE;
        cba[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba[0].colorBlendOp = VK_BLEND_OP_ADD;
        cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba[0].alphaBlendOp = VK_BLEND_OP_ADD;
        cba[1].blendEnable = VK_FALSE;
        cba[1].colorWriteMask = 0;
        if (!mc2vk::create(s->t1_fringePipe, s->device, [&](VkPipeline* h) { return vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h); })) {
            vlog("TIER-1 fringe vkCreateGraphicsPipelines failed."); setFallback("tier1_pipeline_failed"); return false;
        }
        mc2vk::nameObject(s->device, s->t1_fringePipe.get(), "colorprop.tier1FringePipe");
    }

    // ---- Command pool + 2 command buffers (per parity). ----
    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s->gfxFamily;
        if (!mc2vk::create(s->cpool, s->device, [&](VkCommandPool* h) { return vkCreateCommandPool(s->device, &pci, nullptr, h); })) {
            vlog("vkCreateCommandPool failed."); return false;
        }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = s->cpool.get(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 2;
        if (vkAllocateCommandBuffers(s->device, &cai, s->cbuf) != VK_SUCCESS) {
            vlog("vkAllocateCommandBuffers failed."); return false;
        }
    }

    // VK-P11 STATIC-PROP MATERIAL: bind a valid dummy albedo array + layer SSBO now
    // (needs the command pool above for the one-shot upload) so binding3 is a valid
    // descriptor before the first frame. The REAL array is imported once geometry is
    // finalized (cp_import_albedo_array in run()); materialReady gates FS sampling.
    if (s->wantMaterial && s->tier1) {
        if (!cp_ensure_dummy_albedo(s, cp_target_off(s))) { setFallback("material_dummy_failed"); return false; }
        // VK-P11 STATIC-PROP MATERIAL alpha-ON: dummy alpha-ON array + layer SSBO so
        // bindings 5+6 are valid before the first frame (materialReadyOn gates sampling).
        if (!cp_ensure_dummy_albedo(s, cp_target_on(s))) { setFallback("material_on_dummy_failed"); return false; }
        int32_t neg = -1;
        if (!cp_upload_layer_table(s, &neg, 1))    { setFallback("material_layer_buf_failed"); return false; }
        if (!cp_upload_layer_table_on(s, &neg, 1)) { setFallback("material_on_layer_buf_failed"); return false; }
        write_descriptors(s);  // now writes bindings 2/3 (alpha-OFF) + 5/6 (alpha-ON) dummies too
    }

    s->inited = true;
    setFallback("");
    vlog("island initialized %dx%d (borrowed shared device; Tier-1 shared-GBuffer prop co-write).", w, h);
    return true;
}

// Copy the shared static-prop VBO/IBO into VK-owned buffers ONCE per map, reusing the
// shadow island's geometry feed (SAME shared buffers). Returns false if unavailable.
bool upload_geometry_once(ColorPropIsland* s) {
    if (s->geomUploaded) return true;
    Mc2ShadowGeomFeed feed{};
    if (!mc2_shadow_island_get_geometry_feed(&feed)) return false;
    // The vertex / index bytes come from the batcher's CPU copy (the bytes the GL buffers were created from).
    const void* vboCpu = nullptr; const void* iboCpu = nullptr;
    uint64_t vboBytes = 0, iboBytes = 0;
    if (!mc2_static_prop_geometry_cpu(&vboCpu, &vboBytes, &iboCpu, &iboBytes)) return false;
    // The accessor never reports empty staging; a size that differs from the feed means the two describe different maps.
    if (vboBytes != feed.vboBytes || iboBytes != feed.iboBytes) {
        vlog("geometry declined: the CPU copy (%llu / %llu bytes) does not match the feed (%llu / %llu).",
             static_cast<unsigned long long>(vboBytes), static_cast<unsigned long long>(iboBytes), static_cast<unsigned long long>(feed.vboBytes),
             static_cast<unsigned long long>(feed.iboBytes));
        return false;
    }
    // A failed copy leaves both buffers empty, so the next frame's retry starts from nothing.
    if (!s->vbo.createFilled(s->allocator, s->device, vboCpu, vboBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "colorprop.vbo") ||
        !s->ibo.createFilled(s->allocator, s->device, iboCpu, iboBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "colorprop.ibo")) {
        vlog("geometry upload failed (buffer creation or map); retrying next frame.");
        s->vbo.reset();
        return false;
    }

    s->packets.assign(feed.packets, feed.packets + feed.packetCount);
    s->typeRuns.clear();
    s->typeRuns.resize(feed.typeCount);
    for (uint32_t t = 0; t < feed.typeCount; ++t) {
        s->typeRuns[t].typeID      = t;
        s->typeRuns[t].firstPacket = feed.typeFirstPacket[t];
        s->typeRuns[t].packetCount = feed.typePacketCount[t];
    }
    g_health.geomVerts = static_cast<unsigned long>((feed.vboBytes / (feed.vertexStride ? feed.vertexStride : 1)));
    // VK-FIELD-ORACLE-1: the color pipeline hardcodes a 40-byte vertex binding
    // stride (init_island via[]/vib.stride). If the shared geometry feed ever
    // changes its packed stride, the pipeline's fetch would misalign and props
    // would garble/vanish -- surface it loudly rather than silently.
    if (feed.vertexStride != mc2_static_prop_vertex_stride()) {
        vlog("ERROR: prop geometry stride=%u != pipeline binding stride %u -- "
             "props will misrender; update vib.stride/attributes in init_island.",
             feed.vertexStride, mc2_static_prop_vertex_stride());
    }
    s->geomUploaded = true;
    vlog("prop geometry uploaded once: vbo=%llu B ibo=%llu B stride=%u packets=%u types=%u.",
         static_cast<unsigned long long>(feed.vboBytes), static_cast<unsigned long long>(feed.iboBytes),
         feed.vertexStride, feed.packetCount, feed.typeCount);
    return true;
}

// currently-bound draw FBO (MainColor at flush() time). Reused water-island shape.
extern "C" void mc2_vk_color_prop_island_invalidate_geometry() {
    ColorPropIsland* s = parkedIsland();
    if (!s) return;
    mc2vk::getOrCreateEngineVulkanContext().waitIdle("color_prop_invalidate_geometry");
    s->vbo.reset();
    s->ibo.reset();
    s->geomUploaded = false;
    s->packets.clear();
    s->typeRuns.clear();
    // The albedo arrays and the hot-colour table belong to the map too: the next map's feed imports its own (the island itself outlives the map).
    // The previous map's two albedo arrays stay allocated until that import replaces them: the per-parity descriptor sets still name their views, and a set
    // naming a destroyed view must not be bound by a draw before the rewrite, so the GPU memory is retained across a menu stay rather than released here.
    s->materialReady = false;
    s->materialReadyOn = false;
    s->hotReady = false;
    ++s->mapGeneration;
}

extern "C" uint64_t mc2_color_prop_island_alpha_content_hash() {
    const auto* s = parkedIsland();
    return s && s->materialReadyOn ? s->alphaContentHash : 0;
}

extern "C" int mc2_color_prop_island_get_alpha_array(void** view, uint32_t* generation) {
    const ColorPropIsland* s = parkedIsland();
    if (!view || !generation || !s || !s->materialReadyOn || !s->albedoOnTexture) return 0;
    if (!mc2vk::getOrCreateEngineVulkanContext().textures().ready(s->albedoOnTexture.get())) return 0;
    *view = reinterpret_cast<void*>(s->albedoOnTexture.view());
    *generation = s->mapGeneration;
    return 1;
}

extern "C" int mc2_vk_color_prop_island_enabled() {
    return 1;
}

extern "C" int mc2_vk_color_prop_island_run(int w, int h) {
    ZoneScopedN("VK-props-island");   // VK-FLIP-FIELD-REPORT-1 coarse region zone
    if (w <= 0 || h <= 0) return 0;

    using clk = std::chrono::steady_clock;
    auto usSince = [](clk::time_point t0){ return std::chrono::duration<double, std::micro>(clk::now() - t0).count(); };
    const clk::time_point tTotal0 = clk::now();
    const bool perf = propPerfGateRequested();
    double us_fence_wait = 0.0;   // deferred retire (beginFrame wait) OR inline wait under the killswitch

    // Pull the per-frame color instance feed (visible buckets + world->clip + bias).
    Mc2ColorPropFeed feed{};
    if (!mc2_color_prop_island_get_frame_feed(&feed)) { setFallback("no_color_feed"); return 0; }
    if (!feed.instances112 || feed.count == 0) { setFallback("no_color_instances"); return 0; }
    // MC2_VK_SHADOW_MATRIX_DIAG: the object matrix the props are projected with next to the live camera matrix, so a light-space leak into the colour pass shows.


    // Lazy init / rebuild on size change.
    mc2vk::VulkanContext& slotCtx = mc2vk::getOrCreateEngineVulkanContext();
    if (const ColorPropIsland* parked = parkedIsland(); parked && (parked->w != w || parked->h != h)) slotCtx.retireIsland(mc2vk::IslandSlot::ColorProp);
    if (!islandSlot()) {
        auto fresh = std::make_unique<ColorPropIsland>();
        if (!init_island(fresh.get(), w, h)) {
            vlog("init failed (%s) — GL fallback.", g_health.fallbackReason);
            slotCtx.waitIdle("color_prop_init_failure");   // objects made before the failure may have been submitted
            return 0;
        }
        islandSlot() = std::move(fresh);
    }
    ColorPropIsland* s = parkedIsland();
    // VK-OWNERSHIP-GAP-FIX (item-4 correctness): direct-chain sets useInterop=false
    // (init:781) but the old guard '|| !s->useInterop' then bailed here, so under the
    // strict umbrella the island returned 0 and GL silently drew props into the
    // VK-owned GBuffer (used_vulkan=0). Enter the native render path when direct-chain
    // OR the hybrid interop arm is armed -- mirrors mech/terrain (a0cba0c4/1f4a1a5a)
    // whose guard is 'disabled || !inited' with directChain unconditional-native.
    if (s->disabled || !s->inited) return 0;  // VK-RUNG8-ITEM5: direct-chain-native only

    if (!upload_geometry_once(s)) { setFallback("geometry_not_ready"); return 0; }

    // VK-P11 STATIC-PROP MATERIAL: copy-once import the alpha-OFF albedo 2D_ARRAY +
    // per-type layer table once geometry is finalized. Fail-soft: any miss (feed not
    // ready, BC7-bucketed, import failure) leaves materialReady=false -> FS keeps teal.
    if (s->wantMaterial && s->tier1 && (!s->materialReady || !s->materialReadyOn)) {
        Mc2ColorPropMaterialFeed mf{};
        if (mc2_color_prop_island_get_material_feed(&mf) && mf.arrayKeyOff != 0 && !mf.bucketed) {
            s->terrainSunMC2[0] = mf.terrainSunMC2[0];
            s->terrainSunMC2[1] = mf.terrainSunMC2[1];
            s->terrainSunMC2[2] = mf.terrainSunMC2[2];
            bool changed = false;
            // The night emission colours of the type table, once per map (like the material arrays).
            if (!s->hotReady && mf.typeHotColors && mf.typeCount > 0) {
                if (cp_upload_hot_table(s, std::span<const float>(mf.typeHotColors, static_cast<size_t>(mf.typeCount) * 12u))) {
                    s->hotReady = true;
                    changed = true;
                    vlog("hot-colour table active: %u types.", mf.typeCount);
                }
            }
            // ---- alpha-OFF (class 0): textured layer per type -> s_texArrayOff. ----
            if (!s->materialReady) {
                std::vector<int32_t> layerTbl(mf.typeCount, -1);
                for (uint32_t t = 0; t < mf.typeCount; ++t) {
                    if (mf.typeAlphaClass[t] == 0 && mf.typeAlbedoLayer[t] >= 0)
                        layerTbl[t] = mf.typeAlbedoLayer[t];
                }
                if (cp_import_albedo_array(s, cp_target_off(s), mf.arrayKeyOff, mf.albedoOff)) {
                    cp_upload_layer_table(s, layerTbl.data(), mf.typeCount);
                    changed = true;
                    vlog("material feed active: alpha-OFF %u types textured (sun=%.2f,%.2f,%.2f).",
                         mf.typeCount, s->terrainSunMC2[0], s->terrainSunMC2[1], s->terrainSunMC2[2]);
                }
            }
            // VK-FLIP-FIELD-REPORT-2 BLACK-FENCE FIX: snapshot the per-packet layer
            // tables (both groups) once the packet feed is available, regardless of
            // which group's SSBO import just ran — the push-constant draw path reads
            // these every frame, not gated by materialReady/materialReadyOn latches.
            if (mf.packetAlbedoLayer && mf.packetCount > 0) {
                s->packetLayerOff.assign(mf.packetCount, -1);
                s->packetLayerOn.assign(mf.packetCount, -1);
                // FENCE-UVSCALE-1: default 1.0 (no crop) so any packet the feed
                // doesn't cover (or a stale/mismatched feed) behaves exactly as
                // before this change -- fail-soft to the old full-layer sample.
                s->packetUvScaleX.assign(mf.packetCount, 1.0f);
                s->packetUvScaleY.assign(mf.packetCount, 1.0f);
                for (uint32_t t = 0; t < mf.typeCount && t < s->typeRuns.size(); ++t) {
                    const TypeRun& tr = s->typeRuns[t];
                    const bool isOn = (mf.typeAlphaClass[t] == 1);
                    for (uint32_t p = 0; p < tr.packetCount; ++p) {
                        const uint32_t pk = tr.firstPacket + p;
                        if (pk >= mf.packetCount) break;
                        const int32_t layer = mf.packetAlbedoLayer[pk];
                        if (isOn) s->packetLayerOn[pk]  = layer;
                        else      s->packetLayerOff[pk] = layer;
                        if (mf.packetUvScaleX && mf.packetUvScaleY) {
                            s->packetUvScaleX[pk] = mf.packetUvScaleX[pk];
                            s->packetUvScaleY[pk] = mf.packetUvScaleY[pk];
                        }
                    }
                }
            }
            // ---- alpha-ON (class 1): the alpha-MIXED buildings. typeAlbedoLayer[t] is
            // ALREADY group-relative into s_texArrayOn for class-1 types, so the same
            // table shape works with the alpha-ON array + a 0.5 alpha-test in the FS. ----
            if (!s->materialReadyOn && mf.arrayKeyOn != 0) {
                std::vector<int32_t> layerOnTbl(mf.typeCount, -1);
                for (uint32_t t = 0; t < mf.typeCount; ++t) {
                    if (mf.typeAlphaClass[t] == 1 && mf.typeAlbedoLayer[t] >= 0)
                        layerOnTbl[t] = mf.typeAlbedoLayer[t];
                }
                if (cp_import_albedo_array(s, cp_target_on(s), mf.arrayKeyOn, mf.albedoOn)) {
                    cp_upload_layer_table_on(s, layerOnTbl.data(), mf.typeCount);
                    changed = true;
                    vlog("material feed active: alpha-ON %u types textured (alpha-test 0.5).",
                         mf.typeCount);
                }
            }
            if (changed) write_descriptors(s);  // rebind real arrays (3/6) + layer SSBOs (2/5)
        } else if (mf.bucketed) {
            static bool warned = false;
            if (!warned) { vlog("material feed is BC7-bucketed — DEFERRED; props keep flat teal."); warned = true; }
        }
    }

    // VK-PROP-FENCE-DEFER-1 (water-pilot template): begin the shared-slot frame BEFORE
    // any per-parity staging (the LightsData copy below is per-frame CPU staging). The
    // slot-fence wait inside beginFrame IS the deferred retire of the submit that last
    // used this parity slot, and it protects everything indexed [par] below: the light
    // SSBO, the instance SSBO, the frame UBO, the descriptor set, and the command
    // buffer. The inline post-submit wait this replaces is kept only under the
    // MC2_VK_PROP_FENCE_DEFER=0 killswitch. (The copy-once material imports above are
    // self-synced by their one-shot inline fence waits — safe before OR after this.)
    // Per-frame data goes into the engine frame ring BEFORE beginFrame (the ring slot belongs to the frame, not to the parity, so staging here races
    // nothing, and a ring failure cannot strand the shared fence beginFrame acquires).
    Mc2ColorPropLightFeed lightFeed{};
    const bool haveLightFeed = mc2_color_prop_island_get_light_feed(&lightFeed);
    mc2vk::VulkanContext& rctx = mc2vk::getOrCreateEngineVulkanContext();
    const VkDeviceSize ringAlign = rctx.frameAlignment();
    const uint32_t instanceCount = feed.count;
    const VkDeviceSize instBytes = static_cast<VkDeviceSize>(instanceCount) * sizeof(GpuStaticPropInstance);
    int poolN = (haveLightFeed && lightFeed.poolLights48 && lightFeed.numPoolLights > 0) ? lightFeed.numPoolLights : 0;
    if (poolN > kMaxPoolLights) poolN = kMaxPoolLights;
    const VkDeviceSize instOff = mc2vk::alignUp(sizeof(ColorFrameUBO), ringAlign);
    const VkDeviceSize poolOff = mc2vk::alignUp(instOff + (instBytes ? instBytes : sizeof(GpuStaticPropInstance)), ringAlign);
    const mc2vk::FrameAlloc mainSpan = rctx.frameAlloc(poolOff + static_cast<VkDeviceSize>((poolN > 0 ? poolN : 1)) * 48ull);
    if (!mainSpan) { setFallback("frame_ring_alloc_failed"); s->disabled = true; return 0; }
    uint8_t* const mainPtr = static_cast<uint8_t*>(mainSpan.ptr);

    VkImageResource* colorRes = reinterpret_cast<VkImageResource*>(s->t1_colorRes);
    const clk::time_point tFence0 = clk::now();
    VkFrameSync fsC{};
    { ZoneScopedN("VKIsl.Props.FenceWait");   // VK-ISLAND-TRACY-1: deferred retire of the parity slot
      fsC = vkImageResourceBeginFrame(colorRes); }
    us_fence_wait = usSince(tFence0);
    const uint32_t par = fsC.parity & 1u;
    const std::array<VkImageView,2> views{s->albedoTexture.view(),s->albedoOnTexture.view()};
    if (s->descriptorsDirty[par] || s->boundTextures[par]!=views) {
        write_descriptors(s,int(par)); s->boundTextures[par]=views;
    }
    void* hzbMask = nullptr;
    gosPostProcess* pp = getGosPostProcess();
    const bool hzbCulled = pp && pp->runVkPostHzbNative(feed, s->mapGeneration, &hzbMask);
    VkDescriptorBufferInfo maskInfo{hzbCulled ? reinterpret_cast<VkBuffer>(hzbMask) : s->hotBuf.get(),0,VK_WHOLE_SIZE};
    VkWriteDescriptorSet maskWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    maskWrite.dstSet = s->set[par]; maskWrite.dstBinding = 9; maskWrite.descriptorCount = 1;
    maskWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; maskWrite.pBufferInfo = &maskInfo;
    vkUpdateDescriptorSets(s->device,1,&maskWrite,0,nullptr);


    // VK-PROP-AMBIENT-IBL-1 / VK-PROP-LIGHTFEED-NATIVE-1: per-frame copy of the game's
    // CPU LightsData array (mc2::textures::ready() lightData_, the byte source of the GL SSBO
    // upload at renderLists) into the island's own SSBO (binding 4) so the vert shader's
    // ambientTerm sum reads the same ObjectLights[] the GL prop VS does. This is the mech
    // item-9 native-feed pattern: the former per-frame glGetBufferSubData GL SSBO readback
    // (a full GL client sync, ~1.3-1.5 ms/frame in the props receipts) was retired, then
    // (VK-PERF-2 cleanup) DELETED outright after the MC2_VK_PROP_FEED_AUDIT byte-compare
    // banked a clean run (3000 audited frames, 3.54 MB/frame, mismatches=0 — see
    // VK-ARC-STATE 2026-07-16). If the CPU array is not ready this frame, lighting is
    // simply not marked ready (shader falls back flat-lit via u.lightingReady=0) — no GL
    // readback rescue. IBL-SH coeffs are a static per-mission table (no GL round-trip)
    // fetched fresh each frame in case the mission's SH set changed.
    s->lightingReady = false;
    mc2vk::FrameAlloc lightSpan;
    const bool nativeLight = haveLightFeed && lightFeed.lightDataCpu &&
                             lightFeed.lightDataCpuBytes >= 3600ull;
    if (s->tier1 && s->wantLighting && nativeLight) {
        // VK-LIGHTFEED-DEDUP-1: try the shared once-per-frame LightsData ring
        // first (ONE copy total across the mech + prop islands instead of twin
        // ~3.5 MB copies; the copy itself is zoned VKIsl.Shared.LightFeed
        // inside acquire). On the shared path only the binding-4 descriptor
        // moves -- set[par] is fence-retired by the beginFrame above, so the
        // rebind is safe. Killswitch MC2_VK_LIGHTFEED_DEDUP=0 or any acquire
        // fail-soft (mid-frame light-count growth, ring alloc failure) runs
        // the pre-slice per-island copy below (zone VKIsl.Props.LightFeed)
        // unchanged.
        const VkDeviceSize lbytes = static_cast<VkDeviceSize>(lightFeed.lightDataCpuBytes);
        lightSpan = mc2vkSharedLightFeedAcquire(lightFeed.lightDataCpu, lightFeed.lightDataCpuBytes);
        if (lightSpan) {
            // Probe reads the CPU source -- byte-identical to the ring copy
            // (single-threaded copy of the same array this same frame).
            prop_light_vertex_probe(static_cast<const uint8_t*>(lightFeed.lightDataCpu), lbytes, feed);
            s->lightingReady = true;
        } else {
            ZoneScopedN("VKIsl.Props.LightFeed");   // VK-ISLAND-TRACY-1: per-frame LightsData CPU->VK copy (own-span path)
            lightSpan = rctx.frameAlloc(lbytes);
            if (lightSpan) {
                std::memcpy(lightSpan.ptr, lightFeed.lightDataCpu, static_cast<size_t>(lbytes));
                prop_light_vertex_probe(static_cast<const uint8_t*>(lightSpan.ptr), lbytes, feed);
                s->lightingReady = true;
            }
        }
    }

    const unsigned long f0a = g_forbidden.vkCmdCopyImageToBuffer;
    const unsigned long f0b = g_forbidden.glGetTexImage;
    const unsigned long f0c = g_forbidden.glReadPixels;

    const clk::time_point tStage0 = clk::now();
    // ---- 1) Upload the per-frame color instances into slot `par` (grouped by typeID). ----
    {
        ZoneScopedN("VK-props-instance-upload");   // VK-FLIP-FIELD-REPORT-1: per-frame instance SSBO staging
        if (instBytes) std::memcpy(mainPtr + instOff, feed.instances112, static_cast<size_t>(instBytes));
    }
    g_health.instRecords = instanceCount;

    // ---- 2) Upload the per-frame UBO (worldToClipGL + bias; instBase=0). ----
    {
        ColorFrameUBO u{};
        std::memcpy(u.worldToClipGL, feed.worldToClipGL, sizeof(u.worldToClipGL));
        u.objectDepthBias = feed.objectDepthBias;
        u.instBase = 0;
        // VK-P11 STATIC-PROP MATERIAL: gate FS albedo sample + computePBR on the real
        // import having landed (Tier-1 only). material-disabled paths leave these zero (the shaders
        // never read them).
        u.materialReady = (s->wantMaterial && s->tier1 && s->materialReady && rctx.textures().ready(s->albedoTexture.get())) ? 1 : 0;
        u.materialReadyOn = (s->wantMaterial && s->tier1 && s->materialReadyOn && rctx.textures().ready(s->albedoOnTexture.get())) ? 1 : 0;
        u.terrainSunMC2[0] = s->terrainSunMC2[0];
        u.terrainSunMC2[1] = s->terrainSunMC2[1];
        u.terrainSunMC2[2] = s->terrainSunMC2[2];
        u.terrainSunMC2[3] = 0.0f;
        // VK-PROP-AMBIENT-IBL-1: IBL-SH coeffs (std140 vec3[9] tail, .w pad each) +
        // strength + lightingReady. lightingReady gates the vert shader's real
        // ObjectLights ambientTerm sum; iblShStrength==0.0 short-circuits evalShL2
        // exactly like the GL guard (byte-identical when the feature/env is off).
        if (haveLightFeed) {
            for (int i = 0; i < 9; ++i) {
                u.iblSh[i][0] = lightFeed.iblSh[i][0];
                u.iblSh[i][1] = lightFeed.iblSh[i][1];
                u.iblSh[i][2] = lightFeed.iblSh[i][2];
                u.iblSh[i][3] = 0.0f;
            }
            u.iblShStrength = lightFeed.iblShStrength;
        } else {
            std::memset(u.iblSh, 0, sizeof(u.iblSh));
            u.iblShStrength = 0.0f;
        }
        u.lightingReady = s->lightingReady ? 1 : 0;
        // The eye's night state lights the hot-colour tags; without the feed the island stays on its daytime arm.
        u.isNight      = haveLightFeed ? lightFeed.isNight : 0;
        u.nightFactor  = haveLightFeed ? lightFeed.nightFactor : 0.0f;
        u.hotTableReady = s->hotReady ? 1 : 0;   // the shader reads the table only when the real one is bound
        u.ambientV1Strength = haveLightFeed ? lightFeed.ambientV1Strength : 0.0f;
        u.pbrV1Strength = haveLightFeed ? lightFeed.pbrV1Strength : 0.0f;
        u.pbrV1RoughnessOverride = haveLightFeed ? lightFeed.pbrV1RoughnessOverride : -1.0f;
        for (int k = 0; k < 4; ++k) u.cameraWorldPos[k] = haveLightFeed ? lightFeed.cameraWorldPos[k] : 0.0f;
        // Night light pools: this frame's pool copy (GL caps the set at 256), flushed because the sequential-write placement need not be
        // host-coherent; a failed upload turns the term off.
        if (poolN > 0) std::memcpy(mainPtr + poolOff, lightFeed.poolLights48, static_cast<size_t>(poolN) * 48u);
        u.poolF[0] = lightFeed.poolMoverDim; u.poolF[1] = lightFeed.poolMoverScale; u.poolF[2] = lightFeed.poolObjScale; u.poolF[3] = 0.0f;
        u.poolI[0] = poolN;                  u.poolI[1] = lightFeed.poolSoft;
        u.poolI[2] = lightFeed.poolMoverMax; u.poolI[3] = lightFeed.poolCompress;
        // ADJUDICATION-1 WASHOUT engagement probe: one-shot dump of the exact UBO
        // lighting terms so we can tell fed-real from fail-soft-constant. If
        // lightingReady==0, the VS ambientTerm falls back to a hemisphere constant
        // (= washout). Prints WHY the LightsData copy did/didn't land.
        {
            static bool s_lit_dumped = false;
            if (mc2::gate::get<"MC2_VK_PROP_LIGHT_PROBE">() && !s_lit_dumped) {
                s_lit_dumped = true;
                std::fprintf(stderr,
                    "[PROP_LIGHT_PROBE v1] lightingReady=%d haveLightFeed=%d tier1=%d "
                    "wantLighting=%d wantMaterial=%d materialReady=%d "
                    "iblShStrength=%.4f iblSh0=(%.4f,%.4f,%.4f) "
                    "lightDataCpuBytes=%llu terrainSun=(%.3f,%.3f,%.3f)\n",
                    u.lightingReady, haveLightFeed ? 1 : 0, s->tier1 ? 1 : 0,
                    s->wantLighting ? 1 : 0, s->wantMaterial ? 1 : 0, u.materialReady,
                    u.iblShStrength, u.iblSh[0][0], u.iblSh[0][1], u.iblSh[0][2],
                    static_cast<unsigned long long>(lightFeed.lightDataCpuBytes),
                    u.terrainSunMC2[0], u.terrainSunMC2[1], u.terrainSunMC2[2]);
                std::fflush(stderr);
            }
        }
        std::memcpy(mainPtr, &u, sizeof(u));
    }
    // set[par] follows the buffers the spans came from; the draw passes the offsets (bindings 0, 1, then 4 when declared, then 8).
    sync_ring_bindings(s, par, mainSpan, lightSpan);
    uint32_t dynOffsets[4];
    uint32_t nDyn = 0;
    dynOffsets[nDyn++] = static_cast<uint32_t>(mainSpan.offset);
    dynOffsets[nDyn++] = static_cast<uint32_t>(mainSpan.offset + instOff);
    if (s->tier1 && s->wantLighting) dynOffsets[nDyn++] = static_cast<uint32_t>(lightSpan ? lightSpan.offset : mainSpan.offset);
    if (s->tier1) dynOffsets[nDyn++] = static_cast<uint32_t>(mainSpan.offset + poolOff);
    g_health.stageInstUs = usSince(tStage0);
    // VK-FLIP-FIELD-REPORT-1: the instance+UBO staging above is the per-frame prop
    // upload cost (a coarse zone for it would nest cleanly; kept as the usSince metric
    // to avoid an extra scope around the two vmaMap blocks).

    // The feed is already grouped by ascending typeID; build contiguous type-runs.
    const GpuStaticPropInstance* insts =
        reinterpret_cast<const GpuStaticPropInstance*>(feed.instances112);

    // ===================================================================
    // TIER-1 co-write: props rendered IN-PLACE into the SHARED scene color(MRT0)+
    // normal(MRT1)+depth GBuffer. GL authored terrain into all three and handed them
    // to VK (each in its *_ATTACHMENT layout); VK LOADs (does NOT clear) all three,
    // depth-tests GEQUAL reverse-Z, and only overwrites where the prop wins. No
    // no composite, NO re-clear of GBuffer1 (respects clearGBuffer1()).
    // ===================================================================
    if (s->tier1) {
        // VK-RUNG8-ITEM5: direct-chain-native only. The shared GBuffer was left
        // Vulkan-owned by the terrain first-owner (same attachment layouts), so no
        // GL<->VK semaphore handoff is needed. VK-PROP-FENCE-DEFER-1: the beginFrame
        // (parity fence for CPU ordering) is HOISTED above the light/inst/UBO staging.
        VkCommandBuffer cb = s->cbuf[par];

        const clk::time_point tRender0 = clk::now();
        ZoneNamedN(zPropRec, "VK-props-record-submit", true);   // VK-FLIP-FIELD-REPORT-1: CPU record+queue-submit (to tier1-block end)
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }
        vk_render_pass_timer::Begin(cb, gos_render_pass_timer::Pass_SpColor);
        vk_render_pass_timer::CancelUnlessSubmitted timerGuard(gos_render_pass_timer::Pass_SpColor);

        // Begin the LOAD renderpass — NO clear values (all attachments loadOp=LOAD).
        VkRenderPassBeginInfo rbi{};
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = s->t1_rpass.get(); rbi.framebuffer = s->t1_fb.get();
        rbi.renderArea = {{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
        rbi.clearValueCount = 0; rbi.pClearValues = nullptr;
        MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Props");
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &scissor);

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->t1_pipe.get());
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &s->set[par], nDyn, dynOffsets);
        VkDeviceSize vboOff = 0;
        const VkBuffer vertexBuffer = s->vbo.get();
        vkCmdBindVertexBuffers(cb, 0, 1, &vertexBuffer, &vboOff);
        vkCmdBindIndexBuffer(cb, s->ibo.get(), 0, VK_INDEX_TYPE_UINT32);

        // ADJUDICATION-1 draw-level dump: env-gated one-shot CSV of every packet's
        // draw parameters + push-constant layers. This is the fence-fix ground truth
        // (the exact per-packet data RenderDoc would show at a VK vkCmdDrawIndexed,
        // which a GL-only RenderDoc capture cannot see for this headless-VK island).
        // Gated on the same sim-freeze predicate the deterministic sweep uses.
        FILE* s_pktDump = nullptr;
        {
            static bool s_dumped = false;
            const std::string dpText = mc2::gate::str<"MC2_VK_PROP_PACKET_DUMP">();   // "" when unset or empty
            const char* dp = dpText.empty() ? nullptr : dpText.c_str();
            const bool frozen = mc2CaptureGaitFreezeActive() != 0 ||
                                !mc2::gate::isSet<"MC2_VISUAL_BOOKMARK_CAPTURE">();
            if (dp && dp[0] && !s_dumped && frozen) {
                s_dumped = true;
                s_pktDump = std::fopen(dp, "w");
                if (s_pktDump)
                    std::fprintf(s_pktDump, "typeID,packet,layerOff,layerOn,uvScaleX,uvScaleY,indexCount,firstIndex,baseVertex,instCount,firstInstance\n");
            }
        }

        unsigned long typeRuns = 0;
        // PROP-COVERAGE-1: pass 0 draws every packet (alpha-tested ones cut at the coverage
        // threshold); pass 1 re-draws only the alpha-tested packets with the fringe pipeline,
        // after every core is in the depth buffer.
        for (int32_t fringePass = 0; fringePass < 2; ++fringePass) {
        if (fringePass == 1)
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->t1_fringePipe.get());
        uint32_t i = 0;
        while (i < instanceCount) {
            const uint32_t tid = insts[i].typeID;
            uint32_t j = i;
            while (j < instanceCount && insts[j].typeID == tid) ++j;
            const uint32_t instCount = j - i;
            if (tid < s->typeRuns.size()) {
                const TypeRun& tr = s->typeRuns[tid];
                for (uint32_t p = 0; p < tr.packetCount; ++p) {
                    const uint32_t pk = tr.firstPacket + p;
                    if (pk >= s->packets.size()) break;
                    const Mc2ShadowPacket& pkt = s->packets[pk];
                    // VK-FLIP-FIELD-REPORT-2 BLACK-FENCE FIX: push THIS packet's own
                    // group-relative albedo layer (off/on), not the type's shared
                    // "first-valid-wins" layer — see packetLayerOff/On comment above.
                    // FENCE-UVSCALE-1: push this packet's own uvScaleX/Y alongside
                    // its layer -- default 1.0 (no crop) matches pre-fix behavior
                    // for any packet the feed didn't cover.
                    struct { int32_t layerOff, layerOn; float uvScaleX, uvScaleY; int32_t alphaTest, fringe, hzb; } pc = {
                        (pk < s->packetLayerOff.size())  ? s->packetLayerOff[pk]  : -1,
                        (pk < s->packetLayerOn.size())   ? s->packetLayerOn[pk]   : -1,
                        (pk < s->packetUvScaleX.size())  ? s->packetUvScaleX[pk]  : 1.0f,
                        (pk < s->packetUvScaleY.size())  ? s->packetUvScaleY[pk]  : 1.0f,
                        mc2_static_prop_packet_alpha_test(pk),
                        fringePass,
                        hzbCulled ? 1 : 0,
                    };
                    // Only packets the FS can alpha-test have a fringe (the alpha-ON layer or the per-packet bit).
                    if (fringePass == 1 && pc.layerOn < 0 && pc.alphaTest == 0) continue;
                    if (s_pktDump && fringePass == 0)
                        std::fprintf(s_pktDump, "%u,%u,%d,%d,%.4f,%.4f,%u,%u,%d,%u,%u\n",
                                     tid, pk, pc.layerOff, pc.layerOn, pc.uvScaleX, pc.uvScaleY,
                                     pkt.indexCount, pkt.firstIndex,
                                     static_cast<int32_t>(pkt.baseVertex), instCount, i);
                    vkCmdPushConstants(cb, s->playout.get(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                        0, sizeof(pc), &pc);
                    vkCmdDrawIndexed(cb, pkt.indexCount, instCount,
                                     pkt.firstIndex, static_cast<int32_t>(pkt.baseVertex), i);
                }
                if (fringePass == 0) ++typeRuns;
            }
            i = j;
        }
        }
        if (s_pktDump) { std::fclose(s_pktDump); s_pktDump = nullptr; }
        g_health.typeRuns = typeRuns;

        vkCmdEndRenderPass(cb);
        gpuScope.end();
        vk_render_pass_timer::End(cb, gos_render_pass_timer::Pass_SpColor);
        if (vkEndCommandBuffer(cb) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

        // VK-RUNG8-ITEM5: direct-chain-native submit — waits/signals NO GL<->VK
        // semaphores (the shared GBuffer stays Vulkan-owned); the color fence below
        // orders CPU before the next Vulkan island records against these images.
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 0; si.pWaitSemaphores = nullptr; si.pWaitDstStageMask = nullptr;
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 0; si.pSignalSemaphores = nullptr;
        { ZoneScopedN("VKIsl.Props.Submit");   // VK-ISLAND-TRACY-1: vkQueueSubmit
          if (mc2vk::submitGraphics(si, fsC.fence) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; } }
        timerGuard.submitted();

        // VK-PROP-FENCE-DEFER-1: the inline retire wait is GONE — the submit's fence
        // (the shared parity-slot fence) is waited at the slot's next
        // vkImageResourceBeginFrame, i.e. the deferred retire the beginFrame hoisted
        // above the staging already timed into us_fence_wait. Everything this frame's
        // submit uses is per-parity ([par]), write-once, or synced by its own inline
        // one-shot fence, so nothing is rewritten before that wait. GPU-side ordering
        // against downstream islands/post/composite never depended on this CPU wait
        // (same-queue submission order + their command buffers' entry barriers).
        // Killswitch MC2_VK_PROP_FENCE_DEFER=0 restores the inline wait.
        if (!propFenceDeferEnabled()) {
            ZoneScopedN("VKIsl.Props.InlineFenceWait");   // VK-ISLAND-TRACY-1: killswitch-only inline retire
            const clk::time_point tInline0 = clk::now();
            if (vkWaitForFences(s->device, 1, &fsC.fence, VK_TRUE, 5ull*1000*1000*1000) != VK_SUCCESS) {
                setFallback("fence_timeout"); s->disabled = true; return 0;
            }
            us_fence_wait = usSince(tInline0);
        }
        g_health.drawUs = usSince(tRender0);

        // NO composite — props were co-written in-place into the real GBuffer.

        const bool readbackFired =
            (g_forbidden.vkCmdCopyImageToBuffer != f0a) ||
            (g_forbidden.glGetTexImage != f0b) || (g_forbidden.glReadPixels != f0c);
        if (readbackFired) {
            vlog("FATAL(tier1): a readback fired on the co-write path — failing frame + disabling.");
            setFallback("interop_did_readback"); s->disabled = true; return 0;
        }

        g_health.vkDraws = 1;
        ++g_health.usedVulkan;
        // VK-RUNG8-ITEM5: never interop (direct-chain-native only).
        g_health.totalUs = usSince(tTotal0);
        static bool s_t1FirstOk = false;
        if (!s_t1FirstOk) {
            vlog("first TIER-1 in-place GBuffer co-write: instCount=%u typeRuns=%lu "
                 "(props co-written into shared color(MRT0)+normal(MRT1)+depth; no composite).",
                 instanceCount, g_health.typeRuns);
            s_t1FirstOk = true;
        }
        if (perf) {
            // [VK_PROPS_PERF] — water-style per-frame us breakdown (MC2_VK_PROP_PERF).
            // record_submit_us = record + submit (+ inline fence wait under killswitch);
            // fence_wait_us = the deferred retire (beginFrame wait) or the inline wait.
            std::fprintf(stderr,
                "[VK_PROPS_PERF] tier1=1 inst=%u type_runs=%lu stage_us=%.1f "
                "record_submit_us=%.1f fence_wait_us=%.1f island_total_us=%.1f\n",
                instanceCount, g_health.typeRuns, g_health.stageInstUs,
                g_health.drawUs, us_fence_wait, g_health.totalUs);
            std::fflush(stderr);
        }
        return 1;
    }

    return 0;
}
extern "C" void mc2_render_backend_color_prop_region_health(
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

extern "C" void mc2_vk_color_prop_island_destroy() {
    vlog("[VK_PROP_COLOR_TIER1] vulkan_available=%d used_vulkan=%lu used_interop=%lu interop_active=%d "
         "vk2gl_sem=%d validation_errors=%lu inst_records=%lu type_runs=%lu geom_verts=%lu "
         "fallback_reason=\"%s\" vk_draws=%lu forbidden_copyImgToBuf=%lu forbidden_getTexImage=%lu "
         "forbidden_readPixels=%lu stage_inst_us=%.1f draw_us=%.1f total_us=%.1f",
         g_health.vulkanAvailable, g_health.usedVulkan, g_health.usedInterop, g_health.interopActive,
         g_health.vk2glSemActive, g_health.validationErrors, g_health.instRecords, g_health.typeRuns,
         g_health.geomVerts, g_health.fallbackReason, g_health.vkDraws,
         g_forbidden.vkCmdCopyImageToBuffer, g_forbidden.glGetTexImage, g_forbidden.glReadPixels,
         g_health.stageInstUs, g_health.drawUs, g_health.totalUs);
    mc2vk::getOrCreateEngineVulkanContext().retireIsland(mc2vk::IslandSlot::ColorProp);
}

}

#endif // MC2_VULKAN_ISLAND
