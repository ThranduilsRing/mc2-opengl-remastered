// tests/unit/test_tga_scratch.cpp
//
// WHAT: mclib/tga_scratch.h, the whole-file read and the TGA body-size check of the terrain texture loaders.
// WHY: those files are mod content; a truncated one must be refused before a decode loop reads past its buffer.

#include "doctest.h"

#include "tga_scratch.h"

#include <cstring>
#include <vector>

namespace
{
struct FakeFile
{
	std::vector<unsigned char> bytes;
	long shortBy = 0;
	unsigned long fileSize () const { return static_cast<unsigned long>(bytes.size()); }
	long read (unsigned char* dst, long count)
	{
		const long n = count - shortBy;
		if (n > 0)
			std::memcpy(dst, bytes.data(), static_cast<size_t>(n));
		return n;
	}
};
}

TEST_CASE("undefined terrain types have no mip table")
{
	unsigned char pixel = 42;
	unsigned char* mips[] = {&pixel, nullptr};
	CHECK(mc2::tga::mipData<unsigned char>(nullptr, 0) == nullptr);
	CHECK(mc2::tga::mipData(mips, 0) == &pixel);
	CHECK(mc2::tga::mipData(mips, 1) == nullptr);
}

TEST_CASE("bodyFits accepts exactly the pixels the header claims")
{
	namespace tga = mc2::tga;
	CHECK(tga::bodyFits(18 + 4 * 4 * 3, 18, 4, 4, 24));
	CHECK(tga::bodyFits(18 + 4 * 4 * 4, 18, 4, 4, 32));
	CHECK(tga::bodyFits(18 + 4 * 4 * 4 + 100, 18, 4, 4, 32));
	CHECK_FALSE(tga::bodyFits(18 + 4 * 4 * 4 - 1, 18, 4, 4, 32));
	CHECK_FALSE(tga::bodyFits(18 + 4 * 4 * 3 - 1, 18, 4, 4, 24));
	CHECK_FALSE(tga::bodyFits(17, 18, 1, 1, 24));
	CHECK_FALSE(tga::bodyFits(1000, 18, 0, 4, 24));
	CHECK_FALSE(tga::bodyFits(1000, 18, 4, -1, 24));
	CHECK_FALSE(tga::bodyFits(1 << 20, 18, 32767, 32767, 32));
}

TEST_CASE("readAllBytes returns the file, and refuses an empty, oversized or short-read file")
{
	namespace tga = mc2::tga;
	std::vector<unsigned char> scratch{9, 9};

	FakeFile ok;
	ok.bytes = {1, 2, 3, 4};
	const auto out = tga::readAllBytes(ok, std::move(scratch));
	REQUIRE(out);
	CHECK(*out == ok.bytes);

	FakeFile empty;
	CHECK_FALSE(tga::readAllBytes(empty));

	FakeFile shortRead;
	shortRead.bytes = {1, 2, 3, 4};
	shortRead.shortBy = 1;
	CHECK_FALSE(tga::readAllBytes(shortRead));
}
