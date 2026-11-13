// Shared surface of the vulkan_water_island.cpp translation units (vulkan_water_island.cpp, vulkan_water_island_{images,init,run}.cpp).
//
// vulkan_water_island.cpp keeps the health / interop counters, the env gates, setFallback / vlog / load_spv, the buffer and descriptor
// helpers and the health / enabled / destroy entry points; the rest is split by responsibility (see DOC in each piece).
// Every piece, and this header, compiles to nothing without MC2_VULKAN_ISLAND.
#ifndef VULKAN_WATER_ISLAND_INTERNAL_H
#define VULKAN_WATER_ISLAND_INTERNAL_H


#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()

#include <vk_mem_alloc.h>

#include "vk_image_resource.h"
#include "vk_texture_residency.h"
#include "vk_island_util.h"
#include "vk_hdri_source.h"
#include "vk_native_producers.h"   // STEP 3.4P: native compute producers
#include "vulkan_context.h"
#include "vk_vegetation_ground.h"
#include "gos_terrain_water_stream.h"
#include "terrain_field_store.h"   // the shore sidecars' CPU copies

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
#include <cstddef>   // offsetof (M2 layout asserts)
#include <memory>
#include <vector>
#include <string>
#include <functional>
#include <chrono>

// ---------------------------------------------------------------------------
// Health snapshot (mirrors the SSAO island's) — read by debug_state_dump.cpp.
// ---------------------------------------------------------------------------

namespace vk_water_detail {

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
    // GPU cull health: the last completed frame's visible quads against the CPU
    // eligible bound. visible == 0 on a map with water, or visible > eligible, is a regression visible in the log.
    unsigned long waterVisible = 0, waterEligibleBound = 0;
    double copyColorInUs = 0.0, copyDepthInUs = 0.0, drawUs = 0.0, colorOutUs = 0.0, totalUs = 0.0;
};

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

// std140 UBO mirrors (LOCKSTEP with water_geom.vert/.frag). ------------------
struct WaterVsParams {
    float worldToClipGL[16]; // column_major; SAME raw 16 floats as GL GL_FALSE upload  (off 0)
    float p0[4];             // waterElevation, alphaDepth, mapTopLeftX, mapTopLeftY  (off 64)
    float p1[4];             // frameCos, maxMinUV, uvScale, uvOffsetX                (off 80)
    float p2[4];             // uvOffsetY, fudge, alphaEdgeByte, alphaMiddleByte      (off 96)
    float p3[4];             // alphaDeepByte, shoreFlat, shoreDepthK, pad            (off 112)
};
constexpr size_t kWaterVsUboSize = sizeof(WaterVsParams);
static_assert(sizeof(WaterVsParams) == 128, "WaterVsParams std140 size (water_geom.vert)");
static_assert(offsetof(WaterVsParams, p0) == 64 && offsetof(WaterVsParams, p1) == 80 &&
              offsetof(WaterVsParams, p2) == 96 && offsetof(WaterVsParams, p3) == 112, "WaterVsParams offsets");
struct WaterFsParams {
    float fog_color[4];      // off 0
    float f0[4];             // off 16  time, camX, camY, camZ
    float f1[4];             // off 32  alphaDepth, ABSORPTION_DENSITY, WATER_MAX_ALPHA, RIPPLE_GAIN
    float f2[4];             // off 48  GLINT_GAIN, u_waterReflStrength, u_skyYaw, u_waterHdriLod
    float f3[4];             // off 64  screenW, screenH, nightDim, animatedShore
    float DEEP_COLOR[4];     // off 80
    float SHALLOW_COLOR[4];  // off 96
    // VK-PARITY-WATER-SHORE-1 (WATER-SHORE-EDGE-1) -- LOCKSTEP with water_geom.frag.
    float f4[4];             // off 112 u_shoreMode, u_hasShoreHeight, u_hasShoreMask, u_shoreFeatherWu
    float f5[4];             // off 128 u_shoreHeightBounds
    float f6[4];             // off 144 u_shoreMaskBounds
    float f7[4];             // off 160 waterElevation, mask span (wu), mask bound (0/1), terrain grid halfMap
    // VK-PARITY-F3: LOCKSTEP with water_geom.frag f8/f9.
    float f8[4];             // off 176 edgeFeather, edgeFeatherStrength, halfMap, skyTintStrength
    float f9[4];             // off 192 skyTintColor.xyz, projAspect
};
static_assert(sizeof(WaterFsParams) == 208, "WaterFsParams std140 size (water_geom.frag)");
static_assert(offsetof(WaterFsParams, f0) == 16 && offsetof(WaterFsParams, f3) == 64 &&
              offsetof(WaterFsParams, DEEP_COLOR) == 80 && offsetof(WaterFsParams, f4) == 112 &&
              offsetof(WaterFsParams, f7) == 160 && offsetof(WaterFsParams, f9) == 192, "WaterFsParams offsets");

// The persistent island state.
struct WaterIsland : mc2vk::IslandState {
    mc2veg::GroundLighting groundLighting;
    bool disabled = false;
    int  waitStrikes = 0;
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
    VkDeviceSize  recipeBytes = 0;
    bool          recipeUploaded = false;
    VkDeviceSize  thinCapBytes[mc2vk::kMaxFramesInFlight] = {0, 0};   // grows to high-water-mark

    // RUNG-6.5 WATER-REFLECTION: copy-once HDRI equirect import (binding 2).
    // Copied ONCE from the GL HDRI texture (BC6H mip0 sidecar in the field
    // config, or RGBA16F + full mip chain on the tinyexr fallback path) — NOT a
    // per-frame copy. hdriRealImported=false -> a 1x1 black dummy is bound and
    // the island patches fsParams' u_waterHdriLod slot to -1 (SH-L2 fallback,
    // same semantics GL uses when isHdriReady()==false).
    bool          hdriRealImported = false;   // real GL HDRI bytes present (not dummy)
    bool          hdriAttempted    = false;   // import tried once (success or fail)
    uint64_t      hdriGeneration   = 0;       // the CPU HDRI load generation the import came from

    // VK-PARITY-WATER-SHORE-1: copy-once imports of the GL water-shore textures
    // (binding 3 = R32F hi-res visual height, binding 4 = RGBA8 shoreline mask).
    // Re-imported when either field-store version changes (edits or new mission).
    // The texture service shares the mask allocation with terrain. A dummy keeps
    // the statically-used binding valid when the
    // GL texture is absent; has*=false makes the FS degrade 2 -> 1 -> 0 like GL.
    bool          shHReal = false, shMReal = false;   // real data imported (not dummy)
    bool          shHSdf = false;                     // the real height field is a cooked shore SDF
    bool          shoreAttempted = false;
    uint64_t      shoreHeightVersion = 0, shoreMaskVersion = 0;

    VkDescriptorSet   set[mc2vk::kMaxFramesInFlight] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // per parity slot (VK-WATER-FENCE-DEFER-1); allocated from `pool`, freed with it
    mc2vk::DescWriteStamp moverStamp[mc2vk::kMaxFramesInFlight]{};   // binding 24 of set[i]
    VkCommandBuffer   cbuf[mc2vk::kMaxFramesInFlight] = {VK_NULL_HANDLE, VK_NULL_HANDLE};  // allocated from `cpool`, freed with it
    mc2vk::GpuTicket  submitted[mc2vk::kMaxFramesInFlight];   // per engine frame slot: the last submit that used cbuf[slot], set[slot] and buffers[slot]

    bool descNeedsThinRewrite = true; // thinBuf realloc -> descriptor rewrite

    // Owned objects are declared in creation order and released in reverse by the destructor, while the device is alive (the engine context releases the
    // parked island ahead of the device). An image is declared before its view so the view is released first.
    // The vertex and fragment parameter UBOs live in the engine frame ring (one span per frame); set[par] binds them with dynamic offsets (bindings 0, 1).
    // ringBound[par] = the ring buffer they currently point at; a frame whose span came from another buffer re-points them after the slot fence.
    VkBuffer ringBound[mc2vk::kMaxFramesInFlight] = {};
    mc2vk::OwnedBuffer recipeBuf;
    mc2vk::OwnedBuffer thinBuf[mc2vk::kMaxFramesInFlight];
    mc2vk::OwnedImage reflectionImage;
    // Independent full-resolution reverse-Z depth; never aliases main scene depth.
    mc2vk::OwnedImage planarDepthImage;
    mc2vk::OwnedImageView planarDepthView;
    mc2vk::OwnedImageView reflectionView;
    mc2vk::OwnedBuffer maskHorizontal, maskDepth;
    mc2vk::OwnedDescriptorSetLayout maskDsl;
    mc2vk::OwnedDescriptorPool maskPool;
    VkDescriptorSet maskSet = VK_NULL_HANDLE;
    mc2vk::OwnedPipelineLayout maskLayout;
    mc2vk::OwnedPipeline maskCompute, maskCoverage;
    VkRect2D reflectionScissor{};
    bool reflectionInitialized = false;
    bool planarDepthInitialized = false;
    bool mapHasWater = false;
    VkFormat sceneFmts[2]{VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};   // dynamic rendering: scene colour0 + normal
    VkFormat sceneDepthFmt = VK_FORMAT_UNDEFINED;
    mc2vk::OwnedDescriptorSetLayout dsl;
    mc2vk::OwnedDescriptorPool pool;
    bool imageDescriptorsDirty[mc2vk::kMaxFramesInFlight]={};
    std::array<VkImageView,4> boundTextures[mc2vk::kMaxFramesInFlight]{};
    // The shadow bake's mask (binding 7) while the bake is ready; the shader reads it only when the frame's f7.z says so.
    VkImageView maskView = VK_NULL_HANDLE;
    bool hdriSourceReal=false,shHSourceReal=false,shMSourceReal=false;
    mc2vk::ResidentTextureRef hdriTexture;
    mc2vk::OwnedSampler   hdriSampler;
    float                 sampBias = 0.0f;   // mip bias hdriSampler was built with
    mc2vk::ResidentTextureRef shHTexture;
    mc2vk::ResidentTextureRef shMTexture;
    mc2vk::OwnedShaderModule vert, frag;
    mc2vk::OwnedPipelineLayout playout;
    mc2vk::OwnedPipeline pipe;
    mc2vk::OwnedCommandPool cpool[mc2vk::kMaxFramesInFlight];   // per-slot pools: vkResetCommandPool per frame; cpool[0] also serves the synchronous one-shot

    // STEP 3.4P: the native Vulkan lighting + cull/pack producers (declared last: released first). `nativeFailed` = init was tried and refused, so the
    // GL readback feed stays in use for the session.
    // Whole reflected views own their descriptors, compaction and draw buffers.
    // Their slot writes are protected by this water island's submission tickets.
    std::unique_ptr<mc2vk::IslandState> planarTerrain, planarProps;
    mc2vk::VulkanContext::RecordFn planarTerrainRecord, planarPropsRecord;
    std::unique_ptr<mc2vk::NativeProducers> native;
    bool     nativeFailed = false;
    bool     thinNative[mc2vk::kMaxFramesInFlight] = {false, false};   // what binding 6 of each parity set currently points at: true = native thin_, false = thinBuf
    uint64_t nativeFrames = 0;   // frames the native path completed; schedules the parity frames
};

}  // namespace vk_water_detail
using namespace vk_water_detail;

// Globals defined in vulkan_water_island.cpp (or elsewhere in the engine) that the split pieces share.

// State and functions of the water island, shared by the pieces (defined in the piece that owns them).
namespace vk_water_detail {
extern WaterIslandHealth g_health;
extern int g_waterInitReachedBuild;   // 1 once init_island passed its readiness gates (vk_island_fail_policy.h)
bool init_reflection(WaterIsland* s);
bool record_reflection(WaterIsland* s, VkCommandBuffer cb, bool enabled, uint32_t par, const uint32_t (&offsets)[2]);
extern InteropForbidden g_forbidden;
extern unsigned long g_depthInGetTexImage;
extern unsigned long g_depthInCopyBufferToImage;
// The island lives in the engine context's slot: the context releases it ahead of the device whatever path ends the process.
std::unique_ptr<mc2vk::IslandState>& islandSlot();
WaterIsland* parkedIsland();
void setFallback(std::string_view reason);
void vlog(const char* fmt, ...);
bool waterPerfGateRequested();
bool waterDirectChainRequested();
std::string spv_dir();
VkShaderModule load_spv(VkDevice device, std::string_view path);
bool make_buffer(WaterIsland* s, mc2vk::OwnedBuffer& out, VkDeviceSize size, VkBufferUsageFlags usage,
                 std::string_view name, int i0 = -1, int i1 = -1);
void write_descriptors(WaterIsland* s, int only = -1);
void refresh_hdri_sampler(WaterIsland* s);   // rebuild the mip-biased sampler when the effective resolve state changed
bool ensure_thin_capacity(WaterIsland* s, uint32_t par, VkDeviceSize bytes);
bool upload_recipes_device(WaterIsland* s, const void* recipes, uint32_t recipeCount);
bool ensure_hdri(WaterIsland* s, uint64_t hdriGeneration);
bool import_shore_tex(WaterIsland* s, const void* texels, int texelsW, int texelsH, bool isFloat, mc2vk::ResidentTextureRef& texture, bool* real);
bool ensure_shore(WaterIsland* s);
bool init_island(WaterIsland* s, int w, int h);
}  // namespace vk_water_detail


#endif // VULKAN_WATER_ISLAND_INTERNAL_H
