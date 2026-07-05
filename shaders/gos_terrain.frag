//#version 400 (version provided by material prefix)

#define PREC highp

#include <include/shadow.hglsl>
#include <include/noise.hglsl>
#include <include/render_contract.hglsl>
#include <include/terrain_depth_bias.hglsl>  // single-sourced reverse-Z OVERLAY_DEPTH_BIAS (K7)
#include <include/edge_haze.hglsl>

// [RENDER_CONTRACT]
//   Pass:           TerrainBase
//   Color0:         RGBA color, opaque (water/shoreline blends)
//   GBuffer1:       rc_gbuffer1_shadowHandled / rc_gbuffer1_shadowHandled_flatUp
//                   for opaque terrain; rc_gbuffer1_legacyTerrainMaterialAlpha
//                   for water/shoreline (CONTRACT VIOLATION §3.1, F1)
//   ShadowContract: castsStatic=true, castsDynamic=false,
//                   skipsPostScreenShadow=true (binary-true for opaque;
//                   ambiguous for water — see §3.1)
//   StateContract:  depthTest=true, depthWrite=true, blend=Opaque,
//                   requiresMRT=true

in PREC vec4 Color;
in PREC vec2 Texcoord;
in PREC float TerrainType;
in PREC vec3 WorldNorm;
in PREC vec3 WorldPos;
in PREC float UndisplacedDepth;

layout (location=0) out PREC vec4 FragColor;
#ifdef MRT_ENABLED
layout (location=1) out PREC vec4 GBuffer1;
#endif

uniform sampler2D tex1;  // colormap
uniform sampler2D tex2;  // detail normal (engine default, fallback)
uniform sampler2D tex3;  // cement-catalog atlas (was: legacy detail displacement, unused with per-material POM).
                         // Bound at unit 3 by gos_terrain_bridge_drawIndirect when useCementAtlas != 0.
                         // Texture-object wrap = GL_REPEAT; the bridge clears unit-3 sampler with
                         // glBindSampler(3, 0) so the texture-object wrap is the contract.
                         // No mipmaps (cement atlas cells lack gutters; bleed risk).

// Per-material normal maps. Units assigned by kTerrainMatNormalUnits[] in
// gameos_graphics.cpp: matNormal0-3 = units 5-8, matNormal4 (snow) = unit 12.
// Alpha channel = displacement map for per-material POM.
uniform sampler2D matNormal0;  // rock     (unit 5)
uniform sampler2D matNormal1;  // grass    (unit 6)
uniform sampler2D matNormal2;  // dirt     (unit 7)
uniform sampler2D matNormal3;  // concrete (unit 8)
uniform sampler2D matNormal4;  // snow     (unit 12)

#include <include/terrain_mat_layers.hglsl>
#ifdef TERRAIN_NORMAL_ARRAY
// Array path: all 5 per-material normal maps packed into layers MAT_LAYER_*.
// Bound at unit kTerrainTexUnitNormalArray (= 5) by C++.
uniform sampler2DArray matNormalArray;
#endif

uniform PREC vec4 terrainLightDir;
uniform PREC vec4 detailNormalTiling;
uniform int u_pathTint;  // MC2_SHADER_PATH_TINT: 1 = solid signature colour (debug); 0 = normal
// Atlas-mode toggle for tex1 (colormap) sampling. When the indirect path binds
// the merged colormap atlas, we need atlas-absolute UV for tex1 sampling, but
// per-tile UV preserves the legacy frag shader's detail-tiling, anti-tile
// derivatives, POM ray-march, and matNormal mix math. AtlasUV is reconstructed
// in the frag from WorldPos (always available — set by all VS variants) using
// the atlas* uniforms. NOT a varying — that would break linking against the
// legacy non-thin VS chain which doesn't output AtlasUV. Bridge for indirect
// path sets useAtlasColormap=1 + the atlas uniforms; legacy paths leave
// useAtlasColormap=0 (default) and keep using per-tile Texcoord against their
// per-tile-bound textures.
uniform int   useAtlasColormap;
uniform float atlasMapTopLeftX;
uniform float atlasMapTopLeftY;
uniform float atlasOneOverWorldUnits;

// --- Cement catalog atlas (Stage 4 / PR2) ---------------------------------
// Bound at sampler unit 3 (tex3) by the indirect bridge.  Per-quad layer
// index + validity bit live in TerrainQuadThinRecord._pad0 (control.w),
// read from binding-2 SSBO indexed by the flat RecordIdx varying.
//
// _pad0 encoding (plan v2.1 V23, widened in V27):
//   bit 31     = CEMENT_LAYER_VALID
//   bits 30:16 = reserved for future layers
//   bits 15:0  = cement atlas layer index (0..65535 encoding cap;
//                practically capped at 1024 by atlas budget)
//
// C++ struct: GameOS/gameos/gos_terrain_patch_stream.h:103-111 (8×uint32, 32 B).
// std430 packs identically to 2×uvec4.  control.x=recipeIdx, .y=terrainHandle,
// .z=flags, .w=_pad0.  lightRGBs.{x,y,z,w}=lightRGB{0..3} (BGRA-packed).
uniform int   useCementAtlas;          // 0 = M2 / legacy, 1 = indirect cement-armed
uniform int   atlasCementGridSide;     // cells per row/col of cement atlas
uniform float atlasCementWorldUnitsPerTile;  // = Terrain::worldUnitsPerVertex (128.0)
// Stage B: transition mask array (14 layers, R8, procedural bilinear shapes).
uniform sampler2DArray u_transitionMaskArray;  // unit 4 (legacy path) or unit 4 (chunk)
uniform int            u_useTransitionMask;    // 0 = mask not yet ready, 1 = armed

flat in uint RecordIdx;

struct TerrainQuadThinRecord_Frag {
    uvec4 control;       // x=recipeIdx, y=terrainHandle, z=flags, w=cementWord
    uvec4 lightRGBs;
    // Fix B 2026-05-14: clipPos[4] declared here PURELY for std430 stride
    // lockstep with the four declarations of this struct.  The frag does
    // NOT read clipPos — that's the VS's job — but the struct size must
    // match or thinRecsFrag[i > 0] will read the wrong bytes for cement
    // layer-index decoding.  Silent-break risk #1 per adversarial review.
    vec4  clipPos[4];
};
// AMD L1-coherency: mirror the coherent qualifier on the compute writer
// (gpu_driven_terrain_solid.comp:118) and VS reader (gos_terrain_thin.vert:9).
// The frag reads `_pad0` (cement layer index) which lives in bytes 12-15 of
// the thin record; coherent here keeps the read symmetric with VS.
layout(std430, binding = 2) coherent readonly buffer ThinRecordBufFrag {
    TerrainQuadThinRecord_Frag thinRecsFrag[];
};

uniform PREC vec4 detailNormalStrength;
uniform PREC vec4 fog_color;
uniform PREC vec4 pomParams;
uniform PREC vec4 cameraPos;
uniform PREC vec4 terrainWorldScale;
uniform PREC vec4 terrainViewDir;
uniform PREC vec4 tessDebug;  // x=mode: 0=off, 1=normals, 2=worldPos
uniform float time;           // elapsed seconds (for cloud shadow animation)
uniform PREC float mapHalfExtent;  // half side length of playable map (0 = disabled)

// C1 tactical mission-gated material profile (see mclib/terrain.h for the
// C++ enum). Default 0 = LEGACY = exact pre-C1 classifier behavior.
// Non-zero values widen specific classifier windows for known-bad missions
// only. Disposable; removed when real material-palette architecture lands.
uniform int g_terrainMaterialProfile;

// Per-material and global color tuning (set via gos_SetTerrainMatNormalBoost /
// gos_SetTerrainTintStrengthScale). Defaults match the previous shader constants.
uniform PREC vec4  matNormalBoost;     // [rock, grass, dirt, concrete]; default (0.9, 1.1, 1.1, 2.5)
// Per-material UV tiling (set via gos_SetTerrainMatTiling). Grass default
// lowered from 12→2 to reduce excessive normal-map repetition at PBR zoom.
uniform PREC vec4  matTiling;          // [rock, grass, dirt, concrete]; default (3, 2, 1, 6)
uniform PREC float matTilingSnow;      // snow tiling; default 1.0
// TERRAIN-CLASSIFY-TUNING-1: colormap RGB channel-delta classifier thresholds.
// Sand_M24 profile widens the dirt gate via gos_SetTerrainClassDirt at mission start.
//   grass = (gMinusRLo, gMinusRHi, gBrightLo, gBrightHi): G-R delta + G brightness
//   dirt  = (rMinusGLo, rMinusGHi, rBrightLo, rBrightHi): R-G delta + R brightness
uniform PREC vec4  terrainClassGrass;  // default (-0.02, 0.06, 0.22, 0.40)
uniform PREC vec4  terrainClassDirt;   // default (-0.02, 0.06, 0.22, 0.45)
// TERRAIN-TINT-UI-1: material base tint colors (tunable via ImGui).
uniform PREC vec3  tintRock;   // default (0.36, 0.37, 0.40)
uniform PREC vec3  tintGrass;  // default (0.35, 0.42, 0.25)
uniform PREC vec3  tintDirt;   // default (0.48, 0.42, 0.33)
uniform PREC float tintStrengthScale;  // 0=colormap passthrough, 1=full material tint; default 1.0
uniform PREC float snowBrightnessDampen;  // <1 darkens detected snow (snowWeight-gated); default 0.78

// TERRAIN-NORMALS-FROM-HEIGHT-1 / TERRAIN-LIGHTING-1/2 uniforms +
// computeTerrainNormalFromHeight() helper. Shared with terrain_overlay.frag
// (cement transitions) via this include so the two shaders cannot drift.
// Default-OFF byte-equivalence is enforced CPU-side by force-zeroing the
// strength/factor uniforms when the matching env gate is unset.
#include <include/terrain_height_normal.hglsl>

// --- Distance LOD thresholds (tunable, in MC2 world units) ---
// 1 terrain tile ≈ 128 world units
const float LOD_NEAR       = 4000.0;   // full quality (covers stock zoom)
const float LOD_NEAR_FADE  = 4500.0;   // transition band end
const float LOD_MID        = 5500.0;   // mid quality
const float LOD_MID_FADE   = 6500.0;   // far quality begins
const PREC vec3 kLumaWeights = vec3(0.299, 0.587, 0.114);

// --- Hash / noise for cell bombing ---

PREC vec2 hash22(PREC vec2 p) {
    PREC vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

// Anti-tiling: blend 3 offset samples to break repetition without seams
PREC vec4 sampleAntiTile(sampler2D tex, PREC vec2 uv, PREC float scale) {
    PREC vec2 off1 = hash22(floor(uv / scale)) * scale;
    PREC vec2 off2 = hash22(floor(uv / scale) + vec2(7.0, 13.0)) * scale;

    PREC vec4 s0 = texture(tex, uv);
    PREC vec4 s1 = texture(tex, uv + off1);
    PREC vec4 s2 = texture(tex, uv + off2);

    // Blend weights from position within cell — always normalized to sum to 1
    PREC vec2 f = fract(uv / scale);
    PREC float w0 = 1.0;
    PREC float w1 = smoothstep(0.2, 0.5, f.x) * smoothstep(0.2, 0.5, f.y);
    PREC float w2 = smoothstep(0.2, 0.5, 1.0 - f.x) * smoothstep(0.2, 0.5, 1.0 - f.y);
    PREC float wTotal = w0 + w1 + w2;

    return (s0 * w0 + s1 * w1 + s2 * w2) / wTotal;
}

#ifdef TERRAIN_NORMAL_ARRAY
PREC vec4 sampleAntiTileArr(int layer, PREC vec2 uv, PREC float scale) {
    PREC vec2 off1 = hash22(floor(uv / scale)) * scale;
    PREC vec2 off2 = hash22(floor(uv / scale) + vec2(7.0, 13.0)) * scale;

    PREC vec3 uvl  = vec3(uv,        float(layer));
    PREC vec3 uvl1 = vec3(uv + off1, float(layer));
    PREC vec3 uvl2 = vec3(uv + off2, float(layer));

    PREC vec4 s0 = texture(matNormalArray, uvl);
    PREC vec4 s1 = texture(matNormalArray, uvl1);
    PREC vec4 s2 = texture(matNormalArray, uvl2);

    PREC vec2 f = fract(uv / scale);
    PREC float w0 = 1.0;
    PREC float w1 = smoothstep(0.2, 0.5, f.x) * smoothstep(0.2, 0.5, f.y);
    PREC float w2 = smoothstep(0.2, 0.5, 1.0 - f.x) * smoothstep(0.2, 0.5, 1.0 - f.y);
    PREC float wTotal = w0 + w1 + w2;

    return (s0 * w0 + s1 * w1 + s2 * w2) / wTotal;
}
#endif

// --- Color classification helpers ---

PREC vec3 rgb2hsv(PREC vec3 c) {
    PREC vec4 K = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);
    PREC vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    PREC vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    PREC float d = q.x - min(q.w, q.y);
    PREC float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

// MC2 terrain palette — RGB channel-delta classifier.
// grass = (gMinusRLo, gMinusRHi, gBrightLo, gBrightHi): G beats R → green tilt
// dirt  = (rMinusGLo, rMinusGHi, rBrightLo, rBrightHi): R beats G → warm tilt
// rock  = fallback; concrete weight comes from TerrainType (cement vertices) in main().
// Thresholds are uniforms (tunable via ImGui + visual_tuning.json).
PREC vec4 getColorWeights(PREC vec3 color) {
    PREC vec4 w = vec4(0.0);

    // Grass: G channel beats R (green tilt), sufficient green brightness
    PREC float gMinusR = color.g - color.r;
    w.y = smoothstep(terrainClassGrass.x, terrainClassGrass.y, gMinusR)
        * smoothstep(terrainClassGrass.z, terrainClassGrass.w, color.g);

    // Dirt: R channel beats G (warm tilt), sufficient red brightness
    PREC float rMinusG = color.r - color.g;
    w.z = smoothstep(terrainClassDirt.x, terrainClassDirt.y, rMinusG)
        * smoothstep(terrainClassDirt.z, terrainClassDirt.w, color.r);

    w.x = 1.0 - max(w.y, w.z);  // everything else → rock
    w.w = 0.0;

    // Water: both G and B exceed R (teal/cyan/blue tint) → push toward rock
    PREC float isWater = smoothstep(0.0, 0.08, min(color.g, color.b) - color.r);
    w.x += isWater;
    w.y *= (1.0 - isWater);
    w.z *= (1.0 - isWater);

    PREC float total = w.x + w.y + w.z + w.w;
    w = (total < 0.01) ? vec4(1.0, 0.0, 0.0, 0.0) : w / total;
    return w;
}

// --- Per-material displacement sampling ---

const PREC vec4 pomScaleMat = vec4(1.0, 1.0, 2.5, 1.0);  // rock doubled 0.5→1.0 for stronger displacement

PREC float sampleDisplacement(PREC vec2 uv, PREC vec4 weights) {
    PREC float d = 0.0;
#ifdef TERRAIN_NORMAL_ARRAY
    if (weights.x > 0.01) d += weights.x * texture(matNormalArray, vec3(uv, float(MAT_LAYER_ROCK))).a;
    if (weights.y > 0.01) d += weights.y * texture(matNormalArray, vec3(uv, float(MAT_LAYER_GRASS))).a;
    if (weights.z > 0.01) d += weights.z * 1.0;  // dirt: blank alpha (no POM shift)
    if (weights.w > 0.01) d += weights.w * texture(matNormalArray, vec3(uv, float(MAT_LAYER_CONCRETE))).a;
#else
    if (weights.x > 0.01) d += weights.x * texture(matNormal0, uv).a;
    if (weights.y > 0.01) d += weights.y * texture(matNormal1, uv).a;
    if (weights.z > 0.01) d += weights.z * 1.0;  // dirt: blank alpha (no POM shift)
    if (weights.w > 0.01) d += weights.w * texture(matNormal3, uv).a;
#endif
    return 1.0 - d;
}

// --- POM ---

PREC vec2 parallaxMapping(PREC vec2 uv, PREC vec3 viewDirTS, PREC float scale, PREC vec4 matWeights)
{
    PREC float numLayers = mix(pomParams.z, pomParams.y, max(viewDirTS.y, 0.0));
    PREC float layerDepth = 1.0 / numLayers;
    PREC float currentLayerDepth = 0.0;
    PREC vec2 P = viewDirTS.xz / max(viewDirTS.y, 0.001) * scale;
    PREC vec2 deltaUV = P / numLayers;
    PREC vec2 currentUV = uv;
    PREC float currentDepth = sampleDisplacement(currentUV, matWeights);
    for (int i = 0; i < 64; i++) {
        if (currentLayerDepth >= currentDepth) break;
        currentUV -= deltaUV;
        currentDepth = sampleDisplacement(currentUV, matWeights);
        currentLayerDepth += layerDepth;
    }
    PREC vec2 prevUV = currentUV + deltaUV;
    PREC float afterDepth = currentDepth - currentLayerDepth;
    PREC float beforeDepth = sampleDisplacement(prevUV, matWeights) - currentLayerDepth + layerDepth;
    // Guard against zero denominator when consecutive samples are identical;
    // AMD silently produces inf/nan but NVIDIA may trap — use 1e-6 epsilon.
    PREC float weight = afterDepth / max(abs(afterDepth - beforeDepth), 1.0e-6);
    return mix(currentUV, prevUV, weight);
}

void main(void)
{
    // MC2_SHADER_PATH_TINT: solid RED so this shader's surfaces are unmistakable.
    if (u_pathTint != 0) {
        FragColor = vec4(1.0, 0.0, 0.0, 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }
    // Debug visualizations for tessellation data
    // Distance-based LOD factors (1.0 = full quality, 0.0 = cheapest)
    // cameraPos is in Stuff/MLR space: .x=left/right, .y=elevation, .z=forward
    // WorldPos is in raw MC2 space: .x=east, .y=north, .z=elevation
    // LOD center: camera ground position with altitude boost for zoom-out
    vec2 camGround = vec2(-cameraPos.x, cameraPos.z);
    float hDist = distance(WorldPos.xy, camGround);
    float altBoost = max(cameraPos.y - WorldPos.z, 0.0) * 0.7;
    float camDist = hDist + altBoost;
    float lodNear = 1.0 - smoothstep(LOD_NEAR, LOD_NEAR_FADE, camDist);
    float lodMid  = 1.0 - smoothstep(LOD_MID,  LOD_MID_FADE,  camDist);

    // Screen-space world-unit footprint of one pixel — fwidth-based.
    // Per-triangle-constant (linear varying), so it creates step bands at
    // tessellation LOD boundaries. Safe for FBM/rock (low tiling, subtle fade).
    PREC float worldPixelSize = length(fwidth(WorldPos.xy));

    // FBM breakup fade: scale 0.018 ≈ 1 cycle / 56 WU.
    PREC float breakupFade = 1.0 - smoothstep(0.3, 0.8, worldPixelSize * 0.018);

    // Smooth camera-distance proxy for grass fade — avoids tessellation LOD
    // chunk boundaries that fwidth(WorldPos) would expose at 12x tiling.
    // grassPixelScale 0.0015: tune up to sharpen close grass, down for softer.
    PREC float worldPixelSizeSmooth = camDist * 0.0015;
    PREC float grassFreq = worldPixelSizeSmooth * 0.094;

    // Wider fade band (0.20..1.20 vs 0.3..0.8) so the grass normal transition
    // dissolves gradually rather than crossing the threshold in a tight visible band.
    PREC float grassNormalFade = 1.0 - smoothstep(0.20, 1.20, grassFreq);

    // Legacy probe: reserve negative values for an unconditional "tess frag is running"
    // visual. Positive values are surface debug modes and must flow through normally.
    if (tessDebug.x < -0.5) {
        gl_FragDepth = gl_FragCoord.z;
        FragColor = vec4(1.0, 0.0, 0.0, 1.0);  // SOLID RED = tess frag running
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }

    // Debug mode 8 (Alt+1): cement-word diagnostic visualization.
    // Reads per-quad cementWord BEFORE the override branch runs, so we can see
    // exactly what reaches the fragment shader independent of useCementAtlas
    // gating or downstream UV/tex3 math.  Channels are independent answers:
    //   R = 1.0 if CEMENT_LAYER_VALID bit set (the validity flag arrived intact)
    //   G = (cementWord & 0xFF) / 255  (low byte of layer index — modulation
    //                                   confirms the index varies per quad)
    //   B = 1.0 if useCementAtlas == 0 (cement uniform plumbing inactive —
    //                                   tells "uniform off" from "word zero")
    // Triage at a glance:
    //   black on cement pads      → cementWord = 0 reaching frag (upstream)
    //   solid blue                → useCementAtlas uniform not set (bridge)
    //   red(+green) on pads only  → pipeline OK, bug is downstream (UV/tex3)
    if (tessDebug.x > 7.5 && tessDebug.x < 8.5) {
        uint cw = thinRecsFrag[RecordIdx].control.w;
        float dR = ((cw & 0x80000000u) != 0u) ? 1.0 : 0.0;
        float dG = float(cw & 0xFFu) / 255.0;
        float dB = (useCementAtlas == 0) ? 1.0 : 0.0;
        gl_FragDepth = gl_FragCoord.z;
        FragColor = vec4(dR, dG, dB, 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }

    // Debug mode 9: thin-record control-channel diagnostic.  Complement to
    // mode 8 — probes whether the compute shader's *other* writes to the
    // thin record reach the frag at all.  If mode 8 is black but mode 9
    // modulates, the cementWord write is the specific failure.  If both are
    // black, the entire thin-record handoff (compute → ring slot → frag SSBO
    // binding) is broken on this draw.
    //   R = (recipeIdx & 0xFF) / 255  — compute writes recipeIdx unconditionally
    //   G = (flags     & 0xFF) / 255  — compute writes flags unconditionally
    //   B = (terrainHandle & 0xFF)/255 — compute writes terrainHandle (per-quad)
    // Any non-zero channel = compute is alive and the thin-record buffer is
    // the right buffer.  All-zero = wrong buffer or compute not running.
    if (tessDebug.x > 8.5 && tessDebug.x < 9.5) {
        uvec4 c = thinRecsFrag[RecordIdx].control;
        float dR = float(c.x & 0xFFu) / 255.0;
        float dG = float(c.z & 0xFFu) / 255.0;
        float dB = float(c.y & 0xFFu) / 255.0;
        gl_FragDepth = gl_FragCoord.z;
        FragColor = vec4(dR, dG, dB, 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }

    // Atlas-mode tex1 sampling: when the indirect bridge has bound the merged
    // colormap atlas, reconstruct atlas-absolute UV from WorldPos (always set
    // by all VS variants — safer than a varying that the legacy non-thin VS
    // chain doesn't emit, which would break linking against gos_terrain.frag).
    // Per-tile Texcoord remains the source for detail tiling / anti-tile /
    // POM / matNormal mix, preserving legacy frag math.
    PREC vec2 colormapUV;
    if (useAtlasColormap != 0) {
        colormapUV.x = (WorldPos.x - atlasMapTopLeftX) * atlasOneOverWorldUnits;
        colormapUV.y = (atlasMapTopLeftY - WorldPos.y) * atlasOneOverWorldUnits;
    } else {
        colormapUV = Texcoord;
    }
    PREC vec4 texColor = texture(tex1, colormapUV);

    // Cement-catalog override: gated on useCementAtlas (M2/legacy never set this)
    // AND the validity bit in the per-quad cement word.  TerrainType is NOT a
    // gate — alpha-cement boundary fragments can interpolate TerrainType near 3.0
    // while their _pad0 has no validity bit set, and we must NOT sample cement[0]
    // for those (advisor C2).  When validity bit is set, the quad is genuinely
    // pure-cement and TerrainType is exactly 3.0 across all corners.
    //
    // cementWord bit layout (Stage B extension):
    //   bit 31     = CEMENT_VALID
    //   bit 30     = IS_TRANSITION
    //   bits 29:24 = maskId (0..13) — index into u_transitionMaskArray
    //   bits 15:0  = cementAtlas layerIdx (unchanged)
    if (useCementAtlas != 0) {
        uint cementWord    = thinRecsFrag[RecordIdx].control.w;
        bool cementValid   = (cementWord & 0x80000000u) != 0u;
        bool isTransition  = (cementWord & 0x40000000u) != 0u;
        if (cementValid) {
            uint layerIdx = cementWord & 0xFFFFu;
            int  gridSide = atlasCementGridSide;
            if (gridSide < 1) gridSide = 1;
            int  cCol = int(layerIdx) % gridSide;
            int  cRow = int(layerIdx) / gridSide;
            PREC vec2 cTileUV  = fract(vec2(WorldPos.x, -WorldPos.y) / atlasCementWorldUnitsPerTile);
            PREC vec2 cAtlasUV = (vec2(float(cCol), float(cRow)) + cTileUV) / float(gridSide);
            PREC vec4 cementColor = texture(tex3, cAtlasUV);
            if (isTransition) {
                // Transition: legacy overlay draw handles cement blend. Shader pass-through.
            } else {
                // Solid cement quad — full replacement (original behavior).
                texColor = cementColor;
            }
        }
    }
    PREC float waterFlag = smoothstep(0.35, 0.45, rgb2hsv(texColor.rgb).x);
    PREC float materialAlpha = mix(1.0, 0.25, waterFlag);

#ifdef ALPHA_TEST
    if(texColor.a < 0.5)
        discard;
#endif

    int surfaceDebugMode = int(floor(tessDebug.x + 0.5));
    PREC vec4 c = Color.bgra;
    PREC float vertexLum = dot(c.rgb, kLumaWeights);

    // TERRAIN-NORMALS-FROM-HEIGHT-1: debug mode 10 — visualize the
    // height-derived normal as RGB. terrainHeightParams.x > 0 means the
    // per-mission R32F height texture has been uploaded; black otherwise.
    // Independent of the useTerrainNormalsFromHeight gate so the upload
    // path can be diagnosed even with the lighting gate off.
    if (surfaceDebugMode == 10) {
        PREC vec3 vis = vec3(0.0);
        if (terrainHeightParams.x > 0.5) {
            PREC vec3 hN = computeTerrainNormalFromHeight(WorldPos.xy);
            vis = hN * 0.5 + 0.5;
        }
        gl_FragDepth = gl_FragCoord.z;
        FragColor = vec4(vis, 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }

    // Debug mode 1: depth diagnostic — R=actual rasterized, G=UndisplacedDepth
    // Toggle RAlt+0 to compare old screen-space vs new world-space path.
    // Green > Red = UndisplacedDepth is deeper (correct). Red > Green = potential z-fighting.
    if (surfaceDebugMode == 1) {
        float actual = gl_FragCoord.z;
        float undis  = UndisplacedDepth;
        // Amplify to [0,1] range (terrain depth typically near 1.0, differences small)
        float lo = 0.85;
        float hi = 1.0;
        float r = clamp((actual - lo) / (hi - lo), 0.0, 1.0);
        float g = clamp((undis  - lo) / (hi - lo), 0.0, 1.0);
        FragColor = vec4(r, g, 0.0, 1.0);
        gl_FragDepth = actual;  // don't override depth in diagnostic mode
        return;
    }

    // Debug mode 2: show raw terrain colormap everywhere.
    if (surfaceDebugMode == 2) {
        gl_FragDepth = gl_FragCoord.z;
        FragColor = vec4(texColor.rgb, 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }

    // Smooth colormap classification — tiered by distance
    // Near: 9-tap disc, Mid: 5-tap cross, Far: 1-tap (no blur)
    const PREC float blurRadius = 0.18;
    PREC float r2 = blurRadius * 0.707;
    const PREC float uvMargin = 0.005;
    PREC vec2 uvMin = vec2(uvMargin);
    PREC vec2 uvMax = vec2(1.0 - uvMargin);

    PREC vec3 colAvg;
    if (lodMid < 0.01) {
        // Far: no blur — single sample
        colAvg = texColor.rgb;
    } else if (lodNear < 0.01) {
        // Mid: 5-tap cross only (skip diagonals)
        colAvg = texColor.rgb;
        colAvg += texture(tex1, clamp(colormapUV + vec2( blurRadius, 0.0), uvMin, uvMax)).rgb;
        colAvg += texture(tex1, clamp(colormapUV + vec2(-blurRadius, 0.0), uvMin, uvMax)).rgb;
        colAvg += texture(tex1, clamp(colormapUV + vec2(0.0,  blurRadius), uvMin, uvMax)).rgb;
        colAvg += texture(tex1, clamp(colormapUV + vec2(0.0, -blurRadius), uvMin, uvMax)).rgb;
        colAvg /= 5.0;
    } else {
        // Near: full 9-tap disc
        colAvg = texColor.rgb;
        colAvg += texture(tex1, clamp(colormapUV + vec2( blurRadius, 0.0), uvMin, uvMax)).rgb;
        colAvg += texture(tex1, clamp(colormapUV + vec2(-blurRadius, 0.0), uvMin, uvMax)).rgb;
        colAvg += texture(tex1, clamp(colormapUV + vec2(0.0,  blurRadius), uvMin, uvMax)).rgb;
        colAvg += texture(tex1, clamp(colormapUV + vec2(0.0, -blurRadius), uvMin, uvMax)).rgb;
        colAvg += texture(tex1, clamp(colormapUV + vec2( r2,  r2), uvMin, uvMax)).rgb;
        colAvg += texture(tex1, clamp(colormapUV + vec2(-r2,  r2), uvMin, uvMax)).rgb;
        colAvg += texture(tex1, clamp(colormapUV + vec2( r2, -r2), uvMin, uvMax)).rgb;
        colAvg += texture(tex1, clamp(colormapUV + vec2(-r2, -r2), uvMin, uvMax)).rgb;
        colAvg /= 9.0;
    }
    if (surfaceDebugMode == 3) {
        gl_FragDepth = gl_FragCoord.z;
        FragColor = vec4(colAvg, 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }
    PREC vec4 matWeights = getColorWeights(colAvg);

    // Snow weight: low-sat, mid-to-high-value colormap pixels (white-ish).
    // Wider raw gate catches blur-softened snow edges; sharpen step pushes decisive
    // snow pixels to full strength so snow areas don't fade through a grey no-mans-land.
    PREC vec3 hsvAvg = rgb2hsv(colAvg);
    PREC float snowRaw = smoothstep(0.15, 0.03, hsvAvg.y) * smoothstep(0.42, 0.62, hsvAvg.z);
    PREC float snowWeight = smoothstep(0.25, 0.55, snowRaw);
    // TerrainType: discrete 0/1/2/3 at vertices, interpolated by TES.
    // Boundary patches (cement+terrain vertex mix) have fragments with TerrainType in (2,3).
    // Pure terrain tiles never exceed TerrainType=2, so smoothstep(2.0,3.0) only activates
    // for fragments near cement vertices — blending concrete material into edge patches
    // without touching distant terrain tiles.
    PREC float pureConcrete = smoothstep(2.0, 3.0, TerrainType);
    // Use a stronger curve for color than for material/normal blending so boundary tiles
    // keep the smooth transition shape but visually track the pure cement tone more closely.
    PREC float concreteColorBlend = sqrt(clamp(pureConcrete, 0.0, 1.0));

    matWeights = mix(matWeights, vec4(0.0, 0.0, 0.0, 1.0), pureConcrete);
    // Snow is suppressed on cement tiles (pureConcrete dominates there).
    snowWeight *= (1.0 - pureConcrete);
    // Snow steals from the other weights proportionally so the total across all 5 = 1.
    matWeights *= (1.0 - snowWeight);

    PREC float totalWeights = matWeights.x + matWeights.y + matWeights.z + matWeights.w;
    if (totalWeights > 0.01) {
        matWeights /= totalWeights;
    } else {
        matWeights = vec4(1.0, 0.0, 0.0, 0.0);
    }

    // Far tier: keep only 2 strongest materials to halve normal map samples
    if (lodMid < 0.01) {
        float maxW = max(max(matWeights.x, matWeights.y), max(matWeights.z, matWeights.w));
        vec4 mask = step(maxW * 0.5, matWeights);
        matWeights *= mask;
        float total = matWeights.x + matWeights.y + matWeights.z + matWeights.w;
        if (total > 0.01) matWeights /= total;
    }

    if (surfaceDebugMode == 4) {
        gl_FragDepth = gl_FragCoord.z;
        FragColor = vec4(matWeights.x, matWeights.y, matWeights.z, 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }

    // Per-material tiling (rock, grass, dirt/riverbed, concrete).
    // Values are C++-tunable uniforms (gos_SetTerrainMatTiling / ImGui "Per-Material Tiling").
    // Grass was 12.0 historically; default lowered to 2.0 to match visual expectations.
    PREC float baseTiling = detailNormalTiling.x;

    // Compute per-material UVs (straight tiling, anti-tiling done at sample time)
    PREC vec2 uvRock     = Texcoord * baseTiling * matTiling.x;
    PREC vec2 uvGrass    = Texcoord * baseTiling * matTiling.y;
    PREC vec2 uvDirt     = Texcoord * baseTiling * matTiling.z;
    PREC vec2 uvConcrete = Texcoord * baseTiling * matTiling.w;
    PREC vec2 uvSnow     = Texcoord * baseTiling * matTilingSnow;

    // POM — full at near, off at far (lodNear fades 1→0)
    if (pomParams.x > 0.0 && lodNear > 0.01) {
        PREC float effectivePomScale = pomParams.x * dot(pomScaleMat, matWeights);
        PREC vec3 viewDir = normalize(vec3(0.15, 0.85, 0.15));
        PREC float pomTiling = dot(matTiling, matWeights);
        PREC vec2 pomUV = Texcoord * baseTiling * pomTiling;
        PREC vec2 pomOffset = parallaxMapping(pomUV, viewDir, effectivePomScale * lodNear, matWeights) - pomUV;
        // Fade POM offset to zero at distance boundary (prevents popping)
        pomOffset *= lodNear;
        uvRock += pomOffset;
        uvGrass += pomOffset;
        uvDirt += pomOffset;
        uvConcrete += pomOffset;
    }

    // Per-material normal strength
    // Effective strength = normalBoost * detailNormalStrength.x (4.0 from C++)
    // Rock 1.3→0.9, grass 1.5→1.1: over-boost was producing grain-like noise at RTS zoom.
    // Dirt 1.1 is the "looks fantastic" reference — do not change it.
    // Concrete 2.5 unchanged — flat surfaces benefit from strong normal definition.
    // Non-const: normalBoost.y is scaled below by the combined grass fade.
    PREC vec4 normalBoost = matNormalBoost;  // tunable via ImGui / gos_SetTerrainMatNormalBoost

    // Screen-space derivative AA — fade normals when detail goes sub-pixel
    PREC float fwRock     = clamp(1.0 - (length(fwidth(uvRock))     - 0.5) * 2.0, 0.0, 1.0);
    PREC float fwGrass    = clamp(1.0 - (length(fwidth(uvGrass))    - 0.5) * 2.0, 0.0, 1.0);
    PREC float fwDirt     = clamp(1.0 - (length(fwidth(uvDirt))     - 0.5) * 2.0, 0.0, 1.0);
    PREC float fwConcrete = clamp(1.0 - (length(fwidth(uvConcrete)) - 0.5) * 2.0, 0.0, 1.0);

    // Combine UV-derivative AA with world-space frequency fade for grass.
    // fwGrass catches sub-pixel UV aliasing; grassNormalFade catches projected
    // frequency grain at grazing angles regardless of zoom or LOD tier.
    normalBoost.y *= fwGrass * grassNormalFade;

    // Anti-tile scale — proportional to tiling so low-tiling materials skip it
    // At tiling >= 4, full anti-tiling (3.0); at tiling <= 1, plain sampling
    PREC float atsRock     = mix(0.0, 3.0, clamp((matTiling.x - 1.0) / 3.0, 0.0, 1.0));
    PREC float atsGrass    = mix(0.0, 3.0, clamp((matTiling.y - 1.0) / 3.0, 0.0, 1.0));
    PREC float atsDirt     = mix(0.0, 3.0, clamp((matTiling.z - 1.0) / 3.0, 0.0, 1.0));
    PREC float atsConcrete = mix(0.0, 3.0, clamp((matTiling.w - 1.0) / 3.0, 0.0, 1.0));

    // Sample each material — anti-tiled when near, plain texture when far
    PREC vec3 detailN = vec3(0.0);
    PREC vec4 matSample;
    bool useAntiTile = (lodNear > 0.01);

#ifdef TERRAIN_NORMAL_ARRAY
    if (matWeights.x > 0.01) {
        matSample = (useAntiTile && atsRock > 0.01)
            ? sampleAntiTileArr(MAT_LAYER_ROCK, uvRock, atsRock)
            : texture(matNormalArray, vec3(uvRock, float(MAT_LAYER_ROCK)));
        detailN += matWeights.x * normalBoost.x * fwRock * (matSample.rgb * 2.0 - 1.0);
    }
    if (matWeights.y > 0.01) {
        matSample = (useAntiTile && atsGrass > 0.01)
            ? sampleAntiTileArr(MAT_LAYER_GRASS, uvGrass, atsGrass)
            : texture(matNormalArray, vec3(uvGrass, float(MAT_LAYER_GRASS)));
        detailN += matWeights.y * normalBoost.y * (matSample.rgb * 2.0 - 1.0);
    }
    if (matWeights.z > 0.01) {
        matSample = (useAntiTile && atsDirt > 0.01)
            ? sampleAntiTileArr(MAT_LAYER_DIRT, uvDirt, atsDirt)
            : texture(matNormalArray, vec3(uvDirt, float(MAT_LAYER_DIRT)));
        detailN += matWeights.z * normalBoost.z * fwDirt * (matSample.rgb * 2.0 - 1.0);
    }
    if (matWeights.w > 0.01) {
        matSample = (useAntiTile && atsConcrete > 0.01)
            ? sampleAntiTileArr(MAT_LAYER_CONCRETE, uvConcrete, atsConcrete)
            : texture(matNormalArray, vec3(uvConcrete, float(MAT_LAYER_CONCRETE)));
        detailN += matWeights.w * normalBoost.w * fwConcrete * (matSample.rgb * 2.0 - 1.0);
    }
    if (snowWeight > 0.01) {
        PREC vec4 snowSample = texture(matNormalArray, vec3(uvSnow, float(MAT_LAYER_SNOW)));
        detailN += snowWeight * 0.9 * (snowSample.rgb * 2.0 - 1.0);
    }
#else
    if (matWeights.x > 0.01) {
        matSample = (useAntiTile && atsRock > 0.01) ? sampleAntiTile(matNormal0, uvRock, atsRock) : texture(matNormal0, uvRock);
        detailN += matWeights.x * normalBoost.x * fwRock * (matSample.rgb * 2.0 - 1.0);
    }
    if (matWeights.y > 0.01) {
        matSample = (useAntiTile && atsGrass > 0.01) ? sampleAntiTile(matNormal1, uvGrass, atsGrass) : texture(matNormal1, uvGrass);
        detailN += matWeights.y * normalBoost.y * (matSample.rgb * 2.0 - 1.0);
    }
    if (matWeights.z > 0.01) {
        matSample = (useAntiTile && atsDirt > 0.01) ? sampleAntiTile(matNormal2, uvDirt, atsDirt) : texture(matNormal2, uvDirt);
        detailN += matWeights.z * normalBoost.z * fwDirt * (matSample.rgb * 2.0 - 1.0);
    }
    if (matWeights.w > 0.01) {
        matSample = (useAntiTile && atsConcrete > 0.01) ? sampleAntiTile(matNormal3, uvConcrete, atsConcrete) : texture(matNormal3, uvConcrete);
        detailN += matWeights.w * normalBoost.w * fwConcrete * (matSample.rgb * 2.0 - 1.0);
    }
    if (snowWeight > 0.01) {
        PREC vec4 snowSample = texture(matNormal4, uvSnow);
        detailN += snowWeight * 0.9 * (snowSample.rgb * 2.0 - 1.0);
    }
#endif
    // Cement normal: suppress the regular detail normal in cement-masked regions and
    // apply the painted-concrete normal (MAT_LAYER_PAINTED_CONC) instead, so cement
    // gets real surface relief rather than the flat-surface zeroing from legacy.
    // pureConcrete blends smoothly at boundary patches (smoothstep 2->3 TerrainType).
    detailN *= (1.0 - pureConcrete);
    if (pureConcrete > 0.001) {
#ifdef TERRAIN_NORMAL_ARRAY
        PREC vec4 cementNormalSample = texture(matNormalArray, vec3(uvConcrete, float(MAT_LAYER_PAINTED_CONC)));
#else
        PREC vec4 cementNormalSample = texture(matNormal3, uvConcrete);  // fallback: reuse concrete slot
#endif
        detailN += pureConcrete * normalBoost.w * fwConcrete * (cementNormalSample.rgb * 2.0 - 1.0);
    }

    // TERRAIN-DETAIL-ANTI-TILE-1: tie detail-normal strength to terrain LOD so it
    // stops forming a repeating "carpet" at distance. lodNear is 1.0 at LOD0 (full
    // detail), fades 1->0 across the LOD boundary, and is 0 in the far/mid tiers —
    // matching the discrete u_lodStep gating on the LOD-chunk path.
    // Gate flag rides in detailNormalStrength.z (>0.5 = ON, set by C++); OFF =>
    // detailMul stays 1.0 => byte-identical render.
    PREC float detailMul = (detailNormalStrength.z > 0.5) ? lodNear : 1.0;

    PREC vec3 N;
    N.xy = detailN.xy * detailNormalStrength.x * detailMul;
    // TERRAIN-NORMALS-FROM-HEIGHT-1: when the gate is enabled, add the
    // macroscopic slope (height-derived) into the local-z-up tangent-plane
    // parametrization the detail normal map writes into. Detail normals
    // still contribute high-frequency bumps; the new term restores
    // macroscopic surface tilt that the legacy flat-up base normal omitted.
    // Default OFF — when useTerrainNormalsFromHeight == 0 this branch is
    // skipped and the output is byte-identical to the legacy path.
    if (useTerrainNormalsFromHeight != 0 && terrainHeightParams.x > 0.5) {
        PREC vec3 hN = computeTerrainNormalFromHeight(WorldPos.xy);
        // hN is unit-length in (east, north, up). Translate to local Z=up
        // parametrization N.xy by projecting onto the same plane the detail
        // normals already use: N.xy += hN.xy / hN.z. Floor hN.z so a near-
        // cliff sample cannot blow the perturbation to infinity.
        // TERRAIN-TUNING-UI-1: strength scales the additive term so the
        // inspector slider can dial macroscopic slope influence 0..1.5
        // without recompiling. strength=1.0 = pre-slice behavior.
        N.xy += (hN.xy / max(hN.z, 0.05)) * terrainNormalsFromHeightStrength;
    }
    // Clamp normal deflection to prevent extreme angles that cause black snow
    // Max deflection of 0.7 means the normal can tilt ~35 degrees max
    N.xy = clamp(N.xy, -0.75, 0.75);
    N.z = 1.0;
    N = normalize(N);

    // LIGHTING-DEBUG-VIEWS-1A: mode 41 = final per-fragment normal as RGB.
    if (surfaceDebugMode == 41) {
        gl_FragDepth = gl_FragCoord.z;
        FragColor = vec4(N * 0.5 + 0.5, 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled(N);
#endif
        return;
    }

    PREC float NdotL = dot(N, terrainLightDir.xyz);
    // Floor lowered 0.1→0.02 so shadow-facing bumps can actually read as dark.
    PREC float diffuse = clamp(NdotL, 0.02, 1.0);

    // LIGHTING-DEBUG-VIEWS-1A: mode 42 = sun N·L diffuse term (grayscale).
    if (surfaceDebugMode == 42) {
        gl_FragDepth = gl_FragCoord.z;
        FragColor = vec4(vec3(diffuse), 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled(N);
#endif
        return;
    }

    // --- Material color tinting ---
    // tintRock/tintGrass/tintDirt are uniforms (TERRAIN-TINT-UI-1, tunable via ImGui).
    const PREC vec3 tintConcrete = vec3(0.55, 0.53, 0.50);
    const PREC vec3 tintSnow     = vec3(0.75, 0.78, 0.84);  // dimmed cool grey-white

    PREC vec3 materialTint = tintRock * matWeights.x
                           + tintGrass * matWeights.y
                           + tintDirt * matWeights.z
                           + tintConcrete * matWeights.w
                           + tintSnow * snowWeight;

    // Luminance-adaptive tint: dark colormap pixels get far less tint pull so they
    // don't lift to mid-grey. Snow always gets full tint (cool white must pop).
    PREC float colLum = dot(texColor.rgb, kLumaWeights);
    PREC float tintBase = mix(0.18, 0.50, smoothstep(0.1, 0.6, colLum));
    PREC float tintStrength = mix(tintBase, 0.85, snowWeight) * tintStrengthScale;
    PREC vec3 baseColor = mix(texColor.rgb, materialTint, tintStrength);
    {
        // Preserve the authored colormap tone for runway/cement.
        // Full concrete definitely comes through this shader path. Use the authored
        // runway/apron colormap directly for solid cement instead of routing it back
        // toward the generic concrete material tint.
        PREC vec3 concreteColor = texColor.rgb;
        baseColor = mix(baseColor, concreteColor, concreteColorBlend);
    }

    // --- Phase 4C: Triplanar cliff mapping ---
    // On steep slopes, darken and shift toward rock color to simulate exposed rock faces
    {
        PREC float slopeZ = abs(WorldNorm.z);
        // Start blending at ~30° slope (0.85), full at ~55° (0.55)
        PREC float cliffBlend = smoothstep(0.85, 0.55, slopeZ);
        if (cliffBlend > 0.01) {
            // Desaturate and darken toward rock tint on cliff faces
            PREC float luma = dot(baseColor, vec3(0.299, 0.587, 0.114));
            PREC vec3 cliffColor = mix(vec3(luma), tintRock, 0.6) * 0.8;
            baseColor = mix(baseColor, cliffColor, cliffBlend * 0.7);
        }
    }

    // World-space break-up noise for non-snow terrain. Two-octave — low frequency
    // for large patches, higher frequency for surface-texture feel. Dialed back ~10%
    // from the debug-time range now that the lighting range fix carries its own weight.
    {
        PREC float lowFreq  = fbm(WorldPos.xy * 0.0035, 3) * 0.5 + 0.5;
        PREC float highFreq = fbm(WorldPos.xy * 0.018,  2) * 0.5 + 0.5;
        PREC float breakupNoise = mix(lowFreq, highFreq, 0.55);
        PREC float breakupMod = mix(0.78, 1.18, breakupNoise);
        PREC float breakupAmount = (1.0 - snowWeight) * breakupFade;
        baseColor *= mix(1.0, breakupMod, breakupAmount);
    }

    // LIGHTING-DEBUG-VIEWS-1A: mode 40 = albedo (surface base color before any
    // lighting/shadow/ambient). Includes material tint, cliff and break-up
    // albedo-domain modulation; excludes sun, shadow, hemi fill, fog.
    if (surfaceDebugMode == 40) {
        gl_FragDepth = gl_FragCoord.z;
        FragColor = vec4(c.rgb * baseColor, 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled(N);
#endif
        return;
    }

    c.rgb *= baseColor;
    // Snow brightness dampen — only detected-snow fragments (snowWeight) are darkened.
    c.rgb *= mix(1.0, snowBrightnessDampen, snowWeight);
    // Normal map lighting — widened range from (0.55,1.15) to (0.35,1.20)
    // so dark sides of bumps read noticeably darker, creating actual bump contrast.
    PREC float normalLight = mix(0.35, 1.20, diffuse);
    normalLight = mix(normalLight, 1.0, pureConcrete * 0.85);
    c.rgb *= normalLight;

    if (surfaceDebugMode == 5) {
        gl_FragDepth = gl_FragCoord.z;
        FragColor = vec4(vec3(normalLight), 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled(N);
#endif
        return;
    }

    // Shadow — variable PCF taps by distance.
    // Pass flat up-normal (not detail-perturbed N) so slope-scale bias stays consistent
    // pixel-to-pixel. Using N here caused sprinkle/inverted-shadow patterns on bumpy
    // terrain — bias flipped across neighboring detail-normal deflections, letting
    // some pixels escape shadow while neighbors received it. Overlays already pass
    // vec3(0,0,1) for the same reason.
    const PREC vec3 shadowN = vec3(0.0, 0.0, 1.0);
    int shadowTaps = (lodNear > 0.5) ? 16 : (lodMid > 0.5) ? 8 : 4;
    float staticShadow = calcShadow(WorldPos, shadowN, terrainLightDir.xyz, shadowTaps);
    int dynTaps = (lodNear > 0.5) ? 8 : 4;
    float dynShadow = calcDynamicShadow(WorldPos, shadowN, terrainLightDir.xyz, dynTaps);
    // SHADOW-TERRAIN-COMBINE-MIN-1: take the darkest of the two shadow maps
    // instead of multiplying them. min() makes duplicate static+dynamic caster
    // coverage idempotent and prevents double-darkening when a caster appears in
    // both maps (e.g. a building in the world-fixed static map AND the
    // camera-fitted dynamic map): min(0.4,0.4)=0.4 vs the old 0.4*0.4=0.16.
    // Matches the screen-space/object receiver composition (shadow_screen.frag).
    // CLIFF SHADOW FLOOR: steep faces fall to ~0 under CSM → near-black. Lift ONLY
    // steep faces by a gated floor. Default 0.0 => no-op (byte-identical).
    float cliffShadowBlend = smoothstep(0.85, 0.55, abs(WorldNorm.z));
    float shadow = max(min(staticShadow, dynShadow), u_terrainCliffShadowFloor * cliffShadowBlend);

    // LIGHTING-DEBUG-VIEWS-1A: mode 44 = combined PCF shadow factor (grayscale,
    // 1=lit 0=occluded). Distinct from legacy modes 6/31 so the unified named
    // enum (MC2_LIGHTING_DEBUG_VIEW=shadow) is consistent across render families.
    if (surfaceDebugMode == 44) {
        gl_FragDepth = gl_FragCoord.z;
        FragColor = vec4(vec3(shadow), 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled(N);
#endif
        return;
    }

    // DEBUG-VIZ: 30 = dynamic-cast shadow only (isolates building dynamic shadow),
    // 31 = min(static,dyn). Grayscale, early-return. Gated by surfaceDebugMode only.
    if (surfaceDebugMode == 30) {
        gl_FragDepth = gl_FragCoord.z;
        FragColor = vec4(vec3(dynShadow), 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled(N);
#endif
        return;
    }
    if (surfaceDebugMode == 31) {
        gl_FragDepth = gl_FragCoord.z;
        FragColor = vec4(vec3(shadow), 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled(N);
#endif
        return;
    }

    c.rgb *= shadow;

    // TERRAIN-LIGHTING-1: hemisphere ambient fill. Added AFTER shadow
    // multiplication so sky/ground bounce continues to light shadowed
    // terrain (direct sun is shadowed; ambient is not). Sky/ground
    // tints are picked to match the existing fog colour (cool blue-
    // grey) and the rock/dirt material tints already in this shader.
    // Strength 0 short-circuits to no-op — CPU upload-site force-
    // zeroes the uniform when env gate MC2_TERRAIN_LIGHTING_V1 is OFF.
    // TERRAIN-LIGHTING-1/2: hemisphere additive (computed once; consumed
    // by the main render path AND by debug mode 11 below for inspection).
    PREC vec3 hemiContrib = vec3(0.0);
    if (terrainLightingV1Strength > 0.0) {
        const PREC vec3 hemiSkyTint    = vec3(0.55, 0.62, 0.75);
        const PREC vec3 hemiGroundTint = vec3(0.32, 0.28, 0.22);
        // N is the final per-fragment normal (after detail + height-derived
        // perturbation). N.z ≈ 1 on flat ground → full sky; N.z ≈ 0 on
        // a cliff face → halfway sky/ground; N.z near -1 (overhangs,
        // not present in terrain meshes) → full ground.
        PREC float skyFactor = N.z * 0.5 + 0.5;
        PREC vec3  hemiFill  = mix(hemiGroundTint, hemiSkyTint, skyFactor);
        // Snow areas already have aggressive specular sparkle and bright
        // tint — damp ambient there so we don't blow out snow.
        PREC float hemiAmount = terrainLightingV1Strength * (1.0 - 0.5 * snowWeight);
        // TERRAIN-LIGHTING-2: shadow-aware fill modulation. floor=1.0 →
        // mix collapses to 1.0 → V1 behavior (no shadow influence).
        // floor<1.0 → hemi contribution scales toward `floor` in fully
        // shadowed terrain, keeping shadows readable instead of getting
        // full sky bounce. `shadow` is the existing PCF sun shadow term
        // (staticShadow * dynShadow) computed a few lines above.
        PREC float hemiShadowMix = mix(terrainLightingV2ShadowFillFloor, 1.0, shadow);
        hemiContrib = hemiFill * hemiAmount * 0.25 * hemiShadowMix;
        c.rgb += hemiContrib;
    }

    // LIGHTING-DEBUG-VIEWS-1A: mode 43 = ambient/hemisphere fill only (×4 for
    // visibility, capped). Black when MC2_TERRAIN_LIGHTING_V1 gate is OFF.
    // Same channel meaning as legacy mode 11; kept under the unified enum id.
    if (surfaceDebugMode == 43) {
        gl_FragDepth = gl_FragCoord.z;
        FragColor = vec4(min(hemiContrib * 4.0, vec3(1.0)), 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }

    // TERRAIN-LIGHTING-2: debug mode 11 — hemi-only contribution. Shows
    // exactly what the additive term puts on screen for a given fragment
    // (including the V2 shadow-floor modulation). At V1 strength=0 or
    // gate=OFF, this returns black. Multiplied ×4 so subtle contributions
    // remain visible; the cap at 1.0 prevents clamping confusion.
    if (surfaceDebugMode == 11) {
        PREC vec3 vis = min(hemiContrib * 4.0, vec3(1.0));
        gl_FragDepth = gl_FragCoord.z;
        FragColor = vec4(vis, 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }

    // --- Snow sparkle ---
    // High-frequency hashed micro-glints gated by snow weight, light direction, and shadow.
    // Keeps perfectly still when camera is still (hash on world position), matches sun direction.
    if (snowWeight > 0.05) {
        PREC vec2 glintUV = WorldPos.xy * 0.75;
        PREC vec2 gc = floor(glintUV);
        PREC float hash = fract(sin(dot(gc, vec2(12.9898, 78.233))) * 43758.5453);
        PREC float glint = step(0.985, hash);
        // Specular-ish: hot only when normal roughly faces the sun (half-angle cheap approx)
        PREC vec3 viewApprox = normalize(vec3(0.0, 0.0, 1.0));
        PREC vec3 H = normalize(terrainLightDir.xyz + viewApprox);
        PREC float specMask = pow(clamp(dot(N, H), 0.0, 1.0), 48.0);
        PREC float sparkle = glint * specMask * snowWeight * shadow * 0.45;
        c.rgb += vec3(sparkle);
    }

    // --- Blowing-snow particle glow ---
    // Cloud-shadow darkening moved to the fullscreen cloud pass (cloud.frag).
    // The animated FBM is retained here only to drive the snow-edge particle
    // glow below — it no longer darkens the terrain.
    {
        PREC vec2 cloudUV = WorldPos.xy * 0.0006 + vec2(time * 0.012, time * 0.005);
        PREC float cloudNoise = fbm(cloudUV, 4) * 0.5 + 0.5;
        PREC float cloudShadow = smoothstep(0.3, 0.7, cloudNoise);

        // Blowing snow: near snow edges the animated FBM drives a subtle additive
        // white glow that reads as windblown particles drifting across the boundary.
        // Peaks at ~mid snowWeight (transition zones); fades out on pure rock or pure snow.
        PREC float snowEdge = smoothstep(0.02, 0.35, snowWeight) *
                              (1.0 - smoothstep(0.75, 1.0, snowWeight));
        PREC vec3  snowParticleTint = vec3(0.98, 0.99, 1.02);
        c.rgb += snowParticleTint * cloudShadow * snowEdge * 0.09 * shadow;
    }

    if (surfaceDebugMode == 6) {
        gl_FragDepth = gl_FragCoord.z;
        FragColor = vec4(vec3(shadow), 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_legacyTerrainMaterialAlpha(N, materialAlpha);
#endif
        return;
    }

    // LIGHTING-DEBUG-VIEWS-1A: mode 46 = over/under-bright heatmap of the fully
    // lit terrain color BEFORE fog. red = overbright (luma>1), blue = underlit
    // (luma<0.05), grayscale = in-range luma. Surfaces clipping past 1.0 or
    // crushed to black flag lighting-balance problems for the audit.
    if (surfaceDebugMode == 46) {
        PREC float luma = dot(c.rgb, kLumaWeights);
        PREC vec3 heat;
        if (luma > 1.0) {
            heat = vec3(1.0, clamp(2.0 - luma, 0.0, 1.0) * 0.4, 0.0);   // red, hotter = redder
        } else if (luma < 0.05) {
            heat = vec3(0.0, 0.0, 1.0);                                  // blue = crushed black
        } else {
            heat = vec3(luma);                                          // in-range grayscale
        }
        gl_FragDepth = gl_FragCoord.z;
        FragColor = vec4(heat, 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled(N);
#endif
        return;
    }

    // --- Phase 4B: Height-based exponential fog ---
    {
        PREC float camDist2D = distance(WorldPos.xy, cameraPos.xy);
        PREC float terrainHeight = WorldPos.z;
        PREC float fogDensity = 0.00006;
        PREC float heightScale = exp(-max(terrainHeight, 0.0) * 0.002);
        PREC float fogAmount = 1.0 - exp(-camDist2D * fogDensity * heightScale);
        fogAmount = clamp(fogAmount, 0.0, 0.70);
        PREC vec3 fogCol = vec3(0.58, 0.65, 0.75);
        c.rgb = mix(c.rgb, fogCol, fogAmount);
    }

    // --- Map-edge haze ---
    // Vanilla MC2 emitted a ring of terrain beyond the playable area and hid it with
    // haze-to-sky. With global fog disabled, those meta-ring tiles (which sample magenta
    // "no-data" texels from the colormap interior) become visible. Apply a short-range
    // haze-to-sky fade across the last ~one-tile band to reproduce the vanilla result
    // without bringing back global distance fog.
    if (mapHalfExtent > 0.0) {
        c.rgb = mix(c.rgb, EDGE_HAZE_SKY, edgeHazeAmount(WorldPos.xy, mapHalfExtent));
    }

    c.a = 1.0;
    FragColor = c;

#ifdef MRT_ENABLED
    GBuffer1 = rc_gbuffer1_legacyTerrainMaterialAlpha(N, materialAlpha);
#endif

    // Write depth for overlay/object depth testing. REVERSE-Z / GL_GEQUAL
    // regime (glClipControl ZERO_TO_ONE, glClearDepth(0); near->1, far->0;
    // LARGER NDC z = closer to camera = wins GEQUAL).
    //
    // Must match the thin-VS convention: gos_terrain_thin.vert applies
    // TERRAIN_DEPTH_FUDGE (-0.002) pre-divide in clip space to push terrain
    // FARTHER from camera, so both overlays (at +OVERLAY_DEPTH_BIAS) and
    // objects at ground level (at ~0 bias) WIN the GEQUAL tie. This fragment
    // override replicates that same -0.002 nudge via gl_FragDepth, using
    // min(undisplaced, displaced) to ensure upward-displaced terrain does not
    // occlude overlays drawn at the undisplaced surface.
    //
    // REGRESSION NOTE: using +OVERLAY_DEPTH_BIAS (+0.0005) here instead of
    // TERRAIN_DEPTH_FUDGE (-0.002) makes terrain appear CLOSER, causing
    // objects at ground level (depth ~D_terrain) to fail GEQUAL against the
    // written D_terrain+0.0005 -- the "sinking vehicles / overlay over buildings"
    // regression. TERRAIN_DEPTH_FUDGE is the correct constant here.
    gl_FragDepth = clamp(min(UndisplacedDepth, gl_FragCoord.z) + TERRAIN_DEPTH_FUDGE, 0.0, 1.0);
}
