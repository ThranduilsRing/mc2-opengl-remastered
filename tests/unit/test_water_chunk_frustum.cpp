#include "doctest.h"
#include "water_chunk_frustum.h"
#include <random>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace wf = mc2::waterfrustum;

namespace {
// water_cull_pack.comp.slang triangleVisible: the rule the GPU cull admits a triangle by.
bool triangleVisible(const glm::vec4& a, const glm::vec4& b, const glm::vec4& c) {
    if (a.w <= 1e-4f && b.w <= 1e-4f && c.w <= 1e-4f) return false;
    if (a.x < -a.w && b.x < -b.w && c.x < -c.w) return false;
    if (a.x >  a.w && b.x >  b.w && c.x >  c.w) return false;
    if (a.y < -a.w && b.y < -b.w && c.y < -c.w) return false;
    if (a.y >  a.w && b.y >  b.w && c.y >  c.w) return false;
    return true;
}
glm::mat4 camera(const glm::vec3& eye, const glm::vec3& at) {
    return glm::perspective(glm::radians(40.0f), 16.0f / 9.0f, 1.0f, 8000.0f) * glm::lookAt(eye, at, glm::vec3(0, 0, 1));
}
}  // namespace

TEST_CASE("water chunk frustum: whenever the cull admits a quad, the box test of a box containing it does not reject it") {
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    int admitted = 0;
    for (int trial = 0; trial < 3000; ++trial) {
        const glm::vec3 eye(unit(rng) * 2000, unit(rng) * 2000, 200 + 400 * (unit(rng) + 1));
        const glm::vec3 at(unit(rng) * 1500, unit(rng) * 1500, 0);
        if (glm::length(at - eye) < 1.0f) continue;
        const glm::mat4 m = camera(eye, at);
        const float elevation = unit(rng) * 20, amplitude = std::abs(unit(rng)) * 3;
        const float cx = unit(rng) * 1500, cy = unit(rng) * 1500, size = 8 + 600 * std::abs(unit(rng));
        glm::vec3 v[4];
        float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
        for (int i = 0; i < 4; ++i) {
            v[i] = glm::vec3(cx + ((i & 1) ? size : 0) * (1 + 0.2f * unit(rng)), cy + ((i & 2) ? size : 0) * (1 + 0.2f * unit(rng)),
                             elevation + (i & 1 ? amplitude : -amplitude));
            lo[0] = std::min(lo[0], v[i].x); hi[0] = std::max(hi[0], v[i].x);
            lo[1] = std::min(lo[1], v[i].y); hi[1] = std::max(hi[1], v[i].y);
        }
        const glm::vec4 c[4] = {m * glm::vec4(v[0], 1), m * glm::vec4(v[1], 1), m * glm::vec4(v[2], 1), m * glm::vec4(v[3], 1)};
        const bool gpu = triangleVisible(c[0], c[1], c[2]) || triangleVisible(c[0], c[2], c[3]);
        if (!gpu) continue;
        ++admitted;
        CHECK_FALSE(wf::boxOutside(glm::value_ptr(m), lo[0], lo[1], hi[0], hi[1], elevation, amplitude));
    }
    CHECK(admitted > 200);
}

TEST_CASE("water chunk frustum: behind the camera is outside, across the near plane is not") {
    const glm::mat4 m = camera(glm::vec3(0, 0, 100), glm::vec3(0, 500, 0));
    CHECK(wf::boxOutside(glm::value_ptr(m), -50, -900, 50, -600, 0, 2));      // wholly behind the eye
    CHECK_FALSE(wf::boxOutside(glm::value_ptr(m), -50, -300, 50, 300, 0, 2));  // straddles the eye plane
    CHECK_FALSE(wf::boxOutside(glm::value_ptr(m), -20, 300, 20, 360, 0, 2));   // straight ahead
    CHECK(wf::boxOutside(glm::value_ptr(m), 5000, 300, 5100, 360, 0, 2));      // far off to the side
}

TEST_CASE("water chunk frustum: any-chunk walk ignores empty chunks and an empty list") {
    struct Chunk { float minX, minY, maxX, maxY; std::vector<int> recipes; };
    const glm::mat4 m = camera(glm::vec3(0, 0, 100), glm::vec3(0, 500, 0));
    CHECK_FALSE(wf::anyChunkInView(glm::value_ptr(m), std::vector<Chunk>{}, 0, 2));
    CHECK_FALSE(wf::anyChunkInView(glm::value_ptr(m), std::vector<Chunk>{{-20, 300, 20, 360, {}}}, 0, 2));        // in view but empty
    CHECK_FALSE(wf::anyChunkInView(glm::value_ptr(m), std::vector<Chunk>{{-50, -900, 50, -600, {1}}}, 0, 2));      // behind
    CHECK(wf::anyChunkInView(glm::value_ptr(m), std::vector<Chunk>{{-50, -900, 50, -600, {1}}, {-20, 300, 20, 360, {1}}}, 0, 2));
}

TEST_CASE("water: a map whose water level is below all its terrain is dry (DarkRain torrin)") {
    struct V { unsigned char water; };
    const V dry[] = {{0x00}, {0x40}, {0x80}, {0xC0}};      // anim markers only, no underwater bit
    const V wet[] = {{0x40}, {0x81}, {0x00}};
    CHECK_FALSE(wf::anyWaterVertex(dry));
    CHECK(wf::anyWaterVertex(wet));
    CHECK_FALSE(wf::anyWaterVertex(std::span{wet, size_t{0}}));
}
