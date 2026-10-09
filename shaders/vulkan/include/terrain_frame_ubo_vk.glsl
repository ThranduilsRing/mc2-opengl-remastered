// terrain_frame_ubo_vk.glsl -- the terrain island's per-patch std140 uniform block
// (set 0, binding 0, one dynamic slot), declared once for every VK terrain shader stage
// (tier1 / skirt vert + frag, overlay, decal). Offsets are locked to the C++
// TerrainFrameUBO in GameOS/gameos/vulkan_terrain_island.cpp by tools/check_ubo_layout.py.
// Resource-free on purpose (no samplers, no helpers) so vertex stages can include it.
#ifndef TERRAIN_FRAME_UBO_VK_GLSL
#define TERRAIN_FRAME_UBO_VK_GLSL

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

#endif // TERRAIN_FRAME_UBO_VK_GLSL
