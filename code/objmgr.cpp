//---------------------------------------------------------------------------
//
//	objmgr.cpp - This file contains the GameObject Manager class code
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
// Include Files

//=============
#ifndef MCLIB_H
#include"mclib.h"
#endif

#ifndef DOBJMGR_H
#include"dobjmgr.h"
#endif

#include <cstdio>
#include <cmath>   // std::abs for clip.w sign-safe frustum test
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <chrono>  // FRAME-JOBS-1: prepass wall-clock timing
#include "../GameOS/gameos/diagnostic_trace.h"  // FRAME-JOBS-1: JSONL trace emit
#include "objmgr_watch_policy.h"  // OBJMGR-WATCH-POLICY-EXTRACT-1: shared watch bounds/index policy
#include "gos_static_prop_killswitch.h"  // g_useGpuStaticProps
#include "terrain_runtime.h"
#include "static_update_counters.h"      // g_staticUpdateRunCount/SkipCount/EmitSummary
#include "../GameOS/gameos/gpu_cull_substrate.h"       // C0: GPU cull substrate SSBO upload
#include "../GameOS/gameos/gpu_cull_parity.h"         // C0-4: AABB parity check
#include "../GameOS/gameos/gpu_cull_readback.h"        // C3: per-actor GPU visibility snapshot
#include "../GameOS/gameos/gos_static_prop_registry.h" // Task 5: mission-load bulk registration
#include "move_recon.h"  // MC2_MOVE_RECON per-frame pathfinding cost instrumentation
#include "object_walk_trace.h"  // OBJECT-WALK-FACTS-1 (gated, default-OFF)
#include "frame_jobs.h"  // FRAME-JOBS-1: parallelForRange worker pool
#include "brain_special_dispatch.h"  // BRAIN-COMMIT-PHASE-1: commitAllBrainIntents
#include "warrior.h"                 // BRAIN-COMMIT-PHASE-1: MechWarrior, getBrainRuntime
#include <atomic>
extern std::atomic<int> g_workerResubmitCalls;  // FRAME-JOBS-2D: msl.cpp

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef OBJTYPE_H
#include"objtype.h"
#endif

#ifndef GAMEOBJ_H
#include"gameobj.h"
#endif

#ifndef BLDNG_H
#include"bldng.h"
#endif

#ifndef UNITDESG_H
#include"unitdesg.h"
#endif

#include"gos_profiler.h"

#ifndef ARTLRY_H
#include"artlry.h"
#endif

#ifndef TURRET_H
#include"turret.h"
#endif

#ifndef GROUP_H
#include"group.h"
#endif

#ifndef TEAM_H
#include"team.h"
#endif

#ifndef COMNDR_H
#include"comndr.h"
#endif

#ifndef MECH_H
#include"mech.h"
#endif

#ifndef GVEHICL_H
#include"gvehicl.h"
#endif

#ifndef TERROBJ_H
#include"terrobj.h"
#endif

#ifndef WEAPONBOLT_H
#include"weaponbolt.h"
#endif

#ifndef CARNAGE_H
#include"carnage.h"
#endif

#ifndef LIGHT_H
#include"light.h"
#endif

#ifndef GATE_H
#include"gate.h"
#endif

#ifdef USE_ELEMENTALS
#ifndef ELEMNTL_H
#include"elemntl.h"
#endif
#endif

#ifndef COLLSN_H
#include"collsn.h"
#endif

#ifndef WEAPONFX_H
#include"weaponfx.h"
#endif

#ifndef MISSION_H
#include"mission.h"
#endif

#ifndef GOS_PROFILER_H
#include"gos_profiler.h"
#endif

// ---------------------------------------------------------------------------
// C3: env-gated lifecycle routing killswitch.
// MC2_GPU_CULL_LIFECYCLE=1 enables GPU visibility-based update() gating.
// Default off — legacy inView/canBeSeen/blockActive path unchanged.
// ---------------------------------------------------------------------------
static const bool s_gpuCullLifecycle = (getenv("MC2_GPU_CULL_LIFECYCLE") != nullptr);

// ---------------------------------------------------------------------------
// MF3-GENERATIONAL-HANDLE-1: generational watch-id validation killswitch.
// MC2_WATCHID_GENERATION present -> bump-on-free + validate-on-access via the
// watchGeneration[] side array. Default off: the side array is still allocated
// in lockstep with watchList[] (always valid) but is never read/written, so
// behavior is byte-identical to before. No slot reuse this slice.
// ---------------------------------------------------------------------------
static const bool s_watchGen = (getenv("MC2_WATCHID_GENERATION") != nullptr);

// ---------------------------------------------------------------------------
// GOM Recon: object type classification & mutation tracking (read-only).
// MC2_GOM_RECON=1 enables per-frame logging of terrain object types and counts.
// Zero overhead when disabled. Gated diagnostics only.
// ---------------------------------------------------------------------------
static const bool s_gomReconEnabled = (getenv("MC2_GOM_RECON") != nullptr);
inline bool MC2_GOM_RECON_ENABLED() { return s_gomReconEnabled; }

static bool mc2SkipStaticNaturalEnabled (void)
{
	// DEFAULT-ON (opt-out kill switch): the pure-static-natural update skip is the
	// validated R2b fast path (~4977 -> ~145 terrain-object updates on dense maps).
	// Only literal "0" opts out (bisection / revert escape hatch); unset or any other
	// value enables it. Same idiom as MC2_QUADSETUP_ARMED_SKIP / MC2_TERRAIN_LOD_CHUNK.
	// Gameplay-critical objects (gates/turrets/spotlights/special buildings with
	// alarm/lookout/sensor/power) are excluded from the skip set at the call sites,
	// so they still tick every frame regardless of this gate.
	static int cached = -1;
	if (cached < 0) {
		const char* v = getenv("MC2_SKIP_STATIC_TREES");
		cached = (v && v[0] == '0' && v[1] == '\0') ? 0 : 1;
	}
	return(cached != 0);
}

static bool mc2SkipStaticBuildingsEnabled (void)
{
	// DEFAULT-OFF (opt-in): skip all static BUILDING-class objects, not just
	// Pine-named ones.  This is new/unproven — leave OFF unless explicitly set.
	// Set MC2_SKIP_STATIC_BUILDINGS=1 (or any non-"0" value) to enable.
	// With this unset the binary is BYTE-IDENTICAL to the pre-broadening path.
	static int cached = -1;
	if (cached < 0) {
		const char* v = getenv("MC2_SKIP_STATIC_BUILDINGS");
		cached = (v && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0')) ? 1 : 0;
	}
	return(cached != 0);
}

static bool mc2SkipStaticNaturalDiagEnabled (void)
{
	static int cached = -1;
	if (cached < 0)
		cached = (getenv("MC2_SKIP_STATIC_TREES_DIAG") != nullptr) ? 1 : 0;
	return(cached != 0);
}

// R2B-STATIC-NATURAL-TOUCH-PRESERVE-1 ──────────────────────────────────────────
// DEFAULT-ON kill switch. The R2b "skip static-natural update" fast path issues a
// bare `continue` for static trees/pine-buildings at turn>=3, which skips update()
// — the producer that stamps the registered multi-shape's cachedFrame_. The
// static-prop registry flush (gos_static_prop_registry.cpp) drops any registered
// multi whose cachedFrame_ != currentFrame, so the skipped prop is silently culled
// from the instance SSBO and never drawn (the black-tree-bug class — see
// .planning/PROJECT.md "cachedFrame_ stamp" — reintroduced by the R2b skip; the
// GPU symptom is tree typeIDs entirely absent from the instanced draw). This gate
// restores the minimal liveness stamp inside the skip path WITHOUT running the
// expensive update. Set MC2_R2B_TOUCH_PRESERVE=0 to A/B (off => reproduce the
// drop; the registry stale-drop counter goes nonzero for tree typeIDs).
static bool mc2R2bTouchPreserveEnabled (void)
{
	static int cached = -1;
	if (cached < 0) {
		const char* v = getenv("MC2_R2B_TOUCH_PRESERVE");
		cached = (v && v[0] == '0' && v[1] == '\0') ? 0 : 1;  // default-ON
	}
	return(cached != 0);
}

static bool mc2R2bTouchTraceEnabled (void)
{
	static int cached = -1;
	if (cached < 0)
		cached = (getenv("MC2_R2B_STATIC_NATURAL_TRACE") != nullptr) ? 1 : 0;
	return(cached != 0);
}

// Stamp the static-registry liveness (cachedFrame_) of an appearance whose
// expensive update() the R2b fast path is about to skip. Cheap: no transform, no
// texture, no bounds — just advances the registered multi-shape's frame stamp so
// the registry flush keeps it in the instance SSBO. Returns true if any shape was
// stamped (for the touched_liveness trace counter).
static inline bool mc2R2bTouchStaticLiveness (AppearancePtr appearance, uint32_t frame)
{
	if (!appearance) return false;
	if (TreeAppearance* ta = dynamic_cast<TreeAppearance*>(appearance)) {
		bool stamped = false;
		for (long lod = 0; lod < MAX_LODS; ++lod)
			if (ta->staticReg[lod].registered && ta->staticReg[lod].shape) {
				ta->staticReg[lod].shape->setCachedFrame(frame);
				stamped = true;
			}
		return stamped;
	}
	if (BldgAppearance* ba = dynamic_cast<BldgAppearance*>(appearance)) {
		if (ba->staticReg.registered && ba->staticReg.shape) {
			ba->staticReg.shape->setCachedFrame(frame);
			return true;
		}
	}
	return false;
}

static bool mc2StaticTypeNameStartsWith (const char* typeName, const char* prefix)
{
	return(typeName && prefix && (std::strncmp(typeName, prefix, std::strlen(prefix)) == 0));
}

// alpha-Stage 1 §5 Stage 0: candidate-predicate disagreement probe.
// For each live actor each frame, compute four CANDIDATE predicates that
// the unconflated four-gate design would have driven, then accumulate
// pairwise XOR disagreement counters. The probe answers the
// adversarial-steelman deciding question: do the four read-concerns of
// the single inView bit actually demand different answers, often enough
// to justify the architectural retirement of the conflation?
//
// Candidates (per spec §5 Stage 0):
//   render_cand    = canBeSeen_instr() — routes through
//                    appearance->canBeSeen() which returns appearance->inView.
//                    This is the actual coarse-frustum bit consumers read.
//                    (v1.1 fix 2026-05-20: was inView_instr() which has
//                    only one override in the codebase — Artillery at
//                    artlry.h:215 — so it returned FALSE for every
//                    non-Artillery actor and the probe measured nothing
//                    useful. Most missions have no Artillery on screen,
//                    so the bogus 95% disagreement was effectively
//                    "non-Artillery actor through sim widening" — not
//                    a real render-vs-sim measurement.)
//   sim_cand       = render OR blockActive OR (framesSinceActive < N)
//                    (the proposed Stage 3 hysteresis-with-floor signal)
//   lifecycle_cand = TRUE for any actor visited (we got past the
//                    objList[i]==NULL skip and the implicit alive check)
//   ai_cand        = movers: readback-lagged when readback enabled, else
//                    render. statics: render. (Stage 5 producer rule.)
//
// Pairwise XOR counters tell us how often each pair of consumers would
// have demanded a different answer. Of particular interest:
//   render_vs_sim — the gate-pair-invariant violation rate (the bug
//                   class §4.3 names). Should be HIGH if hysteresis
//                   actually catches real off-screen activity.
//   render_vs_ai  — the readback-vs-coarse divergence for movers.
//                   Currently 0 unless readback is enabled.
//   sim_vs_lifecycle — how often hysteresis disagrees with "alive."
//                      Should be HIGH because most alive actors are
//                      off-screen but the disagreement is benign.
//   render_vs_lifecycle — render says no, alive says yes. ~100% of
//                         off-screen actor-frames. Reported but not
//                         a "bug-class" disagreement.
//
// Output gate per spec §5 Stage 0:
//   < 5% render_vs_sim disagreement → conflation is benign; abort spec
//   > 20% render_vs_sim disagreement → confirm META-FIX; full rollout
//
// Env-gated MC2_INVIEW_CONFLATION_TRACE=1 per debug_instrumentation_rule.md.
// 120-frame summary roll matches [TOBJPARITY v1] cadence.
static bool s_inViewConflationEnabled = (getenv("MC2_INVIEW_CONFLATION_TRACE") != nullptr);
// Hysteresis floor for sim_cand (Stage 0 tunable knob per spec §9 Q3).
// Default 4 frames matches conservative recommendation.
static const uint8_t s_inViewConflationHysteresisN = 4u;

// OBJECT-WALK-FACTS-1: TU-local accumulator for the framesSinceActive sweep.
namespace mc2_object_walk_trace { Counters g_counters = {}; }

// Pairwise XOR accumulators (frame-actor counts).
static unsigned long long g_invConfActorFrames      = 0ULL;
static unsigned long long g_invConfRenderVsSim      = 0ULL;
static unsigned long long g_invConfRenderVsLife     = 0ULL;
static unsigned long long g_invConfRenderVsAi       = 0ULL;
static unsigned long long g_invConfSimVsLife        = 0ULL;
static unsigned long long g_invConfSimVsAi          = 0ULL;
static unsigned long long g_invConfLifeVsAi         = 0ULL;
// Movers vs statics for partial breakdown.
static unsigned long long g_invConfMovers           = 0ULL;
static unsigned long long g_invConfStatics          = 0ULL;
static unsigned long long g_invConfMoverRenderVsAi  = 0ULL;
static unsigned long long g_invConfFrameCount       = 0ULL;

void g_invViewConflationRollAndMaybeEmit() {
    if (!s_inViewConflationEnabled) return;
    // 120-frame interval to match [TOBJPARITY v1] cadence.
    if (++g_invConfFrameCount % 120ULL == 0ULL) {
        // Avoid divide-by-zero in summary line.
        const unsigned long long denom = g_invConfActorFrames ? g_invConfActorFrames : 1ULL;
        fprintf(stderr,
            "[INVIEW_CONFLATION v1] event=summary actor_frames=%llu"
            " render_vs_sim=%llu (%.2f%%)"
            " render_vs_ai=%llu (%.2f%%)"
            " sim_vs_ai=%llu (%.2f%%)"
            " render_vs_life=%llu (%.2f%%)"
            " sim_vs_life=%llu (%.2f%%)"
            " life_vs_ai=%llu (%.2f%%)"
            " movers=%llu statics=%llu"
            " mover_render_vs_ai=%llu hysteresis_N=%u\n",
            g_invConfActorFrames,
            g_invConfRenderVsSim, 100.0 * (double)g_invConfRenderVsSim / (double)denom,
            g_invConfRenderVsAi,  100.0 * (double)g_invConfRenderVsAi  / (double)denom,
            g_invConfSimVsAi,     100.0 * (double)g_invConfSimVsAi     / (double)denom,
            g_invConfRenderVsLife,100.0 * (double)g_invConfRenderVsLife/ (double)denom,
            g_invConfSimVsLife,   100.0 * (double)g_invConfSimVsLife   / (double)denom,
            g_invConfLifeVsAi,    100.0 * (double)g_invConfLifeVsAi    / (double)denom,
            g_invConfMovers, g_invConfStatics,
            g_invConfMoverRenderVsAi,
            (unsigned)s_inViewConflationHysteresisN);
        fflush(stderr);
        g_invConfActorFrames     = 0ULL;
        g_invConfRenderVsSim     = 0ULL;
        g_invConfRenderVsLife    = 0ULL;
        g_invConfRenderVsAi      = 0ULL;
        g_invConfSimVsLife       = 0ULL;
        g_invConfSimVsAi         = 0ULL;
        g_invConfLifeVsAi        = 0ULL;
        g_invConfMovers          = 0ULL;
        g_invConfStatics         = 0ULL;
        g_invConfMoverRenderVsAi = 0ULL;
    }
}

// ---------------------------------------------------------------------------
// C0-4: helper — category enum → short name string for parity logging
// ---------------------------------------------------------------------------
static const char* catNameForCategory(gpu_cull::GpuActorCategory cat) {
    switch (cat) {
        case gpu_cull::Cat_Mech:       return "Mech";
        case gpu_cull::Cat_GroundVeh:  return "GV";
        case gpu_cull::Cat_Gate:       return "Gate";
        case gpu_cull::Cat_Turret:     return "Turret";
        case gpu_cull::Cat_StaticProp: return "StaticProp";
        default:                       return "Other";
    }
}

// ---------------------------------------------------------------------------
// C0-3: GPU cull record emitter
// Called inside GameObjectManager::update() loops, after update() returns,
// gated on getExists() so the emit is observer-only (no lifecycle effect).
// Category and consumerFlags are supplied by each call site.
//
// C1a: bounding radius is now derived from the concrete appearance type:
//   - Mech3DAppearance and GVAppearance: direct OBBRadius field read (no virtual call)
//   - BldgAppearance / BDActorAppearance: virtual getRadius() returns OBBRadius
//   - All other types: virtual getRadius(); fall back to 30.0f if it returns 0.0f
// ---------------------------------------------------------------------------
static void emitGpuCullRecord(GameObjectPtr obj,
                               gpu_cull::GpuActorCategory cat,
                               uint32_t consumerFlags)
{
    if (!gpu_cull::substrate_isEnabled()) return;
    AppearancePtr app = obj->getAppearance();
    if (!app) return;

    // C1a: derive bounding radius from concrete type via virtual getRadius().
    // Mech3DAppearance and GVAppearance now override getRadius() to return OBBRadius.
    // BldgAppearance and BDActorAppearance already overrode it. Others return 0.0f.
    // Fall back to 30.0f if getRadius() returns 0.0f (defensive).
    float boundingRadius = app->getRadius();
    if (boundingRadius <= 0.0f) boundingRadius = 30.0f;

    gpu_cull::GpuActorRecord rec{};
    // Store worldCenter in raw MC2 world coords (x=east, y=north, z=elev).
    // The terrainMVP (gos_GetTerrainMVPMat4) already bakes the cameraPos axis swap
    // into its row layout: AW*v = worldToClip*(-vx, vz, vy, 1)^T.
    // Feeding pre-swapped cameraPos here would double-apply the swap and mis-cull
    // everything. The CPU projectZ path does its own swap before multiplying by
    // worldToClip — so both paths agree when worldCenter = (pos.x, pos.y, pos.z).
    rec.worldCenter[0] = obj->position.x;
    rec.worldCenter[1] = obj->position.y;
    rec.worldCenter[2] = obj->position.z;
    rec.boundingRadius = boundingRadius;
    // AABB: center ± radius (C0 placeholder; C0-4 AABB parity validates).
    rec.worldAabbMin[0] = rec.worldCenter[0] - boundingRadius;
    rec.worldAabbMin[1] = rec.worldCenter[1] - boundingRadius;
    rec.worldAabbMin[2] = rec.worldCenter[2] - boundingRadius;
    rec.worldAabbMax[0] = rec.worldCenter[0] + boundingRadius;
    rec.worldAabbMax[1] = rec.worldCenter[1] + boundingRadius;
    rec.worldAabbMax[2] = rec.worldCenter[2] + boundingRadius;
    rec.category        = static_cast<uint32_t>(cat) & static_cast<uint32_t>(gpu_cull::CategoryMask);
    rec.flags           = gpu_cull::Flag_None;
    rec.actorId         = static_cast<uint32_t>(obj->getHandle());
    // C1b: populate blockIdx for the block-active rollup (C1-RB).
    // getBlockAndVertexNumber() is side-effect-free; blockNum is the index
    // into Terrain::objBlockInfo[]. Stored in rec.blockIdx (was _pad0).
    {
        int blockNum = 0, vertexNum = 0;
        obj->getBlockAndVertexNumber(blockNum, vertexNum);
        rec.blockIdx = static_cast<uint32_t>(blockNum);
    }

    // C1a: prevVisibilityBit uses the MODERN clip-space frustum test so that
    // compute_emitParitySummary() compares GPU cull vs CPU clip-space cull on the
    // same MVP (previous frame's MVP — same one the compute shader will use).
    // The legacy app->inView uses screen-rect admission which wraps behind-camera
    // actors into screen bounds (clip.w < 0 → negative rhw → coordinates flip),
    // producing false_neg=hundreds in the parity summary. The modern test correctly
    // rejects clip.w <= 0.
    // fallback: if MVP not yet valid (frame 0), use legacy inView.
    {
        uint32_t modernBit = app->inView ? 1u : 0u;
        const float* mvp = gos_GetTerrainMVPMat4();
        if (mvp) {
            // GLSL: clip = mat * vec4(center, 1) where mat = AW^T (GL_FALSE row-major upload).
            // In column-major GLSL: mat(i,j) = mvp[j*4+i].
            // clip[i] = sum_j(mat(i,j)*v[j]) = sum_j(mvp[j*4+i]*v[j]).
            const float cx = rec.worldCenter[0], cy = rec.worldCenter[1], cz = rec.worldCenter[2];
            const float clipx = mvp[0]*cx + mvp[4]*cy + mvp[8]*cz  + mvp[12];
            const float clipy = mvp[1]*cx + mvp[5]*cy + mvp[9]*cz  + mvp[13];
            const float clipz = mvp[2]*cx + mvp[6]*cy + mvp[10]*cz + mvp[14];
            const float clipw = mvp[3]*cx + mvp[7]*cy + mvp[11]*cz + mvp[15];
            // MC2 clip.w sign: Stuff matrix gives either sign for visible objects.
            // Sign-normalize so w > 0 before applying standard frustum tests,
            // mirroring what the GPU clipper does implicitly.
            // (see clip_w_sign_trap.md, terrain_tes_projection.md)
            // Lockstep with: shaders/gpu_cull_predicate.glsl, mclib/object_admission_predicate.cpp
            const float s   = (clipw < 0.0f) ? -1.0f : 1.0f;
            const float ncx = clipx * s;
            const float ncy = clipy * s;
            const float ncz = clipz * s;
            const float ncw = clipw * s;  // always >= 0
            const bool admit = (ncw > 1e-5f) &&
                               (ncx >= -ncw) && (ncx <= ncw) &&
                               (ncy >= -ncw) && (ncy <= ncw) &&
                               (ncz >= 0.0f) && (ncz <= ncw);
            modernBit = admit ? 1u : 0u;
        }
        // m-2: prevVisibilityBit reflects CPU clip-space admission, not GPU readback.
        // After C1b GPU authority flip the GPU is the authoritative visibility source;
        // this field drifts for off-screen actors that were admitted by the CPU predicates
        // but culled by the GPU frustum. The field is unused by the compute shader
        // (struct-layout only, never read in gpu_cull.comp); it drives only the
        // substrate_getCpuVisibleCount() parity summary. Wire to readback_isActorVisibleLagged
        // when C3 default-on flip happens (C3-9), so the summary reflects GPU state.
        rec.prevVisibilityBit = modernBit;
    }
    rec.consumerFlags   = consumerFlags;
    // blockIdx is set above in the getBlockAndVertexNumber() block.

    gpu_cull::parity_checkRecord(rec.actorId, catNameForCategory(cat),
                                 rec.worldCenter,
                                 obj->position.x, obj->position.y, obj->position.z);
    gpu_cull::substrate_submitDynamicActor(rec);
}

#define BRIDGE_OBJTYPE				448
#define MINE1						60
#define MINE2						251
#define MINE3						521
#define MINE4						522
#define MINE5						610
#define MINE6						611
#define MINE7						612

//---------------------------------------------------------------------------
// Static Globals
#if 1	// ids can be 8 long, INCLUDING TERMINATOR!!!!!
char DEFAULT_LIST_ID[] = "DEFAULT";
char CLANMECH_LIST_ID[] = "CLANMEC";
char ISMECH_LIST_ID[] = "ISMECH";
char ICON_LIST_ID[] = "ICONS";
char WEAPON_LIST_ID[] = "WEAPON";
#else
char DEFAULT_LIST_ID[] = "DEFAULT!";
char CLANMECH_LIST_ID[] = "CLANMECH";
char ISMECH_LIST_ID[] = "ISMECH";
char ICON_LIST_ID[] = "ICONS";
char WEAPON_LIST_ID[] = "WEAPON";
#endif
//long ObjectQueue::objectsInList = 0;

extern long* usedBlockList;			//Trust ME~~!!!!!!!!!!!!!!!!!!!!!!!!
extern long* moverBlockList;			//Trust ME~~!!!!!!!!!!!!!!!!!!!!!!!!  AGAIN
extern uint32_t g_mc2FrameCounter;	// defined at mclib/tgl.cpp:3718
extern bool updateTerrainObjects;
extern bool	updateObjects;
extern bool	renderTerrainObjects;
extern bool	renderObjects;
extern GameObjectPtr MoverRoster[MAX_MOVER_PART_ID - MIN_MOVER_PART_ID + 1];
extern bool MaxObjectsDrawn;
extern bool drawOldWay;

/*
ObjectQueuePtr objectList = NULL;
ObjectQueueNodePtr clanMechList = NULL;
ObjectQueueNodePtr innerSphereMechList = NULL;
ObjectQueueNodePtr iconList = NULL;
ObjectQueueNodePtr weaponList = NULL;
*/

#define	VISIBLE_THRESHOLD	1

GameObjectManagerPtr ObjectManager = NULL;
GameObjectPtr* collisionList = NULL;
long numCollidables = 0;
//***************************************************************************
//* MISC routines
//***************************************************************************

bool blockInList (long blockNum) 
{
	long totalBlocks = Terrain::blocksMapSide * Terrain::blocksMapSide;

	for (long i = 0; i < totalBlocks; i++) {
		if (usedBlockList[i] == blockNum)
			return(TRUE);
		else if (usedBlockList[i] == -1)
			return(FALSE);
	}
	
	return(FALSE);
}

//---------------------------------------------------------------------------

bool moverInList (long blockNum) 
{
	long totalBlocks = Terrain::blocksMapSide * Terrain::blocksMapSide;

	for (long i = 0; i < totalBlocks; i++) {
		if (moverBlockList[i] == blockNum)
			return(TRUE);
		else if (moverBlockList[i] == -1)
			return(FALSE);
	}
	
	return(FALSE);
}

//***************************************************************************
//* GAMEOBJECT MANAGER class
//***************************************************************************

void* GameObjectManager::operator new (size_t ourSize) {

	void* result = systemHeap->Malloc(ourSize);
	if (!result) 
	{
		Fatal(0, " GameObjectManager.new: unable to create GameObject Manager ");
	}

	return(result);
}

//---------------------------------------------------------------------------

void GameObjectManager::operator delete (void* us) {

	systemHeap->Free(us);
}

//---------------------------------------------------------------------------

void GameObjectManager::init (void) {

	objTypeManager = NULL;

	numMechs = 0;
	numVehicles = 0;
	numElementals = 0;
	numTerrainObjects = 0;
	numBuildings = 0;
	numTurrets = 0;
	numWeapons = 0;
	numCarnage = 0;
	numLights = 0;
	numGates = 0;
	numArtillery = 0;

	numGateControls = 0;
	numTurretControls = 0;
	numPowerGenerators = 0;
	numSpecialBuildings = 0;

	//carnageCount = 0;
	Terrain::numObjBlocks = 0;

	mechs = NULL;
	vehicles = NULL;
	elementals = NULL;
	terrainObjects = NULL;
	buildings = NULL;
	turrets = 0;
	weapons = NULL;
	carnage = NULL;
	lights = NULL;
	artillery = NULL;
	gates = NULL;

	objList = NULL;
	collidableList = NULL;
	numCollidables = 0;
	numGoodMovers = 0;
	numBadMovers = 0;
	numMovers = 0;
	nextReinforcementPartId = MIN_REINFORCEMENT_PART_ID;
	numRemoved = 0;
	nextWatchID = 1;

	currentWeaponsIndex = 0;
	currentCarnageIndex = 0;
	currentLightIndex = 0;
	currentArtilleryIndex = 0;
	
	long totalBlocks = Terrain::blocksMapSide * Terrain::blocksMapSide;

	for (long i = 0; i < totalBlocks; i++) {
		Terrain::objBlockInfo[i].active = false;
		Terrain::objBlockInfo[i].firstHandle = 0;
		Terrain::objBlockInfo[i].numObjects = 0;
	}

}

//---------------------------------------------------------------------------

void GameObjectManager::init (const char* objTypeDataFile, long objTypeHeapSize, long objHeapSize) {

	if (objTypeManager)
		delete objTypeManager;

	objTypeManager = new ObjectTypeManager;
	if (!objTypeManager)
		Fatal(0, " GameObjectManager.init: unable to create objTypeManager ");
	long result = objTypeManager->init(objTypeDataFile, objTypeHeapSize, objHeapSize);
	if (result != NO_ERR)
		Fatal(0, " GameObjectManager.init: unable to init objTypeManager ");
}

//---------------------------------------------------------------------------

void GameObjectManager::setNumObjects (long nMechs,
									   long nVehicles,
									   long nElementals,
									   long nTerrainObjects,
									   long nBuildings,
									   long nTurrets,
									   long nWeapons,
									   long nCarnage,
									   long nLights,
									   long nArtillery,
									   long nGates) 
{

	long i=0;

	numMechs = nMechs;
	numVehicles = nVehicles;
	numElementals = nElementals;

	//-------------------------------------------------------------------------
	// We need to make room for a potential max # of reinforcements per team...
	//
	// Please DONT add 2 to each of these.   You wind up pointing into the next list!!!!
	// -fs   6/14/2000
	maxMechs = numMechs + MAX_TEAMS * MAX_REINFORCEMENTS_PER_TEAM;
	maxVehicles = numVehicles + MAX_TEAMS * MAX_REINFORCEMENTS_PER_TEAM;
	maxMovers = maxMechs + maxVehicles + numElementals;

	if (nTerrainObjects > -1)
		numTerrainObjects = nTerrainObjects;
	if (nBuildings > -1)
		numBuildings = nBuildings;
	if (nTurrets > -1)
		numTurrets = nTurrets;
	if (nWeapons > -1)
		numWeapons = nWeapons;
	if (nCarnage > -1)
		numCarnage = nCarnage;
	if (nLights > -1)
		numLights = nLights;
	if (nArtillery > -1)
		numArtillery = nArtillery;
	if (nGates > -1)
		numGates = nGates;

	GameObject::setInitialize(true);

	//-----------------------------------------------------------
	// First element in list is NULL (handle of 0 is always NULL)
	objList = (GameObjectPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(GameObjectPtr) * (getMaxObjects() + 1));
	memset(objList,0,sizeof(GameObjectPtr) * (getMaxObjects() + 1));

	watchList = (GameObjectPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(GameObjectPtr) * (getMaxObjects() + 1));
	memset(watchList,0,sizeof(GameObjectPtr) * (getMaxObjects() + 1));

	// MF3-GENERATIONAL-HANDLE-1: generation side array, LOCKSTEP with watchList
	// (same size, same memset-0). Allocated unconditionally so the pointer is
	// always valid; only read/written under s_watchGen. Shares watchList's
	// objectCache lifecycle (no discrete Free; reclaimed with the cache).
	watchGeneration = (uint16_t*)ObjectTypeManager::objectCache->Malloc(sizeof(uint16_t) * (getMaxObjects() + 1));
	memset(watchGeneration,0,sizeof(uint16_t) * (getMaxObjects() + 1));

	long curHandle = 1;
	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (maxMechs > 0) {
		mechs = (BattleMechPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(BattleMechPtr) * maxMechs);
		if (!mechs)
			Fatal(maxMechs, " GameObjectManager.setNumObjects: cannot malloc mechs ");
		for (i = 0; i < maxMechs; i++) {
			mechs[i] = new BattleMech;
			mechs[i]->setHandle(curHandle);
			objList[curHandle++] = mechs[i];
		}
		for (i = numMechs; i < maxMechs; i++)
			MC2_DESTROY(mechs[i], "pool_unused");
	}

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (maxVehicles > 0) {
		vehicles = (GroundVehiclePtr*)ObjectTypeManager::objectCache->Malloc(sizeof(GroundVehiclePtr) * maxVehicles);
		if (!vehicles)
			Fatal(maxVehicles, " GameObjectManager.setNumObjects: cannot malloc vehicles ");
		for (i = 0; i < maxVehicles; i++) {
			vehicles[i] = new GroundVehicle;
			vehicles[i]->setHandle(curHandle);
			objList[curHandle++] = vehicles[i];
		}
		for (i = numVehicles; i < maxVehicles; i++)
			MC2_DESTROY(vehicles[i], "pool_unused");
	}

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
#ifdef USE_ELEMENTALS
	if (numElementals > 0) {
		elementals = (ElementalPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(ElementalPtr) * numElementals);
		if (!elementals)
			Fatal(numElementals, " GameObjectManager.setNumObjects: cannot malloc elementals ");
		for (i = 0; i < numElementals; i++) {
			elementals[i] = new Elemental;
			elementals[i]->setHandle(curHandle);
			objList[curHandle++] = elementals[i];
		}
	}
#endif

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (numTerrainObjects > 0) {
		terrainObjects = (TerrainObjectPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(TerrainObjectPtr) * numTerrainObjects);
		if (!terrainObjects)
			Fatal(numTerrainObjects, " GameObjectManager.setNumObjects: cannot malloc terrain objects ");

		for (i = 0; i < numTerrainObjects; i++) {
			terrainObjects[i] = new TerrainObject;
			terrainObjects[i]->setHandle(curHandle);
			objList[curHandle++] = terrainObjects[i];
		}
	}

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (numBuildings > 0) {
		buildings = (BuildingPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(BuildingPtr) * numBuildings);
		if (!buildings)
			Fatal(numBuildings, " GameObjectManager.setNumObjects: cannot malloc buildings ");
		for (i = 0; i < numBuildings; i++) {
			buildings[i] = new Building;
			buildings[i]->setHandle(curHandle);
			objList[curHandle++] = buildings[i];
		}
	}

	if (numTurrets > 0)
	{
		turrets = (TurretPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(TurretPtr) * numTurrets);
		if ( !turrets )
			Fatal(numTurrets, " GameObjectManager.setNumObjects: cannot malloc Turrets ");
			
		for (i = 0; i < numTurrets; i++) 
		{
			turrets[i] = new Turret;
			turrets[i]->setHandle(curHandle);
			objList[curHandle++] = turrets[i];
		}
	}

	if (numGates > 0)
	{
		gates = (GatePtr*)ObjectTypeManager::objectCache->Malloc(sizeof(GatePtr) * numGates);
		if ( !gates )
			Fatal(numGates, " GameObjectManager.setNumObjects: cannot malloc Gates ");
			
		for (i = 0; i < numGates; i++) 
		{
			gates[i] = new Gate;
			gates[i]->setHandle(curHandle);
			objList[curHandle++] = gates[i];
		}
	}

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (numWeapons > 0) {
		weapons = (WeaponBoltPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(WeaponBoltPtr) * numWeapons);
		if (!weapons)
			Fatal(numWeapons, " GameObjectManager.setNumObjects: cannot malloc weapons ");
		for (i = 0; i < numWeapons; i++) {
			weapons[i] = new WeaponBolt;
			weapons[i]->setHandle(curHandle);
			objList[curHandle++] = weapons[i];
		}
	}

	if (numCarnage > 0) {
		carnage = (CarnagePtr*)ObjectTypeManager::objectCache->Malloc(sizeof(CarnagePtr) * numCarnage);
		if (!carnage)
			Fatal(numCarnage, " GameObjectManager.setNumObjects: cannot malloc carnage ");
		for (i = 0; i < numCarnage; i++) {
			carnage[i] = new Carnage;
			carnage[i]->setHandle(curHandle);
			objList[curHandle++] = carnage[i];
		}
	}

	if (numArtillery > 0) {
		artillery = (ArtilleryPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(ArtilleryPtr) * numArtillery);
		if (!artillery)
			Fatal(numArtillery, " GameObjectManager.setNumObjects: cannot malloc artillery ");
		for (i = 0; i < numArtillery; i++) {
			artillery[i] = new Artillery;
			artillery[i]->setHandle(curHandle);
			objList[curHandle++] = artillery[i];
		}
	}

	useMoverLineOfSightTable = true;
	moverLineOfSightTable = (char*)systemHeap->Malloc(maxMovers * maxMovers);
	if (!moverLineOfSightTable)
		Fatal(numGates, " GameObjectManager.setNumObjects: cannot malloc moverLineOfSightTable ");

	GameObject::setInitialize(false);
}

//---------------------------------------------------------------------------

BattleMechPtr GameObjectManager::getMech (long mechIndex) {

	if (!mechs || (mechIndex < 0) || (mechIndex >= numMechs))
		return(NULL);

	return(mechs[mechIndex]);
}

//---------------------------------------------------------------------------

GroundVehiclePtr GameObjectManager::getVehicle (long vehicleIndex) {

	if (!vehicles || (vehicleIndex < 0) || (vehicleIndex >= numVehicles))
		return(NULL);

	return(vehicles[vehicleIndex]);
}

//---------------------------------------------------------------------------

GroundVehiclePtr GameObjectManager::getOpenVehicle (void)
{
	for (long i=0;i<maxVehicles;i++)
	{
		if (!vehicles[i]->getExists())
		{
			if (i > numVehicles)
				numVehicles = i;

			return vehicles[i];
		}
	}

	return NULL;
}

//---------------------------------------------------------------------------

ElementalPtr GameObjectManager::getElemental (long elementalIndex) {

	if (!elementals || (elementalIndex < 0) || (elementalIndex >= numElementals))
		return(NULL);

	return(elementals[elementalIndex]);
}

//---------------------------------------------------------------------------

BattleMechPtr GameObjectManager::newMech (void) {

	if (numMechs < maxMechs) {
		mechs[numMechs]->init(false);
		mechs[numMechs]->setExists(true);
		setPartId(mechs[numMechs], -1, -1, -1);
		mechs[numMechs]->watchID = 0;
		
		//Gotta rebuild the list or the new stuff don't show up!
		// This flag will force rebuild at next collision check
		rebuildCollidableList = true;
		
		return(mechs[numMechs++]);
	}

	return(NULL);
}

//---------------------------------------------------------------------------

GroundVehiclePtr GameObjectManager::newVehicle (void) {

	if (numVehicles < maxVehicles) {
		vehicles[numVehicles]->init(false);
		vehicles[numVehicles]->setExists(true);
		setPartId(vehicles[numVehicles], -1, -1, -1);
		vehicles[numVehicles]->watchID = 0;
		
		//Gotta rebuild the list or the new stuff don't show up!
		// This flag will force rebuild at next collision check
		rebuildCollidableList = true;
		
 		return(vehicles[numVehicles++]);
	}

	return(NULL);
}

//---------------------------------------------------------------------------

// WATCHID-LOAD-GUARD-1: rate-limited diagnostic for invalid watch data read
// from an untrusted/corrupt/cross-version save. Emits a one-time stderr line
// (visible without any diag tag) plus a rate-limited WATCHID_LOAD JSONL event.
static void mc2_watchidLoadDiag(const char* what, long a, long b)
{
	static unsigned long s_loadDrops = 0;
	++s_loadDrops;
	if (s_loadDrops == 1)
		fprintf(stderr, "[WATCHID_LOAD] invalid watch data on load (%s a=%ld b=%ld); "
			"slot invalidated. Further occurrences rate-limited.\n", what, a, b);
	if (mc2_diag::tagEnabled("WATCHID_LOAD") &&
		(s_loadDrops == 1 || (s_loadDrops & 0xFF) == 0)) {
		char _wl_buf[160];
		snprintf(_wl_buf, sizeof(_wl_buf),
			"{\"what\":\"%s\",\"a\":%ld,\"b\":%ld,\"drops\":%lu}", what, a, b, s_loadDrops);
		mc2_diag::writeEvent("WATCHID_LOAD", 1, 0, _wl_buf);
	}
}

//---------------------------------------------------------------------------

void GameObjectManager::setWatchID (GameObjectPtr obj) {

	if (obj->watchID == 0) {
		// OBJMGR-WATCHID-BOUNDS-1: watchList is sized getMaxObjects()+1 at
		// allocation and getMaxObjects() never grows after load, so valid
		// indices are [0, getMaxObjects()]. On watch-ID exhaustion (long
		// reinforcement churn burns a fresh slot per re-acquire) fail safe:
		// leave the object un-watchable (watchID stays 0) instead of writing
		// OOB. Reuse is deliberately avoided — it would reintroduce ABA /
		// stale-target hazards. The Save-side clamp is independent (see ::Save).
		if (!mc2watch::canAssignWatchId(nextWatchID, getMaxObjects())) {
			static unsigned long s_watchIdDrops = 0;
			++s_watchIdDrops;
			if (s_watchIdDrops == 1)
				fprintf(stderr, "[WATCHID_BOUND] watch-ID space exhausted "
					"(cap=%ld); object left un-watchable. Further drops rate-limited.\n",
					getMaxObjects());
			if (mc2_diag::tagEnabled("WATCHID_BOUND") &&
				(s_watchIdDrops == 1 || (s_watchIdDrops & 0xFF) == 0)) {
				char _wb_buf[128];
				snprintf(_wb_buf, sizeof(_wb_buf),
					"{\"nextWatchID\":%lu,\"cap\":%ld,\"drops\":%lu}",
					nextWatchID, getMaxObjects(), s_watchIdDrops);
				mc2_diag::writeEvent("WATCHID_BOUND", 1, 0, _wb_buf);
			}
			return;
		}
		watchList[nextWatchID] = obj;
		// MF3-GENERATIONAL-HANDLE-1: stamp the slot's generation on assign (gated).
		// No reuse this slice, so nextWatchID is a fresh slot (gen 0 -> 1).
		if (s_watchGen)
			watchGeneration[nextWatchID] = mc2watch::nextGenerationOnAssign(watchGeneration[nextWatchID]);
		obj->watchID = nextWatchID++;
	}
}

//---------------------------------------------------------------------------
// MF3-GENERATIONAL-HANDLE-1: additive generational resolve. When the gate is
// OFF this is behavior-identical to getByWatchID (generation ignored). When ON,
// a handle whose expectedGen no longer matches the slot's current generation is
// rejected (fail-safe NULL + rate-limited diag), the same shape as an
// unresolvable id. No hard assert: a stale result is a recoverable drop, like
// the existing un-watchable / load-invalidate fail-safes. Zero callers this
// slice; exercised by gate-ON proof only.
GameObjectPtr GameObjectManager::getByWatchIDGenerational (unsigned long watchID, uint16_t expectedGen) {

	if (!mc2watch::isResolvableWatchId(watchID, nextWatchID))
		return NULL;
	if (s_watchGen && !mc2watch::generationMatches(watchGeneration[watchID], expectedGen)) {
		static unsigned long s_watchStaleDrops = 0;
		++s_watchStaleDrops;
		if (mc2_diag::tagEnabled("WATCHID_STALE") &&
			(s_watchStaleDrops == 1 || (s_watchStaleDrops & 0xFF) == 0)) {
			char _ws_buf[160];
			snprintf(_ws_buf, sizeof(_ws_buf),
				"{\"watchID\":%lu,\"expected\":%u,\"stored\":%u,\"drops\":%lu}",
				watchID, (unsigned)expectedGen,
				(unsigned)watchGeneration[watchID], s_watchStaleDrops);
			mc2_diag::writeEvent("WATCHID_STALE", 1, 0, _ws_buf);
		}
		return NULL;
	}
	return watchList[watchID];
}

//---------------------------------------------------------------------------

void GameObjectManager::freeMover (MoverPtr mover) {

	bool foundIt = modifyMoverLists(mover, MOVERLIST_DELETE);
	if (foundIt) {
		mover->release();
		MC2_DESTROY(mover, "mover_freed");
		mover->setFlag(OBJECT_FLAG_REMOVED, true);
		mover->setPartId(0);
		// MF3-GENERATIONAL-HANDLE-1: bump the slot's generation on free (gated) so
		// any cross-frame handle captured at the old generation reads as stale.
		if (s_watchGen)
			watchGeneration[mover->watchID] = mc2watch::bumpGenerationOnFree(watchGeneration[mover->watchID]);
		watchList[mover->watchID] = NULL;
		mover->watchID = 0;
	}
}

//---------------------------------------------------------------------------

void GameObjectManager::tradeMover (MoverPtr mover, long newTeamID, long newCommanderID) {

	if (newTeamID > -1) {
		if (mover->teamId != newTeamID)
			mover->salvaged = true;
		if (mover->getObjectClass() == BATTLEMECH) {
			((BattleMech*)mover)->killed = false;
			((BattleMech*)mover)->lost = false;
		}
		mover->tradeRefresh();
	}
	mover->setTeamId(newTeamID, true);
	if (mover->getGroup())
		mover->getGroup()->remove(mover);
	mover->watchID = 0;
	if (newTeamID > -1)
		Team::teams[newTeamID]->addToRoster(mover);
	mover->setCommanderId(newCommanderID);
	modifyMoverLists(mover, MOVERLIST_TRADE);
}

//---------------------------------------------------------------------------

#ifdef USE_ELEMENTALS

ElementalPtr GameObjectManager::addElemental (void) {

	if (numMechs < maxMechs) {
		elementals[numElementals]->setExists(true);
		setPartId(elementals[numElementals], -1, -1, -1);
		return(elementals[numElementals++]);
	}
	return(NULL);
}

#endif

//---------------------------------------------------------------------------

TerrainObjectPtr GameObjectManager::getTerrainObject (long terrainObjectIndex) {

	if (!terrainObjects || (terrainObjectIndex < 0) || (terrainObjectIndex >= numTerrainObjects))
		return(NULL);

	return(terrainObjects[terrainObjectIndex]);
}

//---------------------------------------------------------------------------

BuildingPtr GameObjectManager::getBuilding (long buildingIndex) {

	if (!buildings || (buildingIndex < 0) || (buildingIndex >= numBuildings))
		return(NULL);

	return(buildings[buildingIndex]);
}

//---------------------------------------------------------------------------

TurretPtr GameObjectManager::getTurret (long turretIndex) 
{
	if (!turrets || (turretIndex < 0) || (turretIndex >= numTurrets))
		return(NULL);

	return(turrets[turretIndex]);
}

//---------------------------------------------------------------------------

GatePtr GameObjectManager::getGate (long gateIndex) 
{
	if (!gates || (gateIndex < 0) || (gateIndex >= numGates))
		return(NULL);

	return(gates[gateIndex]);
}

//---------------------------------------------------------------------------

WeaponBoltPtr GameObjectManager::getWeapon (void) 
{
	if (!weapons || (currentWeaponsIndex < 0) || (currentWeaponsIndex >= numWeapons))
		return(NULL);

	currentWeaponsIndex++;
	if (currentWeaponsIndex >= numWeapons)
		currentWeaponsIndex = 0;

	//Make sure this weapon has an opportunity to damage its target
	weapons[currentWeaponsIndex]->finishNow();	

	return(weapons[currentWeaponsIndex]);
}

//---------------------------------------------------------------------------

CarnagePtr GameObjectManager::getCarnage (CarnageEnumType carnageType) {

	if (!carnage || (currentCarnageIndex < 0) || (currentCarnageIndex >= numCarnage))
		return(NULL);

	currentCarnageIndex++;
	if (currentCarnageIndex >= numCarnage)
		currentCarnageIndex = 0;

	carnage[currentCarnageIndex]->finishNow();
	carnage[currentCarnageIndex]->init(carnageType);
	
	return(carnage[currentCarnageIndex]);
}

//---------------------------------------------------------------------------

void GameObjectManager::releaseCarnage (CarnagePtr obj) {

	obj->setOwner(NULL);
	MC2_DESTROY(obj, "pool_released");
}

//---------------------------------------------------------------------------

LightPtr GameObjectManager::getLight (void) {

	if (!lights || (currentLightIndex < 0) || (currentLightIndex >= numLights))
		return(NULL);

	if (currentLightIndex >= numLights)
		currentLightIndex = 0;

//	lights[currentLightIndex]->finishNow();
	lights[currentLightIndex]->init(false);
	lights[currentLightIndex]->setExists(true);
	return(lights[currentLightIndex++]);
}

//---------------------------------------------------------------------------

void GameObjectManager::releaseLight (LightPtr obj) {

	MC2_DESTROY(obj, "pool_released");
//	obj->setOwner(NULL);
}

//---------------------------------------------------------------------------

ArtilleryPtr GameObjectManager::getArtillery (void) 
{
	if (!artillery || (currentArtilleryIndex < 0) || (currentArtilleryIndex >= numArtillery))
		STOP(("Artillery Strikes are out of range in ObjectManager %d",currentArtilleryIndex));

	currentArtilleryIndex++;
	if (currentArtilleryIndex >= numArtillery)
		currentArtilleryIndex = 0;

	//OK to return NULL now.  Lets Multiplayer know that there are no more strikes available.
	if (artillery[currentArtilleryIndex]->getExists())
		return NULL;
		
	artillery[currentArtilleryIndex]->init(false);
	artillery[currentArtilleryIndex]->setExists(true);
	
	return(artillery[currentArtilleryIndex]);
}

//---------------------------------------------------------------------------
#define	MAX_TERRAIN_OBJECTS		3000

#define OLD_FILE_SIZE	2200

void GameObjectManager::countTerrainObjects (PacketFile* terrainFile, long firstHandle) 
{
	int packet = terrainFile->getCurrentPacket();
	int size = terrainFile->getPacketSize();
	MemoryPtr pBuffer = new unsigned char[size];

#ifdef _DEBUG	
	int bytesRead = 
#endif
		terrainFile->readPacket( packet, pBuffer );
	gosASSERT( bytesRead == size );
	
	File* terrainObjectFile = new File;
	terrainObjectFile->open( (char*)pBuffer, size );
	
	totalObjCount = terrainObjectFile->readInt();

	if (totalObjCount)
	{
		objData = (ObjDataLoader *)systemHeap->Malloc(sizeof(ObjDataLoader) * totalObjCount);
		memset(objData,0,sizeof(ObjDataLoader) * totalObjCount);
	}
	else
	{
		objData = NULL;
	}
	
	ObjDataLoader *data = objData;
	for ( int i = 0; i < totalObjCount; ++i )
	{
		data->objTypeNum = terrainObjectFile->readInt();
		data->vector.x = terrainObjectFile->readFloat();
		data->vector.y = terrainObjectFile->readFloat();
		data->vector.z = terrainObjectFile->readFloat();

		data->rotation = terrainObjectFile->readFloat();		
		data->damage = terrainObjectFile->readInt();

		data->teamId = terrainObjectFile->readInt();
		data->parentId = terrainObjectFile->readInt();
		
		// padding
		terrainObjectFile->readInt();
		terrainObjectFile->readInt();

		// convert to block and vertex
		int tileCol;
		int tileRow;

		land->worldToTile( data->vector, tileRow, tileCol );

		int blockI = tileCol/20;
		int blockJ = tileRow/20;
		
		data->blockNumber = blockJ * land->blocksMapSide + blockI;
		tileCol -= blockI * 20;
		tileRow -= blockJ * 20;
		
		data->vertexNumber = tileRow * 20 + tileCol;
		countObject(data);
		
		data++;
	}

	// fix up handles
	long curHandle = firstHandle;
	for (int i = 0; i < Terrain::numObjBlocks; ++i )
	{
		Terrain::objBlockInfo[i].firstHandle = curHandle;
		curHandle += Terrain::objBlockInfo[i].numObjects;						
	}

	delete terrainObjectFile;
	delete[] pBuffer;

}

void GameObjectManager::countObject( ObjDataLoader *data)
{
	ObjectTypePtr objType = objTypeManager->get(data->objTypeNum);
	if (!objType)
		objType = objTypeManager->load(data->objTypeNum);
	
	if (!objType)
		return;
			//Fatal(objDataBlock[i].objTypeNum, " GameObjectManager.countTerrainObjects: bad objType ");
		switch (objType->getObjectClass()) 
		{
			case TERRAINOBJECT:
			case TREE:
				Terrain::objBlockInfo[data->blockNumber].numCollidableObjects++;
				numTerrainObjects++;
				break;
			case TURRET:
				Terrain::objBlockInfo[data->blockNumber].numCollidableObjects++;
				numTurrets++;
				break;
			case GATE:
				Terrain::objBlockInfo[data->blockNumber].numCollidableObjects++;
				numGates++;
				break;
			case KLIEG_LIGHT:
				// Klieg lights are placed terrain objects loaded as BuildingType.
				// Count as buildings but skip the BuildingTypePtr sensor checks.
				numBuildings++;
				break;
			case BUILDING:
			case TREEBUILDING:
			if (((((BuildingTypePtr)objType)->perimeterAlarmRange > 0.0f) &&
				(((BuildingTypePtr)objType)->perimeterAlarmTimer > 0.0f)) ||
				(((BuildingTypePtr)objType)->lookoutTowerRange > 0.0f) ||
				(((BuildingTypePtr)objType)->sensorRange > 0.0f))
				{
					Terrain::objBlockInfo[data->blockNumber].numCollidableObjects++;
				}
				numBuildings++;
				break;
			case BRIDGE:
				numTerrainObjects++;
				break;
			default:
				Fatal(objType->getObjectClass(), " GameObjectManager.countTerrainObjects: bad object type ");
		}
		Terrain::objBlockInfo[data->blockNumber].numObjects++;
}

//---------------------------------------------------------------------------

inline bool isLandMine (long objTypeNum) 
{
	return false;
}

//---------------------------------------------------------------------------

long GameObjectManager::getSpecificObjects (long objClass, long objSubType, GameObjectPtr* objects, long maxObjects) {

	long numValidObjects = 0;
	for (long i = 0; i < getNumObjects(); i++) {
		GameObjectPtr obj = objList[i];
		if (obj && (obj->getObjectClass() == objClass) && (obj->getObjectType()->getSubType() == objSubType)) {
			if (numValidObjects == maxObjects)
				Fatal(objClass, " GameObjectManager.getSpecificObjects: too many ");
			objects[numValidObjects++] = obj;
		}
	}
	return(numValidObjects);
}

//---------------------------------------------------------------------------

void GameObjectManager::loadTerrainObjects (PacketFile* terrainFile,
											volatile float& progress, float progressRange ) 
{
	ZoneScopedN("GameObjectManager::loadTerrainObjects");
	long curTerrainObjectIndex = 0;
	long curBuildingIndex = 0;
	long curTurretIndex = 0;
	long curGateIndex = 0;

	//------------------------------------------------------------------
	long* handles = new long[2 * Terrain::numObjBlocks];

	for ( int i = 0; i < Terrain::numObjBlocks; ++i )
	{
		handles[2 * i] = Terrain::objBlockInfo[i].firstHandle;
		handles[(2 * i) + 1] = Terrain::objBlockInfo[i].firstHandle + Terrain::objBlockInfo[i].numCollidableObjects;
	}

	ObjDataLoader *data = objData;

	float increment = 0.0f;
	if (totalObjCount)
		increment = progressRange/totalObjCount;
	{
		ZoneScopedN("GameObjectManager::loadTerrainObjects addObjectLoop");
		for (int i = 0; i < totalObjCount; ++i )
		{
			addObject( data, 
				curTerrainObjectIndex, curBuildingIndex, 
				curTurretIndex, curGateIndex, handles[2 * data->blockNumber], 
				handles[(2 * data->blockNumber) + 1] 
				);

			data++;
			progress += increment;
		}
	}

	delete[] handles;
	handles = NULL;
	
	//Done loading the objects, free the memory holding them!!
	systemHeap->Free(objData);
	objData = NULL;

	//---------------------------------------------------
	// Finally, let's build the control building lists...
	{ ZoneScopedN("GameObjectManager::loadTerrainObjects turretLinks");
	for (int i = 0; i < numTurrets; i++) 
	{
		if ((turrets[i]->parentId != 0xffffffff) && (turrets[i]->parentId != 0)) 
		{
			BuildingPtr controlBuilding = (BuildingPtr)ObjectManager->findByCellPosition((turrets[i]->parentId>>16),(turrets[i]->parentId & 0x0000ffff));
			if (controlBuilding && !controlBuilding->getFlag(OBJECT_FLAG_CONTROLBUILDING)) 
			{
				controlBuilding->setFlag(OBJECT_FLAG_CAPTURABLE, true);
				controlBuilding->setFlag(OBJECT_FLAG_CONTROLBUILDING, true);
				controlBuilding->listID = numTurretControls;
				turretControls[numTurretControls++] = controlBuilding;
			}
			else if (!controlBuilding)
			{
				Stuff::Vector3D worldPos;
				if (land)
					land->cellToWorld((turrets[i]->parentId>>16),(turrets[i]->parentId & 0x0000ffff),worldPos);
				PAUSE(("Turret linked to bldg @ R %d, C %d  X:%f Y:%f No Bldg there!",(turrets[i]->parentId>>16),(turrets[i]->parentId & 0x0000ffff),worldPos.x,worldPos.y));
				turrets[i]->parentId = 0xffffffff;
			}
		}
	}
	}

	{ ZoneScopedN("GameObjectManager::loadTerrainObjects gateLinks");
	for (int i = 0; i < numGates; i++) 
	{
		if ((gates[i]->parentId != 0xffffffff) && (gates[i]->parentId != 0))
		{
			BuildingPtr controlBuilding = (BuildingPtr)ObjectManager->findByCellPosition((gates[i]->parentId>>16),(gates[i]->parentId & 0x0000ffff));
			if (controlBuilding && !controlBuilding->getFlag(OBJECT_FLAG_CONTROLBUILDING)) 
			{
				controlBuilding->setFlag(OBJECT_FLAG_CONTROLBUILDING, true);
				controlBuilding->listID = numGateControls;
				gateControls[numGateControls++] = controlBuilding;
			}
	   		else if (!controlBuilding)
	   		{
	   			PAUSE(("Gate tried to link to building at Row %d, Col %d.  No Bldg there!",(gates[i]->parentId>>16),(gates[i]->parentId & 0x0000ffff)));
				gates[i]->parentId = 0xffffffff;
	   		}
		}
	}
	}

	//-----------------------------------------------------------------------------------
	// Create list of special buildings.  These buildings will be updated at least once
	// every frame regardless of where they are on the terrain and what is visible.
	// This is because perimeter alarms and lookout buildings and ????? must work even
	// if the player is NOT looking at them!!
	{ ZoneScopedN("GameObjectManager::loadTerrainObjects specialBuildings");
	for (int i=0;i<numBuildings;i++)
	{
		if (buildings[i]->isSpecialBuilding())
		{
			specialBuildings[numSpecialBuildings++] = buildings[i];
		}
	}
	}
	
 	//--------------------------------------------------------------------
	//Now, lets point every lit building to at least one power generator.
	{ ZoneScopedN("GameObjectManager::loadTerrainObjects powerGenerators");
	for (int i=0;i<numBuildings;i++)
	{
		if (buildings[i]->isPowerSource())
		{
			powerGenerators[numPowerGenerators++] = buildings[i];
		}
	}
	}
	
	if (numPowerGenerators)
	{
		for (int i=0;i<numBuildings;i++)
		{
			Stuff::Vector3D maxDist;
			long generatorIndex = 0;
			maxDist.Subtract(buildings[i]->getPosition(),powerGenerators[0]->getPosition());
			float minDistance = maxDist.GetApproximateLength();
			
			for (long j=1;j<numPowerGenerators;j++)
			{
				maxDist.Subtract(buildings[i]->getPosition(),powerGenerators[j]->getPosition()); 
				float newDistance = maxDist.GetApproximateLength(); 
				
				if (newDistance < minDistance)
				{
					generatorIndex = j;
					minDistance = newDistance;
				}
			}	
			
			buildings[i]->setPowerSupply(powerGenerators[generatorIndex]);
		}
		
		for (int i=0;i<numTerrainObjects;i++)
		{
			Stuff::Vector3D maxDist;
			long generatorIndex = 0;
			maxDist.Subtract(terrainObjects[i]->getPosition(),powerGenerators[0]->getPosition());
			float minDistance = maxDist.GetApproximateLength();
			
			for (long j=1;j<numPowerGenerators;j++)
			{
				maxDist.Subtract(terrainObjects[i]->getPosition(),powerGenerators[j]->getPosition()); 
				float newDistance = maxDist.GetApproximateLength(); 
				
				if (newDistance < minDistance)
				{
					generatorIndex = j;
					minDistance = newDistance;
				}
			}	
			
			terrainObjects[i]->setPowerSupply(powerGenerators[generatorIndex]);
		}
	}
}

//---------------------------------------------------------------------------
void GameObjectManager::primeTerrainObjectsForMissionLoad (volatile float& progress, float progressRange)
{
	ZoneScopedN("GameObjectManager::primeTerrainObjectsForMissionLoad");
	if (numTerrainObjects <= 0)
		return;

	long warmedAppearances = 0;
	const float increment = progressRange / float(numTerrainObjects);
	for (long i = 0; i < numTerrainObjects; ++i)
	{
		TerrainObjectPtr obj = terrainObjects[i];
		if (obj && obj->getExists())
		{
			obj->primeAppearanceForMissionLoad();
			++warmedAppearances;
		}

		progress += increment;
	}

	TracyPlot("Terrain object appearances warmed during mission load", int64_t(warmedAppearances));
}

// Task 5 (Track B): bulk static-prop registration at mission load.
// Walks every Bldg/Tree appearance spawned by addObject AFTER
// primeTerrainObjectsForMissionLoad (position/rotation set) and BEFORE
// finalizeGeometry. registerStatic() calls TransformMultiShape_PositionsOnly
// to populate per-leaf shapeToWorld matrices, then buildRecipeFromShape per
// leaf, then GpuStaticPropRegistry::registerRecipe.
// Default-off: MC2_STATIC_PROP_MISSION_LOAD_REG=0 (first-render fallback covers).
void GameObjectManager::registerStaticPropsForMissionLoad() {
	ZoneScopedN("GameObjectManager::registerStaticPropsForMissionLoad");
	if (!GpuStaticPropRegistry::isMissionLoadRegEnabled()) return;

	int totalEnumerated = 0, totalRegistered = 0, totalSkipped = 0;
	// 2026-05-10 diag: per-array breakdown to localise the buildings-don't-render bug.
	int byArr_enum[4] = {0,0,0,0}; // 0=terrainObjects 1=buildings 2=turrets 3=gates
	int byArr_reg[4]  = {0,0,0,0};
	int byArr_skip[4] = {0,0,0,0};
	int byArr_noapp[4]= {0,0,0,0};
	int currentArr = 0;

	// Push game-object position/rotation into Appearance::position/rotation
	// before registerStatic() reads them. Without this, buildings/turrets/gates
	// register at world origin (ghost-prop pile) and miss frustum admission,
	// falling through to a broken legacy first-render path that renders them
	// solid black. Applied uniformly to all four arrays — terrainObjects[]
	// were already primed by primeTerrainObjectsForMissionLoad but re-priming
	// is harmless. Known side effect: mech shadow regression (intermittent,
	// camera-dependent); accepted trade-off until shadow path is debugged.
	auto registerOne = [&](GameObjectPtr obj) {
		if (!obj) return;
		++totalEnumerated;
		++byArr_enum[currentArr];
		AppearancePtr app = obj->getAppearance();
		if (!app) { ++totalSkipped; ++byArr_skip[currentArr]; ++byArr_noapp[currentArr]; return; }
		app->setObjectParameters(obj->getPosition(), obj->getRotation(), 0, 0, 0);
		app->registerStatic();
		if (app->isStaticRegistered()) { ++totalRegistered; ++byArr_reg[currentArr]; }
		else                           { ++totalSkipped;    ++byArr_skip[currentArr]; }
	};

	currentArr = 0;
	for (long i = 0; i < numTerrainObjects; ++i)
		registerOne(terrainObjects[i]);
	currentArr = 1;
	for (long i = 0; i < numBuildings; ++i)
		registerOne(buildings[i]);
	currentArr = 2;
	for (long i = 0; i < numTurrets; ++i)
		registerOne(turrets[i]);
	currentArr = 3;
	for (long i = 0; i < numGates; ++i)
		registerOne(gates[i]);

	fprintf(stderr,
		"[STATIC_PROP_REG v1] event=mission_load enumerated=%d registered=%d skipped=%d\n",
		totalEnumerated, totalRegistered, totalSkipped);
	fprintf(stderr,
		"[STATIC_PROP_REG v1] event=mission_load_byarr "
		"terrainObjects=%d/%d (skip=%d noapp=%d) "
		"buildings=%d/%d (skip=%d noapp=%d) "
		"turrets=%d/%d (skip=%d noapp=%d) "
		"gates=%d/%d (skip=%d noapp=%d)\n",
		byArr_reg[0], byArr_enum[0], byArr_skip[0], byArr_noapp[0],
		byArr_reg[1], byArr_enum[1], byArr_skip[1], byArr_noapp[1],
		byArr_reg[2], byArr_enum[2], byArr_skip[2], byArr_noapp[2],
		byArr_reg[3], byArr_enum[3], byArr_skip[3], byArr_noapp[3]);
	fflush(stderr);
}

// STATIC-REG-PREWARM-QUEUE-1: mission-load off-screen light bake.
// Must be called AFTER finalizeGeometry() and AFTER eye->init() so that
// world lights are valid (Camera::getWorldLights()/getNumLights() return live data).
// Walks the same four object arrays as registerStaticPropsForMissionLoad().
// For each registered-and-latched static prop, calls
//   app->prewarmStaticLightBake(cam)
// which runs SetLightList → TransformMultiShape_HierarchyOnly → mc2CacheOrBakeStaticGpuLight
// and clears needsFullBakeNextFrame if the bake produced a valid permanent SSBO slot.
// Guarded by MC2_STATIC_REG_PREWARM=1 (default off).
void GameObjectManager::prewarmStaticPropLightBakes(Camera* cam)
{
	ZoneScopedN("GameObjectManager::prewarmStaticPropLightBakes");
	static const bool s_enabled  = (getenv("MC2_STATIC_REG_PREWARM")       != nullptr);
	static const bool s_trace    = (getenv("MC2_STATIC_REG_PREWARM_TRACE") != nullptr);
	if (!s_enabled) return;

	auto t0 = std::chrono::steady_clock::now();

	int attempted = 0, baked = 0;
	int skipped_no_lights = 0, skipped_not_registered = 0;
	int skipped_no_multishape = 0, failed_no_cached_index = 0;

	auto tryBake = [&](GameObjectPtr obj) {
		if (!obj) return;
		AppearancePtr app = obj->getAppearance();
		if (!app) { ++skipped_no_multishape; return; }
		if (!app->isStaticRegistered()) { ++skipped_not_registered; return; }
		if (!app->needsPrewarmBake()) return;  // already baked or not latched
		if (!cam || cam->getNumLights() == 0) { ++skipped_no_lights; return; }
		++attempted;
		if (s_trace) {
			fprintf(stderr,
				"STATIC_REG_PREWARM_TRACE: obj=%p recipeIdx=%d\n",
				(void*)obj, app->getStaticRecipeIndex());
		}
		if (app->prewarmStaticLightBake(cam)) {
			++baked;
		} else {
			++failed_no_cached_index;
		}
	};

	for (long i = 0; i < numTerrainObjects; ++i) tryBake(terrainObjects[i]);
	for (long i = 0; i < numBuildings;      ++i) tryBake(buildings[i]);
	for (long i = 0; i < numTurrets;        ++i) tryBake(turrets[i]);
	for (long i = 0; i < numGates;          ++i) tryBake(gates[i]);

	// Count remaining needsPrewarmBake objects (acceptance metric).
	int remaining_needs_full_bake = 0;
	auto countRemaining = [&](GameObjectPtr obj) {
		if (!obj) return;
		AppearancePtr app = obj->getAppearance();
		if (app && app->needsPrewarmBake()) ++remaining_needs_full_bake;
	};
	for (long i = 0; i < numTerrainObjects; ++i) countRemaining(terrainObjects[i]);
	for (long i = 0; i < numBuildings;      ++i) countRemaining(buildings[i]);
	for (long i = 0; i < numTurrets;        ++i) countRemaining(turrets[i]);
	for (long i = 0; i < numGates;          ++i) countRemaining(gates[i]);

	auto t1 = std::chrono::steady_clock::now();
	long long elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

	printf("STATIC_REG_PREWARM: enabled=1"
	       " attempted=%d baked=%d"
	       " skipped_no_lights=%d skipped_not_registered=%d"
	       " skipped_no_multishape=%d failed_no_cached_index=%d"
	       " remaining_needs_full_bake=%d elapsed_us=%lld\n",
	       attempted, baked,
	       skipped_no_lights, skipped_not_registered,
	       skipped_no_multishape, failed_no_cached_index,
	       remaining_needs_full_bake, elapsed_us);
	fflush(stdout);
}

extern GameObjectFootPrint* tempSpecialAreaFootPrints;
extern long tempNumSpecialAreas;

void GameObjectManager::addObject (ObjDataLoader *objData, long& curTerrainObjectIndex, 
								  long& curBuildingIndex, long& curTurretIndex, long &curGateIndex,
								  long& curCollidableHandle, long& curNonCollidableHandle )
{
	// THIS IS NOT COMPLETELY DONE! MAKE SURE ALL TERRAIN OBJ TYPES
	// ARE ACCOUNTED FOR HERE!

	long objTypeNum = objData->objTypeNum;
	if (!isLandMine(objTypeNum)) {
		ObjectTypePtr objType = getObjectType(objTypeNum);
		GameObjectPtr obj = NULL;
		if (!objType)
			return;
			//Fatal();
		
		Stuff::Vector2DOf<long> numbers;
		numbers.x = objData->vertexNumber;
		numbers.y = objData->blockNumber;

		unsigned char realDmg = objData->damage & 0x0f;
		switch (objType->getObjectClass()) {
			case TERRAINOBJECT:
			case TREE:
				obj = getTerrainObject(curTerrainObjectIndex++);
				if (obj)
				{
					((TerrainObjectPtr)obj)->init(true, objType);
					if (realDmg)
						((TerrainObjectPtr)obj)->setDamage(((TerrainObjectTypePtr)objType)->getDamageLevel());
						
					objList[curCollidableHandle] = obj;
					obj->setHandle(curCollidableHandle++);
					
					obj->setExists(true);
				}
				else
				{
					Fatal(curBuildingIndex," No More Trees ");
				}
				break;
			case TURRET:
				obj = getTurret(curTurretIndex++);
				if (obj)
				{
					((TurretPtr)obj)->init(true, objType);
					if (realDmg)
						obj->setDamage(((TurretTypePtr)objType)->getDamageLevel());

					objList[curCollidableHandle] = obj;
					obj->setHandle(curCollidableHandle++);
					obj->setExists(true);
					obj->setParentId(objData->parentId);
					obj->setTeamId(objData->teamId,true);
				}
				else
				{
					Fatal(curTurretIndex," No More Turrets ");
				}
				break;

			case GATE:
				obj = getGate(curGateIndex++);
				if (obj)
				{
					((GatePtr)obj)->init(true, objType);
					if (realDmg)
						obj->setDamage(((GateTypePtr)objType)->getDamageLvl());

					objList[curCollidableHandle] = obj;
					obj->setHandle(curCollidableHandle++);
					obj->setExists(true);
					obj->setParentId(objData->parentId);
					obj->setTeamId(objData->teamId,true);
				}
				else
				{
					Fatal(curGateIndex," No More Gates ");
				}
				break;

			case BUILDING:
			case TREEBUILDING:
				obj = getBuilding(curBuildingIndex++);
				if (obj)
				{
					((BuildingPtr)obj)->init(true, objType);
					if (realDmg)
						obj->setDamage(((BuildingTypePtr)objType)->getDamageLevel());
					((BuildingPtr)obj)->baseTileId = (objData->damage >> 4);
					if (obj->isSpecialBuilding())
					{
						objList[curCollidableHandle] = obj;
						obj->setHandle(curCollidableHandle++);
					}
					else
					{
						objList[curNonCollidableHandle] = obj;
						obj->setHandle(curNonCollidableHandle++);
					}
					
					obj->setExists(true);
					obj->setParentId(objData->parentId);
					obj->setTeamId(objData->teamId,true);
				}
				else
				{
					Fatal(curBuildingIndex," No More Buildings ");
				}
				break;
			case BRIDGE:
				break;
			default:
				Fatal(objType->getObjectClass(), " GameObjectManager.countTerrainObjects: bad object type ");
		}

		if (obj) {
			obj->setTerrainPosition(objData->vector, numbers);
			int cellRow, cellCol;
			obj->getCellPosition(cellRow, cellCol);
			setPartId(obj, cellRow, cellCol);

			//Just keep the designers from hurting themselves
			if (objType->getObjectClass() != TURRET)
				obj->setRotation(objData->rotation);

			if (obj->isTerrainObject()) {
				Stuff::Vector3D pos = objData->vector;
				((TerrainObjectPtr)obj)->calcCellFootprint(pos);
				if (obj->getObjectClass() == BUILDING) {
					if (objType->getSubType() == BUILDING_SUBTYPE_WALL) {
						for (long i = 0; i < tempNumSpecialAreas; i++)
							if (tempSpecialAreaFootPrints[i].cellPositionRow == cellRow)
								if (tempSpecialAreaFootPrints[i].cellPositionCol == cellCol) {
									((BuildingPtr)obj)->calcSubAreas(tempSpecialAreaFootPrints[i].numCells, tempSpecialAreaFootPrints[i].cells);
									break;
								}
						((BuildingPtr)obj)->closeSubAreas();
						}
					else if (objType->getSubType() == BUILDING_SUBTYPE_LANDBRIDGE) {
						for (long i = 0; i < tempNumSpecialAreas; i++)
							if (tempSpecialAreaFootPrints[i].cellPositionRow == cellRow)
								if (tempSpecialAreaFootPrints[i].cellPositionCol == cellCol) {
									((BuildingPtr)obj)->calcSubAreas(tempSpecialAreaFootPrints[i].numCells, tempSpecialAreaFootPrints[i].cells);
									break;
								}
						((BuildingPtr)obj)->openSubAreas();
					}
					}
				else if (obj->getObjectClass() == GATE) {
					for (long i = 0; i < tempNumSpecialAreas; i++)
						if (tempSpecialAreaFootPrints[i].cellPositionRow == cellRow)
							if (tempSpecialAreaFootPrints[i].cellPositionCol == cellCol) {
								((BuildingPtr)obj)->calcSubAreas(tempSpecialAreaFootPrints[i].numCells, tempSpecialAreaFootPrints[i].cells);
								break;
							}
					((GatePtr)obj)->openSubAreas();
					// NULL guards — GlobalMoveMap[1] NULL on bail'd mod content.
					for (int i = 0; i < ((GatePtr)obj)->numSubAreas0; i++) {
						if (GlobalMoveMap[0]) GlobalMoveMap[0]->setAreaOwnerWID(((GatePtr)obj)->subAreas0[i], ((GatePtr)obj)->getWatchID());
						if (GlobalMoveMap[1]) GlobalMoveMap[1]->setAreaOwnerWID(((GatePtr)obj)->subAreas1[i], ((GatePtr)obj)->getWatchID());
						if (((GatePtr)obj)->status == OBJECT_STATUS_DESTROYED) {
							if (GlobalMoveMap[0]) GlobalMoveMap[0]->setAreaTeamID(((GatePtr)obj)->subAreas0[i], -1);
							if (GlobalMoveMap[1]) GlobalMoveMap[1]->setAreaTeamID(((GatePtr)obj)->subAreas1[i], -1);
							}
						else {
							if (GlobalMoveMap[0]) GlobalMoveMap[0]->setAreaTeamID(((GatePtr)obj)->subAreas0[i], ((GatePtr)obj)->teamId);
							if (GlobalMoveMap[1]) GlobalMoveMap[1]->setAreaTeamID(((GatePtr)obj)->subAreas1[i], -1);
						}
					}
					((GatePtr)obj)->setTeamId(obj->getTeamId(), true);
					short* curCoord = ((GatePtr)obj)->cellsCovered;
					for (int i = 0; i < ((GatePtr)obj)->numCellsCovered; i++) {
						long r = *curCoord++;
						long c = *curCoord++;
						GameMap->setGate(r, c, true);
					}
				}
			}
		}
	}
}

//---------------------------------------------------------------------------

void GameObjectManager::destroy (void) 
{
	//--------------------------------------------------------------
	// Free 'em all up!!
	long i=0;
	if (mechs && maxMechs > 0) 
	{
		for (i = 0; i < maxMechs; i++) 
		{
			delete mechs[i];
			mechs[i] = NULL;
		}
	}
	mechs = NULL;

	//--------------------------------------------------------------
	if (vehicles && maxVehicles > 0) 
	{
		for (i = 0; i < maxVehicles; i++) 
		{
			delete vehicles[i];
			vehicles[i] = NULL;
		}
	}
	vehicles = NULL;

	//--------------------------------------------------------------
	if (terrainObjects && numTerrainObjects > 0) 
	{
		for (i = 0; i < numTerrainObjects; i++) 
		{
			delete terrainObjects[i];
			terrainObjects[i] = NULL;
		}
	}
	terrainObjects = NULL;

	//--------------------------------------------------------------
	if (buildings && numBuildings > 0) 
	{
		for (i = 0; i < numBuildings; i++) 
		{
			delete buildings[i];
			buildings[i] = NULL;
		}
	}
	buildings = NULL;

	//--------------------------------------------------------------
	if (turrets && numTurrets > 0)
	{
		for (i = 0; i < numTurrets; i++) 
		{
			delete turrets[i];
			turrets[i] = NULL;
		}
	}
	turrets = NULL;

	//--------------------------------------------------------------
	if (gates && numGates > 0)
	{
		for (i = 0; i < numGates; i++) 
		{
			delete gates[i];
			gates[i] = NULL;
		}
	}
	gates = NULL;

	//--------------------------------------------------------------
	if (weapons && numWeapons > 0) 
	{
		for (i = 0; i < numWeapons; i++) 
		{
			delete weapons[i];
			weapons[i] = NULL;
		}
	}
	weapons = NULL;

	//--------------------------------------------------------------
	if (carnage && numCarnage > 0) 
	{
		for (i = 0; i < numCarnage; i++) 
		{
			delete carnage[i];
			carnage[i] = NULL;
		}
	}
	carnage = NULL;

	//--------------------------------------------------------------
	if (lights && numLights > 0) 
	{
		for (i = 0; i < numLights; i++) 
		{
			delete lights[i];
			lights[i] = NULL;
		}
	}
	lights = NULL;

	//--------------------------------------------------------------
	if (artillery && numArtillery > 0) 
	{
		for (i = 0; i < numArtillery; i++) 
		{
			delete artillery[i];
			artillery[i] = NULL;
		}
	}
	artillery = NULL;

	//--------------------------------------------------------------
	if (objTypeManager) 
	{
		delete objTypeManager;
		objTypeManager = NULL;
	}

	delete collisionSystem;
	collisionSystem = NULL;

	systemHeap->Free(moverLineOfSightTable);
	moverLineOfSightTable = NULL;
}

//---------------------------------------------------------------------------

void GameObjectManager::render (bool terrain, bool movers, bool other) {

	//-----------------------------------------------------
	//Set render states as few times as possible.

	if (drawOldWay)
	{
		//--------------------------------
		//Set States for Software Renderer
		if (Environment.Renderer == 3)
		{
			gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
	
			gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
			gos_SetRenderState( gos_State_MonoEnable, 1);
			gos_SetRenderState( gos_State_Perspective, 0);
			gos_SetRenderState( gos_State_Clipping, 1);
			gos_SetRenderState( gos_State_Specular, 0);
			gos_SetRenderState( gos_State_Dither, 0);
			gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
			gos_SetRenderState( gos_State_Filter, gos_FilterNone);
			gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
			gos_SetRenderState( gos_State_ZCompare, 1);
			gos_SetRenderState(	gos_State_ZWrite, 1);
		}
		//--------------------------------
		//Set States for Hardware Renderer	
		else
		{
			gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
			gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
			gos_SetRenderState( gos_State_MonoEnable, 0);
			gos_SetRenderState( gos_State_Perspective, 1);
			gos_SetRenderState( gos_State_Clipping, 1);
			gos_SetRenderState( gos_State_Specular, 1);
			gos_SetRenderState( gos_State_Dither, 1);
			gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
			gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
			gos_SetRenderState( gos_State_ZCompare, 1);
			gos_SetRenderState(	gos_State_ZWrite, 1);
		}
	}

	if (terrain && renderObjects)
	{
		for (long terrainBlock = 0; terrainBlock < Terrain::numObjBlocks; terrainBlock++)
		{
			// Cull-cascade consumer (object render). Post-8c source of truth for
			// objBlockInfo[].active / objVertexActive[] is the cull-merged Step 6
			// slim reduction loop in mclib/terrain.cpp (8c-part-1 merges the
			// per-vertex cull writes into that loop) — NOT the deleted VPL body,
			// NOT a "Step 5 / 5B slim pass" (that producer never existed; v3.2
			// deleted it). See VPL-retirement plan v3.5 note (CRIT-0).
			if (Terrain::objBlockInfo[terrainBlock].active)
			{
				long numObjs = Terrain::objBlockInfo[terrainBlock].numObjects;
				long objIndex = Terrain::objBlockInfo[terrainBlock].firstHandle;
				for (long terrainObj = 0; terrainObj < numObjs; terrainObj++, objIndex++)
				{
					if (objList[objIndex] &&
						objList[objIndex]->getExists() &&
						Terrain::objVertexActive[objList[objIndex]->getVertexNum()])
					{
						objList[objIndex]->render();
						if (MaxObjectsDrawn)
						{
							//-----------------------------------------
							// No more element groups, so stop drawing.
							return;
						}
					}
				}
			}
		}
	}

	if (movers) {
		if (mechs && (numMechs < maxMechs)) 
		{
			for (long i = 0; i < numMechs; i++)
			{
				if (mechs[i] && mechs[i]->getExists())
					mechs[i]->render();
			}
		}

		if (vehicles) {
			for (long i = 0; i < maxVehicles; i++)
				if (vehicles[i] && vehicles[i]->getExists())
					vehicles[i]->render();
		}

#ifdef USE_ELEMENTALS
		if (elementals) {
			for (long i = 0; i < numElementals; i++)
				if (elementals[i] && elementals[i]->getExists())
					elementals[i]->render();
		}
#endif
	}

	if (other) {
		//----------------------------------------
		// All other objects should be rendered...
		if (weapons) {
			for (long i = 0; i < numWeapons; i++) {
				if (weapons[i] && weapons[i]->getExists())
					weapons[i]->render();
			}
		}

		if (carnage) {
			for (long i = 0; i < numCarnage; i++) {
				if (carnage[i] && carnage[i]->getExists())
				{
					carnage[i]->render();
				}
			}
		}

		if (lights) {
			for (long i = 0; i < numLights; i++) {
				if (lights[i] && lights[i]->getExists())
					lights[i]->render();
			}
		}

		if (artillery) {
			for (long i = 0; i < numArtillery; i++) {
				if (artillery[i] && artillery[i]->getExists())
					artillery[i]->render();
			}
		}
	}
	gos_SetRenderState( gos_State_Fog, 0);
}

//---------------------------------------------------------------------------

void GameObjectManager::renderShadows (bool terrain, bool movers, bool other) {

	//-----------------------------------------------------
	//Set render states as few times as possible.

	//--------------------------------
	//Set States for Software Renderer
	if (Environment.Renderer == 3)
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeFlat);
		gos_SetRenderState( gos_State_MonoEnable, 1);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Clipping, 1);
		gos_SetRenderState( gos_State_AlphaTest, 0);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 0);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendDecal);
		gos_SetRenderState( gos_State_Filter, gos_FilterNone);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
		gos_SetRenderState( gos_State_ZCompare, 0);
		gos_SetRenderState(	gos_State_ZWrite, 0);
	}
	//--------------------------------
	//Set States for Hardware Renderer	
	else
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeFlat);
		gos_SetRenderState( gos_State_MonoEnable, 1);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Clipping, 1);
		gos_SetRenderState( gos_State_AlphaTest, 1);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 1);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterNone);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
		gos_SetRenderState( gos_State_ZCompare, 2);
		gos_SetRenderState(	gos_State_ZWrite, 1);
	}

	if (terrain && renderObjects)
	{
		for (long terrainBlock = 0; terrainBlock < Terrain::numObjBlocks; terrainBlock++)
		{
			// Cull-cascade consumer (shadow render). Post-8c source of truth for
			// objBlockInfo[].active / objVertexActive[] is the cull-merged Step 6
			// slim reduction loop in mclib/terrain.cpp (8c-part-1 merges the
			// per-vertex cull writes into that loop) — NOT the deleted VPL body,
			// NOT a "Step 5 / 5B slim pass" (that producer never existed; v3.2
			// deleted it). See VPL-retirement plan v3.5 note (CRIT-0).
			if (Terrain::objBlockInfo[terrainBlock].active)
			{
				long numObjs = Terrain::objBlockInfo[terrainBlock].numObjects;
				long objIndex = Terrain::objBlockInfo[terrainBlock].firstHandle;
				for (long terrainObj = 0; terrainObj < numObjs; terrainObj++, objIndex++)
				{
					if (objList[objIndex] &&
						objList[objIndex]->getExists() &&
						Terrain::objVertexActive[objList[objIndex]->getVertexNum()])
					{
						objList[objIndex]->renderShadows();
						if (MaxObjectsDrawn) {
							//-----------------------------------------
							// No more element groups, so stop drawing.
							return;
						}
					}
				}
			}
		}
	}

	if (movers) {
		if (mechs) {
			for (long i = 0; i < numMechs; i++)
				if (mechs[i] && mechs[i]->getExists())
					mechs[i]->renderShadows();
		}

		if (vehicles) {
			for (long i = 0; i < numVehicles; i++)
				if (vehicles[i] && vehicles[i]->getExists())
					vehicles[i]->renderShadows();
		}
#ifdef USE_ELEMENTALS
		if (elementals) {
			for (long i = 0; i < numElementals; i++)
				if (elementals[i] && elementals[i]->getExists())
					elementals[i]->renderShadows();
		}
#endif
	}

	if (other) {
		if (carnage) {
			for (long i = 0; i < numCarnage; i++) {
				if (carnage[i] && carnage[i]->getExists())
					carnage[i]->renderShadows();
			}
		}
	}

	gos_SetRenderState( gos_State_Fog, 0);
}

//---------------------------------------------------------------------------

// [GOM_UPDATE_COST v1] — GOM-UPDATE-COST-1 coarse per-phase CPU cost split of
// GameObjectManager::update(). Smoke-visible complement of the GameLogic.Units.*
// Tracy zones (smoke runs cannot take user-driven Tracy captures). Pattern =
// [RENDERLISTS_COST v1] (txmmgr.cpp): env-gated, window-averaged, default OFF
// (zero overhead beyond one cached-bool test per phase when unset; ~2
// steady_clock reads per coarse phase per frame when set — the four whole-block
// spans only, per the 100ns-floor rule, NEVER per-object).
// Enable: MC2_GOM_UPDATE_COST_SPLIT=1 -> one [GOM_UPDATE_COST v1] stderr summary
// every 60 frames: per-frame mean µs per phase. self_us = total minus the sum
// of instrumented phases (unattributed gaps: framesSinceActive sweep, captureList,
// FRAME-JOBS prepass, etc.).
namespace gomcost {
static const bool s_enabled = []() {
    const char* v = getenv("MC2_GOM_UPDATE_COST_SPLIT");
    return v && v[0] != '0';
}();
enum Phase {
    kStaticTouch = 0, kMoverUpdate, kOtherFx, kSubstrateFlush, kTotal, kPhaseCount
};
static const char* kName[kPhaseCount] = {
    "static_touch", "mover_update", "other_fx", "substrate_flush", "total"
};
static unsigned long long s_ns[kPhaseCount] = {};
static int s_frames = 0;
static inline unsigned long long nowNs() {
    return static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}
// Lexical phase span. Zero-cost (two branch-predicted tests) when disabled.
struct Span {
    int p; unsigned long long t0;
    explicit Span(int phase) : p(phase), t0(s_enabled ? nowNs() : 0ULL) {}
    ~Span() { if (s_enabled) s_ns[p] += nowNs() - t0; }
};
// Whole-function span + window emit. Construct FIRST so its dtor runs LAST.
struct TotalSpan {
    unsigned long long t0;
    TotalSpan() : t0(s_enabled ? nowNs() : 0ULL) {}
    ~TotalSpan() {
        if (!s_enabled) return;
        s_ns[kTotal] += nowNs() - t0;
        if (++s_frames < 60) return;
        const double wf = static_cast<double>(s_frames);
        double phaseSumUs = 0.0;
        char line[512]; int off = 0;
        off += snprintf(line + off, sizeof(line) - off,
            "[GOM_UPDATE_COST v1] event=summary frames=%d", s_frames);
        for (int p = 0; p < kPhaseCount; ++p) {
            const double us = static_cast<double>(s_ns[p]) / 1000.0 / wf;
            if (p != kTotal) phaseSumUs += us;
            if (off < static_cast<int>(sizeof(line)))
                off += snprintf(line + off, sizeof(line) - off,
                                " %s_us=%.1f", kName[p], us);
        }
        const double selfUs =
            static_cast<double>(s_ns[kTotal]) / 1000.0 / wf - phaseSumUs;
        if (off < static_cast<int>(sizeof(line)))
            snprintf(line + off, sizeof(line) - off, " self_us=%.1f", selfUs);
        fprintf(stderr, "%s\n", line);
        fflush(stderr);
        memset(s_ns, 0, sizeof(s_ns));
        s_frames = 0;
    }
};
} // namespace gomcost

void GameObjectManager::update (bool terrain, bool movers, bool other)
{
	ZoneScopedN("GameObjectManager::update");
	gomcost::TotalSpan _gomTotalSpan;  // [GOM_UPDATE_COST v1] first-constructed => dtor last
	//----------------------------
	// Now, update game objects...

	// C3: build per-actor GPU visibility snapshot from N-1 readback FIRST,
	// so the framesSinceActive sweep below reads a freshly-built snapshot.
	// Motion-tolerance slice: also build when READBACK alone is enabled so
	// the [GPU_CULL v1] event=motion_tolerance summary (dilated_admits +
	// conservative_or_admits counters) fires even without LIFECYCLE wired.
	// readback_buildActorVisSnapshot is no-op-cheap when no good slot exists.
	if (s_gpuCullLifecycle || gpu_cull::readback_isEnabled()) {
		ZoneScopedN("GOM.readbackSnapshot");
		// Max handle is bounded by maxObjects + slack; 4096 is safe for MC2 (~2000 max).
		gpu_cull::readback_buildActorVisSnapshot(4096u);
	}

	// Tier-1 instrumentation (stability spec §3.3): single source of truth for
	// framesSinceActive. One sweep over objList covers every GameObject this
	// manager owns. Uses the three virtual accessors added on GameObject base.
	{
		ZoneScopedN("GOM.framesSinceActive sweep");
		// OBJECT-WALK-FACTS-1: gated, default-OFF sweep telemetry. objWalk
		// caches the gate bool once (dtor emits one "OBJWALK" frame event);
		// the hot loop only touches plain longs when objWalkOn. OFF = one
		// predicted branch, no counter writes, byte-identical.
		mc2_object_walk_trace::SweepScope objWalk;
		const bool objWalkOn = objWalk.active;
		const long maxObjs = getMaxObjects();
		if (objWalkOn) mc2_object_walk_trace::g_counters.objectsWalked = maxObjs;
		for (long i = 1; i <= maxObjs; i++) {
			GameObjectPtr obj = objList[i];
			if (!obj) continue;
			if (objWalkOn) {
				mc2_object_walk_trace::Counters& oc = mc2_object_walk_trace::g_counters;
				++oc.liveObjects;
				if (obj->isMover()) ++oc.moversWalked; else ++oc.staticsWalked;
			}
			bool activeThisFrame_instr;
			if (s_gpuCullLifecycle && gpu_cull::readback_isEnabled()) {
				// C3: GPU visibility gates the lifecycle accumulator.
				// block-active is a supplemental gate for off-screen AI-active objects.
				// Requires READBACK to be enabled — without it, readback_isActorVisibleLagged
				// fail-opens (returns true for every actor), making the accumulator useless.
				activeThisFrame_instr =
				    gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(obj->getHandle()))
				 || obj->blockActive_instr();
			} else {
				activeThisFrame_instr =
				       obj->inView_instr()
				    || obj->canBeSeen_instr()
				    || obj->blockActive_instr();
			}
			if (activeThisFrame_instr) {
				obj->framesSinceActive = 0;
				if (objWalkOn) ++mc2_object_walk_trace::g_counters.activeThisFrame;
			} else {
				if (obj->framesSinceActive < 255) {
					obj->framesSinceActive++;
				}
				if (objWalkOn) ++mc2_object_walk_trace::g_counters.agedThisFrame;
			}

			// alpha-Stage 1 §5 Stage 0 probe: compute 4 candidate
			// predicates per actor + accumulate pairwise XOR. Cheap:
			// 4 bool reads + 6 XORs per live actor per frame. Probe
			// stays env-gated; zero cost when MC2_INVIEW_CONFLATION_TRACE
			// is unset.
			if (s_inViewConflationEnabled) {
				// v1.1 (2026-05-20): canBeSeen_instr() not inView_instr().
				// canBeSeen_instr routes through appearance->canBeSeen() →
				// returns appearance->inView (the actual consumer bit).
				// inView_instr has only one override (Artillery); using it
				// made render_cand=FALSE for ~all actors.
				const bool render_cand    = obj->canBeSeen_instr();
				const bool sim_cand       = render_cand
				                          || obj->blockActive_instr()
				                          || (obj->framesSinceActive < s_inViewConflationHysteresisN);
				const bool lifecycle_cand = true; // alive by virtue of being in objList[i]
				const bool isMover_flag   = obj->isMover();
				const bool ai_cand        = isMover_flag
				                          ? (gpu_cull::readback_isEnabled()
				                              ? gpu_cull::readback_isActorVisibleLagged(
				                                    static_cast<uint32_t>(obj->getHandle()))
				                              : render_cand)
				                          : render_cand;
				++g_invConfActorFrames;
				if (isMover_flag) {
					++g_invConfMovers;
					if (render_cand != ai_cand) ++g_invConfMoverRenderVsAi;
				} else {
					++g_invConfStatics;
				}
				if (render_cand != sim_cand)       ++g_invConfRenderVsSim;
				if (render_cand != lifecycle_cand) ++g_invConfRenderVsLife;
				if (render_cand != ai_cand)        ++g_invConfRenderVsAi;
				if (sim_cand    != lifecycle_cand) ++g_invConfSimVsLife;
				if (sim_cand    != ai_cand)        ++g_invConfSimVsAi;
				if (lifecycle_cand != ai_cand)     ++g_invConfLifeVsAi;
			}
		}
		// Per-frame roll-and-maybe-emit (120-frame summary cadence).
		g_invViewConflationRollAndMaybeEmit();
	}

	// 2026-05-13: substrate_frameBegin() was MOVED to Mission::update
	// (code/mission.cpp, immediately before the
	//   if (isPaused) updateAppearancesOnly(); else update();
	// branch).  Reason: this function is pause-gated externally — when
	// the mission is paused, GameObjectManager::update is skipped and
	// updateAppearancesOnly runs instead, so a frameBegin call here
	// never fires during pause.  Meanwhile render-time submits via
	// BldgAppearance/TreeAppearance::render and GpuStaticPropRegistry::
	// flush continue to append substrate records every frame.  Without
	// a per-frame reset the substrate ring slot accumulates records
	// across pause frames; compute cull then writes inflated
	// bucketCountData, coalesce multi-draw overruns each bucket's
	// instance range, and the user sees "every prop's location
	// layered with copies of other props at the same origin" pause
	// smearing.  See the LODBUG/pause-smear investigation 2026-05-13
	// and pause_unpause_diagnostic_for_static_render_bugs.md.

	{ ZoneScopedN("GOM.captureList"); updateCaptureList(); }

	if (terrain && renderObjects)
	{
		int frameJobsPrePassCount = 0; // FRAME-JOBS-1: handle count for per-frame trace
		// ── FRAME-JOBS-1: parallel recalcBounds pre-pass ─────────────────────────────
		// Tier B only: recalcBoundsAndStamp() on whitelisted types.
		// No touch(), no update(), no GL, no txmmgr. See FRAME-JOBS-2 for Tier C.
		// boundsFrame stamp prevents double-compute in the serial loop below.
		{
			static std::vector<long> s_frameJobsHandles;
			s_frameJobsHandles.clear();
			frameJobsResetFrameStats();

			if (frameJobsEnabled()) {
				// Build flat handle list with basic eligibility filter.
				// Intentionally loose — isRecalcBoundsWorkerSafe() filters inside the lambda.
				for (int block = 0; block < Terrain::numObjBlocks; ++block) {
					if (!Terrain::objBlockInfo[block].active) continue;
					long h = Terrain::objBlockInfo[block].firstHandle;
					int  n = Terrain::objBlockInfo[block].numObjects;
					for (int i = 0; i < n; ++i, ++h) {
						GameObjectPtr obj = objList[h];
						if (!obj || !obj->getExists()) continue;
						if (!Terrain::objVertexActive[obj->getVertexNum()]) continue;
						s_frameJobsHandles.push_back(h);
					}
				}

				const int count = static_cast<int>(s_frameJobsHandles.size());
				frameJobsPrePassCount = count; // FRAME-JOBS-1: exported for per-frame trace

				// FRAME-JOBS-1: wall-clock timing + Tracy zone for the parallel recalcBounds pass.
				auto _fj_t0 = std::chrono::high_resolution_clock::now();
				{
					ZoneScopedN("FrameJobs.RecalcBounds");
					parallelForRange(count, frameJobsBatch(),
						[](int begin, int end) {
							// FRAME-JOBS-1: Tier B only. No appearance->update() here.
							// Tier C (txmmgr) is NOT worker-safe. See FRAME-JOBS-2.
							for (int i = begin; i < end; ++i) {
								GameObjectPtr obj = ObjectManager->objList[s_frameJobsHandles[i]];
								if (!obj) continue;
								AppearancePtr ap = obj->getAppearance();
								if (!ap) continue;
								if (!ap->isRecalcBoundsWorkerSafe()) {
									if (frameJobsTrace()) {
										printf("FRAME_JOBS WARN: unsupported appearance type at handle %ld\n",
										       s_frameJobsHandles[i]);
									}
									continue;
								}
								ap->recalcBoundsAndStamp();
							}
						});
				}
				auto _fj_t1 = std::chrono::high_resolution_clock::now();
				float _fj_prepass_us = std::chrono::duration<float, std::micro>(_fj_t1 - _fj_t0).count();

				if (frameJobsTrace()) {
					FrameJobsFrameStats _fj_stats = frameJobsGetFrameStats();
					printf("FRAME_JOBS_PERF: batch=%d workers=%d handles=%d chunks=%d prepass_us=%.1f\n",
					       frameJobsBatch(),
					       _fj_stats.workerCount,
					       count,
					       _fj_stats.chunksExecuted,
					       _fj_prepass_us);
				}

				// FRAME-JOBS-1: emit to diagnostic JSONL trace (MC2_DIAG_TAGS=FRAME_JOBS or *)
				if (mc2_diag::tagEnabled("FRAME_JOBS")) {
					FrameJobsFrameStats _fj_stats = frameJobsGetFrameStats();
					char _fj_buf[192];
					snprintf(_fj_buf, sizeof(_fj_buf),
					         "{\"batch\":%d,\"workers\":%d,\"handles\":%d,\"chunks\":%d,\"prepass_us\":%.1f}",
					         frameJobsBatch(),
					         _fj_stats.workerCount,
					         count,
					         _fj_stats.chunksExecuted,
					         _fj_prepass_us);
					mc2_diag::writeEvent("FRAME_JOBS", 1, 0, _fj_buf);
				}
			}

			// ── FRAME-JOBS-2D: split touch prepass ──────────────────────────────────────
			// Phase 1 (workers): touchWorkerPrepass() — lock-free per-instance prep
			//   (bldgShape->Touch() / treeShape->Touch() / selectActiveLOD()).
			// Phase 2 (main thread serial): touchSerialCommit() — light-data resubmit.
			// g_workerResubmitCalls must be 0 every frame — nonzero = split broken.
			if (frameJobsTouchEnabled()) {
				ZoneScopedN("FrameJobs.Touch");
				static std::vector<long> s_splitHandles;
				s_splitHandles.clear();
				int candidates = 0, skipped_not_split_safe = 0;

				// Filter s_frameJobsHandles to split-safe appearance types only.
				for (long h : s_frameJobsHandles) {
					++candidates;
					GameObjectPtr obj = objList[h];
					if (!obj) { ++skipped_not_split_safe; continue; }
					AppearancePtr ap = obj->getAppearance();
					if (!ap || !ap->isTouchSplitSafe()) { ++skipped_not_split_safe; continue; }
					s_splitHandles.push_back(h);
				}
				int worker_submitted = static_cast<int>(s_splitHandles.size());

				// Phase 1: parallel lock-free prep ─────────────────────────────────────
				auto _tw0 = std::chrono::high_resolution_clock::now();
				int touchCount = worker_submitted;
				if (touchCount > 0) {
					parallelForRange(touchCount, frameJobsBatch(), [](int begin, int end) {
						g_isFrameJobsWorker = true;
						for (int i = begin; i < end; ++i) {
							GameObjectPtr obj = ObjectManager->objList[s_splitHandles[i]];
							if (!obj) continue;
							AppearancePtr ap = obj->getAppearance();
							if (ap) ap->touchWorkerPrepass();
						}
						g_isFrameJobsWorker = false;
					});
				}
				auto _tw1 = std::chrono::high_resolution_clock::now();
				float worker_us = std::chrono::duration<float, std::micro>(_tw1 - _tw0).count();

				// Phase 2: serial commit ───────────────────────────────────────────────
				int serial_commits = 0;
				int prev_resubmit_calls = g_workerResubmitCalls.load(std::memory_order_relaxed);
				auto _tc0 = std::chrono::high_resolution_clock::now();
				for (long h : s_splitHandles) {
					GameObjectPtr obj = objList[h];
					if (!obj) continue;
					AppearancePtr ap = obj->getAppearance();
					if (ap) { ap->touchSerialCommit(); ++serial_commits; }
				}
				auto _tc1 = std::chrono::high_resolution_clock::now();
				float commit_us = std::chrono::duration<float, std::micro>(_tc1 - _tc0).count();
				int worker_resubmit_calls = g_workerResubmitCalls.load(std::memory_order_relaxed) - prev_resubmit_calls;

				if (frameJobsTrace()) {
					printf("FRAME_JOBS_TOUCH: enabled=1 split=yes candidates=%d worker_submitted=%d"
					       " serial_commits=%d worker_resubmit_calls=%d"
					       " skipped_not_split_safe=%d\n",
					       candidates, worker_submitted, serial_commits, worker_resubmit_calls,
					       skipped_not_split_safe);
					printf("FRAME_JOBS_TOUCH_PERF: worker_us=%.1f serial_commit_us=%.1f"
					       " avg_worker_us_per_object=%.3f avg_serial_commit_us_per_object=%.3f\n",
					       worker_us, commit_us,
					       worker_us / std::max(1, worker_submitted),
					       commit_us / std::max(1, serial_commits));
				}
				// [LIGHTBRIDGE-BAKED-PROBE-1] per-frame probe/copy counters
				static bool s_lightbridgeTrace = (std::getenv("MC2_LIGHTBRIDGE_COMMIT_TRACE") != nullptr);
				if (s_lightbridgeTrace && (g_mc2FrameCounter % 300 == 0)) {
					extern std::atomic<int>       g_bakedProbeCalls;
					extern std::atomic<int>       g_bakedCopyCalls;
					extern std::atomic<long long> g_bakedCopyBytes;
					printf("LIGHTBRIDGE_COMMIT: baked_probe_calls=%d baked_copy_calls=%d baked_copy_bytes=%lld\n",
					       g_bakedProbeCalls.load(std::memory_order_relaxed),
					       g_bakedCopyCalls.load(std::memory_order_relaxed),
					       g_bakedCopyBytes.load(std::memory_order_relaxed));
					std::fflush(stdout);
				}
			}
			// ── end FRAME-JOBS-2D pre-pass ───────────────────────────────────────────────
		}
		// ── end FRAME-JOBS-1 pre-pass ────────────────────────────────────────────────

		ZoneScopedN("GameLogic.Units.TerrainObjects");
		gomcost::Span _gomStaticTouch(gomcost::kStaticTouch); // [GOM_UPDATE_COST v1]
		// ── FRAME-JOBS-2G: Path B terrain-loop touch cost instrumentation ────────────
		// Gate: MC2_FRAME_JOBS_PATHB_DIAG=1. Diagnostic-only; zero behavior change.
		// Times the outer terrain block loop and classifies appearance types per candidate.
		static const bool s_pathBDiag = !!std::getenv("MC2_FRAME_JOBS_PATHB_DIAG");
		static bool s_pathBDiagBanner = false;
		if (!s_pathBDiagBanner) {
			s_pathBDiagBanner = true;
			const char* pathbEnv = std::getenv("MC2_FRAME_JOBS_PATHB_DIAG");
			printf("[FRAME_JOBS_PATHB v1] event=startup gate_var=%s diag=%d\n",
			       pathbEnv ? pathbEnv : "(null)", s_pathBDiag ? 1 : 0);
			std::fflush(stdout);
		}
		static int64_t s_pathB_candidates    = 0;
		static int64_t s_pathB_touch_calls   = 0;
		static int64_t s_pathB_skipped       = 0;   // stamp match → touch() will early-return
		static int64_t s_pathB_ran_nosplit   = 0;   // no stamp → touch() will run
		static int64_t s_pathB_total_us      = 0;
		static int64_t s_pathB_bldg_calls    = 0;
		static int64_t s_pathB_tree_calls    = 0;
		static int64_t s_pathB_other_calls   = 0;
		static int64_t s_pathB_diag_frame    = -1;
		// Per-frame local accumulators (reset each frame)
		int64_t pathB_candidates    = 0;
		int64_t pathB_touch_calls   = 0;
		int64_t pathB_skipped       = 0;
		int64_t pathB_ran_nosplit   = 0;
		int64_t pathB_bldg_calls    = 0;
		int64_t pathB_tree_calls    = 0;
		int64_t pathB_other_calls   = 0;
		std::chrono::high_resolution_clock::time_point _pathB_t0;
		if (s_pathBDiag) _pathB_t0 = std::chrono::high_resolution_clock::now();
		// ── end FRAME-JOBS-2G preamble ────────────────────────────────────────────────
		long specialBuildingsUpdated = 0;
		long gatesUpdated = 0;
		long activeBlocksVisited = 0;
		long terrainObjectsUpdated = 0;
		long terrainObjectsVisited = 0;
		long cullRecordsEmitted = 0;
		long skippedStaticNatural = 0;
		long touchedLiveness = 0;   // R2B-STATIC-NATURAL-TOUCH-PRESERVE-1: liveness stamps applied in the skip path
		long eligibleStaticNatural = 0;
		long staticNaturalCandidates = 0;
		long fallingNaturalUpdated = 0;
		long justCreatedNaturalUpdated = 0;
		long missNotStaticNow = 0;
		long missStaticNotRegistered = 0;
		long missStaticShapeMismatch = 0;
		long missStaticNeedsFullBake = 0;
		long missStaticOther = 0;
		long missStaticUnregTreeAppearance = 0;
		long missStaticUnregBldgAppearance = 0;
		long missStaticUnregOtherAppearance = 0;
		long missStaticUnregObjTree = 0;
		long missStaticUnregObjTerrain = 0;
		long missStaticUnregObjBuilding = 0;
		long missStaticUnregObjOther = 0;
		long missSpecial = 0;
		long missAlarm = 0;
		long missLookout = 0;
		long missSensor = 0;
		long missPower = 0;
		long missControl = 0;
		long missMechBay = 0;
		long missFalling = 0;
		long missJustCreated = 0;
		long nonNaturalUpdated = 0;
		long unknownUpdated = 0;
		long staticBuildingCandidates = 0;
		long skippedStaticBuildings = 0;
		long missBuildingNotStaticNow = 0;
		const bool skipStaticNatural = mc2SkipStaticNaturalEnabled();
		const bool r2bTouchPreserve = mc2R2bTouchPreserveEnabled();   // R2B-STATIC-NATURAL-TOUCH-PRESERVE-1
		const bool r2bTouchTrace = mc2R2bTouchTraceEnabled();
		const bool skipStaticBuildings = mc2SkipStaticBuildingsEnabled();
		const bool skipStaticNaturalDiag = mc2SkipStaticNaturalDiagEnabled();
		const uint32_t staticDiagFrame = g_mc2FrameCounter;
		const bool emitStaticDiagThisFrame = skipStaticNaturalDiag &&
		                                   ((staticDiagFrame <= 5) || ((staticDiagFrame % 300) == 0));
		long nonNaturalSampleCount = 0;
		std::unordered_map<std::string, long> missStaticUnregTypeCounts;

		// MC2_GOM_RECON: type classification & mutation tracking
		long recon_buildings = 0, recon_gates = 0, recon_turrets = 0, recon_other = 0;
		long recon_updated = 0, recon_multiShapeLoops = 0, recon_lightScans = 0;

		//First Update all of the Special Buildings.
		// They will mark themselves updated and not re-update below.
		for (long spBuilding = 0; spBuilding < numSpecialBuildings;spBuilding++)
		{
			if (specialBuildings[spBuilding] && specialBuildings[spBuilding]->getExists())
			{
				long updateRet_instr = specialBuildings[spBuilding]->update();
				specialBuildings[spBuilding]->lastUpdateRet = (int32_t)updateRet_instr;
				if (!updateRet_instr)
				{
					//-----------------------------------------
					// Update failed, so it no longer exists...
					MC2_DESTROY(specialBuildings[spBuilding], "update_false");
				}
				else
				{
					specialBuildingsUpdated++;
				}
				// C0-3: emit after update() so inView is fresh; gated on getExists()
				if (specialBuildings[spBuilding] && specialBuildings[spBuilding]->getExists())
					emitGpuCullRecord(specialBuildings[spBuilding], gpu_cull::Cat_Other,
					                  gpu_cull::Consumer_RenderGate | gpu_cull::Consumer_LifecycleGate);
			}
		}

		//Then update all of the gates.
		// They too will mark themselves and not re-update.
		// MUST update every frame or they don't open!!
		for (long nGates = 0;nGates < numGates;nGates++)
		{
			if (gates[nGates] && gates[nGates]->getExists())
			{
				long updateRet_instr = gates[nGates]->update();
				gates[nGates]->lastUpdateRet = (int32_t)updateRet_instr;
				if (!updateRet_instr)
				{
					//-----------------------------------------
					// Update failed, so it no longer exists...
					MC2_DESTROY(gates[nGates], "update_false");
				}
				else
				{
					gatesUpdated++;
				}
				// C0-3: emit after update() so inView is fresh; gated on getExists()
				if (gates[nGates] && gates[nGates]->getExists())
					emitGpuCullRecord(gates[nGates], gpu_cull::Cat_Gate,
					                  gpu_cull::Consumer_RenderGate | gpu_cull::Consumer_LifecycleGate);
			}
		}

		// C3: terrain block inner-loop GPU-visibility gate — deferred from C3-A.
		// Adding a readback_isActorVisibleLagged() inner gate here would skip update()
		// for objects in active blocks that happen to be GPU-invisible. That is UNSAFE:
		// buildings and turrets in active AI blocks need update() even when offscreen
		// (gate logic, turret tracking, power supply). The FPS win from this loop is
		// small compared to mech/GV render paths (most mission time is at wolfman zoom
		// with few active terrain blocks visible). The cascade structure is preserved
		// unchanged as required by cull_gates_are_load_bearing.md.
		for (long terrainBlock = 0; terrainBlock < Terrain::numObjBlocks; terrainBlock++)
		{
			// Cull-cascade consumer (object update). Post-8c source of truth for
			// objBlockInfo[].active / objVertexActive[] is the cull-merged Step 6
			// slim reduction loop in mclib/terrain.cpp (8c-part-1 merges the
			// per-vertex cull writes into that loop) — NOT the deleted VPL body,
			// NOT a "Step 5 / 5B slim pass" (that producer never existed; v3.2
			// deleted it). See VPL-retirement plan v3.5 note (CRIT-0).
			if (Terrain::objBlockInfo[terrainBlock].active)
			{
				activeBlocksVisited++;
				long numObjs = Terrain::objBlockInfo[terrainBlock].numObjects;
				long objIndex = Terrain::objBlockInfo[terrainBlock].firstHandle;
				for (long terrainObj = 0; terrainObj < numObjs; terrainObj++,objIndex++)
				{
					{
						ZoneScopedN("GOM.TerrainObjects.ActiveGate");
						if (objList[objIndex] &&
							Terrain::objVertexActive[objList[objIndex]->getVertexNum()] &&
							objList[objIndex]->getExists())
						{
							terrainObjectsVisited++;
						}
					}
					if (objList[objIndex] &&
						Terrain::objVertexActive[objList[objIndex]->getVertexNum()] &&
						objList[objIndex]->getExists())
					{
						eligibleStaticNatural++;

						// MC2_GOM_RECON: classify object type
						GameObjectPtr obj = objList[objIndex];
						ObjectTypePtr objType = obj->getObjectType();
						AppearancePtr appearance = obj->getAppearance();

						// FRAME-JOBS-2G: Path B per-candidate classification
						if (s_pathBDiag && appearance) {
							++pathB_candidates;
							++pathB_touch_calls;
							auto* __pathB_bldg = dynamic_cast<BldgAppearance*>(appearance);
							auto* __pathB_tree = dynamic_cast<TreeAppearance*>(appearance);
							bool __pathB_will_skip = false;
							if (__pathB_bldg) {
								__pathB_will_skip = (__pathB_bldg->touchSerialCommitFrame == g_mc2FrameCounter);
								++pathB_bldg_calls;
							} else if (__pathB_tree) {
								__pathB_will_skip = (__pathB_tree->touchSerialCommitFrame == g_mc2FrameCounter);
								++pathB_tree_calls;
							} else {
								++pathB_other_calls;
							}
							if (__pathB_will_skip) ++pathB_skipped;
							else ++pathB_ran_nosplit;
						}
						const bool isJustCreated = obj->getFlag(OBJECT_FLAG_JUSTCREATED);
						const bool isFalling = obj->getFlag(OBJECT_FLAG_FALLING);
						const char* typeName = (objType && objType->getAppearanceTypeName()) ? objType->getAppearanceTypeName() : "<null>";
						const bool isPineAppearance = mc2StaticTypeNameStartsWith(typeName, "Pine");
						bool isTreeCandidate = false;
						bool isPineStaticBuildingCandidate = false;
						long pineSpecialBuilding = 0;
						long pinePerimeterAlarm = 0;
						long pineLookout = 0;
						long pineSensor = 0;
						long pinePowerSource = 0;
						long pineControlBuilding = obj->getFlag(OBJECT_FLAG_CONTROLBUILDING) ? 1L : 0L;
						long pineMechBay = obj->getFlag(OBJECT_FLAG_MECHBAY) ? 1L : 0L;

						if (objType)
						{
							long objectClass = objType->getObjectClass();
							if (objectClass == TREE)
							{
								isTreeCandidate = true;
							}
							else if (objectClass == TERRAINOBJECT)
							{
								TerrainObjectTypePtr terrainType = (TerrainObjectTypePtr)objType;
								isTreeCandidate = (terrainType->subType == TERROBJ_TREE);
							}
							else if ((objectClass == BUILDING) && (isPineAppearance || skipStaticBuildings))
							{
								BuildingPtr building = dynamic_cast<Building*>(obj);
								BuildingTypePtr bldgType = dynamic_cast<BuildingTypePtr>(objType);
								if (building && bldgType)
								{
									pineSpecialBuilding = building->isSpecialBuilding() ? 1L : 0L;
									pinePerimeterAlarm = ((bldgType->perimeterAlarmRange > 0.0f) &&
									                      (bldgType->perimeterAlarmTimer > 0.0f)) ? 1L : 0L;
									pineLookout = (bldgType->lookoutTowerRange > 0.0f) ? 1L : 0L;
									pineSensor = (bldgType->sensorRange > 0.0f) ? 1L : 0L;
									pinePowerSource = bldgType->powerSource ? 1L : 0L;
									isPineStaticBuildingCandidate =
										isPineAppearance &&
										!pineSpecialBuilding &&
										!pinePerimeterAlarm &&
										!pineLookout &&
										!pineSensor &&
										!pinePowerSource &&
										!pineControlBuilding &&
										!pineMechBay;
								}
								else
								{
									nonNaturalUpdated++;
								}
							}
							else
							{
								nonNaturalUpdated++;
							}
						}
						else
						{
							unknownUpdated++;
						}

						if (isTreeCandidate)
						{
							staticNaturalCandidates++;
							if (skipStaticNatural && (turn >= 3) && !isFalling && !isJustCreated)
							{
								// R2B-STATIC-NATURAL-TOUCH-PRESERVE-1: skipping update()
								// would freeze this tree's registered cachedFrame_, so the
								// static-prop registry flush drops it from the instance SSBO
								// (black-tree-bug class). Stamp liveness without the expensive
								// update. Gated for A/B by MC2_R2B_TOUCH_PRESERVE.
								if (r2bTouchPreserve &&
								    mc2R2bTouchStaticLiveness(appearance, (uint32_t)g_mc2FrameCounter))
									touchedLiveness++;
								skippedStaticNatural++;
								continue;
							}
							if (isFalling)
							{
								fallingNaturalUpdated++;
								missFalling++;
							}
							else if (isJustCreated)
							{
								justCreatedNaturalUpdated++;
								missJustCreated++;
							}
						}
						else if (isPineStaticBuildingCandidate)
						{
							staticNaturalCandidates++;
							const long staticNow = appearance ? (appearance->IsStaticNow() ? 1L : 0L) : 0L;
							if (skipStaticNatural && (turn >= 3) && staticNow && !isFalling && !isJustCreated)
							{
								// Power-out propagation must run even for skipped buildings.
								// Replicates bldng.cpp:889 with an added null guard.
								BuildingPtr __skipBldg = dynamic_cast<Building*>(obj);
								if (__skipBldg && __skipBldg->powerSupply && appearance)
								{
									GameObjectPtr __powerObj = ObjectManager->getByWatchID(__skipBldg->powerSupply);
									if (__powerObj && __powerObj->getStatus() == OBJECT_STATUS_DESTROYED)
										appearance->setLightsOut(true);
								}
								// R2B-STATIC-NATURAL-TOUCH-PRESERVE-1: same liveness stamp as
								// the tree branch — keep the skipped static building in the
								// registry flush instead of letting cachedFrame_ go stale.
								if (r2bTouchPreserve &&
								    mc2R2bTouchStaticLiveness(appearance, (uint32_t)g_mc2FrameCounter))
									touchedLiveness++;
								skippedStaticNatural++;
								continue;
							}
							if (isFalling)
							{
								fallingNaturalUpdated++;
								missFalling++;
							}
							else if (isJustCreated)
							{
								justCreatedNaturalUpdated++;
								missJustCreated++;
							}
							else if (!staticNow)
							{
								missNotStaticNow++;
								bool staticReasonRecorded = false;
								if (TreeAppearance* treeAppearance = dynamic_cast<TreeAppearance*>(appearance))
								{
									missStaticUnregTreeAppearance++;
									const long activeLod = treeAppearance->activeLOD;
									const bool registered = (activeLod >= 0) && (activeLod < MAX_LODS) &&
									                        treeAppearance->staticReg[activeLod].registered;
									const bool shapeMatch = (activeLod != 0) ||
									                        (treeAppearance->staticReg[0].shape == treeAppearance->treeShape);
									if (!registered)
									{
										missStaticNotRegistered++;
										staticReasonRecorded = true;
									}
									else if (!shapeMatch)
									{
										missStaticShapeMismatch++;
										staticReasonRecorded = true;
									}
									else if (treeAppearance->needsFullBakeNextFrame)
									{
										missStaticNeedsFullBake++;
										staticReasonRecorded = true;
									}
								}
								else if (BldgAppearance* bldgAppearance = dynamic_cast<BldgAppearance*>(appearance))
								{
									missStaticUnregBldgAppearance++;
									if (!bldgAppearance->staticReg.registered)
									{
										missStaticNotRegistered++;
										staticReasonRecorded = true;
									}
									else if (bldgAppearance->staticReg.shape != bldgAppearance->bldgShape)
									{
										missStaticShapeMismatch++;
										staticReasonRecorded = true;
									}
									else if (bldgAppearance->needsFullBakeNextFrame)
									{
										missStaticNeedsFullBake++;
										staticReasonRecorded = true;
									}
								}
								else
								{
									missStaticUnregOtherAppearance++;
								}
								switch (obj->getObjectClass())
								{
								case TREE:
									missStaticUnregObjTree++;
									break;
								case TERRAINOBJECT:
									missStaticUnregObjTerrain++;
									break;
								case BUILDING:
									missStaticUnregObjBuilding++;
									break;
								default:
									missStaticUnregObjOther++;
									break;
								}
								if (typeName && typeName[0])
									missStaticUnregTypeCounts[std::string(typeName)]++;
								else
									missStaticUnregTypeCounts[std::string("<null>")]++;
								if (!staticReasonRecorded)
									missStaticOther++;
							}
						}
						else if (skipStaticBuildings && !isPineStaticBuildingCandidate &&
						         (objType && objType->getObjectClass() == BUILDING))
						{
							// Broadened static-building skip: all BUILDING-class objects that are
							// NOT already handled by the Pine path above (isPineStaticBuildingCandidate
							// covered those).  Only active when MC2_SKIP_STATIC_BUILDINGS=1.
							// mustUpdate exclusion set mirrors the Pine candidacy check above.
							BuildingPtr building = dynamic_cast<Building*>(obj);
							BuildingTypePtr bldgType = dynamic_cast<BuildingTypePtr>(objType);
							if (building && bldgType)
							{
								const bool mustUpdate =
									building->isSpecialBuilding() ||
									((bldgType->perimeterAlarmRange > 0.0f) && (bldgType->perimeterAlarmTimer > 0.0f)) ||
									(bldgType->lookoutTowerRange > 0.0f) ||
									(bldgType->sensorRange > 0.0f) ||
									bldgType->powerSource ||
									obj->getFlag(OBJECT_FLAG_CONTROLBUILDING) ||
									obj->getFlag(OBJECT_FLAG_MECHBAY);
								if (!mustUpdate)
								{
									staticBuildingCandidates++;
									const long staticNow = appearance ? (appearance->IsStaticNow() ? 1L : 0L) : 0L;
									if ((turn >= 3) && staticNow && !isFalling && !isJustCreated)
									{
										// Power-out propagation must run even for skipped buildings.
										// Replicates bldng.cpp:889 with an added null guard.
										if (building->powerSupply && appearance)
										{
											GameObjectPtr __powerObj = ObjectManager->getByWatchID(building->powerSupply);
											if (__powerObj && __powerObj->getStatus() == OBJECT_STATUS_DESTROYED)
												appearance->setLightsOut(true);
										}
										skippedStaticBuildings++;
										continue;
									}
									if (!staticNow)
										missBuildingNotStaticNow++;
								}
							}
						}
						// Per-object sample lines removed — use MC2_SKIP_STATIC_TREES_DIAG summary instead.

						if (isPineAppearance && !isPineStaticBuildingCandidate)
						{
							if (pineSpecialBuilding)
								missSpecial++;
							if (pinePerimeterAlarm)
								missAlarm++;
							if (pineLookout)
								missLookout++;
							if (pineSensor)
								missSensor++;
							if (pinePowerSource)
								missPower++;
							if (pineControlBuilding)
								missControl++;
							if (pineMechBay)
								missMechBay++;
						}

						if (MC2_GOM_RECON_ENABLED()) {
							if (dynamic_cast<Building*>(obj)) recon_buildings++;
							else if (dynamic_cast<Gate*>(obj)) recon_gates++;
							else if (dynamic_cast<Turret*>(obj)) recon_turrets++;
							else recon_other++;
						}

						{
							ZoneScopedN("GOM.TerrainObjects.ObjUpdate");
							long updateRet_instr = objList[objIndex]->update();
							objList[objIndex]->lastUpdateRet = (int32_t)updateRet_instr;
							if (!updateRet_instr)
							{
								//-----------------------------------------
								// Update failed, so it no longer exists...
								MC2_DESTROY(objList[objIndex], "update_false");
							}
							else
							{
								terrainObjectsUpdated++;
								if (MC2_GOM_RECON_ENABLED()) {
									recon_updated++;
								}
							}
						}
						// C0-3: emit after update() so inView is fresh; gated on getExists()
						if (objList[objIndex] && objList[objIndex]->getExists())
						{
							ZoneScopedN("GOM.TerrainObjects.GpuCullRecord");
							emitGpuCullRecord(objList[objIndex], gpu_cull::Cat_Other,
							                  gpu_cull::Consumer_RenderGate | gpu_cull::Consumer_LifecycleGate);
							cullRecordsEmitted++;
						}
					}
				}
			}
		}

		// ── FRAME-JOBS-2G: Path B timer stop + per-300-frame print ──────────────────
		if (s_pathBDiag) {
			auto _pathB_t1 = std::chrono::high_resolution_clock::now();
			int64_t frame_us = (int64_t)std::chrono::duration<double, std::micro>(_pathB_t1 - _pathB_t0).count();
			s_pathB_candidates  += pathB_candidates;
			s_pathB_touch_calls += pathB_touch_calls;
			s_pathB_skipped     += pathB_skipped;
			s_pathB_ran_nosplit += pathB_ran_nosplit;
			s_pathB_total_us    += frame_us;
			s_pathB_bldg_calls  += pathB_bldg_calls;
			s_pathB_tree_calls  += pathB_tree_calls;
			s_pathB_other_calls += pathB_other_calls;
			const int64_t curDiagFrame = (int64_t)g_mc2FrameCounter;
			if (curDiagFrame != s_pathB_diag_frame) {
				s_pathB_diag_frame = curDiagFrame;
				if (curDiagFrame > 0 && (curDiagFrame % 300) == 0) {
					double avg_us = (s_pathB_touch_calls > 0)
						? (double)s_pathB_total_us / (double)s_pathB_touch_calls
						: 0.0;
					printf("FRAME_JOBS_PATHB: candidates=%lld touch_calls=%lld"
					       " skipped=%lld ran_nosplit=%lld total_us=%lld"
					       " avg_us_per_call=%.2f bldg=%lld tree=%lld other=%lld\n",
					       (long long)s_pathB_candidates,
					       (long long)s_pathB_touch_calls,
					       (long long)s_pathB_skipped,
					       (long long)s_pathB_ran_nosplit,
					       (long long)s_pathB_total_us,
					       avg_us,
					       (long long)s_pathB_bldg_calls,
					       (long long)s_pathB_tree_calls,
					       (long long)s_pathB_other_calls);
					std::fflush(stdout);
					// Reset window accumulators
					s_pathB_candidates  = 0;
					s_pathB_touch_calls = 0;
					s_pathB_skipped     = 0;
					s_pathB_ran_nosplit = 0;
					s_pathB_total_us    = 0;
					s_pathB_bldg_calls  = 0;
					s_pathB_tree_calls  = 0;
					s_pathB_other_calls = 0;
				}
			}
		}
		// ── end FRAME-JOBS-2G ────────────────────────────────────────────────────────

		// MC2_GOM_RECON: emit object type classification
		if (MC2_GOM_RECON_ENABLED()) {
			static uint32_t lastReconFrame = 0;
			uint32_t curFrame = g_mc2FrameCounter;
			if (curFrame != lastReconFrame) {
				lastReconFrame = curFrame;
				SPEWALWAYS(("GOM_RECON", "frame=%u buildings=%ld gates=%ld turrets=%ld other=%ld updated=%ld",
				             curFrame, recon_buildings, recon_gates, recon_turrets, recon_other, recon_updated));
			}
		}

		// R2B-STATIC-NATURAL-TOUCH-PRESERVE-1 trace (MC2_R2B_STATIC_NATURAL_TRACE=1).
		// touched_liveness should track skipped_full_update closely (every skipped
		// REGISTERED static prop gets its cachedFrame_ stamped). Correlate with the
		// registry's STATIC_PROP_REGISTRY stale-drop counter: with preserve=1 the
		// tree-typeID stale drops go to ~0; with MC2_R2B_TOUCH_PRESERVE=0 they spike.
		if (r2bTouchTrace && ((g_mc2FrameCounter <= 3u) || (g_mc2FrameCounter % 300u) == 0u)) {
			std::fprintf(stderr,
			    "R2B_STATIC_NATURAL: frame=%u skipped_full_update=%ld touched_liveness=%ld preserve=%d\n",
			    (uint32_t)g_mc2FrameCounter, skippedStaticNatural, touchedLiveness,
			    r2bTouchPreserve ? 1 : 0);
			std::fflush(stderr);
		}

		if (skipStaticNaturalDiag) {
			static uint32_t lastStaticSkipFrame = 0;
			if ((staticDiagFrame <= 5) || ((staticDiagFrame % 300) == 0)) {
				if (staticDiagFrame != lastStaticSkipFrame) {
					lastStaticSkipFrame = staticDiagFrame;
					std::fprintf(stderr,
					             "[STATIC_NATURAL_SKIP] frame=%u eligible=%ld natural=%ld skipped=%ld falling=%ld justCreated=%ld miss_notStaticNow=%ld miss_static_unregistered=%ld miss_static_shape=%ld miss_static_fullBake=%ld miss_static_other=%ld miss_unreg_treeAppr=%ld miss_unreg_bldgAppr=%ld miss_unreg_otherAppr=%ld miss_unreg_objTree=%ld miss_unreg_objTerrain=%ld miss_unreg_objBuilding=%ld miss_unreg_objOther=%ld miss_special=%ld miss_alarm=%ld miss_lookout=%ld miss_sensor=%ld miss_power=%ld miss_control=%ld miss_mechBay=%ld miss_falling=%ld miss_justCreated=%ld nonNatural=%ld unknown=%ld updated=%ld bldg_candidates=%ld bldg_skipped=%ld bldg_miss_notStaticNow=%ld\n",
					             staticDiagFrame, eligibleStaticNatural, staticNaturalCandidates, skippedStaticNatural,
					             fallingNaturalUpdated, justCreatedNaturalUpdated,
					             missNotStaticNow, missStaticNotRegistered, missStaticShapeMismatch,
					             missStaticNeedsFullBake, missStaticOther,
					             missStaticUnregTreeAppearance, missStaticUnregBldgAppearance, missStaticUnregOtherAppearance,
					             missStaticUnregObjTree, missStaticUnregObjTerrain, missStaticUnregObjBuilding, missStaticUnregObjOther,
					             missSpecial, missAlarm, missLookout, missSensor,
					             missPower, missControl, missMechBay, missFalling, missJustCreated,
					             nonNaturalUpdated, unknownUpdated, terrainObjectsUpdated,
					             staticBuildingCandidates, skippedStaticBuildings, missBuildingNotStaticNow);
					if (!missStaticUnregTypeCounts.empty())
					{
						std::vector<std::pair<std::string, long>> topUnregTypes(
							missStaticUnregTypeCounts.begin(), missStaticUnregTypeCounts.end());
						std::sort(topUnregTypes.begin(), topUnregTypes.end(),
						          [](const std::pair<std::string, long>& a, const std::pair<std::string, long>& b) {
							          if (a.second != b.second)
								          return a.second > b.second;
							          return a.first < b.first;
						          });
						const size_t topCount = std::min<size_t>(8, topUnregTypes.size());
						for (size_t i = 0; i < topCount; ++i)
						{
							std::fprintf(stderr,
							             "[STATIC_REG_HEALTH] unreg_rank=%u type=%s count=%ld\n",
							             static_cast<unsigned>(i),
							             topUnregTypes[i].first.c_str(),
							             topUnregTypes[i].second);
						}
					}
					std::fflush(stderr);
				}
			}
		}

		TracyPlot("TerrainObjects specialBuildings updated", int64_t(specialBuildingsUpdated));
		TracyPlot("TerrainObjects gates updated", int64_t(gatesUpdated));
		TracyPlot("TerrainObjects active blocks", int64_t(activeBlocksVisited));
		TracyPlot("TerrainObjects visible objects updated", int64_t(terrainObjectsUpdated));
		TracyPlot("TerrainObjects dynamic updates", int64_t(g_staticUpdateRunCount()));
		TracyPlot("TerrainObjects static skipped",  int64_t(g_staticUpdateSkipCount()));
		TracyPlot("GOM.TerrainObjects.ActiveBlocks", int64_t(activeBlocksVisited));
		TracyPlot("GOM.TerrainObjects.VisitedObjects", int64_t(terrainObjectsVisited));
		TracyPlot("GOM.TerrainObjects.UpdatedObjects", int64_t(terrainObjectsUpdated));
		TracyPlot("GOM.TerrainObjects.CullRecords", int64_t(cullRecordsEmitted));

		const uint32_t curFrame = g_mc2FrameCounter;
		if (curFrame > 0 && (curFrame % 600) == 0 &&
		    curFrame != g_staticUpdateLastSummaryFrame_get()) {
			g_staticUpdateEmitSummary(curFrame);
		}
		// [TOBJSPLIT v1] once-per-frame roll + 600-frame summary, placed at the
		// end of the per-frame loop (frame counter lives in terrobj.cpp). The
		// analogous terrain SlimSplit roll was removed by 8Z-DEADCODE-SWEEP-1.
		g_tobjSplitRollAndMaybeEmit();
		// [TOBJPARITY v1] once-per-frame roll + 120-frame summary for the
		// superset-parity counter probe (proof-gate #2). Called here at the
		// same per-frame boundary as TOBJSPLIT (see static_update_counters.h).
		g_tobjParityRollAndMaybeEmit();

		// FRAME-JOBS-1: per-frame trace
		if (frameJobsEnabled() && frameJobsTrace()) {
			FrameJobsFrameStats stats = frameJobsGetFrameStats();
			printf("FRAME_JOBS: workers=%d chunks=%d handles=%d serial=%s\n",
			       stats.workerCount,
			       stats.chunksExecuted,
			       frameJobsPrePassCount,
			       stats.serialFallback ? "yes" : "no");
		}
	}
	
 	if (movers) {
		static MoverPtr removeList[MAX_MOVERS];
		long numRemoved = 0;

		{ gomcost::Span _gomMoverUpdate(gomcost::kMoverUpdate); // [GOM_UPDATE_COST v1] mechs+vehicles update() walk
		if (mechs)
		{
			ZoneScopedN("GameLogic.Units.Mechs");
			for (long i = 0; i < numMechs; i++)
			{
				MoverPtr mover = mechs[i];
				if (mover && mover->getExists())
				{
					long updateRet_instr = mover->update();
					mover->lastUpdateRet = (int32_t)updateRet_instr;
					if (!updateRet_instr)
						MC2_DESTROY(mover, "update_false");
					if (mover->getFlag(OBJECT_FLAG_REMOVED))
						removeList[numRemoved++] = mover;
					// C0-3: emit after update() so inView is fresh; gated on getExists()
					if (mover->getExists())
						emitGpuCullRecord(mover, gpu_cull::Cat_Mech,
						                  gpu_cull::Consumer_AIGate | gpu_cull::Consumer_WeaponSpawnNode |
						                  gpu_cull::Consumer_LifecycleGate | gpu_cull::Consumer_RenderGate);
				}
			}
		}

		if (vehicles)
		{
			ZoneScopedN("GameLogic.Units.Vehicles");
			for (long i = 0; i < maxVehicles; i++)
			{
				MoverPtr mover = vehicles[i];
				if (mover && mover->getExists())
				{
					long updateRet_instr = mover->update();
					mover->lastUpdateRet = (int32_t)updateRet_instr;
					if (!updateRet_instr)
						MC2_DESTROY(mover, "update_false");
					if (mover->getFlag(OBJECT_FLAG_REMOVED))
						removeList[numRemoved++] = mover;
					// C0-3: emit after update() so inView is fresh; gated on getExists()
					if (mover->getExists())
						emitGpuCullRecord(mover, gpu_cull::Cat_GroundVeh,
						                  gpu_cull::Consumer_AIGate | gpu_cull::Consumer_WeaponSpawnNode |
						                  gpu_cull::Consumer_LifecycleGate | gpu_cull::Consumer_RenderGate);
				}
			}
		}

		for (long i = 0; i < numRemoved; i++)
			mission->removeMover(removeList[i]);
		} // [GOM_UPDATE_COST v1] end mover_update span

		// BRAIN-COMMIT-PHASE-1: deferred intent commit phase (MC2_BRAIN_COMMIT_PHASE=1).
		// Runs AFTER all mechs and vehicles have called update() (runBrain/updateActions).
		// Requires MC2_BRAIN_INTENT_QUEUE=1; gate OFF = no-op (inline-commit path unchanged).
		// Warriors are committed in ascending WID order for deterministic multi-warrior ordering.
		// Each warrior with pendingIntentCount > 0 gets commitBrainIntents() called once.
		// Emits: [BRAIN_COMMIT_PHASE] committed=<n> warriors in WID order
		{
			static const bool s_commitPhaseGate = ([](){
				const char* v = std::getenv("MC2_BRAIN_COMMIT_PHASE");
				return (v && std::atoi(v) != 0);
			})();
			static const bool s_intentQueueGate = ([](){
				const char* v = std::getenv("MC2_BRAIN_INTENT_QUEUE");
				return (v && std::atoi(v) != 0);
			})();
			if (s_commitPhaseGate && s_intentQueueGate && numMovers > 0) {
				// Collect (WID, mover) pairs for stable ordering.
				static std::vector<std::pair<long, MoverPtr>> s_commitOrder;
				s_commitOrder.clear();
				for (long mi = 0; mi < numMovers; mi++) {
					MoverPtr mover = moverList[mi];
					if (!mover || !mover->getExists()) continue;
					MechWarriorPtr pilot = mover->getPilot();
					if (!pilot) continue;
					MechBrainRuntime* rt = pilot->getBrainRuntime();
					if (!rt || rt->pendingIntentCount <= 0) continue;
					s_commitOrder.emplace_back(mover->getWatchID(), mover);
				}
				// Sort ascending by WID — deterministic, not pointer/iteration order.
				std::sort(s_commitOrder.begin(), s_commitOrder.end(),
				          [](const std::pair<long, MoverPtr>& a, const std::pair<long, MoverPtr>& b){
				              return a.first < b.first;
				          });
				int committed = 0;
				for (auto& kv : s_commitOrder) {
					MoverPtr mover = kv.second;
					if (!mover->getExists()) continue;  // destroyed during this frame's mover loop
					MechWarriorPtr pilot = mover->getPilot();
					if (!pilot) continue;
					MechBrainRuntime* rt = pilot->getBrainRuntime();
					if (!rt || rt->pendingIntentCount <= 0) continue;
					commitBrainIntents(pilot, rt);
					++committed;
				}
				if (committed > 0) {
					std::fprintf(stderr, "[BRAIN_COMMIT_PHASE] committed=%d warriors in WID order\n", committed);
					std::fflush(stderr);
				}
			}
		}
	}

	if (other) {
		gomcost::Span _gomOtherFx(gomcost::kOtherFx); // [GOM_UPDATE_COST v1] turrets/weapons/carnage/lights/artillery
		//---------------------------------------
		// All other objects should be updated...

		if (turrets)
		{
			ZoneScopedN("GameLogic.Units.Turrets");
			for (long i=0;i<numTurrets;i++)
			{
				if (turrets[i] && turrets[i]->getExists())
				{
					long updateRet_instr = turrets[i]->update();
					turrets[i]->lastUpdateRet = (int32_t)updateRet_instr;
					if (!updateRet_instr)
						MC2_DESTROY(turrets[i], "update_false");
					// C0-3: emit after update() so inView is fresh; gated on getExists()
					if (turrets[i] && turrets[i]->getExists())
						emitGpuCullRecord(turrets[i], gpu_cull::Cat_Turret,
						                  gpu_cull::Consumer_RenderGate | gpu_cull::Consumer_LifecycleGate);
				}
			}
		}

		if (weapons) {
			for (long i=0;i<numWeapons;i++) {
				if (weapons[i] && weapons[i]->getExists()) {
					long updateRet_instr = weapons[i]->update();
					weapons[i]->lastUpdateRet = (int32_t)updateRet_instr;
					if (!updateRet_instr)
						MC2_DESTROY(weapons[i], "update_false");
				}
			}
		}

		if (carnage) {
			for (long i = 0; i < numCarnage; i++) {
				if (carnage[i] && carnage[i]->getExists()) {
					long updateRet_instr = carnage[i]->update();
					carnage[i]->lastUpdateRet = (int32_t)updateRet_instr;
					if (!updateRet_instr)
						MC2_DESTROY(carnage[i], "update_false");
				}
			}
		}

		if (lights) {
			for (long i = 0; i < numLights; i++) {
				if (lights[i] && lights[i]->getExists()) {
					long updateRet_instr = lights[i]->update();
					lights[i]->lastUpdateRet = (int32_t)updateRet_instr;
					if (!updateRet_instr)
						MC2_DESTROY(lights[i], "update_false");
				}
			}
		}

		if (artillery) {
			for (long i = 0; i < numArtillery; i++) {
				if (artillery[i] && artillery[i]->getExists()) {
					long updateRet_instr = artillery[i]->update();
					artillery[i]->lastUpdateRet = (int32_t)updateRet_instr;
					if (!updateRet_instr)
						MC2_DESTROY(artillery[i], "update_false");
				}
			}
		}
	}

	// C0-3: finalize GPU cull substrate SSBO for this frame.
	// Called unconditionally — substrate_flushUpload internally checks isEnabled().
	{ ZoneScopedN("GOM.substrateFlushUpload");
	  gomcost::Span _gomSubstrate(gomcost::kSubstrateFlush); // [GOM_UPDATE_COST v1]
	  gpu_cull::substrate_flushUpload(); }

	// MC2_MOVE_RECON: per-frame tick (lazy init, atexit register, periodic emit).
	moveReconFrameTick();
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::get (GameObjectHandle handle) {

	if ((handle < 1) || (handle > getMaxObjects()))
		return(NULL);

	return(objList[handle]);
}

//---------------------------------------------------------------------------

long GameObjectManager::buildMoverLists (void) {

	numMovers = 0;
	numGoodMovers = 0;
	numBadMovers = 0;

	for (long i = 0; i < numMechs; i++) {
		MoverPtr mover = dynamic_cast<MoverPtr>(mechs[i]);
		if (!mover->getTeam())
			continue;
		moverList[numMovers++] = mover;
		if (mover->getTeam()->isFriendly(Team::home))
			goodMoverList[numGoodMovers++] = mover;
		else if (mover->getTeam()->isEnemy(Team::home))
			badMoverList[numBadMovers++] = mover;
	}

	for (int i = 0; i < numVehicles; i++) {
		MoverPtr mover = dynamic_cast<MoverPtr>(vehicles[i]);
		if (!mover->getTeam())
			continue;
		moverList[numMovers++] = mover;
		if (mover->getTeam()->isFriendly(Team::home))
			goodMoverList[numGoodMovers++] = mover;
		else if (mover->getTeam()->isEnemy(Team::home))
			badMoverList[numBadMovers++] = mover;
	}

	return(NO_ERR);
}

//---------------------------------------------------------------------------

bool GameObjectManager::modifyMoverLists (MoverPtr mover, long action) {

	switch (action) {
		case MOVERLIST_DELETE: {
			bool foundIt = false;
			if (mover->getObjectClass() == BATTLEMECH) {
				long i = 0;
				for (i = 0; i < numMechs; i++) {
					if (mechs[i] == mover)
						break;
				}
				if (i < numMechs) {
					foundIt = true;
					BattleMechPtr mech = mechs[i];
					memmove(&mechs[i], &mechs[i + 1], (maxMechs - i - 1) * sizeof(BattleMechPtr));
					mechs[maxMechs - 1] = mech;
					numMechs--;
				}
				}
			else if (mover->getObjectClass() == GROUNDVEHICLE) {
				long i = 0;
				for (i = 0; i < numVehicles; i++) {
					if (vehicles[i] == mover)
						break;
				}
				if (i < numVehicles) {
					foundIt = true;
					GroundVehiclePtr vehicle = vehicles[i];
					memmove(&vehicles[i], &vehicles[i + 1], (maxVehicles - i - 1) * sizeof(GroundVehiclePtr));
					vehicles[maxVehicles - 1] = vehicle;
					numVehicles--;
				}
			}

			for (long i = 0; i < numMovers; i++)
				if (moverList[i] == mover) {
					moverList[i] = moverList[--numMovers];
					break;
				}

			if (foundIt && mover->getTeam()) {
				if (mover->getTeam()->isFriendly(Team::home)) {
					long i = 0;
					for (i = 0; i < numGoodMovers; i++)
						if (mover == goodMoverList[i])
							break;
					if (i < numGoodMovers)
						goodMoverList[i] = goodMoverList[--numGoodMovers];
					}
				else if (mover->getTeam()->isEnemy(Team::home)) {
					long i = 0;
					for (i = 0; i < numBadMovers; i++)
						if (mover == badMoverList[i])
							break;
					if (i < numBadMovers)
						badMoverList[i] = badMoverList[--numBadMovers];
				}
			}
			return(foundIt);
		}
		case MOVERLIST_ADD:
			moverList[numMovers++] = mover;
			if (mover->getTeam()) {
				if (mover->getTeam()->isFriendly(Team::home))
					goodMoverList[numGoodMovers++] = mover;
				else if (mover->getTeam()->isEnemy(Team::home))
					badMoverList[numBadMovers++] = mover;
			}
			break;
		case MOVERLIST_TRADE:
			//-----------------------------------------------
			// First, remove it from whatever list it's on...
			long i = 0;
			for (i = 0; i < numGoodMovers; i++)
				if (mover == goodMoverList[i])
					break;
			if (i < numGoodMovers)
				goodMoverList[i] = goodMoverList[--numGoodMovers];
			else {
				long i = 0;
				for (i = 0; i < numBadMovers; i++)
					if (mover == badMoverList[i])
						break;
				if (i < numBadMovers)
					badMoverList[i] = badMoverList[--numBadMovers];
			}
			//----------------------------------
			// Now, add it to the proper list...
			if (mover->getTeam()) {
				if (mover->getTeam()->isFriendly(Team::home))
					goodMoverList[numGoodMovers++] = mover;
				else if (mover->getTeam()->isEnemy(Team::home))
					badMoverList[numBadMovers++] = mover;
			}
	}
	return(true);
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findObject (Stuff::Vector3D position) {

	float closestDistance = 10000.0;
	GameObjectPtr closestObj = NULL;
	long numObjects = getMaxObjects();
	for (long objIndex = 1; objIndex <= numObjects; objIndex++) {
		GameObjectPtr obj = objList[objIndex];
		Assert(obj != NULL, objIndex, " GameObjectManager.findObject: NULL obj ");
		if (obj->getExists() && !obj->inTransport()) {
			float distanceFromObject = obj->distanceFrom(position);
			if (distanceFromObject < closestDistance)
				if (distanceFromObject < obj->getExtentRadius()) {
					closestObj = obj;
					closestDistance = distanceFromObject;
				}
		}
	}

	return(closestObj);
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findObjectByTypeHandle (long typeHandle) {

	//-------------------------------------------------
	// This function was called findObjectId() in MC...

	long numObjects = getMaxObjects();
	for (long objIndex = 1; objIndex <= numObjects; objIndex++) {
		GameObjectPtr obj = objList[objIndex];
		if (obj->getExists() && (obj->getTypeHandle() == typeHandle))
			return(obj);
	}
	return(NULL);
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findByPartId (long partId) {

	if (partId == 0)
		return(NULL);

	long numObjects = getMaxObjects();
	for (long objIndex = 1; objIndex <= numObjects; objIndex++) 
	{
		GameObjectPtr obj = objList[objIndex];
		if (obj && obj->getExists() && (obj->getPartId() == partId))
			return(obj);
	}
	return(NULL);
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findByBlockVertex (long blockNum, long vertex) {

	//----------------------------------------
	// SLOW compared to using object handle...
	long partId = calcPartId(TERRAINOBJECT, blockNum, vertex);
	return(findByPartId(partId));
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findByCellPosition (long row, long col) 
{
	//-------------------------------------------------------------------------------------
	// Must implement for Linkage code.  10/20/99 -fs
	// PLEASE DO NOT CALL EVERY FRAME. THIS ONE WILL BE SLOWER THEN CHRISTMAS!!!!!!!!!!!!
	// Store the pointer if at all possible.  YOU HAVE BEEN WARNED!
	long numObjects = getMaxObjects();
	for (long objIndex = 1; objIndex <= numObjects; objIndex++)
	{
		GameObjectPtr obj = objList[objIndex];
		if (obj->getExists())
		{
			int cellR, cellC;
			objList[objIndex]->getCellPosition(cellR,cellC);

			if ((cellR == row) && (cellC == col))
				return objList[objIndex];
		}
	}

	return(NULL);
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findByUnitInfo (long commander, long group, long mate) {

	//----------------------------------------
	// SLOW compared to using object handle...
	long partId = calcPartId(MOVER, commander, group, mate);
	return(findByPartId(partId));
}

//---------------------------------------------------------------------------

// Lazy pick-time screen projection. Replaces the per-frame recalcBounds
// projection body (deleted 2026-05-18, Task 2/3, commits 4294937/d5c5546/
// 69e7968/d511ad9) for the ONLY surviving consumer: mouse-pick (per-click
// over already-cull-narrowed active blocks, not per-frame). Populates a
// local screen rect; PerPolySelect (geometry-space, mclib/bdactor.cpp,
// survives) does the precise hit-test unchanged. Typed BldgAppearance* --
// Appearance* has no position/getPosition; position/rotation are public on
// ObjectAppearance, typeUpperLeft/typeLowerRight on appearType.
//
// Byte-equivalence note: the deleted recalcBounds block's FINAL
// upperLeft/lowerRight (the values pick consumed) were ENTIRELY the
// 8-corner projected min/max -- the screenPos-seeded values it computed
// first were unconditionally overwritten by the corner min/max loop
// (pre-delete mclib/bdactor.cpp lines 1396-1399, rev 4294937^). This
// helper therefore seeds the rect from corner[0] (matching the pre-delete
// `if(!i){maxX=minX=bcsp[i].x;...}` init) and does NOT seed from a
// projection of `position`, so the resulting rect is byte-identical to
// the old screen rect. The 8-corner boxCoords construction below is
// lifted VERBATIM from `git show 4294937^:mclib/bdactor.cpp` lines
// 1288-1399 (the deleted `if (inView)` projection block); do not
// "improve" the box geometry -- pick must remain byte-equivalent or
// building selection silently shifts/misses.
static bool projectPickCandidateRect(BldgAppearance* ba,
                                     long& outMinX, long& outMinY,
                                     long& outMaxX, long& outMaxY)
{
	if (!ba || !eye)
		return false;

	// appearType is the public BldgAppearanceType* member (mclib/bdactor.h
	// :192); typeUpperLeft/typeLowerRight live on the AppearanceType base
	// (mclib/apprtype.h:52-53).
	BldgAppearanceType* appearType = ba->appearType;
	if (!appearType)
		return false;

	const Stuff::Vector3D& position = ba->position;
	float rotation = ba->rotation;

	Stuff::Vector3D boxCoords[8];
	Stuff::Vector4D bcsp[8];

	Stuff::Vector3D boxStart;
	boxStart.x = -appearType->typeUpperLeft.x;
	boxStart.y = appearType->typeUpperLeft.z;
	boxStart.z = appearType->typeUpperLeft.y;

	Stuff::Vector3D boxEnd;
	boxEnd.x = -appearType->typeLowerRight.x;
	boxEnd.y = appearType->typeLowerRight.z;
	boxEnd.z = appearType->typeLowerRight.y;

	Stuff::Vector3D addCoords;

	addCoords.x = boxStart.x;
	addCoords.y = boxStart.y;
	addCoords.z = boxEnd.z;
	if (rotation != 0.0f)
		Rotate(addCoords,-rotation);

	boxCoords[0].Add(position,addCoords);

	addCoords.x = boxStart.x;
	addCoords.y = boxEnd.y;
	addCoords.z = boxEnd.z;
	if (rotation != 0.0f)
		Rotate(addCoords,-rotation);

	boxCoords[1].Add(position,addCoords);

	addCoords.x = boxEnd.x;
	addCoords.y = boxEnd.y;
	addCoords.z = boxEnd.z;
	if (rotation != 0.0f)
		Rotate(addCoords,-rotation);

	boxCoords[2].Add(position,addCoords);

	addCoords.x = boxEnd.x;
	addCoords.y = boxStart.y;
	addCoords.z = boxEnd.z;
	if (rotation != 0.0f)
		Rotate(addCoords,-rotation);

	boxCoords[3].Add(position,addCoords);

	addCoords.x = boxStart.x;
	addCoords.y = boxStart.y;
	addCoords.z = boxStart.z;
	if (rotation != 0.0f)
		Rotate(addCoords,-rotation);

	boxCoords[4].Add(position,addCoords);

	addCoords.x = boxEnd.x;
	addCoords.y = boxStart.y;
	addCoords.z = boxStart.z;
	if (rotation != 0.0f)
		Rotate(addCoords,-rotation);

	boxCoords[5].Add(position,addCoords);

	addCoords.x = boxEnd.x;
	addCoords.y = boxEnd.y;
	addCoords.z = boxStart.z;
	if (rotation != 0.0f)
		Rotate(addCoords,-rotation);

	boxCoords[6].Add(position,addCoords);

	addCoords.x = boxStart.x;
	addCoords.y = boxEnd.y;
	addCoords.z = boxStart.z;
	if (rotation != 0.0f)
		Rotate(addCoords,-rotation);

	boxCoords[7].Add(position,addCoords);

	float maxX = 0.0f, maxY = 0.0f;
	float minX = 0.0f, minY = 0.0f;

	// INPUT-CURSOR-OFFSCREEN-TARGET-RECON-1 / -FIX-1: a near-plane-clipped OBB corner
	// projects to (0,0) with w<=1e-4 (camera.h:860-862). Including it in the screen-rect
	// min/max stretches the rect to the screen origin, so an object whose lower corners
	// sit below terrain (and thus behind the near plane at low camera angles) yields a
	// huge bogus rect that swallows the cursor -> wrong/offscreen object wins the pick.
	// PROVEN in TangoMaster m1: behind_corners=4, pos.z=48.7 (terrain ~325), rect
	// [-3369,-3759,1675,809] contained a cursor 436px away.
	// FIX: build the rect from in-front corners only (skip w<=1e-4). If ALL corners are
	// behind the camera the object has no valid screen rect this frame -> not pickable.
	// Killswitch MC2_PICK_BEHIND_LEGACY=1 restores the byte-identical legacy behavior.
	static const bool s_pickBehindLegacy = (std::getenv("MC2_PICK_BEHIND_LEGACY") != nullptr);
	int behindCount = 0;

	for (long i=0;i<8;i++)
	{
		eye->projectForScreenXY(boxCoords[i],bcsp[i]);
		if (bcsp[i].w <= 1e-4f)
			++behindCount;
		if (!i)
		{
			maxX = minX = bcsp[i].x;
			maxY = minY = bcsp[i].y;
		}

		if (i)
		{
			if (bcsp[i].x > maxX)
				maxX = bcsp[i].x;

			if (bcsp[i].x < minX)
				minX = bcsp[i].x;

			if (bcsp[i].y > maxY)
				maxY = bcsp[i].y;

			if (bcsp[i].y < minY)
				minY = bcsp[i].y;
		}
	}

	// Pre-delete recalcBounds wrote these as upperLeft.x=minX, upperLeft.y
	// =minY, lowerRight.x=maxX, lowerRight.y=maxY (rev 4294937^ lines
	// 1396-1399); the pick test then compared (float)upperLeft/lowerRight.
	// The float->long narrowing here is the only representational change;
	// (long)(float screen coord) truncates toward zero, and the pick
	// comparison below is integer mouseX/mouseY against the rect, so the
	// hit set is byte-equivalent for all on-screen rects.
	outMinX = (long)minX;
	outMinY = (long)minY;
	outMaxX = (long)maxX;
	outMaxY = (long)maxY;

	// CURSOR_TARGET mechanism emit: only when a corner was behind the camera (rare,
	// exactly the bug). Gate MC2_CURSOR_TARGET_TRACE + MC2_DIAG_TAGS=CURSOR_TARGET.
	// Read via get_diagnostic_events("CURSOR_TARGET"). No behavior change.
	if (behindCount > 0)
	{
		static const bool s_cursorTargetTrace = (std::getenv("MC2_CURSOR_TARGET_TRACE") != nullptr);
		if (s_cursorTargetTrace && mc2_diag::tagEnabled("CURSOR_TARGET"))
		{
			char _ct_buf[256];
			snprintf(_ct_buf, sizeof(_ct_buf),
			         "{\"site\":\"pick_rect\",\"behind_corners\":%d,\"pos\":[%.1f,%.1f,%.1f],\"rect\":[%ld,%ld,%ld,%ld]}",
			         behindCount, position.x, position.y, position.z,
			         outMinX, outMinY, outMaxX, outMaxY);
			mc2_diag::writeEvent("CURSOR_TARGET", 1, 0, _ct_buf);
		}
	}

	// INPUT-CURSOR-OFFSCREEN-TARGET-FIX-2: any OBB corner behind the near plane means
	// the camera straddles / sits inside the object's bounding box (e.g. a prop ~280
	// units below terrain, viewed from above). Near-plane corners — both the ones
	// behind (project to (0,0)) AND the in-front ones with tiny w (perspective divide
	// blows them to extreme screen coords) — make the screen rect span the whole view,
	// so the object wins the pick at every pixel ("cannot capture" wall). No reliable
	// 2D rect exists for such an object; reject it. A building selected normally (camera
	// outside it) has all 8 corners in front -> behindCount==0 -> unaffected.
	// Killswitch MC2_PICK_BEHIND_LEGACY=1 restores the legacy (broken) behavior.
	if (!s_pickBehindLegacy && behindCount > 0)
		return false;

	return true;
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findObjectByMouse (long mouseX,
													long mouseY,
													GameObjectPtr* searchList,
													long listSize,
													bool skipDisabled) {

	if (!searchList)
		Fatal(0, " GameObjectManager.findObjectByMouse: NULL searchList ");

	for (long objIndex = 0; objIndex < listSize; objIndex++) 
	{
		if (searchList[objIndex] && searchList[objIndex]->getExists()) 
		{
			GameObjectPtr obj = searchList[objIndex];
			Assert(obj != NULL, objIndex, " GameObjectManager.findObjectByMouse: NULL obj ");
			AppearancePtr objAppearance = obj->getAppearance();
			if (objAppearance && objAppearance->canBeSeen()) 
			{
				if (obj->getWindowsVisible() == (turn - VISIBLE_THRESHOLD))
				{
					//-----------------------------------------------------
					float tlx = objAppearance->upperLeft.x;
					float tly = objAppearance->upperLeft.y;
					float brx = objAppearance->lowerRight.x;
					float bry = objAppearance->lowerRight.y;
						
					if ((mouseX >= tlx) && 
						(mouseX <= brx) &&
						(mouseY >= tly) &&
						(mouseY <= bry)) 
					{
						//---------------------------
						// We're on it, so save it...
						if (!obj->isMover() || (obj->isMover() && obj->isOnGUI() && Terrain::IsGameSelectTerrainPosition(obj->getPosition())))
						{
							if (skipDisabled) 
							{
								if (!obj->isDisabled() && 
									(obj->getObjectClass() != TREE) && 
									(obj->getDamageLevel() != 36000000) && 				//We are a rock clump
									objAppearance->PerPolySelect(mouseX, mouseY))
									return(obj);
							}
							else
							{
								//Do not target trees or artillery strikes!!
								if ((obj->getObjectClass() != TREE) && 
									(obj->getObjectClass() != ARTILLERY) &&                                  									(obj->getDamageLevel() != 36000000) && 				//We are a rock clump
									(obj->getDamageLevel() != 36000000) && 				//We are a rock clump
									objAppearance->PerPolySelect(mouseX, mouseY))
									return(obj);
							}
						}
					}
				}
			}
		}
	}
	
	return(NULL);
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findMoverByMouse (long mouseX,
												   long mouseY,
												   long commanderId,
												   bool skipDisabled) {

	GameObjectPtr* searchList = NULL;
	long numMovers = getMaxMovers();
	if (objList)
		searchList = &objList[1];

	if (!searchList)
		return(NULL);

	if (commanderId == -1)
		for (long objIndex = 0; objIndex < numMovers; objIndex++) 
		{
			if (searchList[objIndex] && searchList[objIndex]->getExists()) 
			{
				GameObjectPtr obj = searchList[objIndex];
				Assert(obj != NULL, objIndex, " GameObjectManager.findObjectByMouse: NULL obj ");
				AppearancePtr objAppearance = obj->getAppearance();
				if (objAppearance && objAppearance->canBeSeen()) 
				{
					if (obj->getWindowsVisible() == (turn - VISIBLE_THRESHOLD)) 
					{
						//-----------------------------------------------------
						float tlx = objAppearance->upperLeft.x;
						float tly = objAppearance->upperLeft.y;
						float brx = objAppearance->lowerRight.x;
						float bry = objAppearance->lowerRight.y;
							
						if ((mouseX >= tlx) && 
							(mouseX <= brx) &&
							(mouseY >= tly) &&
							(mouseY <= bry)) 
						{
							//---------------------------
							// We're on it, so save it...
							// Movers are NOT per poly!!
							if (!obj->isMover() || (obj->isMover() && obj->isOnGUI() && Terrain::IsGameSelectTerrainPosition(obj->getPosition())))
							{
								if (skipDisabled) 
								{
									if (!obj->isDisabled())
										return(obj);
								}
								else
									return(obj);
							}
						}
					}
				}
			}
		}
	else
		for (long objIndex = 0; objIndex < numMovers; objIndex++) 
		{
			if (searchList[objIndex] && searchList[objIndex]->getExists()) 
			{
				GameObjectPtr obj = searchList[objIndex];
				Assert(obj != NULL, objIndex, " GameObjectManager.findObjectByMouse: NULL obj ");
				if (obj->getCommanderId() == commanderId)
					continue;
				AppearancePtr objAppearance = obj->getAppearance();
				if (objAppearance && objAppearance->canBeSeen()) 
				{
					if (obj->getWindowsVisible() == (turn - VISIBLE_THRESHOLD)) 
					{
						//-----------------------------------------------------
						float tlx = objAppearance->upperLeft.x;
						float tly = objAppearance->upperLeft.y;
						float brx = objAppearance->lowerRight.x;
						float bry = objAppearance->lowerRight.y;
							
						if ((mouseX >= tlx) && 
							(mouseX <= brx) &&
							(mouseY >= tly) &&
							(mouseY <= bry)) 
						{
							if (!obj->isMover() || (obj->isMover() && obj->isOnGUI() && Terrain::IsGameSelectTerrainPosition(obj->getPosition())))
							{
								//---------------------------
								// We're on it, so save it...
								if (skipDisabled) 
								{
									if (!obj->isDisabled())
										return(obj);
								}
								else
									return(obj);
							}
						}
					}
				}
			}
		}

	return(NULL);
}

//---------------------------------------------------------------------------

// ---- MC2_PICK_RECON: recon-only split of findTerrainObjectByMouse (the dense-
// urban FindTerrainObj 3.6ms hotspot). Default OFF, NO behavior change. Counts
// candidates (objects in active blocks), inRect-passers (-> PerPolySelect volume),
// rectProj calls, and per-call wall-time. Run interactively (mouse over a dense
// cluster) to size the fix: candidate-volume vs per-PerPolySelect cost vs the
// per-frame block scan. (NEXT-SESSION measurement scaffolding; see memory.)
#include <chrono>
namespace {
	static const bool s_pickRecon = (getenv("MC2_PICK_RECON") != nullptr);
	static unsigned long long s_pkCalls=0, s_pkCandidates=0, s_pkInRect=0, s_pkRectProj=0;
	static unsigned long long s_pkNs=0, s_pkMaxNs=0, s_pkMaxCand=0;
	static bool s_pkAtexit=false;
	static void pkEmit() {
		if (!s_pickRecon) return;
		std::printf("[PICK_RECON v1] event=shutdown calls=%llu candidates=%llu inRect=%llu "
			"rectProj=%llu total_ms=%.1f avg_us=%.1f max_us=%.1f max_candidates_call=%llu\n",
			s_pkCalls, s_pkCandidates, s_pkInRect, s_pkRectProj, (double)s_pkNs/1e6,
			s_pkCalls?(double)s_pkNs/s_pkCalls/1000.0:0.0, (double)s_pkMaxNs/1000.0, s_pkMaxCand);
		std::fflush(stdout);
	}
	struct PkTimer {
		std::chrono::steady_clock::time_point t0; bool on;
		PkTimer():on(s_pickRecon){ if(on){ if(!s_pkAtexit){s_pkAtexit=true;std::atexit(pkEmit);} t0=std::chrono::steady_clock::now(); ++s_pkCalls; } }
		~PkTimer(){ if(!on)return; unsigned long long ns=(unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count(); s_pkNs+=ns; if(ns>s_pkMaxNs)s_pkMaxNs=ns; }
	};
}

GameObjectPtr GameObjectManager::findTerrainObjectByMouse (long mouseX,
														   long mouseY,
														   bool skipDisabled)
{
	// v0+: top-level cache on (mouseX, mouseY, cameraRevision).
	// Returns immediately when cursor and camera are both unchanged.
	// cameraRevision driven by Camera::worldToClip memcmp — covers every
	// mutation path (position/rotation/FOV/viewport/cinematic/script).
	// Updated at the single return site below.
	static long         s_pmX = -999999, s_pmY = -999999;
	static uint32_t     s_pmCamRev = 0;
	static unsigned long s_pmWatchID = 0;   // UAF FIX: cache by watch-ID, not raw pointer
	uint32_t camRev = eye ? eye->getViewProjectionRevision() : 0;
	// PICK-CACHE-UAF FIX: the picked object can be destroyed between frames (e.g. a building
	// blown up, or any mover killed). Caching the raw GameObjectPtr and later calling a
	// virtual on it (getExists()) dispatches through a freed vtable -> EXEC violation
	// (objmgr.cpp:3781, crash seen in findTerrainObjectByMouse). Cache the watch-ID instead
	// and re-resolve through getByWatchID(), which returns NULL if the object is gone.
	if (mouseX == s_pmX && mouseY == s_pmY &&
		camRev != 0 && camRev == s_pmCamRev)
	{
		return s_pmWatchID ? getByWatchID(s_pmWatchID) : NULL;
	}
	// Camera moved this frame — return stale (watch-ID-validated) result without scanning.
	// Hover highlight is cosmetic; player is panning, not selecting.
	// camRev is updated so that once camera settles the next frame triggers a fresh scan.
	if (camRev != 0 && camRev != s_pmCamRev) {
		s_pmCamRev = camRev;
		return s_pmWatchID ? getByWatchID(s_pmWatchID) : NULL;
	}

	PkTimer _pk;  // MC2_PICK_RECON per-call wall-time
	int32_t pickCandidates = 0;
	int32_t activeBlocksVisited = 0;
	GameObjectPtr pickBest = NULL;        // BUILDING-PICK FIX: world-space nearest-to-camera
	float         pickBestDistSq = 3.4e38f;
	for (long terrainBlock = 0; terrainBlock < Terrain::numObjBlocks; terrainBlock++)
	{
		// Cull-cascade consumer (mouse pick). Post-8c source of truth for
		// objBlockInfo[].active / objVertexActive[] is the cull-merged Step 6
		// slim reduction loop in mclib/terrain.cpp (8c-part-1 merges the
		// per-vertex cull writes into that loop) — NOT the deleted VPL body,
		// NOT a "Step 5 / 5B slim pass" (that producer never existed; v3.2
		// deleted it). See VPL-retirement plan v3.5 note (CRIT-0).
		if (Terrain::objBlockInfo[terrainBlock].active)
		{
			activeBlocksVisited++;
			long numObjs = Terrain::objBlockInfo[terrainBlock].numObjects;
			long objIndex = Terrain::objBlockInfo[terrainBlock].firstHandle;
			pickCandidates += numObjs;
				if (s_pickRecon) { s_pkCandidates += (unsigned long long)numObjs; if ((unsigned long long)numObjs > s_pkMaxCand) s_pkMaxCand = (unsigned long long)numObjs; }

			// CRIT-1 re-home (Task 4): the per-object test below is
			// DUPLICATED from the shared 5-param findObjectByMouse loop
			// body (canBeSeen guard -> windowsVisible equality -> coarse
			// rect -> PerPolySelect) as a TERRAIN-STATIC-ONLY inline
			// test. Terrain statics are NO LONGER routed through the
			// shared 5-param overload; that overload is left byte-
			// UNCHANGED for its other (mover) call site
			// (objmgr.cpp findObjectByMouse(...,&objList[1],
			// getMaxObjects(),false)). Replacing the guard inside the
			// shared overload would gate movers on a readback slot they
			// have no entry for -> silent mover-pick regression. The
			// lists walked here are terrain-statics-only by construction
			// (countTerrainObjects fills objBlockInfo[] only for
			// TERRAINOBJECT/TREE/TURRET/GATE/BUILDING/TREEBUILDING/
			// BRIDGE). Trees stay pick-excluded via the
			// getObjectClass()!=TREE skip (preserved verbatim below).
			GameObjectPtr* searchList = &objList[objIndex];
			for (long li = 0; li < numObjs; li++)
			{
				if (searchList[li] && searchList[li]->getExists())
				{
					GameObjectPtr obj = searchList[li];
					Assert(obj != NULL, li, " GameObjectManager.findTerrainObjectByMouse: NULL obj ");
					AppearancePtr objAppearance = obj->getAppearance();
					// CRIT-1: canBeSeen() guard repointed to the GPU cull
					// readback-visible set (fail-open when readback is
					// disabled, so a stock install with GPU cull off keeps
					// the legacy "always considered" behavior). Same idiom
					// as objmgr.cpp:1930 / mech.cpp / gvehicl.cpp.
					bool readbackVisible = gpu_cull::readback_isEnabled()
						? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(obj->getHandle()))
						: true;
					if (objAppearance && readbackVisible)
					{
						// windowsVisible equality UNCHANGED. The stamp
						// (code/terrobj.cpp TerrainObject::update(),
						// `if (inView) windowsVisible = turn;`) survives
						// via the coarse-angular recalcBounds return path:
						// the projection-body delete made `inView` a
						// strict SUPERSET (coarse-only), so the stamp
						// still fires for every pick-eligible object and
						// this equality still holds. Newly-admitted
						// objects are filtered by the lazy rect +
						// geometry-space PerPolySelect below. Do NOT
						// "fix" this equality -- it is correct by the
						// coarse-superset argument (review-verified fact).
						if (obj->getWindowsVisible() == (turn - VISIBLE_THRESHOLD))
						{
							//-----------------------------------------------------
							// CRIT-1: lazy per-candidate screen rect.
							// Class-guarded cast to BldgAppearance*.
							//
							// CORRECTION (Task 4 spec-review): an earlier
							// version of this comment claimed only
							// BUILDING/TREEBUILDING use BldgAppearance and
							// that TERRAINOBJECT/BRIDGE/TURRET/GATE "do
							// not". That was FALSE. TERRAINOBJECT and
							// BRIDGE (terrobj.cpp `appearance = new
							// BldgAppearance`), TURRET (turret.cpp same),
							// GATE (gate.cpp same) and ARTILLERY
							// (artlry.cpp same) ALL instantiate
							// BldgAppearance too. (TREE uses
							// TreeAppearance; movers use Mech3D/GV.)
							//
							// The rect pre-filter is INTENTIONALLY applied
							// ONLY to BUILDING/TREEBUILDING here. For the
							// other BldgAppearance classes the lazy rect
							// is deliberately DROPPED: skipping it is a
							// correctness-safe OVER-inclusion (a candidate
							// is never wrongly rejected by a missing
							// rect), and PerPolySelect (geometry-space,
							// run below) remains the authoritative pick
							// test for every class. This matches the pre-
							// delete behavior for a missing/zero rect (the
							// deleted projection block only produced a rect
							// for BldgAppearance, and only the
							// building-class path consumed it).
							long ulx, uly, lrx, lry;
							bool haveRect = false;
							long oc = obj->getObjectClass();
							BldgAppearance* ba =
								((oc == BUILDING) || (oc == TREEBUILDING) || (oc == TERRAINOBJECT) || (oc == BRIDGE) || (oc == TURRET) || (oc == GATE))
									? (BldgAppearance*)obj->getAppearance()
									: NULL;
							if (ba) {
								// v2: per-instance screen-rect cache keyed on cameraRevision.
								// Safe for static terrain objects (never move post-spawn).
								// camRev==0 means Camera not yet initialised: always project.
								auto& pc = ba->pickCache_;
								if (camRev != 0 && pc.valid && pc.cameraRevision == camRev) {
									// cache hit — reuse stored rect
									ulx = pc.ulx; uly = pc.uly;
									lrx = pc.lrx; lry = pc.lry;
									haveRect = true;
								} else {
									if (s_pickRecon) ++s_pkRectProj;
									haveRect = projectPickCandidateRect(ba, ulx, uly, lrx, lry);
									if (haveRect && camRev != 0) {
										pc.ulx = ulx; pc.uly = uly;
										pc.lrx = lrx; pc.lry = lry;
										pc.cameraRevision = camRev;
										pc.valid = true;
									}
								}
							}

							bool inRect = true;
							if (haveRect)
							{
								inRect = (mouseX >= ulx) &&
								         (mouseX <= lrx) &&
								         (mouseY >= uly) &&
								         (mouseY <= lry);
							}

							// BUILDING-PICK FIX (world-space): the candidate's projected world OBB
							// (projectPickCandidateRect, O(8) corners, correct eye projection) must
							// contain the cursor. Replaces per-face screen PerPolySelect, which needs
							// the CPU listOfVisibleFaces the GPU static-prop path no longer bakes
							// (numVisibleFaces==0 -> buildings were untargetable). No per-face work.
							if (haveRect && inRect)
							{
								if (s_pickRecon) ++s_pkInRect;
								if (!obj->isMover() || (obj->isMover() && obj->isOnGUI() && Terrain::IsGameSelectTerrainPosition(obj->getPosition())))
								{
									bool okClass;
									if (skipDisabled)
										okClass = (!obj->isDisabled() && (oc != TREE) && (obj->getDamageLevel() != 36000000));
									else
										okClass = ((oc != TREE) && (oc != ARTILLERY) && (obj->getDamageLevel() != 36000000));
									if (okClass)
									{
										// Disambiguate overlapping OBBs by nearest-to-camera (front building wins).
										Stuff::Vector3D _cp = eye->getPosition();
										Stuff::Vector3D _op = obj->getPosition();
										float _dx=_op.x-_cp.x, _dy=_op.y-_cp.y, _dz=_op.z-_cp.z;
										float _dd=_dx*_dx+_dy*_dy+_dz*_dz;
										if (_dd < pickBestDistSq) { pickBestDistSq = _dd; pickBest = obj; }
									}
								}
							}
						}
					}
				}
			}
		}
	}

	TracyPlot("MIF.TerrainPick.activeBlocks", int64_t(activeBlocksVisited));
	TracyPlot("MIF.TerrainPick.candidates", int64_t(pickCandidates));
	// v0+: update top-level cache before returning.
	s_pmX = mouseX; s_pmY = mouseY; s_pmCamRev = camRev;
	s_pmWatchID = pickBest ? pickBest->getWatchID() : 0;   // UAF FIX: cache watch-ID, not raw ptr
	return(pickBest);   // BUILDING-PICK FIX: nearest world-OBB hit (NULL if none)
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findObjectByMouse (long mouseX, long mouseY) {

	GameObjectPtr obj = NULL;

	long homeCommanderId = Commander::home->getId();
	{
		ZoneScopedN("MIF.FindMover");
		obj = findMoverByMouse(mouseX, mouseY, homeCommanderId, true);
		if (obj)
			return(obj);
		obj = findMoverByMouse(mouseX, mouseY, homeCommanderId, false);
		if (obj)
			return(obj);
		obj = findMoverByMouse(mouseX, mouseY, -1, true);
		if (obj)
			return(obj);
		obj = findMoverByMouse(mouseX, mouseY, -1, false);
		if (obj)
			return(obj);
	}

	{
		ZoneScopedN("MIF.FindObjList");
		obj = findObjectByMouse(mouseX, mouseY, &objList[1], getMaxObjects(),false);
		if (obj)
			return(obj);
	}

	{
		ZoneScopedN("MIF.FindTerrainObj");
		return(findTerrainObjectByMouse(mouseX, mouseY,true));
	}
}

//---------------------------------------------------------------------------
bool GameObjectManager::moverInRect(long index, Stuff::Vector3D &dStart, Stuff::Vector3D &dEnd)
{
	//------------------------------------------------------------------------------
	// This function checks the mover passed in to see if it
	// is within the magic rectangle.  It assumes we are looking for our alignment.
	// This is because we are drag selecting in the GUI!
	if ((index < 0) || (index >= getMaxMovers()))
		return(false);
		
	MoverPtr checkMover = getMover(index);
	if (checkMover && checkMover->getExists() && (checkMover->getTeam() == Team::home))
	{
		AppearancePtr objAppearance = checkMover->getAppearance();
		if (objAppearance /*&& objAppearance->canBeSeen() */) 
		{
//			if (checkMover->getWindowsVisible() > (turn - VISIBLE_THRESHOLD)) 
			{
				//-----------------------------------------------------
				// Note that this effectively tests inTransport, since
				// windowsVisible only gets set when not inTransport...
				//objAppearance->recalcBounds();	//Shouldn't need to do this.  They should already be correct!
				long left 	= fmin(dStart.x, dEnd.x);
				long right 	= fmax(dStart.x, dEnd.x);
				long top 	= fmin(dStart.y, dEnd.y);
				long bottom = fmax(dStart.y, dEnd.y);
				
				if ((left <= objAppearance->getScreenPos().x) && 
					(right >= objAppearance->getScreenPos().x) &&
					(top <= objAppearance->getScreenPos().y) &&
					(bottom >= objAppearance->getScreenPos().y))
				{
					return(true);
				}
			}
		}
	}
	
	return(false);
}	

//---------------------------------------------------------------------------

ObjectTypePtr GameObjectManager::loadObjectType (ObjectTypeNumber typeHandle) {

	return(objTypeManager->load(typeHandle));
}

//---------------------------------------------------------------------------

ObjectTypePtr GameObjectManager::getObjectType (ObjectTypeNumber typeHandle) {

	return(objTypeManager->get(typeHandle));
}

//---------------------------------------------------------------------------

void GameObjectManager::removeObjectType (ObjectTypeNumber typeHandle) {

	objTypeManager->remove(typeHandle);
}

//---------------------------------------------------------------------------

GameObjectHandle GameObjectManager::getHandle (GameObjectPtr obj) {

	for (long i = 1; i <= getMaxObjects(); i++)
		if (objList[i] == obj)
			return(i);
	return(0);
}

//---------------------------------------------------------------------------

long GameObjectManager::calcPartId (long objectClass, long param1, long param2, long param3) {

	//-------------------------------------------------------------------
	// This should be the only function used to calc partIds for objects.
	// This will allow easy modification of the formulas without making
	// changes everywhere...
	long partId = 0;
	switch (objectClass) {
		case MOVER:
		case BATTLEMECH:
		case GROUNDVEHICLE:
		case ELEMENTAL:
			if (param1 == -1) {
				if (nextReinforcementPartId == (MAX_REINFORCEMENT_PART_ID + 1))
					return(0);
				partId = nextReinforcementPartId++;
				}
			else
				partId = MIN_MOVER_PART_ID +
						 param1 * MAX_MOVERGROUPS * MAX_MOVERGROUP_COUNT_START +
						 param2 * MAX_MOVERGROUP_COUNT_START +
						 param3;
			break;
		case TERRAINOBJECT:
		case TREE:
		case BUILDING:
		case TREEBUILDING:
		case TURRET:
		case GATE:
			partId = MIN_TERRAIN_PART_ID + param1 * MAX_MAP_CELL_WIDTH + param2;
			break;
		case TRAINCAR:
			Fatal(0, " TRAINS NEED PART IDS ");
			break;
	}
	return(partId);
}

//---------------------------------------------------------------------------

void GameObjectManager::setPartId (GameObjectPtr obj, long param1, long param2, long param3) {

	//------------------------------------------------------------------------
	// ALL game objects should have their partIds set through this function.
	// This simply allows easier modification and monitoring of the partIds,
	// should we have any reason to modify or test 'em as missions are loaded.
	long partId = calcPartId(obj->getObjectClass(), param1, param2, param3);
	obj->setPartId(partId);
}

//---------------------------------------------------------------------------

long GameObjectManager::initCollisionSystem (FitIniFile* missionFile) {

	collisionSystem = new CollisionSystem;
	gosASSERT(collisionSystem != NULL);
		
	collisionSystem->init(missionFile);

	return(0);
}

//---------------------------------------------------------------------------

long GameObjectManager::buildCollidableList (void) 
{
	rebuildCollidableList = false;
	
	if (collidableList) 
	{
		ObjectTypeManager::objectCache->Free(collidableList);
		collidableList = NULL;
	}

	// First, how many collidables are there?
	numCollidables = numMechs + numVehicles + numElementals + numTurrets + numGates + numCarnage + numArtillery;

	collidableList = (GameObjectPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(GameObjectPtr) * numCollidables);

	long curIndex = 0;
	for (long i = 0; i < numMechs; i++)
		collidableList[curIndex++] = mechs[i];

	for (int i = 0; i < numVehicles; i++)
		collidableList[curIndex++] = vehicles[i];

	for (int i = 0; i < numElementals; i++)
		collidableList[curIndex++] = (GameObjectPtr)elementals[i];

	for (int i=0;i<numTurrets;i++)
		collidableList[curIndex++] = turrets[i];

	for (int i=0;i<numGates;i++)
		collidableList[curIndex++] = gates[i];

	for (int i=0;i<numCarnage;i++)
		collidableList[curIndex++] = carnage[i];
		
	for (int i=0;i<numArtillery;i++)
		collidableList[curIndex++] = artillery[i];

	Assert(curIndex == numCollidables, curIndex, " GameObjectManager.buildCollidableList: oof ");
	
	return(0);
}

//---------------------------------------------------------------------------

long GameObjectManager::getCollidableList (GameObjectPtr*& objList) 
{
	if (rebuildCollidableList)
		buildCollidableList();
		
	objList = collidableList;
	return(numCollidables);
}

//---------------------------------------------------------------------------

long GameObjectManager::updateCollisions (void) {

	if (!collidableList)
		buildCollidableList();

	collisionSystem->checkObjects();

	return(0);
}

//---------------------------------------------------------------------------

void GameObjectManager::detectStaticCollision (GameObjectPtr obj1, GameObjectPtr obj2) {

	collisionSystem->detectStaticCollision(obj1, obj2);
}

//---------------------------------------------------------------------------

void GameObjectManager::updateCaptureList (void) {

	for (int i = 0; i < MAX_TEAMS; i++)
		numCaptures[i] = 0;
	for (int i = 0; i < getNumMovers(); i++) {
		MoverPtr mover = getMover(i);
		if (mover->isDisabled())
			continue;
		MechWarriorPtr pilot = mover->getPilot();
		if (pilot) {
			TacticalOrderPtr tacOrder;
			tacOrder = pilot->getCurTacOrder();
			if (tacOrder->code == TACTICAL_ORDER_CAPTURE) {
				captureList[mover->getTeamId()][numCaptures[mover->getTeamId()]++] = tacOrder->targetWID;
			}
		}
	}
}

//---------------------------------------------------------------------------

bool GameObjectManager::isTeamCapturing (TeamPtr team, GameObjectWatchID targetWID) {

	if (team) {
		long teamID = team->getId();
		for (long i = 0; i < numCaptures[teamID]; i++)
			if (targetWID == captureList[teamID][i])
				return(true);
		}
	else {
		for (long teamID = 0; teamID < MAX_TEAMS; teamID++)
			for (long i = 0; i < numCaptures[teamID]; i++)
				if (targetWID == captureList[teamID][i])
					return(true);
	}
	return(false);
}

//---------------------------------------------------------------------------

CarnagePtr GameObjectManager::createFire (ObjectTypeNumber fireObjTypeHandle,
										  GameObjectPtr owner,
										  Stuff::Vector3D& position,
										  float tonnage) {

	CarnagePtr fire = getCarnage(CARNAGE_FIRE);
	if (fire) {
		ObjectTypePtr objectType = getObjectType(fireObjTypeHandle);
		if (objectType) {
			//------------------------------------------------------------
			// Make sure the object type we loaded is really a fire object
			// type. If not, NULL out of here...
			if (objectType->getObjectClass() != FIRE)
				return(NULL);
			fire->init(false, objectType);
			fire->setOwner(owner);
			fire->setTonnage(tonnage);
			fire->setPosition(position);
			fire->setExists(true);
		}
	}
	return(fire);
}

//---------------------------------------------------------------------------

CarnagePtr GameObjectManager::createExplosion (long effectId,
											   GameObjectPtr owner,
											   Stuff::Vector3D& position,
											   float damage,
											   float radius) 
{
	long explosionObjTypeHandle = weaponEffects->GetEffectObjNum(effectId);
	if (explosionObjTypeHandle != INVALID_OBJECT) 
	{
		CarnagePtr explosion = getCarnage(CARNAGE_EXPLOSION);
		if (explosion)
		{
			ObjectTypePtr objectType = getObjectType(explosionObjTypeHandle);
			if (objectType && (objectType->getObjectClass() == EXPLOSION))
			{
				//Call in this order or badness.
				explosion->init(effectId);
				explosion->init(false, objectType);
				explosion->setPosition(position);
				explosion->setExtentRadius(radius);
				explosion->setExplDmg(damage);
				explosion->setExists(true);
				explosion->setOwner(owner);
				return(explosion);
			}
			else if (objectType && (objectType->getObjectClass() == FIRE))
			{
				//Call in this order or badness.
				explosion->init(effectId);
				explosion->init(false, objectType);
				explosion->setPosition(position);
				explosion->setExtentRadius(radius);
				explosion->setExplDmg(damage);
				explosion->setExists(true);
				explosion->setOwner(owner);
				return(explosion);
			}
			else
			{
				//Turn on when effects are officially working!
				// Don't really care if nothing happens.  Should be a bug not a crash!
				// -fs
			}
		}
		else
		{
			//Should we warn if we are out of explosions or just go on?
			//-fs
		}
	}

	return(NULL);
}

//---------------------------------------------------------------------------

LightPtr GameObjectManager::createLight (ObjectTypeNumber lightObjTypeHandle) {

	LightPtr light = getLight();
	if (light) {
		ObjectTypePtr objectType = getObjectType(lightObjTypeHandle);
		light->init(false, objectType);
//		light->setOwner(owner);
//		light->setTonnage(tonnage);
		Stuff::Vector3D startPosition;
		startPosition.Zero();
		light->setPosition(startPosition);
		light->setExists(true);
	}
	return(light);
}

//---------------------------------------------------------------------------
WeaponBoltPtr GameObjectManager::createWeaponBolt (long effectId) 
{
	WeaponBoltPtr weaponBolt = getWeapon();
	if (weaponBolt) 
	{
		ObjectTypeNumber weaponBoltObjTypeHandle = weaponEffects->GetEffectObjNum(effectId);
		ObjectTypePtr objectType = getObjectType(weaponBoltObjTypeHandle);
		if (!objectType)
		{
			STOP(("Object Type for a weapon Bolt was NULL.  EffectId: %d  ObjType: %d",effectId, weaponBoltObjTypeHandle));
			// STOP is a no-op in RelWithDebInfo, so guard the null-deref below
			// explicitly: drop the effect (caller handles a NULL weaponFX) instead
			// of crashing on objectType->getObjectClass() when an effect type has
			// no registered WEAPONBOLT object (e.g. asset gap on a given map).
			return(NULL);
		}
		if (objectType->getObjectClass() != WEAPONBOLT)
			return(NULL);
		//ALWAYS CALL IN THIS ORDER OR NO EFFECT!!!!!!!!!!!!!!!!!!!!
		weaponBolt->init(effectId);
		weaponBolt->init(false, objectType);
		weaponBolt->setExists(true);
	}

	return(weaponBolt);
}

//---------------------------------------------------------------------------

ArtilleryPtr GameObjectManager::createArtillery (long artilleryType, Stuff::Vector3D& position) 
{
	ArtilleryPtr artillery = getArtillery();
	if (artillery) 
	{
		static long strikeObjectId[NUM_ARTILLERY_TYPES] = 
		{
			SMALL_ARTLRY, BIG_ARTLRY, SENSOR_ARTLRY
		};
		
 		ObjectTypePtr objectType = getObjectType(strikeObjectId[artilleryType]);
		artillery->init(false, objectType);
		artillery->setPosition(position);
		artillery->setExists(true);
	}
	
	return(artillery);
}

void GameObjectManager::updateAppearancesOnly( bool terrain, bool movers, bool other)
{

	if (terrain && renderObjects)
	{
		for (long terrainBlock = 0; terrainBlock < Terrain::numObjBlocks; terrainBlock++)
		{
			// Cull-cascade consumer (object collect). Post-8c source of truth for
			// objBlockInfo[].active / objVertexActive[] is the cull-merged Step 6
			// slim reduction loop in mclib/terrain.cpp (8c-part-1 merges the
			// per-vertex cull writes into that loop) — NOT the deleted VPL body,
			// NOT a "Step 5 / 5B slim pass" (that producer never existed; v3.2
			// deleted it). See VPL-retirement plan v3.5 note (CRIT-0).
			if (Terrain::objBlockInfo[terrainBlock].active)
			{
				long numObjs = Terrain::objBlockInfo[terrainBlock].numObjects;
				long objIndex = Terrain::objBlockInfo[terrainBlock].firstHandle;
				for (long terrainObj = 0; terrainObj < numObjs; terrainObj++, objIndex++)
				{
					if (objList[objIndex] &&
						Terrain::objVertexActive[objList[objIndex]->getVertexNum()] &&
						objList[objIndex]->getExists())
					{
						if (objList[objIndex]->getAppearance()->recalcBounds()) 
						{
							// Must force here as well.
							Stuff::Vector3D pos = objList[objIndex]->getPosition();
							float rot = objList[objIndex]->getRotation();
							pos.z = TerrainRuntime::groundElevation(pos);
							long drawFlags = objList[objIndex]->drawFlags;
							int teamID = objList[objIndex]->getTeamId();
							objList[objIndex]->getAppearance()->setObjectParameters(pos,rot,drawFlags,teamID,Team::getRelation(teamID, Team::home->getId()));

							objList[objIndex]->getAppearance()->update( false );
							objList[objIndex]->getAppearance()->setVisibility(true, true);	
						}
					}
				}
			}
		}
	}

	if (movers) 
	{
		if (mechs) 
		{
			for (long i = 0; i < numMechs; i++)
				if (mechs[i] && mechs[i]->getExists())
				{
					bool inView = mechs[i]->getAppearance()->recalcBounds();
					if (inView)
						mechs[i]->windowsVisible = turn;
					
					// Must force here as well.
					Stuff::Vector3D pos = mechs[i]->getPosition();
					float rot = mechs[i]->getRotation();
					pos.z = TerrainRuntime::groundElevation(pos);
					bool selected = mechs[i]->isSelected();
					int teamID = mechs[i]->getTeamId();
					mechs[i]->getAppearance()->setObjectParameters(pos,rot,mechs[i]->drawFlags,teamID,Team::getRelation(teamID, Team::home->getId()));

					mechs[i]->getAppearance()->update( false );
					mechs[i]->getAppearance()->setVisibility(inView,true);
				}
		}

		if (vehicles) 
		{
			for (long i = 0; i < numVehicles; i++)
				if (vehicles[i] && vehicles[i]->getExists())
				{
					bool inView = vehicles[i]->getAppearance()->recalcBounds();
					vehicles[i]->windowsVisible = turn;

					// Must force here as well.
					Stuff::Vector3D pos = vehicles[i]->getPosition();
					float rot = vehicles[i]->getRotation();
					pos.z = TerrainRuntime::groundElevation(pos);
					if ((pos.z < MapData::waterDepth))
						pos.z = MapData::waterDepth;
					bool selected = vehicles[i]->isSelected();
					int teamID = vehicles[i]->getTeamId();
					vehicles[i]->getAppearance()->setObjectParameters(pos, rot, vehicles[i]->drawFlags, teamID, Team::getRelation(teamID, Team::home->getId()));
					vehicles[i]->getAppearance()->update( false );
					vehicles[i]->getAppearance()->setInView( inView );
				}
		}

	}

	if (other) {
		//---------------------------------------
		// All other objects should be updated...
		if (turrets) 
		{
			for (long i=0;i<numTurrets;i++) 
			{
				if (turrets[i] && turrets[i]->getExists()) 
				{
					if (turrets[i]->getAppearance()->recalcBounds())
					{
						// Must force here as well.
						Stuff::Vector3D pos = turrets[i]->getPosition();
						float rot = turrets[i]->getRotation();
						pos.z = TerrainRuntime::groundElevation(pos);
						bool selected = turrets[i]->isSelected();
						int teamID = turrets[i]->getTeamId();
						turrets[i]->getAppearance()->setObjectParameters(pos,rot,selected,teamID,Team::getRelation(teamID, Team::home->getId()));

						turrets[i]->getAppearance()->update( false );
						turrets[i]->getAppearance()->setInView( 1 );
					}
					else
						turrets[i]->getAppearance()->setInView( 0 );
				}
			}
		}
		
/*		if (weapons) {
			for (long i=0;i<numWeapons;i++) {
				if (weapons[i] && weapons[i]->getExists()) {
					if (!weapons[i]->update())
						MC2_DESTROY(weapons[i], "update_false");
				}
			}
		}*/

		/*if (carnage) {
			for (long i = 0; i < numCarnage; i++) {
				if (carnage[i] && carnage[i]->getExists()) {
					if (!carnage[i]->update())
						MC2_DESTROY(carnage[i], "update_false");
				}
			}
		}*/

		if (lights) 
		{
			for (long i = 0; i < numLights; i++) 
			{
				if (lights[i] && lights[i]->getExists()) 
				{
					if (lights[i]->getAppearance()->recalcBounds() )
					{
						lights[i]->getAppearance()->update( false );
						lights[i]->getAppearance()->setInView( 1 );
					}
					else
						lights[i]->getAppearance()->setInView(0);
				}
			}
		}

		if (artillery) 
		{
			for (long i = 0; i < numArtillery; i++) 
			{
				if (artillery[i] && artillery[i]->getExists()) 
				{
					if (artillery[i]->getAppearance()->recalcBounds() )
					{
						artillery[i]->getAppearance()->update( false );
						artillery[i]->getAppearance()->setInView( 1 );
					}
					else
						artillery[i]->getAppearance()->setInView( 0 );
				}
			}
		}
	}
}

//-------------------------------------------------------------------
void GameObjectManager::CopyTo (ObjectManagerData *data)
{
	data->maxObjects 		= getMaxObjects();
	data->numElementals		= numElementals;
	data->numTerrainObjects = numTerrainObjects;
	data->numBuildings		= numBuildings;
	data->numTurrets		= numTurrets;
	data->numWeapons		= numWeapons;
	data->numCarnage		= numCarnage;
	data->numLights			= numLights;
	data->numArtillery		= numArtillery;
	data->numGates			= numGates;
	data->maxMechs			= maxMechs;
	data->maxVehicles		= maxVehicles;
	data->numMechs			= numMechs;
	data->numVehicles		= numVehicles;
	data->nextWatchId		= nextWatchID;
}

//-------------------------------------------------------------------
void GameObjectManager::CopyFrom (ObjectManagerData *data)
{
	numElementals		=    data->numElementals;
	numTerrainObjects   =    data->numTerrainObjects;
	numBuildings        =    data->numBuildings;
	numTurrets          =    data->numTurrets;
	numWeapons          =    data->numWeapons;
	numCarnage          =    data->numCarnage;
	numLights           =    data->numLights;
	numArtillery        =    data->numArtillery;
	numGates            =    data->numGates;
	maxMechs            =    data->maxMechs;
	maxVehicles         =    data->maxVehicles;
	numMechs            =    data->numMechs;
	numVehicles         =    data->numVehicles;

	// WATCHID-LOAD-GUARD-1: nextWatchId is untrusted on-disk data. getByWatchID()
	// reads watchList[watchID] for watchID < nextWatchID, and watchList is sized
	// getMaxObjects()+1 (allocated in Load right after this), so a corrupt or
	// cross-version nextWatchId would let a later getByWatchID() index out of
	// bounds. Clamp to the allocation. The count fields above are already applied,
	// so getMaxObjects() reflects the loaded scene. Never trust the serialized
	// value blindly (mirrors the Save-side clamp in OBJMGR-WATCHID-BOUNDS-1).
	const long _cap = getMaxObjects() + 1;
	const unsigned long _clampedNext = mc2watch::clampSavedNextWatchId(data->nextWatchId, getMaxObjects());
	if ((long)_clampedNext != data->nextWatchId)
		mc2_watchidLoadDiag("nextWatchId", (long)data->nextWatchId, _cap);
	nextWatchID = _clampedNext;
}

//-------------------------------------------------------------------
long GameObjectManager::Save (PacketFilePtr file, long packetNum)
{
	int32_t *watchSave = (int32_t*)malloc(sizeof(int32_t) * (getMaxObjects() + 1));
	memset(watchSave,0,sizeof(int32_t) * (getMaxObjects() + 1));

	ObjectManagerData data;
	CopyTo(&data);

	file->writePacket(packetNum,(MemoryPtr)&data,sizeof(ObjectManagerData),STORAGE_TYPE_RAW);
	packetNum++;

	for (long i=0;i<=getMaxObjects();i++)
	{
		if (objList[i])
		{
			objList[i]->Save(file,packetNum);
			packetNum++;

			//Find the watchID from the watchlist for this object.
			// If none, let it be.  It'll be zero already
			// DO NOT CALL getWatchID!!!!!!!
			// That will assign them to objects which don't have them!!!!
			// OBJMGR-WATCHID-BOUNDS-1: clamp independently of setWatchID —
			// watchList and watchSave are both sized getMaxObjects()+1, so j must
			// not exceed getMaxObjects(). Previously j<=nextWatchID could read
			// watchList[j] / write watchSave[j] one past the allocation
			// (nextWatchID can legitimately equal getMaxObjects()+1).
			const long _watchHi = mc2watch::clampSaveLoopLimit(nextWatchID, getMaxObjects());
			for (long j=0;j<=_watchHi;j++)
			{
				if (watchList[j] == objList[i])
				{
					watchSave[j] = i;
				}
			}
		}
		else
		{
			packetNum++;
		}
	}

	file->writePacket(packetNum,(MemoryPtr)watchSave, sizeof(int32_t) * (getMaxObjects() + 1), STORAGE_TYPE_ZLIB);
	packetNum++;

	free(watchSave);
	watchSave = NULL;

	return packetNum;
}

//-------------------------------------------------------------------
long GameObjectManager::Load (PacketFilePtr file, long packetNum)
{
	ObjectManagerData data;
	file->readPacket(packetNum,(MemoryPtr)&data);
	packetNum++;

	CopyFrom(&data);

	//------------------------------------------------------------------
	//Create the ObjList and WatchList EXACTLY like we normally do.
	GameObject::setInitialize(true);

	//-----------------------------------------------------------
	// First element in list is NULL (handle of 0 is always NULL)
	objList = (GameObjectPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(GameObjectPtr) * (getMaxObjects() + 1));
	memset(objList,0,sizeof(GameObjectPtr) * (getMaxObjects() + 1));

	watchList = (GameObjectPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(GameObjectPtr) * (getMaxObjects() + 1));
	memset(watchList,0,sizeof(GameObjectPtr) * (getMaxObjects() + 1));

	// MF3-GENERATIONAL-HANDLE-1: Load re-alloc of the generation side array, in
	// LOCKSTEP with watchList above. Load = fresh generations by design (reset
	// to 0); generation is runtime-only and never deserialized.
	watchGeneration = (uint16_t*)ObjectTypeManager::objectCache->Malloc(sizeof(uint16_t) * (getMaxObjects() + 1));
	memset(watchGeneration,0,sizeof(uint16_t) * (getMaxObjects() + 1));

	long i = 0;
	long curHandle = 1;
	maxMovers = maxMechs + maxVehicles + numElementals;
	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (maxMechs > 0) 
	{
		mechs = (BattleMechPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(BattleMechPtr) * maxMechs);
		if (!mechs)
			Fatal(maxMechs, " GameObjectManager.setNumObjects: cannot malloc mechs ");
	}

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (maxVehicles > 0) 
	{
		vehicles = (GroundVehiclePtr*)ObjectTypeManager::objectCache->Malloc(sizeof(GroundVehiclePtr) * maxVehicles);
		if (!vehicles)
			Fatal(maxVehicles, " GameObjectManager.setNumObjects: cannot malloc vehicles ");
	}

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (numTerrainObjects > 0) 
	{
		terrainObjects = (TerrainObjectPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(TerrainObjectPtr) * numTerrainObjects);
		if (!terrainObjects)
			Fatal(numTerrainObjects, " GameObjectManager.setNumObjects: cannot malloc terrain objects ");
	}

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (numBuildings > 0) 
	{
		buildings = (BuildingPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(BuildingPtr) * numBuildings);
		if (!buildings)
			Fatal(numBuildings, " GameObjectManager.setNumObjects: cannot malloc buildings ");
	}

	if (numTurrets > 0)
	{
		turrets = (TurretPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(TurretPtr) * numTurrets);
		if ( !turrets )
			Fatal(numTurrets, " GameObjectManager.setNumObjects: cannot malloc Turrets ");
	}

	if (numGates > 0)
	{
		gates = (GatePtr*)ObjectTypeManager::objectCache->Malloc(sizeof(GatePtr) * numGates);
		if ( !gates )
			Fatal(numGates, " GameObjectManager.setNumObjects: cannot malloc Gates ");
	}

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (numWeapons > 0) 
	{
		weapons = (WeaponBoltPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(WeaponBoltPtr) * numWeapons);
		if (!weapons)
			Fatal(numWeapons, " GameObjectManager.setNumObjects: cannot malloc weapons ");
	}

	if (numCarnage > 0) 
	{
		carnage = (CarnagePtr*)ObjectTypeManager::objectCache->Malloc(sizeof(CarnagePtr) * numCarnage);
		if (!carnage)
			Fatal(numCarnage, " GameObjectManager.setNumObjects: cannot malloc carnage ");
	}

	if (numArtillery > 0) 
	{
		artillery = (ArtilleryPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(ArtilleryPtr) * numArtillery);
		if (!artillery)
			Fatal(numArtillery, " GameObjectManager.setNumObjects: cannot malloc artillery ");
	}

	useMoverLineOfSightTable = true;
	moverLineOfSightTable = (char*)systemHeap->Malloc(maxMovers * maxMovers);
	if (!moverLineOfSightTable)
		Fatal(numGates, " GameObjectManager.setNumObjects: cannot malloc moverLineOfSightTable ");

	long curTerrObjNum = 0;
	long curBuildingNum = 0;
	long curTurretNum = 0;
	long curGateNum = 0;
	long curArtilleryNum = 0;
	long curCarnageNum = 0;
	long curMechNum = 0;
	long curVehicleNum = 0;
	long curBoltNum = 0;

	long oldBlockNum = -1;
	for (i=0;i<=getMaxObjects();i++)
	{
		//OK.
		// For each object that was saved, Load the data.
		// If objectTypeNum != 0, create the object from the type EXACTLY like we do above.
		// then init from the savedData.
		// Next Object.

		//First, get the size of the packet saved.
		// This will tell me what kind of object it was!
		file->seekPacket(packetNum);
		DWORD packetSize = file->getPacketSize();
		if (packetSize == 0)
		{
			//NO Object stored here.
			// Increment Packet and move on
			// First object in ObjList, for example!!
			packetNum++;
		}
		else if ((packetSize == sizeof(GameObjectData)) ||
				(packetSize == sizeof(MoverData)))
		{
			STOP(("We saved a pure GameObject or Pure Mover in slot %d, packet %d",i,packetNum));
		}
		else if (packetSize == sizeof(TerrainObjectData))
		{
			//We have a TerrainObject.
			// Get its data.
			TerrainObjectData data;
			file->readPacket(packetNum,(MemoryPtr)(&data));
			packetNum++;

			TerrainObjectPtr obj = new TerrainObject; 
			objList[i] = terrainObjects[curTerrObjNum] = obj;
			objList[i]->setHandle(i);

			if (data.objectTypeNum > 0)	//We have an object type to copy from!!
			{
				ObjectTypePtr objType = getObjectType(data.objectTypeNum);
				if (!objType)
					STOP(("We saved an Object we don't know how to re-create %d",data.objectTypeNum));

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
				obj->setDamage(data.damage);
				obj->setRotation(data.rotation);

				if (data.blockNumber != oldBlockNum)
				{
					oldBlockNum = data.blockNumber;
					Terrain::objBlockInfo[data.blockNumber].firstHandle = i;
				}

				Terrain::objBlockInfo[data.blockNumber].numCollidableObjects++;
				Terrain::objBlockInfo[data.blockNumber].numObjects++;
			}
			else
			{
				MC2_DESTROY(obj, "load_empty_slot");
			}

			curTerrObjNum++;
		}
		else if (packetSize == sizeof(BuildingData))
		{
			//We have a Building.
			BuildingData data;
			file->readPacket(packetNum,(MemoryPtr)(&data));
			packetNum++;

			BuildingPtr obj = new Building; 
			objList[i] = buildings[curBuildingNum] = obj;
			objList[i]->setHandle(i);

			if (data.objectTypeNum > 0)	//We have an object type to copy from!!
			{
				ObjectTypePtr objType = getObjectType(data.objectTypeNum);
				if (!objType)
					STOP(("We saved an Object we don't know how to re-create %d",data.objectTypeNum));

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
				obj->setDamage(data.damage);
				obj->setRotation(data.rotation);

				if (data.blockNumber != oldBlockNum)
				{
					oldBlockNum = data.blockNumber;
					Terrain::objBlockInfo[data.blockNumber].firstHandle = i;
				}

				if (((((BuildingTypePtr)objType)->perimeterAlarmRange > 0.0f) &&
					(((BuildingTypePtr)objType)->perimeterAlarmTimer > 0.0f)) ||
					(((BuildingTypePtr)objType)->lookoutTowerRange > 0.0f) ||
					(((BuildingTypePtr)objType)->sensorRange > 0.0f))
				{
					Terrain::objBlockInfo[data.blockNumber].numCollidableObjects++;
				}

				Terrain::objBlockInfo[data.blockNumber].numObjects++;
			}
			else
			{
				MC2_DESTROY(obj, "load_empty_slot");
			}
			curBuildingNum++;
		}
		else if (packetSize == sizeof(TurretData))
		{
			//We have a Turret.
			TurretData data;
			file->readPacket(packetNum,(MemoryPtr)(&data));
			packetNum++;

			TurretPtr obj = new Turret; 
			objList[i] = turrets[curTurretNum] = obj;
			objList[i]->setHandle(i);

			if (data.objectTypeNum > 0)	//We have an object type to copy from!!
			{
				ObjectTypePtr objType = getObjectType(data.objectTypeNum);
				if (!objType)
					STOP(("We saved an Object we don't know how to re-create %d",data.objectTypeNum));

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
				obj->setDamage(data.damage);
				obj->setRotation(data.rotation);

				if (data.blockNumber != oldBlockNum)
				{
					oldBlockNum = data.blockNumber;
					Terrain::objBlockInfo[data.blockNumber].firstHandle = i;
				}

				Terrain::objBlockInfo[data.blockNumber].numCollidableObjects++;
				Terrain::objBlockInfo[data.blockNumber].numObjects++;
			}
			else
			{
				MC2_DESTROY(obj, "load_empty_slot");
			}
			curTurretNum++;
		}
		else if (packetSize == sizeof(GateData))
		{
			//We have a Gate.
			GateData data;
			file->readPacket(packetNum,(MemoryPtr)(&data));
			packetNum++;

			GatePtr obj = new Gate; 
			objList[i] = gates[curGateNum] = obj;
			objList[i]->setHandle(i);

			if (data.objectTypeNum > 0)	//We have an object type to copy from!!
			{
				ObjectTypePtr objType = getObjectType(data.objectTypeNum);
				if (!objType)
					STOP(("We saved an Object we don't know how to re-create %d",data.objectTypeNum));

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
				obj->setDamage(data.damage);
				obj->setRotation(data.rotation);

				if (data.blockNumber != oldBlockNum)
				{
					oldBlockNum = data.blockNumber;
					Terrain::objBlockInfo[data.blockNumber].firstHandle = i;
				}

				Terrain::objBlockInfo[data.blockNumber].numCollidableObjects++;
				Terrain::objBlockInfo[data.blockNumber].numObjects++;
			}
			else
			{
				MC2_DESTROY(obj, "load_empty_slot");
			}
			curGateNum++;
		}
		else if (packetSize == sizeof(ArtilleryData))
		{
			//We have an Artillery.
			ArtilleryData data;
			file->readPacket(packetNum,(MemoryPtr)(&data));
			packetNum++;

			ArtilleryPtr obj = new Artillery; 
			objList[i] = artillery[curArtilleryNum] = obj;
			objList[i]->setHandle(i);

			if (data.objectTypeNum > 0)	//We have an object type to copy from!!
			{
				ObjectTypePtr objType = getObjectType(data.objectTypeNum);
				if (!objType)
					STOP(("We saved an Object we don't know how to re-create %d",data.objectTypeNum));

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
			}
			else
			{
				MC2_DESTROY(obj, "load_empty_slot");
			}
			curArtilleryNum++;
		}
		else if (packetSize == sizeof(CarnageData))
		{
			//We have a Carnage.
			CarnageData data;
			file->readPacket(packetNum,(MemoryPtr)(&data));
			packetNum++;

			CarnagePtr obj = new Carnage; 
			objList[i] = carnage[curCarnageNum] = obj;
			objList[i]->setHandle(i);

			if (data.objectTypeNum > 0)	//We have an object type to copy from!!
			{
				ObjectTypePtr objType = getObjectType(data.objectTypeNum);
				if (!objType)
					STOP(("We saved an Object we don't know how to re-create %d",data.objectTypeNum));

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
			}
			else
			{
				MC2_DESTROY(obj, "load_empty_slot");
			}
			curCarnageNum++;
		}
		else if (packetSize == sizeof(MechData))
		{
			//We have a BattleMech.
			MechData data;
			file->readPacket(packetNum,(MemoryPtr)(&data));
			packetNum++;

			BattleMechPtr obj = new BattleMech; 
			objList[i] = mechs[curMechNum] = obj;
			objList[i]->setHandle(i);

			if (data.objectTypeNum > 0)	//We have an object type to copy from!!
			{
				ObjectTypePtr objType = getObjectType(data.objectTypeNum);
				if (!objType)
					STOP(("We saved an Object we don't know how to re-create %d",data.objectTypeNum));

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
			}
			else
			{
				MC2_DESTROY(obj, "load_empty_slot");
			}
			curMechNum++;
		}
		else if (packetSize == sizeof(GroundVehicleData))
		{
			//We have a groundVehicle.
			GroundVehicleData data;
			file->readPacket(packetNum,(MemoryPtr)(&data));
			packetNum++;

			GroundVehiclePtr obj = new GroundVehicle; 
			objList[i] = vehicles[curVehicleNum] = obj;
			objList[i]->setHandle(i);

			if (data.objectTypeNum > 0)	//We have an object type to copy from!!
			{
				ObjectTypePtr objType = getObjectType(data.objectTypeNum);
				if (!objType)
					STOP(("We saved an Object we don't know how to re-create %d",data.objectTypeNum));

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
			}
			else
			{
				MC2_DESTROY(obj, "load_empty_slot");
			}
			curVehicleNum++;
		}
		else if (packetSize == sizeof(WeaponBoltData))
		{
			//We have a WeaponBolt.
			WeaponBoltData data;
			file->readPacket(packetNum,(MemoryPtr)(&data));
			packetNum++;

			WeaponBoltPtr obj = new WeaponBolt; 
			objList[i] = weapons[curBoltNum] = obj;
			objList[i]->setHandle(i);

			if (data.objectTypeNum > 0)	//We have an object type to copy from!!
			{
				ObjectTypePtr objType = getObjectType(data.objectTypeNum);
				if (!objType)
					STOP(("We saved an Object we don't know how to re-create %d",data.objectTypeNum));

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
			}
			else
			{
				MC2_DESTROY(obj, "load_empty_slot");
			}
			curBoltNum++;
		}
	}

	if (curTerrObjNum != numTerrainObjects)
		STOP(("Didn't load %d but instead %d TerrainObjects",numTerrainObjects,curTerrObjNum));

	if (curBuildingNum != numBuildings)
		STOP(("Didn't load %d but instead %d Buildings",numBuildings,curBuildingNum));

	if (curTurretNum != numTurrets)
		STOP(("Didn't load %d but instead %d Turrets",numTurrets,curTurretNum));

	if (curGateNum != numGates)
		STOP(("Didn't load %d but instead %d Gates",numGates,curGateNum));

	if (curArtilleryNum != numArtillery)
		STOP(("Didn't load %d but instead %d Artillery",numArtillery,curArtilleryNum));

	if (curCarnageNum != numCarnage)
		STOP(("Didn't load %d but instead %d Carnage",numCarnage,curCarnageNum));

	if (curMechNum != maxMechs)
		STOP(("Didn't load %d but instead %d Mechs",maxMechs,curMechNum));

	if (curVehicleNum != maxVehicles)
		STOP(("Didn't load %d but instead %d Vehicles",maxVehicles,curVehicleNum));

	if (curBoltNum != numWeapons)
		STOP(("Didn't load %d but instead %d WeaponBolts",numWeapons,curBoltNum));

	rebuildCollidableList = true;

	//---------------------------------------------------
	// Finally, let's build the control building lists...
	for (i = 0; i < numTurrets; i++) 
	{
		if ((turrets[i]->parentId != 0xffffffff) && (turrets[i]->parentId != 0)) 
		{
			BuildingPtr controlBuilding = (BuildingPtr)ObjectManager->findByCellPosition((turrets[i]->parentId>>16),(turrets[i]->parentId & 0x0000ffff));
			if (controlBuilding && !controlBuilding->getFlag(OBJECT_FLAG_CONTROLBUILDING)) 
			{
				controlBuilding->setFlag(OBJECT_FLAG_CAPTURABLE, true);
				controlBuilding->setFlag(OBJECT_FLAG_CONTROLBUILDING, true);
				controlBuilding->listID = numTurretControls;
				turretControls[numTurretControls++] = controlBuilding;
			}
			else if (!controlBuilding)
			{
				Stuff::Vector3D worldPos;
				if (land)
					land->cellToWorld((turrets[i]->parentId>>16),(turrets[i]->parentId & 0x0000ffff),worldPos);
				PAUSE(("Turret linked to bldg @ R %d, C %d  X:%f Y:%f No Bldg there!",(turrets[i]->parentId>>16),(turrets[i]->parentId & 0x0000ffff),worldPos.x,worldPos.y));
				turrets[i]->parentId = 0xffffffff;
			}
		}
	}

	for (i = 0; i < numGates; i++) 
	{
		if ((gates[i]->parentId != 0xffffffff) && (gates[i]->parentId != 0))
		{
			BuildingPtr controlBuilding = (BuildingPtr)ObjectManager->findByCellPosition((gates[i]->parentId>>16),(gates[i]->parentId & 0x0000ffff));
			if (controlBuilding && !controlBuilding->getFlag(OBJECT_FLAG_CONTROLBUILDING)) 
			{
				controlBuilding->setFlag(OBJECT_FLAG_CONTROLBUILDING, true);
				controlBuilding->listID = numGateControls;
				gateControls[numGateControls++] = controlBuilding;
			}
	   		else if (!controlBuilding)
	   		{
	   			PAUSE(("Gate tried to link to building at Row %d, Col %d.  No Bldg there!",(gates[i]->parentId>>16),(gates[i]->parentId & 0x0000ffff)));
				gates[i]->parentId = 0xffffffff;
	   		}
		}
	}

	//-----------------------------------------------------------------------------------
	// Create list of special buildings.  These buildings will be updated at least once
	// every frame regardless of where they are on the terrain and what is visible.
	// This is because perimeter alarms and lookout buildings and ????? must work even
	// if the player is NOT looking at them!!
	for (i=0;i<numBuildings;i++)
	{
		if (buildings[i]->isSpecialBuilding())
		{
			specialBuildings[numSpecialBuildings++] = buildings[i];
		}
	}
	
 	//--------------------------------------------------------------------
	//Now, lets point every lit building to at least one power generator.
	for (i=0;i<numBuildings;i++)
	{
		if (buildings[i]->isPowerSource())
		{
			powerGenerators[numPowerGenerators++] = buildings[i];
		}
	}
	
	if (numPowerGenerators)
	{
		for (i=0;i<numBuildings;i++)
		{
			Stuff::Vector3D maxDist;
			long generatorIndex = 0;
			maxDist.Subtract(buildings[i]->getPosition(),powerGenerators[0]->getPosition());
			float minDistance = maxDist.GetApproximateLength();
			
			for (long j=1;j<numPowerGenerators;j++)
			{
				maxDist.Subtract(buildings[i]->getPosition(),powerGenerators[j]->getPosition()); 
				float newDistance = maxDist.GetApproximateLength(); 
				
				if (newDistance < minDistance)
				{
					generatorIndex = j;
					minDistance = newDistance;
				}
			}	
			
			buildings[i]->setPowerSupply(powerGenerators[generatorIndex]);
		}
		
		for (i=0;i<numTerrainObjects;i++)
		{
			Stuff::Vector3D maxDist;
			long generatorIndex = 0;
			maxDist.Subtract(terrainObjects[i]->getPosition(),powerGenerators[0]->getPosition());
			float minDistance = maxDist.GetApproximateLength();
			
			for (long j=1;j<numPowerGenerators;j++)
			{
				maxDist.Subtract(terrainObjects[i]->getPosition(),powerGenerators[j]->getPosition()); 
				float newDistance = maxDist.GetApproximateLength(); 
				
				if (newDistance < minDistance)
				{
					generatorIndex = j;
					minDistance = newDistance;
				}
			}	
			
			terrainObjects[i]->setPowerSupply(powerGenerators[generatorIndex]);
		}
	}

	GameObject::setInitialize(false);

	//Reload the Object Watchers
	int32_t *watchSave = (int32_t*)malloc(sizeof(int32_t) * (getMaxObjects() + 1));
	memset(watchSave,0,sizeof(int32_t) * (getMaxObjects() + 1));

	file->readPacket(packetNum,(MemoryPtr)watchSave);
	packetNum++;

	//Find the watchID from the watchlist for this object.
	// If none, let it be.  It'll be zero already
	// DO NOT CALL getWatchID!!!!!!!
	// That will assign them to objects which don't have them!!!!
	// WATCHID-LOAD-GUARD-1: watchSave[] is untrusted on-disk data. A corrupt or
	// cross-version save can hold an index outside [0, maxObj]; objList is sized
	// maxObj+1. Validate each entry before dereferencing objList. On an invalid
	// index, invalidate that watch slot (treat the saved object reference as
	// gone) and continue — never OOB-read objList, never crash the load. Loop is
	// bounded by the allocated capacity (getMaxObjects()), not any serialized
	// value.
	const long _maxObj = getMaxObjects();
	for (int j=0;j<_maxObj;j++)
	{
		const int32_t _idx = watchSave[j];
		if (!mc2watch::isValidWatchSaveIndex(_idx, _maxObj))
		{
			watchList[j] = NULL;
			mc2_watchidLoadDiag("watchSave", (long)_idx, _maxObj);
			continue;
		}
		watchList[j] = objList[_idx];
	}

	free(watchSave);
	watchSave = NULL;

	return packetNum;
}

//***************************************************************************
