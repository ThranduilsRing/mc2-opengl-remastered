// RenderCore/gpu_abi/GpuTerrainFrameUBO.h
//
// SINGLE SOURCE OF TRUTH for the VK terrain island's per-patch std140 uniform block
// (set 0, binding 0, addressed by dynamic offset, one 1024 B slot per LOD0 patch).
// Hand-written; tools/gpu_abi_gen.py reads the annotated struct below and emits,
// committed and byte-deterministic:
//   RenderCore/gpu_abi/GpuTerrainFrameUBO.layout.gen.h        (C++ sizeof/offsetof asserts,
//                                                              computed from std140 rules)
//   shaders/vulkan/include/terrain_frame_ubo_vk.glsl          (the GLSL uniform block `uT`)
// Regenerate: py -3 tools/gpu_abi_gen.py --write      Verify: --check
//
// Consumers: GameOS/gameos/vulkan_terrain_island.cpp (producer) and every VK terrain
// stage (terrain_tier1 / skirt vert + frag, overlay, decal) via the generated include.
// Do NOT re-declare this block anywhere else.
//
// Annotations on the trailing comment (see tools/gpu_abi_gen.py):
//   @mat4 / @vec4  float[16] / float[4] as a GLSL mat4 / vec4
//   @mat4xN        float[16*N] as mat4[N]
//   @rgb           float[3] as three scalars <name>R/G/B (packed, no vec3 padding)
//   @cpp_only      C++ tail padding that is not part of the GLSL block
// Explicit `_*Pad` members are real GLSL members that keep later members 16-aligned.
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

// @gpu std140 glsl=TerrainFrameUBO block=uT set=0 set_macro=TERRAIN_RESOURCE_SET binding=0 slot=1024
struct TerrainFrameUBO {
    float   worldToClipGL[16];  // @mat4
    int32_t mapSide;
    float   halfMap;
    int32_t blockOriginX;
    int32_t blockOriginY;
    // COLORMAP-BASE sub-slice: atlas-UV reconstruction params (mirror GL
    // u_atlasTopLeftX/Y + u_atlasOneOverWorldUnits) + colormap-ready flag.
    float   atlasTopLeftX;
    float   atlasTopLeftY;
    float   atlasOneOverWorldUnits;
    int32_t colormapReady;
    // CEMENT/ROAD atlas sub-slice: cement uniforms (mirror the GL LOD-chunk frag's
    // u_useCement/u_cementGridSide/u_cementWUPT/u_useTransitionMask/u_cementDiagConnect).
    int32_t useCement;
    int32_t cementLayerCount;      // CEMENT-ARRAY-1: layers in the u_cementArt texture array (== catalog tile count)
    float   cementWUPT;
    int32_t useTransitionMask;
    int32_t cementDiagConnect;
    // VK-TERRAIN-LIGHTING sub-slice. std140: pad to off 128 (vec4 align), then the sun
    // dir vec4 + the static light-space mat4 + scalar toggles. LOCKSTEP with the frag.
    int32_t _lightPad0;   // read as the per-cell pool light budget (0 = unlimited); kept under the pad name for the layout gen
    int32_t _lightPad1;   // read as the lean-preset flag (1 on Low/Potato): water skips its ripple noise
    int32_t _lightPad2;
    float   terrainLightDir[4];  // @vec4
    float   staticLightSpaceMatrix[16];  // @mat4
    int32_t lightingEnabled;   // MC2_VK_TERRAIN_LIGHTING gate
    int32_t enableShadows;   // static shadow map imported+valid this frame
    float   shadowSoftness;   // GL default 2.5
    float   reserved220;       // was sceneLightScale (the prelit ground's mission night scale); now EnvironmentUBO.sceneLight.x
    // VK-TERRAIN-DETAIL sub-slice: terrain normal perturbation (macro slope + material
    // detail normal) so grass reads with NdotL relief like GL. LOCKSTEP with the frag.
    int32_t detailEnabled;   // MC2_VK_TERRAIN_DETAIL gate (requires lighting)
    int32_t detailReady;   // matNormalArray 2D_ARRAY imported+bound (2: and it carries the cliff layer)
    float   nfhStrength;   // terrainNormalsFromHeightStrength (GL default 1.0)
    float   detailTiling;   // detailNormalTiling.x (GL default 1.0)
    float   detailStrength;   // detailNormalStrength.x (GL default 1.0)
    // VK-TERRAIN-DYNSHADOW sub-slice: dynamic (building/mech) shadow. LOCKSTEP with frag.
    // std140: pad the dynamic light-space mat4 to a 16B boundary (off 256).
    int32_t enableDynShadows;   // dynamic shadow map imported+valid this frame
    int32_t _dynPad0;
    int32_t _dynPad1;
    float   dynamicLightSpaceMatrix[16];  // @mat4
    // VK-TERRAIN-LODFADE sub-slice: camera world position (Stuff/MLR space) + the
    // gate toggle, packed into the existing pad tail (was _pad[48], off 320..511) so
    // the 512B dynamic-alignment slot does NOT grow. LOCKSTEP with terrain_tier1.frag.
    float   cameraPosMC2[4];  // @vec4 gos_GetTerrainCameraPos(), .w pad
    int32_t lodFadeEnabled;
    // VK-TERRAIN-SKIRT sub-slice (rung 4): skirt pull-down depth, mirrors GL
    // u_skirtDepth (gos_terrain_lod_chunk.cpp skirtDepths[i]). Read by
    // terrain_tier1_skirt.vert only (the main-patch vert never reads it — main
    // patches always draw with skirtDepth conceptually 0, same as GL's "draw main
    // patch (skirtDepth=0)" comment). Packed into the existing pad tail so the 512B
    // dynamic-alignment slot does NOT grow. LOCKSTEP with terrain_tier1_skirt.vert.
    float   skirtDepth;
    // VK-TERRAIN-CSM-NEAR-1 sub-slice: near-cascade-array dynamic shadow (ports GL's
    // calcDynamicShadow cascade-select VERBATIM). LOCKSTEP with terrain_tier1.frag.
    // Slot grew 512->1024B to fit N cascade matrices; pad to a 16B boundary first.
    int32_t _csmPad0;
    int32_t _csmPad1;
    int32_t csmEnabled;   // MC2_VK_TERRAIN_CSM near-cascade path armed+valid this frame
    int32_t csmCount;   // 1..MC2_SHADOW_CSM_MAX (mirrors GL dynamicCsmCount)
    float   csmDepthSpan;   // (farP-nearP) of the shared CSM ortho z-row
    int32_t csmDebugMode;   // CSM-BLACKOUT-RECON-1 one-shot debug viz (0=off)
    float   csmCascadeMatrices[48];  // @mat4xN dynamicCascadeMatrices[MC2_SHADOW_CSM_MAX] (mat4 x3)
    float   csmCascadeTexelWorld[4];  // @vec4 per-cascade world-texel footprint (vec3 used + pad)
    float   csmFullMapTexelWorld;   // dynamicFullMapTexelWorld (separate lower-res full-map tex)
    // VK-TERRAIN-VISUALDISPLACE-FAR-1 (rung 5): mode-2 far-band Z-swap params, packed
    // into the pad tail (slot does NOT grow). LOCKSTEP with terrain_tier1.vert /
    // terrain_tier1_skirt.vert / terrain_tier1.frag.
    int32_t visualSide;   // fine bake side V (0 = bake not snapshotted)
    float   visualDisplaceFar;   // MC2_TERRAIN_VISUAL_DISPLACE_FAR fade 0..1
    int32_t visualMode;   // 0 = no displace, 2 = far-band Z-swap (per patch)
    int32_t edgeStitch;   // VK-TERRAIN-EDGE-STITCH-1 packed N/S/W/E stride bytes
    int32_t quadCountX;   // max local offset X for edge tests
    int32_t quadCountY;   // max local offset Y for edge tests
    // VK-PARITY-TERRAIN-1: TERRAIN-AMBIENT-FLOOR-2 + CEMENT-NIGHT-LIGHT-1 (LOCKSTEP with frag/verts).
    float   ambientFloor;
    float   ambientDayGate;
    float   missionFlatR;
    float   missionFlatG;
    float   missionFlatB;
    // VK-PARITY-TERRAIN-2: NIGHT-LIGHT-POOLS-1..4 + LIGHT-CLUSTER-PERF-1 + MOVER-CONTACT-
    // SHADOW-1 + PAD-AREA-GLOW-1 (LOCKSTEP with terrain_tier1{,_skirt}.{vert,frag}).
    int32_t numPoolLights;
    int32_t lightGridDim;
    float   lightGridMinX;
    float   lightGridMinY;
    float   lightGridInvCellX;
    float   lightGridInvCellY;
    int32_t lightPoolSoft;
    float   lightPoolMoverDim;
    float   lightPoolMoverScale;
    int32_t lightPoolMoverMax;
    int32_t lightPoolCompress;
    float   moverContactShadow;
    float   padGlowR;
    float   padGlowG;
    float   padGlowB;
    float   padGlowSpill;
    // VK-PARITY-SHADOW-1: SHADOW-DIST-FADE-1 series + TERRAIN-SHADOW-GUARD-1 + BANDFIX_V2
    // (LOCKSTEP with terrain_tier1{,_skirt}.{vert,frag}); producer mc2ShadowFadeKnobs().
    float   shadowDistFadeWu;
    float   shadowDistFadeMax;
    float   shadowMinSunSin;
    int32_t shadowKnobFlags;   // b0 FADE_CONT b1 RAMP0 b2 ALT_SCALE b3 BANDFIX_V2
    // VK-PARITY-F3 (LOCKSTEP with terrain_tier1{,_skirt}.frag): legacy material tint + snow dampen.
    float   tintRock[3];  // @rgb
    float   tintGrass[3];  // @rgb
    float   tintDirt[3];  // @rgb
    float   tintConcrete[3];  // @rgb
    float   tintSnow[3];  // @rgb
    float   tintStrengthScale;
    float   snowBrightnessDampen;
    float   edgeFeatherStrength;   // TERRAIN-EDGE-FEATHER-1: map-edge haze amount (0 = off)
    int32_t reserved776;           // was the cement atlas seam-fix flag; the cement texture array has no cell borders to inset
    // TERRAIN-SHORELINE-V3/V4 (LOCKSTEP with terrain_tier1.frag); producer gos_TerrainShorelineKnobs().
    int32_t shorelineOn;           // MC2_TERRAIN_SHORELINE: wet / foam elevation bands drawn
    int32_t shorelineHasMask;      // the mask sidecar (binding 14) is real, not the dummy
    float   shorelineWaterElev;    // water plane Z (world units)
    float   shorelineTime;         // shader clock seconds (foam animation)
    float   shorelineStrength;     // wet darken multiplier
    float   shorelineFoamStrength; // foam rim multiplier
    float   shorelineWetRun;       // wet lobe horizontal run (wu)
    float   shorelineFoamRun;      // foam lobe horizontal run (wu)
    float   shorelineEdgeJitter;   // static fbm jitter on the run (wu)
    float   shorelineBoundsX;      // mask bounds, world units: top-left X
    float   shorelineBoundsY;      // top-left Y (= max Y)
    float   shorelineBoundsW;      // size X
    float   shorelineBoundsH;      // size Y
    // SHORE-FIELD-1: the smooth shore field (binding 15) the coastal colormap follows (owner: "colormap tint for now").
    int32_t shoreFieldOn;          // the bound field is the mission's (mode 3 + sidecar), not the dummy
    int32_t shoreFieldIsSdf;       // the bound field is a cooked shore SDF, not a raw visual-height bake
    float   shoreFieldBoundsX;     // field bounds, world units: top-left X
    float   shoreFieldBoundsY;     // top-left Y (= max Y)
    float   shoreFieldBoundsW;     // grid span X
    float   shoreFieldBoundsH;     // grid span Y
    // CONCRETE-ROAD-PBR-DETAIL: binding 18 detail array (normal xy, roughness, AO) ready + strength scale; 0 = base game look.
    int32_t concreteDetail;
    float   concreteDetailStrength;
    int32_t groundMottleOn;        // vegetation palette only while a population is resident
    int32_t _pad[39];  // @cpp_only slot tail padding to the 1024B stride
};

#include "GpuTerrainFrameUBO.layout.gen.h"
