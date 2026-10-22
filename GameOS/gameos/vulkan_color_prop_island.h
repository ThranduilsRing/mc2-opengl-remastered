// vulkan_color_prop_island.h — VK-P11 STATIC-PROP COLOR: the FIRST VK-native COLOR
// GEOMETRY region (water was color too, but composited a procedural mesh; this is the
// first region that renders REAL static-prop triangle geometry into an exportable
// color image the terrain/mech color regions inherit).
//
// The whole feature is #ifdef MC2_VULKAN_ISLAND; these declarations are extern "C"
// so the caller (mclib/txmmgr.cpp at the GpuStaticPropBatcher::flush() site) pulls in
// NO Vulkan headers.
//
// Gate MC2_VK_PROP_COLOR_TIER1 default-OFF -> GL static-prop color path
// byte-identical. Feed contract (per-frame color instances + shared VBO/IBO geometry)
// is declared in vulkan_shadow_island_live.h (Mc2ColorPropFeed / Mc2ShadowGeomFeed);
// the color island reuses the shadow feed's shared geometry buffers.

#ifndef MC2_VULKAN_COLOR_PROP_ISLAND_H
#define MC2_VULKAN_COLOR_PROP_ISLAND_H

#ifdef MC2_VULKAN_ISLAND

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// True when MC2_VK_PROP_COLOR_TIER1 requests the VK color-prop region (the caller
// then SKIPS the GL GpuStaticPropBatcher::flush() color draws and calls
// mc2_vk_color_prop_island_run instead).
int mc2_vk_color_prop_island_enabled();

// The alpha-ON albedo array view (a VkImageView in SHADER_READ_ONLY layout, opaque here) the island imported for the alpha-tested props, or 0 while the island
// has none. `generation` changes at every map change, so a caller that bound the view re-binds even when a new array comes back with the same handle value. Owned by the island: the caller reads it within the frame and re-queries each frame (a new map brings a new array).
uint64_t mc2_color_prop_island_alpha_content_hash();
int mc2_color_prop_island_get_alpha_array(void** view, uint32_t* generation);

// Per-frame entry. Renders the visible static-prop color instances in-place into the
// shared scene GBuffer. Pulls its geometry/instance feed from the batcher.
//   w/h — the scene color target size (getWidth()/getHeight()).
// Returns 1 if the VK color pass rendered this frame; 0 = fell back
// (caller must then run the GL flush() so props still draw).
int mc2_vk_color_prop_island_run(int w, int h);
// Clears the static-prop batcher per-frame buckets (GL flush() is skipped when the island runs).
void mc2_color_prop_island_end_frame(void);

// The CPU bytes of this map's shared static-prop VBO / IBO (the exact bytes the GL buffers were created from), valid from
// geometry finalize until the next map load. Returns 0 before finalize. Lets a backend build its own buffers without a GL
// readback; implemented by the static-prop batcher (gos_static_prop_batcher_finalize.cpp).
int mc2_static_prop_geometry_cpu(const void** vbo, uint64_t* vboBytes, const void** ibo, uint64_t* iboBytes);
// 1 when GL alpha-tests the shared-geometry packet `packet` this frame (its material's ALPHA_TEST bit or its texture's live alpha flag), else 0.
int mc2_static_prop_packet_alpha_test(uint32_t packet);

// Health accessor (mirrors mc2_render_backend_shadow_region_health).
void mc2_render_backend_color_prop_region_health(
    int* vulkanAvailable, int* buildEnabled, const char** deviceName,
    unsigned long* validationErrors, unsigned long* attempted, unsigned long* usedVulkan,
    const char** fallbackReason, unsigned long* vkDraws,
    int* interopActive, int* vk2glSemActive, unsigned long* usedInterop,
    unsigned long* instRecords, unsigned long* typeRuns, unsigned long* geomVerts,
    unsigned long* forbiddenCopyImgToBuf, unsigned long* forbiddenGetTexImage,
    unsigned long* forbiddenReadPixels,
    double* stageInstUs, double* drawUs, double* totalUs);

// Teardown (called from gameosmain.cpp before the shared device is destroyed).
void mc2_vk_color_prop_island_destroy();

#ifdef __cplusplus
}
#endif

#endif // MC2_VULKAN_ISLAND
#endif // MC2_VULKAN_COLOR_PROP_ISLAND_H
