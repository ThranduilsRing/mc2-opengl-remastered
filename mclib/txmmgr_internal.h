// Shared surface of the txmmgr translation units (txmmgr*.cpp).
//
// txmmgr.cpp keeps the manager lifecycle, addRenderShape and the node pin/update code; the rest of
// MC_TextureManager is split by responsibility (see DOC in each piece).
#ifndef TXMMGR_INTERNAL_H
#define TXMMGR_INTERNAL_H

#ifndef TXMMGR_H
#include"txmmgr.h"
#include "gos_crashbundle.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#endif
#include "render_contract.h"  // [RENDER_PASS v1] noteRenderPass
#include"tex_resolve_table.h"
#include "diagnostic_trace.h"
#include "cone_glow.h"   // LIGHT-CONE-MODELS-1: FlushFrame at renderLists top
#include "object_light_row.h"  // LIGHT-ROW-PURE-1: the pure row producer

#ifndef TGAINFO_H
#include"tgainfo.h"
#endif

#ifndef FILE_H
#include"file.h"
#endif

#ifndef TIMING_H
#include"timing.h"
#endif

#ifndef CAMERA_H
#include"camera.h"
#endif

#ifndef LZ_H
#include"lz.h"
#endif

#ifndef CIDENT_H
#include"cident.h"
#endif

#include"../GameOS/gameos/gos_profiler.h"

#include"terrain.h"   // VPL-#shadow Phase 1+2: Terrain::mapData for the full-map static-shadow build

#ifndef PATHS_H
#include"paths.h"
#endif

#include<gameos.hpp>
#include<mlr/mlr.hpp>
#include<gosfx/gosfxheaders.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <atomic>   // FRAME-JOBS-2 precondition: g_numShadowShapes
#include <mutex>    // FRAME-JOBS-2 precondition: s_lightDataMapMu
#include <chrono>   // [LIGHTBRIDGE v1] coarse per-frame populate sizing
#include <intrin.h> // __rdtsc — [SPFLUSH_COST_SPLIT v1]
#include <utils/gl_utils.h>
#include <GL/glew.h>                 // TEXMGR-COMPRESSED-UPLOAD-1: BPTC cap + GL_COMPRESSED_* enums
#include "../RenderCore/KtxLoader.h" // TEXMGR-COMPRESSED-UPLOAD-1: BC7 .ktx2 sidecar loader
#include "gos_postprocess.h"
#include "gos_profiler.h"
#include "../GameOS/gameos/gos_static_prop_batcher.h"
#ifdef MC2_VULKAN_ISLAND
#include "../GameOS/gameos/vulkan_shadow_island_live.h"  // VK-P8a SHADOW-INTEROP: producer call
#include "../GameOS/gameos/vulkan_color_prop_island.h"   // VK-P11 STATIC-PROP COLOR: producer call
#include "../GameOS/gameos/vulkan_mech_island.h"          // VK-P14 MECH TIER-1: producer call
#include "../GameOS/gameos/vulkan_legacy_shape_island.h"  // VK-PARITY-F1: legacy MLR solid loop VK owner
#include "../GameOS/gameos/vulkan_terrain_island.h"        // VK-PARITY-OVERLAY-1: overlay/decal VK owner
#endif
#include "../GameOS/gameos/render_frame_plan.h"  // RENDER-FRAME-PLAN-SCAFFOLD-1
#include "../GameOS/gameos/render_snapshot.h"  // getLastRenderSnapshot() for snap-cull (v2.3)
#include "../GameOS/gameos/gos_static_prop_registry.h"  // Stage 3.C: flush()
#include "../GameOS/gameos/gos_mech_batcher.h"
#include "../GameOS/gameos/gos_validate.h"  // drainGLErrors (Tier-1 instr §4)
#include "../GameOS/gameos/gos_terrain_patch_stream.h"
#include "../GameOS/gameos/gos_terrain_indirect.h"
#include "../GameOS/gameos/gos_render_pass_timer.h"
#include "../GameOS/gameos/gos_terrain_bridge.h"   // [TERRAIN_SURFACE] PR-2 surface validation draw
#include "../RenderCore/terrain_path_telemetry.h"  // LEGACY-MLR-DELETE-1: tripwire noteTerrainPath(LegacyMLR)
#include "../RenderCore/top_level_pass_executor.h"  // APPLY-STATE-MECHOPAQUE-1: findTopLevelStateDesc
#include "../GameOS/gameos/gos_terrain_mask_dispatch.h"  // B4 Stage 1b: mask-SOLID draw
#include "../GameOS/gameos/gpu_cull_compute.h"  // C1b: compute_dispatch() moved here from mission.cpp
#include "../GameOS/gameos/gpu_cull_substrate.h"
#include "dynamic_decal_ring.h"  // MC2_DYNAMIC_DECALS gather before gos_DrawDecals
#include "Gates/gate.h"

// Globals defined in txmmgr.cpp (or elsewhere in the engine) that the split pieces share.
extern MC_TextureManager *mcTextureManager;
extern bool MLRVertexLimitReached;
extern bool useFog;
extern DWORD BaseVertexColor;
extern uint32_t g_mc2FrameCounter;
extern long s_txmAllocThisMission;

// Eager gate caches and counters defined in txmmgr.cpp / txmmgr_diag.cpp and read by several pieces.
extern const bool s_spflushTxmEnabled;
extern unsigned long long s_spflush_baseinstance_upload_cyc;
extern unsigned long long s_spflush_recipe_rebuilds_txm;
extern const bool s_txmLeakTrace;


// Bump on every fresh node-slot creation (the three creation paths).
inline void txmLeakNoteAlloc()
{
    if (s_txmLeakTrace) s_txmAllocThisMission++;
}

// Free functions defined in the split pieces.
bool mc2LightBridgeRepointEnabled();
bool mc2LightBakeEnabled();
uint32_t mc2StaticLightHighWater();
void mc2MarkStaticLightPrefixDirty();
bool mc2ConsumeStaticLightPrefixDirty();
bool mc2StaticLightUploadSplitEnabled();
void mc2LightBakeStabilityObserve(int32_t recipeIndex, uint32_t lightDataIndex);
bool mc2IsBakedStaticLightPresent(int32_t recipeIndex);
void mc2SetBakedStaticLight(int32_t recipeIndex, const TG_HWLightsData& in);
void mc2EraseBakedStaticLight(int32_t recipeIndex);
void mc2InvalidateBakedStaticLightCache();
void mc2ClearAllBakedStaticLight();
bool mc2IsStaticLightSlotBaked(int32_t recipeIndex);
bool mc2PeekStaticLightSlotSummary(int32_t recipeIndex, int* outNumLights, float amb[3], float sun[3]);
void mc2WriteStaticLightSlot(int32_t recipeIndex, const TG_HWLightsData& baked);
uint32_t mc2LightSlotBakedHighWater();
uint64_t mc2LightSlotDedupHits();
uint64_t mc2LightSlotActorKeyHits();
uint32_t mc2LightSlotTableCount();
void mc2LightBakeParityCheck(int32_t recipeIndex, const TG_HWLightsData* gatheredLeaf, float wx, float wy, float wz, const char* appearance);
void mc2PublishObjectLightPolicy(uint32_t classPolicy, bool sunRecolour, uint32_t sunARGB, bool sunDisabled, bool ambientRecolour, uint32_t ambientARGB, float posX, float posY, float posZ, float terrainSample);
void mc2ClearObjectLightPolicy(void);
void GatherLightsParameters(TG_HWLightsData* lights);
void ProbeGatherLightsForList(TG_LightPtr* lights, unsigned long nLights, TG_HWLightsData* out);

#endif // TXMMGR_INTERNAL_H
