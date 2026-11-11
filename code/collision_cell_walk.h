// SIM-TICK-CUT: occupied-cell broadphase walk for CollisionGrid. Pure logic (no engine types) so a doctest can
// compare it with the dense row-major walk it replaces.
//
// Items are placed in insertion order, each in one cell. The walk visits occupied cells in ascending cell
// index (row-major), objects within a cell newest-first (the legacy LIFO bucket order), and for each object
// asks for the four neighbour ranges in the legacy order: the rest of its own cell, x+1, x+1/y+1, y+1.
// Cost is O(items log items) instead of O(cells) per tick; scratch arrays persist across ticks.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mc2::sim {

class OccupiedCellWalk {
public:
	struct Item { uint32_t cell; };

	// order()[slot] = index into the placed list, cell-major, newest first within a cell.
	void build(const Item* items, size_t count, uint32_t xWidth, uint32_t yWidth) {
		xWidth_ = xWidth;
		yWidth_ = yWidth;
		const size_t cells = static_cast<size_t>(xWidth) * yWidth;
		if (range_.size() != cells) {
			range_.assign(cells, Range{});
			gen_ = 0;
		}
		if (++gen_ == 0) {   // generation wrapped: stale stamps could alias, so clear them
			range_.assign(cells, Range{});
			gen_ = 1;
		}
		keys_.resize(count);
		for (size_t k = 0; k < count; ++k)
			keys_[k] = (static_cast<uint64_t>(items[k].cell) << 32) | static_cast<uint32_t>(~static_cast<uint32_t>(k));
		std::sort(keys_.begin(), keys_.end());
		order_.resize(count);
		occupied_.clear();
		uint32_t prevCell = 0;
		for (size_t slot = 0; slot < count; ++slot) {
			const uint32_t cell = static_cast<uint32_t>(keys_[slot] >> 32);
			order_[slot] = ~static_cast<uint32_t>(keys_[slot] & 0xFFFFFFFFu);
			if (slot == 0 || cell != prevCell) {
				occupied_.push_back(cell);
				range_[cell] = Range{gen_, static_cast<uint32_t>(slot), static_cast<uint32_t>(slot + 1)};
				prevCell = cell;
			} else {
				range_[cell].end = static_cast<uint32_t>(slot + 1);
			}
		}
	}

	const std::vector<uint32_t>& order() const { return order_; }
	const std::vector<uint32_t>& occupied() const { return occupied_; }

	// object(slot) once per object, then range(slot, first, last) for up to four slot ranges. Empty ranges are
	// never reported (the dense walk's empty ranges visited nothing).
	template <class OnObject, class OnRange>
	void walk(OnObject&& onObject, OnRange&& onRange) const {
		for (const uint32_t cell : occupied_) {
			const uint32_t x = cell % xWidth_;
			const uint32_t y = cell / xWidth_;
			const Range own = range_[cell];
			const Range right = x + 1 < xWidth_ ? at(cell + 1) : Range{};
			const Range diag = (x + 1 < xWidth_ && y + 1 < yWidth_) ? at(cell + 1 + xWidth_) : Range{};
			const Range down = y + 1 < yWidth_ ? at(cell + xWidth_) : Range{};
			for (uint32_t slot = own.begin; slot != own.end; ++slot) {
				onObject(slot);
				if (slot + 1 != own.end) onRange(slot, slot + 1, own.end);
				if (right.begin != right.end) onRange(slot, right.begin, right.end);
				if (diag.begin != diag.end) onRange(slot, diag.begin, diag.end);
				if (down.begin != down.end) onRange(slot, down.begin, down.end);
			}
		}
	}

private:
	struct Range { uint32_t gen = 0, begin = 0, end = 0; };
	Range at(size_t cell) const {
		const Range& r = range_[cell];
		return r.gen == gen_ ? r : Range{};
	}
	uint32_t xWidth_ = 0, yWidth_ = 0, gen_ = 0;
	std::vector<Range> range_;
	std::vector<uint64_t> keys_;
	std::vector<uint32_t> order_;
	std::vector<uint32_t> occupied_;
};

} // namespace mc2::sim
