#pragma once
#include <cmath>
#include <glm/glm.hpp>

// Gameplay coordinates are XY horizontal, Z height; range is in world units.
inline bool jumpTargetValid(bool capable, bool passable, const glm::vec3& from,
                            const glm::vec3& to, float range)
{
    return capable && passable && range > 0.0f &&
        std::hypot(from.x - to.x, from.y - to.y) <= range;
}
