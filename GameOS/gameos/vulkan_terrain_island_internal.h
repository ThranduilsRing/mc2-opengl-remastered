// Shared surface of the vulkan_terrain_island.cpp translation units (vulkan_terrain_island.cpp, vulkan_terrain_island_{island,images,run,overlay}.cpp).
//
// vulkan_terrain_island.cpp keeps the health / interop counters, the env gates, setFallback / vlog / load_spv and the island state;
// the rest is split by responsibility (see DOC in each piece). Every piece, and this header, compiles to nothing without MC2_VULKAN_ISLAND.
#ifndef VULKAN_TERRAIN_ISLAND_INTERNAL_H
#define VULKAN_TERRAIN_ISLAND_INTERNAL_H

#ifdef MC2_VULKAN_ISLAND

#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>

#include <vk_mem_alloc.h>

#include "vk_image_resource.h"
#include "vk_island_util.h"
#include "vk_owned.h"
#include "vk_texture_residency.h"
#include "vk_gpu_timestamps.h"   // VK-DRAW-PERF-1: MC2_VK_GPU_TIMESTAMPS GPU segment timers
#include "vulkan_context.h"
#include "vk_light_lists.h"
#include "vulkan_terrain_island.h"
#include "vulkan_ground_lighting.h"
#include "Gates/gate_cold.h"   // gate::get<>() and str<>()
#include "vk_shared_gbuffer_gate.h"     // VK-P14 GATE-CONSOLIDATION-1 umbrella resolver
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"
#include "vulkan_shadow_island_live.h"  // VK-SHADOW-DIRECT-CONSUME-1: direct dyn-shadow view accessors
#include "../../RenderCore/gpu_abi/GpuTerrainFrameUBO.h"  // single-source std140 per-patch UBO

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <vector>
#include <string>
#include <chrono>
#include <unordered_map>
#include <memory>
#include <tracy/Tracy.hpp>   // VK-FLIP-FIELD-REPORT-1: coarse per-region frame zones


// Types shared by the pieces of this island (the functions that use them live in namespace vk_terrain_detail).
namespace vk_terrain_detail {

// ---------------------------------------------------------------------------
// Health snapshot (mirrors the prop island's).
// ---------------------------------------------------------------------------
struct TerrainIslandHealth {
    int   vulkanAvailable = 0;
    int   buildEnabled    = 1;
    char  deviceName[256] = {0};
    unsigned long attempted   = 0;
    unsigned long usedVulkan  = 0;
    char  fallbackReason[256] = {0};
    unsigned long vkDraws     = 0;
    int   interopActive  = 0;
    int   vk2glSemActive = 0;
    unsigned long patchRecords    = 0;   // LOD0 patches co-written this frame
    unsigned long heightSnapshots = 0;   // height-SSBO snapshot copies (bumps on change)
    double stageUs = 0.0, drawUs = 0.0, totalUs = 0.0;
};

struct InteropForbidden {
    unsigned long glGetTexImage = 0;   // color CPU pull (must be 0)
    unsigned long glReadPixels  = 0;   // color CPU pull (must be 0)
};

// int16 local-offset vertex (mirrors terrain_lod_chunk.cpp LocalVertex { int16 lx,ly }).
struct LocalVertex { int16_t lx, ly; };
const VkFormat kColorFmt = VK_FORMAT_R16G16B16A16_SFLOAT; // MainColor-compatible

// A cached LOD0 patch grid (VK-owned VBO/IBO), keyed by (quadCountX,quadCountY).
struct PatchMesh {
    mc2vk::OwnedBuffer vbo;
    mc2vk::OwnedBuffer ibo;
    uint32_t      indexCount = 0;
};

// VK-TERRAIN-SKIRT (rung 4): a cached skirt-strip mesh (VK-owned VBO/IBO), keyed by
// (quadCountX,quadCountY,lodStep) same as the main PatchMesh -- mirrors GL's
// per-patch skirtVbo/skirtIbo built alongside the main grid (getOrBuildPatch,
// gos_terrain_lod_chunk.cpp:459-518). Per-edge offset/count (N,S,W,E) let the draw
// loop honor the per-block skirt edge mask exactly like GL's mask-bit loop.
struct SkirtMesh {
    mc2vk::OwnedBuffer vbo;
    mc2vk::OwnedBuffer ibo;
    uint32_t      edgeOffset[4] = {0,0,0,0};   // 0=N,1=S,2=W,3=E (index offsets, in indices)
    uint32_t      edgeCount[4]  = {0,0,0,0};
};

// VK-TERRAIN-SKIRT vertex: mirrors GL's SkirtVertex { int16 lx, ly, isSkirt, _pad }.
struct SkirtVertex { int16_t lx, ly, isSkirt, _pad; };

struct OvlPC {                       // LOCKSTEP with OverlayPC in terrain_overlay_tier1.{vert,frag}
    float   fogColor[4];             // off 0
    float   artFlatExempt;           // off 16
    float   mapHalfExtent;           // off 20
    int32_t biasLegacy;              // off 24
    int32_t cementAdopt;             // off 28
    int32_t seamFix;                 // off 32
    int32_t _pad[3];                 // off 36..47
};
static_assert(sizeof(OvlPC) == 48, "OvlPC push-constant lockstep");

struct OvlTex {
    mc2vk::ResidentTextureRef texture;
    VkDescriptorSet set[mc2vk::kMaxFramesInFlight] = {};
    VkImageView bound[mc2vk::kMaxFramesInFlight] = {};   // freed with the pool
    // The texels this import came from; a reloaded texture publishes a new vector, so identity says when to re-import.
    std::shared_ptr<const std::vector<uint8_t>> source;
};

struct OvlPending {
    uint32_t kind = 0;               // 0 = terrain overlay (cement/road art), 1 = decal
    bool     isStatic = false;       // persistent bake (verts stay owned by the caller)
    uint64_t version = 0;            // static bake version
    const uint8_t* staticPtr = nullptr;
    std::vector<uint8_t> verts;      // per-frame copy (28B WorldOverlayVert each)
    uint32_t vertCount = 0;
    std::vector<Mc2VkOverlayDraw> draws;
};

struct OvlState {
    int w = 0, h = 0;
    bool inited = false, disabled = false;
    int  waitStrikes = 0;
    // OWNED objects in creation order, released in reverse (pipelines before their layout, framebuffer before its render pass, textures first).
    mc2vk::OwnedRenderPass rpass;
    mc2vk::OwnedFramebuffer fb;
    mc2vk::OwnedShaderModule vert[2], frag[2];
    mc2vk::OwnedPipeline pipe[2];
    mc2vk::OwnedPipelineLayout playout;
    mc2vk::OwnedDescriptorSetLayout texDsl;
    mc2vk::OwnedDescriptorPool texPool;
    mc2vk::OwnedSampler samp;
    mc2vk::OwnedBuffer stat;   // persistent bake; replaced buffers retire after their last GPU use
    VkDeviceSize statCap = 0; uint64_t statVersion = UINT64_MAX;
    bool statUsed = false;    // a load snapshot can be corrected in place before its first submit
    VkCommandBuffer cbuf[mc2vk::kMaxFramesInFlight] = {};   // one per slot, each from its own pool
    mc2vk::OwnedCommandPool cpool[mc2vk::kMaxFramesInFlight];   // per-slot pools: vkResetCommandPool instead of per-cb reset
    OvlTex dummy;
    std::unordered_map<uint32_t, OvlTex> texs;   // key: gos texture handle
};

// The island state. The engine context owns it while it is parked (IslandSlot::Terrain), so the objects below are released by the context ahead of
// the allocator and device they were made on. The owners are declared in creation order and released in reverse.
struct TerrainIsland : mc2vk::IslandState {
    bool disabled = false;
    int  waitStrikes = 0;
    bool inited   = false;

    int w = 0, h = 0;

    // Borrowed from the shared engine context (NOT owned).
    VkInstance       instance  = VK_NULL_HANDLE;
    VkPhysicalDevice phys      = VK_NULL_HANDLE;
    VkDevice         device    = VK_NULL_HANDLE;
    uint32_t         gfxFamily = UINT32_MAX;
    VkQueue          queue     = VK_NULL_HANDLE;
    VmaAllocator     allocator = VK_NULL_HANDLE;

    // Borrowed shared-GBuffer images (owned by the postprocess registries).
    VkImageResource* colorRes  = nullptr;
    VkImageResource* normalRes = nullptr;
    VkImageResource* depthRes  = nullptr;
    // VK-RUNG8-ITEM5: shared scene is VK-only (no GL interop view). Terrain is the
    // direct-chain first-owner; the former colorView/normalView/depthView are gone.

    // LOAD-preserve co-write renderpass + framebuffer (over the 3 shared views).
    mc2vk::OwnedRenderPass  rpass;
    mc2vk::OwnedFramebuffer fb;

    // Pipeline (2 GBuffer outputs, reverse-Z GEQUAL depth-write, cull NONE, no blend).
    mc2vk::OwnedDescriptorSetLayout dsl;
    mc2vk::OwnedDescriptorPool  pool;
    VkDescriptorSet   set[mc2vk::kMaxFramesInFlight] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // per parity slot (VK-TERRAIN-FENCE-DEFER-1); set[i] binds ubo[i]
    mc2vk::LightLists lightLists;
    uint64_t setRev[mc2vk::kMaxFramesInFlight] = {};            // new value on every write to set[i]'s ground-copied bindings
    mc2vk::DescWriteStamp moverStamp[mc2vk::kMaxFramesInFlight]; // binding 24 of set[i]; terrain run and overlay share the set and the stamp
    Mc2GroundLightingFrame groundFrame{};
    uint64_t groundFrameNo = ~static_cast<uint64_t>(0);
    mc2vk::OwnedPipelineLayout  playout;
    mc2vk::OwnedPipeline        pipe;
    mc2vk::OwnedShaderModule    vert;
    mc2vk::OwnedShaderModule    frag;

    // VK-TERRAIN-SKIRT (rung 4): second pipeline for skirt strips (2-attribute vertex
    // input: ivec2 localOffset + int isSkirtFlag, mirrors GL's skirt VAO). Shares the
    // SAME descriptor set / pipeline layout / renderpass as the main pipeline (only
    // the vertex-input state + shader modules differ) -- cull/depth/blend state is
    // IDENTICAL (double-sided, reverse-Z GEQUAL, opaque), matching GL's skirt draw
    // which reuses the same terrain program + GL state, just a different VAO/attribs.
    mc2vk::OwnedPipeline        skirtPipe;
    mc2vk::OwnedShaderModule    skirtVert;
    mc2vk::OwnedShaderModule    skirtFrag;

    // VK-owned snapshot of the GL height SSBO (binding 23 -> here binding 1).
    mc2vk::OwnedBuffer      heightBuf;
    VkDeviceSize  heightCapBytes = 0;
    uint64_t      heightVersionCopied = UINT64_MAX; // last snapshotted feed version

    // VK-TERRAIN-VISUALDISPLACE-FAR-1 (rung 5): VK-owned snapshot of the GL 4x visual
    // heightfield bake (GL binding 26 -> here binding 10). Version-gated mission-load
    // copy, mirrors the cement-word SSBO snapshot machinery.
    mc2vk::OwnedBuffer      visualHeightBuf;
    VkDeviceSize  visualHeightCapBytes = 0;
    uint64_t      visualHeightVersionCopied = UINT64_MAX;
    bool          visualHeightReady = false;        // a real bake snapshot is live

    // COLORMAP-BASE sub-slice: VK-owned copy-once import of the GL colormap atlas
    // (g_atlasGLTex). Sampled image + view + sampler; binding 2. Keyed by the GL tex
    // name so a re-built atlas (per-mission) re-imports.
    bool descriptorsDirty[mc2vk::kMaxFramesInFlight] = {};
    std::array<VkImageView,7> boundTextures[mc2vk::kMaxFramesInFlight]{};
    mc2vk::ResidentTextureRef colormapTexture;
    mc2vk::OwnedSampler     colormapSamp;
    uint64_t      colormapVersionImported = 0;   // field-store colormap version last imported (0 = none yet)
    uint64_t      colormapShoreVersionImported = 0;
    std::array<float, 9> colormapCookParams{};
    bool          colormapReady = false;     // set once the sampled image is live
    bool          colormapBreakup = false;
    mc2vk::ResidentTextureRef macroGradientTexture;

    // CEMENT/ROAD atlas sub-slice: VK-owned copy-once import of the GL cement atlas
    // (g_cementAtlasGLTex, runtime RGBA8). Sampled image + view + REPEAT sampler; binding
    // 3. Keyed by the GL tex name so a per-mission rebuild re-imports. PLUS a VK-owned
    // snapshot of the GL per-vertex cement-word SSBO (binding 25 -> here binding 4).
    mc2vk::ResidentTextureRef cementTexture;
    mc2vk::OwnedSampler     cementSamp;   // REPEAT/LINEAR (GL_REPEAT wrap)
    uint64_t      cementVersionImported = 0;      // field-store cement atlas version last imported (0 = none)
    bool          cementReady = false;            // set once the cement image is live
    // TERRAIN-SHORELINE-V3: binding 14, the shoreline mask sidecar (dummy 1x1 until a mission ships one).
    mc2vk::ResidentTextureRef shoreMaskTexture;
    uint64_t      shoreMaskVersionImported = 0;   // field-store mask version last imported (0 = none)
    bool          shoreMaskReady = false;         // the bound mask is the mission's, not the dummy
    // Baked material weights reuse the former, unconsumed shore-image slot.
    mc2vk::ResidentTextureRef materialTexture;
    mc2vk::ResidentTextureRef materialPending;         // a coverage-only update, swapped in once resident (the old one stays bound until then)
    std::vector<uint8_t> materialBase, materialFinal;  // coverage-free weights (cached per surface/types/control/profile) and base + hard-surface coverage
    uint32_t materialW = 0, materialH = 0;
    int materialProfile = -1;
    uint64_t materialSurfaceVersion = 0, materialTypesVersion = 0, materialControlVersion = 0, materialCoverageVersion = 0;
    mc2vk::OwnedBuffer      cementBuf;
    VkDeviceSize  cementCapBytes = 0;
    uint64_t      cementVersionCopied = UINT64_MAX; // last snapshotted cement-word version
    bool          cementWordsReady = false;       // set once the cement SSBO has a real snapshot

    // The STATIC sun shadow map at binding 5: the VK shadow island's map (shadowExternalView) when its build matches the frame's generation, else this
    // island's 1x1 dummy (shadowImg / shadowView) with enableShadows=0. Sampled through a depth-COMPARE sampler (compareOp=LESS_OR_EQUAL, LINEAR HW
    // PCF, CLAMP_TO_BORDER white=1.0), exactly like the GL sampler2DShadow.
    bool          shadowReady = false;           // binding 5 holds the island's map of this frame's generation
    mc2vk::ResidentTextureRef staticVisibilityDummy;
    mc2vk::OwnedSampler staticVisibilitySampler;
    VkImageView staticVisibilityExternalView = VK_NULL_HANDLE;
    VkImageView staticVisibilityBoundView = VK_NULL_HANDLE;
    VkImageView localShadowBoundView = VK_NULL_HANDLE;   // LOCAL-LIGHTS-2: binding 19 (shadow island's live atlas)

    // VK-TERRAIN-DETAIL sub-slice: the material detail-normal ARRAY. A VK copy-once
    // import of the GL terrain_normal_array_tex_ (9-layer RGBA8 GL_TEXTURE_2D_ARRAY,
    // GL_REPEAT wrap). Sampled at binding 6 (sampler2DArray, REPEAT/LINEAR). Dummy 1x1
    // 1-layer until the real array imports (matNormalReady gates the FS sample).
    mc2vk::ResidentTextureRef matNormalTexture;
    mc2vk::OwnedSampler     matNormalSamp;   // REPEAT/LINEAR (GL_REPEAT wrap)
    uint64_t      matNormalVersionImported = 0;      // last source version imported
    uint64_t      matNormalVersionRejected = 0;      // rejected source version, logged once
    uint64_t      matNormalCookedVersion = 0;        // field-store version of the cooked BC7 layers last tried
    uint32_t      matNormalLayers = 0;               // layer count of the imported array
    bool          matNormalReady = false;            // set once the real array is live
    // CONCRETE-ROAD-PBR-DETAIL: binding 18, 2-layer RGBA8 array (0 pad concrete, 1 road asphalt): normal xy, roughness, AO.
    mc2vk::ResidentTextureRef concreteDetailTexture;
    mc2vk::OwnedSampler     concreteDetailSamp;
    bool          concreteDetailReady = false;
    bool          concreteDetailTried = false;
    bool          cliffRockPresent = false;     // TERRAIN-CLIFF-ROCK: layers 2/3 of the concrete detail array hold the rock set
    mc2vk::ResidentTextureRef concreteDetailPending;   // uploading; swapped in (and bound) only once the residency service reports it ready

    // Per-patch dynamic UBO (one 512B slot per LOD0 patch this frame; 512 is a multiple
    // of the 256B minUniformBufferOffsetAlignment — grew from 256 for the dynshadow mat4).
    // VK-TERRAIN-FENCE-DEFER-1: the ONLY per-frame CPU-written buffer this island's
    // submit consumes (per-parity staging audit) — now a PER-PARITY-SLOT pair, staged
    // [par] AFTER the hoisted beginFrame so the slot-fence wait protects the rewrite.
    // Everything else the submit reads is either write-once (patch/skirt cache VBO/IBOs,
    // copy-once image imports), rare-write device-idle-guarded (height/cement/visual-
    // height snapshots), or synced by its own one-shot inline fence (shadow copies).
    // Per-frame data lives in the engine frame ring: the per-patch dynamic UBO (binding 0) and the light pool / grid header / grid index SSBOs (bindings
    // 11-13) are spans of it, bound with dynamic offsets (in binding order 0, 11, 12, 13). ringBound[par][k] (k = 0 UBO, 1-3 pool/header/indices) = the
    // ring buffer that binding of set[par] currently points at; a frame whose spans came from another buffer (the ring grew), or a set rewritten by
    // write_descriptors, is re-pointed after beginFrame retired the parity.
    VkBuffer      ringBound[mc2vk::kMaxFramesInFlight][4] = {};

    // VK-PARITY-TERRAIN-2: per-parity host-visible copies of the GL terrain light pool
    // (GL binding 28 -> VK binding 11), light-grid header (29 -> 12) and index list
    // (30 -> 13). [par][k], k = 0 pool / 1 header / 2 indices. Rewritten each frame from
    // the SAME CPU arrays the GL producer uploads each frame (pool <= 12 KB, header 32 KB
    // at the default 64x64 grid, indices = stamped count); staged [par] after the slot
    // fence like the UBO. Grow-only (device-idle on regrow, rare).

    std::unordered_map<uint32_t, PatchMesh> patchCache;
    // Per engine slot: [patch records][skirt records][patch commands][skirt commands] for the indirect terrain draws. Grow-only; the slot's
    // fence is waited before the slot is rewritten (same contract as the per-parity light buffers).
    mc2vk::OwnedBuffer drawBuf[mc2vk::kMaxFramesInFlight];
    VkDeviceSize       drawBufCap[mc2vk::kMaxFramesInFlight] = {};
    // TERRAIN-HIRES-NEAR-DIAG: per-chunk mode-1 meshes whose quad diagonals follow the bake (key: span + block origin),
    // valid for one bake version; cleared (device idle) when the bake changes.
    std::unordered_map<uint64_t, PatchMesh> diagPatchCache;
    uint64_t      diagPatchVersion = UINT64_MAX;
    std::unordered_map<uint32_t, SkirtMesh> skirtCache;  // VK-TERRAIN-SKIRT (rung 4)

    mc2vk::OwnedCommandPool     cpool;   // one-shot command buffers only (allocated and freed per use)
    mc2vk::OwnedCommandPool     slotPool[2 * mc2vk::kMaxFramesInFlight];   // one pool per cbuf slot
    VkCommandBuffer   cbuf[2 * mc2vk::kMaxFramesInFlight] = {};   // terrain run slots, then scene clear slots
    // GPU-FRONTIER-1: per engine frame slot, the last submit that used the slot's command buffer / descriptor set. The terrain run, the overlay
    // (which draws through the run's set[slot]) and the scene clear each wait on their own previous submit only.
    mc2vk::GpuTicket  submitted[mc2vk::kMaxFramesInFlight], ovlSubmitted[mc2vk::kMaxFramesInFlight], clearSubmitted[mc2vk::kMaxFramesInFlight];
    std::array<uint64_t, mc2vk::kMaxFramesInFlight> setBoundFrame = [] {
        std::array<uint64_t, mc2vk::kMaxFramesInFlight> frames; frames.fill(UINT64_MAX); return frames;
    }();   // engine frame in which set[slot] was last rewritten (mover binding, dirty textures)

    // VK-DRAW-PERF-1: per-pass GPU timestamp timer (3 stamps: pass-start / draws-start /
    // pass-end). Gated + fail-soft inside init(); no-op unless MC2_VK_GPU_TIMESTAMPS set.
    mc2vkts::GpuTimer gpuTimer;

    // The overlay / decal pass built on this island's pool and shared state; declared last, so it is released first.
    std::unique_ptr<OvlState> ovl;

    TerrainIsland() = default;
    ~TerrainIsland() override { gpuTimer.destroy(); }
};

const uint32_t kOvlMaxTex = 480;
const uint32_t kOvlVertStride = 28;   // WorldOverlayVert

// Forward decls (COLORMAP-BASE helpers defined after snapshot_height, used earlier).
bool ensure_dummy_colormap(TerrainIsland* s);
void import_colormap(TerrainIsland* s, const Mc2TerrainTier1Feed& feed);
bool ensure_dummy_cement(TerrainIsland* s);
void import_cement(TerrainIsland* s, const Mc2TerrainTier1Feed& feed);
bool ensure_dummy_shore_mask(TerrainIsland* s);
void import_shore_mask(TerrainIsland* s, const Mc2TerrainTier1Feed& feed);
bool ensure_dummy_materials(TerrainIsland* s);
void import_materials(TerrainIsland* s);
bool snapshot_cement(TerrainIsland* s, const Mc2TerrainTier1Feed& feed);


bool bind_static_shadow(TerrainIsland* s, const Mc2TerrainTier1Feed& feed);



bool ensure_dummy_matnormal(TerrainIsland* s);
void import_matnormal(TerrainIsland* s, const Mc2TerrainTier1Feed& feed);
bool ensure_dummy_concrete_detail(TerrainIsland* s);
void import_concrete_detail(TerrainIsland* s);

}  // namespace vk_terrain_detail
using namespace vk_terrain_detail;

// Globals defined in vulkan_terrain_island.cpp (or elsewhere in the engine) that the split pieces share.

// Functions and state of the island, shared by the pieces (defined in the piece that owns them).
namespace vk_terrain_detail {
extern TerrainIslandHealth g_health;
extern InteropForbidden g_forbidden;
extern float g_prefixClearColor[4];
std::unique_ptr<mc2vk::IslandState>& islandSlot();   // the engine context's slot the island is parked in
TerrainIsland* parkedIsland();                       // null until the island is born
extern int  g_terrainInitReachedBuild;
extern bool g_ovlDeclined;
extern std::vector<OvlPending> g_ovlPending;
void setFallback(std::string_view reason);
void vlog(const char* fmt, ...);
bool terrainFenceDeferEnabled();
bool terrainPerfGateRequested();
bool terrainAllLodsRequested();
bool terrainSkirtsRequested();
std::string spv_dir();
mc2vk::OwnedShaderModule load_spv(VkDevice device, std::string_view path);
bool make_buffer(TerrainIsland* s, mc2vk::OwnedBuffer& out, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                 std::string_view name, int i0 = -1, int i1 = -1);
// Makes a fresh host-visible buffer of `size` and retires the old one on the open-frame ticket (no device idle). A false return leaves `buf` untouched.
bool replace_host_buffer(TerrainIsland* s, mc2vk::OwnedBuffer& buf, VkDeviceSize size, VkBufferUsageFlags usage, std::string_view name);
const PatchMesh* getOrBuildPatch(TerrainIsland* s, int qcX, int qcY, int lodStep,
                                 const float* fine = nullptr, int fineSide = 0, int originX = 0, int originY = 0);
const SkirtMesh* getOrBuildSkirtMesh(TerrainIsland* s, int qcX, int qcY, int lodStep);
void write_descriptors(TerrainIsland* s, int parity = -1);
bool ensure_height_capacity(TerrainIsland* s, VkDeviceSize bytes);
// Re-points set[par]'s ring bindings (0 UBO, 11-13 light pool) at the buffers this frame's spans live in.
void sync_ring_bindings(TerrainIsland* s, uint32_t par, const mc2vk::FrameAlloc& ubo, const mc2vk::FrameAlloc (&lp)[3]);
bool init_island(TerrainIsland* s, int w, int h);
bool snapshot_height(TerrainIsland* s, const Mc2TerrainTier1Feed& feed);
VkCommandBuffer begin_oneshot(TerrainIsland* s);
void end_oneshot(TerrainIsland* s, VkCommandBuffer cb);
bool create_colormap_sampler(TerrainIsland* s);
bool upload_sampled_image(TerrainIsland* s, mc2vk::ResidentTextureRef& outTexture, VkFormat fmt, uint32_t width, uint32_t height, const void* bytes, VkDeviceSize byteCount);
bool upload_colormap_image(TerrainIsland* s, VkFormat fmt, uint32_t width, uint32_t height, const void* bytes, VkDeviceSize byteCount);
bool create_cement_sampler(TerrainIsland* s);
bool upload_sampled_image_array(TerrainIsland* s, mc2vk::ResidentTextureRef& outTexture, VkFormat fmt, uint32_t width, uint32_t height, uint32_t layers, const void* bytes, VkDeviceSize byteCount);
bool create_matnormal_sampler(TerrainIsland* s);
bool snapshot_visual_height(TerrainIsland* s, const Mc2TerrainTier1Feed& feed);



void ovl_destroy_tex(OvlTex& t);
void ovl_destroy(TerrainIsland* s);
bool ovl_alloc_tex_set(TerrainIsland* s, OvlState* o, OvlTex& t);
bool ovl_upload_tex(TerrainIsland* s, OvlTex& e, uint32_t w, uint32_t h, VkFormat fmt, const void* bytes, VkDeviceSize byteCount);
OvlTex* ovl_get_tex(TerrainIsland* s, OvlState* o, uint32_t gosHandle);
bool ovl_init(TerrainIsland* s, OvlState* o, int w, int h);
}  // namespace vk_terrain_detail

// Main terrain patches share one frame UBO. Overlays retain the original UBO ABI.
struct TerrainPatchConstants {
    int32_t blockOriginX, blockOriginY;
    int32_t quadCountX, quadCountY;
    int32_t edgeStitch, visualMode;
    float skirtDepth;
    int32_t pad = 0;
};
// One record per indirect draw, read by the vertex shader through buffer device address (LOCKSTEP with TerrainPatchRec in terrain_patch.glsl).
static_assert(sizeof(TerrainPatchConstants) == 32);

// Run-piece helpers the overlay piece also calls.
bool ensure_island_ready(int w, int h);
// Stages this frame's light pool + grid into frame-ring spans lp[0..2] (bindings 11-13 are always bound: an unused one is a small placeholder). False when
// the ring has no memory for them.
bool stage_light_pool(TerrainIsland* s, const Mc2TerrainTier1Feed& feed, int32_t& vkPoolLights, int32_t& vkGridDim, mc2vk::FrameAlloc (&lp)[3]);
void fill_frame_ubo(TerrainIsland* s, const Mc2TerrainTier1Feed& feed, const Mc2TerrainTier1Patch& patch, int32_t vkPoolLights, int32_t vkGridDim, TerrainFrameUBO& u);

#endif // MC2_VULKAN_ISLAND

#endif // VULKAN_TERRAIN_ISLAND_INTERNAL_H
