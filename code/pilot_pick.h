#pragma once
// Out-of-pool pilot choice for LogisticsData::getBestPilot (pure, header-only so a doctest covers it).
// `filtered` holds only pilots that passed isAvailable() and !warriorInUse(); never fall back past the filter.
#include <span>
template <class P>
inline P* pickOutOfPoolPilot(std::span<P* const> filtered)
{
	return filtered.empty() ? nullptr : filtered.front();
}
