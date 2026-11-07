// LG14 part 2: the tactic table. Expected values are literals from the pre-table sources (move.h flag values, the brain
// Tactics block names, the shipped TacticType ordinals), not derived from the table under test.
#include "doctest.h"
#include "tactic_table.h"

#include <cstring>

namespace tt = tactic_table;

namespace {
// move.h TACORDER_PARAM_TACTIC_* literals.
constexpr unsigned long kRightFlag = 4194304ul;   // 1<<22
constexpr unsigned long kLeftFlag = 8388608ul;    // 1<<23
constexpr unsigned long kRearFlag = 16777216ul;
constexpr unsigned long kSfFlag = 33554432ul;
constexpr unsigned long kTurretFlag = 67108864ul;
constexpr unsigned long kJoustFlag = 134217728ul;
constexpr unsigned long kLegacyMask = 0x0FC00000ul;
} // namespace

TEST_CASE("tactic_table: all twelve values and names") {
	static_assert(tt::kCount == 12);
	const char* const expect[12] = {"None", "FlankLeft", "FlankRight", "FlankRear", "StopAndFire", "Turret",
		"Joust", "IndirectFire", "HullDown", "FightingWithdraw", "Pursue", "HitAndRun"};
	for (int i = 0; i < 12; ++i) {
		CHECK(std::strcmp(tt::name(i), expect[i]) == 0);
		CHECK(tt::in_domain(i));
		CHECK(tt::find_name(expect[i]) == (i == 0 ? 0 : i));
	}
}

TEST_CASE("tactic_table: out-of-domain values are bounded") {
	CHECK_FALSE(tt::in_domain(-1));
	CHECK_FALSE(tt::in_domain(12));
	CHECK(std::strcmp(tt::name(12), "Unknown") == 0);
	CHECK(std::strcmp(tt::name(-1), "Unknown") == 0);
	CHECK(std::strcmp(tt::code2(99), "??") == 0);
	CHECK(std::strcmp(tt::code2(1), "LT") == 0);
	CHECK(std::strcmp(tt::code2(2), "RT") == 0);
	CHECK(std::strcmp(tt::code2(11), "HR") == 0);
}

TEST_CASE("tactic_table: brain aliases") {
	CHECK(tt::find_name("Flank") == 2);
	CHECK(tt::find_name("Rear") == 3);
	CHECK(tt::find_name("Standard") == 4);
	CHECK(tt::find_name("Suppress") == 7);
	CHECK(tt::find_name("Rearguard") == 9);
	CHECK(tt::find_name("flank") == -1);
	CHECK(tt::find_name("") == -1);
	CHECK(tt::find_name(std::string_view{}) == -1);
}

TEST_CASE("tactic_table: LEFT is value 1 with the LEFT flag, RIGHT is value 2 with the RIGHT flag") {
	CHECK(tt::to_params(1) == kLeftFlag);
	CHECK(tt::to_params(2) == kRightFlag);
	CHECK(tt::to_params(3) == kRearFlag);
	CHECK(tt::to_params(4) == kSfFlag);
	CHECK(tt::to_params(5) == kTurretFlag);
	CHECK(tt::to_params(6) == kJoustFlag);
	CHECK(tt::to_params(0) == 0ul);
	CHECK(tt::kLegacyParamMask == kLegacyMask);
	CHECK(tt::from_params(kLeftFlag) == 1);
	CHECK(tt::from_params(kRightFlag) == 2);
	CHECK(tt::from_params(kRearFlag) == 3);
	CHECK(tt::from_params(kSfFlag) == 4);
	CHECK(tt::from_params(kTurretFlag) == 5);
	CHECK(tt::from_params(kJoustFlag) == 6);
}

TEST_CASE("tactic_table: legacy flags with two bits set decode to NONE, as the original switch did") {
	CHECK(tt::from_params(kLeftFlag | kRightFlag) == 0);
	CHECK(tt::from_params(0ul) == 0);
}

TEST_CASE("tactic_table: extended tactics 7..11 round trip through the typed field, outside the legacy mask") {
	for (int v = 7; v <= 11; ++v) {
		const unsigned long p = tt::to_params(v);
		CHECK(p != 0ul);
		CHECK((p & kLegacyMask) == 0ul);
		CHECK(p <= 0xFFFFFFFFul);
		CHECK(tt::from_params(p) == v);
	}
	// other order params bits do not disturb the decode
	CHECK(tt::from_params(tt::to_params(11) | 0x3Ful) == 11);
	CHECK(tt::from_params(tt::to_params(2) | 0x3Ful) == 2);
}
