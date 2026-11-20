// ISSUE-9 called shots: absolute floors on the final aimed-shot hit chance.
// Header-only and Gate-free so the unit test includes it as shipped.
// BALANCE HERE: percent (0-100), applied only under MC2_CALLED_SHOT_FIX.
#pragma once
#include <algorithm>

namespace called_shot {
constexpr float kHeadFloorPct = 30.0f;
constexpr float kArmFloorPct  = 45.0f;
constexpr float kLegFloorPct  = 50.0f;

// An aimed shot never drops below floorPct, but the floor never lifts it above
// the unaimed chance (a call is not better than a plain shot at long range).
inline float applyFloor(float aimedChance, float unaimedChance, float floorPct) {
	return std::max(aimedChance, std::min(floorPct, unaimedChance));
}
} // namespace called_shot
