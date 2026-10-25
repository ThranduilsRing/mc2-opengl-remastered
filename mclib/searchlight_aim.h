#pragma once
#include <algorithm>
#include <cmath>

namespace mc2::searchlight {
inline float approachAngle(float current, float desired, float speed, float dt)
{
    const float error = std::remainder(desired - current, 360.0f);
    return std::remainder(current + std::clamp(error * (1.0f - std::exp(-dt / 0.9f)),
        -speed * dt, speed * dt), 360.0f);
}
}
