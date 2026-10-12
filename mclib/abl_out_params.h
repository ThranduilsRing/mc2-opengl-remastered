// WHAT: defined values for the real[3] out-parameters of ABL position builtins.
// WHY: a builtin that has no answer (the object is not a mover, or does not exist) must not leave the script's variable as
// it was: the script then reads whatever the variable held before, which in mc2_15 was garbage that grew a debug string past
// its char[50] declaration. The zero vector is the documented "no position" answer.
#pragma once

#include <span>

namespace abl {

inline void writeVec3(std::span<float, 3> out, float x, float y, float z) noexcept {
	out[0] = x;
	out[1] = y;
	out[2] = z;
}

inline void writeZeroVec3(std::span<float, 3> out) noexcept {
	writeVec3(out, 0.0f, 0.0f, 0.0f);
}

}  // namespace abl
