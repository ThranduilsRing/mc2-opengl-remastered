// vulkan_impact_decals.h -- the Vulkan box-decal pass for weapon impacts (docs/impact-damage-design.md).
//
// The presentation layer (code/impact_presentation.cpp) hands one frame's decal records (impact::GpuDecal, 128 bytes each,
// hot/glowing ones first) to gos_SetImpactDecalFrame; the scene renderer calls mc2_vk_impact_decals_run right after the
// terrain overlay/decal batch, before water and transparent effects. The pass rasterizes each decal box, reconstructs the
// surface from the shared scene depth, and modifies the opaque scene colour in place: a per-channel multiply (albedo
// darkening, char tint and the relit perturbed normal) and, for hot decals, an additive emissive glow.
#pragma once

#include <cstdint>

extern "C" {
void gos_SetImpactDecalFrame(const void* records, uint32_t count, uint32_t hotCount);
int  mc2_vk_impact_decals_run(int w, int h);
int  mc2_vk_impact_decals_prewarm(int w, int h);   // build the pipelines and atlas at load (vk_render_infra prewarm)
void mc2_vk_impact_decals_destroy();

struct Mc2ImpactDecalHealth {
    unsigned long framesDrawn = 0;    // frames the pass recorded and submitted
    unsigned long framesEmpty = 0;    // frames with no live decal (no submit)
    unsigned long declined = 0;       // frames refused (not ready / init failure)
    uint32_t lastCount = 0, lastHot = 0;
    char reason[48] = {0};
};
void mc2_vk_impact_decals_health(Mc2ImpactDecalHealth* out);
}
