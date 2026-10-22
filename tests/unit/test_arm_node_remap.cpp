// Witness for the arm-off weapon-node remap (sim change): a weapon on a lost arm used to be remapped by constants
// (1 -> 3, 2 -> 5) that are body-location numbers, not node indices.
#include "doctest.h"
#include "mclib/arm_node_remap.h"

using namespace mc2_arm_remap;

namespace {
// The pre-fix behaviour, kept here only as the "before" side of the witness.
long legacyRemap(long facing, bool rightArmOff, bool leftArmOff) {
	if (rightArmOff && facing == 1) facing = 3;
	if (leftArmOff && facing == 2) facing = 5;
	return facing;
}
// Weapon nodes (indices into the weapon-node range, absolute index = +2 smoke nodes):
//   w0 right arm energy, w1 left arm energy, w2 center torso any, w3 right torso missile, w4 left torso ballistic
const Node kNodes[] = {
	{kEnergy, true, false, 1}, {kEnergy, false, true, 1}, {kAny, false, false, 2},
	{kMissile, false, false, 0}, {kBallistic, false, false, 0},
};
}

TEST_CASE("arm remap: before - legacy constants name the wrong nodes") {
	// facing 1 is a smoke node (absolute index) and 3 is the right-torso MISSILE node: the remap hit neither the arm's
	// node nor a node chosen for the weapon's type
	CHECK(legacyRemap(1, true, false) == 3);
	CHECK(legacyRemap(2, false, true) == 5);   // 5: whatever node index 5 happens to be, chosen with no regard to type
	CHECK(legacyRemap(2, true, false) == 2);   // the right arm is gone and the weapon still fires from facing 2
}

TEST_CASE("arm remap: after - lost-arm node moves to a compatible node on the remaining body") {
	// energy weapon on the lost right arm (w0): the intact left-arm energy node is the least-used compatible one
	CHECK(remapLostArmNode(kNodes, 0, kEnergy, true, false) == 1);
	// energy weapon on the lost left arm (w1) with the right arm intact: the right-arm node
	CHECK(remapLostArmNode(kNodes, 1, kEnergy, false, true) == 0);
	// both arms gone, energy weapon: the center-torso node is all that is left
	CHECK(remapLostArmNode(kNodes, 0, kEnergy, true, true) == 2);
}

TEST_CASE("arm remap: usable nodes and impossible remaps are unchanged") {
	CHECK(remapLostArmNode(kNodes, 3, kMissile, true, true) == 3);    // torso node, arms irrelevant
	CHECK(remapLostArmNode(kNodes, 0, kEnergy, false, true) == 0);    // its own arm is intact
	CHECK(remapLostArmNode(kNodes, 9, kEnergy, true, true) == 9);     // not a weapon node
	const Node onlyArms[] = {{kAny, true, false, 0}, {kAny, false, true, 0}};
	CHECK(remapLostArmNode(onlyArms, 0, kEnergy, true, true) == 0);   // nowhere to go: keep the node
}

TEST_CASE("arm remap: least-used compatible node wins, ties go to the lowest index") {
	const Node nodes[] = {{kAny, true, false, 0}, {kAny, false, false, 3}, {kAny, false, false, 1}, {kAny, false, false, 1}};
	CHECK(remapLostArmNode(nodes, 0, kBallistic, true, false) == 2);
}
