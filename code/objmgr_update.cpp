// GameObjectManager: GameObjectManager::update with its GOM_UPDATE_COST spans, in-view conflation probe, GPU-cull record and static-skip gates.
// Declared in the class headers; shared names in objmgr_internal.h.
#include "mc2_czstring.h"
#include <atomic>
#include "brain_execution_policy.h"
#include "job_system.h"
#include "objmgr_internal.h"
#include "sim_world_trace.h"
#include "sim_overlap.h"
#include "mc2fmt.h"
#include <utility>
#include "diagnostic_trace.h"
#include "gpu_cull_compute.h"
#include "objmgr_walk_ns.h"
#include "terrain_walk_skip.h"


// ---------------------------------------------------------------------------
// C3: env-gated lifecycle routing killswitch.
// MC2_GPU_CULL_LIFECYCLE=1 enables GPU visibility-based update() gating.
// Default off — legacy inView/canBeSeen/blockActive path unchanged.
// ---------------------------------------------------------------------------
static const bool s_gpuCullLifecycle = false;

// ---------------------------------------------------------------------------
// GOM Recon: object type classification & mutation tracking (read-only).
// MC2_GOM_RECON=1 enables per-frame logging of terrain object types and counts.
// Zero overhead when disabled. Gated diagnostics only.
// ---------------------------------------------------------------------------
static const bool s_gomReconEnabled = mc2::gate::get<"MC2_GOM_RECON">();
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
		cached = true;
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
		cached = false;
	}
	return(cached != 0);
}

static bool mc2SkipStaticNaturalDiagEnabled (void)
{
	static int cached = -1;
	if (cached < 0)
		cached = false ? 1 : 0;
	return(cached != 0);
}

static bool mc2StaticTypeNameStartsWith (mc2::czstring typeName, mc2::czstring prefix)
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
static bool s_inViewConflationEnabled = false;
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
        mc2::fmt::print_err("[INVIEW_CONFLATION v1] event=summary actor_frames={:d} render_vs_sim={:d} ({:.2f}%) render_vs_ai={:d} ({:.2f}%) sim_vs_ai={:d} ({:.2f}%) render_vs_life={:d} ({:.2f}%) sim_vs_life={:d} ({:.2f}%) life_vs_ai={:d} ({:.2f}%) movers={:d} statics={:d} mover_render_vs_ai={:d} hysteresis_N={:d}\n", mc2::fmt::as_u64(g_invConfActorFrames), mc2::fmt::as_u64(g_invConfRenderVsSim), 100.0 * static_cast<double>(g_invConfRenderVsSim) / static_cast<double>(denom), mc2::fmt::as_u64(g_invConfRenderVsAi), 100.0 * static_cast<double>(g_invConfRenderVsAi)  / static_cast<double>(denom), mc2::fmt::as_u64(g_invConfSimVsAi), 100.0 * static_cast<double>(g_invConfSimVsAi)     / static_cast<double>(denom), mc2::fmt::as_u64(g_invConfRenderVsLife), 100.0 * static_cast<double>(g_invConfRenderVsLife)/ static_cast<double>(denom), mc2::fmt::as_u64(g_invConfSimVsLife), 100.0 * static_cast<double>(g_invConfSimVsLife)   / static_cast<double>(denom), mc2::fmt::as_u64(g_invConfLifeVsAi), 100.0 * static_cast<double>(g_invConfLifeVsAi)    / static_cast<double>(denom), mc2::fmt::as_u64(g_invConfMovers), mc2::fmt::as_u64(g_invConfStatics), mc2::fmt::as_u64(g_invConfMoverRenderVsAi), mc2::fmt::as_u32(static_cast<unsigned>(s_inViewConflationHysteresisN)));
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
static const bool s_enabled = false;
enum class Phase {
    kStaticTouch = 0, kMoverUpdate, kOtherFx, kSubstrateFlush, kTotal, kPhaseCount
};
inline constexpr int kPhaseCount = std::to_underlying(Phase::kPhaseCount);
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
    explicit Span(Phase phase) : p(std::to_underlying(phase)), t0(s_enabled ? nowNs() : 0ULL) {}
    ~Span() { if (s_enabled) s_ns[p] += nowNs() - t0; }
};
// Whole-function span + window emit. Construct FIRST so its dtor runs LAST.
struct TotalSpan {
    unsigned long long t0;
    TotalSpan() : t0(s_enabled ? nowNs() : 0ULL) {}
    ~TotalSpan() {
        if (!s_enabled) return;
        s_ns[std::to_underlying(Phase::kTotal)] += nowNs() - t0;
        if (++s_frames < 60) return;
        const double wf = static_cast<double>(s_frames);
        double phaseSumUs = 0.0;
        char line[512]; int off = 0;
        off += mc2::fmt::to_buf_n(line + off, sizeof(line) - off, "[GOM_UPDATE_COST v1] event=summary frames={:d}", mc2::fmt::as_i32(s_frames));
        for (int p = 0; p < kPhaseCount; ++p) {
            const double us = static_cast<double>(s_ns[p]) / 1000.0 / wf;
            if (p != std::to_underlying(Phase::kTotal)) phaseSumUs += us;
            if (off < static_cast<int>(sizeof(line)))
                off += mc2::fmt::to_buf_n(line + off, sizeof(line) - off, " {}_us={:.1f}", mc2::fmt::safe(kName[p]), us);
        }
        const double selfUs =
            static_cast<double>(s_ns[std::to_underlying(Phase::kTotal)]) / 1000.0 / wf - phaseSumUs;
        if (off < static_cast<int>(sizeof(line)))
            mc2::fmt::to_buf_n(line + off, sizeof(line) - off, " self_us={:.1f}", selfUs);
        mc2::fmt::print_err("{}\n", mc2::fmt::safe(line));
        fflush(stderr);
        memset(s_ns, 0, sizeof(s_ns));
        s_frames = 0;
    }
};
} // namespace gomcost


// Test-only hook: destroy the first live building at a configured frame.
void GameObjectManager::updateDestroyTestHook() {
	// STATIC-BLDG-SHADOW-REBAKE-1: deterministic repro hook (test-only, no
	// production callers) proving the destroy -> rebake fix. At the given
	// frame, inflict lethal damage on the first live building via the REAL
	// gameplay damage API (Building::setDamage -- exactly what a weapon hit
	// does; does not free/delete anything, so this is safe to call outside
	// combat), driving it through setObjStatus(OBJECT_STATUS_DESTROYED) ->
	// the shadow-rebake trigger in mclib/bdactor.cpp. Off (default): a single
	// getenv + integer compare, no behavior change.
	{
		static const long s_destroyTestFrame = static_cast<long>(mc2::gate::get<"MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE_TEST_DESTROY_AT_FRAME">());
		if (s_destroyTestFrame >= 0 && static_cast<long>(g_mc2FrameCounter.load(std::memory_order_relaxed)) == s_destroyTestFrame) {
			// Content-pipeline proof: when a mounted package cooked a destroyed render shape, destroy
			// every live building of those appearances (the destroyed-state capture); otherwise the
			// first live building, as the shadow-rebake repro always did.
			auto hasDestroyedOverride = [](BuildingPtr b) {
				AppearancePtr app = b->getAppearance();
				AppearanceTypePtr type = app ? app->getAppearanceType() : nullptr;
				return type && type->getAppearanceClass() == BLDG_TYPE &&
				       static_cast<BldgAppearanceType*>(type)->bldgDmgRenderShape != nullptr;
			};
			bool any = false;
			for (long bi = 0; bi < numBuildings && !any; ++bi)
				any = buildings[bi] && !buildings[bi]->isDestroyed() && hasDestroyedOverride(buildings[bi]);
			for (long bi = 0; bi < numBuildings; ++bi) {
				BuildingPtr b = buildings[bi];
				if (!b || b->isDestroyed() || !b->getExists()) continue;
				if (any && !hasDestroyedOverride(b)) continue;
				mc2::fmt::print_err("[SHADOW_STATIC_BLDG_REBAKE_TEST v1] event=test_destroy frame={:d} buildingIndex={:d}\n", mc2::fmt::as_u32(g_mc2FrameCounter.load(std::memory_order_relaxed)), mc2::fmt::as_i32(bi));
				fflush(stderr);
				b->setDamage(b->getDamageLevel());
				if (!any) break;
			}
		}
	}
}

// Build the per-actor GPU visibility snapshot before the sweep reads it.
void GameObjectManager::updateReadbackSnapshot() {
	// C3: build per-actor GPU visibility snapshot from N-1 readback FIRST,
	// so the framesSinceActive sweep below reads a freshly-built snapshot.
	// Motion-tolerance slice: also build when READBACK alone is enabled so
	// the [GPU_CULL v1] event=motion_tolerance summary (dilated_admits +
	// conservative_or_admits counters) fires even without LIFECYCLE wired.
	// readback_buildActorVisSnapshot is no-op-cheap when no good slot exists.
	if (s_gpuCullLifecycle || gpu_cull::readback_isEnabled()) {
		ZoneScopedN("GOM.readbackSnapshot");
		// Handles index objList, including every generated terrain object.
		gpu_cull::readback_buildActorVisSnapshot(static_cast<uint32_t>(getMaxObjects())+1u);
	}
}

// Single sweep over objList that maintains framesSinceActive and the conflation probe.
void GameObjectManager::updateActiveSweep() {
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
		// framesSinceActive is read only by the MC2_DESTROY_TRACE log line and the
		// conflation probe (the other consumer, the lifecycle gate, is compile-time off).
		// With none of them on, nobody can observe the sweep: skip the O(maxObjects) walk.
		static const bool s_destroyTraceOn = mc2::gate::get<"MC2_DESTROY_TRACE">();
		if (!objWalkOn && !s_destroyTraceOn && !s_inViewConflationEnabled && !s_gpuCullLifecycle) return;
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
}

// Parallel recalcBounds / touch pre-pass for the terrain objects.
void GameObjectManager::updateFrameJobsPrepass(int& frameJobsPrePassCount) {
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
			{ ZoneScopedN("FrameJobs.BuildList");
			for (int block = 0; block < Terrain::numObjBlocks; ++block) {
				if (!Terrain::objBlockInfo[block].active) continue;
				long h = Terrain::objBlockInfo[block].firstHandle;
				int  n = Terrain::objBlockInfo[block].numObjects;
				for (int i = 0; i < n; ++i, ++h) {
					GameObjectPtr obj = objList[h];
					if (!obj || !obj->getExists()) continue;
					if (!Terrain::objVertexActive[obj->getVertexNum()]) continue;
					// Shape/status changes belong on the main thread, before workers compute their bounds.
					if (auto* appearance = obj->getAppearance()) appearance->flushSimEvents();
					s_frameJobsHandles.push_back(h);
				}
			}
			}

			const int count = static_cast<int>(s_frameJobsHandles.size());
			frameJobsPrePassCount = count; // FRAME-JOBS-1: exported for per-frame trace

			// FRAME-JOBS-1: wall-clock timing + Tracy zone for the parallel recalcBounds pass.
			auto _fj_t0 = std::chrono::high_resolution_clock::now();
			{
				ZoneScopedN("FrameJobs.RecalcBounds");
				mc2::jobs::parallel_for(mc2::jobs::Domain::Render, count, frameJobsBatch(), 1000,
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
									mc2::fmt::print_out("FRAME_JOBS WARN: unsupported appearance type at handle {:d}\n", mc2::fmt::as_i32(s_frameJobsHandles[i]));
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
				mc2::fmt::print_out("FRAME_JOBS_PERF: batch={:d} workers={:d} handles={:d} chunks={:d} prepass_us={:.1f}\n", mc2::fmt::as_i32(frameJobsBatch()), mc2::fmt::as_i32(_fj_stats.workerCount), mc2::fmt::as_i32(count), mc2::fmt::as_i32(_fj_stats.chunksExecuted), _fj_prepass_us);
			}

			// FRAME-JOBS-1: emit to diagnostic JSONL trace (MC2_DIAG_TAGS=FRAME_JOBS or *)
			if (mc2_diag::tagEnabled("FRAME_JOBS")) {
				FrameJobsFrameStats _fj_stats = frameJobsGetFrameStats();
				char _fj_buf[192];
				mc2::fmt::to_buf(_fj_buf, "{{\"batch\":{:d},\"workers\":{:d},\"handles\":{:d},\"chunks\":{:d},\"prepass_us\":{:.1f}}}", mc2::fmt::as_i32(frameJobsBatch()), mc2::fmt::as_i32(_fj_stats.workerCount), mc2::fmt::as_i32(count), mc2::fmt::as_i32(_fj_stats.chunksExecuted), _fj_prepass_us);
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
			{ ZoneScopedN("FrameJobs.Touch.Filter");
			for (long h : s_frameJobsHandles) {
				++candidates;
				GameObjectPtr obj = objList[h];
				if (!obj) { ++skipped_not_split_safe; continue; }
				AppearancePtr ap = obj->getAppearance();
				if (!ap || !ap->isTouchSplitSafe()) { ++skipped_not_split_safe; continue; }
				s_splitHandles.push_back(h);
			}
			}
			int worker_submitted = static_cast<int>(s_splitHandles.size());

			// Phase 1: parallel lock-free prep ─────────────────────────────────────
			auto _tw0 = std::chrono::high_resolution_clock::now();
			int touchCount = worker_submitted;
			if (touchCount > 0) {
				mc2::jobs::parallel_for(mc2::jobs::Domain::Render, touchCount, frameJobsBatch(), 1000, [](int begin, int end) {
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
			{ ZoneScopedN("FrameJobs.Touch.Commit");
			for (long h : s_splitHandles) {
				GameObjectPtr obj = objList[h];
				if (!obj) continue;
				AppearancePtr ap = obj->getAppearance();
				if (ap) { ap->touchSerialCommit(); ++serial_commits; }
			}
			}
			auto _tc1 = std::chrono::high_resolution_clock::now();
			float commit_us = std::chrono::duration<float, std::micro>(_tc1 - _tc0).count();
			int worker_resubmit_calls = g_workerResubmitCalls.load(std::memory_order_relaxed) - prev_resubmit_calls;

			if (frameJobsTrace()) {
				mc2::fmt::print_out("FRAME_JOBS_TOUCH: enabled=1 split=yes candidates={:d} worker_submitted={:d} serial_commits={:d} worker_resubmit_calls={:d} skipped_not_split_safe={:d}\n", mc2::fmt::as_i32(candidates), mc2::fmt::as_i32(worker_submitted), mc2::fmt::as_i32(serial_commits), mc2::fmt::as_i32(worker_resubmit_calls), mc2::fmt::as_i32(skipped_not_split_safe));
				mc2::fmt::print_out("FRAME_JOBS_TOUCH_PERF: worker_us={:.1f} serial_commit_us={:.1f} avg_worker_us_per_object={:.3f} avg_serial_commit_us_per_object={:.3f}\n", worker_us, commit_us, worker_us / std::max(1, worker_submitted),
					commit_us / std::max(1, serial_commits));
			}
		}
		// ── end FRAME-JOBS-2D pre-pass ───────────────────────────────────────────────
	}
	// ── end FRAME-JOBS-1 pre-pass ────────────────────────────────────────────────
}

// Update special buildings, gates and the terrain object blocks, then report the counters.
void GameObjectManager::updateTerrainObjects(int& frameJobsPrePassCount) {
    auto advance = [](GameObjectPtr object) {
        if (!object || !object->getExists()) return;
        MC2_WORLD_UPDATE("terrain", object);
        const long result = object->update();
        object->lastUpdateRet = static_cast<int32_t>(result);
        if (!result) MC2_DESTROY(object, "update_false");
    };
    // These lists overlap the terrain blocks. Preserve their early update order
    // while advancing each object only once per tick.
    static std::vector<GameObjectPtr> advanced;   // per-tick allocation hoisted; cleared each call
    advanced.clear();
    advanced.reserve(numSpecialBuildings + numGates);
    auto once = [&](GameObjectPtr object) {
        if (std::find(advanced.begin(), advanced.end(), object) != advanced.end()) return;
        advanced.push_back(object);
        advance(object);
    };
    for (long i = 0; i < numSpecialBuildings; ++i) once(specialBuildings[i]);
    for (long i = 0; i < numGates; ++i) once(gates[i]);
    // SIM-TICK-CUT: walk only the handles that can tick, in the scan's handle order, instead of every block object.
    if (!terrainTickList.built() || terrainTickSpecial != numSpecialBuildings || terrainTickGates != numGates ||
        terrainTickBlocks != Terrain::numObjBlocks)
        rebuildTerrainTickList();
    const auto isStaticIdle = [](GameObjectPtr object) {
        const auto kind = object->getObjectClass();
        return (kind == TREE || kind == TERRAINOBJECT) &&
            !object->getFlag(OBJECT_FLAG_FALLING) && !object->getFlag(OBJECT_FLAG_JUSTCREATED);
    };
    bool pruneNeeded = false;
    terrainTickList.walk([&](const mc2::sim::TerrainTickList::Entry& entry) {
        auto* object = objList[entry.handle];
        if (!object || !object->getExists()) return;
        if (isStaticIdle(object)) { pruneNeeded = true; return; }
        if (entry.duplicate) return;
        advance(object);
    });
    if (pruneNeeded)
        terrainTickList.pruneConditional([&](int32_t handle) {
            auto* object = objList[handle];
            return object && !isStaticIdle(object);
        });
}

bool GameObjectManager::isSpecialOrGate(GameObjectPtr object) const {
    for (long i = 0; i < numSpecialBuildings; ++i) if (specialBuildings[i] == object) return true;
    for (long i = 0; i < numGates; ++i) if (gates[i] == object) return true;
    return false;
}

static void noteTerrainTickCandidate(GameObject* object);

void GameObjectManager::rebuildTerrainTickList() {
    g_noteTerrainTickCandidate = &noteTerrainTickCandidate;
    std::vector<std::pair<int32_t, int32_t>> blocks;
    blocks.reserve(static_cast<size_t>(std::max<long>(Terrain::numObjBlocks, 0)));
    for (long block = 0; block < Terrain::numObjBlocks; ++block) {
        const auto& info = Terrain::objBlockInfo[block];
        blocks.emplace_back(static_cast<int32_t>(info.firstHandle), static_cast<int32_t>(info.numObjects));
    }
    terrainTickList.rebuild(
        blocks, static_cast<int32_t>(getMaxObjects()),
        [&](int32_t handle) {
            auto* object = objList ? objList[handle] : nullptr;
            if (!object) return 0;
            const auto kind = object->getObjectClass();
            if (kind != TREE && kind != TERRAINOBJECT) return 1;
            return (object->getFlag(OBJECT_FLAG_FALLING) || object->getFlag(OBJECT_FLAG_JUSTCREATED)) ? 2 : 3;
        },
        [&](int32_t handle) { return isSpecialOrGate(objList[handle]); });
    terrainTickSpecial = numSpecialBuildings;
    terrainTickGates = numGates;
    terrainTickBlocks = Terrain::numObjBlocks;
}

static void noteTerrainTickCandidate(GameObject* object) {
    if (ObjectManager) ObjectManager->noteTerrainTickFlag(object);
}

void GameObjectManager::noteTerrainTickFlag(GameObjectPtr object) {
    if (!object || !terrainTickList.built()) return;
    const auto kind = object->getObjectClass();
    if (kind != TREE && kind != TERRAINOBJECT) return;
    terrainTickList.insert(static_cast<int32_t>(object->getHandle()), isSpecialOrGate(object));
}

void GameObjectManager::updateMoverWalk(MoverPtr (&removeList)[MAX_MOVERS], long& numRemoved) {
	invalidateSpatialGrid(false);
	if (mechs)
	{
		ZoneScopedN("GameLogic.Units.Mechs");
		// Moving mechs capture their simulation pose during the walk; the batch evaluates them across the Sim job domain and
		// publishes in walk order before anything after the walk reads a pose (see Mech3DAppearance::flushSimPoseBatch).
		Mech3DAppearance::beginSimPoseBatch();
		for (long i = 0; i < numMechs; i++)
		{
			MoverPtr mover = mechs[i];
			if (mover && mover->getExists())
			{
				MC2_WORLD_UPDATE("mechs", mover);
				long updateRet_instr = mover->update();
				invalidateSpatialGrid(false);
				mover->lastUpdateRet = static_cast<int32_t>(updateRet_instr);
				if (!updateRet_instr)
					MC2_DESTROY(mover, "update_false");
				if (mover->getFlag(OBJECT_FLAG_REMOVED))
					removeList[numRemoved++] = mover;
			}
		}
		Mech3DAppearance::flushSimPoseBatch();
	}

	if (vehicles)
	{
		ZoneScopedN("GameLogic.Units.Vehicles");
		for (long i = 0; i < maxVehicles; i++)
		{
			MoverPtr mover = vehicles[i];
			if (mover && mover->getExists())
			{
				MC2_WORLD_UPDATE("vehicles", mover);
				long updateRet_instr = mover->update();
				invalidateSpatialGrid(false);
				mover->lastUpdateRet = static_cast<int32_t>(updateRet_instr);
				if (!updateRet_instr)
					MC2_DESTROY(mover, "update_false");
				if (mover->getFlag(OBJECT_FLAG_REMOVED))
					removeList[numRemoved++] = mover;
			}
		}
	}
}

// Deferred brain-intent commit pass in warrior-ID order.
void GameObjectManager::updateBrainCommit() {
	// BRAIN-COMMIT-PHASE-1: deferred intent commit phase (MC2_BRAIN_COMMIT_PHASE=1).
	// Runs AFTER all mechs and vehicles have called update() (runBrain/updateActions).
	// Requires MC2_BRAIN_INTENT_QUEUE=1; gate OFF = no-op (inline-commit path unchanged).
	// Warriors are committed in ascending WID order for deterministic multi-warrior ordering.
	// Each warrior with pendingIntentCount > 0 gets commitBrainIntents() called once.
	// Emits: [BRAIN_COMMIT_PHASE] committed=<n> warriors in WID order
	{
		const bool s_commitPhaseGate = mc2::brainpolicy::techBrain();
		const bool s_intentQueueGate = mc2::brainpolicy::techBrain();
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
			for ([[maybe_unused]] auto& [watchId, entry] : s_commitOrder) {
				MoverPtr mover = entry;
				if (!mover->getExists()) continue;  // destroyed during this frame's mover loop
				MechWarriorPtr pilot = mover->getPilot();
				if (!pilot) continue;
				MechBrainRuntime* rt = pilot->getBrainRuntime();
				if (!rt || rt->pendingIntentCount <= 0) continue;
				commitBrainIntents(pilot, rt);
				++committed;
			}
			if (committed > 0) {
				mc2::fmt::print_err("[BRAIN_COMMIT_PHASE] committed={:d} warriors in WID order\n", mc2::fmt::as_i32(committed));
				std::fflush(stderr);
			}
		}
	}
}

// Update turrets, weapons, carnage, lights and artillery.
void GameObjectManager::updateOtherFx() {
	const bool combatTraceEnabled = mc2_diag::tagEnabled("VFX_COMBAT");
	//---------------------------------------
	// All other objects should be updated...

	if (turrets)
	{
		ZoneScopedN("GameLogic.Units.Turrets");
		for (long i=0;i<numTurrets;i++)
		{
			if (turrets[i] && turrets[i]->getExists())
			{
				MC2_WORLD_UPDATE("turrets", turrets[i]);
				long updateRet_instr = turrets[i]->update();
				turrets[i]->lastUpdateRet = static_cast<int32_t>(updateRet_instr);
				if (!updateRet_instr)
					MC2_DESTROY(turrets[i], "update_false");
			}
		}
	}

	if (weapons) {
		for (long i=0;i<numWeapons;i++) {
			if (weapons[i] && weapons[i]->getExists()) {
				MC2_WORLD_UPDATE("weapons", weapons[i]);
				long updateRet_instr = weapons[i]->update();
				weapons[i]->lastUpdateRet = static_cast<int32_t>(updateRet_instr);
				if (combatTraceEnabled) {
					weapons[i]->traceCombat("position", "post_update");
					if (!updateRet_instr)
						weapons[i]->traceCombatRetire("update_false");
				}
				if (!updateRet_instr) {
					MC2_DESTROY(weapons[i], "update_false");
				}
			}
		}
	}

	if (carnage) {
		for (long i = 0; i < numCarnage; i++) {
			if (carnage[i] && carnage[i]->getExists()) {
				MC2_WORLD_UPDATE("carnage", carnage[i]);
				long updateRet_instr = carnage[i]->update();
				carnage[i]->lastUpdateRet = static_cast<int32_t>(updateRet_instr);
				if (combatTraceEnabled) {
					carnage[i]->traceCombat("position", "post_update");
					if (!updateRet_instr)
						carnage[i]->traceCombatRetire("update_false");
				}
				if (!updateRet_instr) {
					MC2_DESTROY(carnage[i], "update_false");
				}
			}
		}
	}

	if (lights) {
		for (long i = 0; i < numLights; i++) {
			if (lights[i] && lights[i]->getExists()) {
				MC2_WORLD_UPDATE("lights", lights[i]);
				long updateRet_instr = lights[i]->update();
				lights[i]->lastUpdateRet = static_cast<int32_t>(updateRet_instr);
				if (!updateRet_instr)
					MC2_DESTROY(lights[i], "update_false");
			}
		}
	}

	if (artillery) {
		for (long i = 0; i < numArtillery; i++) {
			if (artillery[i] && artillery[i]->getExists()) {
				MC2_WORLD_UPDATE("artillery", artillery[i]);
				long updateRet_instr = artillery[i]->update();
				artillery[i]->lastUpdateRet = static_cast<int32_t>(updateRet_instr);
				if (!updateRet_instr)
					MC2_DESTROY(artillery[i], "update_false");
			}
		}
	}
}

// One simulation tick of every owned object. The RAII cost spans and Tracy zones stay here so each still
// brackets the same phases; each phase body is a member helper declared in objmgr.h, in execution order.
void GameObjectManager::update (bool terrain, bool movers, bool other)
{
	if (mc2::world_trace::enabled()) mc2::world_trace::snapshot(*this, "gom_begin");
	ZoneScopedN("GameObjectManager::update");
	gomcost::TotalSpan _gomTotalSpan;  // [GOM_UPDATE_COST v1] first-constructed => dtor last

	updateDestroyTestHook();

	//----------------------------
	// Now, update game objects...



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

	if (terrain)
	{
		int frameJobsPrePassCount = 0; // FRAME-JOBS-1: handle count for per-frame trace

		ZoneScopedN("GameLogic.Units.TerrainObjects");
		gomcost::Span _gomStaticTouch(gomcost::Phase::kStaticTouch); // [GOM_UPDATE_COST v1]
		updateTerrainObjects(frameJobsPrePassCount);
	}
	
 	if (movers) {
		static MoverPtr removeList[MAX_MOVERS];
		long numRemoved = 0;

		{ gomcost::Span _gomMoverUpdate(gomcost::Phase::kMoverUpdate); // [GOM_UPDATE_COST v1] mechs+vehicles update() walk
		updateMoverWalk(removeList, numRemoved);

		for (long i = 0; i < numRemoved; i++)
			mission->removeMover(removeList[i]);
		} // [GOM_UPDATE_COST v1] end mover_update span

		updateBrainCommit();
	}

	if (other) {
		gomcost::Span _gomOtherFx(gomcost::Phase::kOtherFx); // [GOM_UPDATE_COST v1] turrets/weapons/carnage/lights/artillery
		updateOtherFx();
	}


	// MC2_MOVE_RECON: per-frame tick (lazy init, atexit register, periodic emit).
	moveReconFrameTick();
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::get (GameObjectHandle handle) {

	if ((handle < 1) || (handle > getMaxObjects()))
		return(nullptr);

	return entities.get(handle);
}

void GameObjectManager::updatePresentationCull() {
    for (long i = 0; i < numMechs; ++i)
        if (mechs[i] && mechs[i]->getExists())
            emitGpuCullRecord(mechs[i], gpu_cull::Cat_Mech, std::to_underlying(gpu_cull::Consumer_RenderGate));
    for (long i = 0; i < numVehicles; ++i)
        if (vehicles[i] && vehicles[i]->getExists())
            emitGpuCullRecord(vehicles[i], gpu_cull::Cat_GroundVeh, std::to_underlying(gpu_cull::Consumer_RenderGate));
    for (long i = 0; i < numTurrets; ++i)
        if (turrets[i] && turrets[i]->getExists())
            emitGpuCullRecord(turrets[i], gpu_cull::Cat_Turret, std::to_underlying(gpu_cull::Consumer_RenderGate));
    gpu_cull::substrate_flushUpload();
    if (gpu_cull::compute_isEnabled()) gpu_cull::compute_emitParitySummary();
}

// static-world-s2: called from a render visit; the object becomes resident only if presentation has settled it too.
void GameObjectManager::noteRenderResident (GameObjectPtr obj, int32_t recipe)
{
	if (!obj || recipe < 0) return;
	const long handle = obj->getHandle();
	if (!mc2::terrainDirtyQueue().setRenderResident(handle)) return;
	if (GpuStaticPropRegistry::setResident(recipe)) staticResident.add(handle, recipe);
}

// G7b K3: the render extract. Same membership and order as the draw loops in GameObjectManager::render, read while
// the sim is quiescent; each object copies what its renderView draws. Pointers stay valid until the next commit
// (deferred frees), which is after the frame's draw.
void GameObjectManager::extractRenderList (mc2::render_view::RenderList& list)
{
	auto add = [](std::vector<mc2::render_view::RenderView>& out, GameObjectPtr obj) {
		mc2::render_view::RenderView& v = out.emplace_back();
		v.obj = obj;
		v.generation = obj->generation;
		obj->extractRenderView(v);
	};

	if (renderObjects)
	{
		const mc2::battlebench::ScopeNs walkNs{mc2::battlebench::WalkExtractNs};   // D2 census
		// Residents (static-world-s2) are skipped: their last render visit settled in the registry. A height or
		// light-environment change drops them all; a mark, a destroy or a cull change drops the one it concerns.
		extern uint64_t mc2StaticLightEnvironmentGeneration(const Camera* eye);
		extern uint32_t mc2LightSlotBakedHighWater();
		const uint64_t residencyKey = (static_cast<uint64_t>(TerrainRuntime::g_heightEpoch) * 1000003ull) ^
			(mc2StaticLightEnvironmentGeneration(eye) * 998244353ull) ^ (static_cast<uint64_t>(mc2LightSlotBakedHighWater()) << 40);
		staticResident.validate(residencyKey, [](long, int32_t) { return true; },
			[](long, int32_t recipe) { GpuStaticPropRegistry::clearResident(recipe); });
		auto& dirty = mc2::terrainDirtyQueue();
		dirty.beginQuiescentRead(mc2::overlap::jobInFlight());
		long residentSkipped = 0, offViewSkipped = 0;
		long offViewByClass[64] = {};   // diag: the skipped views by object class
		if (blockWasActive.size() != static_cast<size_t>(Terrain::numObjBlocks)) blockWasActive.assign(Terrain::numObjBlocks, 0);
		for (long terrainBlock = 0; terrainBlock < Terrain::numObjBlocks; terrainBlock++)
		{
			long numObjs = Terrain::objBlockInfo[terrainBlock].numObjects;
			long objIndex = Terrain::objBlockInfo[terrainBlock].firstHandle;
			if (!Terrain::objBlockInfo[terrainBlock].active)
			{
				if (blockWasActive[terrainBlock] && staticResident.size())   // left the view: its residents stop drawing
					for (long terrainObj = 0; terrainObj < numObjs; terrainObj++)
						if (const int32_t recipe = staticResident.recipe(objIndex + terrainObj); recipe >= 0)
						{
							GpuStaticPropRegistry::clearResident(recipe);
							staticResident.drop(objIndex + terrainObj);
						}
				blockWasActive[terrainBlock] = 0;
				continue;
			}
			blockWasActive[terrainBlock] = 1;
			for (long terrainObj = 0; terrainObj < numObjs; terrainObj++, objIndex++)
			{
					// One load of the slot: the virtual calls stop the compiler reusing objList[objIndex].
					GameObjectPtr obj = objList[objIndex];
					const bool admitted = obj && obj->getExists() && Terrain::objVertexActive[obj->getVertexNum()];
					if (const int32_t recipe = staticResident.recipe(objIndex); recipe >= 0)
					{
						if (admitted && dirty.isRenderResidentQuiescent(objIndex) && GpuStaticPropRegistry::isResident(recipe))
						{
							++residentSkipped;
							continue;
						}
						GpuStaticPropRegistry::clearResident(recipe);
						staticResident.drop(objIndex);
					}
					if (admitted)
					{
						add(list.terrain, obj);
						// perf-terrain-walk: the extract (selection reset, bar/dust capture) still ran; a tree or terrain
						// object outside the view with no bars or dust would render nothing, so the walk skips it.
						// perf-building-resident: so does a building, turret or gate whose extract left Visible clear.
						const mc2::render_view::RenderView& v = list.terrain.back();
						if (mc2::visibleFlagWalkSkips(v.objClass, v.has(mc2::render_view::Visible)) ||
							mc2::terrainWalkSkips(v.objClass, v.has(mc2::render_view::Bars), v.effects[0] != nullptr,
								obj->getAppearance() && obj->getAppearance()->renderIsNoOp()))
						{
							if (v.objClass < std::size(offViewByClass)) ++offViewByClass[v.objClass];
							list.terrain.pop_back();
							++offViewSkipped;
						}
					}
			}
		}
		{
			static long s_calls = 0;
			if (++s_calls % 600 == 0 && mc2_diag::tagRequested("STATIC_RESIDENT"))
			{
				// The views the render walk still visits this frame (the non-resident remainder of RenderObjects.Terrain), by object class.
				long byClass[256] = {};
				for (const mc2::render_view::RenderView& v : list.terrain) ++byClass[v.objClass];
				char hist[512]; size_t n = 0; hist[0] = 0;
				for (int c = 0; c < 256 && n + 24 < sizeof(hist); ++c)
					if (byClass[c]) n += static_cast<size_t>(std::snprintf(hist + n, sizeof(hist) - n, "%s%d:%ld", n ? "," : "", c, byClass[c]));
				char skipHist[256]; size_t m = 0; skipHist[0] = 0;
				for (int c = 0; c < static_cast<int>(std::size(offViewByClass)) && m + 24 < sizeof(skipHist); ++c)
					if (offViewByClass[c]) m += static_cast<size_t>(std::snprintf(skipHist + m, sizeof(skipHist) - m, "%s%d:%ld", m ? "," : "", c, offViewByClass[c]));
				mc2::fmt::print_err("[STATIC_RESIDENT v1] extracts={:d} resident={:d} skipped_last={:d} registry_resident={:d} views={:d} offview_skipped={:d} by_class={} offview_by_class={}\n", mc2::fmt::as_i64(s_calls), mc2::fmt::as_i64(static_cast<long>(staticResident.size())), mc2::fmt::as_i64(residentSkipped), mc2::fmt::as_i64(static_cast<long>(GpuStaticPropRegistry::residentCount())), mc2::fmt::as_i64(static_cast<long>(list.terrain.size())), mc2::fmt::as_i64(offViewSkipped), mc2::fmt::safe(hist), mc2::fmt::safe(skipHist));
			}
		}
	}

	if (mechs && (numMechs < maxMechs))
		for (long i = 0; i < numMechs; i++)
			if (mechs[i] && mechs[i]->getExists())
				add(list.movers, mechs[i]);
	if (vehicles)
		for (long i = 0; i < maxVehicles; i++)
			if (vehicles[i] && vehicles[i]->getExists())
				add(list.movers, vehicles[i]);
#ifdef USE_ELEMENTALS
	if (elementals)
		for (long i = 0; i < numElementals; i++)
			if (elementals[i] && elementals[i]->getExists())
				add(list.movers, elementals[i]);
#endif

	if (weapons)
		for (long i = 0; i < numWeapons; i++)
			if (weapons[i] && weapons[i]->hasPresentation())
				weapons[i]->extractFxPacket(list);
	if (carnage)
		for (long i = 0; i < numCarnage; i++)
			if (carnage[i] && carnage[i]->gosEffect)
				carnage[i]->extractFxPacket(list);
	if (lights)
		for (long i = 0; i < numLights; i++)
			if (lights[i] && lights[i]->getExists())
				add(list.other, lights[i]);
	if (artillery)
		for (long i = 0; i < numArtillery; i++)
			if (artillery[i] && (artillery[i]->getExists() || artillery[i]->hasPresentation()))
				add(list.other, artillery[i]);
}
