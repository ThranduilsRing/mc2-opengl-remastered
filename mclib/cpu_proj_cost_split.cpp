// cpu_proj_cost_split.cpp — F3 CPU projection cost-baseline (measurement-only).
//
// See cpu_proj_cost_split.h header for spec, audit, and design rationale.

#include "cpu_proj_cost_split.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "Gates/gate.h"

namespace mc2_cpu_proj_cost {

// --- Module state ---------------------------------------------------------

bool g_cpuProjEnabled = false;
thread_local bool tls_inRenderLoop = false;

static bool s_initialized = false;
static bool s_finalStatsEmitted = false;

// Per-frame accumulators (reset every frame_begin).
struct PerFrameSample {
    int64_t  ns;       // accumulated ns in this frame (bucket / sidecar)
    int64_t  workload; // workload counter for this frame
};

static PerFrameSample s_curFrameBucket[BUCKET_COUNT]   = {};
static PerFrameSample s_curFrameSidecar[SIDECAR_COUNT] = {};

// Ring buffer of per-frame samples (one entry per committed frame).
// Window cadence: every WINDOW_FRAMES, compute p50/p95/max per bucket and
// emit a window-stats line; then clear the ring (start next window).
static constexpr int WINDOW_FRAMES = 500;

// Per-bucket ring of ns + workload (cleared each window).
static std::vector<int64_t> s_winNs[BUCKET_COUNT];
static std::vector<int64_t> s_winWorkload[BUCKET_COUNT];
static std::vector<int64_t> s_winSidecarNs[SIDECAR_COUNT];
static std::vector<int64_t> s_winSidecarWorkload[SIDECAR_COUNT];

// Full-capture retention for the [CPU_PROJ v1 final] line. Holds per-frame
// projection_total ns (for worst_window_p95 derivation) and per-bucket p95
// per window so we can compute worst_window_p95 = max(window.proj_total.p95).
static std::vector<int64_t> s_fullProjectionTotalNs;   // per frame
static std::vector<int64_t> s_fullBucketP95Ns[BUCKET_COUNT];   // per window
static std::vector<int64_t> s_windowProjTotalP95Ns;            // per window

// Frame counter (global; reset on hard_reset).
static int64_t s_frameCounter        = 0;
static int64_t s_windowStartFrame    = 0;
static int64_t s_winNumber           = 0;

// Worst-case camera label (cosmetic).
static constexpr const char* WORST_CASE_LABEL = "mc2_10";
static constexpr int64_t BUDGET_US = 100;

// Bucket names (in printout order).
static const char* kBucketName[BUCKET_COUNT] = {
    "matrix_build",
    "recalcBounds_perframe",
    "tgl_transform",
    "mlr_total",
};

static const char* kSidecarName[SIDECAR_COUNT] = {
    "skinning_chain",
    "eventdriven_projection_total",
    "cull_admission_perframe",
    "screenxy_perframe",
    "effect_admission_perframe",
    "terrain_admission_perframe",
    "lighting_shadow_perframe",
    "selection_picking_perframe",
    "debug_overlay_perframe",
};

// --- Helpers --------------------------------------------------------------

static inline int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Compute percentile (0..100) over a sorted view of `samples` (will sort in place).
static int64_t percentile_inplace(std::vector<int64_t>& v, double pct) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    if (pct <= 0.0) return v.front();
    if (pct >= 100.0) return v.back();
    double idx = (pct / 100.0) * (v.size() - 1);
    size_t lo = static_cast<size_t>(idx);
    size_t hi = std::min(v.size() - 1, lo + 1);
    double frac = idx - lo;
    return static_cast<int64_t>(v[lo] + frac * (v[hi] - v[lo]));
}

static int64_t max_inplace(const std::vector<int64_t>& v) {
    if (v.empty()) return 0;
    int64_t m = v[0];
    for (auto x : v) if (x > m) m = x;
    return m;
}

static int64_t median_inplace(std::vector<int64_t>& v, double pct) {
    return percentile_inplace(v, pct);
}

// us-format helper.
static inline double ns_to_us(int64_t ns) { return ns / 1000.0; }

// --- Window-stats printer -------------------------------------------------

static void emit_window_stats() {
    // Compute per-frame projection_total across the window from the per-bucket
    // ring buffer entries (one entry per frame, parallel arrays).
    const size_t nFrames = s_winNs[0].size();
    std::vector<int64_t> projTotal(nFrames, 0);
    for (size_t f = 0; f < nFrames; ++f) {
        for (int b = 0; b < BUCKET_COUNT; ++b) {
            // All bucket rings are kept in lock-step (same number of frames).
            if (f < s_winNs[b].size())
                projTotal[f] += s_winNs[b][f];
        }
    }

    // Per-bucket p50/p95/max.
    int64_t projP50 = 0, projP95 = 0, projMax = 0;
    {
        std::vector<int64_t> tmp(projTotal);
        projP50 = percentile_inplace(tmp, 50);
        std::vector<int64_t> tmp2(projTotal);
        projP95 = percentile_inplace(tmp2, 95);
        projMax = max_inplace(projTotal);
    }

    bool over = (projP95 / 1000) > BUDGET_US;
    int64_t winEnd = s_windowStartFrame + static_cast<int64_t>(nFrames) - 1;

    std::fprintf(stderr,
        "[CPU_PROJ v1] window=%lld..%lld worst-case-camera=%s\n"
        "  projection_total           p50=%.1fus p95=%.1fus max=%.1fus  budget=%lldus status=%s\n",
        (long long)s_windowStartFrame, (long long)winEnd, WORST_CASE_LABEL,
        ns_to_us(projP50), ns_to_us(projP95), ns_to_us(projMax),
        (long long)BUDGET_US, over ? "OVER" : "PASS");

    for (int b = 0; b < BUCKET_COUNT; ++b) {
        std::vector<int64_t> tmpA(s_winNs[b]);
        std::vector<int64_t> tmpB(s_winNs[b]);
        int64_t p50 = percentile_inplace(tmpA, 50);
        int64_t p95 = percentile_inplace(tmpB, 95);
        int64_t mx  = max_inplace(s_winNs[b]);
        std::vector<int64_t> tmpW(s_winWorkload[b]);
        int64_t wlP50 = percentile_inplace(tmpW, 50);
        std::fprintf(stderr,
            "    %-24s p50=%.1fus p95=%.1fus max=%.1fus  workload_p50=%lld\n",
            kBucketName[b], ns_to_us(p50), ns_to_us(p95), ns_to_us(mx), (long long)wlP50);
        // Capture per-window p95 for full-stats.
        s_fullBucketP95Ns[b].push_back(p95);
    }

    // Sidecars.
    std::fprintf(stderr, "  sidecar buckets (NEVER in projection_total adjudication):\n");
    for (int s = 0; s < SIDECAR_COUNT; ++s) {
        std::vector<int64_t> tmpA(s_winSidecarNs[s]);
        std::vector<int64_t> tmpB(s_winSidecarNs[s]);
        int64_t p50 = percentile_inplace(tmpA, 50);
        int64_t p95 = percentile_inplace(tmpB, 95);
        int64_t mx  = max_inplace(s_winSidecarNs[s]);
        std::vector<int64_t> tmpW(s_winSidecarWorkload[s]);
        int64_t wlP50 = percentile_inplace(tmpW, 50);
        std::fprintf(stderr,
            "    %-26s p50=%.1fus p95=%.1fus max=%.1fus  n_calls_p50=%lld\n",
            kSidecarName[s], ns_to_us(p50), ns_to_us(p95), ns_to_us(mx),
            (long long)wlP50);
    }

    // Retain for final.
    s_windowProjTotalP95Ns.push_back(projP95);

    // Clear window rings.
    for (int b = 0; b < BUCKET_COUNT; ++b) {
        s_winNs[b].clear();
        s_winWorkload[b].clear();
    }
    for (int s = 0; s < SIDECAR_COUNT; ++s) {
        s_winSidecarNs[s].clear();
        s_winSidecarWorkload[s].clear();
    }
    s_windowStartFrame = s_frameCounter + 1;
    ++s_winNumber;
}

// --- Public API impl ------------------------------------------------------

void init_from_env() {
    if (s_initialized) return;
    s_initialized = true;

    if (mc2::gate::get<"MC2_CPU_PROJ_COST_SPLIT">()) {
        g_cpuProjEnabled = true;
        std::fprintf(stderr,
            "[INSTR v1] enabled: CPU_PROJ_COST_SPLIT (spec v4, budget=%lldus)\n",
            (long long)BUDGET_US);
    } else {
        g_cpuProjEnabled = false;
        std::fprintf(stderr,
            "[CPU_PROJ v1 disabled] set MC2_CPU_PROJ_COST_SPLIT=1 to enable\n");
    }
    s_frameCounter     = 0;
    s_windowStartFrame = 0;
    s_winNumber        = 0;
    s_finalStatsEmitted = false;

    // Register atexit final-flush (only when enabled, but harmless either way).
    std::atexit([](){ final_stats("atexit"); });
}

void frame_begin() {
    if (!g_cpuProjEnabled) return;
    for (int b = 0; b < BUCKET_COUNT; ++b) {
        s_curFrameBucket[b].ns       = 0;
        s_curFrameBucket[b].workload = 0;
    }
    for (int s = 0; s < SIDECAR_COUNT; ++s) {
        s_curFrameSidecar[s].ns       = 0;
        s_curFrameSidecar[s].workload = 0;
    }
}

void frame_end() {
    if (!g_cpuProjEnabled) return;
    // Commit current frame into window rings.
    int64_t projTotalNs = 0;
    for (int b = 0; b < BUCKET_COUNT; ++b) {
        s_winNs[b].push_back(s_curFrameBucket[b].ns);
        s_winWorkload[b].push_back(s_curFrameBucket[b].workload);
        projTotalNs += s_curFrameBucket[b].ns;
    }
    for (int s = 0; s < SIDECAR_COUNT; ++s) {
        s_winSidecarNs[s].push_back(s_curFrameSidecar[s].ns);
        s_winSidecarWorkload[s].push_back(s_curFrameSidecar[s].workload);
    }
    s_fullProjectionTotalNs.push_back(projTotalNs);

    ++s_frameCounter;

    if (static_cast<int>(s_winNs[0].size()) >= WINDOW_FRAMES) {
        emit_window_stats();
    }
}

void hard_reset(const char* reason) {
    if (!g_cpuProjEnabled) return;
    // Flush any partial-window data first so the just-ended mission's data
    // isn't silently dropped.
    final_stats(reason ? reason : "hard_reset");
    // Clear everything for the next mission.
    for (int b = 0; b < BUCKET_COUNT; ++b) {
        s_winNs[b].clear();
        s_winWorkload[b].clear();
        s_fullBucketP95Ns[b].clear();
        s_curFrameBucket[b].ns = 0;
        s_curFrameBucket[b].workload = 0;
    }
    for (int s = 0; s < SIDECAR_COUNT; ++s) {
        s_winSidecarNs[s].clear();
        s_winSidecarWorkload[s].clear();
        s_curFrameSidecar[s].ns = 0;
        s_curFrameSidecar[s].workload = 0;
    }
    s_fullProjectionTotalNs.clear();
    s_windowProjTotalP95Ns.clear();
    s_frameCounter     = 0;
    s_windowStartFrame = 0;
    s_winNumber        = 0;
    s_finalStatsEmitted = false;
    std::fprintf(stderr, "[CPU_PROJ v1] hard_reset reason=%s\n",
                 reason ? reason : "(none)");
}

void final_stats(const char* reason) {
    if (!g_cpuProjEnabled) return;
    if (s_finalStatsEmitted) return;
    // Flush partial window (1..WINDOW_FRAMES-1 frames not yet stats'd).
    if (!s_winNs[0].empty()) {
        emit_window_stats();
    }
    if (s_fullProjectionTotalNs.empty()) {
        // Nothing captured (e.g., atexit with no frames). Stay quiet.
        s_finalStatsEmitted = true;
        return;
    }
    s_finalStatsEmitted = true;

    int64_t frames = static_cast<int64_t>(s_fullProjectionTotalNs.size());

    // Compute full-capture p50/p95/max of projection_total.
    std::vector<int64_t> tmpA(s_fullProjectionTotalNs);
    std::vector<int64_t> tmpB(s_fullProjectionTotalNs);
    int64_t projP50 = percentile_inplace(tmpA, 50);
    int64_t projP95 = percentile_inplace(tmpB, 95);
    int64_t projMax = max_inplace(s_fullProjectionTotalNs);
    int64_t worstWinP95 = max_inplace(s_windowProjTotalP95Ns);
    bool over = (worstWinP95 / 1000) > BUDGET_US;

    std::fprintf(stderr,
        "[CPU_PROJ v1 final] frames=%lld reason=%s worst-case-camera=%s\n"
        "  projection_total p50=%.1fus p95=%.1fus max=%.1fus  worst_window_p95=%.1fus  budget=%lldus status=%s\n",
        (long long)frames, reason ? reason : "(none)", WORST_CASE_LABEL,
        ns_to_us(projP50), ns_to_us(projP95), ns_to_us(projMax),
        ns_to_us(worstWinP95), (long long)BUDGET_US, over ? "OVER" : "PASS");

    // Per-bucket worst-window p95 (max of window p95s).
    for (int b = 0; b < BUCKET_COUNT; ++b) {
        int64_t bWorst = max_inplace(s_fullBucketP95Ns[b]);
        std::fprintf(stderr,
            "    %-24s worst_window_p95=%.1fus\n",
            kBucketName[b], ns_to_us(bWorst));
    }
}

// --- Workload counters ----------------------------------------------------

void add_workload_recalcbounds(int actorsRecalc) {
    if (!g_cpuProjEnabled) return;
    s_curFrameBucket[BUCKET_RECALCBOUNDS_PERFRAME].workload += actorsRecalc;
}

void add_workload_tgl_transform(int numShapes) {
    if (!g_cpuProjEnabled) return;
    s_curFrameBucket[BUCKET_TGL_TRANSFORM].workload += numShapes;
}

void add_workload_mlr_prim_clipped() {
    if (!g_cpuProjEnabled) return;
    s_curFrameBucket[BUCKET_MLR_TOTAL].workload += 1;
}

void add_workload_eventdriven_projectZ() {
    if (!g_cpuProjEnabled) return;
    s_curFrameSidecar[SIDECAR_EVENTDRIVEN_PROJECT_Z].workload += 1;
}

// R2: per-call chrono around projectForObjectAdmission. Pair pattern keeps
// camera.h at forward-decls only (no RAII class in the hot inline header).
// Caller MUST pair begin/end; mismatched pairing silently inflates the bucket.
int64_t cull_admission_begin_ns() {
    if (!g_cpuProjEnabled) return 0;
    return now_ns();
}

void cull_admission_end_ns(int64_t startNs) {
    if (!g_cpuProjEnabled) return;
    int64_t elapsed = now_ns() - startNs;
    s_curFrameSidecar[SIDECAR_CULL_ADMISSION_PERFRAME].ns       += elapsed;
    s_curFrameSidecar[SIDECAR_CULL_ADMISSION_PERFRAME].workload += 1;
}

// R3 narrow-subset wrapper sidecars. One pair per remaining policy-split
// wrapper in camera.h. Same chrono-around-wrapper pattern as the R2
// cull_admission pair — keeps the inner projectZ un-timed.
int64_t screenxy_begin_ns() {
    if (!g_cpuProjEnabled) return 0;
    return now_ns();
}
void screenxy_end_ns(int64_t startNs) {
    if (!g_cpuProjEnabled) return;
    int64_t elapsed = now_ns() - startNs;
    s_curFrameSidecar[SIDECAR_SCREENXY_PERFRAME].ns       += elapsed;
    s_curFrameSidecar[SIDECAR_SCREENXY_PERFRAME].workload += 1;
}

int64_t effect_admission_begin_ns() {
    if (!g_cpuProjEnabled) return 0;
    return now_ns();
}
void effect_admission_end_ns(int64_t startNs) {
    if (!g_cpuProjEnabled) return;
    int64_t elapsed = now_ns() - startNs;
    s_curFrameSidecar[SIDECAR_EFFECT_ADMISSION_PERFRAME].ns       += elapsed;
    s_curFrameSidecar[SIDECAR_EFFECT_ADMISSION_PERFRAME].workload += 1;
}

int64_t terrain_admission_begin_ns() {
    if (!g_cpuProjEnabled) return 0;
    return now_ns();
}
void terrain_admission_end_ns(int64_t startNs) {
    if (!g_cpuProjEnabled) return;
    int64_t elapsed = now_ns() - startNs;
    s_curFrameSidecar[SIDECAR_TERRAIN_ADMISSION_PERFRAME].ns       += elapsed;
    s_curFrameSidecar[SIDECAR_TERRAIN_ADMISSION_PERFRAME].workload += 1;
}

int64_t lighting_shadow_begin_ns() {
    if (!g_cpuProjEnabled) return 0;
    return now_ns();
}
void lighting_shadow_end_ns(int64_t startNs) {
    if (!g_cpuProjEnabled) return;
    int64_t elapsed = now_ns() - startNs;
    s_curFrameSidecar[SIDECAR_LIGHTING_SHADOW_PERFRAME].ns       += elapsed;
    s_curFrameSidecar[SIDECAR_LIGHTING_SHADOW_PERFRAME].workload += 1;
}

int64_t selection_picking_begin_ns() {
    if (!g_cpuProjEnabled) return 0;
    return now_ns();
}
void selection_picking_end_ns(int64_t startNs) {
    if (!g_cpuProjEnabled) return;
    int64_t elapsed = now_ns() - startNs;
    s_curFrameSidecar[SIDECAR_SELECTION_PICKING_PERFRAME].ns       += elapsed;
    s_curFrameSidecar[SIDECAR_SELECTION_PICKING_PERFRAME].workload += 1;
}

int64_t debug_overlay_begin_ns() {
    if (!g_cpuProjEnabled) return 0;
    return now_ns();
}
void debug_overlay_end_ns(int64_t startNs) {
    if (!g_cpuProjEnabled) return;
    int64_t elapsed = now_ns() - startNs;
    s_curFrameSidecar[SIDECAR_DEBUG_OVERLAY_PERFRAME].ns       += elapsed;
    s_curFrameSidecar[SIDECAR_DEBUG_OVERLAY_PERFRAME].workload += 1;
}

// --- Scope guards ---------------------------------------------------------

Scope::Scope(int bucketId) : bucket_(bucketId), armed_(false), startNs_(0) {
    if (!g_cpuProjEnabled) return;
    armed_   = true;
    startNs_ = now_ns();
}

Scope::~Scope() {
    if (!armed_) return;
    int64_t elapsed = now_ns() - startNs_;
    s_curFrameBucket[bucket_].ns += elapsed;
}

SidecarScope::SidecarScope(int sidecarId) : sidecar_(sidecarId), armed_(false), startNs_(0) {
    if (!g_cpuProjEnabled) return;
    armed_   = true;
    startNs_ = now_ns();
}

SidecarScope::~SidecarScope() {
    if (!armed_) return;
    int64_t elapsed = now_ns() - startNs_;
    s_curFrameSidecar[sidecar_].ns += elapsed;
}

RenderLoopGuard::RenderLoopGuard() : prior_(tls_inRenderLoop), armed_(false) {
    if (!g_cpuProjEnabled) return;
    armed_           = true;
    tls_inRenderLoop = true;
}

RenderLoopGuard::~RenderLoopGuard() {
    if (!armed_) return;
    tls_inRenderLoop = prior_;
}

} // namespace mc2_cpu_proj_cost
