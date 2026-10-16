// tests/unit/test_file_family_contract.cpp
//
// What the File family promises to its callers, pinned before the conversion that moved the name, the child list, the in-RAM image and the seek
// tables into standard containers. Every case here passes on the old classes and must keep passing, unchanged, after the conversion; the cases that
// need NEW behaviour (malformed packet headers, an empty fit file) arrive with the commit that makes them true.
//
// WHAT each case pins and WHY a conversion could break it:
//   FC1 the name is null while closed and the opened path while open (mech_loadout.cpp tests getFilename() for null).
//   FC2 a child reads its parent's bytes at the parent's position, reports the parent's name, and is closed by the parent's close (the owner of a
//       packet file closes it without closing the fit views it opened on its packets first).
//   FC3 closing a file opened on a caller's buffer never frees or alters that buffer (the in-RAM image is owned only for FastFile and child files).
//   FC4 a closed file can be opened again and reports the new name (a loader object is reused across missions).
//   FC5 many children: the child list holds the default 50 and a child that closes itself leaves its slot free for the next one.

#include "doctest.h"

#include "file.h"
#include "inifile.h"
#include "packet.h"
#include "ablscan.h"

#include "../../GameOS/gameos/utils/mc2log.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <climits>

extern bool g_mc2TestStopFatal;   // tests/unit/heap_shim.cpp: STOP counts and continues instead of aborting
extern int  g_mc2TestStopCount;

namespace
{

// Lowercase on purpose: File::open lowercases a path before it reaches the OS.
std::string scratch (const char *name)
{
	return std::string("ffc_scratch_") + name;
}

void writeBytes (const std::string &path, const char *bytes, size_t n)
{
	File out;
	REQUIRE(out.create(path.c_str()) == NO_ERR);
	REQUIRE(out.write((MemoryPtr)bytes, n) == (long)n);
	out.close();
}

} // namespace

TEST_SUITE("FileFamilyContract")
{

TEST_CASE("FC1 the name is null while closed and the opened path while open")
{
	const std::string path = scratch("name.bin");
	writeBytes(path, "0123456789", 10);

	File f;
	CHECK(f.getFilename() == nullptr);
	REQUIRE(f.open(path.c_str()) == NO_ERR);
	REQUIRE(f.getFilename() != nullptr);
	CHECK(std::string(f.getFilename()) == path);
	CHECK(f.getLength() == 10);
	f.close();
	CHECK(f.getFilename() == nullptr);
	CHECK(!f.isOpen());

	remove(path.c_str());
}

TEST_CASE("FC2 a child reads its parent's bytes, shares its name and is closed with it")
{
	const std::string path = scratch("child.bin");
	writeBytes(path, "ABCDEFGHIJ", 10);

	File parent;
	REQUIRE(parent.open(path.c_str()) == NO_ERR);
	parent.seek(2);

	File child;
	REQUIRE(child.open(&parent, 4) == NO_ERR);
	CHECK(child.getParent() == &parent);
	REQUIRE(child.getFilename() != nullptr);
	CHECK(std::string(child.getFilename()) == path);

	unsigned char got[5] = {0};
	CHECK(child.read(got, 4) == 4);
	CHECK(memcmp(got, "CDEF", 4) == 0);

	// Closing the parent first closes every child that is still open.
	parent.close();
	CHECK(!child.isOpen());
	child.close();

	remove(path.c_str());
}

TEST_CASE("FC3 closing a file opened on a caller's buffer leaves the buffer alone")
{
	char buffer[] = "caller-owned bytes";
	const size_t n = sizeof(buffer) - 1;

	File f;
	REQUIRE(f.open(buffer, (int)n) == NO_ERR);

	char got[8] = {0};
	CHECK(f.read((MemoryPtr)got, 6) == 6);
	CHECK(memcmp(got, "caller", 6) == 0);
	f.close();

	CHECK(std::string(buffer) == "caller-owned bytes");
}

TEST_CASE("FC4 a closed file opens again and reports the new name")
{
	const std::string a = scratch("again_a.bin");
	const std::string b = scratch("again_b.bin");
	writeBytes(a, "aaaa", 4);
	writeBytes(b, "bbbbbbbb", 8);

	File f;
	REQUIRE(f.open(a.c_str()) == NO_ERR);
	CHECK(std::string(f.getFilename()) == a);
	f.close();
	REQUIRE(f.open(b.c_str()) == NO_ERR);
	CHECK(std::string(f.getFilename()) == b);
	CHECK(f.getLength() == 8);
	f.close();

	remove(a.c_str());
	remove(b.c_str());
}

TEST_CASE("FC5 a parent holds its default 50 children and a closed child frees its slot")
{
	const std::string path = scratch("many.bin");
	writeBytes(path, "0123456789", 10);

	File parent;
	REQUIRE(parent.open(path.c_str()) == NO_ERR);

	File children[51];
	for (int i = 0; i < 50; i++)
		REQUIRE(children[i].open(&parent, 1) == NO_ERR);
	// The 51st child does not fit.
	CHECK(children[50].open(&parent, 1) != NO_ERR);

	children[7].close();
	CHECK(children[50].open(&parent, 1) == NO_ERR);

	parent.close();
	for (File &c : children)
		c.close();

	remove(path.c_str());
}

// ---- Behaviour that is NEW with the standard-container internals (they fail on the old classes) ----

TEST_CASE("FC6 a child closed after its parent was destroyed does not touch the parent")
{
	const std::string path = scratch("orphan.bin");
	writeBytes(path, "0123456789", 10);

	File *parent = new File;
	REQUIRE(parent->open(path.c_str()) == NO_ERR);
	File child;
	REQUIRE(child.open(parent, 4) == NO_ERR);

	// The parent closes (and so closes the child) and is destroyed; the child's own close, then its destructor, run later.
	parent->close();
	delete parent;
	child.close();

	remove(path.c_str());
}

TEST_CASE("FC7 a packet file whose header claims more packets than the file holds is rejected")
{
	const std::string path = scratch("lying.pak");
	// version, first-packet offset 0x7FFFFFF8 (claims about 536 million packets), and 8 more bytes: 16 bytes in all.
	const unsigned char bytes[16] = {0xCE, 0xFA, 0xED, 0xFE, 0xF8, 0xFF, 0xFF, 0x7F, 0, 0, 0, 0, 0, 0, 0, 0};
	writeBytes(path, (const char *)bytes, sizeof(bytes));

	// The rejection is one [PACKET_HEADER_BAD] log line per process and never a STOP: malformed content is rejected with a log line, never a crash.
	g_mc2TestStopFatal = false;
	g_mc2TestStopCount = 0;
	std::FILE *capture = std::tmpfile();
	REQUIRE(capture != nullptr);
	mc2::log::setSpec("*:info");
	mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, capture);

	PacketFile in;
	CHECK(in.open(path.c_str()) == PACKET_OUT_OF_RANGE);
	CHECK(g_mc2TestStopCount == 0);
	CHECK(in.getNumPackets() == 0);
	in.close();

	// A negative offset gives a negative count.
	const unsigned char negative[16] = {0xCE, 0xFA, 0xED, 0xFE, 0xF0, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0};
	writeBytes(path, (const char *)negative, sizeof(negative));
	PacketFile in2;
	CHECK(in2.open(path.c_str()) == PACKET_OUT_OF_RANGE);
	CHECK(in2.getNumPackets() == 0);
	in2.close();
	CHECK(g_mc2TestStopCount == 0);

	std::fflush(capture);
	std::rewind(capture);
	std::string logged;
	char buf[512];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof buf, capture)) > 0) logged.append(buf, n);
	mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, nullptr);
	std::fclose(capture);
	const size_t first = logged.find("[PACKET_HEADER_BAD]");
	CHECK(first != std::string::npos);
	CHECK(logged.find("[PACKET_HEADER_BAD]", first + 1) == std::string::npos);   // one line per process, not one per bad file
	g_mc2TestStopFatal = true;

	remove(path.c_str());
}

TEST_CASE("FC8 a fit file with no blocks opens and finds no block")
{
	const char text[] = "FITini\r\n\r\nFITend\r\n";
	FitIniFile fit;
	REQUIRE(fit.open(text, (int)(sizeof(text) - 1)) == NO_ERR);
	CHECK(fit.seekBlock("Anything") != NO_ERR);
	fit.close();
}

} // TEST_SUITE

namespace {
struct AblMemory {
	unsigned char bytes[4] = {42, 0, 0, 0};
	long length = 4;
	long position = 0;
	int reads = 0;
	int closes = 0;
};

struct AblCallbacks {
	decltype(ABLFile::remainingCB) remaining = ABLFile::remainingCB;
	decltype(ABLFile::readCB) read = ABLFile::readCB;
	decltype(ABLFile::openCB) open = ABLFile::openCB;
	decltype(ABLFile::closeCB) close = ABLFile::closeCB;

	AblCallbacks() {
		ABLFile::remainingCB = [](void* h) { auto& m = *static_cast<AblMemory*>(h); return m.length - m.position; };
		ABLFile::readCB = [](void* h, unsigned char* out, long n) {
			auto& m = *static_cast<AblMemory*>(h);
			++m.reads;
			memcpy(out, m.bytes + m.position, n);
			m.position += n;
			return n;
		};
		ABLFile::openCB = [](void** h, const char*) { *h = nullptr; return -7L; };
		ABLFile::closeCB = [](void** h) { ++static_cast<AblMemory*>(*h)->closes; *h = nullptr; return 0L; };
	}
	~AblCallbacks() {
		ABLFile::remainingCB = remaining; ABLFile::readCB = read;
		ABLFile::openCB = open; ABLFile::closeCB = close;
	}
};
}

TEST_CASE("FileFamilyContract/ABL truncation and oversized reads never reach the unchecked callback") {
	AblCallbacks callbacks;
	AblMemory complete, truncated;
	truncated.length = 3;
	std::FILE* capture = std::tmpfile();
	REQUIRE(capture != nullptr);
	mc2::log::setSpec("*:info");
	mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, capture);
	const int stops = g_mc2TestStopCount;
	{
		ABLFile valid, bad, missing;
		valid.set(&complete);
		CHECK(valid.readInt() == 42);
		CHECK(complete.reads == 1);
		bad.set(&truncated);
		CHECK(bad.readInt() == -1);
		unsigned char guard[4] = {1, 2, 3, 4};
		CHECK(bad.read(guard, LONG_MAX) == 0);
		CHECK(truncated.reads == 0);
		CHECK(guard[0] == 1);
		CHECK(guard[3] == 4);
		CHECK(missing.open("missing.abl") == -7);
		CHECK(missing.get() == nullptr);
	}
	CHECK(complete.closes == 1);
	CHECK(truncated.closes == 1);
	CHECK(g_mc2TestStopCount == stops);
	mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, nullptr);
	std::rewind(capture);
	std::string logged;
	char line[512];
	while (std::fgets(line, sizeof(line), capture)) logged += line;
	std::fclose(capture);
	const auto first = logged.find("[ABL_FILE_READ_BAD]");
	REQUIRE(first != std::string::npos);
	const auto second = logged.find("[ABL_FILE_READ_BAD]", first + 1);
	REQUIRE(second != std::string::npos);
	CHECK(logged.find("[ABL_FILE_READ_BAD]", second + 1) == std::string::npos);
	CHECK(logged.find("[ABL_FILE_OPEN_BAD]") != std::string::npos);
}
