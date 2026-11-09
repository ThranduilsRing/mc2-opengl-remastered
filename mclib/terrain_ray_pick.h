#pragma once

#include <algorithm>
#include <cmath>

namespace mc2 {
// Walk front to back inside the map, then refine the first above/below bracket.
// Direction is unit length; spacing is at most half the finest height texel.
template<class Height>
bool firstTerrainHit(const double (&origin)[3], const double (&direction)[3],
                     const double (&lo)[2], const double (&hi)[2],
                     double spacing, Height height, double& hitT)
{
    if (!(spacing > 0) || direction[2] >= -1e-9) return false;
    double begin = 0;
    // The terrain's authored heights are nonnegative. Include negative terrain
    // too by walking to a plane one map width below zero.
    double end = (origin[2] + std::max(hi[0]-lo[0], hi[1]-lo[1])) / -direction[2];
    for (int axis = 0; axis < 2; ++axis) {
        if (std::abs(direction[axis]) < 1e-12) {
            if (origin[axis] < lo[axis] || origin[axis] > hi[axis]) return false;
        } else {
            double a = (lo[axis]-origin[axis])/direction[axis];
            double b = (hi[axis]-origin[axis])/direction[axis];
            if (a > b) std::swap(a, b);
            begin = std::max(begin, a);
            end = std::min(end, b);
        }
    }
    if (begin > end) return false;
    auto clearance = [&](double t) {
        return origin[2] + t*direction[2]
            - height(origin[0]+t*direction[0], origin[1]+t*direction[1]);
    };
    double previous = begin;
    // Starting below terrain at a clipped map edge is not a visible crossing.
    double previousHeight = clearance(previous);
    if (previousHeight == 0) { hitT = previous; return true; }
    for (double t = begin; t < end;) {
        t = std::min(t + spacing, end);
        const double currentHeight = clearance(t);
        if (previousHeight > 0 && currentHeight <= 0) {
            double a = previous, b = t;
            for (int i = 0; i < 24; ++i) {
                const double mid = (a+b)*0.5;
                if (clearance(mid) > 0) a = mid; else b = mid;
            }
            hitT = (a+b)*0.5;
            return true;
        }
        previous = t;
        previousHeight = currentHeight;
    }
    return false;
}
} // namespace mc2
