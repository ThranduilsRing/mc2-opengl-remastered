#pragma once
// RENDER-FRAME-CONTEXT-1 — additive per-frame context substrate.
//
// gos_FrameCtx() is a READ-ONLY coherent view over the EXISTING authoritative
// per-frame globals (engine frame, view epoch, current EngineView, currency
// counters). It OWNS NOTHING. The globals remain the single source of truth; no
// consumer is required to change behavior; this slice is byte-identical.
//
//   RENDER-FRAME-CONTEXT-1 does not own render state. It aliases the existing
//   authoritative globals into one coherent diagnostic + validation surface.
//
// Authority inversion (globals become shims over this context, writes funnel
// through context setters) is a SEPARATE later slice:
// RENDER-FRAME-CONTEXT-AUTHORITY-1. Do NOT flip authority here.
//
// The mismatch self-check (gos_FrameCtxValidate / MC2_FRAMECTX_MISMATCH_FATAL) is
// a NO-OP BY CONSTRUCTION in this additive slice — the context reads the very
// globals it is compared against, so it cannot diverge yet. It is wired now so the
// safety net already exists when the authority slice makes the globals shims: at
// that point a shim diverging from the context becomes a counted/fatal event.
#include "mc2_czstring.h"
#include <atomic>
#include <cstdint>

namespace RenderCore { struct EngineView; const EngineView& getCurrentView(); }

extern std::atomic<uint32_t> g_mc2FrameCounter;   // engine frame clock (mclib/tgl.cpp)
extern long     g_mvpDiagFrame;      // raw publish counter (~2x/frame) (gameos_graphics.cpp)
extern long     g_viewContentEpoch;  // semantic VIEW-CONTENT epoch (VIEW-EPOCH-DEDUPE-1)
unsigned long   gos_object_mvp_stale_count();          // object draw fell back to live MVP
unsigned long   gos_object_mvp_used_count();           // object draw used depth-matched snapshot
unsigned long   gos_framectx_mismatch_count();         // this slice's M-guard counter
void            gos_framectx_note_mismatch(mc2::czstring what);

// Read-only coherent snapshot of this frame's authoritative state.
struct RenderFrameContext {
    uint64_t                       engineFrame;     // == g_mc2FrameCounter
    long                           viewEpoch;       // == g_mvpDiagFrame (publish counter)
    long                           viewContentEpoch;// == g_viewContentEpoch (semantic)
    const RenderCore::EngineView*  view;            // == &getCurrentView()
    unsigned long                  staleMvpReads;   // == gos_object_mvp_stale_count()
    unsigned long                  mvpSnapshotUsed; // == gos_object_mvp_used_count()
};

// Build the coherent view from the authoritative globals (read-through).
inline RenderFrameContext gos_FrameCtx() {
    RenderFrameContext c;
    c.engineFrame      = static_cast<uint64_t>(g_mc2FrameCounter.load(std::memory_order_relaxed));
    c.viewEpoch        = g_mvpDiagFrame;
    c.viewContentEpoch = g_viewContentEpoch;
    c.view             = &RenderCore::getCurrentView();
    c.staleMvpReads    = gos_object_mvp_stale_count();
    c.mvpSnapshotUsed  = gos_object_mvp_used_count();
    return c;
}

// Self-check: the context must agree with the authoritative globals. No-op by
// construction in the additive slice; load-bearing once globals become shims.
// Counts divergences; MC2_FRAMECTX_MISMATCH_FATAL=1 aborts (CI). Returns true=OK.
inline bool gos_FrameCtxValidate() {
    const RenderFrameContext c = gos_FrameCtx();
    bool ok = true;
    if (c.viewEpoch   != g_mvpDiagFrame)                    { gos_framectx_note_mismatch("viewEpoch");   ok = false; }
    if (c.engineFrame != static_cast<uint64_t>(g_mc2FrameCounter.load(std::memory_order_relaxed))) { gos_framectx_note_mismatch("engineFrame"); ok = false; }
    if (c.view        != &RenderCore::getCurrentView())     { gos_framectx_note_mismatch("view");        ok = false; }
    return ok;
}
