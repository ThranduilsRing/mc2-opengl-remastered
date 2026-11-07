// tests/unit/test_memorystream.cpp
//
// Stuff::MemoryStream::ReadBytes is bounds-checked in every build. The old check sat under _ARMOR, which no target
// defines, so a truncated mod .fx read past the end of its buffer. Missing bytes now read as zeros and the position
// stops at the end of the stream; valid data is unaffected.

#include "doctest.h"

#include <stuff/stuff.hpp>
#include <gosfx/fcurve.hpp>

#include <cstring>
#include <vector>

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

// B6/B8: the serialized MLR/gosFX holders are std::vector. The vector stream overloads must write the legacy
// payload (uint32 count, then count*sizeof(T) raw native-endian element bytes), spelled out here byte by byte,
// and read it back.
template <class T>
static void CheckVectorPayloadMatchesDynamicArray(const std::vector<T> &values)
{
	std::vector<unsigned char> expected(sizeof(uint32_t) + values.size() * sizeof(T));
	const uint32_t count = static_cast<uint32_t>(values.size());
	std::memcpy(expected.data(), &count, sizeof(count));
	if (!values.empty())
		std::memcpy(expected.data() + sizeof(count), values.data(), values.size() * sizeof(T));

	unsigned char vecBuf[512] = {0};
	size_t used = 0;
	{
		Stuff::MemoryStream ms(vecBuf, sizeof(vecBuf));
		MemoryStreamIO_Write(&ms, &values);
		used = ms.GetBytesUsed();
	}
	CHECK(used == expected.size());
	CHECK(std::memcmp(vecBuf, expected.data(), expected.size()) == 0);

	// Read the legacy payload back: same length, same element bytes.
	std::vector<T> back(3);	// non-empty first: Read must resize, not append
	{
		Stuff::MemoryStream ms(expected.data(), expected.size());
		MemoryStreamIO_Read(&ms, &back);
		CHECK(ms.GetBytesUsed() == used);
	}
	REQUIRE(back.size() == values.size());
	if (!values.empty())
		CHECK(std::memcmp(back.data(), values.data(), values.size() * sizeof(T)) == 0);
}

TEST_CASE("std::vector stream payload is byte-identical to DynamicArrayOf for every B6 element type")
{
	Stuff::RegisteredClass::InitializeClass();
	Stuff::MemoryStream::InitializeClass();
	{
		CheckVectorPayloadMatchesDynamicArray(std::vector<glm::vec3>{{1.0f, -2.5f, 3.25f}, {0.0f, 7.0f, -0.125f}});
		CheckVectorPayloadMatchesDynamicArray(std::vector<glm::vec4>{{1.0f, 2.0f, 3.0f, 4.0f}});
		CheckVectorPayloadMatchesDynamicArray(std::vector<Stuff::Vector2DOf<Stuff::Scalar>>{
			Stuff::Vector2DOf<Stuff::Scalar>(0.5f, 0.75f), Stuff::Vector2DOf<Stuff::Scalar>(-1.0f, 2.0f)});
		CheckVectorPayloadMatchesDynamicArray(std::vector<unsigned char>{3, 4, 255});
		CheckVectorPayloadMatchesDynamicArray(std::vector<unsigned short>{0, 1, 2, 65535, 7});
		CheckVectorPayloadMatchesDynamicArray(std::vector<uint32_t>{0xFF00FF00u, 0x12345678u});
		CheckVectorPayloadMatchesDynamicArray(std::vector<unsigned short>{});
	}
	Stuff::MemoryStream::TerminateClass();
	Stuff::RegisteredClass::TerminateClass();
}

static gosFX::CurveKey MakeKey(float t, float s, float v)
{
	gosFX::CurveKey k;
	k.m_time = t;
	k.m_slope = s;
	k.m_value = v;
	return k;
}

// The element types the converted holders really carry: ComplexCurve::m_keys (CurveKey), MLREffect's
// transformedCoords (Stuff::Vector4D) and DWORD. Each runs once shrinking (2 elements into the 3-element
// destination) and once growing (more than 3 elements), so Read must both shrink and grow the vector.
TEST_CASE("std::vector stream payload matches DynamicArrayOf for CurveKey, Vector4D and DWORD, shrinking and growing")
{
	Stuff::RegisteredClass::InitializeClass();
	Stuff::MemoryStream::InitializeClass();
	{
		CheckVectorPayloadMatchesDynamicArray(std::vector<gosFX::CurveKey>{
			MakeKey(0.0f, 1.0f, 2.0f), MakeKey(1.0f, -0.5f, 3.0f)});
		CheckVectorPayloadMatchesDynamicArray(std::vector<gosFX::CurveKey>{
			MakeKey(0.0f, 1.0f, 2.0f), MakeKey(0.25f, 0.0f, 2.5f), MakeKey(0.5f, -1.0f, 2.25f),
			MakeKey(0.75f, 2.0f, 1.0f), MakeKey(1.0f, 0.0f, 0.0f), MakeKey(1.5f, 4.0f, -3.0f)});

		CheckVectorPayloadMatchesDynamicArray(std::vector<Stuff::Vector4D>{
			Stuff::Vector4D(1.0f, 2.0f, 3.0f, 1.0f), Stuff::Vector4D(-1.0f, 0.5f, 0.0f, 2.0f)});
		CheckVectorPayloadMatchesDynamicArray(std::vector<Stuff::Vector4D>{
			Stuff::Vector4D(1.0f, 2.0f, 3.0f, 1.0f), Stuff::Vector4D(-1.0f, 0.5f, 0.0f, 2.0f),
			Stuff::Vector4D(9.0f, 8.0f, 7.0f, 6.0f), Stuff::Vector4D(0.0f, 0.0f, 0.0f, 0.0f),
			Stuff::Vector4D(-3.5f, 4.25f, 100.0f, 0.125f)});

		CheckVectorPayloadMatchesDynamicArray(std::vector<DWORD>{0xDEADBEEFu, 1u});
		CheckVectorPayloadMatchesDynamicArray(std::vector<DWORD>{0xDEADBEEFu, 1u, 2u, 0xFFFFFFFFu, 0u, 42u, 7u});

		// Growth for the original B6 types too.
		CheckVectorPayloadMatchesDynamicArray(std::vector<glm::vec3>{
			{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}, {7.0f, 8.0f, 9.0f}, {-1.0f, -2.0f, -3.0f}});
		CheckVectorPayloadMatchesDynamicArray(std::vector<Stuff::Vector2DOf<Stuff::Scalar>>{
			Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 0.0f), Stuff::Vector2DOf<Stuff::Scalar>(1.0f, 0.0f),
			Stuff::Vector2DOf<Stuff::Scalar>(1.0f, 1.0f), Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 1.0f)});
		CheckVectorPayloadMatchesDynamicArray(std::vector<unsigned char>{4, 4, 3, 4, 4});
	}
	Stuff::MemoryStream::TerminateClass();
	Stuff::RegisteredClass::TerminateClass();
}

}
