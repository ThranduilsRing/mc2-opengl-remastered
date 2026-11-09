// Pure aim helpers for the MC2_FX_FORCE_SPAWN fixtures (warrior_combat.cpp) and the weapon range window.
// No engine dependencies: unit-tested by tests/unit/test_getweaponnode_adapter.cpp.
#pragma once

namespace mc2_fx_aim {

// Mirrors Mover::weaponInRange: the range class window widened by `buffer` on both ends (meters).
inline constexpr bool weaponRangeAccepts(float minRange, float maxRange, float metersToTarget, float buffer) {
	return !(metersToTarget < minRange - buffer) && !(metersToTarget > maxRange + buffer);
}

// Distance (meters) a fixture aims a weapon of `rangeClass` at: the middle of its gamesys.fit [WeaponRanges] window.
// ranges[class] = {min, max}. An out-of-range class falls back to class 0.
inline constexpr float fixtureAimDistance(const float (*ranges)[2], long numClasses, long rangeClass) {
	const long c = (rangeClass >= 0 && rangeClass < numClasses) ? rangeClass : 0;
	return 0.5f * (ranges[c][0] + ranges[c][1]);
}

} // namespace mc2_fx_aim
