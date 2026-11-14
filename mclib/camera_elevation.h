#pragma once
#include <algorithm>
#include <cmath>

namespace mc2cam {
// Wheel notches are events, so use the legacy 60 Hz step independent of frame time.
inline float wheelZoomAmount(float increment, float baseFrameLength, float scale) {
    return increment * 18.0f * scale / (60.0f * baseFrameLength);
}

inline float zoomElevationTarget(float target, float bound, float amount, float scale, float momentum) {
    const float fraction = std::clamp(amount / std::max(scale, 0.01f) * 0.045f
        * (1.0f + momentum * 0.3f), 0.0f, 0.25f);
    return target + (bound - target) * fraction;
}

// The far-zoom scale approaches zero; a stalled frame must not fling the camera.
inline float panStep(float increment, float scale, float dt, float baseFrameLength) {
    return increment / std::max(scale, 0.1f) * std::clamp(dt, 0.0f, 0.05f) / baseFrameLength;
}

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
