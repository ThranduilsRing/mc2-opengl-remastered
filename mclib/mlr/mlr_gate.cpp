//==========================================================================//
// File:    mlr_gate.cpp                                                     //
// Contents: MC2_DISABLE_GOSFX env-gate implementation.                     //
//           Plan v6 §2 (Stage A1) and §3 (Stage A2 default flip).          //
//===========================================================================//

#include "mlr_gate.h"

#include <cstdio>
#include <cstdlib>
#include "Gates/gate.h"

namespace mc2 {
namespace mlr_gate {

namespace {
// VFX-ORIGINALS-LOCKDOWN-1: kDefaultDisabled = false — MLR enabled.
// Tube/Shape/ShapeCloud/DebrisCloud render via original CPU MLR swept-mesh path.
// Oracle classes (CardCloud/ShardCloud/Card/PointCloud) bypass MLR via Effect::Draw
// in their gpu-path exit and are unaffected by this gate.
//
// Do NOT set kDefaultDisabled = true again unless ALL four non-oracle classes above
// have GPU oracle replacements with proven visual parity (interactive combat validated).
// Setting true makes Tube/Shape/ShapeCloud/DebrisCloud invisible — that was the
// regression this flag was introduced to fix in VFX-WEAPON-FX-RESTORE-OPUS-1.
//
// Kill-switch: MC2_DISABLE_GOSFX=1  (runtime override, no recompile needed).
// Migration plan: docs/vfx-originals-lockdown.md.
constexpr bool kDefaultDisabled = false;

// Read once, on first use, by whichever thread gets there first (a function-local static is initialised under the
// language's own lock; the hand-rolled double-checked latch this replaced read g_initialized unsynchronised).
bool compute_disabled() {
    bool disabled = kDefaultDisabled;
    if (mc2::gate::isNonEmpty<"MC2_DISABLE_GOSFX">()) {
        // Explicit env override always wins.
        disabled = mc2::gate::get<"MC2_DISABLE_GOSFX">();
    }
    std::fprintf(stderr, "[INSTR v1] enabled: mlr_gate disabled=%d\n", disabled ? 1 : 0);
    return disabled;
}
} // namespace

bool is_disabled() {
    static const bool disabled = compute_disabled();
    return disabled;
}

} // namespace mlr_gate
} // namespace mc2
