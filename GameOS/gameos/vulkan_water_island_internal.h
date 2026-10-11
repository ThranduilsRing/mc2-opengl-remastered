// Shared surface of the vulkan_water_island.cpp translation units (vulkan_water_island.cpp, vulkan_water_island_{images,init,run}.cpp).
//
// vulkan_water_island.cpp keeps the health / interop counters, the env gates, setFallback / vlog / load_spv, the buffer and descriptor
// helpers, submit_once and the health / enabled / destroy entry points; the rest is split by responsibility (see DOC in each piece).
// Every piece, and this header, compiles to nothing without MC2_VULKAN_ISLAND.
#ifndef VULKAN_WATER_ISLAND_INTERNAL_H
#define VULKAN_WATER_ISLAND_INTERNAL_H

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

#include "vk_image_resource.h"
#include "vk_island_util.h"
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
#include <cstddef>   // offsetof (M2 layout asserts)
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
static_assert(sizeof(WaterVsParams) == 128, "WaterVsParams std140 size (water_geom.vert)");
static_assert(offsetof(WaterVsParams, p0) == 64 && offsetof(WaterVsParams, p1) == 80 &&
              offsetof(WaterVsParams, p2) == 96 && offsetof(WaterVsParams, p3) == 112, "WaterVsParams offsets");
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
static_assert(sizeof(WaterFsParams) == 208, "WaterFsParams std140 size (water_geom.frag)");
static_assert(offsetof(WaterFsParams, f0) == 16 && offsetof(WaterFsParams, f3) == 64 &&
              offsetof(WaterFsParams, DEEP_COLOR) == 80 && offsetof(WaterFsParams, f4) == 112 &&
              offsetof(WaterFsParams, f7) == 160 && offsetof(WaterFsParams, f9) == 192, "WaterFsParams offsets");

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

}  // namespace vk_water_detail
using namespace vk_water_detail;

// Globals defined in vulkan_water_island.cpp (or elsewhere in the engine) that the split pieces share.

// State and functions of the water island, shared by the pieces (defined in the piece that owns them).
namespace vk_water_detail {
extern WaterIslandHealth g_health;
extern InteropForbidden g_forbidden;
extern unsigned long g_depthInGetTexImage;
extern unsigned long g_depthInCopyBufferToImage;
extern WaterIsland* g_island;
void setFallback(std::string_view reason);
void vlog(const char* fmt, ...);
bool waterTier1Requested();
bool waterPerfGateRequested();
bool waterFenceDeferEnabled();
bool waterDirectChainRequested();
std::string spv_dir();
VkShaderModule load_spv(VkDevice device, std::string_view path);
void destroy_island(WaterIsland* s);
bool make_buffer(WaterIsland* s, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* buf, VmaAllocation* alloc);
void write_descriptors(WaterIsland* s);
bool ensure_thin_capacity(WaterIsland* s, uint32_t par, VkDeviceSize bytes);
bool submit_once(WaterIsland* s, const std::function<void(VkCommandBuffer)>& record);
bool ensure_hdri(WaterIsland* s, unsigned int hdriGlTex);
bool import_shore_tex(WaterIsland* s, unsigned int glTex, bool isFloat, VkImage* img, VmaAllocation* alloc, VkImageView* view, bool* real);
bool ensure_shore(WaterIsland* s);
bool init_island(WaterIsland* s, int w, int h);
}  // namespace vk_water_detail

#endif // MC2_VULKAN_ISLAND

#endif // VULKAN_WATER_ISLAND_INTERNAL_H
