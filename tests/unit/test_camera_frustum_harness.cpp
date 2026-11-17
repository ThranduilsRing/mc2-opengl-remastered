#include "doctest.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "camera_frustum_math.h"
#include "tacmap_footprint.h"
#include "../../3rdparty/glm/glm/ext/matrix_transform.hpp"

// Trigger paths: mclib/tacmap_footprint.h, code/gametacmap.cpp.
TEST_CASE("tacmap_footprint/corner_unprojection") {
    glm::dmat4 projection(0.0);
    projection[0][0] = projection[1][1] = 1.0; // 90 degree FOV, square
    projection[2][3] = -1.0;
    projection[3][2] = 1.0; // infinite reverse-Z, near=1
    const auto camera = [&](glm::dvec3 eye, glm::dvec3 up) {
        return glm::mat4(glm::transpose(projection * glm::lookAt(eye, glm::dvec3(0), up)));
    };
    const auto top = camera({0, 0, 10}, {0, 1, 0});
    const auto square = mc2::tacmapFootprint(top, 0, 100);
    REQUIRE(square.size() == 4);
    for (const auto& p : square) {
        CHECK(std::abs(p.x) == doctest::Approx(10));
        CHECK(std::abs(p.y) == doctest::Approx(10));
        CHECK(p.z == 0);
    }
    SUBCASE("zoom and look-at elevation") {
        const auto smaller = mc2::tacmapFootprint(top, 5, 100);
        REQUIRE(smaller.size() == 4);
        for (const auto& p : smaller) {
            CHECK(std::abs(p.x) == doctest::Approx(5));
            CHECK(std::abs(p.y) == doctest::Approx(5));
        }
    }
    for (const glm::dvec3 eye : {glm::dvec3(0, -5, 10), glm::dvec3(5, 0, 10),
                                glm::dvec3(0, -10, 1), glm::dvec3(10, 0, 1)}) {
        const auto view = camera(eye, {0, 0, 1});
        const auto polygon = mc2::tacmapFootprint(view, 0, 100);
        REQUIRE(polygon.size() >= 3);
        CHECK(polygon.size() <= 9);
        bool mapEdge = false;
        for (const auto& p : polygon) {
            CHECK(std::isfinite(p.x));
            CHECK(std::isfinite(p.y));
            CHECK(std::abs(p.x) <= 100.00001);
            CHECK(std::abs(p.y) <= 100.00001);
            const glm::dvec4 clip = glm::dvec4(p, 1) * glm::dmat4(view);
            CHECK(clip.w > 0);
            CHECK(std::abs(clip.x / clip.w) <= 1.00001);
            CHECK(std::abs(clip.y / clip.w) <= 1.00001);
            CHECK(clip.z / clip.w <= 1.00001);
            mapEdge |= std::abs(p.x) > 99.999 || std::abs(p.y) > 99.999;
        }
        if (eye.z == 1) CHECK(mapEdge); // horizon rays stop at map bounds
    }
    CHECK(mc2::tacmapFootprint(glm::mat4(0), 0, 100).empty());
}

namespace {
namespace cf = camera_frustum_math;

static void unitFrustum(float planes[6][4]) {

    const float P[6][4] = {
        { 1, 0, 0, 1}, {-1, 0, 0, 1},
        { 0, 1, 0, 1}, { 0,-1, 0, 1},
        { 0, 0, 1, 1}, { 0, 0,-1, 1},
    };
    for (int i = 0; i < 6; ++i) for (int j = 0; j < 4; ++j) planes[i][j] = P[i][j];
}

TEST_CASE("camera_frustum_harness/aabb_inside_admitted") {
    float pl[6][4]; unitFrustum(pl);
    const float mn[3] = {-0.5f,-0.5f,-0.5f}, mx[3] = {0.5f,0.5f,0.5f};
    CHECK((cf::aabbInFrustum(pl, mn, mx)));
    return;
}

TEST_CASE("camera_frustum_harness/aabb_behind_one_plane_rejected") {
    float pl[6][4]; unitFrustum(pl);

    const float mn[3] = {2.0f,-0.5f,-0.5f}, mx[3] = {3.0f,0.5f,0.5f};
    CHECK((!cf::aabbInFrustum(pl, mn, mx)));
    return;
}

TEST_CASE("camera_frustum_harness/aabb_straddling_admitted") {
    float pl[6][4]; unitFrustum(pl);

    const float mn[3] = {0.5f,-0.5f,-0.5f}, mx[3] = {2.0f,0.5f,0.5f};
    CHECK((cf::aabbInFrustum(pl, mn, mx)));
    return;
}

TEST_CASE("camera_frustum_harness/aabb_zero_extent_no_nan") {
    float pl[6][4]; unitFrustum(pl);
    const float p[3] = {0.0f,0.0f,0.0f};
    CHECK((cf::aabbInFrustum(pl, p, p)));
    const float q[3] = {5.0f,5.0f,5.0f};
    CHECK((!cf::aabbInFrustum(pl, q, q)));
    return;
}

TEST_CASE("camera_frustum_harness/mirrored_lr_plane_rejects") {
    float pl[6][4]; unitFrustum(pl);

    pl[1][0] = 1.0f;
    pl[1][3] = -10.0f;
    const float mn[3] = {-0.5f,-0.5f,-0.5f}, mx[3] = {0.5f,0.5f,0.5f};
    CHECK((!cf::aabbInFrustum(pl, mn, mx)));
    return;
}

TEST_CASE("camera_frustum_harness/point_in_tri_inside") {

    CHECK((cf::pointInScreenTri(0,0, 10,0, 0,10, 2, 2)));
    return;
}

TEST_CASE("camera_frustum_harness/point_in_tri_outside") {
    CHECK((!cf::pointInScreenTri(0,0, 10,0, 0,10, 9, 9)));
    CHECK((!cf::pointInScreenTri(0,0, 10,0, 0,10, -1, -1)));
    return;
}

TEST_CASE("camera_frustum_harness/pillarbox_16x9") {
    int ox, oy, obw, obh;
    bool active = cf::compute43BoxRect(1920, 1080, &ox, &oy, &obw, &obh);
    CHECK((active));
    CHECK((obw == 1440 && obh == 1080));
    CHECK((ox == 240 && oy == 0));
    return;
}

TEST_CASE("camera_frustum_harness/box_4x3_is_noop") {
    int ox, oy, obw, obh;
    bool active = cf::compute43BoxRect(1024, 768, &ox, &oy, &obw, &obh);
    CHECK((!active));
    CHECK((obw == 1024 && obh == 768 && ox == 0 && oy == 0));
    return;
}

TEST_CASE("camera_frustum_harness/letterbox_taller_than_43") {
    int ox, oy, obw, obh;
    bool active = cf::compute43BoxRect(800, 900, &ox, &oy, &obw, &obh);
    CHECK((active));
    CHECK((obw == 800));
    CHECK((obh == (int)(800.0/(4.0/3.0)+0.5)));
    CHECK((oy == (900 - obh) / 2));
    return;
}

TEST_CASE("camera_frustum_harness/box_degenerate_no_div0") {
    int ox, oy, obw, obh;
    CHECK((!cf::compute43BoxRect(0, 0, &ox, &oy, &obw, &obh)));
    CHECK((!cf::compute43BoxRect(100, 0, &ox, &oy, &obw, &obh)));
    return;
}

TEST_CASE("camera_frustum_harness/demo_intentional_fail" * doctest::should_fail()) {
    int ox, oy, obw, obh;
    cf::compute43BoxRect(1920, 1080, &ox, &oy, &obw, &obh);
    CHECK((obw == 9999));
    return;
}

}
