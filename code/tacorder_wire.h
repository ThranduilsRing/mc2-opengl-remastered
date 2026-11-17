// tacorder_wire.h - pure bit packing for TacticalOrder::pack/unpack (order words stored by warrior_queue and sent over the net).
// No engine dependencies, so tests/unit/test_tacorder_wire.cpp can pin the packed words. Integer in, integer out: callers
// convert enums with std::to_underlying outbound and validate the decoded ints (enum_in_domain) before static_cast inbound.
// Bit-identical to the pre-LG14 shift sequence in tacordr.cpp: fields are OR'd unmasked, exactly as before.
#pragma once

#include <cstdint>

namespace tacorder_wire {

// All 32 bits of word 0 are occupied. Method 3 carries ranged alpha.
constexpr int32_t encode_method(int32_t method, bool alpha) {
	return method < 0 || method > 2 || (alpha && method != 0) ? -1 : alpha ? 3 : method;
}
struct DecodedMethod { int32_t method; bool alpha; };
constexpr DecodedMethod decode_method(int32_t wire) {
	return wire == 3 ? DecodedMethod{0, true} : DecodedMethod{wire, false};
}

inline constexpr int32_t kCodeBits = 5;
inline constexpr int32_t kOriginBits = 2;
inline constexpr int32_t kUnitOrderBits = 1;
inline constexpr int32_t kGroupBits = 16;
inline constexpr int32_t kAttackTypeBits = 2;
inline constexpr int32_t kAttackMethodBits = 2;
inline constexpr int32_t kAttackRangeBits = 3;
inline constexpr uint32_t kCodeMask = 0x1F;
inline constexpr uint32_t kAttackRangeMask = 0x7;
// A GUARD order with no target object packs this code (the "MAJOR HACK FOR GUARD").
inline constexpr int32_t kGuardCode = 10;
inline constexpr int32_t kGuardPointSentinel = 31;
inline constexpr int32_t kFireRangeBias = 4;
// Bits below the target/location prefix of word 1: run, mode, wait, faceObject, obliterate, pursue (1 each) and range (3).
inline constexpr int32_t kWord1TailBits = 9;

// Code as written to the wire: GUARD without a target becomes the sentinel.
constexpr int32_t encode_code(int32_t code, bool hasTarget) {
	return (code == kGuardCode && !hasTarget) ? kGuardPointSentinel : code;
}

struct DecodedCode {
	int32_t code;
	bool guardLocation;
};

// Sentinel normalized BEFORE the caller's domain check.
constexpr DecodedCode decode_code(uint32_t word0) {
	const int32_t raw = static_cast<int32_t>(word0 & kCodeMask);
	if (raw == kGuardPointSentinel)
		return {kGuardCode, true};
	return {raw, false};
}

constexpr uint32_t pack_word0(int32_t aimLocation, int32_t method, int32_t type, uint32_t groupFlags, bool unitOrder,
                              int32_t origin, int32_t wireCode) {
	uint32_t w = static_cast<uint32_t>(aimLocation + 2);
	w <<= kAttackMethodBits;
	w |= static_cast<uint32_t>(method);
	w <<= kAttackTypeBits;
	w |= static_cast<uint32_t>(type);
	w <<= kGroupBits;
	w |= groupFlags;
	w <<= kUnitOrderBits;
	w |= unitOrder ? 1u : 0u;
	w <<= kOriginBits;
	w |= static_cast<uint32_t>(origin);
	w <<= kCodeBits;
	w |= static_cast<uint32_t>(wireCode);
	return w;
}

constexpr uint32_t pack_word1_tail(bool run, int32_t mode, bool wait, bool faceObject, bool obliterate, bool pursue,
                                   int32_t fireRange) {
	uint32_t w = run ? 1u : 0u;
	w <<= 1;
	w |= static_cast<uint32_t>(mode);
	w <<= 1;
	w |= wait ? 1u : 0u;
	w <<= 1;
	w |= faceObject ? 1u : 0u;
	w <<= 1;
	w |= obliterate ? 1u : 0u;
	w <<= 1;
	w |= pursue ? 1u : 0u;
	w <<= kAttackRangeBits;
	w |= static_cast<uint32_t>(fireRange + kFireRangeBias);
	return w;
}

constexpr int32_t decode_fire_range(uint32_t word1) {
	return static_cast<int32_t>(word1 & kAttackRangeMask) - kFireRangeBias;
}

} // namespace tacorder_wire
