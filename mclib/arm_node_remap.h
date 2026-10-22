// Pure weapon-node remap for a mech whose arm has been shot off (no engine dependencies, unit-testable).
// A weapon assigned to a node that sits on a lost arm fires from another weapon node of a compatible type on the remaining body.
#pragma once

namespace mc2_arm_remap {

// Mirrors the MECH3D_WEAPONTYPE_* values of objectappearance.h.
inline constexpr long kAny = 0, kMissile = 1, kBallistic = 2, kNonEnergy = 3, kEnergy = 4, kDirect = 5;

struct Node {
	long weaponType;   // what the node accepts (kAny.. kDirect)
	bool onRightArm;
	bool onLeftArm;
	long used;         // how many weapons are assigned to it (stagger)
};

inline bool nodeAccepts(long nodeType, long weaponType) {
	switch (weaponType) {
		case kMissile:   return nodeType == weaponType || nodeType == kAny || nodeType == kNonEnergy;
		case kBallistic: return nodeType == weaponType || nodeType == kAny || nodeType == kDirect || nodeType == kNonEnergy;
		case kEnergy:    return nodeType == weaponType || nodeType == kDirect || nodeType == kAny;
		default:         return true;
	}
}

// own = weapon-node index (0..count-1) the weapon was assigned to. Returns the weapon-node index to fire from; own when
// the node is still usable or no replacement exists. Least-used node wins, ties go to the lowest index.
inline long remapLostArmNode(const Node* nodes, long count, long own, long weaponType, bool rightArmOff, bool leftArmOff) {
	if (own < 0 || own >= count)
		return own;
	const auto lost = [&](long i) { return (rightArmOff && nodes[i].onRightArm) || (leftArmOff && nodes[i].onLeftArm); };
	if (!lost(own))
		return own;
	long best = -1, least = 999999999;
	for (long i = 0; i < count; ++i) {
		if (lost(i) || !nodeAccepts(nodes[i].weaponType, weaponType))
			continue;
		if (nodes[i].used < least) {
			least = nodes[i].used;
			best = i;
		}
	}
	return best >= 0 ? best : own;
}

} // namespace mc2_arm_remap
