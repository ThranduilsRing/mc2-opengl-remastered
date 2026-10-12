// tests/unit/test_abl_concat.cpp
//
// ABL's concat() appends into a script variable declared char[N]; the append must stop at N-1 characters and keep the
// NUL inside the variable, whatever the script asks for (the unbounded strcat it replaced corrupted the heap in mc2_15).

#include "doctest.h"

#include "abl_string_append.h"

#include <cstring>

TEST_SUITE("ABL concat")
{

TEST_CASE("an append that fits is complete and the text stays terminated")
{
	char s[16] = "ab";
	CHECK(abl::appendBounded(s, sizeof s, "cdef") == 0);
	CHECK(std::strcmp(s, "abcdef") == 0);
}

TEST_CASE("an append that overflows is cut at the capacity and reports the dropped count")
{
	char block[12];
	std::memset(block, 'X', sizeof block);
	char* s = block;
	s[0] = '\0';
	// capacity 8: 7 characters and the NUL; the 4 bytes after it must stay untouched
	CHECK(abl::appendBounded(s, 8, "0123456789") == 3);
	CHECK(std::strcmp(s, "0123456") == 0);
	CHECK(s[7] == '\0');
	CHECK(block[8] == 'X');
	CHECK(block[11] == 'X');
	// a full variable accepts nothing more and still drops the whole item
	CHECK(abl::appendBounded(s, 8, "!") == 1);
	CHECK(std::strcmp(s, "0123456") == 0);
}

TEST_CASE("an unterminated variable counts as full and is terminated at its capacity")
{
	char s[6] = {'a', 'b', 'c', 'd', 'e', 'f'};
	CHECK(abl::appendBounded(s, 4, "zz") == 2);
	CHECK(std::strcmp(s, "abc") == 0);
	CHECK(s[4] == 'e');
}

TEST_CASE("a destination without a known size takes nothing")
{
	char s[4] = "ab";
	CHECK(abl::appendBounded(s, 0, "cd") == 2);
	CHECK(std::strcmp(s, "ab") == 0);
	CHECK(abl::appendBounded(nullptr, 8, "cd") == 2);
	CHECK(abl::appendBounded(s, sizeof s, nullptr) == 0);
}

}
