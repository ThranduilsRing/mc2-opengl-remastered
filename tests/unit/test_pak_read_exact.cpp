// tests/unit/test_pak_read_exact.cpp
//
// WHAT: mc2::pak::readExact, the size-checked packet read of the mission map and global move maps.
// WHY: those paks are mod content. A malformed one (a packet of the wrong size, or a missing packet) must be rejected with one
// log line and leave the caller's field untouched, never be copied whole into a fixed-size field and never STOP.

#include "doctest.h"

#include "packet.h"
#include "packet_exact.h"

#include "../../GameOS/gameos/utils/mc2log.h"

#include <cstdint>
#include <cstdio>
#include <string>

extern bool g_mc2TestStopFatal;   // tests/unit/heap_shim.cpp: STOP counts and continues instead of aborting
extern int  g_mc2TestStopCount;

namespace
{

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

size_t countOf (const std::string& text, const char* needle)
{
	size_t count = 0;
	for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
		++count;
	return count;
}

} // namespace

TEST_CASE("readExact accepts an exact packet and rejects a wrong-size or missing one with one log line each")
{
	const std::string path = "pak_read_exact_scratch.pak";
	{
		PacketFile out;
		REQUIRE(out.create(path.c_str()) == NO_ERR);
		out.reserve(2);
		int32_t height = 120;
		unsigned char shortPacket[2] = {7, 7};
		REQUIRE(out.writePacket(0, reinterpret_cast<MemoryPtr>(&height), sizeof(height)) > 0);
		REQUIRE(out.writePacket(1, shortPacket, sizeof(shortPacket)) > 0);
		out.close();
	}

	g_mc2TestStopFatal = false;
	g_mc2TestStopCount = 0;
	std::FILE* capture = std::tmpfile();
	REQUIRE(capture != nullptr);
	mc2::log::setSpec("*:info");
	mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, capture);

	PacketFile in;
	REQUIRE(in.open(path.c_str()) == NO_ERR);

	int32_t value = -1;
	CHECK(mc2::pak::readExact(in, 0, &value, sizeof(value), "height"));
	CHECK(value == 120);

	value = -1;
	CHECK_FALSE(mc2::pak::readExact(in, 1, &value, sizeof(value), "width"));   // 2 bytes where 4 are expected
	CHECK(value == -1);

	CHECK_FALSE(mc2::pak::readExact(in, 2, &value, sizeof(value), "planet"));  // the file holds packets 0 and 1 only
	CHECK_FALSE(mc2::pak::readExact(in, -1, &value, sizeof(value), "planet"));
	CHECK(value == -1);
	in.close();

	const std::string logged = captured(capture);
	mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, nullptr);
	std::fclose(capture);
	CHECK(g_mc2TestStopCount == 0);
	g_mc2TestStopFatal = true;

	CHECK(countOf(logged, "width packet 1 is 2 bytes, expected 4") == 1);
	CHECK(countOf(logged, "planet packet 2 is -1 bytes") == 1);
	CHECK(countOf(logged, "planet packet -1 is -1 bytes") == 1);
	CHECK(countOf(logged, "height packet") == 0);

	std::remove(path.c_str());
}
