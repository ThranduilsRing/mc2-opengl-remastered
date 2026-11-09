#pragma once
#include <algorithm>
#include <cmath>

namespace mc2::explosion_light {
struct Profile {
    int effect;
    float referenceRadius;
    float strength;
    float reach;
};
// Stock effect IDs (weaponfx.h). World-unit radii, dimensionless legacy light
// intensity. Keep class tuning together; stock effect files remain untouched.
inline constexpr Profile profiles[] = {
    {40, 24.0f, 0.14f, 0.55f}, // vehicle
    {46, 24.0f, 0.14f, 0.55f}, // vehicle with splash damage
    {44, 32.0f, 0.20f, 0.65f}, // turret
    {45, 96.0f, 0.32f, 0.85f}, // building / gate
};
inline constexpr float peakCeiling = 0.28f;
inline constexpr float coreShare = 0.05f;
inline constexpr float reachPerRadius = 8.0f;

struct Light { float intensity, inner, outer; };
inline Light scale(int effect, float sourceRadius, float intensity, float inner, float outer) {
    const Profile* profile = nullptr;
    for (const auto& p : profiles)
        if (p.effect == effect) { profile = &p; break; }
    if (!profile) return {intensity, inner, outer};

    // Older saves and callers without geometry use the class reference size.
    const float radius = std::isfinite(sourceRadius) && sourceRadius > 0.0f
        ? sourceRadius : profile->referenceRadius;
    const float size = std::sqrt(std::clamp(radius / profile->referenceRadius, 0.25f, 4.0f));
    const float energy = std::isfinite(intensity) ? std::max(intensity, 0.0f) : 0.0f;
    const float drive = energy * profile->strength * size;
    // Rational shoulder retains ordering/decay, including authored values > 1,
    // rather than TG_Light::SetIntensity's hard clamp at unity.
    const float peak = peakCeiling * (drive / (peakCeiling + drive));
    const float authoredReach = std::isfinite(outer) ? std::max(outer, 0.0f) : 0.0f;
    const float limit = profile->referenceRadius * size * reachPerRadius;
    const float reach = std::min(authoredReach, limit * 64.0f) * profile->reach * size;
    const float outerRadius = std::max(1.0f, limit * (reach / (limit + reach)));
    // A broad constant-intensity core was the white disc. Keep only a small
    // source core, with the existing smooth GPU falloff covering the rest.
    const float authoredCore = std::isfinite(inner) ? std::max(inner, 0.0f) : 0.0f;
    return {peak, std::min(authoredCore * size, outerRadius * coreShare), outerRadius};
}
} // namespace mc2::explosion_light
