// VK-FIELD-VFX-1 — Vulkan VFX (particle) island public ABI.
//
// Renders the GPU-particle billboard stream (gos_particle_bridge) into the SHARED
// scene GBuffer color0 as translucent, depth-tested (GEQUAL, no-write), colored
// billboards textured with the group's sprite page (imported once from the texels
// the GL path uploads) — the "weapon fire survives the VK umbrella" slice. The
// same owner also renders native mesh/ribbon geometry and alpha-tested vegetation.
//
// Consumed under #ifdef MC2_VULKAN_ISLAND from gos_particle_bridge.cpp.

#ifndef VULKAN_VFX_ISLAND_H
#define VULKAN_VFX_ISLAND_H

#include <cstdint>

// One contiguous same-blend sub-range of the particle array (mirrors the fields
// of mc2::particles::GroupInfo). start/count are in PARTICLES (the island
// multiplies by 6 for billboards). Geometry mode sentinels and the 64-byte input
// record are declared in vulkan_vfx_geometry.h; vegetation uses two eight-vertex instanced strips.
struct VfxParticleDraw {
    uint32_t start;      // index of first particle in the flat array
    uint32_t count;      // number of particles in this group
    uint32_t additive;   // 1 = additive (SRC_ALPHA/ONE), 2 = pure additive (ONE/ONE, alpha ignored), 0 = alpha (SRC_ALPHA/1-SRC_ALPHA)
    uint32_t texture;    // gos texture handle of the group's sprite page; 0 = none (group skipped, as GL does)
    float    u0, v0;     // sprite sub-rect origin
    float    us, vs;     // sprite sub-rect size (per tile for a flipbook)
    uint32_t atlasColumns;  // > 1: per-particle frame index selects the tile; else static
};

extern "C" {

// True when the VK VFX island is armed (MC2_VK_VFX sub-gate, default-ON under the
// MC2_VK_SHARED_GBUFFER umbrella). The bridge SKIPS its GL particle draw and calls
// mc2_vk_vfx_island_run instead when this returns 1.
int mc2_vk_vfx_island_enabled();

// Per-frame entry. Uploads the whole particle array + frame constants, then issues
// one depth-tested translucent draw per group into the shared scene color0.
//   particles     — flat GpuParticle array (64B each), particleCount entries.
//   draws          — per-group sub-ranges (blend + [start,count) in particles).
//   worldToClipGL  — the 16 raw floats GL uploads GL_FALSE (terrainMVP).
//   camRight/camUp — GL-world-space billboard basis (3 floats each).
//   w,h            — shared scene dimensions.
// Returns 1 if the VK pass rendered this frame; 0 = fell back (caller must NOT
// then GL-draw under the umbrella — 0 means the pass declined, e.g. not ready).
int mc2_vk_vfx_island_run(
    const void* particles, uint32_t particleCount,
    const VfxParticleDraw* draws, uint32_t drawCount,
    const float* worldToClipGL, const float* camRight, const float* camUp,
    int w, int h);

// Teardown (renderer shutdown). Also emits the one-line health summary.
void mc2_vk_vfx_island_destroy();

}  // extern "C"

#endif  // VULKAN_VFX_ISLAND_H
