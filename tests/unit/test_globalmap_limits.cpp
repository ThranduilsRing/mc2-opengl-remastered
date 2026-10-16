// tests/unit/test_globalmap_limits.cpp
//
// WHAT: the range checks a global move map applies to every count it reads from a .pak (mclib/globalmap_limits.h).
// WHY: those counts are mod content. A malformed pak must be refused before a count sizes a buffer or walks a table; each count
// has its own case so a limit cannot be loosened or dropped unseen.

#include "doctest.h"

#include "globalmap_limits.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gm = mc2::globalmap;

namespace
{
gm::HeaderCounts stock ()
{
	gm::HeaderCounts c;
	c.height = 300;
	c.width = 300;
	c.sectorHeight = 10;
	c.sectorWidth = 10;
	c.numAreas = 200;
	c.numDoors = 400;
	c.numDoorInfos = 800;
	c.numDoorLinks = 5000;
	return c;
}
}

TEST_CASE("a header with in-range counts is accepted, including the limits themselves")
{
	CHECK(gm::rejectHeaderCounts(stock()) == nullptr);

	gm::HeaderCounts top = stock();
	top.height = gm::kMaxCellsPerSide;
	top.width = gm::kMaxCellsPerSide;
	top.numAreas = gm::kMaxAreas;
	top.numDoors = gm::kMaxDoors;
	top.numDoorInfos = gm::kMaxAreas * gm::kMaxDoorsPerArea;
	top.numDoorLinks = (gm::kMaxDoors + 2) * 2 * gm::kMaxLinksPerDoorSide;
	CHECK(gm::rejectHeaderCounts(top) == nullptr);
}

TEST_CASE("each header count outside its range is named and refused")
{
	struct Case { const char* name; void (*bad)(gm::HeaderCounts&); };
	const Case cases[] = {
		{"height", [](gm::HeaderCounts& c) { c.height = 0; }},
		{"height", [](gm::HeaderCounts& c) { c.height = gm::kMaxCellsPerSide + 1; }},
		{"width", [](gm::HeaderCounts& c) { c.width = -1; }},
		{"width", [](gm::HeaderCounts& c) { c.width = 1 << 20; }},
		{"sectorHeight", [](gm::HeaderCounts& c) { c.sectorHeight = -5; }},
		{"sectorHeight", [](gm::HeaderCounts& c) { c.sectorHeight = gm::kMaxCellsPerSide + 1; }},
		{"sectorWidth", [](gm::HeaderCounts& c) { c.sectorWidth = -5; }},
		{"sectorWidth", [](gm::HeaderCounts& c) { c.sectorWidth = gm::kMaxCellsPerSide + 1; }},
		{"numAreas", [](gm::HeaderCounts& c) { c.numAreas = -1; }},
		{"numAreas", [](gm::HeaderCounts& c) { c.numAreas = gm::kMaxAreas + 1; }},
		{"numDoors", [](gm::HeaderCounts& c) { c.numDoors = -1; }},
		{"numDoors", [](gm::HeaderCounts& c) { c.numDoors = gm::kMaxDoors + 1; }},
		{"numDoorInfos", [](gm::HeaderCounts& c) { c.numDoorInfos = -1; }},
		{"numDoorInfos", [](gm::HeaderCounts& c) { c.numDoorInfos = gm::kMaxAreas * gm::kMaxDoorsPerArea + 1; }},
		{"numDoorLinks", [](gm::HeaderCounts& c) { c.numDoorLinks = -1; }},
		{"numDoorLinks", [](gm::HeaderCounts& c) { c.numDoorLinks = (gm::kMaxDoors + 2) * 2 * gm::kMaxLinksPerDoorSide + 1; }},
		{"numDoorLinks", [](gm::HeaderCounts& c) { c.numDoorLinks = 0x7fffffff; }},
	};
	for (const Case& tc : cases)
	{
		gm::HeaderCounts c = stock();
		tc.bad(c);
		const char* named = gm::rejectHeaderCounts(c);
		REQUIRE(named != nullptr);
		CHECK(std::string(named) == tc.name);
	}
}

TEST_CASE("per-area and per-door-side counts are range checked")
{
	CHECK(gm::areaDoorCountInRange(0));
	CHECK(gm::areaDoorCountInRange(gm::kMaxDoorsPerArea));
	CHECK_FALSE(gm::areaDoorCountInRange(-1));
	CHECK_FALSE(gm::areaDoorCountInRange(gm::kMaxDoorsPerArea + 1));
	CHECK_FALSE(gm::areaDoorCountInRange(32767));

	CHECK(gm::doorLinkCountInRange(0));
	CHECK(gm::doorLinkCountInRange(gm::kMaxDoorsPerArea));
	CHECK_FALSE(gm::doorLinkCountInRange(-1));
	CHECK_FALSE(gm::doorLinkCountInRange(gm::kMaxDoorsPerArea + 1));
}

TEST_CASE("a packet larger than the struct array is tolerated within the slack and refused beyond it")
{
	CHECK(gm::tolerantBytes(100, 100) == 100);
	CHECK(gm::tolerantBytes(50, 100) == 100);
	CHECK(gm::tolerantBytes(200, 100) == 200);
	CHECK(gm::tolerantBytes(100 * gm::kMaxPacketSlack, 100) == 100 * gm::kMaxPacketSlack);
	CHECK(gm::tolerantBytes(100 * gm::kMaxPacketSlack + 4097, 100) == 0);
	CHECK(gm::tolerantBytes(std::size_t(1) << 40, 360000) == 0);
	CHECK(gm::tolerantBytes(8192, 0) == 0);
}

TEST_CASE("ownZeroed gives a zeroed non-null view even for a count of zero")
{
	std::vector<int16_t> store;
	int16_t* view = gm::ownZeroed(store, 4);
	REQUIRE(view != nullptr);
	CHECK(store.size() == 4);
	for (int i = 0; i < 4; ++i)
		CHECK(view[i] == 0);
	view[2] = 7;

	view = gm::ownZeroed(store, 0);
	REQUIRE(view != nullptr);
	CHECK(store.size() == 1);
	CHECK(view[0] == 0);

	std::vector<int16_t*> pointers;
	int16_t** ptrView = gm::ownZeroed(pointers, 3);
	CHECK(ptrView[0] == nullptr);
	CHECK(ptrView[2] == nullptr);
}
