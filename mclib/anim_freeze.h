// anim_freeze.h -- MC2_SMOKE_FREEZE_ANIM: stop mech gait and building animation frames from advancing.
//
// WHY: the gait and building animation advance by the wall-clock render frame length, so the pose of a mech (and through the weapon nodes
// and line-of-sight positions that read it, parts of the simulation) differs run to run. Smoke and capture runs set this gate so
// A/B pixel verdicts and the [SIMID v1] stream carry no animation-phase noise.
#pragma once
#include "Gates/gate.h"

namespace mc2 {

inline bool animationsFrozen() {
    static const bool on = gate::get<"MC2_SMOKE_FREEZE_ANIM">();
    return on;
}

}  // namespace mc2
