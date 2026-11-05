// tests/unit/test_cast_census.cpp
//
// The cast-census fixes keep their results: MString's hash (bounded word copies, the five-bit shift count) equals the legacy
// formula for every length, and ABL's double2long (representation bits instead of an int* read of a double) keeps its
// round-half-to-even conversion.

#include "doctest.h"

#include "ablmc2_internal.h"
#include "stuff/mstring.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

// The legacy hash over a NUL-terminated buffer padded to a whole word, so its full-word reads stay inside the buffer.
long legacyHash(const std::string& text) {
	std::vector<char> padded(text.size() + 8, 0);
	std::memcpy(padded.data(), text.data(), text.size());
	static const int andAway[3] = {0x000000ff, 0x0000ffff, 0x00ffffff};
	const int length = static_cast<int>(text.size());
	Stuff::IteratorPosition ret = 0;
	int i, r, len = length >> 2;
	for (i = 0; i < len; i++) {
		std::memcpy(&r, padded.data() + 4 * i, sizeof(r));
		r = r >> (i & 31);	// x64 sar: the count is masked to five bits
		ret = static_cast<Stuff::IteratorPosition>(static_cast<unsigned>(ret) + static_cast<unsigned>(r));
	}
	int word;
	std::memcpy(&word, padded.data() + 4 * i, sizeof(word));
	ret += length & 0x3 ? word & andAway[length & 0x3 - 1] : 0;
	return static_cast<long>(ret & 0x7fffffff);
}

} // namespace

TEST_CASE("CastCensus/MString hash equals the legacy formula for lengths 0..200") {
	std::string text;
	for (int length = 0; length <= 200; ++length) {
		Stuff::MString value(text.c_str());
		CHECK_MESSAGE(static_cast<long>(value.GetHashValue()) == legacyHash(text), "length " << length);
		text += static_cast<char>(0x21 + (length * 37) % 94) ;
		if (length % 7 == 3)
			text.back() = static_cast<char>(0xC3);	// a high byte: sign-extends in the arithmetic shift
	}
}

TEST_CASE("CastCensus/double2long rounds half to even through the representation bits") {
	const double values[] = {0.0, 0.4, 0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 3.49999, 1234567.5, -1234567.5, 2147483647.0, -2147483648.0, 99.9};
	for (double v : values)
		CHECK_MESSAGE(double2long(v) == static_cast<int>(std::nearbyint(v)), "value " << v);
}
