// SIM-TICK-CUT: ordered list of the terrain-block objects that can tick, replacing the per-tick scan
// of every block object. Pure logic (no engine types) so a doctest can compare it with the full scan.
//
// The legacy scan visits every handle of every block in handle order and advances an object unless it is a
// static TREE/TERRAINOBJECT with neither FALLING nor JUSTCREATED set. This list keeps exactly the handles
// that scan could advance: every non-static object ("always"), plus static-class objects while one of those
// two flags is set ("conditional", entered through insert() when the flag is raised, dropped by
// pruneConditional() once neither flag is set). Handles stay ascending, so the visit order is the scan order.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mc2::sim {

class TerrainTickList {
public:
	struct Entry {
		int32_t handle;
		bool conditional;   // static class kept only while FALLING or JUSTCREATED holds
		bool duplicate;     // already advanced earlier in the tick by the special-building / gate lists
	};

	bool built() const { return built_; }
	void invalidate() { built_ = false; }

	// blocks: (firstHandle, numObjects) per block; classify(handle) -> 0 skip (absent), 1 always,
	// 2 static class and currently flagged (conditional), 3 static class and not flagged.
	// duplicate(handle) -> the object is in the special-building or gate list.
	template <class Blocks, class Classify, class Duplicate>
	void rebuild(const Blocks& blocks, int32_t maxHandle, Classify&& classify, Duplicate&& duplicate) {
		entries_.clear();
		covered_.assign(static_cast<size_t>(std::max<int32_t>(maxHandle, 0)) + 1, 0);
		for (const auto& b : blocks) {
			for (int32_t i = 0; i < b.second; ++i) {
				const int32_t h = b.first + i;
				if (h < 0 || h >= static_cast<int32_t>(covered_.size())) continue;
				covered_[static_cast<size_t>(h)] = 1;
				const int kind = classify(h);
				if (kind == 1 || kind == 2)
					entries_.push_back({h, kind == 2, duplicate(h)});
			}
		}
		// Blocks are in ascending handle order for every load path; keep the order total regardless.
		if (!std::is_sorted(entries_.begin(), entries_.end(), byHandle)) {
			std::stable_sort(entries_.begin(), entries_.end(), byHandle);
		}
		built_ = true;
	}

	// A static-class object inside a block gained FALLING or JUSTCREATED. Safe during walk().
	void insert(int32_t handle, bool duplicate) {
		if (!built_ || handle < 0 || handle >= static_cast<int32_t>(covered_.size()) || !covered_[static_cast<size_t>(handle)]) return;
		auto it = std::lower_bound(entries_.begin(), entries_.end(), handle,
			[](const Entry& e, int32_t h) { return e.handle < h; });
		if (it != entries_.end() && it->handle == handle) return;
		entries_.insert(it, {handle, true, duplicate});
	}

	// Visit entries in handle order. visit(entry) may call insert(); an entry added at a later handle is
	// visited this pass (the scan would reach it), one at or before the current handle waits for the next.
	template <class Visit>
	void walk(Visit&& visit) {
		size_t i = 0;
		while (i < entries_.size()) {
			const Entry cur = entries_[i];
			visit(cur);
			if (i >= entries_.size() || entries_[i].handle != cur.handle) {
				i = static_cast<size_t>(std::lower_bound(entries_.begin(), entries_.end(), cur.handle,
					[](const Entry& e, int32_t h) { return e.handle < h; }) - entries_.begin());
			}
			++i;
		}
	}

	// Drop conditional entries the predicate reports as no longer active (call after walk()).
	template <class Active>
	void pruneConditional(Active&& stillActive) {
		entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
			[&](const Entry& e) { return e.conditional && !stillActive(e.handle); }), entries_.end());
	}

	const std::vector<Entry>& entries() const { return entries_; }

private:
	static bool byHandle(const Entry& a, const Entry& b) { return a.handle < b.handle; }
	std::vector<Entry> entries_;
	std::vector<uint8_t> covered_;
	bool built_ = false;
};

} // namespace mc2::sim
