#pragma once
// Out-of-pool pilot choice for LogisticsData::getBestPilot (pure, header-only so a doctest covers it).
// `filtered` holds only pilots that passed isAvailable() and !warriorInUse(); never fall back past the filter.
#include <optional>
#include <span>
#include <vector>
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

// The whole pool selection, shared by LogisticsData::getBestPilot and gotPilotsLeft (and the doctest, which runs it on fakes
// but through this very code). `pilots` is any range of P; `inUse(P&)` says the pilot already flies a live mech (SP and MP
// alike: MPlayer is deliberately not consulted); `cmp(a, b)` > 0 means a ranks ahead of b.
// Returns the chosen pilot (marked used) or nullptr when no available, not-in-a-mech pilot is left.
template <class P, class Range, class InUse, class Cmp>
inline P* selectPoolPilot(Range& pilots, InUse inUse, Cmp cmp)
{
	std::vector<P*> pool;
	for (auto& p : pilots)
		if (poolEligible(p, inUse))
			pool.push_back(&p);
	const int count = static_cast<int>(pool.size());

	for (int i = 1; i < count; ++i)
	{
		P* cur = pool[i];
		for (int j = 0; j < i; ++j)
		{
			if (cmp(cur, pool[j]) > 0)
			{
				pool[i] = pool[j];
				pool[j] = cur;
				break;
			}
		}
	}

	for (P* p : pool)
		if (p->isAvailable() && !p->isUsed())
		{
			p->setUsed(true);
			return p;
		}

	// Out of unused pilots: reuse the best eligible one (re-checked, so a pilot in a live mech is never handed out twice).
	const auto pick = pickOutOfPoolPilot<P>(std::span<P* const>(pool.data(), pool.size()), [&](P* p) { return inUse(*p); });
	if (!pick)
		return nullptr;
	(*pick)->setUsed(true);
	return *pick;
}

// True when at least one eligible pilot has not been handed out yet.
template <class P, class Range, class InUse>
inline bool poolHasUnusedPilot(Range& pilots, InUse inUse)
{
	for (auto& p : pilots)
		if (poolEligible(p, inUse) && !p.isUsed())
			return true;
	return false;
}

// Hover text for the recovery button: the "No pilot available" id replaces the
// normal help text while the pool has no eligible pilot (the button is greyed).
inline int recoveryHelpTextId(bool noPilots, int normalId, int noPilotId)
{
	return noPilots ? noPilotId : normalId;
}
