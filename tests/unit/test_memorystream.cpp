// tests/unit/test_memorystream.cpp
//
// Stuff::MemoryStream::ReadBytes is bounds-checked in every build. The old check sat under _ARMOR, which no target
// defines, so a truncated mod .fx read past the end of its buffer. Missing bytes now read as zeros and the position
// stops at the end of the stream; valid data is unaffected.

#include "doctest.h"

#include <stuff/stuff.hpp>

#include <cstring>

TEST_SUITE("MemoryStream")
{

TEST_CASE("ReadBytes returns the stream's bytes and advances")
{
	Stuff::RegisteredClass::InitializeClass();
	Stuff::MemoryStream::InitializeClass();
	{
		unsigned char src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
		Stuff::MemoryStream ms(src, sizeof(src));

		unsigned char out[4] = {0};
		ms.ReadBytes(out, 4);
		CHECK(std::memcmp(out, src, 4) == 0);
		CHECK(ms.GetBytesRemaining() == 4);

		ms.ReadBytes(out, 4);                       // exactly what is left: no zero fill
		CHECK(std::memcmp(out, src + 4, 4) == 0);
		CHECK(ms.GetBytesRemaining() == 0);
	}
	Stuff::MemoryStream::TerminateClass();
	Stuff::RegisteredClass::TerminateClass();
}

TEST_CASE("ReadBytes past the end zero-fills the missing tail and stops at the end")
{
	Stuff::RegisteredClass::InitializeClass();
	Stuff::MemoryStream::InitializeClass();
	{
		unsigned char src[3] = {0xAA, 0xBB, 0xCC};
		Stuff::MemoryStream ms(src, sizeof(src));

		unsigned char out[8];
		std::memset(out, 0x5A, sizeof(out));
		ms.ReadBytes(out, sizeof(out));             // asks for 8, only 3 exist
		CHECK(out[0] == 0xAA);
		CHECK(out[1] == 0xBB);
		CHECK(out[2] == 0xCC);
		for (int i = 3; i < 8; ++i) CHECK(out[i] == 0);
		CHECK(ms.GetBytesRemaining() == 0);

		std::memset(out, 0x5A, sizeof(out));
		ms.ReadBytes(out, 4);                       // nothing left at all
		for (int i = 0; i < 4; ++i) CHECK(out[i] == 0);
		CHECK(out[4] == 0x5A);                      // and nothing beyond the request is touched
	}
	Stuff::MemoryStream::TerminateClass();
	Stuff::RegisteredClass::TerminateClass();
}

}
