#pragma once
#include <algorithm>
#include "../mclib/math/glm_config.h"

// Correct the locked simulation trajectory toward the live visual target.
// Flight lifetime and damage remain owned by WeaponBolt::update.
inline glm::vec3 missilePresentationPosition(glm::vec3 flight, const glm::vec3& launch,
    const glm::vec3& aim, const glm::vec3& liveTarget)
{
    const float range = glm::length(glm::vec2(aim - launch));
    const float progress = range > 0.001f
        ? std::clamp(glm::length(glm::vec2(flight - launch)) / range, 0.0f, 1.0f) : 1.0f;
    flight += (liveTarget - aim) * progress;
    return flight;
}
