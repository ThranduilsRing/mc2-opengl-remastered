#pragma once
#include <array>
#include <cmath>
#include "math/glm_config.h"

namespace mc2::beam {
struct Vertex {
    glm::vec4 clip;
    glm::vec2 uv;
    glm::vec4 argb;
};
struct Polygon {
    std::array<Vertex, 10> vertices{};
    int count = 0;
};

inline glm::vec2 screenPosition(const glm::vec4& clip, bool perspective,
                               const glm::vec2& mul, const glm::vec2& add) {
    return (perspective ? glm::vec2(clip) / clip.w : glm::vec2(1.0f) - glm::vec2(clip)) * mul + add;
}

// Camera's legacy homogeneous volume is 0 <= x,y,z <= w. Clip before
// division: ScreenXY's behind-camera fallback is a HUD position, not a beam.
inline Polygon clipTriangle(const std::array<Vertex, 3>& triangle) {
    Polygon polygon;
    for (const auto& vertex : triangle) {
        for (int i = 0; i < 4; ++i)
            if (!std::isfinite(vertex.clip[i])) return {};
        polygon.vertices[polygon.count++] = vertex;
    }
    for (int plane = 0; plane < 7 && polygon.count; ++plane) {
        const auto distance = [plane](const glm::vec4& p) {
            if (plane == 6) return p.w - 1e-5f;
            return plane % 2 ? p.w - p[plane / 2] : p[plane / 2];
        };
        Polygon next;
        auto previous = polygon.vertices[polygon.count - 1];
        float before = distance(previous.clip);
        for (int i = 0; i < polygon.count; ++i) {
            const auto current = polygon.vertices[i];
            const float after = distance(current.clip);
            if ((before >= 0) != (after >= 0)) {
                const float t = before / (before - after);
                next.vertices[next.count++] = {
                    glm::mix(previous.clip, current.clip, t),
                    glm::mix(previous.uv, current.uv, t),
                    glm::mix(previous.argb, current.argb, t)};
            }
            if (after >= 0) next.vertices[next.count++] = current;
            previous = current;
            before = after;
        }
        polygon = next;
    }
    return polygon;
}
} // namespace mc2::beam
