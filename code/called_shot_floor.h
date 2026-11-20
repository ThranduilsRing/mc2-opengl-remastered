// ISSUE-9 called shots: absolute floors on the final aimed-shot hit chance.
// Header-only and Gate-free so the unit test includes it as shipped.
// BALANCE HERE: percent (0-100), applied only under MC2_CALLED_SHOT_FIX.
#pragma once
namespace called_shot {
constexpr float kHeadFloorPct = 30.0f;
constexpr float kArmFloorPct  = 45.0f;
constexpr float kLegFloorPct  = 50.0f;

// An aimed shot never drops below floorPct, but the floor never lifts it above
// the unaimed chance (a call is not better than a plain shot at long range).
inline float applyFloor(float aimedChance, float unaimedChance, float floorPct) {
	const float lift = unaimedChance < floorPct ? unaimedChance : floorPct;
	return aimedChance > lift ? aimedChance : lift;
}

// GameRules CalledShots / PilotSkillWeight consumers (read per call from mc2::rules::current(), never cached).
// Mode values mirror mc2::rules::kCalledOff/Floors/Classic (static_assert in mover_attack.cpp).
constexpr long kOff = 0, kFloors = 1, kClassic = 2;
// MC2_CALLED_SHOT_FIX=0 is the legacy kill-switch and forces Classic; otherwise the rule decides.
inline long mode(bool fixGate, long ruleMode) { return fixGate ? ruleMode : kClassic; }
// Off: called shots do not exist for either side (the shot is a plain shot).
inline long aim(long aimLocation, long modeValue) { return modeValue == kOff ? -1 : aimLocation; }
// PilotSkillWeight percent of the pilot's gunnery skill; 100 is the integer early-out (bit-identical to today).
inline float weightSkill(float skill, long pct) {
	if (pct == 100) return skill;
	return skill * (static_cast<float>(pct) / 100.0f);
}
} // namespace called_shot
