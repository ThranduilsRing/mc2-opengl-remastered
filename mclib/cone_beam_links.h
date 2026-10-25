#pragma once
#include <array>
#include <ostream>
#include <bitset>
#include <cstddef>

namespace mc2_cone_glow {
// Authored beam bases are the dark vertices sharing a triangle with an apex.
// Build every apex's links in one triangle pass; pose and vertex order stay live.
template<std::size_t Capacity, class Triangle>
auto beamBaseLinks(const bool (&apex)[Capacity], int vertexCount,
                   const Triangle* triangles, long triangleCount)
{
    std::array<std::bitset<Capacity>, Capacity> links{};
    for (long f = 0; f < triangleCount; ++f) {
        const auto& vertices = triangles[f].Vertices;
        for (int i = 0; i < 3; ++i) {
            const auto a = vertices[i];
            if (a >= static_cast<unsigned>(vertexCount) || !apex[a]) continue;
            for (int j = 0; j < 3; ++j) {
                const auto b = vertices[j];
                if (b < static_cast<unsigned>(vertexCount) && !apex[b]) links[a].set(b);
            }
        }
    }
    return links;
}
}
