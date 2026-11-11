// tests/unit/test_globalmap_init_refusal.cpp
//
// WHAT: GlobalMap::init on a pak whose header counts are out of range: the map is refused, and the packet count it returns still
// steps the caller's cursor over that map's packets.
// WHY: MOVE_readData reads the second and third global maps from the cursor the first returns. A refused map that returned the
// wrong count would misread every map after it, and no helper-only test reaches the return value.

#include "doctest.h"

#include "heap.h"
#include "move.h"
#include "packet.h"

#include "../../GameOS/gameos/utils/mc2log.h"

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace
{
constexpr int32_t kVersion = 0x022523;
constexpr int kHeaderPackets = 10;

struct Header
{
	int32_t height, width, sectorHeight, sectorWidth, numAreas, numDoors, numDoorInfos, numDoorLinks;
};

// Restores the process-wide test switches this case changes, so a failed REQUIRE cannot leak them into later cases.
struct TestStateGuard
{
	std::FILE* capture = nullptr;
	std::string path;

	~TestStateGuard ()
	{
		mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, nullptr);
		if (capture)
			std::fclose(capture);
		if (!path.empty())
			std::remove(path.c_str());
	}
};

std::string captured (std::FILE* capture)
{
	std::fflush(capture);
	std::rewind(capture);
	std::string text;
	char buf[512];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof buf, capture)) > 0)
		text.append(buf, n);
	return text;
}

void writeInt (PacketFile& out, int packet, int32_t value)
{
	REQUIRE(out.writePacket(packet, reinterpret_cast<MemoryPtr>(&value), sizeof(value)) > 0);
}

// The ten header packets of one global map starting at `first`.
void writeHeader (PacketFile& out, int first, const Header& h)
{
	writeInt(out, first + 0, kVersion);
	writeInt(out, first + 1, h.height);
	writeInt(out, first + 2, h.width);
	writeInt(out, first + 3, 30);
	writeInt(out, first + 4, h.sectorHeight);
	writeInt(out, first + 5, h.sectorWidth);
	writeInt(out, first + 6, h.numAreas);
	writeInt(out, first + 7, h.numDoors);
	writeInt(out, first + 8, h.numDoorInfos);
	writeInt(out, first + 9, h.numDoorLinks);
}
} // namespace

TEST_CASE("GlobalMap::init refuses an out-of-range header and returns the map's layout size so the next map is found")
{
	TestStateGuard guard;
	guard.path = (std::filesystem::temp_directory_path() / "mc2_globalmap_init_refusal_scratch.pak").string();

	// Map A claims 7 door infos and 50 doors: it occupies 13 + 7 + (50 + 2) * 2 = 124 packets. Its link count is far beyond what
	// 50 doors can have. Map B starts at packet 124 with a height of zero.
	const Header a{300, 300, 10, 10, 1, 50, 7, 1000000};
	const Header b{0, 300, 10, 10, 1, 50, 7, 100};
	const int mapBStart = 13 + 7 + (50 + NUM_DOOR_OFFSETS) * 2;
	REQUIRE(mapBStart == 124);
	{
		PacketFile out;
		REQUIRE(out.create(guard.path.c_str()) == NO_ERR);
		out.reserve(mapBStart + kHeaderPackets);
		for (int p = 0; p < mapBStart + kHeaderPackets; ++p)
			writeInt(out, p, 0);
		writeHeader(out, 0, a);
		writeHeader(out, mapBStart, b);
		out.close();
	}

	std::FILE* capture = std::tmpfile();
	REQUIRE(capture != nullptr);
	guard.capture = capture;
	mc2::log::setSpec("*:info");
	mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, capture);

	PacketFile in;
	REQUIRE(in.open(guard.path.c_str()) == NO_ERR);

	std::unique_ptr<GlobalMap> mapA(new GlobalMap);
	CHECK(mapA->init(&in, 0) == mapBStart);
	CHECK(mapA->badLoad);
	CHECK(mapA->numAreas == 0);
	CHECK(mapA->numDoors == 0);

	std::unique_ptr<GlobalMap> mapB(new GlobalMap);
	CHECK(mapB->init(&in, mapBStart) == 13 + 7 + (50 + NUM_DOOR_OFFSETS) * 2);
	CHECK(mapB->badLoad);
	in.close();

	const std::string logged = captured(capture);
	CHECK(logged.find("at packet 0: numDoorLinks is out of range") != std::string::npos);
	CHECK(logged.find("at packet 124: height is out of range") != std::string::npos);
}

TEST_CASE("GlobalMap::init falls back to the packets it read when the counts describe no layout")
{
	TestStateGuard guard;
	guard.path = (std::filesystem::temp_directory_path() / "mc2_globalmap_init_negative_scratch.pak").string();
	{
		PacketFile out;
		REQUIRE(out.create(guard.path.c_str()) == NO_ERR);
		out.reserve(kHeaderPackets);
		writeHeader(out, 0, Header{300, 300, 10, 10, 1, -4, 7, 0});
		out.close();
	}

	std::FILE* capture = std::tmpfile();
	REQUIRE(capture != nullptr);
	guard.capture = capture;
	mc2::log::setSpec("*:info");
	mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, capture);

	PacketFile in;
	REQUIRE(in.open(guard.path.c_str()) == NO_ERR);
	std::unique_ptr<GlobalMap> map(new GlobalMap);
	CHECK(map->init(&in, 0) == kHeaderPackets);
	CHECK(map->badLoad);
	in.close();
	CHECK(captured(capture).find("numDoors is out of range") != std::string::npos);
}

TEST_CASE("save fields: global map load retains active door link costs")
{
	TestStateGuard guard;
	guard.path = (std::filesystem::temp_directory_path() / "mc2_globalmap_link_save.pak").string();
	{
		PacketFile out;
		REQUIRE(out.create(guard.path.c_str()) == NO_ERR);
		out.reserve(21);
		writeHeader(out, 0, Header{30, 30, 1, 1, 2, 1, 2, 14});
		short cells[900]{};
		GlobalMapArea areas[2]{};
		for (auto& area : areas) {
			area.numDoors = 1;
			area.teamID = -1;
			area.open = true;
		}
		GlobalMapDoor doors[3]{};
		doors[0].area[1] = 1;
		doors[0].open = true;
		doors[0].numLinks[0] = doors[0].numLinks[1] = 1;
		DoorInfo info[2]{{0, 0}, {0, 1}};
		auto write = [&](int packet, auto& value) {
			REQUIRE(out.writePacket(packet, reinterpret_cast<MemoryPtr>(&value), sizeof(value)) > 0);
		};
		write(10, cells);
		write(11, areas);
		write(12, info[0]);
		write(13, info[1]);
		write(14, doors);
		DoorLink links[3]{};
		links[0].cost = 1000;
		links[0].openCost = 10;
		write(15, links);
		write(16, links);
		DoorLink scratch[2]{};
		for (int packet = 17; packet < 21; ++packet)
			write(packet, scratch);
	}
	PacketFile in;
	REQUIRE(in.open(guard.path.c_str()) == NO_ERR);
	auto map = std::make_unique<GlobalMap>();
	REQUIRE(map->init(&in, 0, true) == 21);
	REQUIRE_FALSE(map->badLoad);
	CHECK(map->doors_links[0][0][0].cost == 1000);
	CHECK(map->doors_links[0][1][0].cost == 1000);
	CHECK(map->doors_links[0][0][0].openCost == 10);
	// Mission assets still normalize the editor's link costs at initial load.
	auto fresh = std::make_unique<GlobalMap>();
	REQUIRE(fresh->init(&in, 0) == 21);
	REQUIRE_FALSE(fresh->badLoad);
	CHECK(fresh->doors_links[0][0][0].cost == 10);
	CHECK(fresh->doors_links[0][1][0].cost == 10);
}

TEST_CASE("GlobalMap packed reachability decodes every slot and preserves other cells")
{
	// Seven areas cross a byte boundary; three bytes per row include padding.
	struct Table {
		unsigned char bytes[21]{};
		GlobalMap map;
		Table() { map.numAreas = 7; map.pathExistsTable = bytes; }
		~Table() { map.pathExistsTable = nullptr; }
	} table;
	const unsigned char values[]{GLOBALPATH_EXISTS_TRUE, GLOBALPATH_EXISTS_FALSE, GLOBALPATH_EXISTS_UNKNOWN};
	for (long row = 0; row < 7; ++row) {
		for (long col = 0; col < 7; ++col) {
			// Each cell cycles TRUE -> FALSE -> UNKNOWN. Snapshot the raw bytes
			// too: decoded neighbors alone would miss writes into row padding.
			for (const auto value : values) {
				unsigned char before[21];
				std::copy_n(table.bytes, 21, before);
				table.map.setPathExists(row, col, value);
				CHECK(table.map.getPathExists(row, col) == value);
				const long byte = row * 3 + col / 4;
				const unsigned char mask = static_cast<unsigned char>(3 << ((col % 4) * 2));
				for (long i = 0; i < 21; ++i) {
					if (i == byte)
						CHECK((table.bytes[i] & ~mask) == (before[i] & ~mask));
					else
						CHECK(table.bytes[i] == before[i]);
				}
			}
			// Leave a nonzero neighbor for the next cell's preservation checks.
			table.map.setPathExists(row, col, GLOBALPATH_EXISTS_FALSE);
		}
	}
	for (long row = 0; row < 7; ++row)
		for (long col = 0; col < 7; ++col)
			CHECK(table.map.getPathExists(row, col) == GLOBALPATH_EXISTS_FALSE);
}
