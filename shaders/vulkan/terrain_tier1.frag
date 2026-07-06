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
#define MAT_LAYER_ROCK   0
#define MAT_LAYER_GRASS  1
#define MAT_LAYER_DIRT   2
#define MAT_LAYER_SNOW   4

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

// --- GL detail-normal accumulation (terrain_lod_chunk.frag chunkDetailNormal,
// :341-419). PORTED CORE: per-layer weighted tangent-space normals (rock /3 boost,
// grass, dirt, snow). DEFERRED vs GL (report): POM, anti-tile (sampleAntiTileArr),
// per-layer fwidth sub-pixel fade, LOD-tier fade, painted-concrete normal, cliff
// triplanar. Uses implicit-LOD texture() (uniform control flow here — no non-uniform
// weight branch, so LOD is well-defined). -----------------------------------------
vec3 mv_detailNormal(vec4 w, float snowWeight, vec2 worldXY) {
    vec2 uv = worldXY * (uT.detailTiling / MATV_WU_PER_TILE);
    vec2 uvRock  = uv * (MATV_TILING.x / 3.0);
    vec2 uvGrass = uv * MATV_TILING.y;
    vec2 uvDirt  = uv * MATV_TILING.z;
    vec2 uvSnow  = uv * MATV_TILING_SNOW;
    vec3 dN = vec3(0.0);
    // rock: boost/3 (GL parity — rock detail was too dense/strong).
    dN += w.x * (MATV_NORMAL_BOOST.x / 3.0) *
          (texture(u_matNormalArray, vec3(uvRock,  float(MAT_LAYER_ROCK))).rgb  * 2.0 - 1.0);
    dN += w.y * MATV_NORMAL_BOOST.y *
          (texture(u_matNormalArray, vec3(uvGrass, float(MAT_LAYER_GRASS))).rgb * 2.0 - 1.0);
    dN += w.z * MATV_NORMAL_BOOST.z *
          (texture(u_matNormalArray, vec3(uvDirt,  float(MAT_LAYER_DIRT))).rgb  * 2.0 - 1.0);
    dN += snowWeight * 0.9 *
          (texture(u_matNormalArray, vec3(uvSnow,  float(MAT_LAYER_SNOW))).rgb  * 2.0 - 1.0);
    return dN;
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
                dN = mv_detailNormal(matW, snowW, v_worldPos.xy);
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
        float dynS = 1.0;
        if (uT.enableDynShadows != 0) {
            dynS = computeBakedShadow(v_worldPos, shadowN, lightDir,
                                      u_dynamicShadow, uT.dynamicLightSpaceMatrix,
                                      uT.shadowSoftness, 8);
        }
        float shadow = min(staticS, dynS);
        shadow = mix(shadow, dynS, pureConcrete ? 1.0 : 0.0);  // concrete: dynamic only
        lit = base * normalLight * shadow;
    }

    fragColor = vec4(lit, 1.0);   // opaque in-place (a=1, NOT premult)

    // ★TERRAIN shadow-class GBuffer1: flat-up normal + a=1.0 (handles own shadow).
    GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
}
