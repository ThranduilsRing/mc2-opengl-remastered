#pragma once
#include "tacordr.h"
#include "terrain_part_chunk.h"
#include "save_archive.h"

// Ordinary assignment starts a new order; a save copy must retain execution state.
inline void copySavedTacticalOrder(TacticalOrder& to, const TacticalOrder& from)
{
	to = from;
	to.id = from.id;
	to.lastMoveGoal = from.lastMoveGoal;
	to.statusCode = from.statusCode;
}

// The legacy packet remains a prefix. Watch IDs are 32-bit, mission-stable identities;
// unlike terrain chunk indices they survive a fresh process and registry.
struct SavedQueueTargets {
	static constexpr uint32_t tag = 0x32545157; // WQT2
	std::vector<uint32_t> targets;
	std::vector<GlobalPathStep> globalPath;
	// Signed: a failed replan on a COMPLEX route leaves the cursor at -1 (one before the first step) until the retry advances it.
	int32_t curGlobalStep = 0;
	std::vector<int16_t> lastGoalPath;
	void serialize(mc2::save::Archive& archive) {
		uint32_t signature = tag, version = 2;
		archive(signature, version);
		if (signature != tag || version != 2) archive.reject("queue_targets_version");
		archive.sequence(targets, 16);
		for (auto target : targets)
			if (target > 0x7fffffffU) archive.reject("queue_target_identity");
		archive.sequence(globalPath, MAX_GLOBAL_PATH, [](auto& a, auto& step) {
			a(step.startDoor, step.thruArea, step.goalDoor,
			  step.start.x, step.start.y, step.start.z, step.goal.x, step.goal.y, step.goal.z,
			  step.goalCell[0], step.goalCell[1], step.costToGoal);
		});
		archive(curGlobalStep);
		archive.sequence(lastGoalPath, MAX_GLOBAL_PATH);
		if (curGlobalStep < -1 || curGlobalStep > static_cast<int32_t>(MAX_GLOBAL_PATH) ||
		    (!globalPath.empty() && curGlobalStep > static_cast<int32_t>(globalPath.size())))
			archive.reject("global_path_cursor");
	}
};

// The target type follows nine flag bits in packedData[1]. Guard-point uses code 31.
inline bool packedOrderTargetsTerrain(unsigned long first, unsigned long second)
{
	switch (first & 0x1fUL) {
	case TACTICAL_ORDER_GUARD:
	case TACTICAL_ORDER_ATTACK_OBJECT:
	case TACTICAL_ORDER_MOVETO_OBJECT:
	case TACTICAL_ORDER_JUMPTO_OBJECT:
	case TACTICAL_ORDER_CAPTURE:
	case TACTICAL_ORDER_REFIT:
	case TACTICAL_ORDER_RECOVER:
	case TACTICAL_ORDER_GETFIXED:
		return ((second >> 9) & 3UL) == 1;
	default:
		return false;
	}
}

// Compact indices cannot survive a save/load, even when they still resolve in this process.
template<class Queue>
void dropCompactTerrainOrdersOnLoad(Queue* queue, char& count, int32_t& index)
{
	if (!terrain_part_chunk::compact())
		return;
	long kept = 0;
	long next = 0;
	for (long i = 0; i < count; ++i) {
		if (packedOrderTargetsTerrain(queue[i].packedData[0], queue[i].packedData[1]))
			continue;
		if (i < index)
			++next;
		queue[kept++] = queue[i];
	}
	count = static_cast<char>(kept);
	index = next < kept ? next : 0;
}
