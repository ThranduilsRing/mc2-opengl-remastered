#pragma once
#include "gos_terrain_lighting.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace mc2::beam_lights {
inline constexpr unsigned kBudget = 32;
inline constexpr float kImpactDuration = 0.16f;

// Render-frame values only: no registry slots, persistent emitters or sim state.
struct Segment {
    std::array<float, 3> start{}, end{};
    uint32_t rgb = 0;
    float remaining = 0;
    bool impact = false;
};

class Pool {
public:
    std::array<gos_terrain_lighting::GpuTerrainLight, kBudget> lights{};
    unsigned count = 0;

    template<class Visible>
    void add(const Segment& beam, Visible visible) {
        const float fade = std::clamp(beam.remaining / kImpactDuration, 0.0f, 1.0f);
        if (!(fade > 0)) return;
        float length2 = 0;
        for (unsigned a = 0; a < 3; ++a) {
            const float d = beam.end[a] - beam.start[a];
            length2 += d * d;
        }
        if (!std::isfinite(length2)) return;
        const unsigned n = beam.impact ? 1u : static_cast<unsigned>(
            std::clamp(std::ceil(std::sqrt(length2) / 55.0f), 1.0f, 6.0f));
        for (unsigned i = 0; i < n; ++i) {
            gos_terrain_lighting::GpuTerrainLight light{};
            const float t = beam.impact ? 1.0f : (i + 0.5f) / n;
            for (unsigned a = 0; a < 3; ++a)
                light.position[a] = beam.start[a] + (beam.end[a] - beam.start[a]) * t;
            if (!visible(light.position)) continue;
            light.lightType = 1;
            light.closeDistance = 8.0f;
            light.farDistance = beam.impact ? 85.0f : 70.0f;
            light.oneOverDistance = 1.0f / (light.farDistance - light.closeDistance);
            // Fold radiance into color, allowing max-channel merging without whiteout.
            const float strength = (beam.impact ? 0.30f : 0.12f) * fade;
            for (unsigned a = 0; a < 3; ++a)
                light.color[a] = ((beam.rgb >> (16 - 8 * a)) & 255u) / 255.0f * strength;
            light.intensity = 1.0f;
            bool merged = false;
            for (unsigned j = 0; j < count; ++j) {
                auto& old = lights[j];
                float d2 = 0;
                for (unsigned a = 0; a < 3; ++a) {
                    const float d = old.position[a] - light.position[a];
                    d2 += d * d;
                }
                if (d2 > 24.0f * 24.0f) continue;
                for (unsigned a = 0; a < 3; ++a)
                    old.color[a] = std::max(old.color[a], light.color[a]);
                old.farDistance = std::max(old.farDistance, light.farDistance);
                old.oneOverDistance = 1.0f / (old.farDistance - old.closeDistance);
                merged = true;
                break;
            }
            if (!merged && count < kBudget) lights[count++] = light;
        }
    }
};
// Main-thread extract hook: the terrain vertex pack precedes projectile packets.
// Append only after those packets exist, before the render frame is frozen.
void appendFrameLights();
} // namespace mc2::beam_lights
