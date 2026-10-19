#pragma once
#include <algorithm>
#include <cmath>

namespace mc2::vegetation {
// Reserve the cap across the whole map before visiting cells. Stopping a dense
// north-to-south scan at the cap can leave the camera's entire region empty.
inline float placementStep(float mapSpan, float authoredStep, int density, int capacity) {
    const int maxClump = std::max(6, density / 2);
    const int side = std::max(1, int(std::sqrt(double(std::max(capacity, 1)) / maxClump)));
    return std::max(authoredStep, mapSpan / float(side + 1));
}
}
