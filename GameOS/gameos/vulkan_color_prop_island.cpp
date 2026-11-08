// vulkan_color_prop_island.cpp — VK-P11 STATIC-PROP COLOR: the FIRST VK-native COLOR
// GEOMETRY region rendering REAL static-prop triangle geometry.
// Renders static props in Vulkan directly into the shared scene GBuffer.
// Shape:
//   - Borrows the shared engine VulkanContext; no private device.
//   - Uses the shared scene color0/normal/depth exportable images.
//   - Copies static-prop geometry once per map and stages visible instances per frame.
//   - Optional material/light feeds extend parity without changing the public health ABI.
//
// The whole TU compiles to nothing unless MC2_VULKAN_ISLAND is defined.
// =============================================================================


#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <span>
#include <string_view>
#include <bit>
#include "../../mclib/battle_bench_wait.h"
#include "../../RenderCore/TemporalInputs.h"
extern uint32_t g_mc2RenderFrame;   // mclib/tgl.cpp
#include <optional>
#include <volk.h>
#include "Gates/gate_cold.h"   // gate::get<>() and str<>()
#include "hitch_stats.h"
#include "vk_render_pass_timer.h"
#include "vk_gpu_profile.h"   // VK-GPU-ZONES: MC2_VK_GPU_SCOPE

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include "../../RenderCore/gpu_abi/GpuStaticPropInstance.h"  // single-source 112B instance record
#include "../../RenderCore/gpu_abi/GpuObjectLights.h"  // single-source per-object light record
#include "../../RenderCore/gpu_abi/GpuColorFrameUBO.h"  // single-source per-frame UBO
#include "gos_terrain_lighting.h"
#include "../../RenderCore/PropMaterialGpu.h"
#include "../../RenderCore/PropMaterialRows.h"
#include "vk_image_resource.h"
#include "vk_island_util.h"
#include "gos_visual_capture.h"      // mc2CaptureGaitFreezeActive() (extern "C")
#include "vk_one_shot.h"
#include "vulkan_context.h"
#include "vk_barrier_batch_vk.h"
#include "vulkan_shadow_island_live.h"   // geometry and color feed contracts
#include "gos_postprocess.h"
#include "gos_static_prop_registry.h"   // OBJECT-ID-1: getRecipeIndexForType
#include "../../RenderWorld/RenderWorld.h"   // OBJECT-ID-1: IsObjectIdBufferEnabled, objectIdRawForStaticPropRecipe
#include "vulkan_color_prop_island.h"
#include "vk_rt_scene.h"
#include "texture_decode.h"
#include "vk_texture_residency.h"
#include "vk_vegetation_ground.h"
#include "vulkan_mover_shadow.h"
#include "Gates/render_options.h"
#include "../include/gameos.hpp"
#include "../../code/render_view.h"
#include "../../code/sim_tick_clock.h"
#include "vk_shared_gbuffer_gate.h"      // VK-P14 GATE-CONSOLIDATION-1 umbrella resolver
#include "vk_shared_light_feed.h"        // VK-LIGHTFEED-DEDUP-1: shared per-frame LightsData SSBO
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"   // TIER-1: fetch shared scene color/normal images
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"   // TIER-1: fetch shared scene depth (island accessors)

extern const float* gos_GetTerrainMVPMat4();   // live camera matrix (matrix diag only)
float mc2NightLightFactor();   // mclib/camera.cpp: 0 day/evening .. 1 full night

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
#include "mc2fmt.h"
#include "vk_island_fail_policy.h"


namespace {

// Flips to 1 once init passes the context/shared-GBuffer readiness gates: a later failure is a build failure
// (vk_island_fail_policy.h), an earlier one retries next frame.
int g_propInitReachedBuild = 0;

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

// GPU-driven props, CPU mirrors of the shader tables (color_prop_cull.comp, prop_packet_vk.glsl). One packet row and one indirect command exist per
// (type run, packet, list); the compute pass writes each command's instanceCount.
struct PropPacketRow {
    int32_t layerOff, layerOn; float uvScaleX, uvScaleY; int32_t alphaTest, fringe, materialId, objectId;
};
static_assert(sizeof(PropPacketRow) == 32);
// The vertex+fragment push block (prop_packet_vk.glsl): the packet-row buffer's device address and the first row of the list one indirect call draws.
struct PropPacketPush { uint64_t rows; uint32_t base; uint32_t pad; };
static_assert(sizeof(PropPacketPush) == 16);
// Draw lists, in command-buffer order: opaque core (no-discard pipeline, early Z), alpha core, fringe, late fringe.
constexpr uint32_t kListOpaque = 0, kListCore = 1, kListFringe = 2, kListLate = 3, kListCount = 4;
struct PropRunRow { uint32_t first, count; uint32_t range[kListCount][2]; uint32_t pad[2]; };   // range[list] = {start, n}
static_assert(sizeof(PropRunRow) == 48);
struct PropDrawCmd { uint32_t indexCount, instanceCount, firstIndex; int32_t baseVertex; uint32_t firstInstance; };
static_assert(sizeof(PropDrawCmd) == 20);
constexpr VkDeviceSize kPropCmdHeader = 16;   // the four list counts precede the commands

struct ColorPropIsland : mc2vk::IslandState {
    bool disabled = false;
    int  waitStrikes = 0;
    bool inited   = false;
    bool useInterop = false;
    bool tier1    = false;   // TIER-1 in-place shared-GBuffer co-write
    bool t1_haveId = false;  // OBJECT-ID-1: the render pass carries the shared object-id attachment

    // ---- TIER-1: shared-GBuffer co-write objects (built over the SAME shared
    // color/normal/depth images the GL scene renders terrain into). ----
    // Borrowed (NOT owned) shared-image resources/views, fetched per-frame from the
    // shared-scene-color/-depth registries. Cached at init for the framebuffer build.
    struct VkImageResource* t1_colorRes  = nullptr;
    struct VkImageResource* t1_normalRes = nullptr;
    struct VkImageResource* t1_depthRes  = nullptr;
    struct VkImageResource* t1_idRes     = nullptr;   // OBJECT-ID-1: shared R32_UINT object-id image (MRT2); null when MC2_OBJECT_ID_BUFFER is off
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
    std::vector<int32_t>        packetMaterialIds;
    bool authoredReady = false;
    bool authoredAttempted = false;
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
    // Per-frame data (UBO, instance rows, LightsData) lives in the engine frame ring; set[par] binds it with dynamic offsets (bindings 0, 1,
    // 4). ringBound[par][binding] = the ring buffer that binding of set[par] currently points at; a frame whose spans came from another buffer
    // (the ring grew), or a set rewritten by write_descriptors, is re-pointed after beginFrame retired the parity.
    VkBuffer      ringBound[mc2vk::kMaxFramesInFlight][9] = {};

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
    std::span<const float> hotCpu; // borrowed map table, also identifies profiled tree groups
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

    VkDescriptorSet   set[mc2vk::kMaxFramesInFlight] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // per parity slot (VK-PROP-FENCE-DEFER-1); set[i] binds ubo/inst/light[i]
    VkCommandBuffer   cbuf[mc2vk::kMaxFramesInFlight] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // allocated from `cpool`, freed with it
    VkCommandBuffer lateCbuf[mc2vk::kMaxFramesInFlight] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    mc2vk::GpuTicket lateSubmitted[mc2vk::kMaxFramesInFlight];   // timeline ticket of the slot's late-coverage submit (replaces the per-slot fence)
    mc2vk::GpuTicket submitted[mc2vk::kMaxFramesInFlight];   // per engine frame slot: the last core submit that used cbuf[slot], set[slot] and the slot's inputs
    // GPU-driven props: per parity slot, the packet rows (binding 15), the run table (16), the compacted instance slots (17) and the three indirect
    // draw lists with their counts (18). A slot's buffers are rewritten only after its submit and late-fringe fences were waited.
    struct PropGpu {
        mc2vk::OwnedBuffer pkt, run, compact, cmd;
        VkDeviceSize pktCap = 0, runCap = 0, compactCap = 0, cmdCap = 0;
    };
    PropGpu gpu[mc2vk::kMaxFramesInFlight];
    std::vector<PropPacketRow> pktScratch[kListCount];   // rows parallel to cmdScratch
    std::vector<PropRunRow> runScratch;
    std::vector<PropDrawCmd> cmdScratch[kListCount];   // indexed by kList*
    uint32_t drawIndirectLimit = 0xFFFFFFFFu; // device maxDrawIndirectCount; maxDrawCount is clamped to it
    // The late (alphaTest == 2) fringe draws, issued by the late pass after the main submit: the list's first row/command and its size.
    uint32_t lateStart = 0, lateCount = 0;
    uint32_t lateParity = 0, lateDynCount = 0, lateOffsets[4]{}, lateGroundOffsets[4]{};
    mc2vk::OwnedShaderModule cullShader;
    mc2vk::OwnedPipelineLayout cullLayout;
    mc2vk::OwnedPipeline cullPipe;

    // Owned objects are declared in creation order and released in reverse by the destructor, while the device is alive (the engine context releases the
    // parked island ahead of the device). An image is declared before its view so the view is released first.
    mc2vk::OwnedBuffer vbo, ibo;
    mc2vk::OwnedRenderPass t1_rpass;      // 2 color LOAD + depth LOAD
    mc2vk::OwnedFramebuffer t1_fb;        // over the 3 shared image views
    mc2vk::OwnedDescriptorSetLayout dsl;
    mc2vk::OwnedDescriptorPool pool;
    mc2vk::ResidentTextureRef albedoTexture;
    bool descriptorsDirty[mc2vk::kMaxFramesInFlight]={};
    std::array<VkImageView,2> boundTextures[mc2vk::kMaxFramesInFlight]{};
    struct MaskBinding {
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorBufferInfo info{};
        uint64_t generation = 0;
        uint32_t mapGeneration = 0;
    };
    MaskBinding boundMask[mc2vk::kMaxFramesInFlight]{};
    VkImageView boundObjectView[mc2vk::kMaxFramesInFlight]{};   // binding 14 last written with this view (null: copied from 24 or never written)
    uint32_t boundObjectViewMap[mc2vk::kMaxFramesInFlight]{};
    mc2vk::DescWriteStamp moverStamp[mc2vk::kMaxFramesInFlight]{};   // binding 24 of set[i]
    mc2vk::DescWriteStamp copy14Stamp[mc2vk::kMaxFramesInFlight]{};  // binding 14 copied from 24 (no object view)
    mc2vk::OwnedSampler albedoSamp;
    mc2vk::OwnedBuffer  layerBuf;         // per-type int layer SSBO
    mc2vk::ResidentTextureRef albedoOnTexture;
    mc2vk::OwnedSampler albedoOnSamp;
    mc2vk::OwnedBuffer  layerOnBuf;       // per-type int layer SSBO (class 1)
    mc2vk::OwnedBuffer  hotBuf;           // per-type hot-colour night emission colours (3 vec4 per type)
    mc2vk::ResidentTextureRef normalTexture, ormTexture;
    std::vector<mc2vk::ResidentTextureRef> propTextures;   // per-object maps, index = uPropTex[] slot
    uint32_t propTexSlots = 1;   // uPropTex[] length; 1 = per-object maps off (lowCost, or a device with tight sampler limits)
    mc2vk::OwnedSampler detailSampler, treeShadowSampler;
    float sampBias = 0.0f;   // mip bias the albedo / albedoOn / detail samplers were built with
    mc2veg::GroundLighting groundLighting;
    std::optional<Mc2GroundLightingFrame> lastGround;   // last terrain frame's offsets; see the run path
    bool groundMissing = false;
    uint32_t groundMissingFrames = 0;       // consecutive frames without a live terrain frame; stale offsets/copies are dropped past a short grace
    bool groundCopied[mc2vk::kMaxFramesInFlight] = {false, false};   // this parity's own ground-lighting set holds a copy from a live terrain frame
    mc2vk::OwnedBuffer materialBuf;
    mc2vk::OwnedShaderModule t1_vert, t1_frag;
    mc2vk::OwnedPipelineLayout playout;
    mc2vk::OwnedPipeline t1_pipe;         // depthWrite ON GEQUAL, 2 outputs, no blend
    mc2vk::OwnedPipeline t1_opaquePipe;   // t1_pipe with the FS discards compiled out (kOpaqueOnly), so early Z runs
    mc2vk::OwnedPipeline t1_fringePipe;   // PROP-COVERAGE-1: depth test only, MRT0 alpha-blended, MRT1 masked
    mc2vk::OwnedCommandPool cpool;   // one-shot command buffers only (allocated and freed per use)
    mc2vk::OwnedCommandPool slotPool[mc2vk::kMaxFramesInFlight];   // per-slot pools for cbuf: vkResetCommandPool instead of per-cb reset
    mc2vk::OwnedCommandPool latePool[mc2vk::kMaxFramesInFlight];   // per-slot pools for lateCbuf
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
    aci.usage = hostVisible ? VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE : VMA_MEMORY_USAGE_AUTO;   // every host-visible buffer here is CPU-written and GPU-read
    aci.flags = hostVisible ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT : 0;
    if (hostVisible) aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;   // callers write with map+memcpy and no flush
    (void)gpuReadHeavy;
    if (!out.create(s->allocator, s->device, bci, aci, name, i0, i1)) {
        vlog("buffer creation failed (%.*s).", static_cast<int>(name.size()), name.data()); return false;
    }
    return true;
}

constexpr VkDeviceSize kFrameUboRange = sizeof(ColorFrameUBO);
constexpr uint32_t kPropTextureSlots = 1024;   // uPropTex[] maximum in prop_material_vk.glsl (spec constant 1)

// Binding 4 (LightsData) is declared when tier1 && wantLighting.
void write_ring_bindings(ColorPropIsland* s, VkDescriptorSet set) {
    if (set == VK_NULL_HANDLE) return;
    const VkBuffer ring = mc2vk::getOrCreateEngineVulkanContext().frameRingBuffer();
    if (ring == VK_NULL_HANDLE) return;
    const VkDeviceSize range = mc2vk::getOrCreateEngineVulkanContext().frameRingRange();
    mc2vk::writeDynamicBinding(s->device, set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, ring, kFrameUboRange);
    mc2vk::writeDynamicBinding(s->device, set, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, ring, range);
    if (s->tier1 && s->wantLighting) mc2vk::writeDynamicBinding(s->device, set, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, ring, range);
}

// Re-point set[par]'s ring bindings at the buffers this frame's spans live in (after beginFrame retired the parity). A binding already there is skipped.
void sync_ring_bindings(ColorPropIsland* s, uint32_t par, const mc2vk::FrameAlloc& mainSpan, const mc2vk::FrameAlloc& lightSpan) {
    struct Want { uint32_t binding; VkDescriptorType type; VkBuffer buf; VkDeviceSize range; bool on; };
    const VkBuffer mainBuf = mainSpan.buffer;
    const VkBuffer lightBuf = lightSpan ? lightSpan.buffer : mainSpan.buffer;
    const VkDeviceSize lightRange = lightSpan ? lightSpan.range : mainSpan.range;
    const Want want[3] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, mainBuf, kFrameUboRange, true},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, mainBuf, mainSpan.range, true},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, lightBuf, lightRange, s->tier1 && s->wantLighting},
    };
    for (const Want& w : want) {
        if (!w.on || w.buf == VK_NULL_HANDLE || s->ringBound[par][w.binding] == w.buf) continue;
        mc2vk::writeDynamicBinding(s->device, s->set[par], w.binding, w.type, w.buf, w.range);
        s->ringBound[par][w.binding] = w.buf;
    }
}

void write_descriptors(ColorPropIsland* s, int parity = -1) {
    if (parity<0) { for (auto& dirty : s->descriptorsDirty) dirty = true; return; }
    for (int i=parity;i<=parity;++i) {
    s->descriptorsDirty[i]=false;
    s->boundMask[i] = {};
    s->boundObjectView[i] = VK_NULL_HANDLE;
    if (s->set[i] == VK_NULL_HANDLE) continue;
    // The ring bindings (0 UBO, 1 instances, 4 LightsData) at the frame ring's current buffer; the next frame of this parity re-points them at
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
    if (s->normalTexture && s->ormTexture && s->detailSampler && s->materialBuf) {
        VkDescriptorImageInfo detail[2] = {
            {s->detailSampler.get(), s->normalTexture.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {s->detailSampler.get(), s->ormTexture.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkDescriptorBufferInfo table{s->materialBuf.get(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet writes[3]{};
        for (uint32_t j = 0; j < 3; ++j) {
            writes[j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[j].dstSet = s->set[i]; writes[j].dstBinding = 10 + j;
            writes[j].descriptorCount = 1;
            writes[j].descriptorType = j < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if (j < 2) writes[j].pImageInfo = &detail[j]; else writes[j].pBufferInfo = &table;
        }
        vkUpdateDescriptorSets(s->device, 3, writes, 0, nullptr);
    }
    if (!s->propTextures.empty() && s->detailSampler) {
        std::vector<VkDescriptorImageInfo> infos(s->propTextures.size());
        for (size_t k = 0; k < infos.size(); ++k)
            infos[k] = {s->detailSampler.get(), s->propTextures[k].view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet tw{};
        tw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; tw.dstSet = s->set[i]; tw.dstBinding = 13;
        tw.descriptorCount = static_cast<uint32_t>(infos.size());
        tw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; tw.pImageInfo = infos.data();
        vkUpdateDescriptorSets(s->device, 1, &tw, 0, nullptr);
    }
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
        mc2::fmt::print_err("[PROP_LIGHT_VERTEX_PROBE v1] idx={:d} numLights={:d} ambient=({:.4f},{:.4f},{:.4f}) sunColor=({:.4f},{:.4f},{:.4f}) sunFound={:d}\n",
            mc2::fmt::as_size(idx), mc2::fmt::as_i32(n), ambient[0], ambient[1], ambient[2],
            sunColor[0], sunColor[1], sunColor[2], mc2::fmt::as_i32(sunFound ? 1 : 0));
    };
    // Real per-instance lightDataIndex values actually used this
    // frame (first few real props), instead of a blind 0..7 scan.
    {
        const GpuStaticPropInstance* insts =
            reinterpret_cast<const GpuStaticPropInstance*>(feed.instances112);
        mc2::fmt::print_err("[PROP_LIGHT_VERTEX_PROBE v1] real inst.lightDataIndex sample: ");
        for (uint32_t ii = 0; ii < feed.count && ii < 12; ++ii) {
            mc2::fmt::print_err("[{:d}]={:d} typeID={:d} ", mc2::fmt::as_u32(ii), mc2::fmt::as_u32(insts[ii].lightDataIndex), mc2::fmt::as_u32(insts[ii].typeID));
        }
        mc2::fmt::print_err("\n");
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
    (void)mipLevels;
    return *samp || mc2vk::infra::makeMaterialSampler(s->device, *samp, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, s->sampBias);
}

// FSR/AA resolve state changed: rebuild the biased samplers and rewrite every parity's sets once.
void cp_refresh_samplers(ColorPropIsland* s) {
    const auto albedo = [s](mc2vk::OwnedSampler* p) { return [s, p] { return cp_make_albedo_sampler(s, p, 0); }; };
    const auto detail = [s] { return mc2vk::infra::makeMaterialSampler(s->device, s->detailSampler, VK_SAMPLER_ADDRESS_MODE_REPEAT, s->sampBias); };
    if (!mc2vk::infra::refreshSamplers(s->sampBias, {{&s->albedoSamp, albedo(&s->albedoSamp)}, {&s->albedoOnSamp, albedo(&s->albedoOnSamp)}, {&s->detailSampler, detail}})) return;
    mc2vk::bumpDescEpoch();
    write_descriptors(s, -1);
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
    image.vkFormat=static_cast<uint32_t>(fmt); image.isCompressed=compressed; image.blockSizeBytes=compressed ? 16 : 0;
    image.pixels.assign(static_cast<const uint8_t*>(bytes),static_cast<const uint8_t*>(bytes)+byteCount);
    for (const auto& region : regions) image.mipByteOffsets.push_back(static_cast<size_t>(region.bufferOffset));
    uint64_t hash=1469598103934665603ull;
    for (uint8_t byte : image.pixels) { hash^=byte; hash*=1099511628211ull; }
    RenderCore::TextureKey key;
    key.source="prop-array:"+std::to_string(hash)+":"+std::to_string(w)+":"+std::to_string(h)+":"+std::to_string(layers);
    key.format=static_cast<uint32_t>(fmt); key.hints=mips>1 ? gosHint_MipmapFilter0 : 0;
    auto texture=mc2vk::getOrCreateEngineVulkanContext().textures().acquire(key,image,{w,h,w,h},layers,true);
    if (!texture) return false;
    if (tgt.texture == &s->albedoOnTexture) {
        // Bytes plus their interpretation identify the actual alpha-ON upload across installs/mods.
        const uint32_t shape[] = {static_cast<uint32_t>(fmt), w, h, layers, mips};
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
    const bool compressed = a->vkFormat == 141u || a->vkFormat == mc2tex::kVkFormatBc7Unorm || a->vkFormat == mc2tex::kVkFormatBc7Srgb;
    if (a->width == 0 || a->height == 0 || a->layers == 0 || a->levels.empty() || (!compressed && a->vkFormat != mc2tex::kVkFormatRgba8Unorm)) {
        vlog("%s albedo array rejected: %ux%u layers=%u levels=%zu format=%u.", tgt.tag, a->width, a->height, a->layers, a->levels.size(), a->vkFormat);
        return false;
    }
    // Potato omits the top mip from residency and staging, not just sampling.
    const uint32_t firstMip = mc2::options::current().baseQuality == 0 && a->levels.size() > 1 ? 1u : 0u;
    const uint32_t mips = static_cast<uint32_t>(a->levels.size()) - firstMip;
    const uint32_t width = mc2tex::mipExtent(a->width, firstMip), height = mc2tex::mipExtent(a->height, firstMip);
    std::vector<uint8_t> blob;
    std::vector<VkBufferImageCopy> regions;
    regions.reserve(mips);
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t sourceMip = m + firstMip;
        const size_t want = mc2tex::textureArrayLevelBytes(*a, sourceMip);
        if (want == 0 || a->levels[sourceMip].size() != want) {
            vlog("%s albedo array rejected: level %u holds %zu bytes, %zu expected.", tgt.tag, sourceMip, a->levels[sourceMip].size(), want);
            return false;
        }
        VkBufferImageCopy r{};
        r.bufferOffset = blob.size();
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, a->layers};
        r.imageExtent = {mc2tex::mipExtent(width, m), mc2tex::mipExtent(height, m), 1};
        regions.push_back(r);
        blob.insert(blob.end(), a->levels[sourceMip].begin(), a->levels[sourceMip].end());
    }
    const VkFormat vkFmt = static_cast<VkFormat>(a->vkFormat);

    // (Re)create sampler with the real mip count.
    if (!cp_make_albedo_sampler(s, tgt.samp, mips)) return false;

    if (!cp_upload_albedo_array(s, tgt, vkFmt, width, height, a->layers,
                                mips, compressed, blob.data(), static_cast<VkDeviceSize>(blob.size()), regions)) {
        return false;
    }
    *tgt.importedGlName = changeKey;
    *tgt.ready = true;
    vlog("%s albedo 2D_ARRAY imported: %ux%u layers=%u mips=%u fmt=%s (%zu B).",
         tgt.tag, width, height, a->layers, mips, a->vkFormat == 141u ? "BC5" : compressed ? "BC7" : "RGBA8", blob.size());
    return true;
}

// Texture/table replacement happens only at map transitions, after retiring
// earlier draws; descriptor writes remain deferred to each retired parity slot.
bool cp_import_authored(ColorPropIsland* s, const Mc2ColorPropMaterialFeed* feed) {
    const bool have = feed && feed->materialCount && feed->materials && feed->packetMaterialIds;
    auto upload = [&](mc2vk::ResidentTextureRef& texture, const mc2tex::TextureArrayCpu* array,
                      const uint8_t neutral[4]) {
        uint32_t layers = 0, key = 0; bool ready = false;
        AlbedoTarget target{&texture, &s->detailSampler, &layers, &key, &ready, "authored-detail"};
        if (!mc2::options::lowCost() && array && array->layers) return cp_import_albedo_array(s, target, s->mapGeneration + 1, array);
        std::vector<VkBufferImageCopy> regions(1);
        regions[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        regions[0].imageExtent = {1,1,1};
        return cp_upload_albedo_array(s, target, VK_FORMAT_R8G8B8A8_UNORM, 1,1,1,1,false,neutral,4,regions);
    };
    mc2vk::getOrCreateEngineVulkanContext().waitIdle("color_prop_authored_materials");
    if (!s->detailSampler && !mc2vk::infra::makeMaterialSampler(s->device, s->detailSampler, VK_SAMPLER_ADDRESS_MODE_REPEAT, s->sampBias)) return false;
    const uint8_t normal[4] = {128,128,255,255}, orm[4] = {255,255,0,255};
    mc2vk::ResidentTextureRef nextNormal, nextOrm;
    mc2vk::OwnedBuffer nextTable;
    if (!upload(nextNormal, have ? feed->normal : nullptr, normal) ||
        !upload(nextOrm, have ? feed->orm : nullptr, orm)) return false;
    RenderCore::PropMaterialGpu neutral{};
    const auto bytes = static_cast<VkDeviceSize>(have ? feed->materialCount : 1u) * sizeof(neutral);
    if (!make_buffer(s, nextTable, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, "colorprop.materials")) return false;
    void* mapped = nextTable.map();
    if (!mapped) return false;
    std::memcpy(mapped, have ? feed->materials : &neutral, static_cast<size_t>(bytes));
    std::vector<mc2vk::ResidentTextureRef> nextTextures;
    const bool texResident = have && feed->textureCount && s->propTexSlots > 1 && feed->textureCount <= s->propTexSlots;
    if (have && !texResident) {
        // Per-object rows index a texture table that is not resident here (one slot on Low/Potato, or more maps than the device has slots):
        // clear the flag and indices so the shader never reads them as detail-array layers or unbound descriptors.
        const size_t dropped = RenderCore::dropPerTextureRows(std::span<RenderCore::PropMaterialGpu>(static_cast<RenderCore::PropMaterialGpu*>(mapped), feed->materialCount));
        if (dropped) vlog("per-object textures not resident (%u maps, %u slots): %zu material rows fall back to class scalars.",
                          feed->textureCount, s->propTexSlots, dropped);
    }
    nextTable.unmap();
    if (texResident) {
        // Each map is its own image with its own mip chain. Potato drops the top mip from residency and staging.
        const uint32_t drop = mc2::options::current().baseQuality == 0 ? 1u : 0u;
        for (uint32_t ti = 0; ti < feed->textureCount; ++ti) {
            const RenderCore::KtxImage& src = feed->textures[ti];
            RenderCore::KtxImage image = src;
            uint32_t w = static_cast<uint32_t>(image.width), h = static_cast<uint32_t>(image.height);
            if (drop && image.mipCount > 1) {
                const uint64_t base = image.mipByteOffsets[1];
                image.pixels.erase(image.pixels.begin(), image.pixels.begin() + static_cast<ptrdiff_t>(base));
                image.mipByteOffsets.erase(image.mipByteOffsets.begin());
                for (auto& o : image.mipByteOffsets) o -= base;
                --image.mipCount; w = mc2tex::mipExtent(w, 1); h = mc2tex::mipExtent(h, 1);
                image.width = static_cast<int>(w); image.height = static_cast<int>(h);
            }
            uint64_t hash = 1469598103934665603ull;
            for (uint8_t byte : image.pixels) { hash ^= byte; hash *= 1099511628211ull; }
            RenderCore::TextureKey key;
            key.source = "prop-tex:" + std::to_string(hash) + ":" + std::to_string(w) + ":" + std::to_string(h) + ":" + std::to_string(image.vkFormat);
            key.format = image.vkFormat; key.hints = image.mipCount > 1 ? gosHint_MipmapFilter0 : 0;
            auto texture = mc2vk::getOrCreateEngineVulkanContext().textures().acquire(key, image, {w, h, w, h}, 1, true);
            if (!texture) { vlog("per-object texture upload failed (%ux%u fmt=%u).", w, h, image.vkFormat); return false; }
            nextTextures.push_back(std::move(texture));
        }
        if (!nextTextures.empty()) vlog("per-object textures resident: %zu.", nextTextures.size());
    }
    // The old maps release through TextureResidency::release, which retires each image on the open-frame ticket (and waitIdle above already drained).
    s->propTextures = std::move(nextTextures);
    s->normalTexture = std::move(nextNormal);
    s->ormTexture = std::move(nextOrm);
    s->materialBuf = std::move(nextTable);
    s->packetMaterialIds.clear();
    if (have) s->packetMaterialIds.assign(feed->packetMaterialIds, feed->packetMaterialIds + feed->packetCount);
    s->authoredReady = feed != nullptr;
    write_descriptors(s);
    return true;
}

// Upload the per-type albedo-layer table (int32 per type) into a layer SSBO
// (alpha-OFF binding2, or alpha-ON binding5).
static bool cp_replace_buffer(ColorPropIsland* s, mc2vk::OwnedBuffer& buf, VkDeviceSize size, std::string_view name) {
    mc2vk::OwnedBuffer fresh;
    if (!make_buffer(s, fresh, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, name)) return false;
    if (buf) mc2vk::getOrCreateEngineVulkanContext().retire(std::move(buf), size);
    buf = std::move(fresh);
    return true;
}
bool cp_upload_layer_table_into(ColorPropIsland* s, mc2vk::OwnedBuffer& buf,
                                VkDeviceSize* capBytes, const int32_t* layers, uint32_t count) {
    const VkDeviceSize bytes = static_cast<VkDeviceSize>((count ? count : 1)) * sizeof(int32_t);
    if (!buf || bytes > *capBytes) {
        // Growth retires the old table on the open-frame ticket (an in-flight submit may still read it); the descriptors repoint lazily per parity.
        const VkDeviceSize cap = std::bit_ceil(static_cast<uint64_t>(bytes < 64 ? 64 : bytes));
        if (!cp_replace_buffer(s, buf, cap, "colorprop.layerTable")) return false;
        *capBytes = cap;
        if (s->inited) write_descriptors(s);
    }
    void* p = buf.map();
    if (!p) return false;
    // The binding is VK_WHOLE_SIZE and the shader bounds on .length(): zero the pow2 slack so stale entries never read as valid types.
    std::memset(p, 0, static_cast<size_t>(*capBytes));
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
    if (!s->hotBuf || bytes > s->hotCapBytes) {
        // The placeholder grows into the real table on the first upload; the old table retires on the open-frame ticket.
        const VkDeviceSize cap = std::bit_ceil(static_cast<uint64_t>(bytes < 64 ? 64 : bytes));
        if (!cp_replace_buffer(s, s->hotBuf, cap, "colorprop.hotColors")) return false;
        s->hotCapBytes = cap;
        rebound = true;
    }
    void* p = s->hotBuf.map();
    if (!p) return false;
    // Zero the pow2 slack too: the binding is VK_WHOLE_SIZE and the shader bounds on .length().
    std::memset(p, 0, static_cast<size_t>(s->hotCapBytes));
    if (!colours.empty()) std::memcpy(p, colours.data(), static_cast<size_t>(bytes));
    s->hotBuf.unmap();
    s->hotCpu = colours;
    if (rebound && s->inited) write_descriptors(s);
    return true;
}

// One-time init.
bool init_island(ColorPropIsland* s, int w, int h) {
    s->w = w; s->h = h;
    ++g_health.attempted;
    g_propInitReachedBuild = 0;

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
        // co-writes props into these SAME images in-place). Any missing => not ready yet,
        // retry next frame. ----
        s->t1_colorRes  = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
        s->t1_normalRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_NORMAL);
        s->t1_depthRes  = mc2SharedSceneDepthGetResource();
        s->t1_idRes     = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_OBJECT_ID);
        if (!s->t1_idRes && RenderWorld::IsObjectIdBufferEnabled()) {
            setFallback("tier1_object_id_not_ready");   // the id image is realised a frame or two after the colour; retry
            return false;
        }
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
        g_propInitReachedBuild = 1;
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

        VkImageResource* idRes = reinterpret_cast<VkImageResource*>(s->t1_idRes);
        const bool haveId = idRes && idRes->view.get() != VK_NULL_HANDLE;
        VkAttachmentDescription att[4]{};
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

        // OBJECT-ID-1: attachment 3 = shared R32_UINT object id, LOAD (the terrain island zeroed it this frame), STORE. With the id buffer
        // off the subpass slot is UNUSED and the pipelines mask it, so one shader serves both.
        if (haveId) {
            att[3].format = idRes->format; att[3].samples = VK_SAMPLE_COUNT_1_BIT;
            att[3].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[3].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att[3].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            att[3].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            att[3].finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        s->t1_haveId = haveId;
        VkAttachmentReference colorRefs[3] = {
            {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
            {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
            {haveId ? 3u : VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        };
        VkAttachmentReference depthRef{2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 3; sub.pColorAttachments = colorRefs;
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
        rpci.attachmentCount = haveId ? 4 : 3; rpci.pAttachments = att;
        rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        if (!mc2vk::create(s->t1_rpass, s->device, [&](VkRenderPass* h) { return vkCreateRenderPass(s->device, &rpci, nullptr, h); })) {
            vlog("TIER-1 vkCreateRenderPass failed."); setFallback("tier1_renderpass_failed"); return false;
        }
        VkImageView fbViews[4] = { colorRes->view.get(), normalRes->view.get(), depthRes->view.get(), haveId ? idRes->view.get() : VK_NULL_HANDLE };
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->t1_rpass.get(); fci.attachmentCount = haveId ? 4 : 3; fci.pAttachments = fbViews;
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
        {
            // Non-update-after-bind layouts must fit maxPerStageDescriptorSamplers (HD 630: 64). Low/Potato never upload per-object maps.
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(s->phys, &props);
            s->drawIndirectLimit = props.limits.maxDrawIndirectCount;
            const uint32_t room = std::min(props.limits.maxPerStageDescriptorSamplers, props.limits.maxPerStageDescriptorSampledImages);
            s->propTexSlots = mc2::options::lowCost() ? 1u : std::clamp(room > 32u ? room - 32u : 1u, 1u, kPropTextureSlots);
            // Test knob: force a small slot count to exercise the more-maps-than-slots path (0 = off). Never raises the device-derived count.
            const int forced = static_cast<int>(mc2::gate::get<"MC2_VK_PROP_TEX_SLOTS">());
            if (forced > 0) s->propTexSlots = std::min(s->propTexSlots, static_cast<uint32_t>(forced));
        }
        VkDescriptorSetLayoutBinding b[32]{};
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
        }
        b[nb].binding = 9; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[nb].descriptorCount = 1; b[nb].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; ++nb;   // the HZB visibility words feed the compaction pass
        if (s->tier1) {   // GPU-driven props: run table, compacted slots, indirect commands (the packet rows are read by device address, no binding)
            b[nb].binding = 16; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[nb].descriptorCount = 1; b[nb].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; ++nb;
            b[nb].binding = 17; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[nb].descriptorCount = 1; b[nb].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT; ++nb;
            b[nb].binding = 18; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[nb].descriptorCount = 1; b[nb].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; ++nb;
        }
        b[nb].binding = 10; b[nb].descriptorCount = 1;
        b[nb].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[nb].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; ++nb;
        b[nb].binding = 11; b[nb].descriptorCount = 1;
        b[nb].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[nb].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; ++nb;
        b[nb].binding = 12; b[nb].descriptorCount = 1;
        b[nb].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[nb].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; ++nb;
        b[nb].binding = 14; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[nb].descriptorCount = 1; b[nb].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; ++nb;
        b[nb].binding = 24; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[nb].descriptorCount = 1; b[nb].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; ++nb;
        b[nb].binding = 25; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[nb].descriptorCount = 1; b[nb].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; ++nb;
        // Per-object texture table (binding 13): descriptor-indexing array, only the slots a map uses are written.
        const uint32_t kTexBinding = 13;
        b[nb].binding = kTexBinding; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[nb].descriptorCount = s->propTexSlots; b[nb].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; ++nb;
        VkDescriptorBindingFlags bindFlags[20]{};
        for (uint32_t k = 0; k < nb; ++k)
            if (b[k].binding == kTexBinding) bindFlags[k] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        bfi.bindingCount = nb; bfi.pBindingFlags = bindFlags;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.pNext = &bfi;
        lci.bindingCount = nb; lci.pBindings = b;
        if (!mc2vk::create(s->dsl, s->device, [&](VkDescriptorSetLayout* h) { return vkCreateDescriptorSetLayout(s->device, &lci, nullptr, h); })) {
            vlog("vkCreateDescriptorSetLayout failed."); return false;
        }
        // VK-PROP-FENCE-DEFER-1: TWO sets (one per parity slot), so pool sizes double.
        // Dynamic: 0 UBO; 1 + 4 (lights) SSBO. Static SSBOs: layer tables (2, 5) and the hot-colour table (7).
        VkDescriptorPoolSize sizes[5]{};
        uint32_t nsz = 0;
        sizes[nsz++] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, mc2vk::kMaxFramesInFlight};
        sizes[nsz++] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, mc2vk::kMaxFramesInFlight * (1u + (lightBinding ? 1u : 0u))};
        sizes[nsz++] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, mc2vk::kMaxFramesInFlight * ((materialBindings ? 2u : 0u) + (s->tier1 ? 5u : 0u) + 2u)};
        sizes[nsz++] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, mc2vk::kMaxFramesInFlight * ((materialBindings ? 2u : 0u) + 4u + s->propTexSlots)};
        sizes[nsz++] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, mc2vk::kMaxFramesInFlight};
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = mc2vk::kMaxFramesInFlight; pci.poolSizeCount = nsz; pci.pPoolSizes = sizes;
        if (!mc2vk::create(s->pool, s->device, [&](VkDescriptorPool* h) { return vkCreateDescriptorPool(s->device, &pci, nullptr, h); })) {
            vlog("vkCreateDescriptorPool failed."); return false;
        }
        VkDescriptorSetLayout layouts[mc2vk::kMaxFramesInFlight]; for (auto& layout : layouts) layout = s->dsl.get();
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = s->pool.get(); dsai.descriptorSetCount = mc2vk::kMaxFramesInFlight; dsai.pSetLayouts = layouts;
        if (MC2_VK_ALLOC_SETS(s->device, &dsai, s->set) != VK_SUCCESS) {
            vlog("vkAllocateDescriptorSets failed."); return false;
        }
        if (s->tier1 && !cp_upload_hot_table(s, {})) { setFallback("hot_buf_failed"); return false; }
        if (!cp_import_authored(s, nullptr)) { setFallback("authored_material_dummy_failed"); return false; }
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
        // GPU-driven props: the per-packet data moved to the packet-row buffer, indexed by packetBase + gl_DrawID; the push is that buffer's device
        // address plus the first row of the list the indirect call draws (PropPacketPush; read by both stages).
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcr.offset = 0; pcr.size = sizeof(PropPacketPush);

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (!s->groundLighting.init(s->device)) return false;
        VkSamplerCreateInfo treeSampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        treeSampler.magFilter = treeSampler.minFilter = VK_FILTER_NEAREST;
        treeSampler.addressModeU = treeSampler.addressModeV = treeSampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        treeSampler.compareEnable = VK_TRUE; treeSampler.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        if (!mc2vk::create(s->treeShadowSampler, s->device, [&](VkSampler* out) { return vkCreateSampler(s->device, &treeSampler, nullptr, out); })) return false;
        const VkDescriptorSetLayout propLayouts[]{s->dsl.get(), s->groundLighting.layout()};
        plci.setLayoutCount = 2; plci.pSetLayouts = propLayouts;
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if (!mc2vk::create(s->playout, s->device, [&](VkPipelineLayout* h) { return vkCreatePipelineLayout(s->device, &plci, nullptr, h); })) {
            vlog("TIER-1 vkCreatePipelineLayout failed."); return false;
        }
        {   // The compaction pass: set 0 only (same layout, bound with the same dynamic offsets); the push is the HZB-active flag.
            s->cullShader = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "color_prop_cull.comp.spv"));
            if (!s->cullShader) { setFallback("tier1_cull_spv_load_failed"); return false; }
            const VkPushConstantRange cpcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
            const VkDescriptorSetLayout cullSetLayout = s->dsl.get();
            VkPipelineLayoutCreateInfo cli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            cli.setLayoutCount = 1; cli.pSetLayouts = &cullSetLayout; cli.pushConstantRangeCount = 1; cli.pPushConstantRanges = &cpcr;
            if (!mc2vk::create(s->cullLayout, s->device, [&](VkPipelineLayout* h) { return vkCreatePipelineLayout(s->device, &cli, nullptr, h); })) {
                vlog("TIER-1 cull vkCreatePipelineLayout failed."); return false;
            }
            VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = s->cullShader.get(); cpi.stage.pName = "main"; cpi.layout = s->cullLayout.get();
            if (!mc2vk::create(s->cullPipe, s->device, [&](VkPipeline* h) { return mc2vk::infra::compute(s->device, VK_NULL_HANDLE, 1, &cpi, nullptr, h); })) {
                vlog("TIER-1 cull compute pipeline failed."); return false;
            }
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
        VkPipelineColorBlendAttachmentState cba[3]{};
        for (int k = 0; k < 3; ++k) {
            cba[k].blendEnable = VK_FALSE;
            cba[k].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        }
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        if (!s->t1_haveId) cba[2].colorWriteMask = 0;
        cb.attachmentCount = 3; cb.pAttachments = cba;

        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsci{};
        dsci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dsci.dynamicStateCount = 2; dsci.pDynamicStates = dyn;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = s->t1_vert.get(); stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = s->t1_frag.get(); stages[1].pName = "main";
        // Compile the small-EU preset without material detail/specular and long light walks.
        // Constant 2 (kOpaqueOnly) is 0 here; the opaque pipeline below sets it to 1.
        int specData[3] = {mc2::options::lowCost() ? (mc2::options::current().baseQuality == 0 ? 0 : 1) : 2,
                           static_cast<int>(s->propTexSlots), 0};
        const VkSpecializationMapEntry qualityEntry[3] = {{0, 0, sizeof(int)}, {1, sizeof(int), sizeof(int)}, {2, 2 * sizeof(int), sizeof(int)}};
        const VkSpecializationInfo qualityInfo{3, qualityEntry, sizeof(specData), specData};
        stages[1].pSpecializationInfo = &qualityInfo;
        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2; gp.pStages = stages;
        gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds; gp.pColorBlendState = &cb;
        gp.pDynamicState = &dsci;
        gp.layout = s->playout.get(); gp.renderPass = s->t1_rpass.get(); gp.subpass = 0;
        if (!mc2vk::create(s->t1_pipe, s->device, [&](VkPipeline* h) { return mc2vk::infra::graphics(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h); })) {
            vlog("TIER-1 vkCreateGraphicsPipelines failed."); setFallback("tier1_pipeline_failed"); return false;
        }
        mc2vk::nameObject(s->device, s->t1_pipe.get(), "colorprop.tier1Pipe");
        // Opaque core pipeline: identical state (same depth compare, write and bias), FS discards compiled out.
        specData[2] = 1;
        if (!mc2vk::create(s->t1_opaquePipe, s->device, [&](VkPipeline* h) { return mc2vk::infra::graphics(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h); })) {
            vlog("TIER-1 opaque vkCreateGraphicsPipelines failed."); setFallback("tier1_pipeline_failed"); return false;
        }
        mc2vk::nameObject(s->device, s->t1_opaquePipe.get(), "colorprop.tier1OpaquePipe");
        specData[2] = 0;

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
        cba[2].colorWriteMask = 0;   // a fringe blends coverage the core pass cut; it never owns the pixel id
        if (!mc2vk::create(s->t1_fringePipe, s->device, [&](VkPipeline* h) { return mc2vk::infra::graphics(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h); })) {
            vlog("TIER-1 fringe vkCreateGraphicsPipelines failed."); setFallback("tier1_pipeline_failed"); return false;
        }
        mc2vk::nameObject(s->device, s->t1_fringePipe.get(), "colorprop.tier1FringePipe");
    }

    // ---- Command pool + 2 command buffers (per parity). ----
    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = 0;
        pci.queueFamilyIndex = s->gfxFamily;
        if (!mc2vk::create(s->cpool, s->device, [&](VkCommandPool* h) { return vkCreateCommandPool(s->device, &pci, nullptr, h); })) {
            vlog("vkCreateCommandPool failed."); return false;
        }
        for (uint32_t i = 0; i < mc2vk::kMaxFramesInFlight; ++i) {
            VkCommandBufferAllocateInfo cai{};
            cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
            if (!mc2vk::create(s->slotPool[i], s->device, [&](VkCommandPool* h) { return vkCreateCommandPool(s->device, &pci, nullptr, h); })) {
                vlog("vkCreateCommandPool failed."); return false;
            }
            cai.commandPool = s->slotPool[i].get();
            if (vkAllocateCommandBuffers(s->device, &cai, &s->cbuf[i]) != VK_SUCCESS) {
                vlog("vkAllocateCommandBuffers failed."); return false;
            }
            if (!mc2vk::create(s->latePool[i], s->device, [&](VkCommandPool* h) { return vkCreateCommandPool(s->device, &pci, nullptr, h); })) return false;
            cai.commandPool = s->latePool[i].get();
            if (vkAllocateCommandBuffers(s->device, &cai, &s->lateCbuf[i]) != VK_SUCCESS) return false;
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
    // Static geometry is read every frame. Keep the identical bytes in device
    // memory rather than the CPU-writable allocation createFilled() provides.
    mc2vk::OwnedBuffer stageVertices, stageIndices;
    // The RT scene (vk_rt_scene.h) builds the static-prop BLASes straight from this VBO.
    const VkBufferUsageFlags rtBuildInput = mc2vk::getOrCreateEngineVulkanContext().rt().accel
        ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR : 0;
    if (!stageVertices.createFilled(s->allocator, s->device, vboCpu, vboBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "colorprop.stage.vertices") ||
        !stageIndices.createFilled(s->allocator, s->device, iboCpu, iboBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "colorprop.stage.indices") ||
        !s->vbo.createDeviceLocal(s->allocator, s->device, vboBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | rtBuildInput,
                                  "colorprop.vbo") ||
        !s->ibo.createDeviceLocal(s->allocator, s->device, iboBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, "colorprop.ibo")) {
        vlog("geometry upload failed (buffer creation or map); retrying next frame.");
        s->vbo.reset();
        s->ibo.reset();
        return false;
    }
    auto& ctx = mc2vk::getOrCreateEngineVulkanContext();
    if (!mc2vk::submitOneShot(ctx, [&](VkCommandBuffer cb) {
        const VkBufferCopy vertices{0, 0, vboBytes}, indices{0, 0, iboBytes};
        vkCmdCopyBuffer(cb, stageVertices.get(), s->vbo.get(), 1, &vertices);
        vkCmdCopyBuffer(cb, stageIndices.get(), s->ibo.get(), 1, &indices);
        mc2vk::sync::cmdMemoryBarrier(ctx, cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                      VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
                                      VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT);
    }, "colorprop_geometry_upload")) {
        vlog("geometry upload failed (copy submit); retrying next frame.");
        s->vbo.reset();
        s->ibo.reset();
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
    mc2vk::rt::scene::invalidateProps();   // the prop BLASes and the TLAS over them go with the map
    ColorPropIsland* s = parkedIsland();
    if (!s) return;
    // The map's geometry retires on the open-frame ticket: in-flight submits may still draw it.
    if (s->vbo) mc2vk::getOrCreateEngineVulkanContext().retire(std::move(s->vbo));
    if (s->ibo) mc2vk::getOrCreateEngineVulkanContext().retire(std::move(s->ibo));
    s->geomUploaded = false;
    s->packets.clear();
    s->lateCount = 0;
    s->typeRuns.clear();
    // The albedo arrays and the hot-colour table belong to the map too: the next map's feed imports its own (the island itself outlives the map).
    // The previous map's two albedo arrays stay allocated until that import replaces them: the per-parity descriptor sets still name their views, and a set
    // naming a destroyed view must not be bound by a draw before the rewrite, so the GPU memory is retained across a menu stay rather than released here.
    s->materialReady = false;
    s->materialReadyOn = false;
    s->hotReady = false;
    s->hotCpu = {};
    s->authoredReady = false;
    s->authoredAttempted = false;
    s->packetMaterialIds.clear();
    ++s->mapGeneration;
    mc2vk::bumpDescEpoch();   // map change: every descriptor write cache rewrites once
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

extern "C" int mc2_color_prop_island_get_albedo_off_array(void** view, uint32_t* generation) {
    const ColorPropIsland* s = parkedIsland();
    if (!view || !generation || !s || !s->materialReady || !s->albedoTexture) return 0;
    if (!mc2vk::getOrCreateEngineVulkanContext().textures().ready(s->albedoTexture.get())) return 0;
    *view = reinterpret_cast<void*>(s->albedoTexture.view());
    *generation = s->mapGeneration;
    return 1;
}

extern "C" int mc2_vk_color_prop_island_enabled() {
    return 1;
}

static bool prepare_mission_resources(ColorPropIsland* s) {
    if (!upload_geometry_once(s)) { setFallback("geometry_not_ready"); return false; }

    // VK-P11 STATIC-PROP MATERIAL: copy-once import the alpha-OFF albedo 2D_ARRAY +
    // per-type layer table once geometry is finalized. Fail-soft: any miss (feed not
    // ready, BC7-bucketed, import failure) leaves materialReady=false -> FS keeps teal.
    if (s->wantMaterial && s->tier1) {
        Mc2ColorPropMaterialFeed mf{};
        if (mc2_color_prop_island_get_material_feed(&mf) && mf.arrayKeyOff != 0 && !mf.bucketed) {
            if (!s->authoredAttempted) {
                s->authoredAttempted = true;
                if (!cp_import_authored(s, &mf))
                    vlog("authored material upload failed; keeping legacy packet routing.");
            }
            s->terrainSunMC2[0] = mf.terrainSunMC2[0];
            s->terrainSunMC2[1] = mf.terrainSunMC2[1];
            s->terrainSunMC2[2] = mf.terrainSunMC2[2];
            if (s->materialReady && s->materialReadyOn) return true;
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

    return true;
}

extern "C" int mc2_vk_color_prop_island_prewarm(int w,int h) {
    auto& ctx=mc2vk::getOrCreateEngineVulkanContext();
    if(!ctx.valid() || w<=0 || h<=0) return 0;
    auto& slot=ctx.islandSlot(mc2vk::IslandSlot::ColorProp);
    if(!slot) {
        auto fresh=std::make_unique<ColorPropIsland>();
        if(!init_island(fresh.get(),w,h)) { ctx.waitIdle("color_prop_prewarm_failure"); return 0; }
        slot=std::move(fresh);
    }
    return prepare_mission_resources(static_cast<ColorPropIsland*>(slot.get())) ? 1 : 0;
}

// GPU-FRONTIER-1: pure command recording for the props pass (no staging, no submit, no fences). Everything it reads was prepared on
// the main thread by mc2_vk_color_prop_island_run; it builds the per-parity prop buffers and writes only the command buffer, the late-fringe range and typeRuns.
struct PropRecordIn {
    ColorPropIsland* s; VkCommandBuffer cb; int w, h; uint32_t par;
    uint32_t nDyn; const uint32_t* dynOffsets; const Mc2GroundLightingFrame* ground;
    bool hzbCulled; uint32_t instanceCount; const GpuStaticPropInstance* insts;
};
static bool record_props(const PropRecordIn& in, unsigned long& typeRunsOut) {
    ColorPropIsland* s = in.s; VkCommandBuffer cb = in.cb; const int w = in.w, h = in.h; const uint32_t par = in.par;
    const uint32_t nDyn = in.nDyn; const uint32_t* dynOffsets = in.dynOffsets; const Mc2GroundLightingFrame* ground = in.ground;
    const bool hzbCulled = in.hzbCulled; const uint32_t instanceCount = in.instanceCount; const GpuStaticPropInstance* insts = in.insts;
    // GPU-driven props: build the packet rows, run table and the three indirect draw lists on the CPU (every packet of every type run, in the
    // old draw order), then let the compaction pass fill each command's instanceCount from the HZB visibility words.
    for (auto& v : s->cmdScratch) v.clear();
    for (auto& v : s->pktScratch) v.clear();
    s->runScratch.clear();

    FILE* s_pktDump = nullptr;
    {   // ADJUDICATION-1 draw-level dump: env-gated one-shot CSV of every core packet's draw parameters and layers (sim-freeze gated).
        static bool s_dumped = false;
        const std::string dpText = mc2::gate::str<"MC2_VK_PROP_PACKET_DUMP">();   // "" when unset or empty
        const char* dp = dpText.empty() ? nullptr : dpText.c_str();
        const bool frozen = mc2CaptureGaitFreezeActive() != 0 ||
                            !mc2::gate::isSet<"MC2_VISUAL_BOOKMARK_CAPTURE">();
        if (dp && dp[0] && !s_dumped && frozen) {
            s_dumped = true;
            s_pktDump = std::fopen(dp, "w");
            if (s_pktDump)
                mc2::fmt::print(s_pktDump, "typeID,packet,layerOff,layerOn,uvScaleX,uvScaleY,indexCount,firstIndex,baseVertex,instCount,firstInstance\n");
        }
    }

    unsigned long typeRuns = 0;
    {
        uint32_t i = 0;
        while (i < instanceCount) {
            const uint32_t tid = insts[i].typeID;
            uint32_t j = i;
            while (j < instanceCount && insts[j].typeID == tid) ++j;
            const uint32_t instCount = j - i;
            if (tid < s->typeRuns.size()) {
                const TypeRun& tr = s->typeRuns[tid];
                PropRunRow rr{i, instCount, {}, {}};
                for (uint32_t list = 0; list < kListCount; ++list) rr.range[list][0] = static_cast<uint32_t>(s->cmdScratch[list].size());
                bool counted = false;
                // Opaque core list: the type's packets before its first alpha-testable one. The rest of the type stays in the
                // alpha core list in packet order, so a coplanar later packet still wins the GEQUAL tie against an earlier one.
                bool opaquePrefix = true;
                // OBJECT-ID-1: the RenderWorld handle for this type (recipe index, generation 1); 0 when the type has no recipe.
                const int32_t typeObjectId = static_cast<int32_t>(RenderWorld::objectIdRawForStaticPropRecipe(
                    GpuStaticPropRegistry::getRecipeIndexForType(tr.typeID)));
                for (uint32_t p = 0; p < tr.packetCount; ++p) {
                    const uint32_t pk = tr.firstPacket + p;
                    if (pk >= s->packets.size()) break;
                    const Mc2ShadowPacket& pkt = s->packets[pk];
                    // BLACK-FENCE FIX: this packet's own group-relative albedo layer (off/on), not the type's shared first-valid-wins layer.
                    // Its own uvScaleX/Y, default 1.0 (no crop) for any packet the feed did not cover.
                    PropPacketRow row{
                        (pk < s->packetLayerOff.size())  ? s->packetLayerOff[pk]  : -1,
                        (pk < s->packetLayerOn.size())   ? s->packetLayerOn[pk]   : -1,
                        (pk < s->packetUvScaleX.size())  ? s->packetUvScaleX[pk]  : 1.0f,
                        (pk < s->packetUvScaleY.size())  ? s->packetUvScaleY[pk]  : 1.0f,
                        mc2_static_prop_packet_alpha_test(pk),
                        0,
                        s->authoredReady && pk < s->packetMaterialIds.size() ? s->packetMaterialIds[pk] : -1,
                        typeObjectId,
                    };
                    const PropDrawCmd cmd{pkt.indexCount, instCount, pkt.firstIndex, static_cast<int32_t>(pkt.baseVertex), i};
                    // The core pass draws every packet (alpha-tested ones cut at the coverage threshold).
                    const bool alphaTestable = row.layerOn >= 0 || row.alphaTest != 0;
                    opaquePrefix = opaquePrefix && !alphaTestable;
                    const uint32_t coreList = opaquePrefix ? kListOpaque : kListCore;
                    s->cmdScratch[coreList].push_back(cmd); s->pktScratch[coreList].push_back(row); ++rr.range[coreList][1];
                    if (s_pktDump)
                        mc2::fmt::print(s_pktDump, "{:d},{:d},{:d},{:d},{:.4f},{:.4f},{:d},{:d},{:d},{:d},{:d}\n",
                                     mc2::fmt::as_u32(tid), mc2::fmt::as_u32(pk), mc2::fmt::as_i32(row.layerOff), mc2::fmt::as_i32(row.layerOn), row.uvScaleX, row.uvScaleY,
                                     mc2::fmt::as_u32(pkt.indexCount), mc2::fmt::as_u32(pkt.firstIndex),
                                     mc2::fmt::as_i32(static_cast<int32_t>(pkt.baseVertex)), mc2::fmt::as_u32(instCount), mc2::fmt::as_u32(i));
                    // PROP-COVERAGE-1 pass 1 re-draws only the packets the FS can alpha-test (alpha-ON layer or the per-packet bit) with the
                    // fringe pipeline, after every core is in the depth buffer; alphaTest == 2 packets wait for the late pass.
                    if (alphaTestable) {
                        row.fringe = 1;
                        const uint32_t list = row.alphaTest == 2 ? kListLate : kListFringe;
                        s->cmdScratch[list].push_back(cmd); s->pktScratch[list].push_back(row);
                        ++rr.range[list][1];
                    }
                    // Native color leaves include cached registry replay; count once before GPU HZB/alpha rejection.
                    if (!counted && pkt.indexCount && mc2::battlebench::output) {
                        mc2::battlebench::nativePropLeaves += instCount;
                        counted = true;
                    }
                }
                if (rr.range[kListOpaque][1] + rr.range[kListCore][1] + rr.range[kListFringe][1] + rr.range[kListLate][1] != 0)
                    s->runScratch.push_back(rr);
                ++typeRuns;
            }
            i = j;
        }
    }
    if (s_pktDump) { std::fclose(s_pktDump); s_pktDump = nullptr; }

    // Each list's first command in the buffer (lists laid out in kList order); run ranges become absolute.
    uint32_t listStart[kListCount + 1] = {};
    for (uint32_t list = 0; list < kListCount; ++list)
        listStart[list + 1] = listStart[list] + static_cast<uint32_t>(s->cmdScratch[list].size());
    for (PropRunRow& rr : s->runScratch)
        for (uint32_t list = 0; list < kListCount; ++list) rr.range[list][0] += listStart[list];
    const uint32_t cmdTotal = listStart[kListCount];
    const uint32_t lateTotal = static_cast<uint32_t>(s->cmdScratch[kListLate].size());
    s->lateStart = listStart[kListLate];
    s->lateCount = (std::min)(lateTotal, s->drawIndirectLimit);
    const uint32_t counts[kListCount] = {listStart[kListOpaque + 1] - listStart[kListOpaque], listStart[kListCore + 1] - listStart[kListCore],
                                         listStart[kListFringe + 1] - listStart[kListFringe], s->lateCount};

    // The per-parity buffers grow with headroom; a slot's previous submit and late fringe were waited before staging, so recreating is safe.
    {
        auto& g = s->gpu[par];
        bool rebound = false;
        auto ensure = [&](mc2vk::OwnedBuffer& buf, VkDeviceSize& cap, VkDeviceSize need, VkBufferUsageFlags usage, bool host, std::string_view name) {
            if (buf && need <= cap) return true;
            const VkDeviceSize cp = std::max<VkDeviceSize>(need + need / 2, 4096);
            buf.reset();
            if (!make_buffer(s, buf, cp, usage, host, name.data(), static_cast<int>(par))) return false;
            cap = cp; rebound = true;
            return true;
        };
        const VkDeviceSize pktBytes = static_cast<VkDeviceSize>(cmdTotal ? cmdTotal : 1) * sizeof(PropPacketRow);
        const VkDeviceSize runBytes = static_cast<VkDeviceSize>(s->runScratch.empty() ? 1 : s->runScratch.size()) * sizeof(PropRunRow);
        const VkDeviceSize compactBytes = static_cast<VkDeviceSize>(instanceCount) * sizeof(uint32_t);
        const VkDeviceSize cmdBytes = kPropCmdHeader + static_cast<VkDeviceSize>(cmdTotal) * sizeof(PropDrawCmd);
        if (!ensure(g.pkt, g.pktCap, pktBytes, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true, "colorprop.packets") ||
            !ensure(g.run, g.runCap, runBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, "colorprop.runs") ||
            !ensure(g.compact, g.compactCap, compactBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, "colorprop.compact") ||
            !ensure(g.cmd, g.cmdCap, cmdBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, true, "colorprop.indirect")) return false;
        if (rebound) {
            const VkBuffer bufs[3] = {g.run.get(), g.compact.get(), g.cmd.get()};
            VkDescriptorBufferInfo infos[3];
            VkWriteDescriptorSet wr[3]{};
            for (uint32_t k = 0; k < 3; ++k) {
                infos[k] = {bufs[k], 0, VK_WHOLE_SIZE};
                wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                wr[k].dstSet = s->set[par]; wr[k].dstBinding = 16 + k; wr[k].descriptorCount = 1;
                wr[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[k].pBufferInfo = &infos[k];
            }
            vkUpdateDescriptorSets(s->device, 3, wr, 0, nullptr);
        }
        // Rows and commands are laid out opaque, core, fringe, late; the cmd buffer starts with the four list counts.
        VkDeviceSize pOff = 0, cOff = kPropCmdHeader;
        bool ok = g.cmd.upload(counts, sizeof(counts), 0);
        for (uint32_t list = 0; list < kListCount; ++list) {
            const auto& rows = s->pktScratch[list]; const auto& cmds = s->cmdScratch[list];
            if (!rows.empty()) ok = g.pkt.upload(rows.data(), rows.size() * sizeof(PropPacketRow), pOff) && ok;
            if (!cmds.empty()) ok = g.cmd.upload(cmds.data(), cmds.size() * sizeof(PropDrawCmd), cOff) && ok;
            pOff += rows.size() * sizeof(PropPacketRow); cOff += cmds.size() * sizeof(PropDrawCmd);
        }
        if (!s->runScratch.empty()) ok = g.run.upload(s->runScratch.data(), s->runScratch.size() * sizeof(PropRunRow), 0) && ok;
        if (!ok) return false;
    }
    const VkBuffer cmdBuf = s->gpu[par].cmd.get();

        vkResetCommandPool(s->device, s->slotPool[par].get(), 0);   // cb is s->cbuf[par]
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return false;
        vk_render_pass_timer::Begin(cb, gos_render_pass_timer::Pass_SpColor);

        // Compaction: the HZB pass's last barrier targets VERTEX|HOST, so this pass first orders itself after the HZB compute writes, and the draw
        // stages (vertex shader reads the compacted slots, the indirect read takes the counts and instanceCounts) after its own writes.
        if (!s->runScratch.empty()) {
            MC2_VK_GPU_SCOPE(cullScope, cb, "VKIsl.Props.Cull");
            VkMemoryBarrier pre{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            pre.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; pre.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &pre, 0, nullptr, 0, nullptr);
            mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s->cullPipe.get());
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s->cullLayout.get(), 0, 1, &s->set[par], nDyn, dynOffsets);
            const uint32_t hzbFlag = hzbCulled ? 1u : 0u;
            vkCmdPushConstants(cb, s->cullLayout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(hzbFlag), &hzbFlag);
            vkCmdDispatch(cb, static_cast<uint32_t>(s->runScratch.size()), 1, 1);
            VkMemoryBarrier post{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            post.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; post.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                                 0, 1, &post, 0, nullptr, 0, nullptr);
            cullScope.end();
        }

        // Begin the LOAD renderpass — NO clear values (all attachments loadOp=LOAD).
        VkRenderPassBeginInfo rbi{};
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = s->t1_rpass.get(); rbi.framebuffer = s->t1_fb.get();
        rbi.renderArea = {{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
        rbi.clearValueCount = 0; rbi.pClearValues = nullptr;
        MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Props");
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);

        float jpx[2];
        RenderCore::temporal::sceneJitterPixels(g_mc2RenderFrame, jpx);
        VkViewport vp{jpx[0], jpx[1], static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &scissor);

        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &s->set[par], nDyn, dynOffsets);
        const VkDescriptorSet groundSet = s->groundLighting.set(par);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 1, 1, &groundSet, 4, ground->offsets.data());
        VkDeviceSize vboOff = 0;
        const VkBuffer vertexBuffer = s->vbo.get();
        vkCmdBindVertexBuffers(cb, 0, 1, &vertexBuffer, &vboOff);
        vkCmdBindIndexBuffer(cb, s->ibo.get(), 0, VK_INDEX_TYPE_UINT32);

        // One indirect-count call per list; maxDrawCount is clamped to the device limit (a loop-wrap hazard on AMD when it is UINT32_MAX).
        auto drawList = [&](VkPipeline pipe, uint32_t first, uint32_t n, uint32_t listWord) {
            if (n == 0) return;
            mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            const PropPacketPush push{s->gpu[par].pkt.deviceAddress(), first, 0};
            vkCmdPushConstants(cb, s->playout.get(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            mc2::battlebench::add(mc2::battlebench::DrawProps);
            vkCmdDrawIndexedIndirectCount(cb, cmdBuf, kPropCmdHeader + static_cast<VkDeviceSize>(first) * sizeof(PropDrawCmd), cmdBuf,
                                          static_cast<VkDeviceSize>(listWord) * sizeof(uint32_t), (std::min)(n, s->drawIndirectLimit), sizeof(PropDrawCmd));
        };
        // Opaque core first so early Z rejects the hidden fragments of everything after it.
        drawList(s->t1_opaquePipe.get(), listStart[kListOpaque], counts[kListOpaque], kListOpaque);
        drawList(s->t1_pipe.get(), listStart[kListCore], counts[kListCore], kListCore);
        drawList(s->t1_fringePipe.get(), listStart[kListFringe], counts[kListFringe], kListFringe);

        vkCmdEndRenderPass(cb);
        gpuScope.end();
        vk_render_pass_timer::End(cb, gos_render_pass_timer::Pass_SpColor);
        if (vkEndCommandBuffer(cb) != VK_SUCCESS) return false;
    typeRunsOut = typeRuns;
    return true;
}

extern "C" int mc2_vk_color_prop_island_run(int w, int h) {
    if (auto* previous = parkedIsland()) previous->lateCount = 0;
    ZoneScopedN("VK-props-island");   // VK-FLIP-FIELD-REPORT-1 coarse region zone
    mc2_hitch2::Scope hitchTotal("props.island");
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
        (void)mc2vk::takeLastCreateFailure();
        if (!init_island(fresh.get(), w, h)) {
            slotCtx.waitIdle("color_prop_init_failure");   // objects made before the failure may have been submitted
            mc2vk::onIslandInitFailure(mc2vk::IslandKind::ColorProp, g_propInitReachedBuild != 0,
                                       mc2vk::takeLastCreateFailure(), g_health.fallbackReason);
            return 0;
        }
        mc2vk::islandInitFailures(mc2vk::IslandKind::ColorProp).reset();
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

    if (!prepare_mission_resources(s)) return 0;
    mc2vk::rt::scene::updateProps(s->vbo.deviceAddress());   // no-op without ray query or while the registry generation is unchanged

    // Frame-ring allocations already belong to the current engine frame slot.
    // Private commands and descriptors retire against this island's submissions,
    // independently of the shared image fence that may name current-frame terrain.
    Mc2ColorPropLightFeed lightFeed{};
    const bool haveLightFeed = mc2_color_prop_island_get_light_feed(&lightFeed);
    mc2vk::VulkanContext& rctx = mc2vk::getOrCreateEngineVulkanContext();
    const VkDeviceSize ringAlign = rctx.frameAlignment();
    const uint32_t instanceCount = feed.count;
    const VkDeviceSize instBytes = static_cast<VkDeviceSize>(instanceCount) * sizeof(GpuStaticPropInstance);
    int poolN = (haveLightFeed && lightFeed.poolLights48 && lightFeed.numPoolLights > 0) ? lightFeed.numPoolLights : 0;
    poolN = std::min(poolN, kMaxPoolLights);
    const VkDeviceSize instOff = mc2vk::alignUp(sizeof(ColorFrameUBO), ringAlign);
    const mc2vk::FrameAlloc mainSpan = rctx.frameAlloc(instOff + (instBytes ? instBytes : sizeof(GpuStaticPropInstance)));
    if (!mainSpan) { setFallback("frame_ring_alloc_failed"); s->disabled = true; return 0; }
    uint8_t* const mainPtr = static_cast<uint8_t*>(mainSpan.ptr);

    const clk::time_point tFence0 = clk::now();
    const uint32_t par = rctx.frameContext().slot;
    {
        ZoneScopedN("VKIsl.Props.FenceWait"); mc2::battlebench::WaitScope gpuWait1(mc2::battlebench::WaitGpuMainNs);
        if (!vk_island_util::waitStrike(s->waitStrikes, s->disabled, rctx.wait(s->submitted[par], 5000000000ull, "prop submission slot"), "VK_PROP")) {
            setFallback("submission_slot_timeout"); vlog("LOUD: submission slot wait timed out (5 s); skipping this frame, will retry next frame."); return 0;
        }
        // Late coverage also reads this slot's descriptors and frame inputs.
        if (!vk_island_util::waitStrike(s->waitStrikes, s->disabled, [&] { mc2_hitch2::FenceWaitScope lateWait("color_prop_late_fence", mc2_hitch2::kSlot); return rctx.wait(s->lateSubmitted[par], 5000000000ull, "prop late submission slot"); }(), "VK_PROP")) {
            setFallback("late_fringe_fence_timeout"); vlog("LOUD: late fringe fence wait timed out (5 s); skipping this frame, will retry next frame."); return 0;
        }
    }
    us_fence_wait = usSince(tFence0);
    cp_refresh_samplers(s);
    // Props do not depend on terrain readiness: with no ground-lighting frame this frame (terrain not inited, fell back, or a different frame) reuse
    // the last frame's offsets over the descriptors this island's own set already holds; only a run that never had one skips the draw.
    auto groundNow = mc2_vk_ground_lighting_frame();
    if (groundNow) { s->lastGround = *groundNow; s->groundMissing = false; s->groundMissingFrames = 0; }
    else if (++s->groundMissingFrames > 2) {   // terrain gone (teardown, ring growth, mission change): the cached offsets and copied set may name freed/recycled resources
        s->lastGround.reset(); for (auto& copied : s->groundCopied) copied = false;
        s->groundLighting.invalidate();
    }
    if (!groundNow && !s->groundMissing) {
        s->groundMissing = true;
        vlog("no terrain ground-lighting frame this frame; %s.", s->lastGround ? "reusing the last ground lighting" : "props skipped until the first one");
    }
    // The copy reads the terrain island's descriptor set, so it runs only from a LIVE frame (a cached set handle can outlive a terrain teardown);
    // a parity that never copied one has nothing valid to bind and skips the draw.
    if (groundNow) { s->groundLighting.copy(s->device, par, *groundNow); s->groundCopied[par] = true; }
    if (!s->lastGround || !s->groundCopied[par] || !mc2vk::bindMoverShadowDescriptors(s->device, s->set[par], &s->moverStamp[par])) return 0;
    const Mc2GroundLightingFrame* ground = &*s->lastGround;
    const auto objectView = reinterpret_cast<VkImageView>(mc2_vk_shadow_island_get_object_view());
    if (objectView) {
        // Binding 14 only changes with the bake's view (or after a map change / descriptor rewrite, which clear this): skip the identical write.
        if (s->descriptorsDirty[par] || s->boundObjectView[par] != objectView ||s->boundObjectViewMap[par] != s->mapGeneration) {
        s->boundObjectView[par] = objectView; s->boundObjectViewMap[par] = s->mapGeneration;
        VkDescriptorImageInfo image{s->treeShadowSampler.get(), objectView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet=s->set[par]; write.dstBinding=14; write.descriptorCount=1;
        write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo=&image;
        vkUpdateDescriptorSets(s->device,1,&write,0,nullptr);
        }
        s->copy14Stamp[par].invalidate();   // 14 now holds the object view, not a copy of 24
    } else {
        s->boundObjectView[par] = VK_NULL_HANDLE;
        // Re-copy only when 24 changed (its stamp revision) or the map did.
        if (s->copy14Stamp[par].needsWrite({1, s->moverStamp[par].rev, s->mapGeneration, mc2vk::descKeyWord(s->set[par])})) {
            VkCopyDescriptorSet copy{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};
            copy.srcSet=copy.dstSet=s->set[par]; copy.srcBinding=24; copy.dstBinding=14; copy.descriptorCount=1;
            vkUpdateDescriptorSets(s->device,0,nullptr,1,&copy);
            mc2vk::noteDescWrites(1,0);
        } else mc2vk::noteDescWrites(0,1);
    }
    const std::array<VkImageView,2> views{s->albedoTexture.view(),s->albedoOnTexture.view()};
    if (s->descriptorsDirty[par] || s->boundTextures[par]!=views) {
        write_descriptors(s,static_cast<int>(par)); s->boundTextures[par]=views;
    }
    void* hzbMask = nullptr;
    uint64_t hzbMaskGeneration = 0;
    gosPostProcess* pp = getGosPostProcess();
    const bool hzbCulled = pp && pp->runVkPostHzbNative(feed, s->mapGeneration, &hzbMask, &hzbMaskGeneration);
    VkDescriptorBufferInfo maskInfo{hzbCulled ? reinterpret_cast<VkBuffer>(hzbMask) : s->hotBuf.get(),0,VK_WHOLE_SIZE};
    VkWriteDescriptorSet maskWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    maskWrite.dstSet = s->set[par]; maskWrite.dstBinding = 9; maskWrite.descriptorCount = 1;
    maskWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; maskWrite.pBufferInfo = &maskInfo;
    auto& boundMask = s->boundMask[par];
    const bool maskUnchanged = boundMask.set == maskWrite.dstSet &&
        boundMask.info.buffer == maskInfo.buffer && boundMask.info.offset == maskInfo.offset &&
        boundMask.info.range == maskInfo.range && boundMask.generation == hzbMaskGeneration &&
        boundMask.mapGeneration == s->mapGeneration;
    if (TracyIsConnected) {
        static thread_local uint64_t calls = 0, unchanged = 0;
        ++calls;
        unchanged += maskUnchanged;
        if (calls % 256 == 0)
            mc2::fmt::print_err("[DRAWSCREEN_MASK] calls={:d} unchanged={:d} writes={:d} items={:d}\n",
                         mc2::fmt::as_u64(static_cast<unsigned long long>(calls)), mc2::fmt::as_u64(static_cast<unsigned long long>(unchanged)),
                         mc2::fmt::as_u64(static_cast<unsigned long long>(calls - unchanged)), mc2::fmt::as_u32(instanceCount));
    }
    if (!maskUnchanged) {
        ZoneScopedN("VKIsl.Props.MaskDescriptorWrite");
        vkUpdateDescriptorSets(s->device,1,&maskWrite,0,nullptr);
        boundMask = {maskWrite.dstSet, maskInfo, hzbMaskGeneration, s->mapGeneration};
    }


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
    const bool nativeLight = haveLightFeed && lightFeed.lights.data &&
                             lightFeed.lights.bytes >= 3600ull;
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
        const VkDeviceSize lbytes = static_cast<VkDeviceSize>(lightFeed.lights.bytes);
        lightSpan = mc2vkSharedLightFeedAcquire(lightFeed.lights);
        if (lightSpan) {
            // Probe reads the CPU source -- byte-identical to the ring copy
            // (single-threaded copy of the same array this same frame).
            prop_light_vertex_probe(static_cast<const uint8_t*>(lightFeed.lights.data), lbytes, feed);
            s->lightingReady = true;
        } else {
            ZoneScopedN("VKIsl.Props.LightFeed");   // VK-ISLAND-TRACY-1: per-frame LightsData CPU->VK copy (own-span path)
            lightSpan = rctx.frameAlloc(lbytes);
            if (lightSpan) {
                std::memcpy(lightSpan.ptr, lightFeed.lights.data, static_cast<size_t>(lbytes));
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
        // Match the interpolated poses, using only the extracted sim snapshot.
        // Seconds stay seconds: the shader's wind speed and amplitude are unchanged.
        const auto& clock = mc2::render_view::clock();
        u.terrainSunMC2[3] = std::max(0.0f, clock.scenarioTime -
            (1.0f - clock.alpha) * mc2::sim::kTickSecondsF);
        // VK-PROP-AMBIENT-IBL-1: IBL-SH coeffs (std140 vec3[9] tail, .w pad each) +
        // strength + lightingReady. lightingReady gates the vert shader's real
        // ObjectLights ambientTerm sum; iblShStrength==0.0 short-circuits evalShL2
        // exactly like the GL guard (byte-identical when the feature/env is off).
        if (haveLightFeed) {
            std::copy_n(lightFeed.sunColor, 4, u.sunColor);
            std::copy_n(lightFeed.ambientColor, 4, u.ambientColor);
            std::copy_n(lightFeed.specDir, 4, u.specDir);
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
        u.nightDarkness = mc2NightLightFactor();   // the retired ambient-fill slot; same value the shadows and water use
        u.pbrV1Strength = haveLightFeed ? lightFeed.pbrV1Strength : 0.0f;
        u.pbrV1RoughnessOverride = haveLightFeed ? lightFeed.pbrV1RoughnessOverride : -1.0f;
        for (int k = 0; k < 4; ++k) u.cameraWorldPos[k] = haveLightFeed ? lightFeed.cameraWorldPos[k] : 0.0f;
        // Diagnostic selector in the unused eye-position component; xyz stays the lighting eye.
        u.cameraWorldPos[3] = static_cast<float>(static_cast<int>(mc2::gate::get<"MC2_PROP_DEBUG_VIEW">()));
        u.poolF[0] = lightFeed.poolMoverDim; u.poolF[1] = lightFeed.poolMoverScale; u.poolF[2] = lightFeed.poolObjScale; u.poolF[3] = mc2::options::current().shadowQuality == 0 ? 0.0f : objectView ? 2.0f : 1.0f;
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
                mc2::fmt::print_err("[PROP_LIGHT_PROBE v1] lightingReady={:d} haveLightFeed={:d} tier1={:d} wantLighting={:d} wantMaterial={:d} materialReady={:d} iblShStrength={:.4f} iblSh0=({:.4f},{:.4f},{:.4f}) lightDataCpuBytes={:d} terrainSun=({:.3f},{:.3f},{:.3f})\n",
                    mc2::fmt::as_i32(u.lightingReady), mc2::fmt::as_i32(haveLightFeed ? 1 : 0), mc2::fmt::as_i32(s->tier1 ? 1 : 0),
                    mc2::fmt::as_i32(s->wantLighting ? 1 : 0), mc2::fmt::as_i32(s->wantMaterial ? 1 : 0), mc2::fmt::as_i32(u.materialReady),
                    u.iblShStrength, u.iblSh[0][0], u.iblSh[0][1], u.iblSh[0][2],
                    mc2::fmt::as_u64(static_cast<unsigned long long>(lightFeed.lights.bytes)),
                    u.terrainSunMC2[0], u.terrainSunMC2[1], u.terrainSunMC2[2]);
                std::fflush(stderr);
            }
        }
        std::memcpy(mainPtr, &u, sizeof(u));
    }
    // set[par] follows the buffers the spans came from; the draw passes the offsets (bindings 0, 1, then 4 when declared).
    sync_ring_bindings(s, par, mainSpan, lightSpan);
    uint32_t dynOffsets[3];
    uint32_t nDyn = 0;
    dynOffsets[nDyn++] = static_cast<uint32_t>(mainSpan.offset);
    dynOffsets[nDyn++] = static_cast<uint32_t>(mainSpan.offset + instOff);
    if (s->tier1 && s->wantLighting) dynOffsets[nDyn++] = static_cast<uint32_t>(lightSpan ? lightSpan.offset : mainSpan.offset);
    s->lateParity = par; s->lateDynCount = nDyn;
    std::copy_n(dynOffsets, nDyn, s->lateOffsets);
    std::copy_n(ground->offsets.data(), 4, s->lateGroundOffsets);
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
        vk_render_pass_timer::CancelUnlessSubmitted timerGuard(gos_render_pass_timer::Pass_SpColor);
        unsigned long typeRunsRec = 0;
        if (!record_props({s, cb, w, h, par, nDyn, dynOffsets, ground, hzbCulled, instanceCount, insts}, typeRunsRec)) { setFallback("record_failed"); s->disabled = true; return 0; }
        g_health.typeRuns = typeRunsRec;

        // Render-pass dependencies order shared attachments; the ticket protects
        // this island's private recording resources.
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 0; si.pWaitSemaphores = nullptr; si.pWaitDstStageMask = nullptr;
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 0; si.pSignalSemaphores = nullptr;
        { ZoneScopedN("VKIsl.Props.Submit");   // VK-ISLAND-TRACY-1: vkQueueSubmit
          if (mc2vk::submitGraphics(si, VK_NULL_HANDLE) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; } }
        s->submitted[par] = rctx.tracker().now();
        timerGuard.submitted();

        // Killswitch MC2_VK_PROP_FENCE_DEFER=0 restores inline retirement.
        if (!propFenceDeferEnabled()) {
            ZoneScopedN("VKIsl.Props.InlineFenceWait"); mc2::battlebench::WaitScope gpuWait2(mc2::battlebench::WaitGpuMainNs);   // VK-ISLAND-TRACY-1: killswitch-only inline retire
            const clk::time_point tInline0 = clk::now();
            if (!rctx.wait(s->submitted[par], 5000000000ull, "prop inline retire")) {
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
            mc2::fmt::print_err("[VK_PROPS_PERF] tier1=1 inst={:d} type_runs={:d} stage_us={:.1f} record_submit_us={:.1f} fence_wait_us={:.1f} island_total_us={:.1f}\n",
                mc2::fmt::as_u32(instanceCount), mc2::fmt::as_ulong(g_health.typeRuns), g_health.stageInstUs,
                g_health.drawUs, us_fence_wait, g_health.totalUs);
            std::fflush(stderr);
        }
        return 1;
    }

    return 0;
}
extern "C" void mc2_vk_color_prop_island_late_fringe() {
    auto* s = parkedIsland();
    if (!s || s->disabled || s->lateCount == 0) return;
    const uint32_t lateStart = s->lateStart, lateCount = s->lateCount;
    s->lateCount = 0;
    const uint32_t par = s->lateParity;
    VkCommandBuffer cb = s->lateCbuf[par];
    vkResetCommandPool(s->device, s->latePool[par].get(), 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { s->disabled = true; return; }
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = s->t1_rpass.get(); rbi.framebuffer = s->t1_fb.get();
    rbi.renderArea = {{0,0},{static_cast<uint32_t>(s->w),static_cast<uint32_t>(s->h)}};
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    float jpx[2];
    RenderCore::temporal::sceneJitterPixels(g_mc2RenderFrame, jpx);
    VkViewport vp{jpx[0],jpx[1],static_cast<float>(s->w),static_cast<float>(s->h),0,1};
    VkRect2D scissor = rbi.renderArea;
    vkCmdSetViewport(cb,0,1,&vp); vkCmdSetScissor(cb,0,1,&scissor);
    mc2vk::infra::bind(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,s->t1_fringePipe.get());
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,s->playout.get(),0,1,&s->set[par],s->lateDynCount,s->lateOffsets);
    // The fringe pipeline shares playout (set 1 = ground lighting); unbound set 1 is invalid and loses the device.
    const VkDescriptorSet lateGround = s->groundLighting.set(par);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,s->playout.get(),1,1,&lateGround,4,s->lateGroundOffsets);
    VkDeviceSize offset = 0; const VkBuffer vertexBuffer = s->vbo.get();
    vkCmdBindVertexBuffers(cb,0,1,&vertexBuffer,&offset);
    vkCmdBindIndexBuffer(cb,s->ibo.get(),0,VK_INDEX_TYPE_UINT32);
    // The late list lives in the main pass's per-parity buffers; its instanceCounts were written by that pass's compaction dispatch, which the
    // main submit precedes in queue order (the compute-to-indirect barrier there covers later commands in submission order).
    const VkBuffer cmdBuf = s->gpu[par].cmd.get();
    const PropPacketPush latePush{s->gpu[par].pkt.deviceAddress(), lateStart, 0};
    vkCmdPushConstants(cb,s->playout.get(),VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(latePush),&latePush);
    mc2::battlebench::add(mc2::battlebench::DrawProps);
    vkCmdDrawIndexedIndirectCount(cb,cmdBuf,kPropCmdHeader + static_cast<VkDeviceSize>(lateStart) * sizeof(PropDrawCmd),cmdBuf,kListLate * sizeof(uint32_t),
                                  (std::min)(lateCount,s->drawIndirectLimit),sizeof(PropDrawCmd));
    vkCmdEndRenderPass(cb);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) { s->disabled = true; return; }
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    mc2vk::VulkanContext& lctx = mc2vk::getOrCreateEngineVulkanContext();
    if (lctx.submit(si, VK_NULL_HANDLE) != VK_SUCCESS) s->disabled = true;
    else s->lateSubmitted[par] = lctx.tracker().now();
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

