// LOD terrain patches co-write the shared scene GBuffer in place.
// MRT0: opaque colormap with cement, material detail, shadows and local lighting.
// MRT1: flat-up normal plus alpha=1, the terrain's self-shadowed receiver class.
// Colormap UVs and material shading retain terrain_lod_chunk.frag conventions.
#version 450
#extension GL_GOOGLE_include_directive : require
#ifndef PREC
#define PREC highp
#endif
#include "../include/noise.hglsl"   // VK-PARITY-F3: fbm() for the GL colour break-up (terrain_lod_chunk.frag:1839)
vec3 mv_lampPoolNormal(vec3 wp, vec3 macroN, float lod);
#define TLP_LAMP_NORMAL mv_lampPoolNormal
#include "include/terrain_frame_vk.glsl"   // TerrainFrameUBO + set-0 resources + CSM shadow helpers
#undef TLP_LAMP_NORMAL
#include "include/ground_lighting.glsl"
#include "include/concrete_detail_vk.glsl"
#include "../include/axis.hglsl"   // mc2ToRender: the POM view vector is built in the MC2 world frame
#include "../include/edge_haze.hglsl"   // edgeHazeAmount + EDGE_HAZE_SKY, shared with GL

layout(location = 0) in vec3 v_worldPos;

layout(location = 0) out vec4 fragColor;   // MRT0 color
layout(location = 1) out vec4 GBuffer1;    // MRT1 normal + shadow class

layout(set = 0, binding = 16) uniform sampler2D u_macroGradient;
layout(set = 0, binding = 15) uniform sampler2D u_materialWeights;
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
const float MATV_WU_PER_TILE  = 768.0;  // MAT_WORLD_UNITS_PER_TILE (terrain_lod_chunk.frag:240)
#include "include/terrain_detail_clamp.glsl"

// Central difference commutes with bilinear interpolation; normalize after filtering.
// Call at the point of use: hoisting the result to the top of main() kept it live across the whole shader and cost ~15% at 4K.
vec3 mv_smoothTerrainNormal(vec2 worldXY) {
    vec2 cell = vec2(worldXY.x + uT.halfMap, uT.halfMap - worldXY.y) / 128.0;
    vec2 gradient = textureLod(u_macroGradient, (cell + 1.5) / float(uT.mapSide + 2), 0.0).rg;
    return normalize(vec3(gradient, 1.0));
}

// Gradients come from uniform main() control flow, before cement/material
// selection. Offset anti-tile lookups use the same continuous UV footprint.
vec2 mv_hash22(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}
vec4 mv_sampleAntiTileArr(int layer, vec2 uv, float scale, vec2 dx, vec2 dy) {
    vec4 s0 = textureGrad(u_matNormalArray, vec3(uv,        float(layer)), dx, dy);
    vec2 f = fract(uv / scale);
    float w1 = smoothstep(0.2, 0.5, f.x) * smoothstep(0.2, 0.5, f.y);
    float w2 = smoothstep(0.2, 0.5, 1.0 - f.x) * smoothstep(0.2, 0.5, 1.0 - f.y);
    // Independent taps overlap without divergent texture branches on NVIDIA.
    // Explicit gradients and the original zero blend weights preserve the footprint.
    vec2 off1 = mv_hash22(floor(uv / scale)) * scale;
    vec4 s1 = textureGrad(u_matNormalArray, vec3(uv + off1, float(layer)), dx, dy);
    vec2 off2 = mv_hash22(floor(uv / scale) + vec2(7.0, 13.0)) * scale;
    vec4 s2 = textureGrad(u_matNormalArray, vec3(uv + off2, float(layer)), dx, dy);
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
    float prevD = curD;
    for (int i = 0; i < 16; ++i) {
        if (curLayer >= curD) break;
        prevD = curD;
        curUV -= dUV;
        curD = mv_pomDisplacement(curUV, w);
        curLayer += layerDepth;
    }
    vec2 prevUV = curUV + dUV;
    float after  = curD - curLayer;
    float before = prevD - curLayer + layerDepth;
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
    float prevH = curH;
    for (int i = 0; i < 32; ++i) {
        if (float(i) >= nSteps || curDepth >= curH) break;
        prevH = curH;
        curUV   += deltaUV;
        curDepth += layerDepth;
        curH = 1.0 - mv_cliffHeight(curUV);
    }
    vec2  prevUV = curUV - deltaUV;
    float afterH = curH - curDepth;
    float beforeH = prevH - (curDepth - layerDepth);
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
#include "include/terrain_detail_normal.glsl"

// Lamp-only detail uses an isotropic footprint computed in uniform main flow.
// The normal is sampled once at the first unoccluded, nonzero lamp.
vec3 mv_lampPoolNormal(vec3 wp, vec3 macroN, float lod) {
    float footprint = exp2(lod) / float(textureSize(u_matNormalArray, 0).x);
    if (footprint >= 0.707107) return macroN;
    vec2 uv = vec2((wp.x - uT.atlasTopLeftX) * uT.atlasOneOverWorldUnits,
                   (uT.atlasTopLeftY - wp.y) * uT.atlasOneOverWorldUnits);
    vec4 m = uT.colormapReady != 0 ? textureLod(u_materialWeights, uv, 0.0) : vec4(0);
    vec4 w = vec4(max(1.0 - m.x - m.y - m.w, 0.0), m.x, m.y, m.w);
    // Keep the lamp consumer small: one XY-only tap with explicit isotropic
    // LOD, no sun/POM/anti-tile call graph inside the light loop.
    const vec4 tiling = vec4(MATV_TILING.x / 3.0, MATV_TILING.yzw);
    vec4 fade = clamp(vec4(2.0) - 2.828427 * footprint * tiling, 0.0, 1.0);
    vec4 strength = w * MATV_NORMAL_BOOST * vec4(1.0 / 3.0, 1, 1, 1) * fade;
    // A lamp needs micro-relief, not a second full material evaluation. Use
    // the strongest visible layer; sun/shore/debug retain the weighted blend.
    int layer = strength.y > strength.x ? 1 : 0;
    if (strength.z > strength[layer]) layer = 2;
    if (strength.w > strength[layer]) layer = 3;
    float scale = tiling[layer];
    float amount = strength[layer];
    float snow = m.z * 0.9 * clamp(2.0 - 2.828427 * footprint * MATV_TILING_SNOW, 0.0, 1.0);
    if (snow > amount) { layer = MAT_LAYER_SNOW; scale = MATV_TILING_SNOW; amount = snow; }
    if (amount <= 0.0) return macroN;
    vec2 detailUV = wp.xy * (uT.detailTiling / MATV_WU_PER_TILE) * scale;
    vec2 dN = amount * (textureLod(u_matNormalArray, vec3(detailUV, float(layer)), lod + log2(scale)).xy * 2.0 - 1.0);
    return normalize(vec3(macroN.xy / max(macroN.z, 0.2) + dN * uT.detailStrength, 1.0));
}

// CEMENT-MIP-EDGE-1: antialiased "x < 0.5" for the cement transition cut; fw = the cut form's
// per-pixel footprint, so the edge is one pixel of coverage at any distance instead of a hard stair.
float cCutLt(float x, float fw) {
    float w = max(fw, 1e-4);
    return 1.0 - smoothstep(0.5 - w, 0.5 + w, x);
}

// Permanent road art uses the former overlay's material response on the terrain
// receiver. Alpha remains binary; rejected texels expose the original ground.
vec3 shadeRoadArt(vec4 art, uint road, uint cw, vec2 dx, vec2 dy) {
    bool adopt = (road & 0x00010000u) != 0u;
    bool cementTile = (cw & 0x80000000u) != 0u;
    bool adoptAlbedo = adopt && cementTile && (cw & 0x01000000u) == 0u;
    vec3 color = art.rgb;
    if (adoptAlbedo) {
        int grid = max(uT.cementGridSide, 1);
        uint layer = cw & 0xffffu;
        vec2 uv = fract(vec2(v_worldPos.x, -v_worldPos.y) / uT.cementWUPT);
        if (uT.cementAtlasSeamFix != 0) {
            float inset = 0.5 * float(grid) / float(textureSize(u_cementAtlas, 0).x);
            uv = clamp(uv, vec2(inset), vec2(1.0 - inset));
        }
        color = textureLod(u_cementAtlas, (vec2(int(layer) % grid, int(layer) / grid) + uv) / float(grid), 0.0).rgb;
    } else if (!adopt) {
        color *= vec3(0.82, 0.80, 0.76);
    }
    color *= vec3(uT.missionFlatR, uT.missionFlatG, uT.missionFlatB);
    vec3 normal = vec3(0, 0, 1);
    float shadow = min(uT.enableShadows != 0 ? terrainStaticShadow(v_worldPos) : 1.0,
                       terrainDynamicShadow(v_worldPos, normal));
    vec3 spec = vec3(0);
    if (uT.concreteDetail != 0) {
        MaterialDetail detail = cdEval(adoptAlbedo ? CD_LAYER_PAD : CD_LAYER_ROAD, v_worldPos.xy, dx, dy);
        float diffuse;
        cdShade(detail, normalize(uT.terrainLightDir.xyz), uT.ambientDayGate, diffuse, spec);
        color *= mix(1.0, detail.ao, 0.5) * detail.lum * diffuse;
        normal = detail.n;
    }
    color = max((color + spec) * shadow, vec3(0));
    if (!cementTile) {
        float dist = distance(v_worldPos.xy, uT.cameraPosMC2.xy);
        float fog = clamp(1.0 - exp(-dist * 0.00006 * exp(-max(v_worldPos.z, 0.0) * 0.002)), 0.0, 0.70);
        color = mix(color, vec3(0.58, 0.65, 0.75), fog);
    }
    if (uT.numPoolLights > 0) {
        float unusedContact = 0.0;
        color += tlp_terrainPoolLight(v_worldPos, uT.numPoolLights,
            uT.lightGridDim, vec2(uT.lightGridMinX, uT.lightGridMinY),
            vec2(uT.lightGridInvCellX, uT.lightGridInvCellY),
            uT.lightPoolSoft, uT.lightPoolMoverDim, uT.lightPoolMoverScale,
            uT.lightPoolMoverMax, uT.lightPoolCompress, 0.0, unusedContact,
            normal, vec3(0, 0, 1), false, 0.0);
    }
    if (uT.halfMap > 0.0)
        color = mix(color, vec3(0.58, 0.65, 0.75),
            smoothstep(uT.halfMap - 256.0, uT.halfMap - 32.0, max(abs(v_worldPos.x), abs(v_worldPos.y))));
    return color;
}

void main() {
    vec2 roadDx = dFdx(v_worldPos.xy), roadDy = dFdy(v_worldPos.xy);
    int plane = uT.mapSide * uT.mapSide;
    if (uT.useCement != 0 && uC.cementWordsF.length() >= 2 * plane) {
        ivec2 tile = clamp(ivec2(floor(vec2(v_worldPos.x + uT.halfMap, uT.halfMap - v_worldPos.y) / 128.0)),
                           ivec2(0), ivec2(uT.mapSide - 1));
        int index = tile.x + tile.y * uT.mapSide;
        uint road = uC.cementWordsF[plane + index];
        if ((road & 0x80000000u) != 0u) {
            int grid = max(uT.cementGridSide, 1);
            int layer = int(road & 0xffffu);
            // Match the original quad UV endpoints, including the half-texel inset.
            vec2 uv = mix(vec2(0.0078125), vec2(0.9921875),
                fract(vec2(v_worldPos.x + uT.halfMap, uT.halfMap - v_worldPos.y) / 128.0));
            vec4 art = textureLod(u_cementAtlas, (vec2(layer % grid, layer / grid) + uv) / float(grid), 0.0);
            if (art.a >= 0.5) {
                fragColor = vec4(shadeRoadArt(art, road, uC.cementWordsF[index], roadDx, roadDy), 1.0);
                GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
                return;
            }
        }
    }
    float cdRoad = 0.0;   // CONCRETE-ROAD-PBR-DETAIL: the cement atlas texel is asphalt (roads, runway)
    vec3 base;
    float cookedBreakup = 0.5;
    vec4 materials = vec4(0.0);
    // VK-TERRAIN-LIGHTING: track whether this fragment resolved to a solid cement slab.
    // GL treats pure concrete as a flat-lit slab (no NdotL relief) and drops the STATIC
    // self-shadow on it (terrain_lod_chunk.frag:894 + :902-907). We mirror that: on a
    // solid-cement hit -> flat-lit with ordinary static and dynamic shadows. (The GL dynamic building shadow on
    // concrete is a SEPARATE dynamic map, still deferred here.)
    bool pureConcrete = false;
    float padEdgeAO = 1.0;
    bool cementHit = false;   // a solid cement atlas texel won (the shoreline wet band skips it, as GL)
    if (uT.colormapReady != 0) {
        // The shared cook already applied the original nine-tap blur and coast-color repair.
        vec2 uv = vec2((v_worldPos.x - uT.atlasTopLeftX) * uT.atlasOneOverWorldUnits,
                       (uT.atlasTopLeftY - v_worldPos.y) * uT.atlasOneOverWorldUnits);
        vec4 macro = textureLod(u_colormap, uv, 0.0);
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
                // Reuse filtered coverage for a narrow contact edge. The
                // authored pad alpha supplies the bevel without neighbor taps.
                if ((cw & 0x01000000u) == 0u)
                    padEdgeAO = 1.0 - 0.72 * cOwnA * (1.0 - cOwnA);
                base = mix(base, cementColor, cOwnA);
                if (cOwnA > 0.5) { cementHit = true; pureConcrete = true; }
            } else if (isTransition) {
                if (uT.useTransitionMask != 0) {
                    // Static neighbor ownership was resolved when the cement words were cooked.
                    int cfg = int((cw >> 16) & 15u);
                    float u = cTileUV.x, v = cTileUV.y;
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
                    if (alpha > 0.5) { cementHit = true; pureConcrete = true; }
                }
            } else {
                base = cementColor;
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
                    pureConcrete = true;
                    cementHit = true;   // as GL's diag-connect branch
                }
            }
        }
    }

    // ★VK-TERRAIN-LIGHTING compose. Mirrors terrain_lod_chunk.frag:889-928 (the NdotL
    // relief band + baked static sun shadow, cement flat-lit). Terrain
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
    // One baked blend feeds tint, detail and shoreline affinity.
    float materialSnow = materials.z;
    vec4 materialW = vec4(max(1.0 - materials.x - materials.y - materials.w, 0.0), materials.x, materials.y, materials.w);
    vec3 baseNormal = mv_smoothTerrainNormal(v_worldPos.xy);
    base *= padEdgeAO;
    vec3 baseColor = base;
    float F3pc = pureConcrete ? 1.0 : 0.0;
    {
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

    // Uniform flow, close to use: avoid keeping the footprint live through cement/tint work.
    vec2 detailUv = v_worldPos.xy * (uT.detailTiling / MATV_WU_PER_TILE);
    vec2 detailUvDx = dFdx(detailUv), detailUvDy = dFdy(detailUv);
    vec3 lit = base;
    if (uT.lightingEnabled != 0) {
        vec3 lightDir = uT.terrainLightDir.xyz;
        // Colormaps are prelit: authored burnins, or sources baked by the loader.
        // Only detail relative to the macro normal may modulate that illumination.
        // Cement retains its authored slab normal and receives the same visibility.
        vec3 N = vec3(0.0, 0.0, 1.0);                            // flat-up terrain normal (default)
        bool resolveLampNormal = false;
        vec3 dbgBaseN = vec3(0.0, 0.0, 1.0); vec3 dbgDN = vec3(0.0);  // CSM-BLACKOUT-RECON-1 taps
        if (uT.detailEnabled != 0 && !pureConcrete) {
            vec3 baseN = baseNormal;
            vec3 dN    = vec3(0.0);
            if (uT.detailReady != 0 && uT.detailStrength != 0.0) {
                if (mv_needDetail(baseN, lightDir, materialW, materialSnow, v_worldPos, normalLightFloor)) {
                    dN = mv_detailNormal(materialW, materialSnow, v_worldPos.xy, baseN.z, detailUvDx, detailUvDy);
                } else {
                    resolveLampNormal = true;
                }
            }
            vec3 macroPert = vec3(baseN.xy / max(baseN.z, 0.2) * uT.nfhStrength, 1.0);
            vec3 pert = macroPert + vec3(dN.xy * uT.detailStrength, 0.0);
            N = normalize(pert);
            if (uT.detailReady >= 2) mv_cliffMaterial(baseN, N, baseColor);   // steep cells: triplanar normal + cliff POM + groove relief
            dbgBaseN = baseN; dbgDN = dN;                        // CSM-BLACKOUT-RECON-1 taps
        }
        // Evaluate the ratio after material sampling so its inputs do not stay
        // live across the detail/cliff texture walks. dbgBaseN already retains
        // the macro normal; flat receivers keep its default (0,0,1).
        vec3 macroN = normalize(vec3(dbgBaseN.xy / max(dbgBaseN.z, 0.2) * uT.nfhStrength, 1.0));
        float macroNdotL = max(dot(macroN, lightDir), normalLightFloor);
        // Equal normals give exactly one, including when detail is disabled.
        // Bound the grazing/back-facing ratio rather than re-lighting macro relief.
        float normalLight = clamp(max(dot(N, lightDir), normalLightFloor) / macroNdotL, 0.5, 1.5);

        // CONCRETE-ROAD-PBR-DETAIL: detail only (AO, relative sun shading, sheen), MC2 colour stays. Solid slabs only: those
        // fragments skip the material detail-normal fetches, so the two concrete fetches replace them (no net fetch increase).
        vec3 cdSpec = vec3(0.0);
        vec2 cdWpDx = dFdx(v_worldPos.xy), cdWpDy = dFdy(v_worldPos.xy);   // uniform flow, late: nothing extra stays live through the detail-normal block
        if (uT.concreteDetail != 0 && pureConcrete) cdApply(cdRoad > 0.5 ? CD_LAYER_ROAD : CD_LAYER_PAD, v_worldPos.xy, cdWpDx, cdWpDy, lightDir, 1.0, baseColor, normalLight, cdSpec, N);

        // Static ground mask and current-frame movers share the same visibility
        // floor. Cement receives both, exactly like the surrounding terrain.
        vec3 shadowN=baseNormal;
        float staticS=uT.enableShadows!=0?terrainStaticShadow(v_worldPos):1.0;
        float dynS=terrainDynamicShadow(v_worldPos,shadowN);
        float shadow = min(staticS, dynS);
        // CSM-BLACKOUT-RECON-1: one-shot debug viz (env MC2_VK_TERRAIN_CSM_DEBUG=N).
        // Resolve before local lighting so the debug inputs do not stay live through its loop.
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
        else if (uT.csmDebugMode == 9) { fragColor = vec4(dbgBaseN * 0.5 + 0.5, 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }
        else if (uT.csmDebugMode == 10) { fragColor = vec4(dbgDN * 0.5 + 0.5, 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }

        float normalSize = float(textureSize(u_matNormalArray, 0).x);
        float lampLod = resolveLampNormal ? 0.5 * log2(max(max(dot(detailUvDx, detailUvDx), dot(detailUvDy, detailUvDy))
                                                       * normalSize * normalSize, 1e-12)) : 0.0;
        lit = shadeGround(baseColor, N, normalLight, shadow, pureConcrete ? 1.0 : 0.0, v_worldPos, macroN, resolveLampNormal, lampLod);
        lit = max(lit + cdSpec * shadow, vec3(0.0));

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
            vec3  macroN     = baseNormal;
            float nzc        = clamp(abs(macroN.z), 0.0, 0.9995);
            float sinSlope   = max(sqrt(max(1.0 - nzc * nzc, 1e-6)), 0.03);
            float horizDist  = max(aboveWater, 0.0) * (nzc / sinSlope);
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
                vec4 slW = materialW; float slSnow = materialSnow;
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
        }
    }

    fragColor = vec4(lit, 1.0);   // opaque in-place (a=1, NOT premult)

    // ★TERRAIN shadow-class GBuffer1: flat-up normal + a=1.0 (handles own shadow).
    GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
}
