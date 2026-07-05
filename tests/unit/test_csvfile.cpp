// tests/unit/test_csvfile.cpp
//
// MINIMAL-VIABLE-HARNESS-2 (TU-1): brings mclib/csvfile.cpp under unit test
// game-free, on the same malloc-backed heap shim + real file stack that
// MINIMAL-VIABLE-HARNESS-1 established. CSVFile is a File subclass, so it adds
// ZERO new link frontier (same shim, same base class) -- it completes the
// data-ingest spine: FIT (test_file_inifile.cpp) + CSV (here) + ini all
// offline-verifiable. Pattern doc: docs/testing/minimal-viable-harness.md.
//
// What this proves against a realistic Buildings.csv-shaped fixture (the exact
// column layout code/logisticsdata.cpp::initVariants reads: col 1 name,
// col 3 nameID, col 4 type-string, col 5 fitID, col 11 scale):
//   * 1-based row/col addressing (seekRowCol increments before comparing).
//   * typed reads (readLong / readFloat / readString / readInt) end to end.
//   * the out-of-range-row -> readString()==1 break condition that drives the
//     initVariants scan loop (`if (retVal != 0) break;`).
//   * the CSV dialect's real edge cases: quoted-field and embedded-comma
//     behavior are NOT RFC-4180 (the parser splits on every comma and the
//     readString copyString() stops at the first double-quote). Asserting the
//     actual behavior locks the contract the game depends on today.

#include "doctest.h"

#include "csvfile.h"

#include <cstring>
#include <string>

#ifndef MC2_TEST_FIXTURE_DIR
#error "MC2_TEST_FIXTURE_DIR must be defined by tests/unit/CMakeLists.txt"
#endif

namespace
{
std::string fixturePath (const char *name)
{
	std::string p(MC2_TEST_FIXTURE_DIR);
	p += "/";
	p += name;
	return p;
}
} // namespace

TEST_SUITE("CsvFile")
{

// The fixture (tests/unit/fixtures/buildings_smoke.csv) mirrors the real
// Buildings.csv column contract read by logisticsdata.cpp. CRLF line endings
// on purpose: File::readLine only skips a trailing '\r' before the '\n', so an
// LF-only CSV produces phantom empty rows -- stock Excel-exported CSVs are
// always CRLF, and matching that is part of the contract.
//   row 1 = header  (Name,Notes,NameID,Type,FitID,...,Scale)
//   row 2 = madcat   MECH    nameID 101  fitID 5001  scale 1.0   (clean cols)
//   row 3 = ranger   VEHICLE nameID 102  fitID 5002  scale 0.75  (clean cols)
//   row 4 = depot    BUILDING nameID 103 fitID 5003  scale 2.5   (blank cols)
//   row 5 = turret   ... with an embedded-comma quoted col 2 (edge case; see
//           the dialect test -- the comma shifts every later column by one)
//   row 6 = // comment row

TEST_CASE("CSVFile opens a Buildings.csv-shaped fixture from disk")
{
	CSVFile csv;
	const std::string path = fixturePath("buildings_smoke.csv");
	REQUIRE(csv.open(path.c_str()) == NO_ERR);
	CHECK(csv.getFileClass() == CSVFILE);

	SUBCASE("typed cell reads at 1-based row/col")
	{
		// row 2, col 3 = NameID 101 (long).
		long nameID = -1;
		CHECK(csv.readLong(2, 3, nameID) == NO_ERR);
		CHECK(nameID == 101);

		// row 2, col 5 = FitID 5001.
		long fitID = -1;
		CHECK(csv.readLong(2, 5, fitID) == NO_ERR);
		CHECK(fitID == 5001);

		// readInt is a thin wrapper over readLong.
		int fitInt = -1;
		CHECK(csv.readInt(3, 5, fitInt) == NO_ERR);
		CHECK(fitInt == 5002);
	}

	SUBCASE("type-string column drives the record classifier")
	{
		char type[32] = {0};
		// row 2 col 4 == "MECH"
		CHECK(csv.readString(2, 4, type, sizeof(type)) == 0);
		CHECK(strcmp(type, "MECH") == 0);

		// row 3 col 4 == "VEHICLE"
		CHECK(csv.readString(3, 4, type, sizeof(type)) == 0);
		CHECK(strcmp(type, "VEHICLE") == 0);

		// row 1 col 4 == "Type" (header row is a normal, addressable row)
		CHECK(csv.readString(1, 4, type, sizeof(type)) == 0);
		CHECK(strcmp(type, "Type") == 0);
	}

	SUBCASE("unquoted name field round-trips")
	{
		char name[64] = {0};
		CHECK(csv.readString(2, 1, name, sizeof(name)) == 0);
		CHECK(strcmp(name, "madcat") == 0);
	}

	csv.close();
	CHECK(!csv.isOpen());
}

TEST_CASE("readString past the last record returns 1 (the initVariants break condition)")
{
	// logisticsdata.cpp scans rows until readString(i,4,...) != 0, so this
	// termination behavior is load-bearing for buildings.csv ingestion.
	CSVFile csv;
	const std::string path = fixturePath("buildings_smoke.csv");
	REQUIRE(csv.open(path.c_str()) == NO_ERR);

	// Row 4 (depot) has clean columns; its col 4 reads the type fine.
	char type[32] = {0};
	CHECK(csv.readString(4, 4, type, sizeof(type)) == 0);
	CHECK(strcmp(type, "BUILDING") == 0);

	// A row well past EOF: seekRowCol clamps on totalRows and returns -1,
	// which readString surfaces as 1 (non-zero => scan loop breaks).
	char sink[32] = {0};
	CHECK(csv.readString(999, 4, sink, sizeof(sink)) == 1);

	csv.close();
}

TEST_CASE("empty cells read back as zero without error")
{
	// Row 4 (depot) leaves Armor/Heat/Speed/Tonnage/Pilot blank. The numeric
	// readers never error (they return NO_ERR and zero the out-param on a
	// failed cell parse), which is exactly how initVariants tolerates the
	// sparse building rows.
	CSVFile csv;
	const std::string path = fixturePath("buildings_smoke.csv");
	REQUIRE(csv.open(path.c_str()) == NO_ERR);

	float armor = 123.0f;               // col 6 is blank on row 4
	CHECK(csv.readFloat(4, 6, armor) == NO_ERR);
	CHECK(armor == doctest::Approx(0.0f));

	// The populated cells on the same row still read correctly.
	long nameID = -1;
	CHECK(csv.readLong(4, 3, nameID) == NO_ERR);
	CHECK(nameID == 103);

	// col 5 (FitID) is populated and reachable.
	long fitID = -1;
	CHECK(csv.readLong(4, 5, fitID) == NO_ERR);
	CHECK(fitID == 5003);

	csv.close();
}

TEST_CASE("countCols off-by-one: the final column (index == column count) is unreachable")
{
	// FINDING locked here: CSVFile::countCols() returns the number of COMMAS,
	// not the number of columns. seekRowCol() then guards `col > totalCols`,
	// so on an N-column row the Nth column (index N) is one past totalCols
	// (= N-1) and always fails -> the numeric readers return 0. The 11-column
	// fixture's Scale is column 11, so readFloat(.,11,.) yields 0. This is the
	// live behavior logisticsdata.cpp tolerates via its `if (scale)` guard;
	// this test exists so a future "fix" to countCols can't change it silently.
	CSVFile csv;
	const std::string path = fixturePath("buildings_smoke.csv");
	REQUIRE(csv.open(path.c_str()) == NO_ERR);

	// The last reachable column (10 = Pilot, blank) reads 0 without error.
	float pilot = 9.0f;
	CHECK(csv.readFloat(2, 10, pilot) == NO_ERR);
	CHECK(pilot == doctest::Approx(0.0f));

	// The 11th column (Scale) is out of range -> reads 0 despite the file
	// holding 1.0 there. (Documents the off-by-one, not an aspiration.)
	float scale = 9.0f;
	CHECK(csv.readFloat(2, 11, scale) == NO_ERR);
	CHECK(scale == doctest::Approx(0.0f));

	csv.close();
}

TEST_CASE("CSV dialect edge cases: quotes and embedded commas are NOT RFC-4180")
{
	// This documents the parser's real (quirky) behavior so a future change
	// that "fixes" it can't do so silently. Two facts the engine relies on:
	//
	//  1. getNextWord()/seekRowCol() split on EVERY comma, with no quote
	//     awareness. The fixture's row-5 col-2 cell is "has, comma inside" --
	//     the comma inside the quotes ends the field early, so the type string
	//     "BUILDING" that lives at col 4 on the clean rows is shifted to col 5
	//     on this row. A caller reading col 4 here gets "104", not the type.
	//
	//  2. readString() -> copyString() copies until the first double-quote,
	//     so a field that STARTS with a quote yields an empty string.
	CSVFile csv;
	const std::string path = fixturePath("buildings_smoke.csv");
	REQUIRE(csv.open(path.c_str()) == NO_ERR);

	// The embedded comma shifts the columns: type is at col 5, not col 4.
	char type[32] = {0};
	CHECK(csv.readString(5, 5, type, sizeof(type)) == 0);
	CHECK(strcmp(type, "BUILDING") == 0);

	// Row 5 col 2 begins with a double-quote => copyString stops immediately
	// => empty string. (Proof that quotes are not stripped.)
	char notes[64] = {'x','y','z','\0'};
	CHECK(csv.readString(5, 2, notes, sizeof(notes)) == 0);
	CHECK(notes[0] == '\0');

	csv.close();
}

} // TEST_SUITE("CsvFile")
