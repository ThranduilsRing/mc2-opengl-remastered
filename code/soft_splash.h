#pragma once
#include <cmath>

namespace soft_splash {
inline bool eligible(bool missile, bool beam, float areaDamage, float areaRadius)
{
    return missile && !beam && !(areaDamage > 0.0f && areaRadius > 0.0f);
}
inline bool candidate(bool marine, bool inactive, bool direct, bool friendly)
{
    return marine && !inactive && !direct && !friendly;
}
inline float damage(float shotDamage, float distanceSquared)
{
    if (!(distanceSquared >= 0.0f && distanceSquared <= 64.0f * 64.0f)) return 0.0f;
    const float result = shotDamage * 0.5f * (1.0f - std::sqrt(distanceSquared) / 64.0f);
    return result >= 0.25f ? result : 0.0f;
}
}
