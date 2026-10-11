// GameObjectManager: GameObjectManager::update with its GOM_UPDATE_COST spans, in-view conflation probe, GPU-cull record and static-skip gates.
// Declared in the class headers; shared names in objmgr_internal.h.
#include "objmgr_internal.h"
#include "mc2fmt.h"


// ---------------------------------------------------------------------------
// C3: env-gated lifecycle routing killswitch.
// MC2_GPU_CULL_LIFECYCLE=1 enables GPU visibility-based update() gating.
// Default off — legacy inView/canBeSeen/blockActive path unchanged.
// ---------------------------------------------------------------------------
static const bool s_gpuCullLifecycle = mc2::gate::get<"MC2_GPU_CULL_LIFECYCLE">();

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
		cached = mc2::gate::get<"MC2_SKIP_STATIC_TREES">();
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
		cached = mc2::gate::get<"MC2_SKIP_STATIC_BUILDINGS">();
	}
	return(cached != 0);
}

static bool mc2SkipStaticNaturalDiagEnabled (void)
{
	static int cached = -1;
	if (cached < 0)
		cached = mc2::gate::get<"MC2_SKIP_STATIC_TREES_DIAG">() ? 1 : 0;
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
		cached = mc2::gate::get<"MC2_R2B_TOUCH_PRESERVE">();  // default-ON
	}
	return(cached != 0);
}

static bool mc2R2bTouchTraceEnabled (void)
{
	static int cached = -1;
	if (cached < 0)
		cached = mc2::gate::get<"MC2_R2B_STATIC_NATURAL_TRACE">() ? 1 : 0;
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
static bool s_inViewConflationEnabled = mc2::gate::get<"MC2_INVIEW_CONFLATION_TRACE">();
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
static const bool s_enabled = mc2::gate::get<"MC2_GOM_UPDATE_COST_SPLIT">();
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
        off += mc2::fmt::to_buf_n(line + off, sizeof(line) - off, "[GOM_UPDATE_COST v1] event=summary frames={:d}", mc2::fmt::as_i32(s_frames));
        for (int p = 0; p < kPhaseCount; ++p) {
            const double us = static_cast<double>(s_ns[p]) / 1000.0 / wf;
            if (p != kTotal) phaseSumUs += us;
            if (off < static_cast<int>(sizeof(line)))
                off += mc2::fmt::to_buf_n(line + off, sizeof(line) - off, " {}_us={:.1f}", mc2::fmt::safe(kName[p]), us);
        }
        const double selfUs =
            static_cast<double>(s_ns[kTotal]) / 1000.0 / wf - phaseSumUs;
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
		if (s_destroyTestFrame >= 0 && (long)g_mc2FrameCounter == s_destroyTestFrame) {
			for (long bi = 0; bi < numBuildings; ++bi) {
				BuildingPtr b = buildings[bi];
				if (!b || b->isDestroyed() || !b->getExists()) continue;
				fprintf(stderr,
					"[SHADOW_STATIC_BLDG_REBAKE_TEST v1] event=test_destroy frame=%u buildingIndex=%ld\n",
					g_mc2FrameCounter, bi);
				fflush(stderr);
				b->setDamage(b->getDamageLevel());
				break;
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
		// Max handle is bounded by maxObjects + slack; 4096 is safe for MC2 (~2000 max).
		gpu_cull::readback_buildActorVisSnapshot(4096u);
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
				mc2::fmt::print_out("FRAME_JOBS_TOUCH: enabled=1 split=yes candidates={:d} worker_submitted={:d} serial_commits={:d} worker_resubmit_calls={:d} skipped_not_split_safe={:d}\n", mc2::fmt::as_i32(candidates), mc2::fmt::as_i32(worker_submitted), mc2::fmt::as_i32(serial_commits), mc2::fmt::as_i32(worker_resubmit_calls), mc2::fmt::as_i32(skipped_not_split_safe));
				mc2::fmt::print_out("FRAME_JOBS_TOUCH_PERF: worker_us={:.1f} serial_commit_us={:.1f} avg_worker_us_per_object={:.3f} avg_serial_commit_us_per_object={:.3f}\n", worker_us, commit_us, worker_us / std::max(1, worker_submitted), commit_us / std::max(1, serial_commits));
			}
		}
		// ── end FRAME-JOBS-2D pre-pass ───────────────────────────────────────────────
	}
	// ── end FRAME-JOBS-1 pre-pass ────────────────────────────────────────────────
}

// Update special buildings, gates and the terrain object blocks, then report the counters.
void GameObjectManager::updateTerrainObjects(int& frameJobsPrePassCount) {
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
							    mc2R2bTouchStaticLiveness(appearance, g_mc2FrameCounter))
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
							    mc2R2bTouchStaticLiveness(appearance, g_mc2FrameCounter))
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
		mc2::fmt::print_err("R2B_STATIC_NATURAL: frame={:d} skipped_full_update={:d} touched_liveness={:d} preserve={:d}\n", mc2::fmt::as_u32(g_mc2FrameCounter), mc2::fmt::as_i32(skippedStaticNatural), mc2::fmt::as_i32(touchedLiveness), mc2::fmt::as_i32(r2bTouchPreserve ? 1 : 0));
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
		mc2::fmt::print_out("FRAME_JOBS: workers={:d} chunks={:d} handles={:d} serial={}\n", mc2::fmt::as_i32(stats.workerCount), mc2::fmt::as_i32(stats.chunksExecuted), mc2::fmt::as_i32(frameJobsPrePassCount), mc2::fmt::safe(stats.serialFallback ? "yes" : "no"));
	}
}

// Update every mech and vehicle and queue removed movers.
void GameObjectManager::updateMoverWalk(MoverPtr (&removeList)[MAX_MOVERS], long& numRemoved) {
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
		static const bool s_commitPhaseGate = mc2::gate::get<"MC2_BRAIN_COMMIT_PHASE">();
		static const bool s_intentQueueGate = mc2::gate::get<"MC2_BRAIN_INTENT_QUEUE">();
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

// Update turrets, weapons, carnage, lights and artillery.
void GameObjectManager::updateOtherFx() {
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

// One simulation tick of every owned object. The RAII cost spans and Tracy zones stay here so each still
// brackets the same phases; each phase body is a member helper declared in objmgr.h, in execution order.
void GameObjectManager::update (bool terrain, bool movers, bool other)
{
	ZoneScopedN("GameObjectManager::update");
	gomcost::TotalSpan _gomTotalSpan;  // [GOM_UPDATE_COST v1] first-constructed => dtor last

	updateDestroyTestHook();

	//----------------------------
	// Now, update game objects...

	updateReadbackSnapshot();
	updateActiveSweep();

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
		updateFrameJobsPrepass(frameJobsPrePassCount);
		ZoneScopedN("GameLogic.Units.TerrainObjects");
		gomcost::Span _gomStaticTouch(gomcost::kStaticTouch); // [GOM_UPDATE_COST v1]
		updateTerrainObjects(frameJobsPrePassCount);
	}
	
 	if (movers) {
		static MoverPtr removeList[MAX_MOVERS];
		long numRemoved = 0;

		{ gomcost::Span _gomMoverUpdate(gomcost::kMoverUpdate); // [GOM_UPDATE_COST v1] mechs+vehicles update() walk
		updateMoverWalk(removeList, numRemoved);

		for (long i = 0; i < numRemoved; i++)
			mission->removeMover(removeList[i]);
		} // [GOM_UPDATE_COST v1] end mover_update span

		updateBrainCommit();
	}

	if (other) {
		gomcost::Span _gomOtherFx(gomcost::kOtherFx); // [GOM_UPDATE_COST v1] turrets/weapons/carnage/lights/artillery
		updateOtherFx();
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
		return(nullptr);

	return(objList[handle]);
}
