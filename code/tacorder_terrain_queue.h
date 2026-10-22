#pragma once
#include "tacordr.h"
#include "terrain_part_chunk.h"

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
