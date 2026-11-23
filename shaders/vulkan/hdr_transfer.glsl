// Scalar transfer functions shared with the CPU doctests.
#ifdef __cplusplus
#include <cmath>
namespace mc2vk::hdr {
using std::pow;
#define HDR_REAL double
#define HDR_INLINE inline
#else
#define HDR_REAL float
#define HDR_INLINE
#endif
HDR_INLINE HDR_REAL hdrDecode(HDR_REAL x) {
    return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
}
HDR_INLINE HDR_REAL hdrPq(HDR_REAL nits) {
    HDR_REAL p = pow(nits > 0.0 ? nits / 10000.0 : 0.0, 2610.0 / 16384.0);
    return pow((3424.0 / 4096.0 + (2413.0 / 128.0) * p) /
               (1.0 + (2392.0 / 128.0) * p), 2523.0 / 32.0);
}
HDR_INLINE HDR_REAL hdrPqInverse(HDR_REAL pq) {
    HDR_REAL p = pow(pq, 32.0 / 2523.0);
    HDR_REAL n = p - 3424.0 / 4096.0;
    return 10000.0 * pow((n > 0.0 ? n : 0.0) / (2413.0 / 128.0 - (2392.0 / 128.0) * p), 16384.0 / 2610.0);
}
// BT.2390 zero-black PQ Hermite EETF. Set source PQ maximum to 3*target-2*knee,
// so the standard knee KS=1.5*maxLum-0.5 lands at 0.75*peak in luminance.
// Preserve paper white even when the display's peak is close to paper white.
HDR_INLINE HDR_REAL hdrEetf(HDR_REAL y, HDR_REAL peak, HDR_REAL paper) {
    HDR_REAL knee = 0.75 * peak > 1.0 ? 0.75 * peak : 1.0;
    if (y <= knee) return y;
    if (peak <= knee) return peak;
    HDR_REAL pqPeak = hdrPq(peak * paper);
    HDR_REAL pqKnee = hdrPq(knee * paper);
    HDR_REAL sourceMax = 3.0 * pqPeak - 2.0 * pqKnee;
    HDR_REAL ks = pqKnee / sourceMax;
    HDR_REAL t = (hdrPq(y * paper) / sourceMax - ks) / (1.0 - ks);
    if (t >= 1.0) return peak;
    HDR_REAL t2 = t * t;
    HDR_REAL t3 = t2 * t;
    HDR_REAL mapped = (2.0 * t3 - 3.0 * t2 + 1.0) * ks +
        (t3 - 2.0 * t2 + t) * (1.0 - ks) + (-2.0 * t3 + 3.0 * t2) * pqPeak / sourceMax;
    HDR_REAL result = hdrPqInverse(mapped * sourceMax) / paper;
    return result < peak ? result : peak;
}
#undef HDR_REAL
#undef HDR_INLINE
#ifdef __cplusplus
}
#endif
