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
#include "mc2fmt.h"

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
    unsigned int             budgetSixths = 0;
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
    mc2::fmt::print_err("[GPU_PARTICLES v1] event=summary emit_total={:d} flush_total={:d} nonempty_flush_total={:d} records_flushed_total={:d} records_per_flush_max={:d} trail_spawn={:d} trail_head={:d}\n", mc2::fmt::as_u64(g_emit_total), mc2::fmt::as_u64(g_flush_total), mc2::fmt::as_u64(g_nonempty_flush_total), mc2::fmt::as_u64(g_records_flushed_total), mc2::fmt::as_u32(g_records_per_flush_max), mc2::fmt::as_u64(Batcher::s_trail_spawn_total), mc2::fmt::as_u64(Batcher::s_trail_head_total));
    std::fflush(stderr);
}

void initialize_env() {
    if (g_enabled_initialized) return;
    g_log_value = mc2::gate::get<"MC2_GPU_PARTICLES_LOG">();
    // Accepted Card/CardCloud geometry is native by default. The kill-switch
    // suppresses submission; their CPU/MLR renderer has been retired.
    g_enabled_value = mc2::gate::get<"MC2_GPU_PARTICLES">();
    g_enabled_initialized = true;
    if (g_enabled_value && g_log_value) {
        mc2::fmt::print_err("[INSTR v1] enabled: gpu_particles log={:d}\n",
                     mc2::fmt::as_i32(static_cast<int>(g_log_value)));
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
        s_val = mc2::gate::get<"MC2_VFX_ORACLE_TUBE">();    // native Vulkan ribbons; =0 is the legacy MLR kill-switch
        s_init = true;
        MC2_DIAGFE(stderr, "[VFX_ORACLE_TUBE v1] gate=%s (default-on; =0 uses legacy MLR)\n",
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
    : impl_(std::make_unique<Impl>(perFrameBudget)) {}

Batcher::~Batcher() = default;

void Batcher::BeginGroup(uint32_t handle, float u0, float v0, float us, float vs,
                         int blendMode, uint32_t atlasColumns) {
    if (!is_enabled()) return;
    // Close the previous open group by recording its count.
    if (impl_->hasOpenGroup && !impl_->groups.empty()) {
        GroupInfo& prev = impl_->groups.back();
        // Adjacent particles may share the same live UV rectangle. Preserve draw order.
        if (prev.handle == handle && prev.u0 == u0 && prev.v0 == v0 &&
            prev.us == us && prev.vs == vs && prev.blendMode == blendMode &&
            prev.atlasColumns == atlasColumns) return;
        prev.count = static_cast<unsigned>(impl_->staging.size()) - prev.start;
    }
    GroupInfo gi;
    gi.handle       = handle;
    gi.u0           = u0;
    gi.v0           = v0;
    gi.us           = us;
    gi.vs           = vs;
    gi.start        = static_cast<unsigned>(impl_->staging.size());
    gi.count        = 0;
    gi.blendMode    = blendMode;
    gi.atlasColumns = atlasColumns;
    impl_->groups.push_back(gi);
    impl_->hasOpenGroup = true;
}

void Batcher::Emit(const GpuParticle& p) {
    if (!is_enabled()) return;
    // Cards carry six authored vertices; charge one particle for the whole quad.
    const unsigned sixths = (p.kind_flags & 64u) ? 1u : 6u;
    if (impl_->budgetSixths + sixths > impl_->budget * 6u) {
        if (!impl_->overflowReported) {
            impl_->overflowReported = true;
            mc2::fmt::print_err("[GPU_PARTICLES v1] event=overflow budget={:d} — record dropped\n", mc2::fmt::as_u32(impl_->budget));
            std::fflush(stderr);
        }
        return;
    }
    impl_->budgetSixths += sixths;
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
            mc2::fmt::print_err("[RESOLVE_PROBE] first_nonempty groups={:d} staging={:d}\n", mc2::fmt::as_u32(static_cast<unsigned>(impl_->groups.size())), mc2::fmt::as_u32(static_cast<unsigned>(impl_->staging.size())));
            std::fflush(stderr);
        }
    }

    static std::unordered_set<uint32_t> s_probed;
    for (auto& grp : impl_->groups) {
        if (grp.handle == 0) continue;
        if (!MidLevelRenderer::MLRTexturePool::Instance) {
            if (logEnabled && s_probed.insert(grp.handle).second)
                mc2::fmt::print_err("[RESOLVE_PROBE] handle={:d} POOL_NULL\n", mc2::fmt::as_u32(grp.handle));
            continue;
        }
        MidLevelRenderer::MLRTexture* mlrTex =
            (*MidLevelRenderer::MLRTexturePool::Instance)[static_cast<int>(grp.handle)];
        if (!mlrTex) {
            if (logEnabled && s_probed.insert(grp.handle).second)
                mc2::fmt::print_err("[RESOLVE_PROBE] handle={:d} TEX_NULL\n", mc2::fmt::as_u32(grp.handle));
            continue;
        }
        MidLevelRenderer::GOSImage* img = mlrTex->GetImage();
        if (!img) {
            if (logEnabled && s_probed.insert(grp.handle).second)
                mc2::fmt::print_err("[RESOLVE_PROBE] handle={:d} IMG_NULL\n", mc2::fmt::as_u32(grp.handle));
            continue;
        }
        DWORD gosHandle = img->GetHandle();
        if (logEnabled && s_probed.insert(grp.handle).second)
            mc2::fmt::print_err("[RESOLVE_PROBE] handle={:d} img={:016X} isLoaded={:d} gosHandle={:d}\n", mc2::fmt::as_u32(grp.handle), mc2::fmt::as_ptr(static_cast<void*>(img)), mc2::fmt::as_i32(static_cast<int>(img->IsLoaded())), mc2::fmt::as_u32(static_cast<unsigned>(gosHandle)));
        grp.handle = static_cast<uint32_t>(gosHandle);
    }
}

void Batcher::Flush() {
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
        last.count = static_cast<unsigned>(impl_->staging.size()) - last.start;
    }
    const unsigned int n = static_cast<unsigned int>(impl_->staging.size());
    ++g_nonempty_flush_total;
    g_records_flushed_total += n;
    if (n > g_records_per_flush_max) g_records_per_flush_max = n;
    if (g_log_value && !g_first_flush_reported) {
        g_first_flush_reported = true;
        mc2::fmt::print_err("[GPU_PARTICLES v1] event=first_flush records={:d} groups={:d}\n", mc2::fmt::as_u32(n), mc2::fmt::as_u32(static_cast<unsigned>(impl_->groups.size())));
        std::fflush(stderr);
    }
    gos_particle_bridge_flush(impl_->staging.data(), n,
                              impl_->groups.empty() ? nullptr : impl_->groups.data(),
                              static_cast<unsigned>(impl_->groups.size()));
    impl_->staging.clear();
    impl_->budgetSixths = 0;
    impl_->groups.clear();
    impl_->overflowReported = false;
    impl_->hasOpenGroup = false;
}

Batcher& Batcher::Instance() {
    // Intentionally leaked; same static-dtor-ordering reasoning as the
    // fx_trace tables (see mclib/fx_trace/fx_trace.cpp). Native geometry
    // stores up to six vertex records per original particle. Keep the
    // original 4096-particle capacity after that expansion.
    static Batcher* s_instance = new Batcher(4096 * 6);
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
