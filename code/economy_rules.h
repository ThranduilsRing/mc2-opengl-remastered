// code/economy_rules.h -- pure helpers for the two economy rules (GameRules::cbillMultiplier, GameRules::salvage). Standard library only.
//
// payoutScaled: the mission-objective C-Bill payout (LogisticsData::endMission, per successful objective) scaled by the multiplier percent.
//   pct == 100 returns the input untouched; otherwise a 64-bit intermediate, truncation toward zero, clamped to int. Only the objective
//   payout is scaled: the additionalCBills stipend, salvage refunds and cheats are not income from objectives and stay unscaled.
// salvagePriceFactor: the fraction of a mech's cost charged to salvage it (salvagemecharea.cpp). MC2 (stock) is exactly 0.8f; Generous is
//   0.6f (cheaper); Scarce is 1.0f (full price). No RNG; which mechs are offered for salvage is unchanged.
#pragma once

#include <algorithm>
#include <climits>
#include <cstdint>

#include "game_rules.h"

namespace mc2::rules {

inline int payoutScaled(int rp, long pct) {
    if (pct == 100) return rp;
    const std::int64_t v = static_cast<std::int64_t>(rp) * static_cast<std::int64_t>(pct) / 100;  // truncates toward zero
    constexpr std::int64_t kLo = INT_MIN, kHi = INT_MAX;
    return static_cast<int>(std::clamp(v, kLo, kHi));
}

inline float salvagePriceFactor(long mode) {
    if (mode == kSalvageGenerous) return 0.6f;
    if (mode == kSalvageScarce) return 1.0f;
    return .8f;  // kSalvageMC2 and any unknown value: stock
}

}  // namespace mc2::rules
