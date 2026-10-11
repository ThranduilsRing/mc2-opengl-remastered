// SAVE-ZERO-1: MC2_SAVE_STRUCT gives a save struct whose padding is zero even on a dirty stack, without disturbing constructor-set defaults.
// Save writes sizeof(struct) bytes of a stack object; padding bytes between members used to carry stack leftovers into the file.
#include "doctest.h"
#include "save_struct_zero.h"

#include <cstddef>
#include <cstring>

namespace {

struct Padded {
	bool flag;           // 3 bytes of padding follow
	int value;
	char tag;            // 3 bytes of tail padding
	Padded() : flag(true), value(-1), tag('x') {}   // like the engine's save structs, members have non-zero constructor defaults
};

// Leave a recognisable pattern in the stack region the next call will reuse.
void dirtyStack() {
	volatile unsigned char junk[512];
	for (volatile unsigned char& b : junk)
		b = 0xCD;
}

bool paddingIsZero(const Padded& p) {
	const unsigned char* raw = reinterpret_cast<const unsigned char*>(&p);
	for (size_t i = offsetof(Padded, flag) + sizeof(bool); i < offsetof(Padded, value); ++i)
		if (raw[i])
			return false;
	for (size_t i = offsetof(Padded, tag) + sizeof(char); i < sizeof(Padded); ++i)
		if (raw[i])
			return false;
	return true;
}

bool buildOnDirtyStack(bool& defaultsKept) {
	dirtyStack();
	MC2_SAVE_STRUCT(Padded, p);
	defaultsKept = p.flag && p.value == -1 && p.tag == 'x';
	return paddingIsZero(p);
}

} // namespace

TEST_CASE("MC2_SAVE_STRUCT: padding is zero on a dirty stack and constructor defaults survive") {
	for (int i = 0; i < 4; ++i) {
		bool kept = false;
		CHECK(buildOnDirtyStack(kept));
		CHECK(kept);
	}
}
