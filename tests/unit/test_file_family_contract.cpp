// tests/unit/test_file_family_contract.cpp
//
// STEP 4 B3.0: what the File family promises to its callers TODAY, pinned before B3.1 moves the name, the child list, the in-RAM image and the seek
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

#include <cstdio>
#include <cstring>
#include <string>

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

} // TEST_SUITE
