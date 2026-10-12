// tests/unit/test_abl_concat.cpp
//
// ABL's concat() and the standard routines that fill a script variable declared char[N] must stop at N-1 characters and keep
// the NUL inside the variable, whatever the script asks for (the unbounded strcat it replaced corrupted the heap in mc2_15);
// the position builtins give a defined answer when they have none.

#include "doctest.h"

#include "abl_out_params.h"
#include "abl_string_append.h"

#include <cstring>

TEST_SUITE("ABL concat")
{

TEST_CASE("an append that fits is complete and the text stays terminated")
{
	char s[16] = "ab";
	CHECK(abl::appendBounded(s, "cdef") == 0);
	CHECK(std::strcmp(s, "abcdef") == 0);
}

TEST_CASE("an append that overflows is cut at the capacity and reports the dropped count")
{
	char block[12];
	std::memset(block, 'X', sizeof block);
	block[0] = '\0';
	// capacity 8: 7 characters and the NUL; the 4 bytes after it must stay untouched
	const std::span<char> s{block, 8};
	CHECK(abl::appendBounded(s, "0123456789") == 3);
	CHECK(std::strcmp(block, "0123456") == 0);
	CHECK(block[7] == '\0');
	CHECK(block[8] == 'X');
	CHECK(block[11] == 'X');
	// a full variable accepts nothing more and still drops the whole item
	CHECK(abl::appendBounded(s, "!") == 1);
	CHECK(std::strcmp(block, "0123456") == 0);
}

TEST_CASE("an unterminated variable counts as full and is terminated at its capacity")
{
	char s[6] = {'a', 'b', 'c', 'd', 'e', 'f'};
	CHECK(abl::appendBounded(std::span<char>{s, 4}, "zz") == 2);
	CHECK(std::strcmp(s, "abc") == 0);
	CHECK(s[4] == 'e');
}

TEST_CASE("a destination without a known size takes nothing")
{
	char s[4] = "ab";
	CHECK(abl::appendBounded(std::span<char>{}, "cd") == 2);
	CHECK(abl::appendBounded(std::span<char>{s}, abl::textOf(nullptr)) == 0);
	CHECK(std::strcmp(s, "ab") == 0);
	CHECK(abl::copyBounded(std::span<char>{}, "cd") == 2);
	CHECK(std::strcmp(s, "ab") == 0);
}

TEST_CASE("getmodule-style copies replace the text, are cut at the capacity and keep the NUL inside the variable")
{
	char block[10];
	std::memset(block, 'X', sizeof block);
	const std::span<char> s{block, 6};
	// fits whole: the earlier text is replaced
	CHECK(abl::copyBounded(s, "abc") == 0);
	CHECK(std::strcmp(block, "abc") == 0);
	// text longer than the variable: 5 characters and the NUL, the bytes after the variable untouched
	CHECK(abl::copyBounded(s, "0123456789") == 5);
	CHECK(std::strcmp(block, "01234") == 0);
	CHECK(block[5] == '\0');
	CHECK(block[6] == 'X');
	CHECK(block[9] == 'X');
	// a null name (no FSM) is the empty text
	CHECK(abl::copyBounded(s, abl::textOf(nullptr)) == 0);
	CHECK(block[0] == '\0');
	// a one-byte variable holds only the NUL
	char one[2] = {'q', 'q'};
	CHECK(abl::copyBounded(std::span<char>{one, 1}, "zz") == 2);
	CHECK(one[0] == '\0');
	CHECK(one[1] == 'q');
}

TEST_CASE("a position builtin with no answer writes the zero vector over whatever the script variable held")
{
	float pos[3] = {-2.5e18f, 7.0f, 1.0e9f};
	abl::writeZeroVec3(std::span<float, 3>{pos});
	CHECK(pos[0] == 0.0f);
	CHECK(pos[1] == 0.0f);
	CHECK(pos[2] == 0.0f);
	abl::writeVec3(std::span<float, 3>{pos}, 1.0f, 2.0f, 3.0f);
	CHECK(pos[0] == 1.0f);
	CHECK(pos[1] == 2.0f);
	CHECK(pos[2] == 3.0f);
}

}
