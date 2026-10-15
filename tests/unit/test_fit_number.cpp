// FIT-NUMBER-1: the FIT number readers that the mission editor shares with the game. The three cases are the divergences the
// editor review found between these readers and a strtol reading; stock and mod data depend on them, so they are pinned.
// The readers write into their argument, so every input is a writable array.

#include "doctest.h"

#include "fit_number.h"

TEST_SUITE("FitNumber")
{

TEST_CASE("a decimal value followed by a comment that holds 0x reads the comment's hex")
{
	char text[] = "7 // 0xff";
	CHECK(mc2::fit::textToLong(text) == 255);
	char text2[] = "7 // 0xff";
	CHECK(mc2::fit::textToULong(text2) == 255ul);
}

TEST_CASE("0x followed by a non-hex character reads 0")
{
	char text[] = "0x-1";
	CHECK(mc2::fit::textToLong(text) == 0);
	char text2[] = "0x-1";
	CHECK(mc2::fit::textToULong(text2) == 0ul);
}

TEST_CASE("an uppercase 0X is not hex: the decimal reader stops at the X")
{
	char text[] = "0X16";
	CHECK(mc2::fit::textToLong(text) == 0);
	char text2[] = "0X16";
	CHECK(mc2::fit::textToULong(text2) == 0ul);
}

TEST_CASE("plain values read as before")
{
	char dec[] = "42";
	CHECK(mc2::fit::textToLong(dec) == 42);
	char hex[] = "0x1F";
	CHECK(mc2::fit::textToULong(hex) == 31ul);
	CHECK(mc2::fit::textToFloat("2.5") == doctest::Approx(2.5f));
}

} // TEST_SUITE("FitNumber")
