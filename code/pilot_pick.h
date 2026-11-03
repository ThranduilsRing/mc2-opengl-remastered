#pragma once
// Out-of-pool pilot choice for LogisticsData::getBestPilot (pure, header-only so a doctest covers it).
// `filtered` holds only pilots that passed isAvailable() and !warriorInUse(); never fall back past the filter.
#include <optional>
#include <span>
// First candidate that is available and not already flying a mech (applies the filter in MP too, where the
// pool listing is not pre-filtered by warriorInUse); nullopt when none qualifies.
template <class P, class InUse>
inline std::optional<P*> pickOutOfPoolPilot(std::span<P* const> candidates, InUse inUse)
{
	for (P* p : candidates)
		if (p && p->isAvailable() && !inUse(p))
			return p;
	return std::nullopt;
}

// Primary pool predicate for getBestPilot/gotPilotsLeft: available and not already in a live mech, in SP and MP alike.
template <class P, class InUse>
inline bool poolEligible(P& p, InUse inUse)
{
	return p.isAvailable() && !inUse(p);
}

// Hover text for the recovery button: the "No pilot available" id replaces the
// normal help text while the pool has no eligible pilot (the button is greyed).
inline int recoveryHelpTextId(bool noPilots, int normalId, int noPilotId)
{
	return noPilots ? noPilotId : normalId;
}
