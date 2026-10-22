// terrain_frame_vk.glsl -- VK-PARITY-OVERLAY-1: the terrain island's set-0 resource
// declarations + lighting/shadow helpers, shared by the VK terrain OVERLAY / DECAL
// fragment shaders (terrain_overlay_tier1.frag, terrain_decal_tier1.frag). The overlay
// pass reuses the terrain island's descriptor set 0 (same TerrainFrameUBO slot layout,
// cement atlas/words, light pool, static/dynamic/CSM shadow maps) so overlays see the
// SAME lighting inputs as the terrain they sit on.
// The fragment-side set-0 resources and shadow helpers live here; the UBO block itself is in
// terrain_frame_ubo_vk.glsl so vertex stages share the same declaration.
// The INCLUDER must enable GL_GOOGLE_include_directive and define PREC.
#ifndef TERRAIN_FRAME_VK_GLSL
#define TERRAIN_FRAME_VK_GLSL
#include "lighting_shadow_vk.glsl"

#include "terrain_frame_ubo_vk.glsl"

layout(set = 0, binding = 2) uniform sampler2D u_colormap;
// CEMENT/ROAD: the VK-owned copy-once import of the GL cement atlas (REPEAT sampler) +
// the map-load snapshot of the GL per-vertex cement-word SSBO (GL binding 25).
layout(set = 0, binding = 3) uniform sampler2D u_cementAtlas;
layout(set = 0, binding = 4, std430) readonly buffer TerrainCementBuf {
    uint cementWordsF[];
} uC;

// VK-PARITY-TERRAIN-2: the GL terrain light pool (GL binding 28) + world-XY light grid
// (GL bindings 29/30). Per-parity host-visible copies of the SAME CPU arrays the GL
// producer (gos_terrain_lighting.cpp PackAndDispatch / BuildAndUploadLightGrid) uploads
// each frame. Member names match the GL frag so the shared include body compiles as-is.
struct PoolLight {
    vec3  position;        // MC2 world (same frame as v_worldPos)
    uint  lightType;       // 1=point 2=spot 3=terrain, 0=skip
    vec3  color;           // R/G/B normalized 0..1
    float closeDistance;
    float farDistance;
    float oneOverDistance;
    float _plPad1;         // >0.5 = mover-attached (producer flag)
    float intensity;
};
layout(set = 0, binding = 11, std430) readonly buffer TerrainLightPoolVK {
    PoolLight poolLights[];
};
layout(set = 0, binding = 12, std430) readonly buffer TerrainLightGridHeaderVK {
    uvec2 lightGridCells[];   // {firstIndex, count} per cell, row-major y*dim + x
};
layout(set = 0, binding = 13, std430) readonly buffer TerrainLightGridIndicesVK {
    uint  lightGridIndices[];
};
#define TLP_CEMENT_WORD(i) uC.cementWordsF[i]
#include "../../include/terrain_light_pool.hglsl"
// VK-TERRAIN-LIGHTING: the STATIC sun shadow map, a VK-owned per-frame copy of the GL
// static shadow depth texture (gos_postprocess getShadowTexture()) into a D32 depth-
// COMPARE image. HW LINEAR compare, compareOp=LESS_OR_EQUAL, CLAMP_TO_BORDER white=1.0
// (matches GL sampler2DShadow). Always a valid descriptor (1x1 dummy until imported);
// uT.enableShadows gates whether it is actually sampled.
layout(set = 0, binding = 5) uniform sampler2DShadow u_staticShadow;
// VK-TERRAIN-DYNSHADOW: the DYNAMIC (building/mech) sun shadow map — a VK-owned per-frame
// copy of the GL dynamic shadow depth texture (pp->getDynamicShadowTexture(), legacy
// single-map path). Same D32 depth-COMPARE format/sampler as static. Always a valid
// descriptor (1x1 dummy until imported); uT.enableDynShadows gates the sample.
layout(set = 0, binding = 7) uniform sampler2DShadow u_dynamicShadow;
// VK-TERRAIN-CSM-NEAR-1: near-cascade array (D32 depth-COMPARE, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
// layers = csmCount-1) + the separate lower-res full-map catch-all cascade (last cascade).
// Both always valid descriptors (1x1/1x1x1 dummies until imported); uT.csmEnabled gates use.
layout(set = 0, binding = 8) uniform sampler2DArrayShadow u_dynamicShadowArray;
layout(set = 0, binding = 9) uniform sampler2DShadow u_dynamicFullMapShadow;

// VK-TERRAIN-CSM-NEAR-1: verbatim port of GL's csmSampleLayer/csmSampleFullMap/csmProject/
// calcDynamicShadow cascade-select (shadow.hglsl). Same Poisson-16 disk, gradient-adaptive
// PCF, slope+texel bias, and cascade-select+overlap-blend math; only the uniform NAMES
// differ (uT.csm* / u_dynamicShadowArray / u_dynamicFullMapShadow vs GL's dynamic*).
const vec2 mv_poissonDisk[16] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2( 0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870), vec2( 0.34495938,  0.29387760),
    vec2(-0.91588581,  0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845), vec2( 0.97484398,  0.75648379),
    vec2( 0.44323325, -0.97511554), vec2( 0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023), vec2( 0.79197514,  0.19090188),
    vec2(-0.24188840,  0.99706507), vec2(-0.81409955,  0.91437590),
    vec2( 0.19984126,  0.78641367), vec2( 0.14383161, -0.14100790)
);

// VK-PARITY-SHADOW-1: bodies moved to lighting_shadow_vk.glsl (lsvk_csmSample*), the
// line-for-line GL csmSampleLayer/FullMap port incl. dist-fade; these bind the UBO knobs.
LsvkShadowKnobs mv_shadowKnobs() {
    return LsvkShadowKnobs(uT.shadowDistFadeWu, uT.shadowDistFadeMax, uT.shadowMinSunSin, uT.shadowKnobFlags);
}
float mv_csmSampleLayer(vec3 projCoords, int layer, vec3 worldPos, float NdotL, float sunAltSin, int numTaps) {
    return lsvk_csmSampleLayer(projCoords, layer, worldPos, NdotL, sunAltSin, numTaps,
                               u_dynamicShadowArray, uT.csmCascadeTexelWorld[layer],
                               uT.csmDepthSpan, uT.shadowSoftness, mv_shadowKnobs());
}
float mv_csmSampleFullMap(vec3 projCoords, vec3 worldPos, float NdotL, float sunAltSin, int numTaps) {
    return lsvk_csmSampleFullMap(projCoords, worldPos, NdotL, sunAltSin, numTaps,
                                 u_dynamicFullMapShadow, uT.csmFullMapTexelWorld,
                                 uT.csmDepthSpan, uT.shadowSoftness, mv_shadowKnobs());
}

bool mv_csmProject(vec3 worldPos, int layer, out vec3 projCoords) {
    mat4 M = uT.csmCascadeMatrices[layer];
    vec4 lsPos = M * vec4(worldPos, 1.0);
    projCoords = lsPos.xyz / lsPos.w;
    projCoords.xy = projCoords.xy * 0.5 + 0.5;
    if (projCoords.z > 1.0 || projCoords.z < 0.0) return false;
    if (projCoords.xy != clamp(projCoords.xy, 0.0, 1.0)) return false;
    return true;
}

// calcDynamicShadow (near-cascade path). Mirrors GL calcDynamicShadow's early-outs
// (enableDynamicShadows/NdotL) via the caller (see main()); this is just the cascade
// loop, verbatim-diffed against shadow.hglsl's PORT-BEGIN/END:calcDynamicShadowCsm block.
// Cascade loop bound: the array length comes from the generated block, never a hand copy.
#define MC2_VK_SHADOW_CSM_MAX GPU_ABI_TERRAINFRAMEUBO_CSMCASCADEMATRICES_LEN
float mv_calcDynamicShadowCsm(vec3 worldPos, float NdotL, float sunAltSin, int numTaps) {
    int count = clamp(uT.csmCount, 1, MC2_VK_SHADOW_CSM_MAX);

    //PORT-BEGIN:calcDynamicShadowCsm
    for (int c = 0; c < count; ++c) {
        vec3 pc;
        if (!mv_csmProject(worldPos, c, pc)) continue;

        float vis = (c == count - 1)
                  ? mv_csmSampleFullMap(pc, worldPos, NdotL, sunAltSin, numTaps)
                  : mv_csmSampleLayer(pc, c, worldPos, NdotL, sunAltSin, numTaps);

        if (c + 1 < count) {
            vec2 d = min(pc.xy, 1.0 - pc.xy);
            float edge = min(d.x, d.y);
            const float band = 0.06;
            if (edge < band) {
                vec3 pc2;
                if (mv_csmProject(worldPos, c + 1, pc2)) {
                    float vis2 = (c + 1 == count - 1)
                               ? mv_csmSampleFullMap(pc2, worldPos, NdotL, sunAltSin, numTaps)
                               : mv_csmSampleLayer(pc2, c + 1, worldPos, NdotL, sunAltSin, numTaps);
                    float t = smoothstep(0.0, band, edge);
                    vis = mix(vis2, vis, t);
                }
            }
        }

        return mix(0.4, 1.0, vis);
    }

    return 1.0;
    //PORT-END:calcDynamicShadowCsm
}

#endif // TERRAIN_FRAME_VK_GLSL
