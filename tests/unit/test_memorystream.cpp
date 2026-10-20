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

TEST_CASE("DynamicMemoryStream born small grows on writes through a MemoryStream reference (BUGS row 79)")
{
	Stuff::RegisteredClass::InitializeClass();
	Stuff::MemoryStream::InitializeClass();
	{
		Stuff::DynamicMemoryStream grown(5); // as NotationFile/Note build a nested page: a 128-byte buffer
		Stuff::MemoryStream& base = grown;
		unsigned char chunk[100];
		for (int i = 0; i < 10; ++i) {
			std::memset(chunk, i + 1, sizeof(chunk));
			base.WriteBytes(chunk, sizeof(chunk));
		}
		base << "tail";
		CHECK(grown.GetBytesUsed() == 1004);
		CHECK(grown.GetBufferSize() >= 1004);
		grown.Rewind();
		const auto* bytes = static_cast<const unsigned char*>(grown.GetPointer());
		for (int i = 0; i < 10; ++i)
			CHECK(bytes[i * 100] == i + 1);
		CHECK(std::memcmp(bytes + 1000, "tail", 4) == 0);
	}
	Stuff::MemoryStream::TerminateClass();
	Stuff::RegisteredClass::TerminateClass();
}
TEST_CASE("DynamicMemoryStream copy owns its own bytes; a borrowed buffer is never freed by the stream")
{
	Stuff::RegisteredClass::InitializeClass();
	Stuff::MemoryStream::InitializeClass();
	{
		Stuff::DynamicMemoryStream original(4);
		const unsigned char abcd[4] = {'a', 'b', 'c', 'd'};
		original.WriteBytes(abcd, 4);
		{
			Stuff::DynamicMemoryStream copy(original);	// deep copy: separate owned buffer
			CHECK(copy.GetBytesUsed() == 4);
			CHECK(copy.GetPointer() != original.GetPointer());
			copy.Rewind();
			CHECK(std::memcmp(copy.GetPointer(), abcd, 4) == 0);
		}	// copy's buffer goes here; the original must still read back intact
		original.Rewind();
		CHECK(std::memcmp(original.GetPointer(), abcd, 4) == 0);

		unsigned char borrowed[16] = {7, 7, 7, 7};
		{
			Stuff::DynamicMemoryStream view(borrowed, sizeof(borrowed));
			CHECK(view.GetPointer() == borrowed);
			const unsigned char nine = 9;
			view.WriteBytes(&nine, 1);
		}	// non-owning: the caller's array outlives the stream untouched apart from the write
		CHECK(borrowed[0] == 9);
		CHECK(borrowed[1] == 7);
	}
	Stuff::MemoryStream::TerminateClass();
	Stuff::RegisteredClass::TerminateClass();
}

TEST_CASE("WriteSwappedBytes / ReadSwappedBytes round-trip through the scratch buffer")
{
	Stuff::RegisteredClass::InitializeClass();
	Stuff::MemoryStream::InitializeClass();
	{
		unsigned char buf[4] = {0};
		Stuff::MemoryStream ms(buf, sizeof(buf));
		const unsigned char in[4] = {1, 2, 3, 4};
		ms.WriteSwappedBytes(in, 4);
		CHECK(buf[0] == 4);
		CHECK(buf[3] == 1);
		ms.Rewind();
		unsigned char out[4] = {0};
		ms.ReadSwappedBytes(out, 4);
		CHECK(std::memcmp(out, in, 4) == 0);
	}
	Stuff::MemoryStream::TerminateClass();
	Stuff::RegisteredClass::TerminateClass();
}

}
