#include "doctest.h"
#include "mover_los_epoch.h"
#include <vector>
#include "goal_area_memo.h"
#include <random>

namespace {
struct GoalProvider {
	std::array<std::array<unsigned char, 70>, 70> table{};
	std::vector<std::array<long, 4>> log;
	unsigned reads = 0;
	unsigned char getPathExists(long from, long to) { ++reads; return table[from][to]; }
	void setPathExists(long from, long to, unsigned char value) {
		log.push_back({1, from, to, value});
		table[from][to] = value;
		// Deliberately flip a different cached area on every write.
		table[0][(to + 1) % 70] = (table[0][(to + 1) % 70] + 1) % 3;
	}
	std::vector<long> calcPath(long area) {
		log.push_back({0, 0, area, 0});
		return area % 3 == 0 ? std::vector<long>{} : std::vector<long>{0, area, (area + 1) % 70};
	}
};
auto goalScan(GoalProvider& source, const std::vector<long>& grid, bool memoized, bool valid) {
	mc2::GoalAreaMemo memo(source, 0);
	auto get = [&](long area) { return memoized ? memo.get(area) : source.getPathExists(0, area); };
	auto set = [&](long from, long to, unsigned char value) {
		if (memoized) memo.set(from, to, value); else source.setPathExists(from, to, value);
	};
	std::vector<int> cells;
	for (long area : grid) {
		int weight = 123;
		if (area != 0 && area != -1) {
			if (get(area) == 0) {
				if (valid) {
					auto path = source.calcPath(area);
					if (!path.empty()) {
						for (long from : path) for (long to : path) set(from, to, 1);
					} else { set(area, 0, 2); set(0, area, 2); }
				} else set(0, area, 2);
			}
			if (get(area) == 2) weight -= 50000;
		}
		cells.push_back(weight);
	}
	return cells;
}
}

TEST_CASE("goal area memo differential seeded grids")
{
	std::mt19937 random(0x53494d);
	for (int seed = 0; seed < 512; ++seed) {
		GoalProvider plain;
		for (auto& row : plain.table) for (auto& value : row) value = random() % 3;
		auto cached = plain;
		std::vector<long> grid{1, 2, 1, 65, 1, -1, 0, 3};
		for (int cell = 0; cell < 256; ++cell) grid.push_back(random() % 10);
		CHECK(goalScan(plain, grid, false, seed % 2) == goalScan(cached, grid, true, seed % 2));
		CHECK(plain.log == cached.log);
		CHECK(plain.table == cached.table);
		CHECK(cached.reads < plain.reads);
	}
}

TEST_CASE("mover LOS epoch table")
{
	std::vector<uint16_t> cells(16, 0);
	uint16_t epoch = 0;
	mc2::losEpochAdvance(cells, epoch);
	CHECK(epoch == 1);
	CHECK(mc2::losEpochState(cells[3], epoch) == 0);
	cells[3] = mc2::losEpochCell(epoch, true);
	cells[4] = mc2::losEpochCell(epoch, false);
	CHECK(mc2::losEpochState(cells[3], epoch) == 2);
	CHECK(mc2::losEpochState(cells[4], epoch) == 1);
	mc2::losEpochAdvance(cells, epoch);
	CHECK(mc2::losEpochState(cells[3], epoch) == 0);
	CHECK(mc2::losEpochState(cells[4], epoch) == 0);
	// wrap: a stale cell must never alias the new epoch
	cells[5] = mc2::losEpochCell(epoch, true);
	epoch = mc2::kLosEpochLimit - 1;
	mc2::losEpochAdvance(cells, epoch);
	CHECK(epoch == 1);
	CHECK(mc2::losEpochState(cells[5], epoch) == 0);
}
