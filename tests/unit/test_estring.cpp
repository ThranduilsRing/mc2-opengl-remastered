// tests/unit/test_estring.cpp
//
// EString::Format sizes its buffer with the C runtime's own format parser (vsnprintf with a null buffer). The hand-rolled
// %-scanner it replaced did not know ll / z / j and then vsprintf wrote into a buffer sized by that guess.

#include "doctest.h"

#include "estring.h"

#include <cstring>

TEST_SUITE("EString")
{

TEST_CASE("Format handles the length modifiers the old scanner mis-sized")
{
	EString s;
	s.Format("%lld|%zu|%I64d|%5.2f|%s|%c", 1234567890123LL, (size_t)42, (__int64)-7, 3.14159, "hi", 'x');
	CHECK(std::strcmp(s.Data(), "1234567890123|42|-7| 3.14|hi|x") == 0);
	CHECK(s.Length() == (int)std::strlen("1234567890123|42|-7| 3.14|hi|x"));
}

TEST_CASE("Format grows past the initial buffer and reuses the string")
{
	EString s("short");
	char big[600];
	std::memset(big, 'z', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	s.Format("[%s]", big);
	CHECK(s.Length() == (int)sizeof(big) - 1 + 2);
	CHECK(s.Data()[0] == '[');
	CHECK(s.Data()[s.Length() - 1] == ']');

	s.Format("%d", 7);                              // shrinks back; no stale tail
	CHECK(std::strcmp(s.Data(), "7") == 0);
	CHECK(s.Length() == 1);
}

TEST_CASE("Format of an empty template and of a width-padded argument")
{
	EString s("x");
	s.Format("%s", "");
	CHECK(s.Length() == 0);
	s.Format("%-8s|%8s|", "ab", "cd");
	CHECK(std::strcmp(s.Data(), "ab      |      cd|") == 0);
}

}
