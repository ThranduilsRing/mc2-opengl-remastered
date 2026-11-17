#pragma once
// OpenLibm v0.8.7 (https://github.com/JuliaMath/openlibm/tree/v0.8.7/src).
// Double fdlibm kernels; original licenses retained below each source name.
// Adaptations: C++ namespaces/inline, bit_cast word access, no ELF aliases or long double.
// Only IEEE sqrt/floor/fabs/copysign remain compiler operations, never host transcendentals.
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
namespace mc2::sim::detail {
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
using u_int32_t = std::uint32_t;
using std::sqrt;
using std::floor;
using std::fabs;
using std::copysign;
double acos64(double x);
double asin64(double x);
double atan64(double x);
double atan264(double y, double x);
double sin64(double x);
double cos64(double x);
double kernelSin(double x, double y, int iy);
double kernelCos(double x, double y);
int reduce(double x, double* y);
int kernelReduce(double* x, double* y, int e0, int nx, int prec);
double exp64(double x);
double pow64(double x, double y);
double scale(double x, int n);
} // namespace mc2::sim::detail
