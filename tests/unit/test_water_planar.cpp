#include "doctest.h"
#include "water_planar_view.h"
#include "water_planar_terrain.h"
#include "water_planar_props.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace wp = mc2::waterplanar;

TEST_CASE("water planar: weather ratio includes terrain rain multiplier") {
    const float baked = wp::weatherLight(60, 20, 0.6f);
    const float current = wp::weatherLight(45, 15, 0.8f);
    CHECK(current / baked == doctest::Approx(1.0f));
    CHECK(wp::weatherLight(60, 20, 1.5f) == 80);
    CHECK(wp::weatherLight(60, 20, 0) == 0);
    CHECK(wp::weatherLight(0, 0, 0.8f) == 0);
}

TEST_CASE("water planar: padded scissor covers ripple and bilinear lookup bounds") {
    const float bounds[4]{-0.5f,-0.25f,0.25f,0.5f};
    for (const auto size : {std::array<int,2>{3840,2160}, std::array<int,2>{1,1}, std::array<int,2>{1279,719}}) {
        const auto pixels=wp::paddedPixels(bounds,size[0],size[1]);
        for (int axis=0;axis<2;++axis) {
            for (const float offset : {-0.60f*0.015f,0.60f*0.015f}) {
                const float lo=(bounds[axis]*0.5f+0.5f+offset)*size[axis];
                const float hi=(bounds[axis+2]*0.5f+0.5f+offset)*size[axis];
                CHECK(pixels[axis]<=std::max(0,static_cast<int>(std::floor(lo-0.5f))));
                CHECK(pixels[axis+2]>=std::min(size[axis],static_cast<int>(std::ceil(hi+0.5f))));
            }
        }
    }
    const float full[4]{-10,-10,10,10};
    CHECK(wp::paddedPixels(full,3840,2160)==std::array<int,4>{0,0,3840,2160});
}

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

TEST_CASE("water planar: leaf sway crossing reflected side planes stays visible") {
    glm::mat4 main{1};
    main[2][2] = 0.1f; main[3][2] = -1;
    const wp::View view(glm::value_ptr(main), 20);
    const std::array<mc2hzb::PropBounds, 1> bounds{{{{0, 0, 0}, {0, 0, 0}, true}}};
    CHECK(wp::propIntersects(view, propAt(0, 2.4f, 0, 24), bounds));
    CHECK(wp::propIntersects(view, propAt(0, -2.4f, 0, 24), bounds));
    CHECK(wp::propIntersects(view, propAt(0, 0, 1.8f, 24), bounds));
    CHECK(wp::propIntersects(view, propAt(0, 0, -1.8f, 24), bounds));
    CHECK_FALSE(wp::propIntersects(view, propAt(0, 2.6f, 0, 24), bounds));
    CHECK_FALSE(wp::propIntersects(view, propAt(0, 0, 2.0f, 24), bounds));
}

TEST_CASE("water planar: geometry bounds include every packet and retain only unknown types") {
    struct Packet { uint32_t firstIndex, indexCount, baseVertex; };
    struct Geometry {
        uint32_t vertexStride;
        const Packet* packets;
        uint32_t packetCount;
        const uint32_t* typeFirstPacket;
        const uint32_t* typePacketCount;
        uint32_t typeCount;
    };
    std::array<std::array<float, 4>, 4> vertices{{
        {-0.1f, -0.1f, -0.1f, 99}, {0.1f, 0.1f, 0.1f, 99},
        {0.5f, 2, 3, 99}, {-0.5f, -2, -3, 99}}};
    std::array<uint32_t, 4> indices{0, 1, 0, 1};
    const std::array<Packet, 3> packets{{{0, 2, 0}, {2, 2, 2}, {4, 1, 0}}};
    const std::array<uint32_t, 4> first{0, 0, 2, 3}, count{2, 1, 1, 0};
    Geometry geom{sizeof(vertices[0]), packets.data(), 3, first.data(), count.data(), 4};
    auto bounds = wp::geometryBounds(geom, std::as_bytes(std::span(vertices)), indices);
    REQUIRE(bounds.size() == 4);
    CHECK(bounds[0].valid);
    CHECK(bounds[0].lo[2] == -3);
    CHECK(bounds[0].hi[1] == 2);
    CHECK(bounds[1].valid);
    CHECK_FALSE(bounds[2].valid); // malformed packet is unknown
    CHECK_FALSE(bounds[3].valid); // no geometry is unknown
    glm::mat4 main{1};
    main[2][2] = 0.1f; main[3][2] = -1;
    const wp::View view(glm::value_ptr(main), 20);
    CHECK(wp::propIntersects(view, propAt(1, 0, 0, 24), bounds));
    CHECK_FALSE(wp::propIntersects(view, propAt(1, 3, 0, 24), bounds));
    CHECK_FALSE(wp::propIntersects(view, propAt(1, 0, 0, 14), bounds));
    CHECK(wp::propIntersects(view, propAt(2, 3, 0, 14), bounds));
    vertices[0][0] = std::numeric_limits<float>::infinity();
    bounds = wp::geometryBounds(geom, std::as_bytes(std::span(vertices)), indices);
    CHECK_FALSE(bounds[1].valid);
    indices[0] = 100;
    bounds = wp::geometryBounds(geom, std::as_bytes(std::span(vertices)), indices);
    CHECK_FALSE(bounds[0].valid);
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
    wp::ReflectionPropsCache cache;
    cache.collect(std::span<const GpuStaticPropInstance>(recipes),
        std::span<const RecipeRange>(ranges), 1, view, {}, out);
    REQUIRE(out.size() == 3);
    CHECK(out[0].typeID == 1);
    CHECK(out[1].typeID == 3);
    CHECK(out[2].typeID == 3);
    CHECK(out[1].actorTint == 123);
    CHECK(out[1].flags == 17);
    CHECK(out[2].actorTint == 456); // stable within a type, source order preserved
    CHECK(recipes[0].typeID == 3); // main/source recipes were not sorted in place
    cache.collect(std::span<const GpuStaticPropInstance>(recipes),
        std::span<const RecipeRange>{}, 1, view, {}, out);
    CHECK(out.empty()); // mission unload clears the previous reflected set
}

// Trigger paths: water_planar_props.h and gos_static_prop_registry.cpp.
TEST_CASE("water planar: reflection cache keys invalidate bounds order and membership") {
    glm::mat4 main{1};
    main[2][2] = 0.1f; main[3][2] = -1;
    wp::View view(glm::value_ptr(main), 20);
    std::array<GpuStaticPropInstance, 3> recipes{propAt(1, 0, 0, 24),
        propAt(0, 0, 0, 24), propAt(1, 3, 0, 24)};
    std::array<RecipeRange, 1> ranges{{{0, 3}}};
    std::array<mc2hzb::PropBounds, 2> bounds{{
        {{-.1f, -.1f, -.1f}, {.1f, .1f, .1f}, true},
        {{-.1f, -.1f, -.1f}, {.1f, .1f, .1f}, true}}};
    wp::ReflectionPropsCache cache;
    std::vector<GpuStaticPropInstance> out;
    uint64_t generation = 1;
    auto collect = [&] { return cache.collect(std::span<const GpuStaticPropInstance>(recipes),
        std::span<const RecipeRange>(ranges), generation, view, bounds, out); };
    REQUIRE(collect());
    REQUIRE(out.size() == 2);
    CHECK(out[0].typeID == 0);
    CHECK(out[1].typeID == 1);
    const auto* storage = out.data();
    const auto capacity = out.capacity();
    CHECK_FALSE(collect());
    CHECK(out.data() == storage);
    CHECK(out.capacity() == capacity);
    // View changes recull cached bounds; they do not rebuild registry order.
    main[3][0] = -3;
    view = wp::View(glm::value_ptr(main), 20);
    CHECK_FALSE(collect());
    REQUIRE(out.size() == 1);
    CHECK(out[0].modelMatrix[3] == recipes[2].modelMatrix[3]);
    main[3][0] = 0;
    view = wp::View(glm::value_ptr(main), 20);
    // A transform edit on the same allocation needs the registry revision.
    recipes[2] = propAt(1, 0, 0, 24);
    ++generation;
    CHECK(collect());
    REQUIRE(out.size() == 3);
    // Payload stays live even when no bounds/order key changes.
    recipes[2].actorTint = 123;
    recipes[2].lightDataIndex = 456;
    CHECK_FALSE(collect());
    CHECK(out[2].actorTint == 123);
    CHECK(out[2].lightDataIndex == 456);
    // Type ordering and geometry edits invalidate on the same allocations.
    recipes[0].typeID = 0;
    ++generation;
    CHECK(collect());
    CHECK(out[0].typeID == 0);
    bounds[0].lo[0] = 5;
    bounds[0].hi[0] = 6;
    CHECK(collect());
    REQUIRE(out.size() == 1);
    bounds[0].valid = false; // unknown geometry remains conservative
    CHECK(collect());
    CHECK(out.size() == 3);
    ranges[0].live = false; // expired TGL borrow without a recipe revision
    CHECK_FALSE(collect());
    CHECK(out.empty());
    ranges[0].count = 0;
    ++generation;
    CHECK(collect());
    CHECK(out.empty());
    ranges[0] = {0, 3}; // recycled recipe range
    ++generation;
    CHECK(collect());
    CHECK(out.size() == 3);
    const auto relocated = recipes;
    CHECK(cache.collect(std::span<const GpuStaticPropInstance>(relocated),
        std::span<const RecipeRange>(ranges), generation, view, bounds, out));
    CHECK(cache.collect(std::span<const GpuStaticPropInstance>{},
        std::span<const RecipeRange>{}, generation, view, bounds, out));
    CHECK(out.empty());
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

TEST_CASE("water planar: each reflection quality level changes the bake and the uploaded march steps") {
    const auto low = wp::bakePlan(1), medium = wp::bakePlan(2), high = wp::bakePlan(3);
    // High is the shipped look: full-size bake, props, 8 march steps.
    CHECK(wp::bakeSize(1000, high) == 1000);
    CHECK(high.props); CHECK(high.marchSteps == 8);
    // Each lower level is strictly cheaper on every axis the levels own.
    CHECK(wp::bakeSize(1000, medium) < wp::bakeSize(1000, high));
    CHECK(wp::bakeSize(1000, low) < wp::bakeSize(1000, medium));
    CHECK(medium.props); CHECK_FALSE(low.props);
    CHECK(medium.marchSteps < high.marchSteps);
    CHECK(low.marchSteps < medium.marchSteps);
    CHECK(wp::bakeSize(1, low) == 1);
    CHECK(wp::bakePlan(99).marchSteps == high.marchSteps);
    CHECK(wp::bakePlan(0).marchSteps == low.marchSteps);   // bakeLevel clamps to 1..3
}

TEST_CASE("water planar: High draws the live planar reflection, Low and Medium the bake") {
    CHECK(wp::livePlanar(3));
    CHECK_FALSE(wp::livePlanar(2));
    CHECK_FALSE(wp::livePlanar(1));
    CHECK_FALSE(wp::livePlanar(0));
}

TEST_CASE("water planar: Off is sky-only, never level 0, and never the full bake") {
    CHECK(wp::effectiveReflectionQuality(0, 2) == 1);
    CHECK(wp::effectiveReflectionQuality(3, 2) == 2);
    CHECK(wp::effectiveReflectionQuality(1, 2) == 2);
    CHECK(wp::effectiveReflectionQuality(3, 0) == 0);
    CHECK(wp::effectiveReflectionQuality(0, 2) == 1);  // Off + unset legacy (defaults 2) keeps sky
    CHECK(wp::effectiveReflectionQuality(0, 0) == 0);  // explicit legacy Off
}

TEST_CASE("water planar: the uploaded debug.z carries each level's march steps and the shader fallback is High") {
    CHECK(wp::uploadedDebugZ(wp::bakePlan(1)) == 4.0f);
    CHECK(wp::uploadedDebugZ(wp::bakePlan(2)) == 6.0f);
    CHECK(wp::uploadedDebugZ(wp::bakePlan(3)) == static_cast<float>(wp::kHighSteps));
    std::ifstream input(std::filesystem::path(MC2_REPO_ROOT) / "shaders/vulkan/water_geom.frag");
    REQUIRE(input.good());
    std::ostringstream text; text << input.rdbuf();
    const auto src = text.str();
    const auto at = src.find("int steps = F.debug.z > 0.5 ? int(F.debug.z) : ");
    REQUIRE(at != std::string::npos);
    CHECK(std::atoi(src.c_str() + at + std::strlen("int steps = F.debug.z > 0.5 ? int(F.debug.z) : ")) == wp::kHighSteps);
}

// Trigger paths: water_planar_props.h and gos_static_prop_registry.cpp.
// Reference = the pre-grid collect: stable type order, per-entry multi() and view test.
TEST_CASE("water planar: grid collect matches the linear reference bit for bit") {
    constexpr uint32_t kTypes = 9;
    std::vector<mc2hzb::PropBounds> bounds(kTypes);
    for (uint32_t t = 0; t < kTypes; ++t)
        bounds[t] = {{-2.f - t, -1.f, -2.f}, {2.f + t, 3.f + t, 2.f}, t != 4};   // type 4: unknown bounds
    uint32_t seed = 12345;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return static_cast<float>(seed >> 8) / 16777216.0f; };
    std::vector<GpuStaticPropInstance> recipes;
    for (uint32_t i = 0; i < 3000; ++i) {
        recipes.push_back(propAt(static_cast<uint32_t>(rnd() * kTypes), (rnd() - 0.5f) * 800, (rnd() - 0.5f) * 800, rnd() * 40));
        recipes.back().actorTint = i;   // identity marker: checks order and payload
    }
    std::vector<RecipeRange> ranges{{0, 1000}, {1000, 500, false}, {1500, 700}, {2200, 0}, {2200, 800}};
    std::vector<GpuStaticPropInstance> out, expect;
    wp::ReflectionPropsCache cache;
    size_t partial = 0, total = 0;
    for (int v = 0; v < 24; ++v) {
        glm::mat4 main{1};
        const float s = 0.002f * (1.0f + 6.0f * rnd());
        main[0][0] = s; main[1][1] = s; main[2][2] = 0.001f + 0.01f * rnd();
        main[3][0] = (rnd() - 0.5f) * 1.5f; main[3][1] = (rnd() - 0.5f) * 1.5f; main[3][2] = -1.0f * rnd();
        if (v == 0) { main[0][0] = main[1][1] = 1e-5f; main[3][0] = main[3][1] = 0; }   // everything inside
        const float water = 10.0f + 20.0f * rnd();
        const wp::View view(glm::value_ptr(main), water);
        struct Ent { size_t recipe, range; };
        std::vector<Ent> all;
        for (size_t r = 0; r < ranges.size(); ++r)
            for (size_t i = ranges[r].begin; i < ranges[r].begin + ranges[r].count; ++i) all.push_back({i, r});
        std::stable_sort(all.begin(), all.end(), [&](const Ent& a, const Ent& b) { return recipes[a.recipe].typeID < recipes[b.recipe].typeID; });
        expect.clear();
        for (const auto& e : all)
            if (ranges[e.range].multi() && wp::propIntersects(view, recipes[e.recipe], bounds)) expect.push_back(recipes[e.recipe]);
        cache.collect(std::span<const GpuStaticPropInstance>(recipes), std::span<const RecipeRange>(ranges), 1, view, bounds, out);
        REQUIRE(out.size() == expect.size());
        CHECK(std::memcmp(out.data(), expect.data(), out.size() * sizeof(out[0])) == 0);
        partial += !out.empty() && out.size() < all.size() - 500;
        total += out.size();
    }
    CHECK(partial >= 3);   // views that cull part of the set exercise cells and the per-entry path
    CHECK(total > 0);
}
