// The FIT number readers that the mission editor shares with the game. The three cases are the divergences the
// editor review found between these readers and a strtol reading; stock and mod data depend on them, so they are pinned.
// The readers write into their argument, so every input is a writable array.

#include "doctest.h"

#include "fit_number.h"
#include "inifile.h"

#include <cstring>

namespace
{
// The editor extracts the FitIniFile bodies from inifile.cpp at configure time, so inifile.cpp keeps its own copy until the editor
// includes fit_number.h; this probe keeps the two copies in lockstep.
struct FitProbe : FitIniFile
{
	using FitIniFile::textToFloat;
	using FitIniFile::textToLong;
	using FitIniFile::textToULong;
};
}

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
	CHECK(mc2::fit::textToFloat("0x1p3") == doctest::Approx(8.0f));   // atof reads C99 hex floats
}

TEST_CASE("FitIniFile reads every pinned case as the shared readers do")
{
	const char* cases[] = { "7 // 0xff", "0x-1", "0X16", "42", "0x1F", "-12", "0xZZ", "2.5", "0x1p3" };
	FitProbe fit;
	for (const char* c : cases)
	{
		char a[32], b[32];
		strcpy(a, c); strcpy(b, c);
		CHECK(fit.textToLong(a) == mc2::fit::textToLong(b));
		strcpy(a, c); strcpy(b, c);
		CHECK(fit.textToULong(a) == mc2::fit::textToULong(b));
		// Exact equality: both sides are the same atof call, and "0x1p3" pins its hex-float parse on both.
		CHECK(fit.textToFloat(c) == mc2::fit::textToFloat(c));
	}
}

} // TEST_SUITE("FitNumber")
