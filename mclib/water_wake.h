#pragma once
#include <cmath>
#include <algorithm>

namespace mc2::waterreactive {
inline float wreckSinkDepth(float depth, bool destroyed, bool wet, float dt, float extent) {
    return destroyed && wet ? std::clamp(depth + std::max(dt, 0.0f) * 8.0f,
                                       0.0f, std::max(32.0f, extent * 2.0f)) : 0.0f;
}
inline bool shouldEmit(bool live, float vx, float vy) {
    return live && std::isfinite(vx) && std::isfinite(vy) && vx * vx + vy * vy > 1.0f;
}
}
