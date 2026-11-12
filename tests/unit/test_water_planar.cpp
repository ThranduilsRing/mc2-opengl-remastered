#include "doctest.h"
#include "water_planar_view.h"
#include "water_planar_terrain.h"
#include "water_planar_props.h"
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace wp = mc2::waterplanar;

TEST_CASE("water planar: mirror fixes the plane and is an involution") {
    for (float water : {-350.0f, 0.0f, 350.0f}) {
        CHECK(wp::wp_mirror_z(water, water) == water);
        for (float z : {-700.0f, -1.0f, 0.0f, 350.0f, 1200.0f}) {
            CHECK(wp::wp_mirror_z(wp::wp_mirror_z(z, water), water) == doctest::Approx(z));
            CHECK(wp::wp_clip_distance(wp::wp_mirror_z(z, water), water)
                  == doctest::Approx(-wp::wp_clip_distance(z, water)));
        }
    }
}

TEST_CASE("water planar: clip original world heights including shoreline crossings") {
    CHECK(wp::wp_clip_distance(349.0f, 350.0f) < 0.0f);
    CHECK(wp::wp_clip_distance(350.0f, 350.0f) == 0.0f);
    CHECK(wp::wp_clip_distance(351.0f, 350.0f) > 0.0f);
    const float below = wp::wp_clip_distance(330.0f, 350.0f);
    const float above = wp::wp_clip_distance(390.0f, 350.0f);
    const float t = -below / (above - below);
    CHECK(330.0f + t * (390.0f - 330.0f) == doctest::Approx(350.0f));
}

TEST_CASE("water planar: compose reflection before an asymmetric perspective projection") {
    // Nonzero entries in every row expose transpose, wrong-side composition,
    // and forgetting to translate the mirror to the authored water elevation.
    const glm::mat4 main{{2, 1, 3, 4}, {5, 7, 11, 13}, {17, 19, 23, 29}, {31, 37, 41, 43}};
    const auto reflected = wp::reflectedWorldToClip(glm::value_ptr(main), 350.0f);
    const glm::mat4 matrix = glm::make_mat4(reflected.data());
    for (const glm::vec4 p : {glm::vec4(1, 2, 3, 1), glm::vec4(-5, 4, 350, 1), glm::vec4(7, -8, 900, 1)}) {
        const auto expected = main * glm::vec4(p.x, p.y, 700.0f - p.z, 1);
        const auto actual = matrix * p;
        for (int row = 0; row < 4; ++row) CHECK(actual[row] == doctest::Approx(expected[row]));
    }
    const auto twice = wp::reflectedWorldToClip(reflected.data(), 350.0f);
    for (int i = 0; i < 16; ++i) CHECK(twice[i] == doctest::Approx(glm::value_ptr(main)[i]));
}

TEST_CASE("water planar: one mirror reverses triangle winding") {
    const glm::mat4 identity(1);
    const auto mirror = wp::reflectedWorldToClip(glm::value_ptr(identity), 350);
    CHECK(glm::determinant(glm::make_mat4(mirror.data())) == doctest::Approx(-1));
    CHECK(wp::reversesWinding);
    // Project X,Z onto the image; the reflected triangle must have opposite area.
    const float before = (2.0f - 0.0f) * (360.0f - 350.0f);
    const float after = (2.0f - 0.0f) * (wp::wp_mirror_z(360, 350) - wp::wp_mirror_z(350, 350));
    CHECK(after == doctest::Approx(-before));
}

TEST_CASE("water planar: reflected cull admits geometry absent from the main view") {
    glm::mat4 main(1);
    main[2][2] = 0.1f;
    main[3][2] = -1; // main depth range is world z=10..20
    const wp::View view(glm::value_ptr(main), 20); // reflected world range is z=20..30
    CHECK(view.intersects({-0.5f, -0.5f, 22}, {0.5f, 0.5f, 25}));
    CHECK_FALSE(view.intersects({-0.5f, -0.5f, 15}, {0.5f, 0.5f, 19}));
    CHECK(view.intersects({-0.5f, -0.5f, 19}, {0.5f, 0.5f, 21}));
    CHECK(view.intersects({-0.5f, -0.5f, 20}, {0.5f, 0.5f, 20}));
    CHECK_FALSE(view.intersects({2, -0.5f, 22}, {3, 0.5f, 25}));
    CHECK_FALSE(view.intersects({-0.5f, -0.5f, 31}, {0.5f, 0.5f, 32}));
}

namespace {
struct TerrainFixture {
    std::vector<float> height = std::vector<float>(24 * 24, 10.0f);
    glm::mat4 main{1};
    Mc2TerrainTier1Feed source{};
    TerrainFixture() {
        main[0][0] = main[1][1] = 1.0f / 4096;
        main[2][2] = -0.01f;
        main[3][2] = 0.5f;
        source.mapSide = 24; source.halfMap = 1472;
        source.heightCpu = height.data(); source.heightBytes = height.size() * sizeof(float);
        source.heightVersion = 1;
        std::copy_n(glm::value_ptr(main), 16, source.worldToClipGL);
    }
};
}

TEST_CASE("water planar: terrain view selects the whole map independently of main patches") {
    TerrainFixture f;
    f.source.cameraPosMC2[0] = -100;
    f.source.cameraPosMC2[1] = 500;
    f.source.cameraPosMC2[2] = 200;
    // An empty main patch list must not remove reflected geometry.
    REQUIRE(f.source.patchCount == 0);
    wp::TerrainView view;
    REQUIRE(view.prepare(f.source, glm::value_ptr(f.main), 0));
    const auto& out = view.feed();
    REQUIRE(out.patchCount == 4);
    CHECK(out.heightCpu == f.source.heightCpu);
    CHECK(f.source.patchCount == 0);
    CHECK(f.source.patches == nullptr);
    CHECK(f.source.worldToClipGL[10] == f.main[2][2]);
    CHECK(out.cameraPosMC2[0] == -100);
    CHECK(out.cameraPosMC2[1] == -500);
    CHECK(out.cameraPosMC2[2] == 200);
    CHECK(f.source.cameraPosMC2[1] == 500);
    int totalQuads = 0;
    for (uint32_t i = 0; i < out.patchCount; ++i) {
        const auto& p = out.patches[i];
        totalQuads += p.quadCountX * p.quadCountY;
        CHECK(p.blockOriginX + p.quadCountX <= 23);
        CHECK(p.blockOriginY + p.quadCountY <= 23);
        CHECK(p.lodStep == 1);
        CHECK(p.visualMode == 0);
        CHECK(p.skirtDepth == 0);
        CHECK(p.skirtEdgeMask == 0);
        CHECK(p.edgeStitch == 0);
        CHECK(p.morph == 0);
    }
    CHECK(totalQuads == 23 * 23);
    CHECK(out.patches[3].quadCountX == 3);
    CHECK(out.patches[3].quadCountY == 3);
}

TEST_CASE("water planar: terrain views own separate patches and reject underwater terrain") {
    TerrainFixture f;
    wp::TerrainView a, b;
    REQUIRE(a.prepare(f.source, glm::value_ptr(f.main), 0));
    const auto* firstPatches = a.feed().patches;
    REQUIRE(b.prepare(f.source, glm::value_ptr(f.main), 20));
    CHECK(b.feed().patchCount == 0);
    CHECK(a.feed().patches == firstPatches);
    CHECK(a.feed().patchCount == 4);
    CHECK(a.feed().patches[3].blockOriginX == 20);
    // Same buffers, new version: a terrain edit changes the reflected set.
    std::fill(f.height.begin(), f.height.end(), -10);
    ++f.source.heightVersion;
    REQUIRE(a.prepare(f.source, glm::value_ptr(f.main), 0));
    CHECK(a.feed().patchCount == 0);
}

TEST_CASE("water planar: fine bake peak keeps reflected terrain despite submerged coarse heights") {
    TerrainFixture f;
    std::fill(f.height.begin(), f.height.end(), -10);
    const int fineSide = (24 - 1) * 4 + 1;
    std::vector<float> fine(static_cast<size_t>(fineSide) * fineSide, -10);
    fine[10 * fineSide + 10] = 10;
    f.source.visualSide = fineSide;
    f.source.visualHeightCpu = fine.data();
    f.source.visualHeightBytes = fine.size() * sizeof(float);
    f.source.visualHeightVersion = 1;
    f.source.visualDisplaceFar = 1;
    wp::TerrainView view;
    REQUIRE(view.prepare(f.source, glm::value_ptr(f.main), 0));
    REQUIRE(view.feed().patchCount == 1);
    CHECK(view.feed().patches[0].visualMode == 1);
    CHECK(view.feed().patches[0].blockOriginX == 0);
    CHECK(view.feed().patches[0].blockOriginY == 0);
    fine[10 * fineSide + 10] = -10;
    ++f.source.visualHeightVersion;
    REQUIRE(view.prepare(f.source, glm::value_ptr(f.main), 0));
    CHECK(view.feed().patchCount == 0);
}

TEST_CASE("water planar: invalid terrain clears a previous view and cannot reuse cached bounds") {
    TerrainFixture f;
    wp::TerrainView view;
    REQUIRE(view.prepare(f.source, glm::value_ptr(f.main), 0));
    --f.source.heightBytes;
    CHECK_FALSE(view.prepare(f.source, glm::value_ptr(f.main), 0));
    CHECK(view.feed().patchCount == 0);
    ++f.source.heightBytes;
    f.source.visualSide = 2; // mismatched bake dimensions
    CHECK_FALSE(view.prepare(f.source, glm::value_ptr(f.main), 0));
    f.source.visualSide = 0;
    f.height[0] = std::numeric_limits<float>::quiet_NaN();
    ++f.source.heightVersion;
    CHECK_FALSE(view.prepare(f.source, glm::value_ptr(f.main), 0));
    CHECK(view.feed().patchCount == 0);
    f.height[0] = 10;
    REQUIRE(view.prepare(f.source, glm::value_ptr(f.main), 0));
    CHECK(view.feed().patchCount == 4);
}

namespace {
GpuStaticPropInstance propAt(uint32_t type, float x, float y, float z) {
    GpuStaticPropInstance instance{};
    instance.modelMatrix[0] = instance.modelMatrix[5] = instance.modelMatrix[10] = instance.modelMatrix[15] = 1;
    struct Point { float x, y, z; } world{x, y, z};
    const auto stuff = mc2ToRender(world);
    instance.modelMatrix[3] = stuff.x;
    instance.modelMatrix[7] = stuff.y;
    instance.modelMatrix[11] = stuff.z;
    instance.typeID = type;
    return instance;
}

struct RecipeRange {
    uint32_t begin, count;
    bool live = true;
    bool visible = false;
    bool noShadow = false;
    bool multi() const { return live; }
};
}

TEST_CASE("water planar: prop bounds use Stuff transforms and reflected world Z") {
    glm::mat4 main{1};
    main[2][2] = 0.1f; main[3][2] = -1;
    const wp::View view(glm::value_ptr(main), 20);
    const std::array<mc2hzb::PropBounds, 1> bounds{{{{-0.1f, -0.1f, -0.1f}, {0.1f, 0.1f, 0.1f}, true}}};
    CHECK(wp::propIntersects(view, propAt(0, 0, 0, 24), bounds));
    CHECK_FALSE(wp::propIntersects(view, propAt(0, 0, 0, 14), bounds));
    CHECK_FALSE(wp::propIntersects(view, propAt(0, 3, 0, 24), bounds));
    CHECK_FALSE(wp::propIntersects(view, propAt(0, 0, 3, 24), bounds));
    CHECK(wp::propIntersects(view, propAt(0, 0, 0, 20), bounds)); // plane crossing retained
    auto nonuniform = propAt(0, 1.5f, 0, 24);
    nonuniform.modelMatrix[0] = 10;
    CHECK(wp::propIntersects(view, nonuniform, bounds)); // scaled box crosses viewport edge
}

TEST_CASE("water planar: unknown or invalid prop bounds retain candidates") {
    const glm::mat4 identity{1};
    const wp::View view(glm::value_ptr(identity), 0);
    const auto instance = propAt(0, 100, 100, 100);
    CHECK(wp::propIntersects(view, instance, {}));
    std::array<mc2hzb::PropBounds, 1> bounds{};
    CHECK(wp::propIntersects(view, instance, bounds));
    bounds[0].valid = true;
    bounds[0].lo[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(wp::propIntersects(view, instance, bounds));
}

TEST_CASE("water planar: full prop registry includes invisible alpha cards and rejects stale recipes") {
    const glm::mat4 identity{1};
    const wp::View view(glm::value_ptr(identity), 0);
    std::array<GpuStaticPropInstance, 5> recipes{propAt(3, 0, 0, 1), propAt(1, 0, 0, 1),
        propAt(3, 0, 0, 1), propAt(2, 0, 0, 1), propAt(4, 0, 0, 1)};
    recipes[0].actorTint = 123; recipes[0].flags = 17;
    recipes[2].actorTint = 456;
    const std::array<RecipeRange, 7> ranges{{{0, 1, true, false, true}, {1, 2}, {3, 1, false},
        {4, 0}, {4, UINT32_MAX}, {UINT32_MAX, 2}, {6, 1}}};
    std::vector<GpuStaticPropInstance> out;
    wp::collectReflectionInstances(std::span<const GpuStaticPropInstance>(recipes),
        std::span<const RecipeRange>(ranges), view, {}, out);
    REQUIRE(out.size() == 3);
    CHECK(out[0].typeID == 1);
    CHECK(out[1].typeID == 3);
    CHECK(out[2].typeID == 3);
    CHECK(out[1].actorTint == 123);
    CHECK(out[1].flags == 17);
    CHECK(out[2].actorTint == 456); // stable within a type, source order preserved
    CHECK(recipes[0].typeID == 3); // main/source recipes were not sorted in place
    wp::collectReflectionInstances(std::span<const GpuStaticPropInstance>(recipes),
        std::span<const RecipeRange>{}, view, {}, out);
    CHECK(out.empty()); // mission unload clears the previous reflected set
}

TEST_CASE("water planar: six native clip faces retain crossings and reject exterior boxes") {
    glm::mat4 main{1};
    main[2][2] = -1;
    const wp::View view(glm::value_ptr(main), 0); // reflected matrix is identity
    for (int axis = 0; axis < 3; ++axis) {
        for (int sign : {-1, 1}) {
            std::array<float, 3> lo{-0.25f, -0.25f, 0.25f}, hi{0.25f, 0.25f, 0.75f};
            const float face = axis == 2 && sign < 0 ? 0.0f : static_cast<float>(sign);
            lo[axis] = face - 0.1f; hi[axis] = face + 0.1f;
            CHECK(view.intersects(lo, hi));
            lo[axis] += sign * 0.3f; hi[axis] += sign * 0.3f;
            CHECK_FALSE(view.intersects(lo, hi));
        }
    }
}

TEST_CASE("water planar: cached bounds are reselected after camera and water changes") {
    TerrainFixture f;
    wp::TerrainView view;
    REQUIRE(view.prepare(f.source, glm::value_ptr(f.main), 0));
    REQUIRE(view.feed().patchCount == 4);
    f.main[3][0] = 10;
    REQUIRE(view.prepare(f.source, glm::value_ptr(f.main), 0));
    CHECK(view.feed().patchCount == 0);
    f.main[3][0] = 0;
    REQUIRE(view.prepare(f.source, glm::value_ptr(f.main), 20));
    CHECK(view.feed().patchCount == 0);
    REQUIRE(view.prepare(f.source, glm::value_ptr(f.main), 0));
    CHECK(view.feed().patchCount == 4);
}
