#pragma once
// Out-of-pool pilot choice for LogisticsData::getBestPilot (pure, header-only so a doctest covers it).
// `filtered` holds only pilots that passed isAvailable() and !warriorInUse(); never fall back past the filter.
template <class P>
inline P* pickOutOfPoolPilot(P* const* filtered, long filteredCount)
{
	return (filtered && filteredCount > 0) ? filtered[0] : nullptr;
}
