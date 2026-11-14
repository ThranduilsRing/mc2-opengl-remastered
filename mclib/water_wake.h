#pragma once
#include <cmath>

namespace mc2::waterreactive {
inline bool shouldEmit(bool live, float vx, float vy) {
    return live && std::isfinite(vx) && std::isfinite(vy) && vx * vx + vy * vy > 1.0f;
}
}
