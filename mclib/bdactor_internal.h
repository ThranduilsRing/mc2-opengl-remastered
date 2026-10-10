// Shared surface of the bdactor translation units (bdactor*.cpp).
// bdactor.cpp keeps the shared globals and the Bldg accessors/init/status/bounds code; the rest of the
// BldgAppearance / TreeAppearance implementation is split by responsibility (see DOC in each piece).
#ifndef BDACTOR_INTERNAL_H
#define BDACTOR_INTERNAL_H

#include <map>    // STATIC-REGISTRY-COVERAGE-RECON-1: top shape names
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include <charconv>
#include "Gates/gate_cold.h"  // gate::invalidate after the importer _putenv_s calls
#include <mutex>  // STATIC-REGISTRY-COVERAGE-RECON-1: shape map guard

// [TOBJSPLIT v1] RDTSC includes for the BldgAppearance/TreeAppearance probe
// points. __rdtsc() overhead ~5-10ns (cost_split_instrumentation_is_observer_
// effect_dominated.md). Accumulators defined in code/terrobj.cpp.
#include <intrin.h>
#include <stdlib.h>
#include <atomic>  // std::atomic — FRAME-JOBS-2 precondition (s_animTypeIdleNowStatic)

#include "gos_static_prop_killswitch.h"
#include "gos_static_prop_batcher.h"
#include "tgl_load_guard.h"  // META-FIX design B PART 1: required base-shape load guard
#include "gos_static_prop_registry.h"  // Stage 3.C: static-registry fast path
#include "cliff_decal_tuning.h"  // TERRAIN-DECAL-SLICE-0C: shared face-frame + live tuning
#include "gos_mech_killswitch.h"       // g_mechPreviewRenderDepth (component preview-fix)
#include "../GameAdapters/StaticPropRenderAdapter.h"  // M1 RenderWorld Tasks 8-11
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <GL/glew.h>
#include "../RenderCore/KtxLoader.h"
#include "../RenderCore/MaterialGpu.h"
#include "gos_object_parity_query.h"  // IsDualEmitArmed — Stage 2.D.2 dual-emit hook
#include <set>            // [LIGHTSLOT v1] Task 0 cardinality gate
#include "gos_smoke.h"    // [LIGHTSLOT v1] SmokeMode::missionHasStarted()
#include "gos_object_recon_tracy.h"  // [OBJECT_RECON v1] slice-2 recon-zero
#include "cpu_proj_cost_split.h"      // F3 CPU projection cost-baseline (RAII scope)
#include "spotlight_diag.h"  // T1.16 — (E)-owned slot tagging for per-slot probe
#include "cone_glow.h"       // LIGHT-CONE-MODELS-1 — node aim + cone glow
#include "gos_profiler.h"  // PERF DIAGNOSTIC 2026-05-06: ZoneScopedN for per-update breakdown
#include <cmath>          // CONE-SPILL-COUPLING-1 — sqrt for the ray/terrain march
#include <cstdlib>

// MODEL-OVERRIDE dual-shape (Slice 2): registry resolve + direct geometry import.
#include "model_override_registry.h"
#include "assimp_importer.h"

#ifndef CAMERA_H
#include"camera.h"
#endif

#ifndef DBASEGUI_H
#include"dbasegui.h"
#endif

#ifndef CIDENT_H
#include"cident.h"
#endif

#ifndef PATHS_H
#include"paths.h"
#endif

#ifndef OBJSTATUS_H
#include"objstatus.h"
#endif

#ifndef UTILITIES_H
#include"utilities.h"
#endif

#ifndef INIFILE_H
#include"inifile.h"
#endif

#ifndef ERR_H
#include"err.h"
#endif

#ifndef TXMMGR_H
#include"txmmgr.h"
#endif

#include"terrain_runtime.h"
#include"static_light_row_guard.h"  // ZERO-LIGHT-ROW-1: guard/forward divergence census

#ifndef TIMING_H
#include"timing.h"
#endif

#ifndef CELINE_H
#include"celine.h"
#endif

#ifndef MOVE_H
#include"move.h"
#endif

#include "../code/unitdesg.h" /* just for definition of MIN_TERRAIN_PART_ID and MAX_MAP_CELL_WIDTH */
#include "../code/static_update_counters.h" /* [TOBJSPLIT v1] g_tobjAngularCyc / g_tobjProjCyc extern decls */
#include "gos_static_prop_batcher.h"

struct TouchEntryDiag {
    uint32_t frame                    = 0xFFFFFFFFu;
    uint64_t serial_commit_hits       = 0;  // touchSerialCommit() calls (stamp set)
    uint64_t legacy_skipped           = 0;  // touch() early-returns (Path B suppressed)
    uint64_t legacy_ran_nosplit       = 0;  // touch() ran with no prior split commit
};

// Globals defined in bdactor.cpp (or elsewhere in the engine) that the split pieces share.
extern bool s_tobjSplitBdOn;
extern uint32_t s_animStartInvalidated;
extern uint32_t s_animStateToStateGesture;
extern unsigned long g_fbDiagPtSpotConsumed;
extern unsigned long g_fbDiagPtSpotZeroDir;
extern float	worldUnitsPerMeter;
extern bool 	drawTerrainGrid;
extern bool		useFog;
extern long 	mechRGBLookup[];
extern long 	mechRGBLookup2[];
extern int ObjectTextureSize;
extern bool reloadBounds;
extern float	metersPerWorldUnit;
extern bool		useShadows;
extern bool useNonWeaponEffects;
extern bool useHighObjectDetail;
extern bool MLRVertexLimitReached;

// Eager gate caches defined in bdactor.cpp (shared by the static-registry pieces).
extern const bool s_propFoliageAlphaFlag;
extern const bool s_proxyReconEnabled;
extern uint32_t g_mc2FrameCounter;  // mclib/tgl.cpp
extern TouchEntryDiag s_touchEntryDiag;
extern MidLevelRenderer::MLRClipper * theClipper;
extern std::atomic<uint32_t> s_animTypeIdleNowStatic;
extern std::atomic<int64_t> g_spr_phase2Calls;
extern std::atomic<int64_t> g_spr_bldgCalls;
extern std::atomic<int64_t> g_spr_treeCalls;
extern std::atomic<int64_t> g_spr_proxyCandidate;
extern std::atomic<int64_t> g_spr_rejNoShape;
extern std::atomic<int64_t> g_spr_rejNoStaticReg;
extern std::atomic<int64_t> g_spr_rejBadLightIdx;
extern std::atomic<int64_t> g_spr_rejNoValidLight;
extern std::atomic<int64_t> g_spr_rejLightGenMismatch;
extern std::atomic<int64_t> g_spr_rejNeedsFullBake;
extern std::atomic<int64_t> g_spr_callCounter;
extern void mc2LightBakeParityCheck(int32_t recipeIndex, const TG_HWLightsData* gatheredLeaf, float wx, float wy, float wz, const char* appearance);
void recordStableLightSkipDiag(bool armed, bool eligible, bool notRegistered, bool noValidLight, bool envGenMiss, bool needsFullBake, bool invalidLightIdx);
void lbssRecordBldg(bool eligible, bool taken, bool blk_idx, bool blk_gen, bool blk_noelig);

// Shared helpers defined in bdactor.cpp and the sidecar piece.
bool textureOrKtxSidecarExists(const char* tgaPath);
bool staticRegIsNaturalBuildingName(const char* name);
void bdLogStaticRegInvalid(const char* kind, int reason, int regIdx, const char* name);
bool buildingPbrGateEnabled();
void loadBuildingImportSourceWithSidecarAxis(TG_TypeMultiShape* shape, const char* importSourceBase, bool* failFlag);
bool loadBuildingPbrFromSidecar(BldgAppearanceType* type, const char* sidecarPath);
void submitBuildingFootprintShadow(const BldgAppearance* appearance);

// [LIGHTSLOT v1] trace entry points (state is private to bdactor_diag.cpp).
namespace mc2_lightslot_trace {
    void recordInstance(const void* appearType, bool isOverride, uint32_t lightDataIndex);
    void recordLodBakeSlot(uint32_t lightDataIndex);
}

inline bool propFoliageAlphaFlag() { return s_propFoliageAlphaFlag; }
// GPU-VENDOR-DETECT-1: defined in GameOS/gameos/gos_render.cpp. True only on
// NVIDIA GPUs. The GPU-driven static-prop stack (compute cull -> indirect
// multidraw -> persistent-mapped readback) was only validated on AMD RDNA3; on
// NVIDIA registered static props (trees/buildings) can draw nothing. Until that
// is fixed on NVIDIA hardware, default the static->dynamic force-fallback ON for
// NVIDIA so props stay visible (legacy CPU draw). AMD is unaffected.
bool gos_IsNvidiaGPU();
// env unset -> default ON for NVIDIA, OFF elsewhere; env="0" -> force OFF
// (override the NVIDIA default); env set to non-"0" -> force ON everywhere.
template <mc2::gate::Name n>
inline bool bdForceDynamicDefault() {
	if (mc2::gate::isSet<n>()) return mc2::gate::get<n>();
	return gos_IsNvidiaGPU();
}

#define SPINRATE					90.0f
#define BASE_NODE_RECYCLE_TIME		0.25f
#define MAX_WEAPON_NODES			4

// Free functions defined in the split pieces.
void LoadOverrideRenderShapeTextures(TG_TypeMultiShape* rs);
void Bldg_ForceRenderShapeTexturesResident(TG_TypeMultiShape* rs);
bool mc2StableLightSkipEnabled();
uint64_t mc2StaticLightEnvironmentGeneration(const Camera* eye);
bool mc2LightbridgeStableSkipEnabled();
bool mc2StableLightSkipTouchEnabled();
void lbssRecordTree(bool taken, bool blk_idx);
bool mc2FrameJobsTouchDiagEnabled();
void touchEntryDiagRollFrame(uint32_t frame);
void mc2CacheOrBakeStaticGpuLight(TG_MultiShape* shape, bool registered, int32_t recipeIndex);
bool mc2ConeGroundHit (const Stuff::Vector3D& apex, const Stuff::Vector3D& aim, float beamLen, Stuff::Vector3D& hitOut);
bool mc2PropLightLatchDiag (void);
uint32_t mc2PropLightLatchDiagInterval (void);
bool bldgStaticShadowRebakeGatesActive();
void bldgTriggerStaticShadowRebakeIfArmed(bool wasRegistered, int32_t recipeIndex, const char* reason);

#endif // BDACTOR_INTERNAL_H
