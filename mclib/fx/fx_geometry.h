#pragma once
#include "fx_runtime.h"

namespace mc2::fx {
enum class GeometryKind { points, triangles, quads, mesh };
struct Vertex {
    glm::vec3 position{};
    glm::vec4 color{};
    glm::vec2 uv{};
};
struct Geometry {
    GeometryKind kind = GeometryKind::triangles;
    const EffectRecord* effect = nullptr;
    // Root-to-leaf authored states for the render boundary to compose.
    std::vector<const Material*> materials;
    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;
    const MeshShape* shape = nullptr;
    glm::mat4 model{1.0f};
    glm::vec3 scale{1.0f};
    glm::vec4 color{1.0f};
};
// Pure frame extraction: neither playback state nor the random stream is mutable.
std::vector<Geometry> generate_geometry(const Instance& effect, const glm::mat4& parent_to_world,
    const glm::vec3& camera_in_world);
}
