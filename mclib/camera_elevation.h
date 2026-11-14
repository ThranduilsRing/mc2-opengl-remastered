#pragma once
#include <algorithm>
#include <cmath>

namespace mc2cam {
// Exact critically damped spring for a constant target over dt (seconds).
inline float easeElevation(float value, float target, float& velocity, float dt, float lo, float hi) {
    target = std::clamp(target, lo, hi);
    value = std::clamp(value, lo, hi);
    if (!(dt > 0.0f)) return value;
    constexpr float omega = 20.0f;
    const float offset = value - target;
    const float c = velocity + omega * offset;
    const float decay = std::exp(-omega * dt);
    const float next = target + (offset + c * dt) * decay;
    velocity = (velocity - omega * c * dt) * decay;
    if ((offset < 0 && next > target) || (offset > 0 && next < target) || next < lo || next > hi) {
        velocity = 0;
        return std::clamp(next, std::min(value, target), std::max(value, target));
    }
    return next;
}
}
