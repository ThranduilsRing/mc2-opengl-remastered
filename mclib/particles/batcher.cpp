//==========================================================================//
// File:    batcher.cpp                                                      //
// Contents: GPU particle batcher implementation.                            //
//           Plan v5 §5.4 B1 Stage 1' Commit 1.                              //
//           FX-GPU-1 Phase 2: BeginGroup / per-group UV sub-rect.           //
//===========================================================================//

#include "batcher.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1

#include <mlr/mlr.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <vector>
#include "Gates/gate.h"

// Bridge entry point implemented in GameOS/gameos/gos_particle_bridge.cpp.
// Forward-declared here so this TU never #includes GameOS/GL headers — the
// one-way dependency rule (see batcher.h doc comment) is enforced at the
// include level. Stage 1' Commit 4 wires the bridge.
extern "C" void gos_particle_bridge_flush(const mc2::particles::GpuParticle* records,
                                          unsigned int                       count,
                                          const mc2::particles::GroupInfo*   groups,
                                          unsigned int                       numGroups);

namespace mc2 {
namespace particles {

// Trail-spawn counters (see batcher.h declaration; incremented by gpu_trail.cpp).
unsigned long long Batcher::s_trail_spawn_total = 0;
unsigned long long Batcher::s_trail_head_total  = 0;

struct Batcher::Impl {
    std::vector<GpuParticle> staging;
    std::vector<GroupInfo>   groups;
    unsigned int             budget;
    bool                     overflowReported;
    bool                     hasOpenGroup;   // true iff BeginGroup was called this frame
    Impl(unsigned int b)
        : staging(), groups(), budget(b), overflowReported(false), hasOpenGroup(false) {
        staging.reserve(b);
        groups.reserve(64);
    }
};

namespace {
bool g_enabled_initialized = false;
bool g_enabled_value       = false;
bool g_log_value           = false;   // MC2_GPU_PARTICLES_LOG=1

// C12 diagnostic counters. Process-lifetime aggregates; dumped at atexit.
// Three diagnostic categories:
//   emit_total == 0                              -> routing bug
//   emit_total > 0, records_flushed_total == 0   -> state-lifecycle bug
//   emit_total > 0, records_flushed_total > 0    -> GPU render bug
unsigned long long g_emit_total              = 0;
unsigned long long g_flush_total             = 0;
unsigned long long g_records_flushed_total   = 0;
unsigned int       g_records_per_flush_max   = 0;
unsigned long long g_nonempty_flush_total    = 0;
bool               g_first_flush_reported    = false;
bool               g_atexit_registered       = false;

void dump_summary() {
    std::fprintf(stderr,
                 "[GPU_PARTICLES v1] event=summary emit_total=%llu flush_total=%llu "
                 "nonempty_flush_total=%llu records_flushed_total=%llu "
                 "records_per_flush_max=%u trail_spawn=%llu trail_head=%llu\n",
                 g_emit_total, g_flush_total, g_nonempty_flush_total,
                 g_records_flushed_total, g_records_per_flush_max,
                 Batcher::s_trail_spawn_total,
                 Batcher::s_trail_head_total);
    std::fflush(stderr);
}

void initialize_env() {
    if (g_enabled_initialized) return;
    g_log_value = mc2::gate::get<"MC2_GPU_PARTICLES_LOG">();
    // REVERTED TO CPU/MLR DEFAULT (user request: GPU FX looked broken). Absent
    // env var → DISABLED (legacy CPU billboards). Opt back into GPU with "1".
    g_enabled_value = mc2::gate::get<"MC2_GPU_PARTICLES">();   // default OFF -> legacy CPU/MLR FX
    if (mc2::gate::isSet<"MC2_GPU_PARTICLES">() && !g_enabled_value
        && mc2::gate::state<"MC2_GPU_PARTICLES">() != mc2::gate::Tri::Off) {
        // Unknown value — warn once if logging enabled.
        if (g_log_value) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::fprintf(stderr,
                    "[GPU_PARTICLES] unknown MC2_GPU_PARTICLES value; disabling\n");
                std::fflush(stderr);
            }
        }
    }
    g_enabled_initialized = true;
    if (g_enabled_value) {
        std::fprintf(stderr, "[INSTR v1] enabled: gpu_particles log=%d\n",
                     (int)g_log_value);
        std::fflush(stderr);
        if (!g_atexit_registered) {
            std::atexit(dump_summary);
            g_atexit_registered = true;
        }
    }
}
}  // namespace

bool Batcher::is_enabled() {
    if (!g_enabled_initialized) initialize_env();
    return g_enabled_value;
}

bool Batcher::is_log_enabled() {
    if (!g_enabled_initialized) initialize_env();
    return g_log_value;
}

// VFX-ORIGINAL-RECORD-ABI-1 (Phase 1): CPU-oracle render gate.
// Default OFF (reverted to CPU/MLR; the VFX-WEAPON-FX-RESTORE-OPUS-1 default-ON was reverted).
// When ON, a migrated class's Draw harvests the CPU sim's live per-particle
// arrays and renders them via the GPU billboard path.
// Opt-in: MC2_VFX_ORACLE_RENDER=1  (only '1' enables; unset or 0 = OFF)
// Read once, process-lifetime.
bool Batcher::is_oracle_render_enabled() {
    static bool s_init = false;
    static bool s_val  = false;
    if (!s_init) {
        // REVERTED TO CPU/MLR DEFAULT (user request). Absent = OFF (legacy MLR
        // cards); opt into GPU with "1".
        s_val = mc2::gate::get<"MC2_VFX_ORACLE_RENDER">();
        s_init = true;
        MC2_DIAGFE(stderr, "[VFX_ROUTE v1] oracle_render=%s reason=%s\n",
            s_val ? "on" : "off",
            s_val ? "opt_in" : "default_off");
        std::fflush(stderr);
    }
    return s_val;
}

// VFX-GPU-SIM-CARDCLOUD-BUFFER-1: compare-only GPU sim gates. Default OFF.
// _cardcloud gates the CardCloud GPU-sim buffer handoff (gather + SSBO
// upload); _compare gates the [VFX_GPU_SIM v1] integrity/compare logs.
// Neither renders or alters the frame. Read once, process-lifetime.
bool Batcher::is_gpu_sim_cardcloud_enabled() {
    static bool s_init = false;
    static bool s_val  = false;
    if (!s_init) {
        s_val = mc2::gate::get<"MC2_VFX_GPU_SIM_CARDCLOUD">();
        s_init = true;
    }
    return s_val;
}

bool Batcher::is_gpu_sim_compare_enabled() {
    static bool s_init = false;
    static bool s_val  = false;
    if (!s_init) {
        s_val = mc2::gate::get<"MC2_VFX_GPU_SIM_COMPARE">();
        s_init = true;
    }
    return s_val;
}

// MC2_VFX_ORACLE_TUBE slice 1: Tube swept-quad ribbon oracle gate.
// DEFAULT-ON 2026-06-15. The 2026-06-11 stopgap ("oracle submits but pixels
// never reach the composited frame") was STALE — the deferred-flush +
// single-COLOR0 MRT fix (gos_particle_bridge.cpp) already repaired the
// RenderWorld render-target regression. Proven two ways:
//   * MC2_VFX_ORACLE_TUBE_COVERAGE occlusion query: samples=1050..1728/frame
//     into fbo=1 sceneDrawBufs=3 (the composited RenderWorld MRT) — tube
//     fragments rasterize + pass depth into the scene FBO. Deterministic,
//     immune to the ~2.3% cross-launch combat nondeterminism that defeats
//     pixel-diff.
//   * Eyes-on (mc2_01 + MC2_FX_FORCE_SPAWN, ~8 frames post-fire): Bushwacker
//     PPC renders the blue oracle ribbon; recognizable weapon FX.
// Default-ON with a =0 kill-switch: absent or any value != "0" = oracle;
// MC2_VFX_ORACLE_TUBE=0 = legacy MLR fallback.
bool Batcher::is_oracle_tube_enabled() {
    static bool s_init = false;
    static bool s_val  = false;
    if (!s_init) {
        s_val = mc2::gate::get<"MC2_VFX_ORACLE_TUBE">();    // REVERTED: DEFAULT-OFF (legacy MLR tubes); =1 opts into GPU
        s_init = true;
        MC2_DIAGFE(stderr, "[VFX_ORACLE_TUBE v1] gate=%s (default-off; =1 enables GPU)\n",
                     s_val ? "on" : "off");
        std::fflush(stderr);
    }
    return s_val;
}

// MC2_VFX_ORACLE_SHAPE: GPU mesh-effect substrate for gosFX::Shape (default
// OFF). Opt-in =1. Read once, process-lifetime (debug_instrumentation_rule.md
// idiom — same as is_oracle_render_enabled but DEFAULT OFF this slice).
bool Batcher::is_oracle_shape_enabled() {
    static bool s_init = false;
    static bool s_val  = false;
    if (!s_init) {
        s_val = mc2::gate::get<"MC2_VFX_ORACLE_SHAPE">();
        s_init = true;
        // QUIET-DEFAULT-BUILD-1: announce behind MC2_DIAG_VERBOSE, matching the
        // sibling is_oracle_tube_enabled() gate above. A default-OFF gate
        // printing "I am off" on every default run is the exact noise class
        // that contract owns.
        MC2_DIAGFE(stderr, "[VFX_MESH v1] oracle_shape=%s\n",
                   s_val ? "on" : "off");
        std::fflush(stderr);
    }
    return s_val;
}

bool Batcher::is_oracle_tube_log_enabled() {
    static bool s_init = false;
    static bool s_val  = false;
    if (!s_init) {
        s_val = mc2::gate::get<"MC2_VFX_ORACLE_TUBE_LOG">();
        s_init = true;
    }
    return s_val;
}

bool Batcher::is_oracle_shape_log_enabled() {
    static bool s_init = false;
    static bool s_val  = false;
    if (!s_init) {
        s_val = mc2::gate::get<"MC2_VFX_ORACLE_SHAPE_LOG">();
        s_init = true;
    }
    return s_val;
}

Batcher::Batcher(unsigned int perFrameBudget)
    : impl_(new Impl(perFrameBudget)) {}

Batcher::~Batcher() { delete impl_; }

void Batcher::BeginGroup(uint32_t handle, float u0, float v0, float us, float vs,
                         int blendMode, uint32_t atlasColumns) {
    if (!is_enabled()) return;
    // Close the previous open group by recording its count.
    if (impl_->hasOpenGroup && !impl_->groups.empty()) {
        GroupInfo& prev = impl_->groups.back();
        prev.count = (unsigned)impl_->staging.size() - prev.start;
    }
    GroupInfo gi;
    gi.handle       = handle;
    gi.u0           = u0;
    gi.v0           = v0;
    gi.us           = us;
    gi.vs           = vs;
    gi.start        = (unsigned)impl_->staging.size();
    gi.count        = 0;
    gi.blendMode    = blendMode;
    gi.atlasColumns = atlasColumns;
    impl_->groups.push_back(gi);
    impl_->hasOpenGroup = true;
}

void Batcher::Emit(const GpuParticle& p) {
    if (!is_enabled()) return;
    if (impl_->staging.size() >= impl_->budget) {
        if (!impl_->overflowReported) {
            impl_->overflowReported = true;
            std::fprintf(stderr,
                         "[GPU_PARTICLES v1] event=overflow budget=%u — record dropped\n",
                         impl_->budget);
            std::fflush(stderr);
        }
        return;
    }
    impl_->staging.push_back(p);
    ++g_emit_total;
}

void Batcher::ResolveTextures()
{
    if (!impl_) return;
    // Force-load all pool textures unconditionally (ForceLoadImages sets
    // unLoadedImages=true before calling LoadImages so the early-exit is
    // bypassed). This handles the common case where gosFX textures were added
    // to the pool AFTER the normal actor-init LoadImages() window, leaving
    // unLoadedImages=false with mcTextureNodeIndex still 0xffffffff.
    if (MidLevelRenderer::MLRTexturePool::Instance)
        MidLevelRenderer::MLRTexturePool::Instance->ForceLoadImages();

    // MC2_GPU_PARTICLES_LOG=1 gated: per-handle RESOLVE_PROBE output.
    // Normal runs see only the first-flush banner; probes require opt-in.
    const bool logEnabled = is_log_enabled();

    // Once-only diagnostic (gated): group/staging counts on first non-empty frame.
    if (logEnabled) {
        static bool s_reported = false;
        if (!s_reported && !impl_->groups.empty()) {
            s_reported = true;
            std::fprintf(stderr,
                         "[RESOLVE_PROBE] first_nonempty groups=%u staging=%u\n",
                         (unsigned)impl_->groups.size(),
                         (unsigned)impl_->staging.size());
            std::fflush(stderr);
        }
    }

    static std::unordered_set<uint32_t> s_probed;
    for (auto& grp : impl_->groups) {
        if (grp.handle == 0) continue;
        if (!MidLevelRenderer::MLRTexturePool::Instance) {
            if (logEnabled && s_probed.insert(grp.handle).second)
                std::fprintf(stderr, "[RESOLVE_PROBE] handle=%u POOL_NULL\n", grp.handle);
            continue;
        }
        MidLevelRenderer::MLRTexture* mlrTex =
            (*MidLevelRenderer::MLRTexturePool::Instance)[static_cast<int>(grp.handle)];
        if (!mlrTex) {
            if (logEnabled && s_probed.insert(grp.handle).second)
                std::fprintf(stderr, "[RESOLVE_PROBE] handle=%u TEX_NULL\n", grp.handle);
            continue;
        }
        MidLevelRenderer::GOSImage* img = mlrTex->GetImage();
        if (!img) {
            if (logEnabled && s_probed.insert(grp.handle).second)
                std::fprintf(stderr, "[RESOLVE_PROBE] handle=%u IMG_NULL\n", grp.handle);
            continue;
        }
        DWORD gosHandle = img->GetHandle();
        if (logEnabled && s_probed.insert(grp.handle).second)
            std::fprintf(stderr, "[RESOLVE_PROBE] handle=%u img=%p isLoaded=%d gosHandle=%u\n",
                grp.handle, (void*)img, (int)img->IsLoaded(), (unsigned)gosHandle);
        grp.handle = static_cast<uint32_t>(gosHandle);
    }
}

// VFX-GPU-SIM-CARDCLOUD-COMPUTE-1: per-frame flush of the CardCloud GPU-sim
// accumulator (GameOS bridge). Declared here to avoid pulling the GL header.
extern "C" void gos_cardcloud_sim_flush(void);

void Batcher::Flush() {
    // Flush the CardCloud GPU-sim accumulator once per frame, BEFORE both the
    // is_enabled() and empty-staging early-returns — the sim accumulates
    // independently of the particle staging buffer. No-op when nothing was
    // accumulated. Placed first so the accumulator can never grow unbounded
    // even if MC2_GPU_PARTICLES is off or a future caller submits without the
    // particle path enabled.
    gos_cardcloud_sim_flush();
    if (!is_enabled()) return;
    ++g_flush_total;
    if (impl_->staging.empty()) {
        impl_->groups.clear();
        impl_->hasOpenGroup = false;
        return;
    }
    // Close the last open group.
    if (impl_->hasOpenGroup && !impl_->groups.empty()) {
        GroupInfo& last = impl_->groups.back();
        last.count = (unsigned)impl_->staging.size() - last.start;
    }
    const unsigned int n = (unsigned int)impl_->staging.size();
    ++g_nonempty_flush_total;
    g_records_flushed_total += n;
    if (n > g_records_per_flush_max) g_records_per_flush_max = n;
    if (!g_first_flush_reported) {
        g_first_flush_reported = true;
        std::fprintf(stderr,
                     "[GPU_PARTICLES v1] event=first_flush records=%u groups=%u\n",
                     n, (unsigned)impl_->groups.size());
        std::fflush(stderr);
    }
    gos_particle_bridge_flush(impl_->staging.data(), n,
                              impl_->groups.empty() ? nullptr : impl_->groups.data(),
                              (unsigned)impl_->groups.size());
    impl_->staging.clear();
    impl_->groups.clear();
    impl_->overflowReported = false;
    impl_->hasOpenGroup = false;
}

Batcher& Batcher::Instance() {
    // Intentionally leaked; same static-dtor-ordering reasoning as the
    // fx_trace tables (see mclib/fx_trace/fx_trace.cpp). Default budget
    // 4096 is a Stage 1' placeholder; Stage 2' sizes from coverage data.
    static Batcher* s_instance = new Batcher(4096);
    return *s_instance;
}

// VFX-SPINE-0: read-only C-linkage accessors for the Object Inspector.
// Defined inside the mc2::particles namespace so they can reach Batcher::Impl
// and the anonymous-namespace counters. No mutation, no new counters added.
extern "C" unsigned long long mc2_vfx_getEmitTotal()           { return g_emit_total; }
extern "C" unsigned long long mc2_vfx_getFlushTotal()          { return g_flush_total; }
extern "C" unsigned long long mc2_vfx_getNonemptyFlushTotal()  { return g_nonempty_flush_total; }
extern "C" unsigned long long mc2_vfx_getRecordsFlushedTotal() { return g_records_flushed_total; }
extern "C" unsigned int       mc2_vfx_getRecordsPerFlushMax()  { return g_records_per_flush_max; }
extern "C" unsigned long long mc2_vfx_getTrailSpawnTotal()     { return Batcher::s_trail_spawn_total; }
extern "C" unsigned long long mc2_vfx_getTrailHeadTotal()      { return Batcher::s_trail_head_total; }
extern "C" int                mc2_vfx_isEnabled()              { return Batcher::is_enabled() ? 1 : 0; }
extern "C" int                mc2_vfx_isLogEnabled()           { return Batcher::is_log_enabled() ? 1 : 0; }
extern "C" unsigned int       mc2_vfx_getBudget()              { return Batcher::Instance().inspect_budget(); }
extern "C" int                mc2_vfx_getOverflowReported()    { return Batcher::Instance().inspect_overflowReported() ? 1 : 0; }

unsigned int Batcher::inspect_budget() const           { return impl_ ? impl_->budget           : 0u; }
bool         Batcher::inspect_overflowReported() const { return impl_ ? impl_->overflowReported : false; }

}  // namespace particles
}  // namespace mc2
