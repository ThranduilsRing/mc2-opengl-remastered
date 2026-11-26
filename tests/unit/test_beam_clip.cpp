#include "doctest.h"
#include "../../code/beam_clip.h"
#include <limits>

TEST_CASE("beam_clip/screen projection matches Camera in both modes") {
    const glm::vec4 clip{0.2f, 0.6f, 0.5f, 2.0f};
    const glm::vec2 mul{1280, 720}, add{10, 20};
    const auto perspective = mc2::beam::screenPosition(clip, true, mul, add);
    CHECK(perspective.x == doctest::Approx(138));
    CHECK(perspective.y == doctest::Approx(236));
    const auto parallel = mc2::beam::screenPosition(clip, false, mul, add);
    CHECK(parallel.x == doctest::Approx(1034));
    CHECK(parallel.y == doctest::Approx(308));
}

namespace {
mc2::beam::Vertex at(float x, float y, float z, float w, float u) {
    return {{x, y, z, w}, {u, 0}, {255, 80, 40, 20}};
}
void bounded(const mc2::beam::Polygon& p) {
    REQUIRE(p.count >= 3);
    for (int i = 0; i < p.count; ++i) {
        const auto& v = p.vertices[i];
        CHECK(v.clip.w > 0);
        for (int axis = 0; axis < 3; ++axis) {
            CHECK(v.clip[axis] / v.clip.w >= -0.00001f);
            CHECK(v.clip[axis] / v.clip.w <= 1.00001f);
        }
        for (int channel = 0; channel < 4; ++channel)
            CHECK(v.argb[channel] == doctest::Approx(glm::vec4(255, 80, 40, 20)[channel]));
    }
}
}

TEST_CASE("beam_clip/real camera negative W survives for visible and offscreen firers") {
    const auto cameraVertex = [](float x, float y) {
        auto v = at(-x * 2, -y * 2, -1, -2, x);
        v.clip = mc2::beam::cameraClip(v.clip, true);
        return v;
    };
    for (float start : {0.2f, -1.0f}) {
        const auto p = mc2::beam::clipTriangle({cameraVertex(start, 0.4f),
            cameraVertex(0.6f, 0.4f), cameraVertex(0.6f, 0.6f)});
        bounded(p);
    }
    const glm::vec4 parallel{0.2f, 0.4f, 0.5f, 1.0f};
    CHECK(mc2::beam::cameraClip(parallel, false) == parallel);
}

TEST_CASE("beam_clip/culled firer ribbon enters the correct edge") {
    // Current world hardpoint projects left of the viewport; its target is visible.
    // The firer has no screen position. Both ribbon triangles must survive.
    const auto a = at(-1, 0.4f, 0.5f, 1, 0), b = at(-1, 0.6f, 0.5f, 1, 0);
    const auto c = at(0.5f, 0.4f, 0.5f, 1, 1), d = at(0.5f, 0.6f, 0.5f, 1, 1);
    for (const auto& triangle : {std::array<mc2::beam::Vertex, 3>{a, c, d}, {a, b, d}}) {
        const auto p = mc2::beam::clipTriangle(triangle);
        bounded(p);
        bool edge = false;
        for (int i = 0; i < p.count; ++i) {
            if (std::abs(p.vertices[i].clip.x) < 1e-6f) {
                edge = true;
                CHECK(p.vertices[i].uv.x == doctest::Approx(2.0f / 3));
                CHECK(p.vertices[i].clip.y >= 0.4f);
                CHECK(p.vertices[i].clip.y <= 0.6f);
            }
        }
        CHECK(edge);
    }
}

TEST_CASE("beam_clip/behind camera endpoint clips before perspective division") {
    const auto p = mc2::beam::clipTriangle({at(-1, -0.5f, -1, -1, 0),
        at(0.6f, 0.4f, 0.5f, 1, 1), at(0.6f, 0.6f, 0.5f, 1, 1)});
    bounded(p);
    CHECK(mc2::beam::clipTriangle({at(-1, -1, -1, -1, 0),
        at(-2, -1, -1, -1, 0), at(-1, -2, -1, -1, 0)}).count == 0);
    CHECK(mc2::beam::clipTriangle({at(std::numeric_limits<float>::infinity(), 0, 0, 1, 0),
        at(0, 0, 0, 1, 0), at(1, 1, 1, 1, 1)}).count == 0);
}
