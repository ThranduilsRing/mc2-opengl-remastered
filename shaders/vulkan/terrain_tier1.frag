// VK-P10 TERRAIN TIER-1 — 2-output GBuffer FS (in-place shared-GBuffer co-write).
// =============================================================================
// Co-writes the LOD0 terrain patch IN-PLACE into the SHARED scene GBuffer (no
// side-image, no composite), exactly like the prop Tier-1 FS but with the TERRAIN
// shadow-class alpha:
//
//   location=0 color  -> sceneColorTex_  (MRT0, RGBA16F). The recon allows either
//                        the full terrain_lod_chunk.frag splat OR a "recognizable
//                        simplified terrain color". The SMALLEST probe uses the
//                        simplified color (a flat recognizable ground albedo,
//                        elevation-modulated so patch relief reads) — the colormap
//                        atlas sample + full material splat/tint/detail-normal/POM/
//                        cement/lighting are DEFERRED (later sub-slices; importing
//                        the GL colormap texture into VK is not in the current
//                        interop toolkit — GL->VK texture import is a separate seam).
//                        a=1 (opaque, in-place — NOT premult; Tier-1 does not blend).
//
//   location=1 GBuffer1 -> sceneNormalTex_ (MRT1, RGBA16F). ★TERRAIN encoding
//                        vec4(0.5, 0.5, 1.0, 1.0): flat-up normal (0.5,0.5,1.0) +
//                        ★a=1.0 = terrain shadow-class "I handle my own shadow"
//                        (rc_gbuffer1_shadowHandled). BIT-EXACT with
//                        terrain_lod_chunk.frag:430 `vec4(0.5,0.5,1.0,1.0)`. This is
//                        the ONE difference vs props (a=0.25); the coverage oracle
//                        histograms a==1.0 inside the terrain silhouette.
#version 450

layout(location = 0) in vec3 v_worldPos;

layout(location = 0) out vec4 fragColor;   // MRT0 color
layout(location = 1) out vec4 GBuffer1;    // MRT1 normal + shadow class

void main() {
    // Recognizable simplified terrain color: muted olive/khaki ground, gently
    // modulated by elevation so the LOD0 patch's relief reads in the A/B capture.
    // (Full colormap-atlas + material splat is a later sub-slice.)
    float hMod = clamp(v_worldPos.z / 512.0 + 0.5, 0.35, 1.0);
    vec3 base = vec3(0.42, 0.40, 0.28) * hMod;
    fragColor = vec4(base, 1.0);   // opaque in-place (a=1, NOT premult)

    // ★TERRAIN shadow-class GBuffer1: flat-up normal + a=1.0 (handles own shadow).
    GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
}
