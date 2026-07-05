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
    int32_t quadCountX;     // maxOffX = (qcX/1)*1 = qcX   (LOD0 -> lodStep==1)
    int32_t quadCountY;     // maxOffY = qcY
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
    // GL_DEPTH_COMPONENT24) the island copies PER FRAME into a VK D32 depth-COMPARE image,
    // plus the static light-space matrix (pp->getLightSpaceMatrix(), row-major GL_FALSE,
    // z already [0,1]). All 0/false when shadows are off -> VK terrain is lit but unshadowed.
    int32_t                     shadowsEnabled;        // pp->shadowsEnabled_ (1 -> map is valid to sample)
    uint32_t                    glStaticShadowTex;     // GL name of shadowDepthTex_ (0 = not ready)
    int32_t                     staticShadowSize;      // shadowMapSize_ (edge in texels, 4096)
    float                       staticLightSpaceMatrix[16]; // pp->getLightSpaceMatrix()
    float                       shadowSoftness;        // GL default 2.5
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
};

// Provided BY the GL terrain path (gos_terrain_lod_chunk.cpp). The island calls this
// from run() to pull the frame's LOD0 patch list + shared params. Returns 1 on a
// valid feed, 0 otherwise (island then fails soft -> GL drew everything).
int mc2_terrain_tier1_get_frame_feed(Mc2TerrainTier1Feed* out);

// True when MC2_VK_TERRAIN_TIER1 (+ MC2_VK_SHARED_SCENE_DEPTH) requests the VK
// terrain region. When true the GL LOD-chunk loop SKIPS the LOD0 main draws (and
// LOD0 skirts) — the island co-writes those into the shared GBuffer instead.
int mc2_vk_terrain_island_enabled();

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
