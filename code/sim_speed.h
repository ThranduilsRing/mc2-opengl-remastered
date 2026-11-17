#pragma once
#include <algorithm>
#include "gameos.hpp"

namespace mc2::sim {
inline float speedClamp(float speed) { return std::clamp(speed, 0.25f, 4.f); }
inline float speedEffective(float speed, bool multiplayer) { return multiplayer ? 1.f : speed; }
inline float speedSet(float current, float requested, bool multiplayer) {
    return multiplayer ? current : speedClamp(requested);
}
inline float speedCycle(float speed, bool down, bool multiplayer) {
    if (multiplayer) return speed;
    return down ? (speed > 2.f ? 2.f : speed > 1.f ? 1.f : speed > 0.5f ? 0.5f : 4.f)
        : (speed < 0.5f ? 0.5f : speed < 1.f ? 1.f : speed < 2.f ? 2.f : speed < 4.f ? 4.f : 0.5f);
}
// Keyboard stepping shares the button's steps, but stops at either end.
inline float speedKeyStep(float speed, long key, bool multiplayer) {
    const bool down = key == KEY_MINUS || key == KEY_SUBTRACT;
    if (!down && key != KEY_ADD && key != KEY_EQUALS && key != (0x01000000 | KEY_EQUALS))
        return speed;
    const float next = speedCycle(speed, down, multiplayer);
    return down ? std::min(speed, next) : std::max(speed, next);
}
}
