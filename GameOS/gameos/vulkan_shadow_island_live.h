// vulkan_shadow_island_live.h — VK-P8a SHADOW-INTEROP public API + caster-geometry
// feed contract. The whole feature is #ifdef MC2_VULKAN_ISLAND; these declarations
// are extern "C" so consumers (gos_postprocess.cpp, txmmgr.cpp, gameosmain.cpp,
// debug_state_dump.cpp) pull in NO Vulkan headers.
//
// STEP 0 caster-feed decision (.claude/VK-P8a-STEP0-CASTER-FEED-NOTE.md):
//   - instances = the CPU GpuStaticPropInstance vector (dynShadowSet, 112B each) the
//     GL path already builds each frame — memcpy'd into a VK SSBO.
//   - geometry  = the shared static-prop VBO/IBO — copied ONCE per map into VK-owned
//     buffers via the feed accessor below (P8a fallback (B): VK owns a copy).

#ifndef MC2_VULKAN_SHADOW_ISLAND_LIVE_H
#define MC2_VULKAN_SHADOW_ISLAND_LIVE_H

#ifdef MC2_VULKAN_ISLAND

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// --- Caster geometry feed (provided by gos_static_prop_batcher.cpp) ---------
// A per-type packet range into the shared VBO/IBO (subset of GpuStaticPropPacket).
struct Mc2ShadowPacket {
    uint32_t firstIndex;   // into shared IBO
    uint32_t indexCount;
    uint32_t baseVertex;   // into shared VBO
};

// One-shot geometry feed for the shadow island. The shared VBO/IBO live only as
// immutable GL buffers after finalize (the CPU staging is freed), so the feed hands
// out the GL buffer HANDLES + byte sizes; the island pulls the bytes ONCE per map
// via glGetBufferSubData (a one-time map-load copy, NOT the per-frame shadow-map
// surface — so it does not touch the zero-copy shadow-map invariant). The packet /
// type arrays are batcher-owned and valid until onMapUnload().
struct Mc2ShadowGeomFeed {
    uint32_t       glVbo;           // GL name of the shared static-prop VBO (s_sharedVbo)
    uint64_t       vboBytes;
    uint32_t       glIbo;           // GL name of the shared static-prop IBO (uint32 indices)
    uint64_t       iboBytes;
    uint32_t       vertexStride;    // kVertexStride (position at offset 0, vec3 float)
    const Mc2ShadowPacket* packets; // per-packet ranges (length packetCount)
    uint32_t       packetCount;
    const uint32_t* typeFirstPacket; // [typeCount] firstPacket per typeID
    const uint32_t* typePacketCount; // [typeCount] packetCount per typeID
    uint32_t       typeCount;
};

// Fills *out from the finalized batcher geometry. Returns false if geometry is not
// yet finalized (island then declines the frame; GL path runs). Implemented in
// gos_static_prop_batcher.cpp. Buffers persist until onMapUnload().
int mc2_shadow_island_get_geometry_feed(Mc2ShadowGeomFeed* out);

// --- Island public API (implemented in vulkan_shadow_island_live.cpp) --------

// True when MC2_VK_SHADOW_INTEROP requests the VK shadow region.
int mc2_vk_shadow_island_enabled();

// Per-frame entry. Returns the GL texture handle of the VK-produced shared shadow
// map (>0) if the VK pass rendered+signalled this frame; 0 = fell back (GL path).
//   size          — square shadow map side (getDynamicShadowMapSize()).
//   lightSpace16  — the 16 raw floats GL uploads (GL_FALSE) for lightSpaceMatrix.
//   instances     — the CPU GpuStaticPropInstance vector (112B each).
//   instanceCount — caster instance count.
//   biasFactor/biasUnits — glPolygonOffset(shadowBiasFactor_, shadowBiasUnits_).
unsigned int mc2_vk_shadow_island_run(
    int size, const float* lightSpace16,
    const void* instances, unsigned int instanceCount,
    float biasFactor, float biasUnits);

// Health accessor (mirrors mc2_render_backend_water_region_health).
void mc2_render_backend_shadow_region_health(
    int* vulkanAvailable, int* buildEnabled, const char** deviceName,
    unsigned long* validationErrors, unsigned long* attempted, unsigned long* usedVulkan,
    const char** fallbackReason, unsigned long* vkDraws,
    int* interopActive, int* vk2glSemActive, unsigned long* usedInterop,
    unsigned long* instRecords, unsigned long* typeRuns, unsigned long* geomVerts,
    unsigned long* forbiddenCopyImgToBuf, unsigned long* forbiddenGetTexImage,
    unsigned long* forbiddenReadPixels,
    double* stageInstUs, double* drawUs, double* totalUs);

// Teardown (called from gameosmain.cpp before the shared device is destroyed).
void mc2_vk_shadow_island_destroy();

#ifdef __cplusplus
}
#endif

#endif // MC2_VULKAN_ISLAND
#endif // MC2_VULKAN_SHADOW_ISLAND_LIVE_H
