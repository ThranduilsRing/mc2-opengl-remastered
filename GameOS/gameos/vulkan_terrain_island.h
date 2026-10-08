// vulkan_terrain_island.h — VK-P10 TERRAIN TIER-1: the FIRST VK-native TERRAIN
// COLOR/DEPTH region. Renders the LIVE LOD-chunk opaque MAIN-triangle draw for the
// nearest (LOD0, lodStep==1) patches IN-PLACE into the SHARED scene GBuffer
// (color MRT0 + normal MRT1 + shared depth) as the opaque PREFIX, then GL/props/
// mechs continue on top. Terrain analogue of the prop Tier-1 island — REUSES the
// prop island's shared-GBuffer plumbing + LOAD-preserve co-write renderpass +
// reverse-Z/double-sided pipeline state verbatim.
//
// SMALLEST PROBE SCOPE: LOD0 (lodStep==1) MAIN triangle draw ONLY. Skirts, coarser
// LOD bands, edge-stitch, and visual-displace are DEFERRED. The GL LOD-chunk path
// keeps drawing everything EXCEPT the LOD0 main patches when this gate is on.
//
// The whole feature is #ifdef MC2_VULKAN_ISLAND; these declarations are extern "C"
// so the caller (gos_terrain_lod_chunk.cpp / terrain.cpp) pulls in NO Vulkan headers.
//
// Gate MC2_VK_TERRAIN_TIER1 default-OFF (requires MC2_VK_SHARED_SCENE_DEPTH) ->
// GL terrain path byte-identical.

#ifndef MC2_VULKAN_TERRAIN_ISLAND_H
#define MC2_VULKAN_TERRAIN_ISLAND_H

#ifdef MC2_VULKAN_ISLAND

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Per-LOD0-patch placement record (mirrors the per-block uniforms the GL draw sets:
// gos_terrain_lod_chunk.cpp:1420-1436). One per LOD0 (lodStep==1) draw command.
struct Mc2TerrainTier1Patch {
    int32_t blockOriginX;   // TerrainDrawCommand::blockOriginX
    int32_t blockOriginY;   // TerrainDrawCommand::blockOriginY
    int32_t quadCountX;     // maxOffX = (qcX/lodStep)*lodStep
    int32_t quadCountY;     // maxOffY = (qcY/lodStep)*lodStep
    // VK-TERRAIN-ALL-LODS sub-slice: lodStep for this patch (1,2,4,5,10,20). Always
    // 1 when MC2_VK_TERRAIN_ALL_LODS is off (rung-3 feed capture only emits lodStep==1
    // patches in that case) -- old callers/behavior unaffected. VK-side patch-grid
    // generation must stride localOffset by this value (mirrors GL makeSamplePositions'
    // `p += lodStep`) so the vert shader's un-scaled `blockOriginX + localOffset.x`
    // math stays IDENTICAL to the LOD0 path.
    int32_t lodStep;
    // VK-TERRAIN-SKIRT sub-slice (rung 4): mirrors the GL per-block skirt inputs
    // (gos_terrain_lod_chunk.cpp skirtDepths[i]/skirtEdgeMasks[i]) captured at the SAME
    // site as the main-patch record so the VK island can draw this patch's skirt strips
    // in the SAME frame with NO extra GL state read. skirtDepth<=0 or skirtEdgeMask==0
    // -> island draws no skirt for this patch (mirrors GL's `if (skirtDepth > 0.0f)` /
    // per-edge mask-bit gate, gos_terrain_lod_chunk.cpp:1736,1755).
    float    skirtDepth;      // world-unit pull-down depth (0 = no skirt, mirrors GL skirtDepths[i])
    uint32_t skirtEdgeMask;   // bit0=N,1=S,2=W,3=E (mirrors GL skirtEdgeMasks[i], 0xF = all four)
    // VK-TERRAIN-VISUALDISPLACE-FAR-1 (rung 5): GL's per-patch visual-displace mode for
    // this block (gos_terrain_lod_chunk.cpp visualMode). Only 0 (no displace) or 2
    // (far-band pure Z-swap, lodStep>1) ever reach the island -- mode-1 (LOD0 4x-finer
    // topology) patches stay GL-owned (excluded at capture, separate L slice later).
    int32_t  visualMode;
    // VK-TERRAIN-EDGE-STITCH-1: packed per-edge coarser-neighbor stride, mirroring
    // terrain_lod_chunk.vert u_edgeStitch (N,S,W,E in bytes). 0 = no stitch.
    uint32_t edgeStitch;
};

// Per-frame terrain feed (mirrors Mc2ColorPropFeed). Populated by the GL LOD-chunk
// submit path when MC2_VK_TERRAIN_TIER1 is armed; consumed by the island's run().
// All pointers are into caller-owned per-frame scratch (valid only during run()).
struct Mc2TerrainTier1Feed {
    const Mc2TerrainTier1Patch* patches;   // LOD0 patch list (lodStep==1 only)
    uint32_t                    patchCount;
    float                       worldToClipGL[16]; // gos_GetTerrainMVPMat4()/dispatch — GL_FALSE
    int32_t                     mapSide;           // s_mapSide
    float                       halfMap;           // s_halfMap
    uint32_t                    glHeightSsbo;      // GL name of the height SSBO (binding 23)
    uint64_t                    heightBytes;       // mapSide*mapSide*sizeof(float)
    uint64_t                    heightVersion;     // bumps when the height field changes (snapshot key)
    // COLORMAP-BASE sub-slice: the GL colormap atlas texture (g_atlasGLTex) the VK
    // island imports copy-once, plus the atlas-UV reconstruction params (same source
    // as the GL LOD-chunk u_colormap / u_atlasTopLeftX/Y / u_atlasOneOverWorldUnits).
    uint32_t                    glColormapTex;         // GL name of g_atlasGLTex (0 = not built yet)
    float                       atlasTopLeftX;
    float                       atlasTopLeftY;
    float                       atlasOneOverWorldUnits;
    // CEMENT/ROAD atlas sub-slice: the GL cement/road overlay atlas (g_cementAtlasGLTex,
    // runtime-assembled RGBA8, GL_REPEAT wrap) the VK island imports copy-once into a
    // REPEAT-sampled image, plus the per-vertex cement-word SSBO (GL binding 25) it
    // snapshots (map-load, same as the height SSBO), plus the cement uniforms the GL
    // LOD-chunk frag reads (gos_terrain_lod_chunk.cpp:1122-1143). All 0/false when the
    // cement atlas isn't ready -> VK frag falls back to the colormap base (no hole).
    uint32_t                    glCementAtlasTex;      // GL name of g_cementAtlasGLTex (0 = not ready)
    int32_t                     cementGridSide;        // gos_terrain_indirect_getCementAtlasGridSide()
    float                       cementWUPT;            // world units per cement tile (== 128)
    int32_t                     useCement;             // 1 iff cement atlas ready AND cement SSBO live
    int32_t                     useTransitionMask;     // gos_terrain_indirect_isTransitionMaskReady()
    int32_t                     cementDiagConnect;     // MC2_TERRAIN_CEMENT_DIAG_CONNECT gate
    uint32_t                    glCementSsbo;          // GL name of the per-vertex cement-word SSBO (binding 25)
    uint64_t                    cementBytes;           // mapSide*mapSide*sizeof(uint)
    uint64_t                    cementVersion;         // bumps when the cement-word field changes (snapshot key)
    // VK-TERRAIN-LIGHTING sub-slice: make the VK terrain LIT (directional relief + baked
    // static sun shadow) to match GL. Gate MC2_VK_TERRAIN_LIGHTING (requires TIER1).
    int32_t                     lightingEnabled;       // 1 iff MC2_VK_TERRAIN_LIGHTING gate on
    float                       terrainLightDir[3];    // gos_GetTerrainLightDir() (sun dir)
    // STATIC sun shadow map: the GL depth texture (gos_postprocess getShadowTexture(),
    // GL_DEPTH_COMPONENT24). VK-SHADOW-COPY-ONCE-1: the GL map is built ONCE per mission
    // (gos_StaticLightMatrixBuilt() latch, gos_MarkStaticLightMatrixBuilt() bumps
    // staticShadowGeneration below) -- NOT per-frame -- so the island copy-once-per-
    // generation instead of copying every frame. staticLightSpaceMatrix
    // (pp->getLightSpaceMatrix(), row-major GL_FALSE, z already [0,1]). All 0/false when
    // shadows are off -> VK terrain is lit but unshadowed.
    int32_t                     shadowsEnabled;        // pp->shadowsEnabled_ (1 -> map is valid to sample)
    uint32_t                    glStaticShadowTex;     // GL name of shadowDepthTex_ (0 = not ready)
    int32_t                     staticShadowSize;      // shadowMapSize_ (edge in texels, 4096)
    uint64_t                    staticShadowGeneration; // gos_GetStaticShadowGeneration() (bumps on rebuild only)
    float                       staticLightSpaceMatrix[16]; // pp->getLightSpaceMatrix()
    float                       shadowSoftness;        // GL default 2.5
    // VK-TERRAIN-DYNSHADOW sub-slice: the DYNAMIC (building/mech) shadow map — the GL
    // depth texture pp->getDynamicShadowTexture() (legacy single-map, non-CSM path) the
    // island copies PER FRAME into a 2nd VK D32 depth-COMPARE image, plus its light-space
    // matrix pp->getDynamicLightSpaceMatrix(). GL: shadow = min(static, dyn), and on
    // pure concrete shadow = dyn (static dropped, dynamic kept). Gate MC2_VK_TERRAIN_DYNSHADOW
    // (requires MC2_VK_TERRAIN_LIGHTING). 0/false -> no dynamic shadow (lit-only), no hole.
    int32_t                     dynShadowsEnabled;     // pp->shadowsEnabled_ && getDynamicShadowFBO() && !CSM
    uint32_t                    glDynamicShadowTex;    // GL name of pp->getDynamicShadowTexture() (0 = not ready)
    int32_t                     dynamicShadowSize;     // dynamic shadow map edge in texels
    float                       dynamicLightSpaceMatrix[16]; // pp->getDynamicLightSpaceMatrix()
    // VK-TERRAIN-DETAIL sub-slice: perturb the terrain normal (macro slope + material
    // detail normal) like GL (terrain_lod_chunk.frag:740-808) so grass gets NdotL relief
    // -> closes the "grass ~3 levels darker" residual. Gate MC2_VK_TERRAIN_DETAIL
    // (requires MC2_VK_TERRAIN_LIGHTING). detailEnabled=0 -> VK terrain N stays flat-up
    // (byte-identical to the lighting-only frame).
    int32_t                     detailEnabled;         // 1 iff MC2_VK_TERRAIN_DETAIL gate on
    uint32_t                    glMatNormalArrayTex;   // GL name of terrain_normal_array_tex_ (0 = not built)
    float                       nfhStrength;           // terrainNormalsFromHeightStrength (GL default 1.0)
    float                       detailTiling;          // detailNormalTiling.x (GL default 1.0)
    float                       detailStrength;        // detailNormalStrength.x (GL default 1.0)
    // VK-TERRAIN-LODFADE sub-slice: fade detail-normal/POM to 0 across the LOD0/band
    // boundary like GL (gos_terrain.frag lodNear, :322-328,616-736), closing the VK
    // LOD0-vs-GL-band color seam. Gate MC2_VK_TERRAIN_LODFADE (requires
    // MC2_VK_TERRAIN_DETAIL); 0 -> VK island's lodNear stays 1.0 always (byte-identical
    // to the pre-fade frame).
    int32_t                     lodFadeEnabled;        // 1 iff MC2_VK_TERRAIN_LODFADE gate on
    float                       cameraPosMC2[3];       // gos_GetTerrainCameraPos() (Stuff/MLR space)

    // VK-TERRAIN-CSM-NEAR-1 sub-slice: near-cascade-array dynamic shadow (ports GL's
    // calcDynamicShadow cascade-select into VK instead of the coarse FULLMAP_EXPERIMENTAL
    // subset). Gate MC2_VK_TERRAIN_CSM (requires MC2_VK_TERRAIN_LIGHTING; independent of
    // the FULLMAP_EXPERIMENTAL escape hatch, which still feeds dynShadowsEnabled above
    // when armed). Source: GL's dynShadowArrayTex_ (GL_TEXTURE_2D_ARRAY depth, near
    // cascades 0..N-2) + the separate dynamicFullMapTex_ (last cascade, lower-res 2D).
    // csmEnabled=0 -> VK frag skips the CSM path entirely (falls back to the legacy
    // single-map dynShadowsEnabled path above, unchanged).
    int32_t                     csmEnabled;            // 1 iff gate on AND GL CSM is actually active+ready
    uint32_t                    glCsmArrayTex;         // GL name of dynShadowArrayTex_ (0 = not ready)
    int32_t                     csmArraySize;           // dynShadowMapSize_ (near-cascade array edge, texels)
    int32_t                     csmCount;               // pp->getDynamicShadowCascadeCount() (1..3)
    float                       csmCascadeMatrices[16 * 3]; // pp->getDynamicCascadeMatrices() (row-major GL_FALSE)
    float                       csmCascadeTexelWorld[3];    // pp->getDynamicCascadeTexelWorld()
    float                       csmDepthSpan;           // pp->getCsmDepthSpan()
    uint32_t                    glCsmFullMapTex;        // GL name of dynamicFullMapTex_ (0 = not ready)
    int32_t                     csmFullMapSize;         // pp->getDynamicFullMapSize()
    float                       csmFullMapTexelWorld;   // pp->getDynamicFullMapTexelWorld()
    // VK-TERRAIN-VISUALDISPLACE-FAR-1 (rung 5): the 4x-finer VISUAL heightfield bake
    // (GL binding 26, s_visualHeightSsbo) the island snapshots version-gated at mission
    // load (mirrors the height SSBO snapshot machinery), plus the mode-2 params the GL
    // vert reads (u_visualSide / u_visualDisplaceFar). All 0 when the sub-gate
    // (MC2_VK_TERRAIN_VISUALDISPLACE_FAR) is off or the bake isn't loaded -> island
    // draws undisplaced (fail-soft; GL also keeps displaced patches in that case).
    uint32_t                    glVisualHeightSsbo;    // GL name of s_visualHeightSsbo (0 = not loaded)
    uint64_t                    visualHeightBytes;     // visualSide*visualSide*sizeof(float)
    uint64_t                    visualHeightVersion;   // bumps on UploadVisualHeightFull (snapshot key)
    int32_t                     visualSide;            // V = (mapSide-1)*4+1 (0 = bake not loaded)
    float                       visualDisplaceFar;     // MC2_TERRAIN_VISUAL_DISPLACE_FAR fade, 0..1 (GL default 1)
    // VK-PARITY-TERRAIN-1: TERRAIN-AMBIENT-FLOOR-2 (floor, daylight gate) and
    // CEMENT-NIGHT-LIGHT-1 (mission flat light rgb) -- same producers GL uploads.
    float                       ambientFloor;
    float                       ambientDayGate;
    float                       missionFlatLight[3];
    // VK-PARITY-TERRAIN-2: NIGHT-LIGHT-POOLS-1..4 + LIGHT-CLUSTER-PERF-1 + PAD-AREA-GLOW-1.
    // poolLights points at numPoolLights packed 48B GpuTerrainLight records (the CPU copy
    // of GL binding 28); lightGridHeader/Indices are the CPU copies of GL bindings 29/30
    // (null + lightGridDim=0 => linear loop). Pointers valid for this frame only.
    int32_t                     numPoolLights;          // 0 => pool loop skipped
    const void*                 poolLights;             // GpuTerrainLight[numPoolLights] (48B std430)
    int32_t                     lightGridDim;           // 0 => grid off
    float                       lightGridMin[2];
    float                       lightGridInvCell[2];
    const uint32_t*             lightGridHeader;        // 2*dim*dim words
    uint32_t                    lightGridHeaderWords;
    const uint32_t*             lightGridIndices;
    uint32_t                    lightGridIndexWords;
    int32_t                     lightPoolSoft;
    float                       lightPoolMoverDim;
    float                       lightPoolMoverScale;
    int32_t                     lightPoolMoverMax;
    int32_t                     lightPoolCompress;
    float                       moverContactShadow;
    float                       padGlow[3];             // premultiplied (GL u_padGlow)
    float                       padGlowSpill;
};

// Provided BY the GL terrain path (gos_terrain_lod_chunk.cpp). The island calls this
// from run() to pull the frame's LOD0 patch list + shared params. Returns 1 on a
// valid feed, 0 otherwise (island then fails soft -> GL drew everything).
int mc2_terrain_tier1_get_frame_feed(Mc2TerrainTier1Feed* out);

// True when MC2_VK_TERRAIN_TIER1 (+ MC2_VK_SHARED_SCENE_DEPTH) requests the VK
// terrain region. When true the GL LOD-chunk loop SKIPS the LOD0 main draws (and
// LOD0 skirts) — the island co-writes those into the shared GBuffer instead.
int mc2_vk_terrain_island_enabled();

// VK-TERRAIN-ALL-LODS (rung 3): true when the coarse-band sub-gate (MC2_VK_TERRAIN_ALL_LODS,
// requires TIER1) is armed. When true the GL LOD-chunk loop's capture predicate below also
// captures lodStep>1 non-displaced, non-stitched patches (bands only -- skirts/stitch stay
// GL-owned this rung). Default-OFF -> capture unchanged (LOD0 only).
int mc2_vk_terrain_all_lods_enabled();

// VK-TERRAIN-SKIRTS (rung 4): true when the skirt/edge-stitch sub-gate
// (MC2_VK_TERRAIN_SKIRTS, requires MC2_VK_TERRAIN_ALL_LODS) is armed. Independent
// kill-switch from ALL_LODS/TIER1 because skirts are the crack-risk surface for this
// rung (VK-band-vs-VK-band AND VK-band-vs-GL-neighbour seams) -- being able to run
// "VK owns bands, GL still draws all skirts" isolates a bad skirt port from a bad
// band port during the oracle sweep. When true, the GL capture site ALSO snapshots
// skirtDepth/skirtEdgeMask into each captured patch's record and SKIPS drawing that
// patch's GL skirt strips (the same continue-pattern as the main-patch skip); the VK
// island then draws the skirt geometry itself in the same co-write pass. Default-OFF
// -> GL keeps drawing all skirts (today's shipped rung-3 state), byte-identical.
int mc2_vk_terrain_skirts_enabled();

// VK-TERRAIN-VISUALDISPLACE-FAR-1 (rung 5): true when the far-band visual-displace
// sub-gate (MC2_VK_TERRAIN_VISUALDISPLACE_FAR, requires MC2_VK_TERRAIN_ALL_LODS) is
// armed. When true the GL capture predicate RELAXES the visualDisplaceActive
// exclusion for mode-2 (lodStep>1, non-stitched) patches: the island captures them
// with visualMode=2 and Z-swaps their heights from the fine bake exactly like GL's
// terrain_lod_chunk.vert mode-2 branch. Mode-1 (LOD0 4x topology) and stitched
// patches stay GL-owned. Same envTriState opt-in pattern as MC2_VK_TERRAIN_SKIRTS
// (unset -> OFF regardless of umbrella state). Default-OFF -> capture unchanged.
int mc2_vk_terrain_visualdisplace_far_enabled();

// FAIL-SOFT ready predicate. True only when the gate is on AND the island is in a state
// where run() is expected to co-write this frame's LOD0 patches (born, inited, not
// disabled, SPV + pipeline live). The GL LOD-chunk loop gates its SPECULATIVE LOD0-skip
// on THIS (not on _enabled): if the island can't render (SPV/pipeline/init/device
// failure, or a latched transient), GL keeps drawing LOD0 so there is NEVER a terrain
// hole. Does not birth the island — an unborn island returns 0 (GL draws LOD0 this
// frame; the first successful run flips it ready).
int mc2_vk_terrain_island_ready();

// FAIL-SOFT prepare: births/(re)inits the island for the scene target size (w/h),
// independent of any per-frame feed, so the GL loop can decide the LOD0-skip BEFORE
// the feed is populated (breaks the skip<->ready<->feed chicken-egg deadlock). Must be
// called on the GL/render thread. Returns 1 iff the island is ready to co-write this
// frame (GL then commits the skip); 0 -> GL draws LOD0 (no hole).
int mc2_vk_terrain_island_prepare(int w, int h);

// Per-frame entry. Co-writes the LOD0 terrain patches IN-PLACE into the shared
// scene color(MRT0)+normal(MRT1)+depth GBuffer (LOAD-preserve, reverse-Z GEQUAL,
// double-sided, opaque). w/h = scene target size (getWidth()/getHeight()).
// Returns 1 if the VK terrain prefix rendered this frame; 0 = fell back (the GL
// LOD-chunk path must then draw the LOD0 patches too).
int mc2_vk_terrain_island_run(int w, int h);

// Health accessor (mirrors mc2_render_backend_color_prop_region_health, terrain fields).
void mc2_render_backend_terrain_region_health(
    int* vulkanAvailable, int* buildEnabled, const char** deviceName,
    unsigned long* attempted, unsigned long* usedVulkan, const char** fallbackReason,
    unsigned long* vkDraws, int* interopActive, int* vk2glSemActive,
    unsigned long* patchRecords, unsigned long* heightSnapshots,
    unsigned long* forbiddenGetTexImage, unsigned long* forbiddenReadPixels,
    double* stageUs, double* drawUs, double* totalUs);

// Teardown (called from gameosmain.cpp before the shared device is destroyed).
void mc2_vk_terrain_island_destroy();

#ifdef __cplusplus
}
#endif

#endif // MC2_VULKAN_ISLAND
#endif // MC2_VULKAN_TERRAIN_ISLAND_H
