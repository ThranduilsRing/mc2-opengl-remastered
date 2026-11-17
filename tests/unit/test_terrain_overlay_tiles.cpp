// tests/unit/test_terrain_overlay_tiles.cpp
// WHAT: mclib/terrain_overlay_tiles.h, the used-tile derivation of the terrain overlay loader.
#include "doctest.h"
#include "terrain_overlay_tiles.h"
#include <set>
#include <utility>
#include <vector>

TEST_CASE("overlay handles map to their tile, the unmatched handle keeps the legacy (0,0) fallback")
{
	const std::vector<long> base{ 100, 110, 130 }, count{ 10, 20, 5 };
	auto at = [&](long h) { return mc2::overlayTileForHandle(base, count, h); };
	CHECK(at(100).overlay == 0); CHECK(at(100).tile == 0);
	CHECK(at(109).overlay == 0); CHECK(at(109).tile == 9);
	CHECK(at(110).overlay == 1); CHECK(at(110).tile == 0);
	CHECK(at(134).overlay == 2); CHECK(at(134).tile == 4);
	CHECK(at(135).overlay == 0); CHECK(at(135).tile == 0);
	CHECK(at(5).overlay == 0);   CHECK(at(5).tile == 0);
}

TEST_CASE("used set is the distinct tiles of the combined handles only")
{
	const std::vector<long> base{ 100, 300 }, count{ 10, 20 };
	std::set<std::pair<long, long>> used;
	for (unsigned long h : { 0xffffUL, 101UL, 101UL, 305UL, 290UL, 0xffffUL })
		if (mc2::overlayHandleCombined(h, true)) {
			const auto r = mc2::overlayTileForHandle(base, count, static_cast<long>(h));
			used.insert({ r.overlay, r.tile });
		}
	CHECK(used == std::set<std::pair<long, long>>{ { 0, 1 }, { 1, 5 } });
	CHECK(mc2::overlayHandleCombined(290, false));
	CHECK_FALSE(mc2::overlayHandleCombined(290, true));
	CHECK_FALSE(mc2::overlayHandleCombined(0xffff, false));
}
