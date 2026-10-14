//***************************************************************************
//
//	score_sort.h -- ordering of scored items with one fixed rule for equal scores
//
//	MechCommander II
//
//***************************************************************************

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

// WHAT: sorts the FILLED items of a candidate list by score. WHY: the old SortList handed the whole padded slot array to the C runtime qsort, so
// the order of equal scores depended on the CRT algorithm and on the slot count (two equal items came out reversed, three as [b,c,a]); the
// rule here is written down and does not depend on the library.
//
// The rule, in order of precedence: (1) a NaN score sorts after every number in BOTH directions; (2) the score in the requested direction;
// (3) equal scores (-0 equals +0, an infinity equals itself) keep the order in which the items were filled. Only the items of the span are
// ordered: there are no padding slots and the span length carries no meaning. The sort works in place on the caller's span and allocates nothing,
// so a hot-path caller sizes a local buffer to its domain bound and pays no heap cost.
namespace mc2
{

enum class SortOrder
{
	Descending, // largest score first (threat, value, attack chance)
	Ascending,	// smallest score first (distance)
};

struct Scored
{
	float score = 0.0f;
	std::int32_t id = 0;		   // what the caller wants back: an object handle, a weapon slot, a range type
	std::uint32_t fillSlot = 0; // set by sort_scored from the position in the span; callers leave it alone
};

inline void sort_scored(std::span<Scored> items, SortOrder order)
{
	for (std::size_t i = 0; i < items.size(); ++i)
		items[i].fillSlot = static_cast<std::uint32_t>(i);
	const bool descending = (order == SortOrder::Descending);
	std::sort(items.begin(), items.end(), [descending](const Scored& a, const Scored& b) {
		const bool nanA = std::isnan(a.score);
		const bool nanB = std::isnan(b.score);
		if (nanA != nanB)
			return nanB;
		if (!nanA && a.score != b.score)
			return descending ? (a.score > b.score) : (a.score < b.score);
		return a.fillSlot < b.fillSlot;
	});
}

} // namespace mc2
