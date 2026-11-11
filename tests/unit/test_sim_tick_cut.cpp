// SIM-TICK-CUT: the terrain tick list and the occupied-cell collision walk must reproduce the legacy full scans
// exactly (same visits, same order), including flags raised while the walk is in progress.
#include "doctest.h"
#include "terrain_tick_list.h"
#include "collision_cell_walk.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace {
struct Rng {
	uint32_t s;
	uint32_t next() { s = s * 1664525u + 1013904223u; return s >> 8; }
};

struct FakeObject {
	bool exists = true;
	bool staticClass = false;   // TREE / TERRAINOBJECT
	bool falling = false;
	bool justCreated = false;
	bool special = false;       // also in the special-building / gate lists (already advanced)
};

// The legacy scan, verbatim in shape: every handle of every block in order.
template <class OnAdvance>
void legacyScan(std::vector<FakeObject>& objs, const std::vector<std::pair<int32_t, int32_t>>& blocks, OnAdvance&& advance) {
	for (const auto& b : blocks)
		for (int32_t i = 0; i < b.second; ++i) {
			auto& o = objs[static_cast<size_t>(b.first + i)];
			if (!o.exists) continue;
			if (o.staticClass && !o.falling && !o.justCreated) continue;
			if (o.special) continue;
			advance(b.first + i);
		}
}
}

TEST_CASE("sim_tick_cut/terrain tick list visits what the full scan visits, in order, across ticks")
{
	for (uint32_t seed = 1; seed <= 20; ++seed) {
		Rng rng{seed};
		const int32_t total = 400;
		std::vector<FakeObject> a(total + 1), b;
		std::vector<std::pair<int32_t, int32_t>> blocks;
		for (int32_t h = 1; h <= total; ++h) {
			a[h].staticClass = rng.next() % 4 != 0;
			a[h].exists = rng.next() % 16 != 0;
			a[h].justCreated = rng.next() % 5 == 0;
			a[h].special = !a[h].staticClass && rng.next() % 10 == 0;
		}
		// blocks leave holes (handles outside any block must never tick)
		for (int32_t h = 1; h <= total;) {
			const int32_t n = 1 + static_cast<int32_t>(rng.next() % 17);
			if (rng.next() % 6 != 0) blocks.emplace_back(h, std::min(n, total - h + 1));
			h += n + static_cast<int32_t>(rng.next() % 3);
		}
		b = a;
		mc2::sim::TerrainTickList list;
		const auto classify = [&](int32_t h) {
			const auto& o = b[h];
			if (!o.staticClass) return 1;
			return (o.falling || o.justCreated) ? 2 : 3;
		};
		list.rebuild(blocks, total, classify, [&](int32_t h) { return b[h].special; });

		for (int tick = 0; tick < 60; ++tick) {
			// An advanced object may raise FALLING on another object (any handle, before or after it);
			// both worlds get the same deterministic raise, the list world through insert().
			std::vector<int32_t> seqA, seqB;
			Rng ra{seed * 977u + static_cast<uint32_t>(tick)}, rb = ra;
			const auto stepWorld = [&](std::vector<FakeObject>& w, Rng& r, std::vector<int32_t>& seq, bool useList, int32_t h) {
				seq.push_back(h);
				w[h].justCreated = false;                              // update() clears JUSTCREATED
				if (r.next() % 9 == 0) {                               // and may topple a tree somewhere
					const int32_t victim = 1 + static_cast<int32_t>(r.next() % total);
					if (w[victim].staticClass && !w[victim].falling) {
						w[victim].falling = true;
						if (useList) list.insert(victim, w[victim].special);
					}
				}
				if (w[h].falling && r.next() % 4 == 0) w[h].falling = false;   // fallen
			};
			legacyScan(a, blocks, [&](int32_t h) { stepWorld(a, ra, seqA, false, h); });
			bool prune = false;
			list.walk([&](const mc2::sim::TerrainTickList::Entry& e) {
				auto& o = b[e.handle];
				if (!o.exists) return;
				if (o.staticClass && !o.falling && !o.justCreated) { prune = true; return; }
				if (e.duplicate) return;
				stepWorld(b, rb, seqB, true, e.handle);
			});
			if (prune)
				list.pruneConditional([&](int32_t h) { const auto& o = b[h]; return !(o.staticClass && !o.falling && !o.justCreated); });
			REQUIRE(seqA == seqB);
			// the quiet toppling the loop above cannot cause: raise some flags between ticks too
			if (tick % 7 == 3) {
				const int32_t victim = 1 + static_cast<int32_t>(rng.next() % total);
				if (a[victim].staticClass) {
					a[victim].falling = b[victim].falling = true;
					list.insert(victim, b[victim].special);
				}
			}
		}
	}
}

TEST_CASE("sim_tick_cut/terrain tick list ignores inserts before a build and outside every block")
{
	mc2::sim::TerrainTickList list;
	list.insert(3, false);
	CHECK(list.entries().empty());
	std::vector<std::pair<int32_t, int32_t>> blocks{{1, 4}};
	list.rebuild(blocks, 10, [](int32_t) { return 3; }, [](int32_t) { return false; });
	CHECK(list.built());
	list.insert(8, false);   // outside the block
	CHECK(list.entries().empty());
	list.insert(3, false);
	list.insert(3, false);   // already present
	REQUIRE(list.entries().size() == 1);
	list.invalidate();
	CHECK_FALSE(list.built());
}

TEST_CASE("sim_tick_cut/occupied cell walk gives the dense walk's pairs in the same order")
{
	for (uint32_t seed = 1; seed <= 30; ++seed) {
		Rng rng{seed};
		const uint32_t xw = 3 + rng.next() % 9, yw = 2 + rng.next() % 9;
		const size_t n = rng.next() % 120;
		std::vector<mc2::sim::OccupiedCellWalk::Item> items(n);
		for (auto& it : items) it.cell = rng.next() % (xw * yw);

		// Dense reference (the legacy CSR walk): cells row-major, newest object first within a cell.
		const size_t cells = static_cast<size_t>(xw) * yw;
		std::vector<std::vector<uint32_t>> bucket(cells);
		for (size_t k = n; k-- > 0;) bucket[items[k].cell].push_back(static_cast<uint32_t>(k));
		std::vector<std::pair<uint32_t, uint32_t>> expected;
		const auto pairWith = [&](uint32_t obj, const std::vector<uint32_t>& cell, size_t from) {
			for (size_t i = from; i < cell.size(); ++i) expected.emplace_back(obj, cell[i]);
		};
		for (uint32_t y = 0; y < yw; ++y)
			for (uint32_t x = 0; x < xw; ++x) {
				const uint32_t c = x + y * xw;
				for (size_t g = 0; g < bucket[c].size(); ++g) {
					const uint32_t obj = bucket[c][g];
					pairWith(obj, bucket[c], g + 1);
					if (x < xw - 1) {
						pairWith(obj, bucket[c + 1], 0);
						if (y < yw - 1) pairWith(obj, bucket[c + 1 + xw], 0);
					}
					if (y < yw - 1) pairWith(obj, bucket[c + xw], 0);
				}
			}

		mc2::sim::OccupiedCellWalk walk;
		for (int rebuild = 0; rebuild < 3; ++rebuild) {   // scratch reuse across ticks must not leak stale cells
			walk.build(items.data(), items.size(), xw, yw);
			const auto& order = walk.order();
			std::vector<std::pair<uint32_t, uint32_t>> got;
			walk.walk([](uint32_t) {}, [&](uint32_t slot, uint32_t first, uint32_t last) {
				for (uint32_t s = first; s != last; ++s) got.emplace_back(order[slot], order[s]);
			});
			REQUIRE(got == expected);
			if (rebuild == 0)   // next tick: a different occupancy on the same grid
				for (auto& it : items) it.cell = (it.cell * 7 + 3) % (xw * yw);
			if (rebuild == 0) {
				// recompute the dense reference for the permuted placement
				bucket.assign(cells, {});
				for (size_t k = n; k-- > 0;) bucket[items[k].cell].push_back(static_cast<uint32_t>(k));
				expected.clear();
				for (uint32_t y = 0; y < yw; ++y)
					for (uint32_t x = 0; x < xw; ++x) {
						const uint32_t c = x + y * xw;
						for (size_t g = 0; g < bucket[c].size(); ++g) {
							const uint32_t obj = bucket[c][g];
							pairWith(obj, bucket[c], g + 1);
							if (x < xw - 1) {
								pairWith(obj, bucket[c + 1], 0);
								if (y < yw - 1) pairWith(obj, bucket[c + 1 + xw], 0);
							}
							if (y < yw - 1) pairWith(obj, bucket[c + xw], 0);
						}
					}
			}
		}
	}
}
