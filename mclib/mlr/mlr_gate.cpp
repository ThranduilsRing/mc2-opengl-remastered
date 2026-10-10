//==========================================================================//
// File:    mlr_gate.cpp                                                     //
// Contents: MC2_DISABLE_GOSFX env-gate implementation.                     //
//           Plan v6 §2 (Stage A1) and §3 (Stage A2 default flip).          //
//===========================================================================//

#include "mlr_gate.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
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

bool g_initialized = false;
bool g_disabled = false;
std::mutex g_mutex;

void initialize_locked() {
    if (g_initialized) return;
    if (mc2::gate::isNonEmpty<"MC2_DISABLE_GOSFX">()) {
        // Explicit env override always wins.
        g_disabled = mc2::gate::get<"MC2_DISABLE_GOSFX">();
    } else {
        g_disabled = kDefaultDisabled;
    }
    g_initialized = true;
    std::fprintf(stderr, "[INSTR v1] enabled: mlr_gate disabled=%d\n",
                 g_disabled ? 1 : 0);
}
} // namespace

bool is_disabled() {
    if (g_initialized) return g_disabled;
    std::lock_guard<std::mutex> lock(g_mutex);
    initialize_locked();
    return g_disabled;
}

} // namespace mlr_gate
} // namespace mc2
