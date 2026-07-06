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
// VK-TERRAIN-LIGHTING sub-slice: make the VK terrain LIT (directional relief + baked
// static sun shadow) so it matches the GL LOD-chunk terrain instead of flat. Uses the
// REUSABLE lighting/shadow helper (props/mechs reuse it later). Gate MC2_VK_TERRAIN_LIGHTING
// (requires MC2_VK_TERRAIN_TIER1): OFF -> uT.lightingEnabled==0 -> the compose is the
// identity (base only), byte-identical to the pre-lighting colormap+cement frame.
#extension GL_GOOGLE_include_directive : require
#include "include/lighting_shadow_vk.glsl"

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
    // CEMENT/ROAD sub-slice: cement uniforms (mirror the GL LOD-chunk frag's
    // u_useCement/u_cementGridSide/u_cementWUPT/u_useTransitionMask/u_cementDiagConnect).
    int   useCement;        // 1 = cement atlas + cement-word snapshot both live
    int   cementGridSide;   // atlas tiles across
    float cementWUPT;       // world units per cement tile (== 128)
    int   useTransitionMask;// CEMENT-HARD-EDGE-1 gate (GL default-on)
    int   cementDiagConnect;// CEMENT-DIAG-CONNECT-1 gate (GL default-off)
    // VK-TERRAIN-LIGHTING sub-slice. std140: pad to a 16B boundary, then a vec4 sun
    // dir + a mat4 static light-space matrix (both 16-aligned) + scalar toggles.
    int   _lightPad0;       // off 116 -> pad so terrainLightDir starts at off 128 (vec4 align)
    int   _lightPad1;       // off 120
    int   _lightPad2;       // off 124
    vec4  terrainLightDir;  // off 128: sun dir (xyz), w unused (mirrors GL terrainLightDir)
    mat4  staticLightSpaceMatrix; // off 144: GL pp->getLightSpaceMatrix() (row-major GL_FALSE)
    int   lightingEnabled;  // off 208: MC2_VK_TERRAIN_LIGHTING gate (0 -> base only)
    int   enableShadows;    // off 212: static shadow map imported+valid this frame
    float shadowSoftness;   // off 216: penumbra radius in texels (GL default 2.5)
    int   _lightPad3;       // off 220
    // VK-TERRAIN-DETAIL sub-slice (extends VK-TERRAIN-LIGHTING). Perturb the terrain
    // surface normal like GL (terrain_lod_chunk.frag:740-808) so grass gets NdotL
    // relief instead of flat-lit -> closes the "grass ~3 levels darker" residual.
    //   detailEnabled : MC2_VK_TERRAIN_DETAIL gate (requires lightingEnabled). 0 ->
    //                   N stays flat-up -> byte-identical to the lighting-only frame.
    //   detailReady   : matNormalArray 2D_ARRAY imported+bound (else macro-slope only).
    //   nfhStrength / detailTiling / detailStrength : GL uniform values (all default
    //                   1.0). Per-material tiling/boost/class thresholds are baked as
    //                   consts below (GL default state; ImGui slider drift not tracked).
    int   detailEnabled;    // off 224
    int   detailReady;      // off 228
    float nfhStrength;      // off 232: terrainNormalsFromHeightStrength (GL default 1.0)
    float detailTiling;     // off 236: detailNormalTiling.x (GL default 1.0)
    float detailStrength;   // off 240: detailNormalStrength.x (GL default 1.0)
    // VK-TERRAIN-DYNSHADOW sub-slice: dynamic (building/mech) shadow. std140 pads the
    // dynamic light-space mat4 to off 256 (matches vulkan_terrain_island.cpp TerrainFrameUBO).
    int   enableDynShadows; // off 244: dynamic shadow map imported+valid this frame
    int   _dynPad0;         // off 248
    int   _dynPad1;         // off 252
    mat4  dynamicLightSpaceMatrix; // off 256
    // VK-TERRAIN-LODFADE sub-slice: camera world position (MC2 space) + the
    // lodFadeEnabled toggle, packed into the existing _pad[48] tail (off 320..511) so
    // the 512B dynamic-alignment slot does NOT grow. Mirrors GL gos_terrain.frag's
    // camDist/lodNear computation (:322-327) -- see terrain_tier1.vert/.frag comment
    // at the lodNear computation site below for the verbatim formula.
    vec4  cameraPosMC2;     // off 320: gos_terrain.frag's `cameraPos` (Stuff/MLR space)
    int   lodFadeEnabled;   // off 336: MC2_VK_TERRAIN_LODFADE gate (0 -> lodNear==1.0 always)
    float skirtDepth;       // off 340: VK-TERRAIN-SKIRT (read by the skirt vert only)
    // VK-TERRAIN-CSM-NEAR-1 sub-slice: near-cascade-array dynamic shadow. Ports GL's
    // calcDynamicShadow cascade-select VERBATIM (shadow.hglsl:191-238). LOCKSTEP with
    // vulkan_terrain_island.cpp TerrainFrameUBO (slot grew 512->1024B for this).
#define MC2_VK_SHADOW_CSM_MAX 3
    int   _csmPad0;                              // off 344
    int   _csmPad1;                               // off 348
    int   csmEnabled;                             // off 352: near-cascade path armed+valid this frame
    int   csmCount;                               // off 356: 1..MC2_VK_SHADOW_CSM_MAX
    float csmDepthSpan;                           // off 360
    int   csmDebugMode;                            // off 364: CSM-BLACKOUT-RECON-1 one-shot viz (0=off)
    mat4  csmCascadeMatrices[3];                  // off 368 (MC2_VK_SHADOW_CSM_MAX; literal for check_ubo_layout.py's regex)
    vec4  csmCascadeTexelWorld;                   // off 560 (xyz used, w pad)
    float csmFullMapTexelWorld;                   // off 576
} uT;

// VK-TERRAIN-DETAIL: the height SSBO (VK-owned snapshot; same buffer the VS reads at
// binding 1). The FS reads it to reproduce GL's smoothTerrainNormal() macro slope
// (terrain_lod_chunk.frag:152-173). Bilinear central difference -> +Z-up world normal.
layout(set = 0, binding = 1, std430) readonly buffer TerrainHeightBufFS {
    float heightsFS[];
} uHF;
// VK-TERRAIN-DETAIL: the material tangent-space detail-normal array (VK copy-once
// import of GL terrain_normal_array_tex_, a 9-layer RGBA8 GL_TEXTURE_2D_ARRAY,
// GL_REPEAT). Layers per include/terrain_mat_layers.hglsl. Dummy 1x1 until imported
// (uT.detailReady gates the sample).
layout(set = 0, binding = 6) uniform sampler2DArray u_matNormalArray;

// Material layer indices (mirror shaders/include/terrain_mat_layers.hglsl).
#define MAT_LAYER_ROCK     0
#define MAT_LAYER_GRASS    1
#define MAT_LAYER_DIRT     2
#define MAT_LAYER_CONCRETE 3
#define MAT_LAYER_SNOW     4

// GL default tuning (terrain_lod_chunk.frag uniforms; ImGui-driven, values are the
// gameos_graphics.cpp member defaults). Baked const for the parity oracle.
const vec4  MATV_NORMAL_BOOST = vec4(0.9, 0.5, 1.1, 2.5);  // rock,grass,dirt,conc
const vec4  MATV_TILING       = vec4(3.0, 2.0, 1.0, 6.0);  // rock,grass,dirt,conc
const float MATV_TILING_SNOW  = 1.0;
const vec4  MATV_CLASS_GRASS  = vec4(-0.02, 0.06, 0.22, 0.40); // gMinusR lo/hi, gBright lo/hi
const vec4  MATV_CLASS_DIRT   = vec4(-0.02, 0.06, 0.22, 0.45); // rMinusG lo/hi, rBright lo/hi
const float MATV_WU_PER_TILE  = 768.0;  // MAT_WORLD_UNITS_PER_TILE (terrain_lod_chunk.frag:240)

// --- GL smoothTerrainNormal port (terrain_lod_chunk.frag:145-173). ----------
float mv_heightAtCell(int cx, int cy) {
    cx = clamp(cx, 0, uT.mapSide - 1);
    cy = clamp(cy, 0, uT.mapSide - 1);
    return uHF.heightsFS[cx + cy * uT.mapSide];
}
float mv_heightBilinear(float fx, float fy) {
    int   x0 = int(floor(fx)), y0 = int(floor(fy));
    float tx = fx - float(x0), ty = fy - float(y0);
    float h00 = mv_heightAtCell(x0,     y0);
    float h10 = mv_heightAtCell(x0 + 1, y0);
    float h01 = mv_heightAtCell(x0,     y0 + 1);
    float h11 = mv_heightAtCell(x0 + 1, y0 + 1);
    return mix(mix(h00, h10, tx), mix(h01, h11, tx), ty);
}
vec3 mv_smoothTerrainNormal(vec2 worldXY) {
    float colF = (worldXY.x + uT.halfMap) / 128.0;
    float rowF = (uT.halfMap - worldXY.y) / 128.0;
    float span = 2.0 * 128.0;
    float dhdx   = (mv_heightBilinear(colF + 1.0, rowF) - mv_heightBilinear(colF - 1.0, rowF)) / span;
    float dhdrow = (mv_heightBilinear(colF, rowF + 1.0) - mv_heightBilinear(colF, rowF - 1.0)) / span;
    float dhdy   = -dhdrow;
    return normalize(vec3(-dhdx, -dhdy, 1.0));
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

// VK-TERRAIN-ANTITILE sub-slice: port the anti-tiling helpers from GL
// (terrain_lod_chunk.frag hash22 + sampleAntiTileArr, :274-294) VERBATIM. VK's
// mv_detailNormal runs in UNIFORM control flow (no per-fragment weight branch — the
// GL `if (w.* > 0.01)` branches are dropped here, all layers always accumulate), so
// implicit-LOD texture() is well-defined and the GL ddx/ddy → textureGrad plumbing
// (only needed for GL's non-uniform branches) is NOT required. Otherwise byte-for-byte
// the same math (hash offsets, smoothstep blend weights, normalize divisor).
vec2 mv_hash22(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}
vec4 mv_sampleAntiTileArr(int layer, vec2 uv, float scale) {
    vec2 off1 = mv_hash22(floor(uv / scale)) * scale;
    vec2 off2 = mv_hash22(floor(uv / scale) + vec2(7.0, 13.0)) * scale;
    vec4 s0 = texture(u_matNormalArray, vec3(uv,        float(layer)));
    vec4 s1 = texture(u_matNormalArray, vec3(uv + off1, float(layer)));
    vec4 s2 = texture(u_matNormalArray, vec3(uv + off2, float(layer)));
    vec2 f = fract(uv / scale);
    float w1 = smoothstep(0.2, 0.5, f.x) * smoothstep(0.2, 0.5, f.y);
    float w2 = smoothstep(0.2, 0.5, 1.0 - f.x) * smoothstep(0.2, 0.5, 1.0 - f.y);
    return (s0 + s1 * w1 + s2 * w2) / (1.0 + w1 + w2);
}

// VK-TERRAIN-POM sub-slice: port GL parallax-occlusion (terrain_lod_chunk.frag
// chunkSampleDisplacement + chunkParallax, :295-327) VERBATIM. GL pomParams is
// {terrain_pom_scale_, 8, 32, 0} = {0.02, 8, 32, 0} (gameos_graphics.cpp:7112 default,
// scale member :2456). Baked const (no new UBO field). Displacement = matNormalArray
// alpha channel (carried through VK's copy-once RGBA8 import). GL reads it via
// textureLod(...,0.0) — base level (POM standard), valid in any control flow. This is
// the full GL POM (fixed faux view dir, 16-layer bounded march); GL POM carries NO
// self-shadow term, so this is already the biggest-safe-subset (POM sans self-shadow).
const vec4 MATV_POM_PARAMS = vec4(0.02, 8.0, 32.0, 0.0);  // scale,minLayers,maxLayers,_
float mv_pomDisplacement(vec2 uv, vec4 w) {
    float d = 0.0;
    if (w.x > 0.01) d += w.x * textureLod(u_matNormalArray, vec3(uv, float(MAT_LAYER_ROCK)),     0.0).a;
    if (w.y > 0.01) d += w.y * textureLod(u_matNormalArray, vec3(uv, float(MAT_LAYER_GRASS)),    0.0).a;
    if (w.w > 0.01) d += w.w * textureLod(u_matNormalArray, vec3(uv, float(MAT_LAYER_CONCRETE)), 0.0).a;
    return 1.0 - d;
}
vec2 mv_parallax(vec2 uv, float scale, vec4 w) {
    const vec3 viewDirTS = vec3(0.15, 0.85, 0.15);
    float numLayers = mix(MATV_POM_PARAMS.y, MATV_POM_PARAMS.z, max(viewDirTS.y, 0.0));
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
    return mix(curUV, prevUV, after / (after - before));
}

// --- GL detail-normal accumulation (terrain_lod_chunk.frag chunkDetailNormal,
// :341-419). NOW PORTED (this sub-slice): per-layer weighted tangent-space normals
// (rock /3 boost, grass, dirt, ★concrete, snow) + ★per-layer fwidth sub-pixel fades
// (fwRock..fwSnow, GL :353-357) + ★anti-tile resample gated by `fw* > 0.5 && ats* >
// 0.01` EXACTLY as GL :392-415. Concrete layer (MAT_LAYER_CONCRETE, matTiling.w) was
// previously omitted — now included to match GL. ★POM (chunkParallax) NOW PORTED too
// (see mv_parallax below). DEFERRED vs GL (report): LOD-tier fade, painted-concrete normal, cliff
// triplanar. Uses implicit-LOD texture()/mv_sampleAntiTileArr (uniform control flow
// here — no non-uniform weight branch, so LOD is well-defined). -------------------
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

vec3 mv_detailNormal(vec4 w, float snowWeight, vec2 worldXY, float lodNear) {
    vec2 uv = worldXY * (uT.detailTiling / MATV_WU_PER_TILE);
    vec2 uvRock     = uv * (MATV_TILING.x / 3.0);
    vec2 uvGrass    = uv * MATV_TILING.y;
    vec2 uvDirt     = uv * MATV_TILING.z;
    vec2 uvConcrete = uv * MATV_TILING.w;
    vec2 uvSnow     = uv * MATV_TILING_SNOW;
    // Screen-space derivative AA (GL fwRock/fwGrass/...): fade a layer to 0 as its
    // tiling goes sub-pixel. Verbatim GL :353-357.
    float fwRock     = clamp(1.0 - (length(fwidth(uvRock))     - 0.5) * 2.0, 0.0, 1.0);
    float fwGrass    = clamp(1.0 - (length(fwidth(uvGrass))    - 0.5) * 2.0, 0.0, 1.0);
    float fwDirt     = clamp(1.0 - (length(fwidth(uvDirt))     - 0.5) * 2.0, 0.0, 1.0);
    float fwConcrete = clamp(1.0 - (length(fwidth(uvConcrete)) - 0.5) * 2.0, 0.0, 1.0);
    float fwSnow     = clamp(1.0 - (length(fwidth(uvSnow))     - 0.5) * 2.0, 0.0, 1.0);

    // ★POM: parallax-offset the material UVs (GL :361-367). fwidth-gated (near only)
    // -> bounded cost. pomParams.x (=0.02) > 0 always here; fwRock > 0.4 gate matches GL.
    // VK-TERRAIN-LODFADE: fade the POM offset by lodNear (GL gos_terrain.frag :617-624
    // `pomOffset *= lodNear` -- prevents popping at the LOD0/band boundary GL fades
    // across; lodNear==1.0 when the gate is off, so this is a no-op then).
    if (MATV_POM_PARAMS.x > 0.0 && fwRock > 0.4 && lodNear > 0.01) {
        const vec4 pomScaleMat = vec4(1.0, 1.0, 2.5, 1.0);
        float pomScale = MATV_POM_PARAMS.x * dot(pomScaleMat, w) * fwRock * lodNear;
        vec2  pomUV    = uv * dot(MATV_TILING, w);
        vec2  pomOff   = (mv_parallax(pomUV, pomScale, w) - pomUV) * fwRock * lodNear;
        uvRock += pomOff; uvGrass += pomOff; uvDirt += pomOff; uvConcrete += pomOff;
    }

    // Anti-tile scale per material (GL :382-385: tiling>=4 full, <=1 off). Near only.
    float atsRock  = mix(0.0, 3.0, clamp((MATV_TILING.x - 1.0) / 3.0, 0.0, 1.0));
    float atsGrass = mix(0.0, 3.0, clamp((MATV_TILING.y - 1.0) / 3.0, 0.0, 1.0));
    float atsDirt  = mix(0.0, 3.0, clamp((MATV_TILING.z - 1.0) / 3.0, 0.0, 1.0));
    float atsConc  = mix(0.0, 3.0, clamp((MATV_TILING.w - 1.0) / 3.0, 0.0, 1.0));

    vec3 dN = vec3(0.0);
    // rock: boost/3 (GL parity — rock detail was too dense/strong). GL :392-397.
    {
        vec4 s = (atsRock > 0.01 && fwRock > 0.5)
               ? mv_sampleAntiTileArr(MAT_LAYER_ROCK, uvRock, atsRock)
               : texture(u_matNormalArray, vec3(uvRock, float(MAT_LAYER_ROCK)));
        dN += w.x * (MATV_NORMAL_BOOST.x / 3.0) * fwRock * (s.rgb * 2.0 - 1.0);
    }
    {
        vec4 s = (atsGrass > 0.01 && fwGrass > 0.5)
               ? mv_sampleAntiTileArr(MAT_LAYER_GRASS, uvGrass, atsGrass)
               : texture(u_matNormalArray, vec3(uvGrass, float(MAT_LAYER_GRASS)));
        dN += w.y * MATV_NORMAL_BOOST.y * fwGrass * (s.rgb * 2.0 - 1.0);
    }
    {
        vec4 s = (atsDirt > 0.01 && fwDirt > 0.5)
               ? mv_sampleAntiTileArr(MAT_LAYER_DIRT, uvDirt, atsDirt)
               : texture(u_matNormalArray, vec3(uvDirt, float(MAT_LAYER_DIRT)));
        dN += w.z * MATV_NORMAL_BOOST.z * fwDirt * (s.rgb * 2.0 - 1.0);
    }
    {
        vec4 s = (atsConc > 0.01 && fwConcrete > 0.5)
               ? mv_sampleAntiTileArr(MAT_LAYER_CONCRETE, uvConcrete, atsConc)
               : texture(u_matNormalArray, vec3(uvConcrete, float(MAT_LAYER_CONCRETE)));
        dN += w.w * MATV_NORMAL_BOOST.w * fwConcrete * (s.rgb * 2.0 - 1.0);
    }
    dN += snowWeight * 0.9 * fwSnow *
          (texture(u_matNormalArray, vec3(uvSnow, float(MAT_LAYER_SNOW))).rgb * 2.0 - 1.0);
    // VK-TERRAIN-LODFADE: GL gos_terrain.frag :727-736 ties the WHOLE detail-normal
    // contribution to lodNear (detailMul), not just the POM offset -- stops the
    // detail-normal "carpet" from popping in/out at the LOD0/band boundary. lodNear
    // is 1.0 when the gate is off (byte-identical).
    return dN * lodNear;
}

// COLORMAP-BASE: the VK-owned copy-once import of the GL colormap atlas.
layout(set = 0, binding = 2) uniform sampler2D u_colormap;
// CEMENT/ROAD: the VK-owned copy-once import of the GL cement atlas (REPEAT sampler) +
// the map-load snapshot of the GL per-vertex cement-word SSBO (GL binding 25).
layout(set = 0, binding = 3) uniform sampler2D u_cementAtlas;
layout(set = 0, binding = 4, std430) readonly buffer TerrainCementBuf {
    uint cementWordsF[];
} uC;
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

float mv_csmSampleLayer(vec3 projCoords, int layer, vec3 worldPos, float NdotL, int numTaps) {
    float slope = max(1.0 - NdotL, 0.0);
    float texelBias = 1.5 * uT.csmCascadeTexelWorld[layer] / max(uT.csmDepthSpan, 1.0);
    float bias = max(0.0012 * slope, 0.0004) + texelBias;
    float currentDepth = projCoords.z - bias;

    float angle = 6.2831853 * fract(sin(dot(worldPos.xz, vec2(12.9898, 78.233))) * 43758.5453);
    float ca = cos(angle), sa = sin(angle);
    mat2 rot = mat2(ca, sa, -sa, ca);

    float depthGradient = length(vec2(dFdx(projCoords.z), dFdy(projCoords.z)));
    float hardness = clamp(depthGradient * 180.0, 0.0, 1.0);
    float adaptiveScale = mix(2.4, 0.8, hardness);

    vec2 texelSize = 1.0 / vec2(textureSize(u_dynamicShadowArray, 0).xy);
    float radius = max(uT.shadowSoftness, 0.5) * adaptiveScale;
    float shadow = 0.0;
    int taps = clamp(numTaps, 1, 16);
    for (int i = 0; i < taps; i++) {
        vec2 offset = rot * mv_poissonDisk[i] * radius * texelSize;
        shadow += texture(u_dynamicShadowArray, vec4(projCoords.xy + offset, float(layer), currentDepth));
    }
    shadow /= float(taps);
    return shadow;
}

float mv_csmSampleFullMap(vec3 projCoords, vec3 worldPos, float NdotL, int numTaps) {
    float slope = max(1.0 - NdotL, 0.0);
    float texelBias = 1.5 * uT.csmFullMapTexelWorld / max(uT.csmDepthSpan, 1.0);
    float bias = max(0.0012 * slope, 0.0004) + texelBias;
    float currentDepth = projCoords.z - bias;

    float angle = 6.2831853 * fract(sin(dot(worldPos.xz, vec2(12.9898, 78.233))) * 43758.5453);
    float ca = cos(angle), sa = sin(angle);
    mat2 rot = mat2(ca, sa, -sa, ca);

    float depthGradient = length(vec2(dFdx(projCoords.z), dFdy(projCoords.z)));
    float hardness = clamp(depthGradient * 180.0, 0.0, 1.0);
    float adaptiveScale = mix(2.4, 0.8, hardness);

    vec2 texelSize = 1.0 / vec2(textureSize(u_dynamicFullMapShadow, 0));
    float radius = max(uT.shadowSoftness, 0.5) * adaptiveScale;
    float shadow = 0.0;
    int taps = clamp(numTaps, 1, 16);
    for (int i = 0; i < taps; i++) {
        vec2 offset = rot * mv_poissonDisk[i] * radius * texelSize;
        shadow += texture(u_dynamicFullMapShadow, vec3(projCoords.xy + offset, currentDepth));
    }
    shadow /= float(taps);
    return shadow;
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
float mv_calcDynamicShadowCsm(vec3 worldPos, float NdotL, int numTaps) {
    int count = clamp(uT.csmCount, 1, MC2_VK_SHADOW_CSM_MAX);

    //PORT-BEGIN:calcDynamicShadowCsm
    for (int c = 0; c < count; ++c) {
        vec3 pc;
        if (!mv_csmProject(worldPos, c, pc)) continue;

        float vis = (c == count - 1)
                  ? mv_csmSampleFullMap(pc, worldPos, NdotL, numTaps)
                  : mv_csmSampleLayer(pc, c, worldPos, NdotL, numTaps);

        if (c + 1 < count) {
            vec2 d = min(pc.xy, 1.0 - pc.xy);
            float edge = min(d.x, d.y);
            const float band = 0.06;
            if (edge < band) {
                vec3 pc2;
                if (mv_csmProject(worldPos, c + 1, pc2)) {
                    float vis2 = (c + 1 == count - 1)
                               ? mv_csmSampleFullMap(pc2, worldPos, NdotL, numTaps)
                               : mv_csmSampleLayer(pc2, c + 1, worldPos, NdotL, numTaps);
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

void main() {
    vec3 base;
    // VK-TERRAIN-LIGHTING: track whether this fragment resolved to a solid cement slab.
    // GL treats pure concrete as a flat-lit slab (no NdotL relief) and drops the STATIC
    // self-shadow on it (terrain_lod_chunk.frag:894 + :902-907). We mirror that: on a
    // solid-cement hit -> flat-lit + no static shadow. (The GL dynamic building shadow on
    // concrete is a SEPARATE dynamic map, still deferred here.)
    bool pureConcrete = false;
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

    // ★CEMENT/ROAD sub-slice: concrete apron / roads. Ports the GL LOD-chunk cement
    // branch (terrain_lod_chunk.frag:484-624) VERBATIM: the solid-tile override, the
    // CEMENT-HARD-EDGE-1 marching-squares transition cut, and CEMENT-DIAG-CONNECT-1.
    // Fed by the per-vertex cement-word SSBO (uC) + the REPEAT-sampled cement atlas.
    // The overlay-sidecar composite (GL :626-642) is STILL DEFERRED (not fed here).
    // uT.useCement==0 (gate off / atlas or words not ready) -> whole block skipped ->
    // colormap base only, byte-identical to the colormap-only frame.
    if (uT.useCement != 0) {
        bool cementHit = false;
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
            vec2 cAtlasUV = (vec2(float(cCol), float(cRow)) + cTileUV) / float(cGridSide);
            vec3 cementColor = texture(u_cementAtlas, cAtlasUV).rgb;
            if (isTransition) {
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
                        alpha = (u + v < 0.5) ? 1.0 : 0.0;
                    } else if (cfg == 4) {        // cTR only
                        alpha = ((1.0 - u) + v < 0.5) ? 1.0 : 0.0;
                    } else if (cfg == 2) {        // cBR only
                        alpha = ((1.0 - u) + (1.0 - v) < 0.5) ? 1.0 : 0.0;
                    } else if (cfg == 1) {        // cBL only
                        alpha = (u + (1.0 - v) < 0.5) ? 1.0 : 0.0;
                    } else if (cfg == 7) {        // missing cTL
                        alpha = (u + v >= 0.5) ? 1.0 : 0.0;
                    } else if (cfg == 11) {       // missing cTR
                        alpha = ((1.0 - u) + v >= 0.5) ? 1.0 : 0.0;
                    } else if (cfg == 13) {       // missing cBR
                        alpha = ((1.0 - u) + (1.0 - v) >= 0.5) ? 1.0 : 0.0;
                    } else if (cfg == 14) {       // missing cBL
                        alpha = (u + (1.0 - v) >= 0.5) ? 1.0 : 0.0;
                    } else if (cfg == 12) {       // cTL+cTR, top
                        alpha = (v < 0.5) ? 1.0 : 0.0;
                    } else if (cfg == 6) {        // cTR+cBR, right
                        alpha = (u >= 0.5) ? 1.0 : 0.0;
                    } else if (cfg == 3) {        // cBR+cBL, bottom
                        alpha = (v >= 0.5) ? 1.0 : 0.0;
                    } else if (cfg == 9) {        // cBL+cTL, left
                        alpha = (u < 0.5) ? 1.0 : 0.0;
                    } else if (cfg == 10) {       // cTL+cBR
                        alpha = ((u + v < 0.5) || ((1.0 - u) + (1.0 - v) < 0.5)) ? 1.0 : 0.0;
                    } else if (cfg == 5) {        // cTR+cBL
                        alpha = (((1.0 - u) + v < 0.5) || (u + (1.0 - v) < 0.5)) ? 1.0 : 0.0;
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
                    vec2 dAtlasUV = (vec2(float(dCol), float(dRow)) + dTileUV) / float(dGridSide);
                    base = texture(u_cementAtlas, dAtlasUV).rgb;
                    pureConcrete = true;
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
        if (uT.detailEnabled != 0 && !pureConcrete) {
            vec3 baseN = mv_smoothTerrainNormal(v_worldPos.xy);  // macro slope (height SSBO)
            vec3 dN    = vec3(0.0);
            if (uT.detailReady != 0) {
                vec4  matW; float snowW;
                mv_weights(base, matW, snowW);                   // classify from colormap base
                // VK-TERRAIN-LODFADE: fades detail-normal/POM to 0 across the LOD0/band
                // boundary (GL gos_terrain.frag :326-328). Gate off -> lodNear==1.0.
                float lodNear = mv_computeLodNear(v_worldPos.xy, v_worldPos.z);
                dN = mv_detailNormal(matW, snowW, v_worldPos.xy, lodNear);
            }
            vec3 pert = vec3(baseN.xy / max(baseN.z, 0.2) * uT.nfhStrength
                             + dN.xy * uT.detailStrength, 1.0);
            N = normalize(pert);
        }
        vec3 lightDir = uT.terrainLightDir.xyz;

        float normalLight = computeNdotLBand(N, lightDir);
        if (pureConcrete) normalLight = 1.0;                     // cement: flat-lit slab

        // Shadow compose — mirrors terrain_lod_chunk.frag:896-907 EXACTLY:
        //   staticS = calcShadow(...16);  dynS = calcDynamicShadow(...8);
        //   shadow  = min(staticS, dynS);
        //   shadow  = mix(shadow, dynS, pureConcrete);   // concrete: dyn only, static dropped
        // GL callers pass shadowN = flat-up (0,0,1), NOT the perturbed N. computeBakedShadow
        // is a VERBATIM match of the legacy calcDynamicShadow body (adaptiveScale 2.4->0.8,
        // mix(0.4,1.0,shadow)); the GL static calcShadow uses the same PCF core here too.
        const vec3 shadowN = vec3(0.0, 0.0, 1.0);
        float staticS = 1.0;
        if (uT.enableShadows != 0) {
            staticS = computeBakedShadow(v_worldPos, shadowN, lightDir,
                                         u_staticShadow, uT.staticLightSpaceMatrix,
                                         uT.shadowSoftness, 16);
        }
        // VK-TERRAIN-CSM-NEAR-1: when the near-cascade-array path is armed+valid this
        // frame (uT.csmEnabled), use the ported cascade-select (mirrors GL sampling the
        // near cascade up close instead of the coarse full-map wash). Falls back to the
        // legacy single dynamic map otherwise (unchanged; MC2_VK_TERRAIN_CSM_FULLMAP_
        // EXPERIMENTAL escape hatch is untouched — it feeds enableDynShadows, not csmEnabled).
        float dynS = 1.0;
        if (uT.csmEnabled != 0) {
            float NdotLdyn = max(dot(shadowN, lightDir), 0.0);
            dynS = (NdotLdyn < 0.05) ? 1.0 : mv_calcDynamicShadowCsm(v_worldPos, NdotLdyn, 8);
        } else if (uT.enableDynShadows != 0) {
            dynS = computeBakedShadow(v_worldPos, shadowN, lightDir,
                                      u_dynamicShadow, uT.dynamicLightSpaceMatrix,
                                      uT.shadowSoftness, 8);
        }
        float shadow = min(staticS, dynS);
        shadow = mix(shadow, dynS, pureConcrete ? 1.0 : 0.0);  // concrete: dynamic only
        lit = base * normalLight * shadow;

        // CSM-BLACKOUT-RECON-1: one-shot debug viz (env MC2_VK_TERRAIN_CSM_DEBUG=N).
        // Bypasses the normal composite entirely so each channel can be inspected in
        // isolation on a live capture. No effect when uT.csmDebugMode==0.
        if (uT.csmDebugMode == 1)      { fragColor = vec4(vec3(normalLight), 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }
        else if (uT.csmDebugMode == 2) { fragColor = vec4(lightDir * 0.5 + 0.5, 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }
        else if (uT.csmDebugMode == 3) { fragColor = vec4(vec3(staticS), 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }
        else if (uT.csmDebugMode == 4) { fragColor = vec4(vec3(dynS), 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }
        else if (uT.csmDebugMode == 5) { fragColor = vec4(vec3(shadow), 1.0); GBuffer1 = vec4(0.5,0.5,1.0,1.0); return; }
    }

    fragColor = vec4(lit, 1.0);   // opaque in-place (a=1, NOT premult)

    // ★TERRAIN shadow-class GBuffer1: flat-up normal + a=1.0 (handles own shadow).
    GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
}
