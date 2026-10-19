// Shared surface of the gos_terrain_lod_chunk translation units (gos_terrain_lod_chunk*.cpp).
//
// gos_terrain_lod_chunk.cpp keeps the program and uniform-location state, Init/Destroy and the SSBO state; the rest of the
// LOD-chunk terrain path is split by responsibility (see DOC in each piece). State more than one piece touches is
// declared here and defined in the piece that owns it.
#ifndef GOS_TERRAIN_LOD_CHUNK_INTERNAL_H
#define GOS_TERRAIN_LOD_CHUNK_INTERNAL_H

#include "gos_terrain_lod_chunk.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "utils/gl_utils.h"
#include "utils/shader_builder.h"
#include "gos_postprocess.h"   // Phase 10 Step 1c: shadow textures + light matrices
#include <gameos.hpp>          // Item 1: mc2ShadowCsmEnabled/Count (CSM shader define)
#include <string>
#include "gl_state_guard.h"    // GlStateGuard slice 2: composable depth/blend/cull RAII
#include "../../RenderCore/PipelineRegistry.h"  // TERRAIN-LODCHUNK-APPLYPIPELINE-ROUTING-1
#include "../../RenderCore/terrain_path_telemetry.h"  // TERRAIN-PATH-TELEMETRY-1
#include "pipeline_binder.h"                     // applyPipeline(TerrainSolid)
#include "../../mclib/render_contract.h"  // [RENDER_PASS v1] noteRenderPass
#include "../../RenderCore/RenderResourceRegistry.h"  // REGISTRY-TERRAIN-SSBO-1: TerrainHeightSsbo
#include "../../RenderCore/GpuBuffer.h"  // GPU-BUFFER-TIER0-LODCHUNK-RAII-1: owning RAII buffers (was GpuBufferOwner identity records)
#include "../../RenderCore/KtxLoader.h"  // TERRAIN-MATERIAL-TEXTURES-1: BC7 KTX2 albedo layer load
#include "gos_profiler.h"  // CLIFF-TESS-PERF: Tracy ZoneScopedN for the near-field tess mirror+draw
#include "gos_terrain_lighting.h"  // NIGHT-LIGHT-POOLS-1: per-frame light-pool SSBO accessors
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

#include <optional>
#include <utility>  // std::move (GPU-BUFFER-TIER0-LODCHUNK-RAII-1: move-only patch buffers)
#include <vector>
#include <map>
#include "Gates/gate.h"

#ifdef MC2_VULKAN_ISLAND
#include "vulkan_terrain_island.h"   // VK-P10 TERRAIN TIER-1: LOD0 shared-GBuffer co-write
#include "vulkan_sky_island.h"       // VK-PARITY-F3: HDRI sky behind the terrain prefix
#include "vk_shared_gbuffer_gate.h"  // VK-P14 GATE-CONSOLIDATION-1 umbrella resolver
#include "vk_feed_dump.h"            // VK-OFFLINE-PROOFS-1: MC2_VK_FEED_DUMP golden dump
#endif




// Terrain MVP matrix — exposed by gameos_graphics.cpp for all terrain draw paths.
extern const float* gos_GetTerrainMVPMat4();

// TERRAIN-SHORELINE-MASK-1: shared render-shader clock (SmokeMode fixed-
// timestep override, same computation as the water fast-path's "time"
// uniform) — exposed by gameos_graphics.cpp. f(worldPos,time)-ONLY foam
// animation; never fed by anything camera-dependent.
extern float gos_GetShaderClockSeconds();

// Fix-B frame-of-reference: the GPU water cull and DrawDecalStatic project with
// the PREVIOUS-frame dispatch MVP (baked in Terrain::geometry()) when armed, NOT
// the live current-frame MVP. The legacy terrain draw also uses that baked MVP,
// so terrain depth + water cull + decals all agree at frame N-1. The chunk MUST
// match, or it writes depth at frame N while water/decals project at N-1 -> a
// 1-frame offset -> shore-water dropout + decal tearing under camera motion.
extern "C" const float* gos_terrain_indirect_getDispatchMvp16();

#ifdef MC2_VULKAN_ISLAND

#endif

extern uint64_t gos_GetTerrainMaterialNormalVersion();
// The gos texture handle of detail-normal slot `slot` (0-8), one layer source of that array; INVALID_TEXTURE_ID when absent.
uint32_t gos_GetTerrainMaterialNormalHandle(int slot);

extern void  gos_GetTerrainMatTiling(float*, float*, float*, float*, float*);

extern void  gos_GetTerrainTintRock(float*, float*, float*);

extern void  gos_GetTerrainTintGrass(float*, float*, float*);

extern void  gos_GetTerrainTintDirt(float*, float*, float*);

extern float gos_GetTerrainTintStrengthScale();

extern float gos_GetTerrainControlAlbedoStrength();  // TERRAIN-CONTROLMAP-ALBEDO-1

// TERRAIN-MATERIAL-LIB-1: promoted frag-literal tints + per-layer roughness/AO
// scalars + the u_useMaterialLib gate. These were previously only wired to the
// legacy tessellated patch-stream path (terrainBindUniformsForPatchStream) --
// the live chunk binder never fetched these locs or uploaded them, so JSON-
// authored terrain_materials.json values never reached this shader in practice.
extern void  gos_GetTerrainTintConcrete(float*, float*, float*);

extern void  gos_GetTerrainTintSnow(float*, float*, float*);

extern void  gos_GetTerrainMatRoughness(float*, float*, float*, float*);

extern void  gos_GetTerrainMatAO(float*, float*, float*, float*);

extern void  gos_GetTerrainMatHeight(float*, float*, float*, float*);  // POM-PERLAYER-HEIGHT-1

extern bool  gos_TerrainMaterialLibEnabled();

extern float gos_GetTerrainCliffShadowFloor();  // CLIFF SHADOW FLOOR

extern float gos_GetTerrainNormalsFromHeightStrength();

extern float gos_GetTerrainPOMScale();

// TERRAIN-CHUNK-POM-1: Stuff/MLR eye position (the SAME vec4 the legacy terrain
// frag consumes as "cameraPos"); local-extern per the established accessor
// pattern in this file (ruling R5).
extern void  gos_GetTerrainCameraPos(float*, float*, float*);

extern int   g_terrainMaterialProfile;   // global; 0 = legacy

// Step 5c: cement catalog atlas (tex3) accessors from gos_terrain_indirect.cpp.

extern int          gos_terrain_indirect_getCementAtlasGridSide();

extern bool         gos_terrain_indirect_isCementAtlasReady();

// Stage B: transition mask array (GL_TEXTURE_2D_ARRAY, unit 11).

extern bool   gos_terrain_indirect_isTransitionMaskReady();

extern void  gos_GetTerrainMatNormalBoost(float*, float*, float*, float*);

extern void  gos_GetTerrainClassGrass(float*, float*, float*, float*);

extern void  gos_GetTerrainClassDirt(float*, float*, float*, float*);

extern float gos_GetTerrainDetailTiling();

extern float gos_GetTerrainDetailStrength();


// Phase 10: colormap atlas accessors (defined in gos_terrain_indirect.cpp,
// global free functions). Same atlas tex1 + UV params the legacy gos_terrain.frag
// useAtlasColormap path consumes.

extern float  gos_terrain_indirect_getAtlasMapTopLeftX();

extern float  gos_terrain_indirect_getAtlasMapTopLeftY();

extern float  gos_terrain_indirect_getAtlasOneOverWorldUnits();

// Phase 10 Step 1b: terrain sun direction (gameos.hpp), same value the legacy
// terrain frag's terrainLightDir uniform receives.
extern void   gos_GetTerrainLightDir(float* x, float* y, float* z);

// CEMENT-NIGHT-LIGHT-1: mission flat-pad light (gameos.hpp).
extern void   gos_GetTerrainMissionFlatLight(float* r, float* g, float* b);

// TERRAIN-AMBIENT-FLOOR-2: daylight level (gameos_graphics.cpp), pushed by
// gamecam.cpp from terrain_daylight.h. Night state is the TRACE input only.
extern float  gos_GetTerrainDaylightLevel();

float gos_TerrainSnowBrightnessDampen();   // VK-PARITY-F3 (defined below)

extern void   gos_GetNightState(bool* isNight, float* nightFactor);


// ---------------------------------------------------------------------------
// Patch geometry cache (Phase 4).
// Each unique (qcX, qcY, lodStep) triple gets one VBO+IBO pair.
// VBO contains int16_t[2] (localX, localY) per vertex.
// IBO contains uint16_t triangle indices.
// ---------------------------------------------------------------------------

// GPU-BUFFER-TIER0-LODCHUNK-RAII-1: the four per-patch buffers are owning
// RenderCore::GpuBuffer values (move-only) instead of bare GLuints. Untracked
// (no RenderResourceId): VBO/IBO are out of registry scope by design (per-draw
// rebind vertex/index data, not owned frame-graph resources) -- the RAII win
// here is create/delete pairing (cache clear frees GL, no hand-rolled loop).
namespace gos_terrain_indirect { bool IsFrameSolidArmed(); }


// TERRAIN-SHORELINE-V3/V4: the shoreline band knobs, one producer for the GL uniforms and the VK terrain feed.
struct TerrainShorelineKnobs {
    bool  on = false;            // MC2_TERRAIN_SHORELINE
    float waterElevation = 0.f;  // water plane Z, world units
    float timeSeconds = 0.f;     // shader clock (foam animation)
    float strength = 1.f, foamStrength = 1.f;
    float wetRun = 16.f, foamRun = 5.f, edgeJitter = 4.f;   // world units
};
TerrainShorelineKnobs gos_TerrainShorelineKnobs();

// State and helpers shared between the pieces. Each is defined once, in the piece that owns it.
bool glStateGuardTerrainEnabled();
extern const bool kTglcForceColor;
extern const int kTglcDiag;
extern const bool  kTglcCementDiagConnect;
extern const bool kTglcCementAtlasSeamFix;
extern const bool kTglcLightPoolSoft;
extern const float kTglcLightPoolMoverDim;
extern const float kTglcLightPoolMoverRadius;
extern const bool kTglcLightPoolMoverMax;
extern const bool kTglcLightPoolCompress;
extern const float kTglcMoverContactShadow;
extern const bool  kTglcSlopeBias;
extern const float kTglcSlopeBiasStr;
extern const float kTglcMacroVariation;
extern const bool kTglcEdgeFeather;
extern const float kTglcEdgeFeatherStr;
extern const bool  kTglcMatAlbedo;
extern RenderCore::GpuBuffer s_heightSsbo;
extern RenderCore::GpuBuffer s_visualHeightSsbo;
extern int    s_visualSide;
extern int    s_visualMipFloats;
extern RenderCore::GpuBuffer s_visualDampSsbo;
extern int s_visualDampSide;
extern std::vector<float> s_visualDampStatic;
extern std::vector<float> s_visualDampCombined;
extern bool s_visualDampHadMovers;
extern RenderCore::GpuBuffer s_typeSsbo;
extern RenderCore::GpuBuffer s_cementSsbo;
extern int    s_mapSide;
extern float  s_halfMap;
extern GLuint s_controlMapTex;
extern int    s_controlMapSide;
extern GLuint s_overlaySidecarTex;
extern float  s_overlayBounds[4];
extern GLuint s_shorelineMaskTex;
extern float  s_shorelineBounds[4];
extern GLuint s_waterShoreHeightTex;
extern float  s_waterShoreHeightBounds[4];
extern unsigned s_waterShoreDataGen;
extern GLuint s_matAlbedoArrayTex;
extern bool   s_matAlbedoLoadTried;
extern float s_matAlbedoStrengthJson;
void tglc_EnsureMatAlbedoArrayLoaded();
const float* mc2_chunkDetailAntiTileYZW();
#ifdef MC2_VULKAN_ISLAND
bool terrainVisualDisplaceGateOn();
#endif
#ifdef MC2_VULKAN_ISLAND
float terrainVisualDisplaceFarParam();
#endif
#ifdef MC2_VULKAN_ISLAND
extern std::vector<Mc2TerrainTier1Patch> s_vkTerrainPatches;
#endif
#ifdef MC2_VULKAN_ISLAND
extern float    s_vkTerrainMvp[16];
#endif
#ifdef MC2_VULKAN_ISLAND
extern bool     s_vkTerrainFeedValid;
#endif

#endif // GOS_TERRAIN_LOD_CHUNK_INTERNAL_H
