// LOD terrain patches co-write the shared scene GBuffer in place.
// MRT0: opaque colormap with cement, material detail, shadows and local lighting.
// MRT1: flat-up normal plus alpha=1, the terrain's self-shadowed receiver class.
// Colormap UVs and material shading retain terrain_lod_chunk.frag conventions.
#version 450
#extension GL_GOOGLE_include_directive : require
#ifndef PREC
#define PREC highp
#endif
layout(constant_id = 10) const int kPlanarReflection = 0;
// Prototype cuts: set a feature to 0 to restore it in the reflected pass.
layout(constant_id = 11) const int kReflectCutPom = 1;   // used by the cliff parallax (terrain_cliff_material.glsl)
layout(constant_id = 12) const int kReflectCutDetail = 1;
layout(constant_id = 13) const int kReflectCutPools = 1;
layout(constant_id = 14) const int kReflectCutMovers = 1;
#define GROUND_POOL_ENABLED (kPlanarReflection == 0 || kReflectCutPools == 0 || uEnv.sceneLight.x < 1.0)
#define GROUND_MOVER_CONTACT (kPlanarReflection != 0 && kReflectCutMovers != 0 ? 0.0 : uT.moverContactShadow)
#include "../include/noise.hglsl"   // VK-PARITY-F3: fbm() for the GL colour break-up (terrain_lod_chunk.frag:1839)
vec3 mv_lampPoolNormal(vec3 wp, vec3 macroN, float lod);
#define TLP_LAMP_NORMAL mv_lampPoolNormal
#include "include/terrain_frame_vk.glsl"   // TerrainFrameUBO + set-0 resources + CSM shadow helpers
#define TS_authoredMaterialsOnly (uT.reserved220 != 0.0)

// -1 preserves the uniform path for generic and skirt pipelines.
layout(constant_id = 20) const int kSp_colormapReady = -1;
#define TS_colormapReady (kSp_colormapReady < 0 ? uT.colormapReady : kSp_colormapReady)
layout(constant_id = 21) const int kSp_useCement = -1;
#define TS_useCement (kSp_useCement < 0 ? uT.useCement : kSp_useCement)
layout(constant_id = 22) const int kSp_useTransitionMask = -1;
#define TS_useTransitionMask (kSp_useTransitionMask < 0 ? uT.useTransitionMask : kSp_useTransitionMask)
layout(constant_id = 23) const int kSp_cementDiagConnect = -1;
#define TS_cementDiagConnect (kSp_cementDiagConnect < 0 ? uT.cementDiagConnect : kSp_cementDiagConnect)
layout(constant_id = 24) const int kSp_lightingEnabled = -1;
#define TS_lightingEnabled (kSp_lightingEnabled < 0 ? uT.lightingEnabled : kSp_lightingEnabled)
layout(constant_id = 25) const int kSp_enableShadows = -1;
#define TS_enableShadows (kSp_enableShadows < 0 ? uT.enableShadows : kSp_enableShadows)
layout(constant_id = 26) const int kSp_detailEnabled = -1;
#define TS_detailEnabled (kSp_detailEnabled < 0 ? uT.detailEnabled : kSp_detailEnabled)
layout(constant_id = 27) const int kSp_detailReady = -1;
#define TS_detailReady (kSp_detailReady < 0 ? uT.detailReady : kSp_detailReady)
layout(constant_id = 28) const int kSp_concreteDetail = -1;
#define TS_concreteDetail (kSp_concreteDetail < 0 ? uT.concreteDetail : kSp_concreteDetail)
layout(constant_id = 29) const int kSp_visualMode = -1;
#define TS_visualMode (kSp_visualMode < 0 ? uT.visualMode : kSp_visualMode)
layout(constant_id = 30) const int kSp_shorelineOn = -1;
#define TS_shorelineOn (kSp_shorelineOn < 0 ? uT.shorelineOn : kSp_shorelineOn)
layout(constant_id = 31) const int kSp_shorelineHasMask = -1;
#define TS_shorelineHasMask (kSp_shorelineHasMask < 0 ? uT.shorelineHasMask : kSp_shorelineHasMask)
#undef TLP_LAMP_NORMAL
#include "include/ground_lighting.glsl"
#include "include/concrete_detail_vk.glsl"
#include "include/world_patch_noise.glsl"   // GROUND-MOTTLE-REF3: shared world patch field (vegetation colours from it too)
#include "../include/axis.hglsl"   // mc2ToRender: the POM view vector is built in the MC2 world frame
#include "../include/edge_haze.hglsl"   // edgeHazeAmount + EDGE_HAZE_SKY, shared with GL

layout(location = 0) in vec3 v_worldPos;
layout(constant_id = 0) const int kCsmDebugMode = 0;   // vk-present-instance 8: spec constant from MC2_SHADOW_DEBUG_MODE; mode 0 compiles the debug chain out
layout(location = 0) out vec4 fragColor;   // MRT0 color
layout(location = 1) out vec4 GBuffer1;    // MRT1 normal + shadow class

layout(set = 0, binding = 16) uniform sampler2D u_macroGradient;
layout(set = 0, binding = 15) uniform sampler2D u_materialWeights;
// VK-TERRAIN-DETAIL: the material tangent-space detail-normal array (VK copy-once
// import of GL terrain_normal_array_tex_, a 9-layer RGBA8 GL_TEXTURE_2D_ARRAY,
// GL_REPEAT). Layers per include/terrain_mat_layers.hglsl. Dummy 1x1 until imported
// (TS_detailReady gates the sample).
layout(set = 0, binding = 6) uniform sampler2DArray u_matNormalArray;
layout(set = 0, binding = 1, std430) readonly buffer TerrainHeightBuf {
    float heights[];
} uH;
// TERRAIN-SHORELINE-V3: optional mask sidecar (G wet, B foam, A valid); read only while TS_shorelineHasMask != 0.
layout(set = 0, binding = 14) uniform sampler2D u_shorelineMask;
layout(set = 0, binding = 20) uniform sampler2D u_forestMask;   // tree-canopy density (R8) over the colormap atlas extent

// Material layer indices (mirror shaders/include/terrain_mat_layers.hglsl).
#define MAT_LAYER_ROCK     0
#define MAT_LAYER_GRASS    1
#define MAT_LAYER_DIRT     2
#define MAT_LAYER_CONCRETE 3
#define MAT_LAYER_SNOW     4
#define MAT_LAYER_FOREST   6   // optional 7th layer: under-forest ground (forest_floor_normal.ktx2); absent -> array depth 6
#define MAT_LAYER_MARBLE_CLIFF 5   // mat5_normal.tga: RGB = marble microsurface normal, A = the cooked cliff displacement

// Profile class controls preserve each texture's authored relative density.
// Flat reference: dirt/snow 768 wu; slope reference: triplanar rock 320 wu;
// cement reference: pad detail 48 wu. Legacy top-down rock remains 2.4x slope.
// TERRAIN-NORMAL-UNIFORM: grass and dirt 0.5: the cooked turf and sand sources have the same fine relief (rms slope
// ~0.27), so flat ground carries ~0.13 slope like the rock band (rock 0.9/3 x 0.3 x crNavigable 0.4/0.3). Dirt was 1.1 on a near-flat source.
#define MATV_NORMAL_BOOST (vec4(0.9, 0.5, 0.5, CD_CONCRETE_RELIEF_STRENGTH) * vec4(uT.normalSlopeStrength, uT.normalFlatStrength, uT.normalFlatStrength, uT.normalCementStrength))
#define MATV_TILING vec4(3.0 * 320.0 / uT.normalSlopePeriod, 2.0 * 768.0 / uT.normalFlatPeriod, 768.0 / uT.normalFlatPeriod, 6.0 * 48.0 / uT.normalCementPeriod)
#define MATV_TILING_SNOW (768.0 / uT.normalFlatPeriod)
const float GM_STRENGTH       = 0.9;   // ground mottle (world_patch_noise.glsl) blend on full natural ground
const float GM_LUMA_KEEP      = 0.25;   // share of the colormap brightness the mottle keeps (dark soil keeps all of it)
const float GM_BEACH_WU       = 40.0;   // horizontal run above the waterline that stays clean beach (~10 m)
const float GM_COAST_DRY_WU   = 400.0;  // straw fades out to the inland sward over this run (~100 m)
const float MATV_WU_PER_TILE  = 768.0;  // MAT_WORLD_UNITS_PER_TILE (terrain_lod_chunk.frag:240)
#include "include/terrain_detail_clamp.glsl"

// Central difference commutes with bilinear interpolation; normalize after filtering.
// Call at the point of use: hoisting the result to the top of main() kept it live across the whole shader and cost ~15% at 4K.
// TERRAIN-HIRES-NEAR-1: the 4x visual-height bake (same SSBO the VS displaces with).
layout(set = 0, binding = 10, std430) readonly buffer TerrainVisualHeightBuf {
    float heightsFine[];
};
#include "include/terrain_cliff_surface.glsl"
#include "include/terrain_hex_tiling.glsl"
// The filtered height is baked per bake version (terrain_fine_height_bake.h): plane 3 of heightsFine, (V+2)^2 cells, p = -1..V.
// Outside the apron (only a zero-weight tap at the map edge) the per-tap path gives the exact old value.
float mv_fineH(ivec2 p) {
    const int V = int(uT.visualSide);
    if (all(greaterThanEqual(p, ivec2(-1))) && all(lessThanEqual(p, ivec2(V))))
        return heightsFine[3 * V * V + (p.y + 1) * (V + 2) + (p.x + 1)];
    return cliffVisualHeight(p);
}
vec2 mv_fineGrad(ivec2 p) {
    return vec2(-(mv_fineH(p + ivec2(1, 0)) - mv_fineH(p - ivec2(1, 0))),
                  mv_fineH(p + ivec2(0, 1)) - mv_fineH(p - ivec2(0, 1))) / 64.0;
}

vec3 mv_smoothTerrainNormal(vec2 worldXY) {
    vec2 cell = vec2(worldXY.x + uT.halfMap, uT.halfMap - worldXY.y) / 128.0;
    vec2 gradient = textureLod(u_macroGradient, (cell + 1.5) / float(uT.mapSide + 2), 0.0).rg;
    // TERRAIN-HIRES-NEAR-1: with the 4x bake live, shade from the bake's own gradient
    // (central differences at 32 wu, bilinear between fine vertices), same convention.
    if (TS_visualMode != 0) {
        vec2  fc = vec2(worldXY.x + uT.halfMap, uT.halfMap - worldXY.y) / 32.0;
        ivec2 i0 = ivec2(floor(fc));
        vec2  f  = fc - vec2(i0);
        vec2 gF = mix(mix(mv_fineGrad(i0), mv_fineGrad(i0 + ivec2(1, 0)), f.x),
                      mix(mv_fineGrad(i0 + ivec2(0, 1)), mv_fineGrad(i0 + ivec2(1, 1)), f.x), f.y);
        gradient = mix(gradient, gF, clamp(uT.visualDisplaceFar, 0.0, 1.0));
    }
    return normalize(vec3(gradient, 1.0));
}

// Gradients come from uniform main() control flow, before cement/material
// selection. Offset anti-tile lookups use the same continuous UV footprint.
vec2 mv_hash22(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}
vec4 mv_sampleAntiTileArr(int layer, vec2 uv, float scale, vec2 dx, vec2 dy, float fw) {
    vec4 s0 = textureGrad(u_matNormalArray, vec3(uv,        float(layer)), dx, dy);
    // Hex-tiling (Mikkelsen 2022) replaces the cell-offset blend on natural layers; concrete keeps its grid-aligned slabs.
    if (layer != MAT_LAYER_CONCRETE)
        return mix(s0, hexTileGrad(u_matNormalArray, float(layer), uv, dx, dy, 1.0, true, vec4(0.5)), smoothstep(0.4, 0.6, fw));
    vec2 f = fract(uv / scale);
    float w1 = smoothstep(0.2, 0.5, f.x) * smoothstep(0.2, 0.5, f.y);
    float w2 = smoothstep(0.2, 0.5, 1.0 - f.x) * smoothstep(0.2, 0.5, 1.0 - f.y);
    // Independent taps overlap without divergent texture branches on NVIDIA.
    // Explicit gradients and the original zero blend weights preserve the footprint.
    vec2 off1 = mv_hash22(floor(uv / scale)) * scale;
    vec4 s1 = textureGrad(u_matNormalArray, vec3(uv + off1, float(layer)), dx, dy);
    vec2 off2 = mv_hash22(floor(uv / scale) + vec2(7.0, 13.0)) * scale;
    vec4 s2 = textureGrad(u_matNormalArray, vec3(uv + off2, float(layer)), dx, dy);
    vec4 antiTile = (s0 + s1 * w1 + s2 * w2) / (1.0 + w1 + w2);
    return mix(s0, antiTile, smoothstep(0.4, 0.6, fw));
}

// The stock burnin includes directional shade and cast shadows. Estimate its
// low-frequency attenuation from a spatial neighbourhood, preserving hue.
// Only steep natural terrain calls this; cement and flat ground stay exact.
float cliffReferenceLuma(vec2 worldXY) {
    vec2 uv = vec2((worldXY.x - uT.atlasTopLeftX) * uT.atlasOneOverWorldUnits,
                   (uT.atlasTopLeftY - worldXY.y) * uT.atlasOneOverWorldUnits);
    float o = 512.0 * uT.atlasOneOverWorldUnits;
    vec3 reference = 0.25 * (textureLod(u_colormap, uv + vec2(o, 0), 0.0).rgb
                          + textureLod(u_colormap, uv - vec2(o, 0), 0.0).rgb
                          + textureLod(u_colormap, uv + vec2(0, o), 0.0).rgb
                          + textureLod(u_colormap, uv - vec2(0, o), 0.0).rgb);
    return dot(reference, vec3(0.299, 0.587, 0.114));
}

#include "include/terrain_cliff_material.glsl"   // MATV_CLIFF_*, mv_cliffMaterial, crApply

// --- GL detail-normal accumulation (terrain_lod_chunk.frag chunkDetailNormal,
// :341-419). NOW PORTED (this sub-slice): per-layer weighted tangent-space normals
// (rock /3 boost, grass, dirt, ★concrete, snow) + ★per-layer fwidth sub-pixel fades
// (fwRock..fwSnow, GL :353-357) + anti-tile resampling blended over fw 0.4..0.6.
// Concrete layer (MAT_LAYER_CONCRETE, matTiling.w) was
// previously omitted — now included to match GL. POM was removed (1/4 px offset). DEFERRED vs GL: LOD-tier fade, painted-concrete normal, cliff
// albedo re-projection (VK-PARITY-GAPS row 14).
#include "include/terrain_detail_normal.glsl"

// Lamp-only detail uses an isotropic footprint computed in uniform main flow.
// The normal is sampled once at the first unoccluded, nonzero lamp.
vec3 mv_lampPoolNormal(vec3 wp, vec3 macroN, float lod) {
    if (TS_authoredMaterialsOnly) return macroN;
    // No hard footprint cut: mv_detailFade below reaches 0 (amount <= 0 -> macroN) well before the sampled mip goes whole-texture.
    vec2 uv = vec2((wp.x - uT.atlasTopLeftX) * uT.atlasOneOverWorldUnits,
                   (uT.atlasTopLeftY - wp.y) * uT.atlasOneOverWorldUnits);
    vec4 m = TS_colormapReady != 0 ? textureLod(u_materialWeights, uv, 0.0) : vec4(0);
    vec4 w = vec4(max(1.0 - m.x - m.y - m.w, 0.0), m.x, m.y, m.w);
    // Keep the lamp consumer small: one XY-only tap with explicit isotropic
    // LOD, no sun/POM/anti-tile call graph inside the light loop.
    vec4 tiling = vec4(MATV_TILING.x / 3.0, MATV_TILING.yzw);
    vec4 fade = vec4(mv_detailFade(mv_detailTexelGM * tiling.x), mv_detailFade(mv_detailTexelGM * tiling.y),
                     mv_detailFade(mv_detailTexelGM * tiling.z), mv_detailFade(mv_detailTexelGM * tiling.w));
    w = mv_groundDetailWeights(w, macroN.z);
    vec4 strength = w * MATV_NORMAL_BOOST * vec4(1.0 / 3.0, 1, 1, 1) * fade;
    // A lamp needs micro-relief, not a second full material evaluation. Use
    // the strongest visible layer; sun/shore/debug retain the weighted blend.
    int layer = strength.y > strength.x ? 1 : 0;
    if (strength.z > strength[layer]) layer = 2;
    if (strength.w > strength[layer]) layer = 3;
    float scale = tiling[layer];
    float amount = strength[layer];
    float snow = m.z * 0.9 * uT.normalFlatStrength * mv_detailFade(mv_detailTexelGM * MATV_TILING_SNOW);
    if (snow > amount) { layer = MAT_LAYER_SNOW; scale = MATV_TILING_SNOW; amount = snow; }
    if (amount <= 0.0) return macroN;
    vec2 detailUV = wp.xy * (uT.detailTiling / MATV_WU_PER_TILE) * scale;
    vec2 dN = amount * mv_detailDecode(textureLod(u_matNormalArray, vec3(detailUV, float(layer)), lod + log2(scale)).rgb).xy;
    return normalize(vec3(macroN.xy / max(macroN.z, 0.2) + dN * uT.detailStrength, 1.0));
}

#include "include/terrain_cement_art.glsl"   // cCutLt, shadeRoadArt, shadeRoadArtTile

void main() {
    if (kPlanarReflection != 0) {
        vec3 n = cross(dFdx(v_worldPos), dFdy(v_worldPos));
        if (dot(n * sign(n.z), mc2ToRender(uT.cameraPosMC2.xyz) - v_worldPos) < 0.0) discard;
    }
    if (kPlanarReflection != 0 && v_worldPos.z < uT.shorelineWaterElev) discard;
    vec3 roadColor;
    float roadCoverage = shadeRoadArtTile(roadColor);
    float cdRoad = 0.0;   // CONCRETE-ROAD-PBR-DETAIL: the cement atlas texel is asphalt (roads, runway)
    vec3 base;
    float cookedBreakup = 0.5;
    vec4 materials = vec4(0.0);
    // VK-TERRAIN-LIGHTING: track whether this fragment resolved to a solid cement slab.
    // GL treats pure concrete as a flat-lit slab (no NdotL relief) and drops the STATIC
    // self-shadow on it (terrain_lod_chunk.frag:894 + :902-907). We mirror that: on a
    // solid-cement hit -> flat-lit with ordinary static and dynamic shadows. (The GL dynamic building shadow on
    // concrete is a SEPARATE dynamic map, still deferred here.)
    float pureConcrete = 0.0;
    bool cementHit = false;   // a solid cement atlas texel won (the shoreline wet band skips it, as GL)
    // Bounded shallow refraction of the seabed material, fading to zero at the waterline.
    // Displace texture reads only: terrain coverage, shadows and material classification stay fixed.
    vec2 seabedOffset = vec2(0.0);
    if (uT.shorelineOn > 1 && kPlanarReflection == 0) {
        float depth = max(uT.shorelineWaterElev - v_worldPos.z, 0.0);
        float phase = dot(v_worldPos.xy, vec2(0.003, 0.002)) + uT.shorelineTime * 1.05;
        seabedOffset = vec2(sin(phase), cos(phase * 1.37)) * min(depth * 0.12, 4.0)
                     * (1.0 - smoothstep(24.0, 80.0, depth));
    }
    if (TS_colormapReady != 0) {
        // The shared cook already applied the original nine-tap blur and coast-color repair.
        vec2 uv = vec2((v_worldPos.x - uT.atlasTopLeftX) * uT.atlasOneOverWorldUnits,
                       (uT.atlasTopLeftY - v_worldPos.y) * uT.atlasOneOverWorldUnits);
        vec2 refractedUV = uv + seabedOffset * vec2(1.0, -1.0) * uT.atlasOneOverWorldUnits;
        vec4 macro = textureGrad(u_colormap, refractedUV, dFdx(refractedUV), dFdy(refractedUV));   // mip chain; the weights below stay LOD 0
        base = macro.rgb;
        cookedBreakup = macro.a;
        materials = textureLod(u_materialWeights, uv, 0.0);
    } else {
        // Fail-soft olive placeholder (colormap not importable/ready this frame).
        float hMod = clamp(v_worldPos.z / 512.0 + 0.5, 0.35, 1.0);
        base = vec3(0.42, 0.40, 0.28) * hMod;
    }

    // ★CEMENT/ROAD sub-slice: concrete apron / roads. Ports the GL LOD-chunk cement
    // branch (terrain_lod_chunk.frag:484-624) VERBATIM: the solid-tile override, the
    // CEMENT-HARD-EDGE-1 marching-squares transition cut, and CEMENT-DIAG-CONNECT-1.
    // Fed by the per-vertex cement-word SSBO (uC) + the REPEAT-sampled cement texture array.
    // The overlay-sidecar composite (GL :626-642) is STILL DEFERRED (not fed here).
    // TS_useCement==0 (gate off / atlas or words not ready) -> whole block skipped ->
    // colormap base only, byte-identical to the colormap-only frame.
    if (TS_useCement != 0) {
        // CEMENT-MIP-EDGE-1: the road "outline". Derivatives come from the CONTINUOUS tile UV in uniform
        // control flow (a fract()ed UV would blow them up at every tile wrap).
        vec2  cCont    = vec2(v_worldPos.x, -v_worldPos.y) / uT.cementWUPT;
        vec2  cGx      = dFdx(cCont), cGy = dFdy(cCont);
        // CEMENT-MIP-EDGE-1: per-pixel tile-UV footprint, for the antialiased transition cut below
        // (the marching-squares cut was binary, a hard aliased stair along every road side).
        float cFu = abs(cGx.x) + abs(cGy.x);
        float cFv = abs(cGx.y) + abs(cGy.y);
        int  ctX = clamp(int(floor((v_worldPos.x + uT.halfMap) / 128.0)), 0, uT.mapSide - 1);
        int  ctY = clamp(int(floor((uT.halfMap - v_worldPos.y) / 128.0)), 0, uT.mapSide - 1);
        uint cw  = uC.cementWordsF[ctX + ctY * uT.mapSide];
        if ((cw & 0x80000000u) != 0u) {
            uint cLayerIdx    = cw & 0xFFFFu;
            bool isTransition = (cw & 0x40000000u) != 0u;
            vec2 cTileUV  = fract(vec2(v_worldPos.x, -v_worldPos.y) / uT.cementWUPT);
            // CEMENT-ARRAY-1: one array layer per cell, so the tile UV samples directly (no cell borders to inset).
            vec4 cementTexel = textureGrad(u_cementArt, vec3(cTileUV, float(cLayerIdx)), cGx, cGy); cdRoad = 1.0 - smoothstep(0.20, 0.30, dot(cementTexel.rgb, vec3(0.299, 0.587, 0.114)));
            vec3 cementColor = cementTexel.rgb * vec3(uT.missionFlatR, uT.missionFlatG, uT.missionFlatB);  // CEMENT-NIGHT-LIGHT-1
            // CEMENT-ALPHA-LAND-1 (terrain_lod_chunk.frag:1009-1015): OWN_ALPHA transition.
            // Word bit23 => the atlas cell carries its own coverage in alpha; composite OVER base.
            bool cementOwnAlpha = (cw & 0x00800000u) != 0u;
            if (isTransition && cementOwnAlpha) {
                vec4 edge = concreteSample(cTileUV, ivec2(ctX, ctY), 0, cGx, cGy);
                float cOwnA = edge.a;
                cementColor = mix(edge.rgb * vec3(uT.missionFlatR, uT.missionFlatG, uT.missionFlatB), cementColor, step(0.999, cementTexel.a));
                base = mix(base, cementColor, cOwnA);
                pureConcrete = cOwnA;
                cementHit = cOwnA > 0.5;
            } else if (isTransition) {
                if (TS_useTransitionMask != 0) {
                    float u = cTileUV.x, v = cTileUV.y;
                    // Cook stores vertex-corner coverage as maskId = cfg-1 (neighbour words would invent corner wedges).
                    int cfg = int((cw >> 24u) & 63u) + 1;
                    float alpha = 0.0;
                    if (cfg == 0) {
                        alpha = 0.0;
                    } else if (cfg == 15) {
                        alpha = 1.0;
                    } else if (cfg == 8) {        // cTL only
                        alpha = cCutLt(u + v, cFu + cFv);
                    } else if (cfg == 4) {        // cTR only
                        alpha = cCutLt((1.0 - u) + v, cFu + cFv);
                    } else if (cfg == 2) {        // cBR only
                        alpha = cCutLt((1.0 - u) + (1.0 - v), cFu + cFv);
                    } else if (cfg == 1) {        // cBL only
                        alpha = cCutLt(u + (1.0 - v), cFu + cFv);
                    } else if (cfg == 7) {        // missing cTL
                        alpha = (1.0 - cCutLt(u + v, cFu + cFv));
                    } else if (cfg == 11) {       // missing cTR
                        alpha = (1.0 - cCutLt((1.0 - u) + v, cFu + cFv));
                    } else if (cfg == 13) {       // missing cBR
                        alpha = (1.0 - cCutLt((1.0 - u) + (1.0 - v), cFu + cFv));
                    } else if (cfg == 14) {       // missing cBL
                        alpha = (1.0 - cCutLt(u + (1.0 - v), cFu + cFv));
                    } else if (cfg == 12) {       // cTL+cTR, top
                        alpha = cCutLt(v, cFv);
                    } else if (cfg == 6) {        // cTR+cBR, right
                        alpha = (1.0 - cCutLt(u, cFu));
                    } else if (cfg == 3) {        // cBR+cBL, bottom
                        alpha = (1.0 - cCutLt(v, cFv));
                    } else if (cfg == 9) {        // cBL+cTL, left
                        alpha = cCutLt(u, cFu);
                    } else if (cfg == 10) {       // cTL+cBR
                        alpha = max(cCutLt(u + v, cFu + cFv), cCutLt((1.0 - u) + (1.0 - v), cFu + cFv));
                    } else if (cfg == 5) {        // cTR+cBL
                        alpha = max(cCutLt((1.0 - u) + v, cFu + cFv), cCutLt(u + (1.0 - v), cFu + cFv));
                    }
                    base = mix(base, cementColor, alpha);
                    if (alpha > 0.5) { cementHit = true; pureConcrete = 1.0; }
                }
            } else {
                base = cementColor;
                cementHit = true;
                pureConcrete = 1.0;
            }
        }

        // CEMENT-DIAG-CONNECT-1 (gate TS_cementDiagConnect): fill this tile's corner
        // quadrant toward a diagonally adjacent SOLID cement tile. Off by default.
        if (TS_cementDiagConnect != 0 && !cementHit) {
            vec2 dTileUV = fract(vec2(v_worldPos.x, -v_worldPos.y) / uT.cementWUPT);
            int dnX = (dTileUV.x > 0.5) ? (ctX + 1) : (ctX - 1);
            int dnY = (dTileUV.y > 0.5) ? (ctY + 1) : (ctY - 1);
            if (dnX >= 0 && dnX < uT.mapSide && dnY >= 0 && dnY < uT.mapSide) {
                uint dnw = uC.cementWordsF[dnX + dnY * uT.mapSide];
                bool dnValid      = (dnw & 0x80000000u) != 0u;
                bool dnTransition = (dnw & 0x40000000u) != 0u;
                if (dnValid && !dnTransition) {
                    uint dLayerIdx = dnw & 0xFFFFu;
                    base = textureGrad(u_cementArt, vec3(dTileUV, float(dLayerIdx)), cGx, cGy).rgb * vec3(uT.missionFlatR, uT.missionFlatG, uT.missionFlatB);  // CEMENT-NIGHT-LIGHT-1
                    pureConcrete = 1.0;
                    cementHit = true;   // as GL's diag-connect branch
                }
            }
        }
    }

    // ★VK-TERRAIN-LIGHTING compose. Mirrors terrain_lod_chunk.frag:889-928 (the NdotL
    // relief band + baked static sun shadow, cement flat-lit). Terrain
    // normal is the flat-up (0,0,1) the GBuffer1 encodes and GL's calcShadow uses as
    // shadowN. Gate TS_lightingEnabled==0 -> lit==base (identity) -> byte-identical to
    // the pre-lighting frame. DEFERRED vs GL: detail-normal/POM/macro-variation relief,
    // the DYNAMIC (mech/building) shadow, and hemisphere ambient fill.
    // VK-PARITY-F3: GL baseColor pipeline (terrain_lod_chunk.frag:1546-1565,1628,1636-1663,
    // 1838-1845) -- default-ON in GL, previously absent here so VK terrain albedo was the raw
    // colormap (darker and less blue than GL on shaded/grassy ground, day and night):
    //   materialTint blend (tintStrength = mix(0.18..0.50 by luma, 0.85, snow) * scale),
    //   restore authored cement tone, cliff desaturate+darken toward rock on steep MACRO slope,
    //   world-space break-up noise (0.78..1.18), snow brightness dampen.
    // Overlay-sidecar coverage is 0 in VK (sidecar not fed), matching GL when OVERLAY_V2 is off.
    // One baked blend feeds tint, detail and shoreline affinity.
    if (TS_authoredMaterialsOnly) materials = vec4(0.0);
    float materialSnow = materials.z;
    vec4 materialW = vec4(max(1.0 - materials.x - materials.y - materials.w, 0.0), materials.x, materials.y, materials.w);
    vec3 baseNormal = mv_smoothTerrainNormal(v_worldPos.xy);
    vec3 prelitBase = base;
    float delitShare = (1.0 - pureConcrete) * (1.0 - smoothstep(0.55, 0.85, abs(baseNormal.z)));
    if (!TS_authoredMaterialsOnly && delitShare > 0.0 && TS_colormapReady != 0) {
        float gain = cliffDelightGain(dot(base, vec3(0.299, 0.587, 0.114)), cliffReferenceLuma(v_worldPos.xy));
        base *= mix(1.0, gain, delitShare);
    }
    vec3 baseColor = base;
    float F3pc = pureConcrete;
    if (!TS_authoredMaterialsOnly) {
        vec4 f3W = materialW; float f3Snow = materialSnow;
        f3W = mix(f3W, vec4(0.0, 0.0, 0.0, 1.0), F3pc);
        f3Snow *= (1.0 - F3pc);
        vec3 materialTint = vec3(uT.tintRockR, uT.tintRockG, uT.tintRockB) * f3W.x
                          + vec3(uT.tintGrassR, uT.tintGrassG, uT.tintGrassB) * f3W.y
                          + vec3(uT.tintDirtR, uT.tintDirtG, uT.tintDirtB) * f3W.z
                          + vec3(uT.tintConcreteR, uT.tintConcreteG, uT.tintConcreteB) * f3W.w
                          + vec3(uT.tintSnowR, uT.tintSnowG, uT.tintSnowB) * f3Snow;
        float colLum    = dot(base, vec3(0.299, 0.587, 0.114));
        float tintBase  = mix(0.18, 0.50, smoothstep(0.1, 0.6, colLum));
        float tintStr   = mix(tintBase, 0.85, f3Snow) * uT.tintStrengthScale;
        baseColor = mix(base, materialTint, tintStr);
        // Cement: restore the authored colormap tone (concreteColorBlend = sqrt(pureConcrete)).
        baseColor = mix(baseColor, base, sqrt(clamp(F3pc, 0.0, 1.0)));
        // Cliff: desaturate + darken toward rock on steep MACRO slope (un-perturbed).
        float macroNzF3 = baseNormal.z;
        float cliffBlend = 1.0 - smoothstep(0.55, 0.85, abs(macroNzF3));
        if (cliffBlend > 0.01) {
            float luma = dot(baseColor, vec3(0.299, 0.587, 0.114));
            vec3  cliffColor = mix(vec3(luma), vec3(uT.tintRockR, uT.tintRockG, uT.tintRockB), 0.6) * 1.0;   // was 0.8: darkened the slope skirt into a halo around cliffs
            // RAVINE-WEATHER-1: horizontal strata on the wall faces (banded by world height, gently warped in plan).
            float sz = v_worldPos.z + 7.0 * sin(v_worldPos.x * 0.009 + 1.3) + 5.0 * sin(v_worldPos.y * 0.012 + v_worldPos.x * 0.004);
            cliffColor *= 1.0 + 0.18 * (0.55 * sin(sz * 0.17) + 0.3 * sin(sz * 0.43 + 1.7) + 0.15 * sin(sz * 1.07 + 0.4));
            baseColor = mix(baseColor, cliffColor, cliffBlend * 0.7 * (1.0 - F3pc));
        }
        // World-space two-octave break-up noise (non-snow, non-concrete).
        float noiseBlend = cookedBreakup;
        if (TS_colormapReady < 2) {
            float lowFreq = fbm(v_worldPos.xy * 0.0035, 3) * 0.5 + 0.5;
            float highFreq = fbm(v_worldPos.xy * 0.018, 2) * 0.5 + 0.5;
            noiseBlend = mix(lowFreq, highFreq, 0.55);
        }
        float breakup = mix(0.78, 1.18, noiseBlend);
        baseColor *= mix(1.0, breakup, (1.0 - f3Snow) * (1.0 - F3pc));
        baseColor *= mix(1.0, uT.snowBrightnessDampen, f3Snow);
    }
    // GROUND-MOTTLE-REF3 (owner ref 3): a world-space olive / yellow-green sward mottled with dry straw patches
    // (5-30 m) on natural ground, straw toward the coast, never on concrete/roads/runway, rock, cliffs, snow or
    // the beach band. The field is shared with the vegetation shaders (include/world_patch_noise.glsl).
    float gmFootprint = wpnFootprint(v_worldPos.xy);   // derivatives outside the branches below
    if (uT.groundMottleOn != 0) {
        float natural = materialW.y + materialW.z;   // grass + dirt: the natural-ground weight
        float slopeClean = smoothstep(0.80, 0.92, abs(baseNormal.z));   // steep ground keeps the cliff/rock look
        // uT.vegetationEnabled carries the vegetation switch (see vulkan_terrain_island_run.cpp): the mottle is vegetation, off with it.
        float wMottle = (uT.vegetationEnabled != 0 ? 1.0 : 0.0) * clamp(natural, 0.0, 1.0) * (1.0 - materialSnow) * (1.0 - F3pc) * slopeClean;
        float coastDry = 0.0;
        if (TS_shorelineOn != 0) {
            float aboveW = v_worldPos.z - uT.shorelineWaterElev;
            float nzm = clamp(abs(baseNormal.z), 0.0, 0.9995);
            float horiz = max(aboveW, 0.0) * (nzm / max(sqrt(max(1.0 - nzm * nzm, 1e-6)), 0.03));
            wMottle *= smoothstep(GM_BEACH_WU * 0.5, GM_BEACH_WU, horiz);   // the beach sand band stays clean
            coastDry = 1.0 - smoothstep(GM_BEACH_WU, GM_COAST_DRY_WU, horiz);
        }
        if (wMottle > 0.001) {
            vec2 gmField = worldPatchField(v_worldPos.xy, gmFootprint);
            vec3 gmCol = worldPatchColor(gmField, coastDry * 0.7);
            // Keep part of the authored colormap brightness variation (luma transfer), take the palette hue.
            float lb = dot(baseColor, vec3(0.299, 0.587, 0.114));
            float lt = dot(gmCol, vec3(0.299, 0.587, 0.114));
            // Dark soil (night maps, scorched or shaded colormap) keeps its own brightness: the mottle never lightens it.
            float keep = mix(1.0, GM_LUMA_KEEP, smoothstep(0.12, 0.26, lb));
            gmCol *= mix(1.0, clamp(lb / max(lt, 1e-3), 0.3, 1.5), keep);
            baseColor = mix(baseColor, gmCol, wMottle * GM_STRENGTH);
        }
    }

    // Uniform flow, close to use: avoid keeping the footprint live through cement/tint work.
    vec2 detailUv = (v_worldPos.xy + seabedOffset) * (uT.detailTiling / MATV_WU_PER_TILE);
    vec2 detailUvDx = dFdx(detailUv), detailUvDy = dFdy(detailUv);
    mv_detailTexelGM = sqrt(abs(detailUvDx.x * detailUvDy.y - detailUvDx.y * detailUvDy.x)) * float(textureSize(u_matNormalArray, 0).x);
    // Continuous world derivatives, before any divergent material branch or POM.
    vec3 cliffDx = dFdx(v_worldPos) / MATV_CLIFF_TS;
    vec3 cliffDy = dFdy(v_worldPos) / MATV_CLIFF_TS;
    float cliffSize = float(textureSize(u_matNormalArray, 0).x);
    vec3 cliffLod = max(vec3(0.0), 0.5 * log2(max(vec3(1e-12), cliffSize * cliffSize * vec3(
        max(dot(cliffDx.yz, cliffDx.yz), dot(cliffDy.yz, cliffDy.yz)),
        max(dot(cliffDx.xz, cliffDx.xz), dot(cliffDy.xz, cliffDy.xz)),
        max(dot(cliffDx.xy, cliffDx.xy), dot(cliffDy.xy, cliffDy.xy))))));
    vec3 lit = base;
    vec3 gbufN = vec3(0.0, 0.0, 1.0);   // GBuffer1 rgb: the lit surface normal (flat-up when lighting is off)
    if (TS_lightingEnabled != 0) {
        vec3 lightDir = uEnv.sunDirLive.xyz;
        // Colormaps are prelit: authored burnins, or sources baked by the loader.
        // Only detail relative to the macro normal may modulate that illumination.
        // Cement retains its authored slab normal and receives the same visibility.
        vec3 N = vec3(0.0, 0.0, 1.0);                            // flat-up terrain normal (default)
        bool resolveLampNormal = false;
        vec3 dbgBaseN = vec3(0.0, 0.0, 1.0); vec3 dbgDN = vec3(0.0);  // CSM-BLACKOUT-RECON-1 taps
        if (TS_detailEnabled != 0 && pureConcrete < 1.0) {
            vec3 baseN = baseNormal;
            vec4 detailW = mv_groundDetailWeights(materialW, baseN.z);
            vec3 dN    = vec3(0.0);
            if (TS_authoredMaterialsOnly && uT.reserved220 > 1.0 && TS_detailReady != 0 &&
                (kPlanarReflection == 0 || kReflectCutDetail == 0) && uT.detailStrength != 0.0)
                dN = mv_authoredDetailNormal(v_worldPos.xy, detailUvDx, detailUvDy);
            if (!TS_authoredMaterialsOnly && (kPlanarReflection == 0 || kReflectCutDetail == 0) && TS_detailReady != 0 && uT.detailStrength != 0.0) {
                if (mv_needDetail(baseN, lightDir, detailW, materialSnow, v_worldPos, normalLightFloor)) {
                    // Under-forest ground: the tree-canopy density mask (splatted from tree placements at mission load), same atlas UV as
                    // the macro colour. Needs the 7th array layer.
                    mv_forestMix = (TS_colormapReady != 0 && textureSize(u_matNormalArray, 0).z > MAT_LAYER_FOREST)
                        ? textureLod(u_forestMask, vec2((v_worldPos.x - uT.atlasTopLeftX) * uT.atlasOneOverWorldUnits,
                                                        (uT.atlasTopLeftY - v_worldPos.y) * uT.atlasOneOverWorldUnits), 0.0).r : 0.0;
                    dN = mv_detailNormal(detailW, materialSnow, v_worldPos.xy + seabedOffset, baseN.z, detailUvDx, detailUvDy);
                } else {
                    resolveLampNormal = true;
                }
            }
            vec3 macroPert = vec3(baseN.xy / max(baseN.z, 0.2) * uT.nfhStrength, 1.0);
            vec3 pert = macroPert + vec3(dN.xy * uT.detailStrength, 0.0);
            N = normalize(pert);
            if (!TS_authoredMaterialsOnly) {
                float blocked = crTerrainCliff(v_worldPos.xy);
                if ((TS_concreteDetail & 2) != 0) crApply(baseN, cliffDx * MATV_CLIFF_TS, cliffDy * MATV_CLIFF_TS, cliffLod, blocked, N, baseColor);
                else if (TS_detailReady >= 2) mv_cliffMaterial(baseN, cliffLod, blocked, N, baseColor);
                if (TS_detailReady != 0) crNavigable(baseN, cliffDx * MATV_CLIFF_TS, cliffDy * MATV_CLIFF_TS,
                                                   (1.0 - blocked) * max(detailW.x, cliffRockWeight(abs(baseN.z))), N, baseColor);
            }
            if (kPlanarReflection != 0 && kReflectCutDetail != 0) N = normalize(macroPert);
            dbgBaseN = baseN; dbgDN = dN;                        // CSM-BLACKOUT-RECON-1 taps
            N = normalize(mix(N, vec3(0.0, 0.0, 1.0), pureConcrete));
        }
        // Evaluate the ratio after material sampling so its inputs do not stay
        // live across the detail/cliff texture walks. dbgBaseN already retains
        // the macro normal; flat receivers keep its default (0,0,1).
        vec3 macroN = normalize(vec3(dbgBaseN.xy / max(dbgBaseN.z, 0.2) * uT.nfhStrength, 1.0));
        float macroNdotL = max(dot(macroN, lightDir), normalLightFloor);
        // Equal normals give exactly one, including when detail is disabled.
        // Bound the grazing/back-facing ratio rather than re-lighting macro relief.
        float normalLight = clamp(max(dot(N, lightDir), normalLightFloor) / macroNdotL, 0.5, 1.5);
        // The cliff burnin already carries ambient. Only its sun share receives
        // another directional modulation; a back-facing face has no sun to occlude.
        float cliffShare = (1.0 - pureConcrete) * (1.0 - smoothstep(0.55, 0.85, abs(baseNormal.z)));
        float cliffSun = cliffDirectShare(dot(macroN, lightDir), normalLightFloor);
        normalLight = mix(normalLight, mix(1.0, normalLight, cliffSun), cliffShare);

        // CONCRETE-ROAD-PBR-DETAIL: detail only (AO, relative sun shading, sheen), MC2 colour stays. Solid slabs only: those
        // fragments skip the material detail-normal fetches, so the two concrete fetches replace them (no net fetch increase).
        vec3 cdSpec = vec3(0.0);
        vec2 cdWpDx = dFdx(v_worldPos.xy), cdWpDy = dFdy(v_worldPos.xy);   // uniform flow, late: nothing extra stays live through the detail-normal block
        if (!TS_authoredMaterialsOnly && (kPlanarReflection == 0 || kReflectCutDetail == 0) && TS_concreteDetail != 0 && pureConcrete > 0.0) cdApply(cdRoad > 0.5 ? CD_LAYER_ROAD : CD_LAYER_PAD, v_worldPos.xy, cdWpDx, cdWpDy, lightDir, pureConcrete, baseColor, normalLight, cdSpec, N);
        gbufN = N;

        // Static ground mask and current-frame movers share the same visibility
        // floor. Cement receives both, exactly like the surrounding terrain.
        vec3 shadowN=baseNormal;
        float staticS=TS_enableShadows!=0?terrainStaticShadow(v_worldPos):1.0;
        float dynS=kPlanarReflection != 0 && kReflectCutMovers != 0 ? 1.0 : terrainDynamicShadow(v_worldPos,shadowN);
        float shadow = min(mix(staticS, mix(1.0, staticS, cliffSun), cliffShare), dynS);
        // CSM-BLACKOUT-RECON-1: one-shot debug viz (env MC2_SHADOW_DEBUG_MODE=N).
        // Resolve before local lighting so the debug inputs do not stay live through its loop. Bypasses the normal composite so each channel can be inspected in
        // isolation on a live capture. No effect when kCsmDebugMode==0.
        if (kCsmDebugMode == 1)      { fragColor = vec4(vec3(normalLight), 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }
        else if (kCsmDebugMode == 2) { fragColor = vec4(lightDir * 0.5 + 0.5, 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }
        else if (kCsmDebugMode == 3) { fragColor = vec4(vec3(staticS), 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }
        else if (kCsmDebugMode == 4) { fragColor = vec4(vec3(dynS), 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }
        else if (kCsmDebugMode == 5) { fragColor = vec4(vec3(shadow), 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }
        else if (kCsmDebugMode == 6) { fragColor = vec4(baseColor, 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }   // VK-PARITY-F3: albedo (GL MC2_LIGHTING_DEBUG_VIEW=40)
        else if (kCsmDebugMode == 7) { fragColor = vec4(vec3(clamp(dot(N, lightDir), 0.02, 1.0)), 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }   // sun N.L diffuse (GL MC2_LIGHTING_DEBUG_VIEW=sun)
        else if (kCsmDebugMode == 8) { fragColor = vec4(N * 0.5 + 0.5, 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }   // surface normal (GL ...=normal)
        else if (kCsmDebugMode == 9) { fragColor = vec4(dbgBaseN * 0.5 + 0.5, 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }
        else if (kCsmDebugMode == 10) { fragColor = vec4(dbgDN * 0.5 + 0.5, 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }
        else if (kCsmDebugMode == 13) { vec2 rel = N.xy / max(N.z, 0.2) - dbgBaseN.xy / max(dbgBaseN.z, 0.2); fragColor = vec4(vec3(clamp(length(rel) * 4.0, 0.0, 1.0)), 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }   // total detail relief (all classes + rock) over the macro slope, x4
        else if (kCsmDebugMode == 12) { fragColor = vec4(prelitBase, 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; } // raw prelit before material/lighting
        // Material weights false colour: rock grey, grass green (forest-floor share yellow), dirt red, concrete blue, snow white.
        else if (kCsmDebugMode == 11) { fragColor = vec4(materialW.x * (1.0 - mv_forestMix * smoothstep(0.75, 0.95, dbgBaseN.z)) * vec3(0.35) + (materialW.y + materialW.z + materialW.x * smoothstep(0.75, 0.95, dbgBaseN.z)) * mv_forestMix * vec3(1.0, 0.9, 0.0) + (1.0 - mv_forestMix) * (materialW.y * vec3(0.1, 0.9, 0.1) + materialW.z * vec3(0.9, 0.2, 0.1)) +
                                                         materialW.w * vec3(0.1, 0.3, 1.0) + vec3(materialSnow), 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }

        float normalSize = float(textureSize(u_matNormalArray, 0).x);
        float lampLod = resolveLampNormal ? 0.5 * log2(max(max(dot(detailUvDx, detailUvDx), dot(detailUvDy, detailUvDy))
                                                       * normalSize * normalSize, 1e-12)) : 0.0;
        // GROUND-AO-1: occlusion of the ambient/sky share only. The prelit colour carries ambient and sun together, so the sun's
        // share (the cliff split, zero in shadow) is exempt. Shape = the per-mission baked horizon AO (macro-gradient texture, .b);
        // material = the cooked source AO where the layer carries it (grass turf, normal-texel B), else a cavity proxy from the detail-normal tilt (the stock sets have no AO map).
        if (uT.groundAoShape > 0.0 || uT.groundAoMaterial > 0.0) {
            vec2 aoCell = vec2(v_worldPos.x + uT.halfMap, uT.halfMap - v_worldPos.y) / 128.0;
            float aoShape = textureLod(u_macroGradient, (aoCell + 1.5) / float(uT.mapSide + 2), 0.0).b;
            float aoMat = 1.0 - 0.45 * clamp(length(dbgDN.xy * uT.detailStrength) * 1.2, 0.0, 1.0);
            aoMat = mix(aoMat, mix(aoMat, mv_srcAoSum / max(mv_srcAoW, 1e-4), clamp(mv_srcAoW, 0.0, 1.0)), uT.groundAoSource);
            float aoAll = clamp(mix(1.0, aoShape, uT.groundAoShape), 0.0, 1.0) * clamp(mix(1.0, aoMat, uT.groundAoMaterial), 0.0, 1.0);
            g_groundAoMul = mix(1.0, aoAll, 1.0 - cliffSun * shadow);
        }
        lit = shadeGround(baseColor, N, normalLight, shadow, pureConcrete, v_worldPos, macroN, resolveLampNormal, lampLod);
        lit = max(lit + cdSpec * shadow, vec3(0.0));

        // TERRAIN-EDGE-FEATHER-1 (GL terrain_lod_chunk.frag, after the mover contact): the last tile band of the map fades to the sky haze
        // colour so the straight map perimeter does not cut a hard edge. halfMap 0 means the map size is not known yet.
        if (uT.edgeFeatherStrength > 0.0 && uT.halfMap > 0.0) {
            float haze = edgeHazeAmount(v_worldPos.xy, uT.halfMap) * uT.edgeFeatherStrength;
            lit = mix(lit, uEnv.sceneLight.x < 1.0 ? uEnv.ambLive.rgb : EDGE_HAZE_SKY, clamp(haze, 0.0, 1.0));
        }

        // TERRAIN-SHORELINE-V3/V4 (terrain_lod_chunk.frag, after the edge feather): wet and foam bands measured as the
        // HORIZONTAL run from the waterline through the macro slope, with static fbm jitter on the run, a fade-in over the
        // first 1.5 wu above the water, the optional mask modulator, a 12-20 deg slope guard and a beach affinity term.
        if (TS_shorelineOn != 0) {
            float aboveWater = v_worldPos.z - uT.shorelineWaterElev;
            vec3  macroN     = baseNormal;
            float nzc        = clamp(abs(macroN.z), 0.0, 0.9995);
            float sinSlope   = max(sqrt(max(1.0 - nzc * nzc, 1e-6)), 0.03);
            float horizDist  = max(aboveWater, 0.0) * (nzc / sinSlope);
            // Coastal look (owner refs 1/4/6): a pale sand beach above the swash that continues under the shallows, where
            // the Beer-Lambert below turns it jade; gentle slopes only, so cliffs and quays keep their own colour.
            // Close up (ref 6) a dark wet strip follows the swash, and a lacy white swash sheet runs up the sand.
            // Night (scene light below 1, day exactly 1): a gentle cool-grey sand strip, no wet strip, a dim swash.
            float beachFw = length(fwidth(v_worldPos.xy)), beachNight = clamp((1.0 - uEnv.sceneLight.x) * 2.5, 0.0, 1.0);
            if (uT.shorelineOn == 2 && horizDist < 110.0 && aboveWater > -40.0) {
                float lumR = clamp(dot(lit, vec3(0.299, 0.587, 0.114)) / max(dot(baseColor, vec3(0.299, 0.587, 0.114)), 0.04), 0.0, 2.5);
                float mask = smoothstep(0.93, 0.975, nzc) * (cementHit ? 0.0 : 1.0) * (1.0 - F3pc) * (1.0 - materialSnow);
                float beach = (1.0 - smoothstep(35.0, 80.0, horizDist + fbm(v_worldPos.xy * 0.011, 2) * 30.0))
                            * (1.0 - smoothstep(4.0, 40.0, -aboveWater)) * mask;
                lit = mix(lit, mix(vec3(0.97, 0.92, 0.79), vec3(0.74, 0.77, 0.80), beachNight) * lumR, beach * mix(0.8, 0.4, beachNight));
                mask *= 1.0 - beachNight;
                float above = smoothstep(0.0, 0.3, aboveWater) * mask, near = 1.0 - smoothstep(1.5, 5.0, beachFw);
                float run = mix(8.0, 24.0, near) + 7.0 * sin(uT.shorelineTime * 1.05 + dot(v_worldPos.xy, vec2(0.003, 0.002)) - 0.9);
                float lace = fbm(v_worldPos.xy * 0.15 + uT.shorelineTime * vec2(0.12, 0.05), 2);
                lit *= 1.0 - mix(0.15, 0.4, near) * above * (1.0 - smoothstep(run, run + 10.0, horizDist + fbm(v_worldPos.xy * 0.04, 2) * 5.0));
                float swash = (1.0 - smoothstep(run * 0.75, run * 0.75 + 2.0, horizDist + lace * 8.0)) * smoothstep(-0.3, 0.1, lace)
                            * above * near;
                lit = mix(lit, vec3(0.90, 0.93, 0.92) * lumR, swash * 0.85);
            }
            // Both bands are zero outside (0, reach): aboveFade is 0 at the waterline and the smoothsteps are 0 past the runs, and
            // |fbm(.., 2)| <= 0.75, so the jittered run cannot re-enter. Skipping there is exact and saves the noise on most pixels.
            float shoreReach = max(uT.shorelineWetRun, uT.shorelineFoamRun) + abs(uT.shorelineEdgeJitter);
            if (horizDist > 0.0 && horizDist < shoreReach) {
                float horizDistJ = max(horizDist + fbm(v_worldPos.xy * 0.055, 2) * uT.shorelineEdgeJitter, 0.0);
                float slWet  = 1.0 - smoothstep(0.0, max(uT.shorelineWetRun, 1e-4), horizDistJ);
                float slFoam = 1.0 - smoothstep(0.0, max(uT.shorelineFoamRun, 1e-4), horizDistJ);
                float aboveFade = smoothstep(0.0, 1.5, horizDist);
                slWet  *= aboveFade;
                slFoam *= aboveFade;
                bool reviewShore = uT.shorelineOn > 1;
                if (reviewShore) {
                    float phase = uT.shorelineTime * 1.05 + dot(v_worldPos.xy, vec2(0.003, 0.002));
                    // The wet tail lags the swash so dark sand remains exposed on retreat.
                    float reach = 46.0 + 18.0 * sin(phase - 0.9);
                    slWet = (1.0 - smoothstep(reach, reach + 32.0, horizDistJ))
                          * smoothstep(0.0, 2.0, horizDist);
                    slFoam = 0.0;
                }
                if (TS_shorelineHasMask != 0) {
                    vec2 slUV = vec2((v_worldPos.x - uT.shorelineBoundsX) / max(uT.shorelineBoundsW, 1e-5),
                                     (uT.shorelineBoundsY - v_worldPos.y) / max(uT.shorelineBoundsH, 1e-5));
                    if (slUV.x >= 0.0 && slUV.x <= 1.0 && slUV.y >= 0.0 && slUV.y <= 1.0) {
                        vec4 sl = texture(u_shorelineMask, slUV);
                        slWet  *= mix(1.0, clamp(sl.g, 0.0, 1.0), sl.a);
                        slFoam *= mix(1.0, clamp(sl.b, 0.0, 1.0), sl.a);
                    }
                }
                float slopeAtten = clamp((N.z - 0.940) / (0.978 - 0.940), 0.0, 1.0);   // full at 12 deg, zero at 20 deg
                vec4 slW = materialW; float slSnow = materialSnow;
                slW = mix(slW, vec4(0.0, 0.0, 0.0, 1.0), F3pc);
                slSnow *= (1.0 - F3pc);
                float litLuma       = dot(lit, vec3(0.299, 0.587, 0.114));
                float shoreAffinity = smoothstep(0.08, 0.26, litLuma) * clamp(1.0 - slW.x - slSnow, 0.0, 1.0);
                if (slWet > 0.001 && !cementHit && slopeAtten > 0.0 && shoreAffinity > 0.001) {
                    float wetAmt = clamp(slWet, 0.0, 1.0) * (1.0 - F3pc) * slopeAtten * shoreAffinity * uT.shorelineStrength;
                    vec3 wetSand = reviewShore ? lit * vec3(0.62, 0.68, 0.70)
                                              : vec3(1.16, 1.06, 0.78) * (litLuma * 0.90);
                    lit = mix(lit, wetSand, clamp(wetAmt, 0.0, 1.0) * 0.85);
                    if (reviewShore) {
                        vec3 eye = normalize(mc2ToRender(uT.cameraPosMC2.xyz) - v_worldPos);
                        float sheen = pow(1.0 - clamp(abs(dot(eye, macroN)), 0.0, 1.0), 5.0);
                        lit += prelitBase * wetAmt * sheen * 0.28;
                    }
                }
                if (slFoam > 0.001 && slopeAtten > 0.0 && shoreAffinity > 0.001) {
                    float foamScroll = fbm(v_worldPos.xy * 0.09 + uT.shorelineTime * 0.15, 3) * 0.5 + 0.5;
                    float foamPulse  = fbm(v_worldPos.xy * 0.20 - uT.shorelineTime * 0.22 + 19.0, 2) * 0.5 + 0.5;
                    float foamNoise  = clamp(mix(foamScroll, foamPulse, 0.5), 0.0, 1.0);
                    float slF        = clamp(slFoam, 0.0, 1.0);
                    float coverage   = smoothstep(1.0 - slF * 0.85, 1.0, foamNoise + (1.0 - slF) * 0.15);
                    float foamAmt    = coverage * slopeAtten * uT.shorelineFoamStrength * shoreAffinity;
                    vec3 foamLight = uEnv.sceneLight.x < 1.0 ? vec3(uT.missionFlatR, uT.missionFlatG, uT.missionFlatB) : vec3(1.0);
                    lit = mix(lit, vec3(0.92) * foamLight, clamp(foamAmt, 0.0, 1.0) * (1.0 - F3pc));
                }
            }
        }
    }

    // Coastal look (FoamFirst): seabed caustics and reef tint. Water absorption
    // lives in water_geom.frag so it also covers submerged objects.
    float causticFw = length(fwidth(v_worldPos.xy)) * 0.21;   // uniform control flow: caustics fade below ~2 px
    if (uT.shorelineOn == 2 && kPlanarReflection == 0 && v_worldPos.z < uT.shorelineWaterElev) {
        float wd = uT.shorelineWaterElev - v_worldPos.z;
        vec2 cp = v_worldPos.xy * 0.03;
        float caustic = pow(max(0.0, sin(cp.x * 7.0 + sin(cp.y * 4.0 + uT.shorelineTime) * 1.3)
                                   * sin(cp.y * 6.0 + cos(cp.x * 4.0 - uT.shorelineTime) * 1.2)), 9.0)
                      * (1.0 - smoothstep(0.6, 2.0, causticFw));
        float reef = smoothstep(0.1, 0.4, fbm(v_worldPos.xy / 380.0 + 7.3, 3)) * smoothstep(3.0, 9.0, wd) * (1.0 - smoothstep(45.0, 90.0, wd));
        // Night: little light reaches the seabed and back; no caustics, a dim, mostly desaturated cool teal-green.
        float seaNight = clamp((1.0 - uEnv.sceneLight.x) * 2.5, 0.0, 1.0);
        lit *= mix(vec3(1.0), vec3(0.42, 0.52, 0.46), reef * 0.75)   // darker reef/weed patches inside the shallows (owner refs)
             * (1.0 + 1.2 * (1.0 - seaNight) * caustic * exp(-0.05 * wd));
        lit = mix(lit, dot(lit, vec3(0.299, 0.587, 0.114)) * vec3(0.80, 0.98, 1.0) * 0.6, 0.75 * seaNight);
    }

    // Shade the receiver before compositing the authored road shoulder. Keeping
    // uniform flow also preserves derivatives across partially covered edges.
    lit = mix(lit, roadColor, roadCoverage);
    gbufN = normalize(mix(gbufN, vec3(0.0, 0.0, 1.0), roadCoverage));
    fragColor = vec4(lit, 1.0);   // opaque in-place (a=1, NOT premult)

    // ★TERRAIN GBuffer1: the real shading normal in rgb (guide buffer for temporal/denoise consumers) and
    // a=1.0 (handles own shadow, the class every consumer keys on; screen_shadow skips a>0.5 before reading rgb).
    GBuffer1 = vec4(gbufN * 0.5 + 0.5, 1.0);
}
