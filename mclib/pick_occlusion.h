#pragma once
#include <algorithm>
#include <cmath>
#include <limits>

class Camera;

namespace mc2 {
// Engine adapter shared with the terrain picker, without changing Camera's header.
bool pickScreenRay(Camera& camera, long x, long y, double (&origin)[3], double (&direction)[3]);

// Ray and bounds are in the object's unrotated MC2 frame. A projected rect
// alone does not say where along the cursor ray the object is encountered.
inline bool pickBoxHit(const double (&origin)[3], const double (&direction)[3],
                       const double (&lo)[3], const double (&hi)[3], double& hit)
{
    double first = 0, last = std::numeric_limits<double>::infinity();
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(origin[i]) || !std::isfinite(direction[i])
            || !std::isfinite(lo[i]) || !std::isfinite(hi[i])) return false;
        const double lower = std::min(lo[i], hi[i]), upper = std::max(lo[i], hi[i]);
        if (std::abs(direction[i]) < 1e-12) {
            if (origin[i] < lower || origin[i] > upper) return false;
        } else {
            double a = (lower-origin[i])/direction[i], b = (upper-origin[i])/direction[i];
            if (a > b) std::swap(a, b);
            first = std::max(first, a); last = std::min(last, b);
        }
        if (first > last) return false;
    }
    if (!std::isfinite(first) || last <= 0) return false;
    hit = first;
    return true;
}

inline bool pickBeforeSurface(double objectT, double surfaceT)
{
    return std::isfinite(objectT) && objectT >= 0 && objectT <= surfaceT + 1e-3;
}
} // namespace mc2
