#pragma once
// Out-of-pool pilot choice for LogisticsData::getBestPilot (pure, header-only so a doctest covers it).
// `filtered` holds only pilots that passed isAvailable() and !warriorInUse(); never fall back past the filter.
#include <span>
// First candidate that is available and not already flying a mech (applies the filter in MP too, where the
// pool listing is not pre-filtered by warriorInUse); nullptr when none qualifies.
template <class P, class InUse>
inline P* pickOutOfPoolPilot(std::span<P* const> candidates, InUse inUse)
{
	for (P* p : candidates)
		if (p && p->isAvailable() && !inUse(p))
			return p;
	return nullptr;
}

// Hover text for the recovery button: the "No pilot available" id replaces the
// normal help text while the pool has no eligible pilot (the button is greyed).
inline int recoveryHelpTextId(bool noPilots, int normalId, int noPilotId)
{
	return noPilots ? noPilotId : normalId;
}
