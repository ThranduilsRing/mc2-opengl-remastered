#pragma once
// Simulation math v1: pinned OpenLibm 0.8.7 binary64 evaluation, then one binary32
// rounding for float callers. This deliberately replaces host-specific *f results.
// Contract: x64 IEEE arithmetic, round-to-nearest-even, gradual underflow, no x87,
// no fast math or FMA contraction (Mc2SimFpContract.cmake). Domain errors have a
// canonical quiet NaN; signed zero and infinities follow the pinned kernels.
// The kernels approximate the transcendentals; they are not correctly-rounded libm.
// Render-only camera, lighting, effects and UI continue to use their host libm.
#include "../../3rdparty/detmath/openlibm.h"

namespace mc2::sim::math {
inline double canonical(double x) {
    return std::isnan(x) ? std::bit_cast<double>(std::uint64_t{0x7ff8000000000000}) : x;
}
inline double sin(double x) { return canonical(detail::sin64(x)); }
inline double cos(double x) { return canonical(detail::cos64(x)); }
inline double acos(double x) { return canonical(detail::acos64(x)); }
inline double asin(double x) { return canonical(detail::asin64(x)); }
inline double atan2(double y, double x) { return canonical(detail::atan264(y, x)); }
inline double pow(double x, double y) { return canonical(detail::pow64(x, y)); }
inline double exp(double x) { return canonical(detail::exp64(x)); }
inline float sin(float x) { return static_cast<float>(sin(static_cast<double>(x))); }
inline float cos(float x) { return static_cast<float>(cos(static_cast<double>(x))); }
inline float acos(float x) { return static_cast<float>(acos(static_cast<double>(x))); }
inline float asin(float x) { return static_cast<float>(asin(static_cast<double>(x))); }
inline float atan2(float y, float x) { return static_cast<float>(atan2(static_cast<double>(y), static_cast<double>(x))); }
inline float pow(float x, float y) { return static_cast<float>(pow(static_cast<double>(x), static_cast<double>(y))); }
inline float exp(float x) { return static_cast<float>(exp(static_cast<double>(x))); }
} // namespace mc2::sim::math
