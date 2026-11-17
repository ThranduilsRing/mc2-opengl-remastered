#pragma once
#include "math/glm_config.h"
#include <array>
#include <cmath>
#include <vector>

namespace mc2 {
// Raw MC2 world, row-vector reverse-Z camera matrix. Unproject the four
// viewport corners at the near edge and a finite depth along each ray.
// Intersect their half-spaces with the look-at plane, starting at the map
// bounds: this also clips rays above the horizon without enormous endpoints
// or folding a corner behind the camera. At most nine vertices survive.
inline std::vector<glm::dvec3> tacmapFootprint(const glm::mat4& worldToClip,
                                             double elevation, double halfMap)
{
    const glm::dmat4 matrix(worldToClip);
    if (!std::isfinite(glm::determinant(matrix)) || glm::determinant(matrix) == 0.0)
        return {};
    const glm::dmat4 inverse = glm::inverse(matrix);
    const auto unproject = [&](double x, double y, double z) {
        const glm::dvec4 p = glm::dvec4(x, y, z, 1.0) * inverse;
        return glm::dvec3(p) / p.w;
    };
    const std::array<glm::dvec2, 4> corners = {{{-1, 1}, {-1, -1}, {1, -1}, {1, 1}}};
    std::array<glm::dvec3, 4> near, along;
    for (int i = 0; i < 4; ++i) {
        near[i] = unproject(corners[i].x, corners[i].y, 1.0);
        along[i] = unproject(corners[i].x, corners[i].y, 0.5);
    }
    const glm::dvec3 inside = unproject(0, 0, 0.5);
    std::vector<glm::dvec3> polygon = {{-halfMap, -halfMap, elevation},
        {halfMap, -halfMap, elevation}, {halfMap, halfMap, elevation},
        {-halfMap, halfMap, elevation}};
    for (int edge = 0; edge < 5 && !polygon.empty(); ++edge) {
        const glm::dvec3 origin = near[edge % 4];
        glm::dvec3 normal = edge < 4
            ? glm::cross(near[(edge + 1) % 4] - origin, along[edge] - origin)
            : glm::cross(near[1] - near[0], near[2] - near[0]);
        if (glm::dot(normal, inside - origin) < 0) normal = -normal;
        std::vector<glm::dvec3> clipped;
        glm::dvec3 previous = polygon.back();
        double before = glm::dot(normal, previous - origin);
        for (const auto& current : polygon) {
            const double after = glm::dot(normal, current - origin);
            if ((before >= 0) != (after >= 0))
                clipped.push_back(previous + (current - previous) * (before / (before - after)));
            if (after >= 0) clipped.push_back(current);
            previous = current;
            before = after;
        }
        polygon = std::move(clipped);
    }
    return polygon;
}
} // namespace mc2
