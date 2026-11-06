// tests/unit/test_abl_include_path.cpp
//
// ABL #include_ resolves next to the including script and keeps '/' whatever separators the source path mixes. The Windows
// regression: a preferred_separator scan dropped the directory of "data\missions/..." and OConst.abi did not open.

#include "doctest.h"

#include "abl_include_path.h"

TEST_CASE("ABL include path: mixed separators keep the directory and use '/'") {
	CHECK(ablIncludePath("data/missions/mc2_01.abl", "OConst.abi") == "data/missions/OConst.abi");
#ifdef _WIN32   // '\' is a separator only on Windows
	CHECK(ablIncludePath("data/missions\\mc2_01.abl", "OConst.abi") == "data/missions/OConst.abi");
	CHECK(ablIncludePath("data\\missions/mc2_01.abl", "OConst.abi") == "data/missions/OConst.abi");
	CHECK(ablIncludePath("data\\missions\\warriors\\brain.abl", "OConst.abi") == "data/missions/warriors/OConst.abi");
#endif
	CHECK(ablIncludePath("mc2_01.abl", "OConst.abi") == "OConst.abi");
}
