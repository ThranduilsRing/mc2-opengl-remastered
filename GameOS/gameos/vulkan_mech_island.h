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
// slot-0 GL texture id (per-actor paint scheme resolved LIVE this frame).
struct Mc2MechDrawRun {
    uint32_t firstIndex;    // into shared IBO (indices, not bytes)
    uint32_t indexCount;
    int32_t  baseVertex;    // signed
    uint32_t instanceBase;  // first instance index into the instance SSBO snapshot
    uint32_t instanceCount;
    uint32_t materialFlags; // bit0: ALPHA_TEST
    uint32_t slot0GlTex;    // LIVE GL_TEXTURE_2D id for this run's paint scheme (0=none)
};

// Copy-ONCE geometry feed: the shared mech VBO/IBO GL names + 48B stride.
// VK-MECH-FEED-NATIVE-1 (rung 7): vboCpu/iboCpu are the CPU-side byte sources of
// the SAME buffers (vboCpu = the exact normals-recomputed `work` blob uploaded to
// s_sharedVbo; iboCpu = s_stagingIbo, uploaded verbatim). Non-null ONLY when the
// MC2_VK_MECH_FEED_NATIVE gate is armed; the island then copies from CPU memory
// and never touches the GL buffer objects (no glGetBufferSubData).
struct Mc2MechGeomFeed {
    uint32_t glVbo;         // s_sharedVbo (GpuMechVertex[])
    uint32_t glIbo;         // s_sharedIbo (uint32[])
    uint64_t vboBytes;
    uint64_t iboBytes;
    uint32_t vertexStride;  // sizeof(GpuMechVertex) == 48
    const void* vboCpu;     // native feed: exact uploaded VBO bytes (or null)
    const void* iboCpu;     // native feed: s_stagingIbo bytes (or null)
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
    float        worldToClipGL[16]; // kAxisSwapMC2toGL * worldToClip (row-major, GL_FALSE)
    float        objectDepthBias;    // OBJECT_DEPTH_BIAS
    // VK-MECH-MATERIAL-LIGHTING-1: calc_light + fog parity feed.
    uint32_t     lightDataGlSsbo;    // live LightsData GL SSBO name (binding 20) to copy
    uint64_t     lightDataBytes;     // its byte size (ObjectLights[] * 3600B, grow-once)
    float        fogColorRGB[3];     // g_scene.fogColor.rgb (mech.frag fog mix source)
    // VK-MECH-FEED-NATIVE-1 (rung 7): the game's CPU LightsData array
    // (mcTextureManager->lightData_, the byte SOURCE of the GL SSBO upload).
    // Non-null only when MC2_VK_MECH_FEED_NATIVE is armed; the island then
    // memcpy's from CPU and skips the per-frame glGetBufferSubData readback.
    const void*  lightDataCpu;       // TG_HWLightsData[count] (3600B each) or null
    uint64_t     lightDataCpuBytes;  // count * 3600
};

// Filled by gos_mech_batcher.cpp. Return 0 when geometry/gate not ready.
int mc2_mech_island_get_geometry_feed(Mc2MechGeomFeed* out);
int mc2_mech_island_get_frame_feed(Mc2MechFrameFeed* out);

// True when the mech island wants this frame (gate on + require satisfied). The
// batcher's flush() then SKIPS its GL color draws (still building the SSBO/feed)
// and the caller runs mc2_vk_mech_island_run().
int mc2_vk_mech_island_enabled();

// Per-frame entry. Co-writes the mech instances IN-PLACE into the shared scene
// GBuffer (LOAD-preserve MRT0+MRT1+depth, GEQUAL reverse-Z, no composite).
//   w/h — the scene color target size.
// Returns 1 if the VK mech pass co-wrote this frame; 0 = fell back (the caller
// must then run the GL mech color flush so mechs still draw).
int mc2_vk_mech_island_run(int w, int h);

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
