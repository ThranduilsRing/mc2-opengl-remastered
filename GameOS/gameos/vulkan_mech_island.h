// vulkan_mech_island.h — VK-P14 MECH TIER-1: the FIRST VK-native SKINNED mech region.
// =============================================================================
// Renders the visible GPU-batcher mech instances IN-PLACE into the SHARED scene
// GBuffer (MRT0 color + MRT1 normal + shared D32S8 depth), VK-skinned (linear
// blend over 4 bone weights + MC2 axis swap), matching the GL mech flush.
//
// Structure = vulkan_color_prop_island.cpp's TIER-1 co-write path, with the
// static-prop 40B vertex + modelMatrix instance swapped for the mech 48B vertex
// (GpuMechVertex) + per-frame bone palette SSBO (GpuMechBone) + instance SSBO
// (GpuMechInstance). The mech VS does LBS skinning (mech.vert:156-180) then the
// Stuff->MC2 axis swap worldMC2=vec3(-x,z,y). SUB-SLICE 1 = skinning + FLAT color
// (the P-conjugation/axis-swap silhouette proof); material + calc_light lighting
// are gated follow-ons (MC2_VK_MECH_MATERIAL / MC2_VK_MECH_LIGHTING).
//
// Gate MC2_VK_MECH_TIER1 (requires MC2_VK_SHARED_SCENE_DEPTH), default-OFF ->
// GL mech color flush byte-identical. The per-frame feed (visible mech instances
// + bone palette + shared VBO/IBO geometry + per-actor slot-0 paint texture) is
// exported by the GL batcher (gos_mech_batcher.cpp) via the Mc2Mech*Feed accessors
// below; NO fenced GL buffer crosses (all copied host-visible).
//
// The whole TU compiles to nothing unless MC2_VULKAN_ISLAND is defined.

#ifndef MC2_VULKAN_MECH_ISLAND_H
#define MC2_VULKAN_MECH_ISLAND_H

#ifdef MC2_VULKAN_ISLAND

#include <cstdint>
#include "../../mclib/vk_object_lights.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// FEED CONTRACT (populated by the GL mech batcher; consumed by the island).
// The island NEVER includes gos_mech_batcher.h — these POD mirrors keep the
// batcher's Vulkan-free.
// ---------------------------------------------------------------------------

// One indexed sub-range of the shared mech IBO for a single (packet,instanceBase).
// Mirrors the flush() DrawCall the GL path issues (globalPacketIdx + instanceBase
// + instanceCount), pre-resolved to firstIndex/indexCount/baseVertex + the live
// slot-0 gos texture handle (per-actor paint scheme resolved LIVE this frame).
struct Mc2MechDrawRun {
    uint32_t firstIndex;    // into shared IBO (indices, not bytes)
    uint32_t indexCount;
    int32_t  baseVertex;    // signed
    uint32_t instanceBase;  // first instance index into the instance SSBO snapshot
    uint32_t instanceCount;
    uint32_t materialFlags; // bit0: ALPHA_TEST
    uint32_t vertexCount;   // vertices of the node range starting at baseVertex (every index of the run lies in it); 0 = unknown, never pre-skinned
    uint64_t slot0Tex;      // LIVE service texture ID of this run's paint scheme (0 = none)
    uint64_t aoTex;         // service texture ID of the per-type AO map (0 = none)
    uint64_t normalTex;     // service texture ID of the per-type normal map (0 = none)
};

// Copy-once geometry feed: the batcher's CPU bytes of this map's mech VBO / IBO (the exact bytes the GL buffers were
// created from: the normals-recomputed vertex blob and the staged indices) and the 48-byte stride. No GL names, so the VK
// island needs no GL buffer. vboCpu is null while a normals rebuild is in flight; the island then retries next frame.
struct Mc2MechGeomFeed {
    uint64_t vboBytes;
    uint64_t iboBytes;
    uint32_t vertexStride;  // sizeof(GpuMechVertex) == 48
    const void* vboCpu;     // native feed: exact uploaded VBO bytes (or null)
    const void* iboCpu;     // native feed: s_stagingIbo bytes (or null)
    uint32_t geomGen;       // bumped on every rebuild of the VBO copy or the staged indices (new map, normals rebuild)
};

// Per-FRAME feed: the instance + bone snapshots the GL flush() just built, plus
// the per-run draw list. All host-visible POD the island memcpy's into VK SSBOs.
struct Mc2MechFrameFeed {
    const void*  instances64;  // GpuMechInstance[instanceCount] (64B each)
    uint32_t     instanceCount;
    const void*  bones64;      // GpuMechBone[boneCount] (64B each) — this frame's palette
    uint32_t     boneCount;
    const Mc2MechDrawRun* runs;
    uint32_t     runCount;
    uint32_t     leafCount;   // actor/node pairs, retained with the feed when an idle frame reuses it
    float        worldToClipGL[16]; // kAxisSwapMC2toGL * worldToClip (row-major, GL_FALSE)
    float        objectDepthBias;    // OBJECT_DEPTH_BIAS
    float        fogColorRGB[3];     // g_scene.fogColor.rgb (mech.frag fog mix source)
    // The game's CPU LightsData array (the bytes the GL SSBO is uploaded from); null before the texture manager exists.
    mc2::vkLights::Feed lights;
    // VK-PARITY-MECH-1: default mech FS shading inputs, from the SAME producers the
    // GL flush uploads (gpuMechShadingParams / GetObjectLightPoolParams / the
    // RenderCore main-view cameraWorldPos that feeds the GL ViewUniforms UBO).
    float        cameraWorldPos[4];  // GL world (ViewUniforms.cameraWorldPos)
    float        ambientStrength;    // MECH-AMBIENT-1 (effective)
    float        specularStrength;   // MECH-SPECULAR-V1 (effective)
    float        metalRoughness;
    float        glassRoughness;
    float        glassLumaThresh;
    float        glassMaxChanThresh;
    int32_t      specDebugMask;
    float        importedGamma;
    float        importedAlbedoScale;
    float        aoStrength;
    float        normalStrength;
    int32_t      numPoolLights;      // object pools (0 = off / none)
    const void*  poolLights48;       // GpuTerrainLight[numPoolLights] (48B each) or null
    int32_t      poolSoft;
    float        poolMoverDim;
    float        poolMoverScale;
    int32_t      poolMoverMax;
    int32_t      poolCompress;
    float        poolObjScale;
};

// The mech casters of the VK shadow cascades: the previous flush's instance and bone snapshots and run list (the data GpuMechBatcher::flushShadow draws), the CPU
// bytes of the geometry and a generation that changes whenever those bytes are rebuilt, so the consumer keeps its own copy and re-uploads only on a change.
struct Mc2MechShadowFeed {
    const void* instances64; uint32_t instanceCount;
    const void* bones64;     uint32_t boneCount;
    const Mc2MechDrawRun* runs; uint32_t runCount;
    const void* vboCpu; uint64_t vboBytes;
    const void* iboCpu; uint64_t iboBytes;
    uint32_t    vertexStride;
    uint32_t    geomGen;
};
// Returns 0 when the shadows are off, the geometry is not finalized or there is nothing to draw (the same guards as the GL flushShadow).
int mc2_mech_island_get_shadow_feed(Mc2MechShadowFeed* out);

// Filled by gos_mech_batcher.cpp. Return 0 when geometry/gate not ready.
int mc2_mech_island_get_geometry_feed(Mc2MechGeomFeed* out);
// Mech batcher's real packed vertex stride (sizeof(GpuMechVertex)); valid before geometry upload.
uint32_t mc2_mech_vertex_stride(void);
int mc2_mech_island_get_frame_feed(Mc2MechFrameFeed* out);

// True when the mech island wants this frame (gate on + require satisfied). The
// batcher's flush() then SKIPS its GL color draws (still building the SSBO/feed)
// and the caller runs mc2_vk_mech_island_run().
int mc2_vk_mech_island_enabled();

// Device-local mech VBO/IBO (VkBuffer handles as uint64) shared with the shadow casters; uploads on demand. 0 = not ready.
int mc2_vk_mech_island_shared_geometry(uint64_t* vbo, uint64_t* ibo, uint32_t* geomGen);

// Per-frame entry. Co-writes the mech instances IN-PLACE into the shared scene
// GBuffer (LOAD-preserve MRT0+MRT1+depth, GEQUAL reverse-Z, no composite).
//   w/h — the scene color target size.
// Returns 1 if the VK mech pass co-wrote this frame; 0 = fell back (the caller
// must then run the GL mech color flush so mechs still draw).
int mc2_vk_mech_island_run(int w, int h);

// Mech-lab panel: the in-game mech material path drawn into the UI preview target.
// The lab stages its actor through the batcher (submitActor + flush builds the feed),
// then posts a request; the preview dispatch (vulkan_legacy_shape_island.cpp) runs
// the flush right after its own pass so the mech LOADs over the panel backdrop.
struct Mc2MechPreviewRequest {
    float worldToClipGL[16];   // same convention as Mc2MechFrameFeed::worldToClipGL (MC2 world -> preview clip, reverse-Z)
    float cameraWorldPos[4];   // GL world (same as Mc2MechFrameFeed::cameraWorldPos)
    const void* lights;        // GpuObjectLights rows (the lab actor submits lightDataIndex 0)
    uint64_t lightBytes;
    int vpX, vpY, vpW, vpH;    // projection rect in the panel's 800x600 logical space (y down), = the CPU path's viewMul/offset
    float exposure;            // linear scale before the panel tonemap
    int sceneW, sceneH;        // shared scene target size (island init / world-run size match)
    float groundX, groundY, groundH;  // mech feet centre and screen height in the panel logical space (groundH 0 = no contact shadow)
};
// Posts (r != NULL) or cancels (NULL) the pending panel draw. The request is consumed by the next flush.
void mc2_vk_mech_preview_request(const Mc2MechPreviewRequest* r);
// Records + submits the pending panel draw into the preview target views (VkImageView handles).
// Returns 1 when it drew, 0 when nothing was pending or it failed (the caller keeps the CPU draw).
int mc2_vk_mech_preview_flush(void* colorView, void* normalView, void* depthView, int w, int h);
// 0 once a preview init/record failure latched (the lab then keeps the CPU mech draw); 1 otherwise.
int mc2_vk_mech_preview_usable(void);
// 1 when the last posted request drew. The lab keeps its CPU draw under the GPU one until this holds.
int mc2_vk_mech_preview_drew_last(void);

// Health accessor (mirrors the prop island's).
void mc2_render_backend_mech_region_health(
    int* vulkanAvailable, int* buildEnabled, const char** deviceName,
    unsigned long* attempted, unsigned long* usedVulkan, const char** fallbackReason,
    unsigned long* vkDraws, int* interopActive, int* vk2glSemActive,
    unsigned long* instRecords, unsigned long* boneRecords, unsigned long* runs,
    unsigned long* geomVerts,
    unsigned long* forbiddenGetTexImage, unsigned long* forbiddenReadPixels,
    double* stageUs, double* drawUs, double* totalUs);

// Teardown (called from gameosmain.cpp before the shared device is destroyed).
void mc2_vk_mech_island_destroy();

#ifdef __cplusplus
}
#endif

#endif // MC2_VULKAN_ISLAND
#endif // MC2_VULKAN_MECH_ISLAND_H
