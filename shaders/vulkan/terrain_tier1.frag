// VK-P10 TERRAIN TIER-1 — 2-output GBuffer FS (in-place shared-GBuffer co-write).
// =============================================================================
// Co-writes the LOD0 terrain patch IN-PLACE into the SHARED scene GBuffer (no
// side-image, no composite), exactly like the prop Tier-1 FS but with the TERRAIN
// shadow-class alpha:
//
//   location=0 color  -> sceneColorTex_  (MRT0, RGBA16F). ★COLORMAP-BASE sub-slice:
//                        samples the REAL colormap atlas (VK-owned copy-once import
//                        of the GL g_atlasGLTex; see vulkan_terrain_island.cpp) at
//                        the SAME world-XY -> atlas-UV as the GL LOD-chunk frag
//                        (terrain_lod_chunk.frag:462-482), including the 9-tap disc
//                        blur, so the VK terrain BASE color matches GL's colormap.
//                        cement/road/overlay-sidecar/detail/POM/lighting are STILL
//                        DEFERRED (later sub-slices). Fail-soft: if the colormap is
//                        not importable/ready (uT.colormapReady==0) fall back to the
//                        olive placeholder so the pass never crashes or holes.
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

// LOCKSTEP with terrain_tier1.vert TerrainFrameUBO (std140, one dynamic slot).
layout(set = 0, binding = 0, std140) uniform TerrainFrameUBO {
    mat4  worldToClipGL;
    int   mapSide;
    float halfMap;
    int   blockOriginX;
    int   blockOriginY;
    // COLORMAP-BASE sub-slice: atlas-UV reconstruction params (mirror the GL
    // u_atlasTopLeftX/Y + u_atlasOneOverWorldUnits uniforms) + a ready flag.
    float atlasTopLeftX;
    float atlasTopLeftY;
    float atlasOneOverWorldUnits;
    int   colormapReady;   // 1 = colormap atlas imported+bound; 0 = fall back olive
} uT;

// COLORMAP-BASE: the VK-owned copy-once import of the GL colormap atlas.
layout(set = 0, binding = 2) uniform sampler2D u_colormap;

void main() {
    vec3 base;
    if (uT.colormapReady != 0) {
        // Mirror terrain_lod_chunk.frag:466-482 EXACTLY (world-XY -> atlas-UV, then
        // the 9-tap disc blur) so the VK base color matches GL's colormap sample.
        vec2 uv;
        uv.x = (v_worldPos.x - uT.atlasTopLeftX) * uT.atlasOneOverWorldUnits;
        uv.y = (uT.atlasTopLeftY - v_worldPos.y) * uT.atlasOneOverWorldUnits;
        const float CMAP_BLUR = 0.0020;
        const float CMAP_R2   = CMAP_BLUR * 0.707;
        base = texture(u_colormap, uv).rgb
             + texture(u_colormap, uv + vec2( CMAP_BLUR, 0.0)).rgb
             + texture(u_colormap, uv + vec2(-CMAP_BLUR, 0.0)).rgb
             + texture(u_colormap, uv + vec2(0.0,  CMAP_BLUR)).rgb
             + texture(u_colormap, uv + vec2(0.0, -CMAP_BLUR)).rgb
             + texture(u_colormap, uv + vec2( CMAP_R2,  CMAP_R2)).rgb
             + texture(u_colormap, uv + vec2(-CMAP_R2,  CMAP_R2)).rgb
             + texture(u_colormap, uv + vec2( CMAP_R2, -CMAP_R2)).rgb
             + texture(u_colormap, uv + vec2(-CMAP_R2, -CMAP_R2)).rgb;
        base /= 9.0;
    } else {
        // Fail-soft olive placeholder (colormap not importable/ready this frame).
        float hMod = clamp(v_worldPos.z / 512.0 + 0.5, 0.35, 1.0);
        base = vec3(0.42, 0.40, 0.28) * hMod;
    }
    fragColor = vec4(base, 1.0);   // opaque in-place (a=1, NOT premult)

    // ★TERRAIN shadow-class GBuffer1: flat-up normal + a=1.0 (handles own shadow).
    GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
}
