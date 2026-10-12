// BldgAppearance / TreeAppearance: stable-light-skip, lightbridge and touch-entry diagnostics, light-slot cardinality trace, prop light-latch diag.
// Declared in the class headers; shared names in bdactor_internal.h.
#include "bdactor_internal.h"
#include "mc2fmt.h"

// [LIGHTBAKE v1] Static-actor lighting mission-load bake gate. Replaces
// the raw shape->CacheGpuLightData() at the 4 static (bldg/tree) call
// sites. The trailing staticReg.lightDataIndex =
// shape->getCachedGpuLightIndex() per-instance capture is UNCHANGED
// (both CacheGpuLightData and EmitBakedGpuLightData set
// cachedGpuLightIndex_). Key = monotonic-never-reused registry
// recipeIndex; invalidate (destruction/LOD swap) routes through
// invalidateStaticRegistration -> GpuStaticPropRegistry::invalidate ->
// mc2EraseBakedStaticLight -> lazy re-bake of the same position-derived
// constant. Kill-switch MC2_LIGHTBAKE (=0 -> unchanged D2 path
// bit-for-bit). Mechs never reach this (mech3d.cpp calls
// CacheGpuLightData directly); generic props take the no-actor-light
// path. C++-only. See docs/superpowers/plans/
// 2026-05-17-static-lighting-bake-SIMPLIFIED.md
// [LIGHTSLOT v1] Task 0 — light-slot cardinality PROOF GATE (diagnostic only,
// env-gated MC2_LIGHTSLOT_TRACE, ZERO behavior change). Proves whether the
// unique static-prop light slots consumed by an override forest are bounded by
// ~(types x LODs) after addLightDataStructure content dedup + the persistent
// baked table, or scale with INSTANCES (K). Pure counting; emits ONE line per
// map at mission_ready (NOT per-frame). All txmmgr numbers read via accessor
// free fns (defined mclib/txmmgr.cpp).
extern uint32_t g_mc2FrameCounter;  // mclib/tgl.cpp
bool mc2StableLightSkipEnabled() {
    static const bool s_enabled = mc2::gate::get<"MC2_STABLE_LIGHT_SKIP">();
    return s_enabled;
}

static bool mc2StableLightSkipDiagEnabled() {
    static const bool s_enabled = mc2::gate::get<"MC2_STABLE_LIGHT_SKIP_DIAG">();
    return s_enabled;
}

uint64_t mc2StaticLightEnvironmentGeneration(const Camera* eye) {
    (void)eye;
    // L1b: intentionally coarse. Global mission lighting is effectively stable.
    // This stays default-off and is only used for bounded diagnostics here.
    return 0;
}

namespace {
struct StableLightSkipDiag {
    bool enabled = false;
    bool envPrinted = false;
    bool initialized = false;
    uint32_t frame = 0xFFFFFFFFu;
    uint64_t armed = 0;
    uint64_t eligible = 0;
    uint64_t skip = 0;
    uint64_t emit = 0;
    uint64_t miss_notRegistered = 0;
    uint64_t miss_noValidLight = 0;
    uint64_t miss_envGen = 0;
    uint64_t miss_needsFullBake = 0;
    uint64_t miss_invalidLightIdx = 0;
};

static StableLightSkipDiag s_stableLightSkipDiag;

static void flushStableLightSkipDiag(uint32_t completedFrame)
{
    if (!s_stableLightSkipDiag.enabled)
        return;
    if (!(completedFrame < 5 || (completedFrame % 60u) == 0u))
        return;

    mc2::fmt::print_err("[STABLE_LIGHT_SKIP] armed={:d} eligible={:d} skip={:d} emit={:d} miss_notRegistered={:d} miss_noValidLight={:d} miss_envGen={:d} miss_needsFullBake={:d} miss_invalidLightIdx={:d}\n", mc2::fmt::as_u64((unsigned long long)s_stableLightSkipDiag.armed), mc2::fmt::as_u64((unsigned long long)s_stableLightSkipDiag.eligible), mc2::fmt::as_u64((unsigned long long)s_stableLightSkipDiag.skip), mc2::fmt::as_u64((unsigned long long)s_stableLightSkipDiag.emit), mc2::fmt::as_u64((unsigned long long)s_stableLightSkipDiag.miss_notRegistered), mc2::fmt::as_u64((unsigned long long)s_stableLightSkipDiag.miss_noValidLight), mc2::fmt::as_u64((unsigned long long)s_stableLightSkipDiag.miss_envGen), mc2::fmt::as_u64((unsigned long long)s_stableLightSkipDiag.miss_needsFullBake), mc2::fmt::as_u64((unsigned long long)s_stableLightSkipDiag.miss_invalidLightIdx));
    std::fflush(stderr);
}

static void noteStableLightSkipFrame(uint32_t frame)
{
    if (!s_stableLightSkipDiag.enabled)
        return;

    if (!s_stableLightSkipDiag.initialized) {
        s_stableLightSkipDiag.initialized = true;
        s_stableLightSkipDiag.frame = frame;
        if (!s_stableLightSkipDiag.envPrinted) {
            const char* v = mc2::gate::isNonEmpty<"MC2_STABLE_LIGHT_SKIP">()
                ? (mc2::gate::get<"MC2_STABLE_LIGHT_SKIP">() ? "1" : "0") : nullptr;
            mc2::fmt::print_err("[STABLE_LIGHT_SKIP] MC2_STABLE_LIGHT_SKIP={}\n", mc2::fmt::safe(v ? v : "(unset)"));
            std::fflush(stderr);
            s_stableLightSkipDiag.envPrinted = true;
        }
        return;
    }

    if (frame == s_stableLightSkipDiag.frame)
        return;

    flushStableLightSkipDiag(s_stableLightSkipDiag.frame);
    s_stableLightSkipDiag.frame = frame;
    s_stableLightSkipDiag.armed = 0;
    s_stableLightSkipDiag.eligible = 0;
    s_stableLightSkipDiag.skip = 0;
    s_stableLightSkipDiag.emit = 0;
    s_stableLightSkipDiag.miss_notRegistered = 0;
    s_stableLightSkipDiag.miss_noValidLight = 0;
    s_stableLightSkipDiag.miss_envGen = 0;
    s_stableLightSkipDiag.miss_needsFullBake = 0;
    s_stableLightSkipDiag.miss_invalidLightIdx = 0;
}

} // namespace

void recordStableLightSkipDiag(bool armed, bool eligible,
    bool notRegistered, bool noValidLight, bool envGenMiss, bool needsFullBake,
    bool invalidLightIdx)
{
    if (!mc2StableLightSkipDiagEnabled())
        return;

    s_stableLightSkipDiag.enabled = true;
    noteStableLightSkipFrame(g_mc2FrameCounter);

    if (armed)
        ++s_stableLightSkipDiag.armed;
    if (eligible)
        ++s_stableLightSkipDiag.eligible;

    if (armed && eligible) {
        ++s_stableLightSkipDiag.skip;
    } else {
        ++s_stableLightSkipDiag.emit;
        if (armed && notRegistered)
            ++s_stableLightSkipDiag.miss_notRegistered;
        if (armed && noValidLight)
            ++s_stableLightSkipDiag.miss_noValidLight;
        if (armed && envGenMiss)
            ++s_stableLightSkipDiag.miss_envGen;
        if (armed && needsFullBake)
            ++s_stableLightSkipDiag.miss_needsFullBake;
        if (armed && invalidLightIdx)
            ++s_stableLightSkipDiag.miss_invalidLightIdx;
    }
}

// [LIGHTBRIDGE-STABLE-SKIP-WIRE-1] Gate + diagnostics
// MC2_LIGHTBRIDGE_STABLE_SKIP=1  — enable the skip path (default OFF)
// MC2_LIGHTBRIDGE_STABLE_SKIP_DIAG=1 — enable per-frame diagnostic print (independent)
extern uint32_t mc2StaticLightHighWater();  // txmmgr.cpp — s_staticLightHighWater accessor

bool mc2LightbridgeStableSkipEnabled() {
    static const bool s_enabled = mc2::gate::get<"MC2_LIGHTBRIDGE_STABLE_SKIP">();
    return s_enabled;
}
static bool mc2LightbridgeStableSkipDiagEnabled() {
    static const bool s_diagEnabled = mc2::gate::get<"MC2_LIGHTBRIDGE_STABLE_SKIP_DIAG">();
    return s_diagEnabled;
}
// STABLE-LIGHT-SKIP-BROADEN-1: extend the LBSS resubmit-skip to the legacy
// touch() Path-B variants (BldgAppearance::touch / TreeAppearance::touch).
// touchSerialCommit() already skips the redundant ResubmitCachedGpuLightData()
// when stableLightSkipEligible, but that path only runs when the FRAME-JOBS
// touch split is armed (MC2_FRAME_JOBS + MC2_FRAME_JOBS_TOUCH, BOTH default-OFF).
// In stock config the live path is touch(), which computed stableLightSkipEligible
// for diagnostics but resubmitted UNCONDITIONALLY — paying a per-static-prop GPU
// light resubmit every frame for provably-stable props. This gate broadens the
// skip to that path. Separate killswitch so it can be flipped independently of
// the serial-commit skip (which requires MC2_FRAME_JOBS to even run). Default OFF
// => byte-identical (the resubmit still runs). Requires MC2_LIGHTBRIDGE_STABLE_SKIP
// + MC2_STABLE_LIGHT_SKIP armed (same invariant proof: baked static prefix
// [0..s_staticLightHighWater) persists frame-to-frame under MC2_LIGHTBAKE).
bool mc2StableLightSkipTouchEnabled() {
    static const bool s_enabled = mc2::gate::get<"MC2_STABLE_LIGHT_SKIP_TOUCH">();
    return s_enabled;
}

namespace {

// Per-frame counters for LIGHTBRIDGE_STABLE_SKIP diagnostics.
// Printed every 300 frames when MC2_LIGHTBRIDGE_STABLE_SKIP_DIAG=1.
struct LightbridgeStableSkipDiag {
    uint32_t frame         = 0xFFFFFFFFu;
    uint64_t b_armed       = 0;  // stableLightSkipArmed
    uint64_t b_eligible    = 0;  // stableLightSkipEligible
    uint64_t b_taken       = 0;  // skip actually fired
    uint64_t b_blk_idx     = 0;  // blocked: invalid lightDataIndex
    uint64_t b_blk_gen     = 0;  // blocked: generation mismatch
    uint64_t b_blk_noelig  = 0;  // blocked: armed but not eligible
    uint64_t t_taken       = 0;
    uint64_t t_blk_idx     = 0;
    uint64_t t_total       = 0;
};

static LightbridgeStableSkipDiag s_lbssDiag;

static void lbssDiagPrint(const LightbridgeStableSkipDiag& d)
{
    mc2::fmt::print_err("LIGHTBRIDGE_STABLE_SKIP: enabled={:d} armed={:d} eligible={:d} taken={:d} blocked_invalid_index={:d} blocked_generation_mismatch={:d} blocked_no_eligible={:d} static_prefix_count={:d}\n", mc2::fmt::as_i32((int)mc2LightbridgeStableSkipEnabled()), mc2::fmt::as_u64(// references file-scope fn, not anon-ns
        (unsigned long long)d.b_armed), mc2::fmt::as_u64((unsigned long long)d.b_eligible), mc2::fmt::as_u64((unsigned long long)d.b_taken), mc2::fmt::as_u64((unsigned long long)d.b_blk_idx), mc2::fmt::as_u64((unsigned long long)d.b_blk_gen), mc2::fmt::as_u64((unsigned long long)d.b_blk_noelig), mc2::fmt::as_u32(mc2StaticLightHighWater()));
    mc2::fmt::print_err("LIGHTBRIDGE_TREE_STABLE_SKIP: total={:d} taken={:d} blocked_invalid_index={:d}\n", mc2::fmt::as_u64((unsigned long long)d.t_total), mc2::fmt::as_u64((unsigned long long)d.t_taken), mc2::fmt::as_u64((unsigned long long)d.t_blk_idx));
    fflush(stderr);
}

static void lbssDiagRollFrame(uint32_t frame)
{
    if (!mc2LightbridgeStableSkipDiagEnabled()) return;
    if (frame == s_lbssDiag.frame) return;
    if (s_lbssDiag.frame != 0xFFFFFFFFu && (s_lbssDiag.frame % 300u) == 0u)
        lbssDiagPrint(s_lbssDiag);
    s_lbssDiag = LightbridgeStableSkipDiag{};
    s_lbssDiag.frame = frame;
}

} // namespace (lbss diag)

void lbssRecordBldg(bool eligible, bool taken,
    bool blk_idx, bool blk_gen, bool blk_noelig)
{
    if (!mc2LightbridgeStableSkipDiagEnabled()) return;
    lbssDiagRollFrame(g_mc2FrameCounter);
    ++s_lbssDiag.b_armed;   // always called when armed (caller gate)
    if (eligible) ++s_lbssDiag.b_eligible;
    if (taken)    ++s_lbssDiag.b_taken;
    if (blk_idx)  ++s_lbssDiag.b_blk_idx;
    if (blk_gen)  ++s_lbssDiag.b_blk_gen;
    if (blk_noelig) ++s_lbssDiag.b_blk_noelig;
}

void lbssRecordTree(bool taken, bool blk_idx)
{
    if (!mc2LightbridgeStableSkipDiagEnabled()) return;
    lbssDiagRollFrame(g_mc2FrameCounter);
    ++s_lbssDiag.t_total;
    if (taken)   ++s_lbssDiag.t_taken;
    if (blk_idx) ++s_lbssDiag.t_blk_idx;
}

// FRAME-JOBS-2F: TOUCH-ENTRY-UNIFICATION diagnostics
// MC2_FRAME_JOBS_TOUCH_DIAG=1 — enable per-300-frame print (independent of lbss diag)
// Counters are cumulative per-frame accumulators, reset every 300 frames.
bool mc2FrameJobsTouchDiagEnabled() {
    static const bool s_enabled = mc2::gate::get<"MC2_FRAME_JOBS_TOUCH_DIAG">();
    return s_enabled;
}

TouchEntryDiag s_touchEntryDiag;

void touchEntryDiagRollFrame(uint32_t frame)
{
    if (!mc2FrameJobsTouchDiagEnabled()) return;
    if (frame == s_touchEntryDiag.frame) return;
    if (s_touchEntryDiag.frame != 0xFFFFFFFFu && (s_touchEntryDiag.frame % 300u) == 0u) {
        mc2::fmt::print_err("FRAME_JOBS_TOUCH_ENTRY: serial_commit_hits={:d} legacy_skipped={:d} double_touch_prevented={:d} legacy_ran_nosplit={:d}\n", mc2::fmt::as_u64((unsigned long long)s_touchEntryDiag.serial_commit_hits), mc2::fmt::as_u64((unsigned long long)s_touchEntryDiag.legacy_skipped), mc2::fmt::as_u64((unsigned long long)s_touchEntryDiag.legacy_skipped), mc2::fmt::as_u64(// same as legacy_skipped
            (unsigned long long)s_touchEntryDiag.legacy_ran_nosplit));
        fflush(stderr);
    }
    s_touchEntryDiag = TouchEntryDiag{};
    s_touchEntryDiag.frame = frame;
}

// [LIGHTSLOT v1] accessor free fns (defined mclib/txmmgr.cpp, global scope).
extern uint32_t mc2LightSlotBakedHighWater();
extern uint64_t mc2LightSlotDedupHits();
extern uint64_t mc2LightSlotActorKeyHits();
extern uint32_t mc2LightSlotTableCount();
namespace mc2_lightslot_trace {
    static bool          s_enabled      = false;
    static bool          s_inited       = false;
    static bool          s_emitted      = false;
    static uint32_t      s_armFrame     = 0;     // frame at which mission first seen ready
    static uint32_t      s_accFrame     = 0xFFFFFFFFu; // frame the current accumulation belongs to
    static uint64_t      s_instances    = 0;     // K: static-prop tree-instance captures THIS frame
    static std::set<uint32_t> s_distinctSlots;   // D/U: distinct lightDataIndex over static instances (this frame)
    static std::set<const void*> s_types;        // N: distinct override tree TYPES (by appearType ptr, this frame)

    static inline void maybeEmit();    // fwd
    static inline void initFromEnv();  // fwd

    // TREE-OVERRIDE-LOD-MVP Task 3 (K×M GATE): registration-time accumulators for
    // the higher-LOD (lod>0) baked light slots. The per-frame s_distinctSlots set
    // only ever sees activeLOD=0 (pinned), so it cannot reveal whether LOD1's
    // per-instance bake created a SECOND lightData_ slot per instance. These
    // counters capture the lightData_ index every lod>0 registration bake
    // resolves to: LOD1_baked_instances counts the bakes; LOD1_distinct_slots is
    // the set of distinct lightData_ indices those bakes resolved to. If LODs of
    // one instance dedup to LOD0's slot, LOD1_distinct_slots stays ~K (PASS); if
    // each instance's LOD1 takes a fresh slot, it grows toward K (additive → K×M
    // total). These are cross-frame (registration is one-shot at mission load).
    static uint64_t      s_lodBakeCount = 0;
    static std::set<uint32_t> s_lodDistinctSlots;
    void recordLodBakeSlot(uint32_t lightDataIndex) {
        initFromEnv();
        if (!s_enabled) return;
        ++s_lodBakeCount;
        if (lightDataIndex != 0xFFFFFFFFu)
            s_lodDistinctSlots.insert(lightDataIndex);
    }

    static inline void initFromEnv() {
        if (s_inited) return;
        s_inited   = true;
        s_enabled  = mc2::gate::get<"MC2_LIGHTSLOT_TRACE">();
    }

    // Record one static-prop tree instance's resolved light slot + its type.
    // Accumulators are per-FRAME (reset on frame change) so s_instances is a
    // true K (instance count), not frame x instance, and the distinct-slot set
    // is a single-frame snapshot of the whole forest.
    void recordInstance(const void* appearType, bool isOverride,
                                      uint32_t lightDataIndex) {
        initFromEnv();
        if (!s_enabled || s_emitted) return;
        if (g_mc2FrameCounter != s_accFrame) {
            // Frame boundary: the just-completed frame (s_accFrame) holds a
            // full snapshot of every static tree touched. Try to emit from it
            // BEFORE clearing for the new frame.
            maybeEmit();
            s_accFrame = g_mc2FrameCounter;
            s_instances = 0;
            s_distinctSlots.clear();
            s_types.clear();
        }
        ++s_instances;
        if (lightDataIndex != 0xFFFFFFFFu)
            s_distinctSlots.insert(lightDataIndex);
        if (appearType && isOverride)
            s_types.insert(appearType);
    }

    // Emit one summary line once, a few frames after mission_ready so every
    // instance has been touched at least once. Called from the tree capture
    // site (cheap; bails immediately once emitted / disabled).
    static inline void maybeEmit() {
        if (!s_enabled || s_emitted) return;
        if (!SmokeMode::missionHasStarted()) return;
        if (s_armFrame == 0) { s_armFrame = s_accFrame; return; }
        // Emit the just-completed frame's snapshot once it is >= 8 frames past
        // mission_ready (forest fully touched + distinct-slot set settled).
        if (s_accFrame == 0xFFFFFFFFu || s_accFrame < s_armFrame + 8) return;
        if (s_instances == 0) return;

        const uint64_t H = mc2LightSlotDedupHits() + mc2LightSlotActorKeyHits();
        const uint32_t B = mc2LightSlotBakedHighWater();
        const size_t   U = s_distinctSlots.size();   // distinct slots attributable to static-prop instances
        const size_t   D = s_distinctSlots.size();   // per-instance distinct (same set — see report)
        mc2::fmt::print_out("[LIGHTSLOT v1] static_prop_instances={:d} override_tree_types={:d} registered_recipes={:d} unique_light_slots={:d} dedup_hits={:d} baked_slots={:d} per_instance_distinct={:d} table_count={:d} lod_bake_count={:d} lod_distinct_slots={:d}\n", mc2::fmt::as_u64((unsigned long long)s_instances), mc2::fmt::as_size(s_types.size()), mc2::fmt::as_u32(B /* R ~= baked recipe high-water */), mc2::fmt::as_size(U), mc2::fmt::as_u64((unsigned long long)H), mc2::fmt::as_u32(B), mc2::fmt::as_size(D), mc2::fmt::as_u32(mc2LightSlotTableCount()), mc2::fmt::as_u64((unsigned long long)s_lodBakeCount), mc2::fmt::as_size(s_lodDistinctSlots.size()));
        std::fflush(stdout);
        s_emitted = true;
    }
}

