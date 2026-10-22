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
//                        (terrain_lod_chunk.frag:462-482). The shared surface cook
//                        supplies the filtered macro color and coast correction.
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
// VK-TERRAIN-LIGHTING sub-slice: make the VK terrain LIT (directional relief + baked
// static sun shadow) so it matches the GL LOD-chunk terrain instead of flat. Uses the
// REUSABLE lighting/shadow helper (props/mechs reuse it later). Gate MC2_VK_TERRAIN_LIGHTING
// (requires MC2_VK_TERRAIN_TIER1): OFF -> uT.lightingEnabled==0 -> the compose is the
// identity (base only), byte-identical to the pre-lighting colormap+cement frame.
#extension GL_GOOGLE_include_directive : require
#include "include/lighting_shadow_vk.glsl"
#ifndef PREC
#define PREC highp
#endif
#include "../include/noise.hglsl"   // VK-PARITY-F3: fbm() for the GL colour break-up (terrain_lod_chunk.frag:1839)
#include "include/terrain_frame_vk.glsl"   // TerrainFrameUBO + set-0 resources + CSM shadow helpers
#include "include/terrain_static_visibility.glsl"
#include "include/terrain_shadow_receiver.glsl"
#include "include/concrete_detail_vk.glsl"
#include "../include/axis.hglsl"   // mc2ToRender: the POM view vector is built in the MC2 world frame
#include "../include/edge_haze.hglsl"   // edgeHazeAmount + EDGE_HAZE_SKY, shared with GL

layout(location = 0) in vec3 v_worldPos;

layout(location = 0) out vec4 fragColor;   // MRT0 color
layout(location = 1) out vec4 GBuffer1;    // MRT1 normal + shadow class

layout(set = 0, binding = 16) uniform sampler2D u_macroGradient;
// VK-TERRAIN-DETAIL: the material tangent-space detail-normal array (VK copy-once
// import of GL terrain_normal_array_tex_, a 9-layer RGBA8 GL_TEXTURE_2D_ARRAY,
// GL_REPEAT). Layers per include/terrain_mat_layers.hglsl. Dummy 1x1 until imported
// (uT.detailReady gates the sample).
layout(set = 0, binding = 6) uniform sampler2DArray u_matNormalArray;
// TERRAIN-SHORELINE-V3: optional mask sidecar (G wet, B foam, A valid); read only while uT.shorelineHasMask != 0.
layout(set = 0, binding = 14) uniform sampler2D u_shorelineMask;

// Material layer indices (mirror shaders/include/terrain_mat_layers.hglsl).
#define MAT_LAYER_ROCK     0
#define MAT_LAYER_GRASS    1
#define MAT_LAYER_DIRT     2
#define MAT_LAYER_CONCRETE 3
#define MAT_LAYER_SNOW     4
#define MAT_LAYER_MARBLE_CLIFF 5   // mat5_normal.tga: RGB = marble microsurface normal, A = the cooked cliff displacement

// GL default tuning (terrain_lod_chunk.frag uniforms; ImGui-driven, values are the
// gameos_graphics.cpp member defaults). Baked const for the parity oracle.
const vec4  MATV_NORMAL_BOOST = vec4(0.9, 0.5, 1.1, 2.5);  // rock,grass,dirt,conc
const vec4  MATV_TILING       = vec4(3.0, 2.0, 1.0, 6.0);  // rock,grass,dirt,conc
const float MATV_TILING_SNOW  = 1.0;
const vec4  MATV_CLASS_GRASS  = vec4(-0.02, 0.06, 0.22, 0.40); // gMinusR lo/hi, gBright lo/hi
const vec4  MATV_CLASS_DIRT   = vec4(-0.02, 0.06, 0.22, 0.45); // rMinusG lo/hi, rBright lo/hi
const float MATV_WU_PER_TILE  = 768.0;  // MAT_WORLD_UNITS_PER_TILE (terrain_lod_chunk.frag:240)

// Central difference commutes with bilinear interpolation; normalize after filtering.
// Call at the point of use: hoisting the result to the top of main() kept it live across the whole shader and cost ~15% at 4K.
vec3 mv_smoothTerrainNormal(vec2 worldXY) {
    vec2 cell = vec2(worldXY.x + uT.halfMap, uT.halfMap - worldXY.y) / 128.0;
    vec2 gradient = textureLod(u_macroGradient, (cell + 1.5) / float(uT.mapSide + 2), 0.0).rg;
    return normalize(vec3(gradient, 1.0));
}

// --- GL colormap material classifier (terrain_lod_chunk.frag chunkColorWeights +
// chunkWeights, :244-338). g_terrainMaterialProfile is default 0 (legacy) here. -----
vec3 mv_rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + 1.0e-10)), d / (q.x + 1.0e-10), q.x);
}
vec4 mv_colorWeights(vec3 color) {
    vec4 w = vec4(0.0);
    float gMinusR = color.g - color.r;
    w.y = smoothstep(MATV_CLASS_GRASS.x, MATV_CLASS_GRASS.y, gMinusR)
        * smoothstep(MATV_CLASS_GRASS.z, MATV_CLASS_GRASS.w, color.g);
    float rMinusG = color.r - color.g;
    w.z = smoothstep(MATV_CLASS_DIRT.x, MATV_CLASS_DIRT.y, rMinusG)
        * smoothstep(MATV_CLASS_DIRT.z, MATV_CLASS_DIRT.w, color.r);
    w.x = 1.0 - max(w.y, w.z);   // everything else -> rock
    float isWater = smoothstep(0.0, 0.08, min(color.g, color.b) - color.r);
    w.x += isWater; w.y *= (1.0 - isWater); w.z *= (1.0 - isWater);
    float total = w.x + w.y + w.z + w.w;
    return (total < 0.01) ? vec4(1.0, 0.0, 0.0, 0.0) : w / total;
}
void mv_weights(vec3 colAvg, out vec4 w, out float snowWeight) {
    w = mv_colorWeights(colAvg);
    vec3 hsv = mv_rgb2hsv(colAvg);
    float snowRaw = smoothstep(0.15, 0.03, hsv.y) * smoothstep(0.42, 0.62, hsv.z);
    snowWeight = smoothstep(0.25, 0.55, snowRaw);
    w *= (1.0 - snowWeight);
    float tot = w.x + w.y + w.z + w.w;
    if (tot > 0.01) w /= tot; else w = vec4(1.0, 0.0, 0.0, 0.0);
}

// Gradients come from uniform main() control flow, before cement/material
// selection. Offset anti-tile lookups use the same continuous UV footprint.
vec2 mv_hash22(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}
vec4 mv_sampleAntiTileArr(int layer, vec2 uv, float scale, vec2 dx, vec2 dy) {
    vec2 off1 = mv_hash22(floor(uv / scale)) * scale;
    vec2 off2 = mv_hash22(floor(uv / scale) + vec2(7.0, 13.0)) * scale;
    vec4 s0 = textureGrad(u_matNormalArray, vec3(uv,        float(layer)), dx, dy);
    vec4 s1 = textureGrad(u_matNormalArray, vec3(uv + off1, float(layer)), dx, dy);
    vec4 s2 = textureGrad(u_matNormalArray, vec3(uv + off2, float(layer)), dx, dy);
    vec2 f = fract(uv / scale);
    float w1 = smoothstep(0.2, 0.5, f.x) * smoothstep(0.2, 0.5, f.y);
    float w2 = smoothstep(0.2, 0.5, 1.0 - f.x) * smoothstep(0.2, 0.5, 1.0 - f.y);
    return (s0 + s1 * w1 + s2 * w2) / (1.0 + w1 + w2);
}

// Terrain parallax-occlusion march with the REAL per-fragment tangent-space view vector (GL terrain_lod_chunk.frag
// chunkParallaxView): more layers at grazing view, an epsilon guard on the denominator of the final blend (AMD yields inf/nan
// silently), a hard 16-step cap. Displacement = matNormalArray alpha channel (VK's copy-once RGBA8 import), read with
// textureLod base level so it is valid in any control flow. No self-shadow term.
const float MATV_POM_SCALE      = 0.02;     // GL u_pomScale
const float MATV_POM_MIN_LAYERS = 8.0;
const float MATV_POM_MAX_LAYERS = 16.0;
const float MATV_POM_FADE_NEAR  = 1500.0;   // world units; one tile is 384
const float MATV_POM_FADE_FAR   = 3500.0;
float mv_pomDisplacement(vec2 uv, vec4 w) {
    float d = 0.0;
    if (w.x > 0.01) d += w.x * textureLod(u_matNormalArray, vec3(uv, float(MAT_LAYER_ROCK)),     0.0).a;
    if (w.y > 0.01) d += w.y * textureLod(u_matNormalArray, vec3(uv, float(MAT_LAYER_GRASS)),    0.0).a;
    if (w.w > 0.01) d += w.w * textureLod(u_matNormalArray, vec3(uv, float(MAT_LAYER_CONCRETE)), 0.0).a;
    return 1.0 - d;
}
vec2 mv_parallax(vec2 uv, float scale, vec4 w, vec3 viewDirTS) {
    float numLayers = mix(MATV_POM_MAX_LAYERS, MATV_POM_MIN_LAYERS, max(viewDirTS.y, 0.0));
    numLayers = clamp(numLayers, 4.0, 16.0);
    float layerDepth = 1.0 / numLayers;
    float curLayer = 0.0;
    vec2 P = viewDirTS.xz / max(viewDirTS.y, 0.001) * scale;
    vec2 dUV = P / numLayers;
    vec2 curUV = uv;
    float curD = mv_pomDisplacement(curUV, w);
    for (int i = 0; i < 16; ++i) {
        if (curLayer >= curD) break;
        curUV -= dUV;
        curD = mv_pomDisplacement(curUV, w);
        curLayer += layerDepth;
    }
    vec2 prevUV = curUV + dUV;
    float after  = curD - curLayer;
    float before = mv_pomDisplacement(prevUV, w) - curLayer + layerDepth;
    return mix(curUV, prevUV, after / max(abs(after - before), 1.0e-6));
}

// Cliff faces: triplanar parallax-occlusion march of ONE world plane (GL terrain_lod_chunk.frag cliffParallaxPlane). viewPlane is the view
// vector projected on the plane's 2D basis; depth is a world-unit height relative to the cliff tiling. The layer-5 alpha is the cliff
// displacement (1 = peak). textureLod only: the march sits in a non-uniform branch.
const float MATV_CLIFF_TS          = 256.0;   // world units per cliff repeat
const float MATV_CLIFF_POM_DEPTH   = 12.0;
const float MATV_CLIFF_POM_STEPS   = 24.0;
const float MATV_CLIFF_HEIGHT_NORMAL_STRENGTH = 2.0;
float mv_cliffHeight(vec2 uv) {
    return textureLod(u_matNormalArray, vec3(uv, float(MAT_LAYER_MARBLE_CLIFF)), 0.0).a;
}
vec2 mv_cliffParallaxPlane(vec2 uv, vec2 viewPlane, float depth, float steps) {
    if (depth <= 0.0) return uv;
    float nSteps = clamp(steps, 8.0, 32.0);
    float layerDepth = 1.0 / nSteps;
    vec2 P = -viewPlane * depth;   // negative: the parallax shifts toward the eye
    vec2 deltaUV = P * layerDepth;
    float curDepth = 0.0;
    vec2  curUV    = uv;
    float curH = 1.0 - mv_cliffHeight(curUV);
    for (int i = 0; i < 32; ++i) {
        if (float(i) >= nSteps || curDepth >= curH) break;
        curUV   += deltaUV;
        curDepth += layerDepth;
        curH = 1.0 - mv_cliffHeight(curUV);
    }
    vec2  prevUV = curUV - deltaUV;
    float afterH = curH - curDepth;
    float beforeH = (1.0 - mv_cliffHeight(prevUV)) - (curDepth - layerDepth);
    float t = afterH / (afterH - beforeH + 1e-5);
    return mix(curUV, prevUV, clamp(t, 0.0, 1.0));
}

// Steep cells: the rock normal is projected from the WORLD axes (triplanar) instead of the top-down per-tile UV, which removes the vertical
// stretch on a cliff face. The shading normal is mixed strongly toward the height gradient of the cliff displacement (its RGB normal is too
// smooth to catch light), and the albedo is darkened by the displacement (groove relief). `N` is the detail-perturbed normal on entry and
// the cliff-blended one on return; `baseColor` takes the groove darkening. macroN is the un-perturbed macro slope normal. The cliff albedo
// re-projection of GL is NOT here: the island carries no material albedo array.
void mv_cliffMaterial(vec3 macroN, inout vec3 N, inout vec3 baseColor) {
    float cb = smoothstep(0.85, 0.55, abs(macroN.z));
    if (cb <= 0.01) return;
    const float ts = MATV_CLIFF_TS;
    vec3 wp = v_worldPos;
    vec3 wn = abs(macroN); wn /= (wn.x + wn.y + wn.z + 1e-5);
    vec2 uvX = wp.yz / ts;   // YZ plane
    vec2 uvY = wp.xz / ts;   // XZ plane
    vec2 uvZ = wp.xy / ts;   // XY plane

    vec3  camW = mc2ToRender(uT.cameraPosMC2.xyz);
    vec3  Vw   = normalize(camW - wp);   // fragment -> eye, MC2 world
    float depth = MATV_CLIFF_POM_DEPTH / ts;
    float steps = MATV_CLIFF_POM_STEPS;
    vec2 vpX = Vw.yz / max(abs(Vw.x), 0.30);
    vec2 vpY = Vw.xz / max(abs(Vw.y), 0.30);
    vec2 vpZ = Vw.xy / max(abs(Vw.z), 0.30);
    uvX = mix(uvX, mv_cliffParallaxPlane(uvX, vpX, depth, steps), clamp(wn.x * 1.5, 0.0, 1.0));
    uvY = mix(uvY, mv_cliffParallaxPlane(uvY, vpY, depth, steps), clamp(wn.y * 1.5, 0.0, 1.0));
    uvZ = mix(uvZ, mv_cliffParallaxPlane(uvZ, vpZ, depth, steps), clamp(wn.z * 1.5, 0.0, 1.0));

    vec3 nX = textureLod(u_matNormalArray, vec3(uvX, float(MAT_LAYER_MARBLE_CLIFF)), 0.0).rgb * 2.0 - 1.0;
    vec3 nY = textureLod(u_matNormalArray, vec3(uvY, float(MAT_LAYER_MARBLE_CLIFF)), 0.0).rgb * 2.0 - 1.0;
    vec3 nZ = textureLod(u_matNormalArray, vec3(uvZ, float(MAT_LAYER_MARBLE_CLIFF)), 0.0).rgb * 2.0 - 1.0;
    vec3 triTilt = vec3(0.0, nX.x, nX.y) * wn.x
                 + vec3(nY.x, 0.0, nY.y) * wn.y
                 + vec3(nZ.x, nZ.y, 0.0) * wn.z;

    // Height-derived shading normal: forward differences of the displacement per plane (one texel of the 2048 map as the step).
    const float hEps = 1.0 / 2048.0;
    float hX0 = mv_cliffHeight(uvX), hXu = mv_cliffHeight(uvX + vec2(hEps, 0.0)), hXv = mv_cliffHeight(uvX + vec2(0.0, hEps));
    float hY0 = mv_cliffHeight(uvY), hYu = mv_cliffHeight(uvY + vec2(hEps, 0.0)), hYv = mv_cliffHeight(uvY + vec2(0.0, hEps));
    float hZ0 = mv_cliffHeight(uvZ), hZu = mv_cliffHeight(uvZ + vec2(hEps, 0.0)), hZv = mv_cliffHeight(uvZ + vec2(0.0, hEps));
    float g = MATV_CLIFF_HEIGHT_NORMAL_STRENGTH / hEps;
    vec3 hnX = normalize(vec3(-(hXu - hX0) * g, -(hXv - hX0) * g, 1.0));
    vec3 hnY = normalize(vec3(-(hYu - hY0) * g, -(hYv - hY0) * g, 1.0));
    vec3 hnZ = normalize(vec3(-(hZu - hZ0) * g, -(hZv - hZ0) * g, 1.0));
    vec3 htTilt = vec3(0.0, hnX.x, hnX.y) * wn.x
                + vec3(hnY.x, 0.0, hnY.y) * wn.y
                + vec3(hnZ.x, hnZ.y, 0.0) * wn.z;
    float hMix = clamp(MATV_CLIFF_HEIGHT_NORMAL_STRENGTH * 0.5, 0.0, 1.0);
    triTilt = mix(triTilt, htTilt, hMix);

    vec3 triN = normalize(macroN + triTilt);
    N = normalize(mix(N, triN, cb));

    float disp = hX0 * wn.x + hY0 * wn.y + hZ0 * wn.z;
    baseColor *= mix(1.0, 0.62 + 0.38 * disp, cb);
}

// --- GL detail-normal accumulation (terrain_lod_chunk.frag chunkDetailNormal,
// :341-419). NOW PORTED (this sub-slice): per-layer weighted tangent-space normals
// (rock /3 boost, grass, dirt, ★concrete, snow) + ★per-layer fwidth sub-pixel fades
// (fwRock..fwSnow, GL :353-357) + ★anti-tile resample gated by `fw* > 0.5 && ats* >
// 0.01` EXACTLY as GL :392-415. Concrete layer (MAT_LAYER_CONCRETE, matTiling.w) was
// previously omitted — now included to match GL. POM is the real-view march
// (see mv_parallax below). DEFERRED vs GL: LOD-tier fade, painted-concrete normal, cliff
// albedo re-projection (VK-PARITY-GAPS row 14).
// VK-TERRAIN-LODFADE: LOD_NEAR/LOD_NEAR_FADE constants verbatim from gos_terrain.frag
// :163-164. camDist matches gos_terrain.frag :322-326 EXACTLY: ground-plane distance
// from the camera (Stuff/MLR .x=left/right,.y=elevation,.z=forward) to WorldPos (MC2
// .x=east,.y=north,.z=elevation), plus a zoom-out altitude boost.
const float LODFADE_NEAR      = 4000.0;
const float LODFADE_NEAR_FADE = 4500.0;
//PORT-BEGIN:lodNear
float mv_computeLodNear(vec2 worldXY, float worldZ) {
    if (uT.lodFadeEnabled == 0) return 1.0;   // gate off -> byte-identical (no fade)
    vec2  camGround = vec2(-uT.cameraPosMC2.x, uT.cameraPosMC2.z);
    float hDist      = distance(worldXY, camGround);
    float altBoost   = max(uT.cameraPosMC2.y - worldZ, 0.0) * 0.7;
    float camDist    = hDist + altBoost;
    return 1.0 - smoothstep(LODFADE_NEAR, LODFADE_NEAR_FADE, camDist);
}
//PORT-END:lodNear

vec3 mv_detailNormal(vec4 w, float snowWeight, vec2 worldXY, float lodNear, float macroNz, vec2 uvDx, vec2 uvDy) {
    vec2 uv = worldXY * (uT.detailTiling / MATV_WU_PER_TILE);
    vec2 uvRock     = uv * (MATV_TILING.x / 3.0);
    vec2 uvGrass    = uv * MATV_TILING.y;
    vec2 uvDirt     = uv * MATV_TILING.z;
    vec2 uvConcrete = uv * MATV_TILING.w;
    vec2 uvSnow     = uv * MATV_TILING_SNOW;
    // Screen-space derivative AA (GL fwRock/fwGrass/...): fade a layer to 0 as its
    // tiling goes sub-pixel. main() supplies the continuous UV footprint.
    vec2 uvWidth = abs(uvDx) + abs(uvDy);
    float fwRock     = clamp(1.0 - (length(uvWidth * (MATV_TILING.x / 3.0)) - 0.5) * 2.0, 0.0, 1.0);
    float fwGrass    = clamp(1.0 - (length(uvWidth * MATV_TILING.y) - 0.5) * 2.0, 0.0, 1.0);
    float fwDirt     = clamp(1.0 - (length(uvWidth * MATV_TILING.z) - 0.5) * 2.0, 0.0, 1.0);
    float fwConcrete = clamp(1.0 - (length(uvWidth * MATV_TILING.w) - 0.5) * 2.0, 0.0, 1.0);
    float fwSnow     = clamp(1.0 - (length(uvWidth * MATV_TILING_SNOW) - 0.5) * 2.0, 0.0, 1.0);

    // POM: parallax-offset the material UVs, fwidth-gated (near only). The march uses the real view vector, faded out over the world
    // distance MATV_POM_FADE_NEAR..FAR (the cost governor), over the slope band where the cliff blend takes over, and over cement
    // (a slab must not tear). lodNear fades the offset across the LOD0 band boundary (1.0 when the gate is off).
    // GL also requires u_lodStep <= 2 before it marches; the VK island has no lod step, and the lodNear fade is its accepted stand-in for that limit.
    if (fwRock > 0.4 && lodNear > 0.01) {
        const vec4 pomScaleMat = vec4(1.0, 1.0, 2.5, 1.0);
        float pomScale = MATV_POM_SCALE * dot(pomScaleMat, w) * fwRock * lodNear;
        vec2  pomUV    = uv * dot(MATV_TILING, w);
        vec2  pomOff   = vec2(0.0);
        vec3  camW     = mc2ToRender(uT.cameraPosMC2.xyz);   // the eye in the MC2 world frame (the feed is the Stuff frame)
        float altBoost = max(camW.z - v_worldPos.z, 0.0) * 0.7;
        float camDist  = distance(v_worldPos.xy, camW.xy) + altBoost;
        float distFade = 1.0 - smoothstep(MATV_POM_FADE_NEAR, MATV_POM_FADE_FAR, camDist);
        float slopeFade = 1.0 - smoothstep(0.85, 0.55, abs(macroNz));
        float pomStrength = distFade * slopeFade * (1.0 - w.w);
        if (pomStrength > 0.001) {
            vec3 Vw = normalize(camW - v_worldPos);                 // fragment -> eye, MC2 world
            // The detail UVs are top-down planar with no y flip: tangent u = east, bitangent v = north, normal = up.
            vec3 viewDirTS = vec3(Vw.x, Vw.z, Vw.y);
            pomOff = (mv_parallax(pomUV, pomScale, w, viewDirTS) - pomUV) * fwRock * lodNear * pomStrength;
        }
        uvRock += pomOff; uvGrass += pomOff; uvDirt += pomOff; uvConcrete += pomOff;
    }

    // Anti-tile scale per material (GL :382-385: tiling>=4 full, <=1 off). Near only.
    float atsRock  = mix(0.0, 3.0, clamp((MATV_TILING.x - 1.0) / 3.0, 0.0, 1.0));
    float atsGrass = mix(0.0, 3.0, clamp((MATV_TILING.y - 1.0) / 3.0, 0.0, 1.0));
    float atsDirt  = mix(0.0, 3.0, clamp((MATV_TILING.z - 1.0) / 3.0, 0.0, 1.0));
    float atsConc  = mix(0.0, 3.0, clamp((MATV_TILING.w - 1.0) / 3.0, 0.0, 1.0));

    vec3 dN = vec3(0.0);
    // rock: boost/3 (GL parity — rock detail was too dense/strong). GL :392-397.
    if (w.x != 0.0 && fwRock != 0.0 && lodNear != 0.0) {
        vec4 s = (atsRock > 0.01 && fwRock > 0.5)
               ? mv_sampleAntiTileArr(MAT_LAYER_ROCK, uvRock, atsRock, uvDx * (MATV_TILING.x / 3.0), uvDy * (MATV_TILING.x / 3.0))
               : textureGrad(u_matNormalArray, vec3(uvRock, float(MAT_LAYER_ROCK)), uvDx * (MATV_TILING.x / 3.0), uvDy * (MATV_TILING.x / 3.0));
        dN += w.x * (MATV_NORMAL_BOOST.x / 3.0) * fwRock * (s.rgb * 2.0 - 1.0);
    }
    if (w.y != 0.0 && fwGrass != 0.0 && lodNear != 0.0) {
        vec4 s = (atsGrass > 0.01 && fwGrass > 0.5)
               ? mv_sampleAntiTileArr(MAT_LAYER_GRASS, uvGrass, atsGrass, uvDx * MATV_TILING.y, uvDy * MATV_TILING.y)
               : textureGrad(u_matNormalArray, vec3(uvGrass, float(MAT_LAYER_GRASS)), uvDx * MATV_TILING.y, uvDy * MATV_TILING.y);
        dN += w.y * MATV_NORMAL_BOOST.y * fwGrass * (s.rgb * 2.0 - 1.0);
    }
    if (w.z != 0.0 && fwDirt != 0.0 && lodNear != 0.0) {
        vec4 s = (atsDirt > 0.01 && fwDirt > 0.5)
               ? mv_sampleAntiTileArr(MAT_LAYER_DIRT, uvDirt, atsDirt, uvDx * MATV_TILING.z, uvDy * MATV_TILING.z)
               : textureGrad(u_matNormalArray, vec3(uvDirt, float(MAT_LAYER_DIRT)), uvDx * MATV_TILING.z, uvDy * MATV_TILING.z);
        dN += w.z * MATV_NORMAL_BOOST.z * fwDirt * (s.rgb * 2.0 - 1.0);
    }
    if (w.w != 0.0 && fwConcrete != 0.0 && lodNear != 0.0) {
        vec4 s = (atsConc > 0.01 && fwConcrete > 0.5)
               ? mv_sampleAntiTileArr(MAT_LAYER_CONCRETE, uvConcrete, atsConc, uvDx * MATV_TILING.w, uvDy * MATV_TILING.w)
               : textureGrad(u_matNormalArray, vec3(uvConcrete, float(MAT_LAYER_CONCRETE)), uvDx * MATV_TILING.w, uvDy * MATV_TILING.w);
        dN += w.w * MATV_NORMAL_BOOST.w * fwConcrete * (s.rgb * 2.0 - 1.0);
    }
    if (snowWeight != 0.0 && fwSnow != 0.0 && lodNear != 0.0) {
        dN += snowWeight * 0.9 * fwSnow *
              (textureGrad(u_matNormalArray, vec3(uvSnow, float(MAT_LAYER_SNOW)),
                           uvDx * MATV_TILING_SNOW, uvDy * MATV_TILING_SNOW).rgb * 2.0 - 1.0);
    }
    // VK-TERRAIN-LODFADE: GL gos_terrain.frag :727-736 ties the WHOLE detail-normal
    // contribution to lodNear (detailMul), not just the POM offset -- stops the
    // detail-normal "carpet" from popping in/out at the LOD0/band boundary. lodNear
    // is 1.0 when the gate is off (byte-identical).
    return dN * lodNear;
}

// CEMENT-MIP-EDGE-1: antialiased "x < 0.5" for the cement transition cut; fw = the cut form's
// per-pixel footprint, so the edge is one pixel of coverage at any distance instead of a hard stair.
float cCutLt(float x, float fw) {
    float w = max(fw, 1e-4);
    return 1.0 - smoothstep(0.5 - w, 0.5 + w, x);
}

void main() {
    // Valid even for quads straddling cement, zero weights or the LOD fade.
    vec2 detailUv = v_worldPos.xy * (uT.detailTiling / MATV_WU_PER_TILE);
    vec2 detailUvDx = dFdx(detailUv), detailUvDy = dFdy(detailUv);
    vec2 cdWpDx = dFdx(v_worldPos.xy), cdWpDy = dFdy(v_worldPos.xy); float cdRoad = 0.0, cdW = 0.0;   // CONCRETE-ROAD-PBR-DETAIL: uniform derivatives; asphalt-texel flag; slab share
    vec3 base;
    float cookedBreakup = 0.5;
    // VK-TERRAIN-LIGHTING: track whether this fragment resolved to a solid cement slab.
    // GL treats pure concrete as a flat-lit slab (no NdotL relief) and drops the STATIC
    // self-shadow on it (terrain_lod_chunk.frag:894 + :902-907). We mirror that: on a
    // solid-cement hit -> flat-lit + no static shadow. (The GL dynamic building shadow on
    // concrete is a SEPARATE dynamic map, still deferred here.)
    bool pureConcrete = false;
    bool cementHit = false;   // a solid cement atlas texel won (the shoreline wet band skips it, as GL)
    if (uT.colormapReady != 0) {
        // The shared cook already applied the original nine-tap blur and coast-color repair.
        vec2 uv = vec2((v_worldPos.x - uT.atlasTopLeftX) * uT.atlasOneOverWorldUnits,
                       (uT.atlasTopLeftY - v_worldPos.y) * uT.atlasOneOverWorldUnits);
        vec4 macro = textureLod(u_colormap, uv, 0.0);
        base = macro.rgb;
        cookedBreakup = macro.a;
    } else {
        // Fail-soft olive placeholder (colormap not importable/ready this frame).
        float hMod = clamp(v_worldPos.z / 512.0 + 0.5, 0.35, 1.0);
        base = vec3(0.42, 0.40, 0.28) * hMod;
    }

    // ★CEMENT/ROAD sub-slice: concrete apron / roads. Ports the GL LOD-chunk cement
    // branch (terrain_lod_chunk.frag:484-624) VERBATIM: the solid-tile override, the
    // CEMENT-HARD-EDGE-1 marching-squares transition cut, and CEMENT-DIAG-CONNECT-1.
    // Fed by the per-vertex cement-word SSBO (uC) + the REPEAT-sampled cement atlas.
    // The overlay-sidecar composite (GL :626-642) is STILL DEFERRED (not fed here).
    // uT.useCement==0 (gate off / atlas or words not ready) -> whole block skipped ->
    // colormap base only, byte-identical to the colormap-only frame.
    if (uT.useCement != 0) {
        // CEMENT-MIP-EDGE-1: the road "outline". The atlas was sampled with texture() on a fract()ed
        // tile UV inside a per-fragment branch: at every tile wrap the implicit derivatives blow up
        // and the sampler drops to the atlas-wide average mip (a dark 1 px seam along every cement
        // tile edge and across roads), and at distance the half-texel inset only guarded mip 0, so
        // the neighbouring atlas cells bled into each road/apron cell's border texels (a dark halo
        // along both road edges on far hills). Derivatives now come from the CONTINUOUS tile UV,
        // here in uniform control flow; the LOD is explicit, capped so a cell keeps >= 2 texels,
        // and the inset is half a texel AT that LOD.
        vec2  cCont    = vec2(v_worldPos.x, -v_worldPos.y) / uT.cementWUPT;
        vec2  cGx      = dFdx(cCont), cGy = dFdy(cCont);
        float cCellTex = max(float(textureSize(u_cementAtlas, 0).x) / float(max(uT.cementGridSide, 1)), 1.0);
        float cLod     = 0.5 * log2(max(max(dot(cGx, cGx), dot(cGy, cGy)) * cCellTex * cCellTex, 1e-12));
        cLod = clamp(cLod, 0.0, min(max(log2(cCellTex) - 1.0, 0.0), float(textureQueryLevels(u_cementAtlas) - 1)));
        float cInsetLod = min(0.5 * exp2(cLod) / cCellTex, 0.25);
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
            int  cGridSide = uT.cementGridSide;
            if (cGridSide < 1) cGridSide = 1;
            int  cCol = int(cLayerIdx) % cGridSide;
            int  cRow = int(cLayerIdx) / cGridSide;
            vec2 cTileUV  = fract(vec2(v_worldPos.x, -v_worldPos.y) / uT.cementWUPT);
            // CEMENT-ATLAS-SEAM-1 (terrain_lod_chunk.frag): half-texel inset for sampling only; the
            // transition mask below keeps the true intra-tile position cTileUV.
            vec2 cSampleUV = cTileUV;
            if (uT.cementAtlasSeamFix != 0)
                cSampleUV = clamp(cTileUV, vec2(cInsetLod), vec2(1.0 - cInsetLod));   // CEMENT-MIP-EDGE-1
            vec2 cAtlasUV = (vec2(float(cCol), float(cRow)) + cSampleUV) / float(cGridSide);
            vec4 cementTexel = textureLod(u_cementAtlas, cAtlasUV, cLod); cdRoad = 1.0 - smoothstep(0.20, 0.30, dot(cementTexel.rgb, vec3(0.299, 0.587, 0.114)));
            vec3 cementColor = cementTexel.rgb * vec3(uT.missionFlatR, uT.missionFlatG, uT.missionFlatB);  // CEMENT-NIGHT-LIGHT-1
            // CEMENT-ALPHA-LAND-1 (terrain_lod_chunk.frag:1009-1015): OWN_ALPHA transition.
            // Word bit23 => the atlas cell carries its own coverage in alpha; composite OVER base.
            bool cementOwnAlpha = (cw & 0x00800000u) != 0u;
            if (isTransition && cementOwnAlpha) {
                float cOwnA = cementTexel.a;
                base = mix(base, cementColor, cOwnA);
                cdW = cOwnA; if (cOwnA > 0.5) { cementHit = true; pureConcrete = true; }
            } else if (isTransition) {
                if (uT.useTransitionMask != 0) {
                    bool nXm = false, nXp = false, nYm = false, nYp = false;
                    bool nXmYm = false, nXpYm = false, nXpYp = false, nXmYp = false;
                    int xm = ctX - 1, xp = ctX + 1, ym = ctY - 1, yp = ctY + 1;
                    if (xm >= 0)                           nXm   = (uC.cementWordsF[xm + ctY * uT.mapSide] & 0x80000000u) != 0u;
                    if (xp <  uT.mapSide)                  nXp   = (uC.cementWordsF[xp + ctY * uT.mapSide] & 0x80000000u) != 0u;
                    if (ym >= 0)                           nYm   = (uC.cementWordsF[ctX + ym * uT.mapSide] & 0x80000000u) != 0u;
                    if (yp <  uT.mapSide)                  nYp   = (uC.cementWordsF[ctX + yp * uT.mapSide] & 0x80000000u) != 0u;
                    if (xm >= 0 && ym >= 0)                nXmYm = (uC.cementWordsF[xm + ym * uT.mapSide] & 0x80000000u) != 0u;
                    if (xp <  uT.mapSide && ym >= 0)       nXpYm = (uC.cementWordsF[xp + ym * uT.mapSide] & 0x80000000u) != 0u;
                    if (xp <  uT.mapSide && yp < uT.mapSide) nXpYp = (uC.cementWordsF[xp + yp * uT.mapSide] & 0x80000000u) != 0u;
                    if (xm >= 0 && yp < uT.mapSide)        nXmYp = (uC.cementWordsF[xm + yp * uT.mapSide] & 0x80000000u) != 0u;
                    float cTL = (nXm   && nYm   && nXmYm) ? 1.0 : 0.0;  // UV (0,0)
                    float cTR = (nXp   && nYm   && nXpYm) ? 1.0 : 0.0;  // UV (1,0)
                    float cBR = (nXp   && nYp   && nXpYp) ? 1.0 : 0.0;  // UV (1,1)
                    float cBL = (nXm   && nYp   && nXmYp) ? 1.0 : 0.0;  // UV (0,1)
                    float u = cTileUV.x, v = cTileUV.y;
                    int cfg = (cTL > 0.5 ? 8 : 0)
                            | (cTR > 0.5 ? 4 : 0)
                            | (cBR > 0.5 ? 2 : 0)
                            | (cBL > 0.5 ? 1 : 0);
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
                    cdW = alpha; if (alpha > 0.5) { cementHit = true; pureConcrete = true; }
                }
            } else {
                base = cementColor; cdW = 1.0;
                cementHit = true;
                pureConcrete = true;
            }
        }

        // CEMENT-DIAG-CONNECT-1 (gate uT.cementDiagConnect): fill this tile's corner
        // quadrant toward a diagonally adjacent SOLID cement tile. Off by default.
        if (uT.cementDiagConnect != 0 && !cementHit) {
            vec2 dTileUV = fract(vec2(v_worldPos.x, -v_worldPos.y) / uT.cementWUPT);
            int  dGridSide = uT.cementGridSide;
            if (dGridSide < 1) dGridSide = 1;
            int dnX = (dTileUV.x > 0.5) ? (ctX + 1) : (ctX - 1);
            int dnY = (dTileUV.y > 0.5) ? (ctY + 1) : (ctY - 1);
            if (dnX >= 0 && dnX < uT.mapSide && dnY >= 0 && dnY < uT.mapSide) {
                uint dnw = uC.cementWordsF[dnX + dnY * uT.mapSide];
                bool dnValid      = (dnw & 0x80000000u) != 0u;
                bool dnTransition = (dnw & 0x40000000u) != 0u;
                if (dnValid && !dnTransition) {
                    uint dLayerIdx = dnw & 0xFFFFu;
                    int  dCol = int(dLayerIdx) % dGridSide;
                    int  dRow = int(dLayerIdx) / dGridSide;
                    vec2 dSampleUV = dTileUV;   // CEMENT-ATLAS-SEAM-1 / CEMENT-MIP-EDGE-1: same inset + LOD as the main branch
                    if (uT.cementAtlasSeamFix != 0)
                        dSampleUV = clamp(dTileUV, vec2(cInsetLod), vec2(1.0 - cInsetLod));
                    vec2 dAtlasUV = (vec2(float(dCol), float(dRow)) + dSampleUV) / float(dGridSide);
                    base = textureLod(u_cementAtlas, dAtlasUV, cLod).rgb * vec3(uT.missionFlatR, uT.missionFlatG, uT.missionFlatB);  // CEMENT-NIGHT-LIGHT-1
                    pureConcrete = true; cdW = 1.0;
                    cementHit = true;   // as GL's diag-connect branch
                }
            }
        }
    }

    // ★VK-TERRAIN-LIGHTING compose. Mirrors terrain_lod_chunk.frag:889-928 (the NdotL
    // relief band + baked static sun shadow, cement flat-lit + shadow-dropped). Terrain
    // normal is the flat-up (0,0,1) the GBuffer1 encodes and GL's calcShadow uses as
    // shadowN. Gate uT.lightingEnabled==0 -> lit==base (identity) -> byte-identical to
    // the pre-lighting frame. DEFERRED vs GL: detail-normal/POM/macro-variation relief,
    // the DYNAMIC (mech/building) shadow, and hemisphere ambient fill.
    // VK-PARITY-F3: GL baseColor pipeline (terrain_lod_chunk.frag:1546-1565,1628,1636-1663,
    // 1838-1845) -- default-ON in GL, previously absent here so VK terrain albedo was the raw
    // colormap (darker and less blue than GL on shaded/grassy ground, day and night):
    //   materialTint blend (tintStrength = mix(0.18..0.50 by luma, 0.85, snow) * scale),
    //   restore authored cement tone, cliff desaturate+darken toward rock on steep MACRO slope,
    //   world-space break-up noise (0.78..1.18), snow brightness dampen.
    // Overlay-sidecar coverage is 0 in VK (sidecar not fed), matching GL when OVERLAY_V2 is off.
    vec3 baseColor = base;
    float F3pc = pureConcrete ? 1.0 : 0.0;
    {
        vec4 f3W; float f3Snow;
        mv_weights(base, f3W, f3Snow);
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
        float macroNzF3 = mv_smoothTerrainNormal(v_worldPos.xy).z;
        float cliffBlend = smoothstep(0.85, 0.55, abs(macroNzF3));
        if (cliffBlend > 0.01) {
            float luma = dot(baseColor, vec3(0.299, 0.587, 0.114));
            vec3  cliffColor = mix(vec3(luma), vec3(uT.tintRockR, uT.tintRockG, uT.tintRockB), 0.6) * 0.8;
            baseColor = mix(baseColor, cliffColor, cliffBlend * 0.7 * (1.0 - F3pc));
        }
        // World-space two-octave break-up noise (non-snow, non-concrete).
        float noiseBlend = cookedBreakup;
        if (uT.colormapReady < 2) {
            float lowFreq = fbm(v_worldPos.xy * 0.0035, 3) * 0.5 + 0.5;
            float highFreq = fbm(v_worldPos.xy * 0.018, 2) * 0.5 + 0.5;
            noiseBlend = mix(lowFreq, highFreq, 0.55);
        }
        float breakup = mix(0.78, 1.18, noiseBlend);
        baseColor *= mix(1.0, breakup, (1.0 - f3Snow) * (1.0 - F3pc));
        baseColor *= mix(1.0, uT.snowBrightnessDampen, f3Snow);
    }

    vec3 lit = base;
    if (uT.lightingEnabled != 0) {
        // VK-TERRAIN-DETAIL: perturb the terrain normal like GL (terrain_lod_chunk.frag
        // :740-808). Gate uT.detailEnabled==0 -> N stays flat-up -> byte-identical to the
        // lighting-only frame. On cement (pureConcrete) GL suppresses the detail perturb
        // (dN *= 1-pureConcrete) and lights the slab flat, so we keep N flat-up there too.
        //   pert = vec3(baseN.xy/max(baseN.z,0.2)*nfhStrength + dN.xy*detailStrength, 1.0)
        //   N    = normalize(pert)
        // baseN = height-derived macro slope (always live via the bound height SSBO);
        // dN    = weighted material detail normal (only when the array imported).
        vec3 N = vec3(0.0, 0.0, 1.0);                            // flat-up terrain normal (default)
        vec3 dbgBaseN = vec3(0.0, 0.0, 1.0); vec3 dbgDN = vec3(0.0);  // CSM-BLACKOUT-RECON-1 taps
        if (uT.detailEnabled != 0 && !pureConcrete) {
            vec3 baseN = mv_smoothTerrainNormal(v_worldPos.xy);
            vec3 dN    = vec3(0.0);
            if (uT.detailReady != 0 && uT.detailStrength != 0.0) {
                vec4  matW; float snowW;
                mv_weights(base, matW, snowW);                   // classify from colormap base
                // VK-TERRAIN-LODFADE: fades detail-normal/POM to 0 across the LOD0/band
                // boundary (GL gos_terrain.frag :326-328). Gate off -> lodNear==1.0.
                float lodNear = mv_computeLodNear(v_worldPos.xy, v_worldPos.z);
                dN = mv_detailNormal(matW, snowW, v_worldPos.xy, lodNear, baseN.z, detailUvDx, detailUvDy);
            }
            vec3 pert = vec3(baseN.xy / max(baseN.z, 0.2) * uT.nfhStrength
                             + dN.xy * uT.detailStrength, 1.0);
            N = normalize(pert);
            if (uT.detailReady >= 2) mv_cliffMaterial(baseN, N, baseColor);   // steep cells: triplanar normal + cliff POM + groove relief
            dbgBaseN = baseN; dbgDN = dN;                        // CSM-BLACKOUT-RECON-1 taps
        }
        vec3 lightDir = uT.terrainLightDir.xyz;

        float normalLight = computeNdotLBand(N, lightDir);
        if (pureConcrete) normalLight = 1.0;                     // cement: flat-lit slab
        vec3 cdSpec = vec3(0.0);   // CONCRETE-ROAD-PBR-DETAIL: detail only (AO, relative sun shading, sheen), MC2's colour stays
        if (uT.concreteDetail != 0 && cdW > 0.0) cdApply(cdRoad > 0.5 ? CD_LAYER_ROAD : CD_LAYER_PAD, v_worldPos.xy, cdWpDx, cdWpDy, lightDir, cdW, baseColor, normalLight, cdSpec);

        // Shadow compose — mirrors terrain_lod_chunk.frag:896-907 EXACTLY:
        //   staticS = calcShadow(...16);  dynS = calcDynamicShadow(...8);
        //   shadow  = min(staticS, dynS);
        //   shadow  = mix(shadow, dynS, pureConcrete);   // concrete: dyn only, static dropped
        // GL callers pass shadowN = flat-up (0,0,1), NOT the perturbed N. computeBakedShadow
        // is a VERBATIM match of the legacy calcDynamicShadow body (adaptiveScale 2.4->0.8,
        // mix(0.4,1.0,shadow)); the GL static calcShadow uses the same PCF core here too.
        // VK-PARITY-SHADOW-1: GL (STATIC-SHADOW-BANDING-2, default ON) feeds the MACRO slope
        // normal as shadowN; calcShadow normal-offsets by it then resets to flat-up for
        // guard+bias, calcDynamicShadow uses it for NdotL. Flag off -> legacy flat-up.
        // (Skirt strips: GL keeps flat-up there; VK shares this frag, residual noted.)
        LsvkShadowKnobs shKnobs = mv_shadowKnobs();
        vec3 shadowN = vec3(0.0, 0.0, 1.0);
        if ((shKnobs.flags & LSVK_KNOB_BANDFIX_V2) != 0) shadowN = mv_smoothTerrainNormal(v_worldPos.xy);
        float staticS = 1.0;
        if (uT.enableShadows != 0) {
            staticS = terrainStaticShadow(v_worldPos, shadowN, lightDir, shKnobs);
        }
        // VK-TERRAIN-CSM-NEAR-1: near-cascade-array path when armed+valid (uT.csmEnabled),
        // else the legacy single dynamic map. Both now carry the GL dist-fade + clamped sun.
        float dynS = 1.0;
        if (uT.csmEnabled != 0 || uT.enableDynShadows != 0) {
            // T15: bounded terrain-only receiver (4 compare fetches); objects keep the tent.
            vec3 shSun = lsvk_shadowSun(lightDir, shKnobs);         // TERRAIN-SHADOW-GUARD-1
            dynS = terrainDynamicShadow(v_worldPos, shadowN, shSun);
        }
        float shadow = min(staticS, dynS);
        lit = max(baseColor * normalLight * shadow + cdSpec * shadow, vec3(0.0));

        // VK-PARITY-TERRAIN-2: NIGHT-LIGHT-POOLS-1..4 / LIGHT-CLUSTER-PERF-1 /
        // MOVER-CONTACT-SHADOW-1 — the SAME shared body GL runs (terrain_lod_chunk.frag
        // :2056-2066), ADDITIVE after the shadow multiply. Cone spill / LIGHT-REALISM
        // reshape is baked into the pool by the shared CPU producer.
        float moverContact = 0.0;
        if (uT.numPoolLights > 0) {
            lit += tlp_terrainPoolLight(v_worldPos, uT.numPoolLights,
                                        uT.lightGridDim, vec2(uT.lightGridMinX, uT.lightGridMinY),
                                        vec2(uT.lightGridInvCellX, uT.lightGridInvCellY),
                                        uT.lightPoolSoft, uT.lightPoolMoverDim, uT.lightPoolMoverScale,
                                        uT.lightPoolMoverMax, uT.lightPoolCompress,
                                        uT.moverContactShadow, moverContact);
        }
        // PAD-AREA-GLOW-1 (terrain_lod_chunk.frag:2075-2076). useCement gates the read of
        // the cement-word SSBO (dummy 16B until the snapshot lands).
        vec3 padGlow = vec3(uT.padGlowR, uT.padGlowG, uT.padGlowB);
        if (dot(padGlow, padGlow) > 0.0 && uT.useCement != 0)
            lit += padGlow * tlp_padGlowMask(v_worldPos.xy, uT.halfMap, uT.mapSide, uT.padGlowSpill);

        // VK-PARITY-TERRAIN-1: TERRAIN-AMBIENT-FLOOR-2, verbatim from
        // terrain_lod_chunk.frag:2267-2277. baseColor == base here (VK has no material
        // tint); flatLitShare == pureConcrete at stock u_cementLit=0 (the GL expression
        // cementLightBlend*(1-u_cementLit)). Floor/gate come from the same C++ producer.
        if (uT.ambientFloor > 0.0 && uT.ambientDayGate > 0.0) {
            float skyF   = N.z * 0.5 + 0.5;
            vec3  ambSky = vec3(0.55, 0.62, 0.75);
            vec3  ambGnd = vec3(0.34, 0.30, 0.24);
            vec3  ambCol = mix(ambGnd, ambSky, skyF);
            float ambShadow = mix(0.55, 1.0, shadow);
            float flatLitShare = pureConcrete ? 1.0 : 0.0;
            vec3  ambTerm   = baseColor * ambCol * uT.ambientFloor * ambShadow
                            * uT.ambientDayGate * (1.0 - flatLitShare);
            lit += ambTerm;
        }

        // MOVER-CONTACT-SHADOW-1 (terrain_lod_chunk.frag:2287-2288): contact-AO on the
        // fully composited lighting. moverContact==0 unless MC2_MOVER_CONTACT_SHADOW>0.
        if (moverContact > 0.0)
            lit *= (1.0 - 0.55 * moverContact);

        // TERRAIN-EDGE-FEATHER-1 (GL terrain_lod_chunk.frag, after the mover contact): the last tile band of the map fades to the sky haze
        // colour so the straight map perimeter does not cut a hard edge. halfMap 0 means the map size is not known yet.
        if (uT.edgeFeatherStrength > 0.0 && uT.halfMap > 0.0) {
            float haze = edgeHazeAmount(v_worldPos.xy, uT.halfMap) * uT.edgeFeatherStrength;
            lit = mix(lit, EDGE_HAZE_SKY, clamp(haze, 0.0, 1.0));
        }

        // TERRAIN-SHORELINE-V3/V4 (terrain_lod_chunk.frag, after the edge feather): wet and foam bands measured as the
        // HORIZONTAL run from the waterline through the macro slope, with static fbm jitter on the run, a fade-in over the
        // first 1.5 wu above the water, the optional mask modulator, a 12-20 deg slope guard and a beach affinity term.
        if (uT.shorelineOn != 0) {
            float aboveWater = v_worldPos.z - uT.shorelineWaterElev;
            vec3  macroN     = mv_smoothTerrainNormal(v_worldPos.xy);
            float nzc        = clamp(abs(macroN.z), 0.0, 0.9995);
            float sinSlope   = max(sqrt(max(1.0 - nzc * nzc, 1e-6)), 0.03);
            float horizDist  = max(aboveWater, 0.0) * (nzc / sinSlope);
            float horizDistJ = max(horizDist + fbm(v_worldPos.xy * 0.055, 2) * uT.shorelineEdgeJitter, 0.0);
            float slWet  = 1.0 - smoothstep(0.0, max(uT.shorelineWetRun, 1e-4), horizDistJ);
            float slFoam = 1.0 - smoothstep(0.0, max(uT.shorelineFoamRun, 1e-4), horizDistJ);
            float aboveFade = smoothstep(0.0, 1.5, horizDist);
            slWet  *= aboveFade;
            slFoam *= aboveFade;
            if (uT.shorelineHasMask != 0) {
                vec2 slUV = vec2((v_worldPos.x - uT.shorelineBoundsX) / max(uT.shorelineBoundsW, 1e-5),
                                 (uT.shorelineBoundsY - v_worldPos.y) / max(uT.shorelineBoundsH, 1e-5));
                if (slUV.x >= 0.0 && slUV.x <= 1.0 && slUV.y >= 0.0 && slUV.y <= 1.0) {
                    vec4 sl = texture(u_shorelineMask, slUV);
                    slWet  *= mix(1.0, clamp(sl.g, 0.0, 1.0), sl.a);
                    slFoam *= mix(1.0, clamp(sl.b, 0.0, 1.0), sl.a);
                }
            }
            float slopeAtten = clamp((N.z - 0.940) / (0.978 - 0.940), 0.0, 1.0);   // full at 12 deg, zero at 20 deg
            vec4 slW; float slSnow;
            mv_weights(base, slW, slSnow);
            slW = mix(slW, vec4(0.0, 0.0, 0.0, 1.0), F3pc);
            slSnow *= (1.0 - F3pc);
            float litLuma       = dot(lit, vec3(0.299, 0.587, 0.114));
            float shoreAffinity = smoothstep(0.08, 0.26, litLuma) * clamp(1.0 - slW.x - slSnow, 0.0, 1.0);
            if (slWet > 0.001 && !cementHit && slopeAtten > 0.0 && shoreAffinity > 0.001) {
                float wetAmt = clamp(slWet, 0.0, 1.0) * (1.0 - F3pc) * slopeAtten * shoreAffinity * uT.shorelineStrength;
                lit = mix(lit, vec3(1.16, 1.06, 0.78) * (litLuma * 0.90), clamp(wetAmt, 0.0, 1.0) * 0.85);
            }
            if (slFoam > 0.001 && slopeAtten > 0.0 && shoreAffinity > 0.001) {
                float foamScroll = fbm(v_worldPos.xy * 0.09 + uT.shorelineTime * 0.15, 3) * 0.5 + 0.5;
                float foamPulse  = fbm(v_worldPos.xy * 0.20 - uT.shorelineTime * 0.22 + 19.0, 2) * 0.5 + 0.5;
                float foamNoise  = clamp(mix(foamScroll, foamPulse, 0.5), 0.0, 1.0);
                float slF        = clamp(slFoam, 0.0, 1.0);
                float coverage   = smoothstep(1.0 - slF * 0.85, 1.0, foamNoise + (1.0 - slF) * 0.15);
                float foamAmt    = coverage * slopeAtten * uT.shorelineFoamStrength * shoreAffinity;
                lit = mix(lit, vec3(0.92), clamp(foamAmt, 0.0, 1.0) * (1.0 - F3pc));
            }
        }

        // CSM-BLACKOUT-RECON-1: one-shot debug viz (env MC2_VK_TERRAIN_CSM_DEBUG=N).
        // Bypasses the normal composite entirely so each channel can be inspected in
        // isolation on a live capture. No effect when uT.csmDebugMode==0.
        if (uT.csmDebugMode == 1)      { fragColor = vec4(vec3(normalLight), 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }
        else if (uT.csmDebugMode == 2) { fragColor = vec4(lightDir * 0.5 + 0.5, 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }
        else if (uT.csmDebugMode == 3) { fragColor = vec4(vec3(staticS), 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }
        else if (uT.csmDebugMode == 4) { fragColor = vec4(vec3(dynS), 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }
        else if (uT.csmDebugMode == 5) { fragColor = vec4(vec3(shadow), 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }
        else if (uT.csmDebugMode == 6) { fragColor = vec4(baseColor, 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }   // VK-PARITY-F3: albedo (GL MC2_LIGHTING_DEBUG_VIEW=40)
        else if (uT.csmDebugMode == 7) { fragColor = vec4(vec3(clamp(dot(N, lightDir), 0.02, 1.0)), 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }   // sun N.L diffuse (GL MC2_LIGHTING_DEBUG_VIEW=sun)
        else if (uT.csmDebugMode == 8) { fragColor = vec4(N * 0.5 + 0.5, 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }   // surface normal (GL ...=normal)
    }

    fragColor = vec4(lit, 1.0);   // opaque in-place (a=1, NOT premult)

    // ★TERRAIN shadow-class GBuffer1: flat-up normal + a=1.0 (handles own shadow).
    GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
}
