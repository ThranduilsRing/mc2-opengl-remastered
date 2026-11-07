// LG14 lane 3: packed tactical order words must stay bit-identical after the order enums became enum class.
// Expected words were computed from the pre-LG14 TacticalOrder::pack shift sequence (32-bit long), not from tacorder_wire.h.
#include "doctest.h"
#include "tacorder_wire.h"

namespace tw = tacorder_wire;

namespace {
constexpr int32_t kGuard = 10;            // TACTICAL_ORDER_GUARD
constexpr int32_t kRecover = 26;          // TACTICAL_ORDER_RECOVER, the highest order code
constexpr int32_t kOriginCommander = 1;   // ORDER_ORIGIN_COMMANDER
constexpr int32_t kOriginSelf = 2;        // ORDER_ORIGIN_SELF
} // namespace

TEST_CASE("tacorder_wire: GUARD without a target packs sentinel 31 and decodes back to GUARD + location") {
	const uint32_t w = tw::pack_word0(-1, 0, 0, 0, false, 0, tw::encode_code(kGuard, false));
	CHECK(w == 0x1000001Fu);
	const tw::DecodedCode d = tw::decode_code(w);
	CHECK(d.code == kGuard);
	CHECK(d.guardLocation);
}

TEST_CASE("tacorder_wire: GUARD with a target packs its own code") {
	const uint32_t w = tw::pack_word0(-1, 0, 0, 0, false, 0, tw::encode_code(kGuard, true));
	CHECK(w == 0x1000000Au);
	const tw::DecodedCode d = tw::decode_code(w);
	CHECK(d.code == kGuard);
	CHECK_FALSE(d.guardLocation);
}

TEST_CASE("tacorder_wire: max code with every word-0 field at its top value") {
	// aimLocation 13 (+2 = 15), method RAMMING 2, type CONSERVING_AMMO 3, all 16 group bits, unit order, origin SELF.
	const uint32_t w = tw::pack_word0(13, 2, 3, 0xFFFFu, true, kOriginSelf, tw::encode_code(kRecover, true));
	CHECK(w == 0xFBFFFFDAu);
	const tw::DecodedCode d = tw::decode_code(w);
	CHECK(d.code == kRecover);
	CHECK_FALSE(d.guardLocation);
	CHECK(tw::encode_code(kRecover, false) == kRecover);
}

TEST_CASE("tacorder_wire: NONE order from the commander, aimLocation -2") {
	const uint32_t w = tw::pack_word0(-2, 0, 0, 0, false, kOriginCommander, tw::encode_code(0, false));
	CHECK(w == 0x20u);
	CHECK(tw::decode_code(w).code == 0);
}

TEST_CASE("tacorder_wire: fire range edges -4 and 3 round-trip through the 3-bit biased field") {
	const uint32_t lo = tw::pack_word1_tail(false, 0, false, false, false, false, -4);
	CHECK(lo == 0x0u);
	CHECK(tw::decode_fire_range(lo) == -4);
	const uint32_t hi = tw::pack_word1_tail(true, 1, true, true, true, true, 3);
	CHECK(hi == 0x1FFu);
	CHECK(tw::decode_fire_range(hi) == 3);
	const uint32_t optimal = tw::pack_word1_tail(true, 0, false, false, false, false, -1);
	CHECK(optimal == 0x103u);
	CHECK(tw::decode_fire_range(optimal) == -1);
	for (int32_t r = -4; r <= 3; ++r)
		CHECK(tw::decode_fire_range(tw::pack_word1_tail(false, 0, false, false, false, false, r)) == r);
}

TEST_CASE("tacorder_wire: word 1 with a location prefix matches TacticalOrder::pack") {
	// pack shifts the cell prefix past the run bit, then the tail adds the remaining 8 bits (tacordr.cpp).
	const uint32_t prefixShifted = ((5u << 10) | 7u) << 1;
	const uint32_t w = (prefixShifted << (tw::kWord1TailBits - 1)) | tw::pack_word1_tail(true, 0, false, false, false, false, -4);
	CHECK(w == 0x280F00u);
	CHECK(tw::decode_fire_range(w) == -4);
}
