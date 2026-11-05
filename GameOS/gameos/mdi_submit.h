// MDI-SUBMISSION-SCAFFOLD-1 — indirect-draw submission self-report.
//
// OBSERVE, NOT STEER. Header-only, all-inline, gated by MC2_MDI_SUBMIT_TRACE (default
// OFF) — byte-for-byte zero cost when off. Adds NO GL calls, NO buffer change, NO
// scheduling. It names every indirect-draw submission so the frame can report how
// geometry entered each pass:
//
//   [MDI_SUBMIT] pass=StaticPropOpaque drawKind=MultiDrawElementsIndirect commands=2233 \
//                producer=GPU indirectBuffer=gpu_cull_indirect fallback=0
//
// Companion to docs/render-backend-seams/mdi-submission-ledger-recon-1.md and enforced by
// scripts/check-mdi-submission-ownership.py (every gl*Indirect() call site must carry an
// mdi_submit::trace).
#pragma once

#include "mc2_czstring.h"
#include <cstdio>
#include <cstdlib>
#include "Gates/gate.h"

namespace mdi_submit {

inline bool traceEnabled() {
    static const bool s = false;
    return s;
}

// `commands` = the drawcount passed to the indirect call (-1 if not cheaply known).
// `producer` = "CPU" (command buffer filled host-side) or "GPU" (compute/atomicAdd).
// `fallbackActive` = true when this call is the CPU/legacy fallback rather than the
// preferred indirect path.
inline void trace(mc2::czstring pass, mc2::czstring drawKind, int commands,
                  const char* producer, const char* indirectBuffer,
                  bool fallbackActive) {
    if (!traceEnabled()) return;
    std::fprintf(stderr,
        "[MDI_SUBMIT] pass=%s drawKind=%s commands=%d producer=%s indirectBuffer=%s fallback=%d\n",
        pass ? pass : "?", drawKind ? drawKind : "?", commands,
        producer ? producer : "?", indirectBuffer ? indirectBuffer : "?",
        fallbackActive ? 1 : 0);
    std::fflush(stderr);
}

}  // namespace mdi_submit
