// cpu_proj_cost_split.h
//
// F3 — CPU projection cost-baseline (measurement-only) v1.
//
// Spec: docs/superpowers/specs/2026-05-20-cpu-projection-cost-baseline-design.md
// Audit: docs/superpowers/specs/2026-05-20-cpu-projection-cost-baseline-INSTRUMENTATION-AUDIT.md
//
// Env gate: MC2_CPU_PROJ_COST_SPLIT=1. When OFF, ALL chrono and ALL counter
// work is short-circuited at a single inline check (g_cpuProjEnabled). The
// instrumentation is genuinely zero-cost when disabled.
//
// Per-frame discipline:
//   * gos_RendererBeginFrame() -> frame_begin() — reset bucket accumulators.
//   * Bucket scopes (RAII) open/close around outer call boundaries; each
//     scope's elapsed nanoseconds + workload count accumulate into the
//     current-frame bucket.
//   * gos_RendererEndFrame() -> frame_end() — commit per-frame bucket
//     samples into the ring buffer. Every 500 frames, print window stats.
//   * Mission::init / Mission::load -> hard_reset() — clear ring buffer +
//     frame counter at mission boundaries.
//
// Single-threaded per audit (a). Thread-local counters used for safety
// and Vulkan-prep forward-compat.

#pragma once

#include "mc2_czstring.h"
#include <cstdint>

namespace mc2_cpu_proj_cost {

// Bucket identifiers. Order matters: matches printout order.
enum class BucketId : int {
    BUCKET_MATRIX_BUILD          = 0,  // Camera.BuildMVP + SetCameraMatrices
    BUCKET_RECALCBOUNDS_PERFRAME = 1,  // 6 recalcBounds overrides (aggregated)
    BUCKET_TGL_TRANSFORM         = 2,  // TG_MultiShape::TransformMultiShape (aggregated)
    BUCKET_MLR_TOTAL             = 3,  // StartDraw + RenderNow
    BUCKET_COUNT                 = 4,
};

// Sidecar (NEVER part of projection_total adjudication).
enum class SidecarId : int {
    SIDECAR_SKINNING_CHAIN          = 0,  // Mech3D.UpdateGeometry outer
    SIDECAR_EVENTDRIVEN_PROJECT_Z   = 1,  // projectZ called outside render loop (count-only)
    SIDECAR_CULL_ADMISSION_PERFRAME = 2,  // projectForObjectAdmission (timed; R2 follow-on)
    // R3 narrow-subset sidecars — one timed bucket per remaining
    // policy-split wrapper. Goal: identify which wrapper accounts for
    // the ~946-call/frame mc2_10 surplus surfaced by
    // SIDECAR_EVENTDRIVEN_PROJECT_Z (R2 falsified cull-admission as the
    // source). Same chrono-around-wrapper pattern as the R2 sidecar.
    SIDECAR_SCREENXY_PERFRAME           = 3,  // projectForScreenXY
    SIDECAR_EFFECT_ADMISSION_PERFRAME   = 4,  // projectForEffectAdmission
    SIDECAR_TERRAIN_ADMISSION_PERFRAME  = 5,  // projectForTerrainAdmission
    SIDECAR_LIGHTING_SHADOW_PERFRAME    = 6,  // projectForLightingShadow
    SIDECAR_SELECTION_PICKING_PERFRAME  = 7,  // projectForSelectionPicking
    SIDECAR_DEBUG_OVERLAY_PERFRAME      = 8,  // projectForDebugOverlay
    SIDECAR_COUNT                       = 9,
};

// Master env flag — checked at the head of every scope/counter path. When
// false, scopes do nothing and counters do not tick. Set once at startup
// by init_from_env(). Defined in cpu_proj_cost_split.cpp.
extern bool g_cpuProjEnabled;

// TLS flag: true inside ObjectManager::render(). Used to attribute
// projectZ calls to render-vs-eventdriven. Set by RenderLoopGuard below.
extern thread_local bool tls_inRenderLoop;

// Public API ---------------------------------------------------------------

// One-shot: read MC2_CPU_PROJ_COST_SPLIT, print banner, init state.
// Safe to call multiple times (idempotent).
void init_from_env();

// Per-frame hooks (no-op when env OFF).
void frame_begin();   // Reset current-frame bucket accumulators.
void frame_end();     // Commit current frame into ring buffer; print window stats if due.

// Mission boundary hook — hard-reset ring buffer + frame counter.
void hard_reset(mc2::czstring reason);

// Final-stats flush — called at process exit AND at mission load. Prints
// the [CPU_PROJ v1 final] line using all per-frame samples retained.
void final_stats(mc2::czstring reason);

// Workload counter accumulators (per-call, accumulated into the current
// frame's bucket). All gated on g_cpuProjEnabled at the entry point.
void add_workload_recalcbounds(int actorsRecalc);
void add_workload_tgl_transform(int numShapes);
void add_workload_mlr_prim_clipped();
void add_workload_eventdriven_projectZ();   // sidecar count-only

// R2 cull-admission instrumentation (projectForObjectAdmission wrapper).
// Free-function pair instead of RAII Scope so camera.h's header-weight
// stays at forward-decls only (consistent with the existing pattern
// established by add_workload_eventdriven_projectZ). Both functions are
// no-ops when env OFF (single g_cpuProjEnabled check at entry).
//   * cull_admission_begin_ns(): returns steady_clock ns sample, or 0
//     when disabled. Caller MUST pair with cull_admission_end_ns().
//   * cull_admission_end_ns(startNs): accumulates elapsed ns into
//     SIDECAR_CULL_ADMISSION_PERFRAME and bumps workload by 1.
//   * Observer-effect caveat: chrono pair ~50-100 ns x ~1000 calls/frame
//     = ~50-100 us self-cost on the measurement itself. The reported
//     bucket time is an UPPER bound. See cost_split observer-effect
//     memory; the R2 capture must be read with that in mind.
int64_t cull_admission_begin_ns();
void    cull_admission_end_ns(int64_t startNs);

// R3 narrow-subset wrapper sidecars. Identical contract to the R2
// cull-admission pair: env-OFF returns 0 / no-op; end_ns accumulates
// elapsed ns and bumps workload by 1 (n_calls_p50 = paired-call count).
// Wired top/bottom of the matching inline wrapper in camera.h. Same
// ~50–100 us upper-bound observer-effect caveat applies per wrapper
// (chrono pair x calls/frame). Cross-validation: the SUM of all 7
// narrow-subset wrapper n_calls (cull_admission + the 6 below) should
// approximately equal eventdriven_projection_total.n_calls; any wild
// discrepancy is a stop-condition (see §14).
int64_t screenxy_begin_ns();
void    screenxy_end_ns(int64_t startNs);
int64_t effect_admission_begin_ns();
void    effect_admission_end_ns(int64_t startNs);
int64_t terrain_admission_begin_ns();
void    terrain_admission_end_ns(int64_t startNs);
int64_t lighting_shadow_begin_ns();
void    lighting_shadow_end_ns(int64_t startNs);
int64_t selection_picking_begin_ns();
void    selection_picking_end_ns(int64_t startNs);
int64_t debug_overlay_begin_ns();
void    debug_overlay_end_ns(int64_t startNs);

// RAII scope guard — opens chrono, closes + accumulates into bucket on
// destruction. Zero-cost when env OFF (constructor early-out).
class Scope {
public:
    explicit Scope(BucketId bucketId);
    ~Scope();
    // Non-copyable, non-movable.
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    BucketId bucket_;
    bool    armed_;
    int64_t startNs_;
};

// Sidecar scope (for skinning_chain etc).
class SidecarScope {
public:
    explicit SidecarScope(SidecarId sidecarId);
    ~SidecarScope();
    SidecarScope(const SidecarScope&) = delete;
    SidecarScope& operator=(const SidecarScope&) = delete;
private:
    SidecarId sidecar_;
    bool    armed_;
    int64_t startNs_;
};

// TLS render-loop guard — set tls_inRenderLoop=true on construction,
// restore prior value on destruction. RAII discipline (handles nesting).
class RenderLoopGuard {
public:
    RenderLoopGuard();
    ~RenderLoopGuard();
private:
    bool prior_;
    bool armed_;
};

} // namespace mc2_cpu_proj_cost
