#pragma once
// Out-of-pool pilot choice for LogisticsData::getBestPilot (pure, header-only so a doctest covers it).
// `filtered` holds only pilots that passed isAvailable() and !warriorInUse(); never fall back past the filter.
#include <span>
template <class P>
inline P* pickOutOfPoolPilot(std::span<P* const> filtered)
{
	return filtered.empty() ? nullptr : filtered.front();
}

// Hover text for the recovery button: the "No pilot available" id replaces the
// normal help text while the pool has no eligible pilot (the button is greyed).
inline int recoveryHelpTextId(bool noPilots, int normalId, int noPilotId)
{
	return noPilots ? noPilotId : normalId;
}
