// terrain_frame_vk.glsl -- VK-PARITY-OVERLAY-1: the terrain island's set-0 resource
// declarations + lighting/shadow helpers, shared by the VK terrain OVERLAY / DECAL
// fragment shaders (terrain_overlay_tier1.frag, terrain_decal_tier1.frag). The overlay
// pass reuses the terrain island's descriptor set 0 (same TerrainFrameUBO slot layout,
// cement atlas/words, light pool, static/dynamic/CSM shadow maps) so overlays see the
// SAME lighting inputs as the terrain they sit on.
// The UBO block + shadow helpers are copied VERBATIM from terrain_tier1.frag (kept
// separate so the shipping terrain frag is untouched); tools/check_ubo_layout.py has a
// "terrain_overlay" pair that locks this copy to the C++ TerrainFrameUBO.
// The INCLUDER must enable GL_GOOGLE_include_directive and define PREC.
#ifndef TERRAIN_FRAME_VK_GLSL
#define TERRAIN_FRAME_VK_GLSL
#include "lighting_shadow_vk.glsl"

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
    // VK-TERRAIN-VISUALDISPLACE-FAR-1 (rung 5): far-band mode-2 Z-swap params (read by
    // the two VS stages only; declared here for the std140 LOCKSTEP + layout check).
    int   visualSide;                             // off 580: fine bake side V (0 = not snapshotted)
    float visualDisplaceFar;                      // off 584: MC2_TERRAIN_VISUAL_DISPLACE_FAR fade 0..1
    int   visualMode;                             // off 588: 0 = no displace, 2 = far-band Z-swap
    int   edgeStitch;                             // off 592: packed N/S/W/E coarser-neighbor stride bytes
    int   quadCountX;                             // off 596: max local offset X for edge tests
    int   quadCountY;                             // off 600: max local offset Y for edge tests
    // VK-PARITY-TERRAIN-1: TERRAIN-AMBIENT-FLOOR-2 + CEMENT-NIGHT-LIGHT-1 (scalars: no std140 vec3 pitfalls)
    float ambientFloor;                           // off 604: gos_TerrainAmbientFloor() (GL u_terrainAmbientFloor)
    float ambientDayGate;                         // off 608: gos_TerrainAmbientDayGate() (GL u_terrainAmbientDayGate)
    float missionFlatR;                           // off 612: gos_GetTerrainMissionFlatLight() (GL u_missionFlatLight)
    float missionFlatG;                           // off 616
    float missionFlatB;                           // off 620
    // VK-PARITY-TERRAIN-2: NIGHT-LIGHT-POOLS-1..4 + LIGHT-CLUSTER-PERF-1 grid +
    // MOVER-CONTACT-SHADOW-1 + PAD-AREA-GLOW-1 knobs. Same C++ producers/values as the
    // GL u_numPoolLights/u_lightGrid*/u_lightPool*/u_moverContactShadow/u_padGlow* uniforms.
    int   numPoolLights;                          // off 624: 0 -> pool loop skipped
    int   lightGridDim;                           // off 628: 0 -> linear loop
    float lightGridMinX;                          // off 632
    float lightGridMinY;                          // off 636
    float lightGridInvCellX;                      // off 640
    float lightGridInvCellY;                      // off 644
    int   lightPoolSoft;                          // off 648
    float lightPoolMoverDim;                      // off 652
    float lightPoolMoverScale;                    // off 656
    int   lightPoolMoverMax;                      // off 660
    int   lightPoolCompress;                      // off 664
    float moverContactShadow;                     // off 668
    float padGlowR;                               // off 672: premultiplied (strength*warm*nightFactor)
    float padGlowG;                               // off 676
    float padGlowB;                               // off 680
    float padGlowSpill;                           // off 684
    // VK-PARITY-SHADOW-1: SHADOW-DIST-FADE-1 series + TERRAIN-SHADOW-GUARD-1 + BANDFIX_V2 knobs
    // (same values GL injects as MC2_SHADOW_* #defines; producer mc2ShadowFadeKnobs()).
    float shadowDistFadeWu;                       // off 688: 0 -> fade block off
    float shadowDistFadeMax;                      // off 692
    float shadowMinSunSin;                        // off 696: 0 -> sun clamp identity
    int   shadowKnobFlags;                        // off 700: b0 FADE_CONT b1 RAMP0 b2 ALT_SCALE b3 BANDFIX_V2
    // VK-PARITY-F3: GL default-ON legacy material tint (TERRAIN-TINT-UI-1) + snow dampen.
    // Same producers as the GL tint*/tintStrengthScale/snowBrightnessDampen uniforms.
    float tintRockR; float tintRockG; float tintRockB;          // off 704
    float tintGrassR; float tintGrassG; float tintGrassB;       // off 716
    float tintDirtR; float tintDirtG; float tintDirtB;          // off 728
    float tintConcreteR; float tintConcreteG; float tintConcreteB;  // off 740
    float tintSnowR; float tintSnowG; float tintSnowB;          // off 752
    float tintStrengthScale;                                    // off 764
    float snowBrightnessDampen;                                 // off 768
} uT;

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
    float _plPad2;
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
