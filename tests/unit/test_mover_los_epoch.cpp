#include "doctest.h"
#include "mover_los_epoch.h"
#include <vector>

TEST_CASE("mover LOS epoch table")
{
	std::vector<uint16_t> cells(16, 0);
	uint16_t epoch = 0;
	mc2::losEpochAdvance(cells.data(), cells.size(), epoch);
	CHECK(epoch == 1);
	CHECK(mc2::losEpochState(cells[3], epoch) == 0);
	cells[3] = mc2::losEpochCell(epoch, true);
	cells[4] = mc2::losEpochCell(epoch, false);
	CHECK(mc2::losEpochState(cells[3], epoch) == 2);
	CHECK(mc2::losEpochState(cells[4], epoch) == 1);
	mc2::losEpochAdvance(cells.data(), cells.size(), epoch);
	CHECK(mc2::losEpochState(cells[3], epoch) == 0);
	CHECK(mc2::losEpochState(cells[4], epoch) == 0);
	// wrap: a stale cell must never alias the new epoch
	cells[5] = mc2::losEpochCell(epoch, true);
	epoch = mc2::kLosEpochLimit - 1;
	mc2::losEpochAdvance(cells.data(), cells.size(), epoch);
	CHECK(epoch == 1);
	CHECK(mc2::losEpochState(cells[5], epoch) == 0);
}
