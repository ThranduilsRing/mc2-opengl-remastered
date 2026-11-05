// Slice-2 Recon Zero instrumentation impl.
//
// See header for rationale. Output line schema:
//
//   [OBJECT_RECON v1] frame=<N>
//     bldg_update={ns:U, calls:U}
//     tree_update={ns:U, calls:U}
//     generic_update={ns:U, calls:U}
//     mShape={ns:U, calls:U}
//     shape={ns:U, calls:U,
//            alloc:U, xform:U, vlight:U, flight:U, emit:U}
//
// The shape-sub-stage fields sum to <= shape.total_ns (Total wall time
// includes per-call overhead outside the timed sub-stages — backface cull
// vector ops, oneOff/oneOn flag bookkeeping, etc.).
//
// Cadence:
// - Per-frame line: gated on MC2_OBJECT_RECON_TRACY=1; only when any counter
//   is non-zero.
// - 600-frame summary: unconditional once any non-zero data observed in the
//   run (matches [TGL_POOL v1] discipline in mclib/tgl.cpp:3457-3472).
// - Shutdown summary: unconditional final emit.

#include "mc2_czstring.h"
#include "gos_object_recon_tracy.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "Gates/gate.h"
#include "mc2fmt.h"

extern "C" void crashbundle_append(mc2::czstring line); // mclib/tgl.cpp helper

namespace mc2_object_recon {

bool g_enabled = false;
Counters g_per_frame;
Counters g_mono;
uint32_t g_frame_with_data = 0;

namespace {
    bool s_initDone = false;

    inline bool counterAnyNonzero(const Counters& c) {
        return (c.bldg_update_ns | c.tree_update_ns | c.generic_update_ns |
                c.mShape_total_ns | c.shape_total_ns) != 0;
    }

    inline void accumulateInto(Counters& dst, const Counters& src) {
        dst.bldg_update_ns       += src.bldg_update_ns;
        dst.bldg_update_calls    += src.bldg_update_calls;
        dst.tree_update_ns       += src.tree_update_ns;
        dst.tree_update_calls    += src.tree_update_calls;
        dst.generic_update_ns    += src.generic_update_ns;
        dst.generic_update_calls += src.generic_update_calls;
        dst.mShape_total_ns      += src.mShape_total_ns;
        dst.mShape_calls         += src.mShape_calls;
        dst.shape_total_ns       += src.shape_total_ns;
        dst.shape_alloc_ns       += src.shape_alloc_ns;
        dst.shape_xform_ns       += src.shape_xform_ns;
        dst.shape_vlight_ns      += src.shape_vlight_ns;
        dst.shape_flight_ns      += src.shape_flight_ns;
        dst.shape_emit_ns        += src.shape_emit_ns;
        dst.shape_calls          += src.shape_calls;
    }

    inline void resetCounters(Counters& c) { c = Counters{}; }

    void emitLine(mc2::czstring tag, uint32_t frame, const Counters& c) {
        char buf[1024];
        mc2::fmt::to_buf(buf, "[OBJECT_RECON v1] {}={:d} bldg_update={{ns:{:d},calls:{:d}}} tree_update={{ns:{:d},calls:{:d}}} generic_update={{ns:{:d},calls:{:d}}} mShape={{ns:{:d},calls:{:d}}} shape={{ns:{:d},calls:{:d},alloc:{:d},xform:{:d},vlight:{:d},flight:{:d},emit:{:d}}}", mc2::fmt::safe(tag), mc2::fmt::as_u32(static_cast<unsigned>(frame)), mc2::fmt::as_u64(static_cast<unsigned long long>(c.bldg_update_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(c.bldg_update_calls)), mc2::fmt::as_u64(static_cast<unsigned long long>(c.tree_update_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(c.tree_update_calls)), mc2::fmt::as_u64(static_cast<unsigned long long>(c.generic_update_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(c.generic_update_calls)), mc2::fmt::as_u64(static_cast<unsigned long long>(c.mShape_total_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(c.mShape_calls)), mc2::fmt::as_u64(static_cast<unsigned long long>(c.shape_total_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(c.shape_calls)), mc2::fmt::as_u64(static_cast<unsigned long long>(c.shape_alloc_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(c.shape_xform_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(c.shape_vlight_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(c.shape_flight_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(c.shape_emit_ns)));
        std::puts(buf);
        crashbundle_append(buf);
        std::fflush(stdout);
    }
} // namespace

void initFromEnv() {
    if (s_initDone) return;
    s_initDone = true;

    g_enabled = false;

    if (g_enabled) {
        std::puts("[OBJECT_RECON v1] event=enabled note=accumulators_active");
        std::fflush(stdout);
    }
}

void drainPerFrame(uint32_t frameCounter) {
    // Lazy init guard — safe even if caller forgot to invoke initFromEnv().
    if (!s_initDone) initFromEnv();

    const bool hadDataThisFrame = counterAnyNonzero(g_per_frame);
    if (hadDataThisFrame && g_frame_with_data == 0) {
        g_frame_with_data = frameCounter;
    }

    // Per-frame line: only when env is set AND we actually accumulated.
    if (g_enabled && hadDataThisFrame) {
        emitLine("frame", frameCounter, g_per_frame);
    }

    // Roll into monotonic.
    accumulateInto(g_mono, g_per_frame);
    resetCounters(g_per_frame);

    // 600-frame summary: matches [TGL_POOL v1] gating discipline.
    if ((frameCounter > 0) && ((frameCounter % 600) == 0) && counterAnyNonzero(g_mono)) {
        emitLine("summary", frameCounter, g_mono);
    }
}

void drainOnShutdown() {
    if (counterAnyNonzero(g_mono) || counterAnyNonzero(g_per_frame)) {
        // Flush any straggling per-frame data into mono so the final summary
        // covers the full run.
        accumulateInto(g_mono, g_per_frame);
        resetCounters(g_per_frame);
        emitLine("shutdown", 0, g_mono);
    }
}

} // namespace mc2_object_recon
