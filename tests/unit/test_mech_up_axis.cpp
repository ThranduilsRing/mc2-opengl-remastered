// tests/unit/test_mech_up_axis.cpp
//
// Which component of the mech/prop shader "world normal" is UP? (lane mech-up-axis; settles the Fable P0
// claim "mech worldNormal is Z-up, so .y is north" against the mech_tier1.vert:197 comment "render space is y-up".)
//
// Path-faithful chain, each link taken from the engine source:
//   1. Mech3DAppearance (mclib/mech3d.cpp:458-463) builds shapeToWorld from a yaw about the Y axis
//      (quatFromEuler(0, yaw, 0)) and the translation mc2ToRender(position): the "Stuff" frame the bone
//      matrices live in is the Y-UP frame (x = -east, y = elevation, z = north).
//   2. worldToClipGL (mclib/camera_project.cpp:261, xform_conventions.h) takes a RAW MC2 world point
//      (x east, y north, z elevation): its first factor is makeAxisSwapMC2toGL. So the vertex shaders
//      (mech_tier1.vert:169-174, color_prop_tier1.vert:151/232) apply mc2ToRender to the bone-space result
//      to get back to MC2 world, for the position AND for the normal.
//   3. Hence worldNormal / nMC2 is in the MC2 world frame: UP IS .z, .y IS NORTH.
// The test replays 1-3 with a synthetic but path-faithful transform (no mech rest-pose fixture exists in the
// GL-free harness): a yawed placement with a sky-facing Y-up model normal, which is what the VBO stores.
#include "math/mc2_xform.h"
#include "doctest.h"
#include "xform_conventions.h"
#include "axis_convert.h"
#include <stuff/stuff.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cmath>

namespace {
// shapeToWorld as mech3d.cpp:458-463 composes it (column-vector glm form of the same rotation + translation).
glm::mat4 mechShapeToWorld(const glm::vec3& mc2Position, float yawDeg)
{
    const float yaw = yawDeg * 0.017453292519943295f;
    const glm::quat q = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));
    glm::mat4 m = glm::mat4_cast(q);
    glm::vec3 xlat;
    mc2ToRender(xlat, mc2Position);
    m[3] = glm::vec4(xlat, 1.0f);
    return m;
}
}

TEST_SUITE("MechUpAxis") {

TEST_CASE("mech: a sky-facing surface has +1 in .z of mc2ToRender(normal), .y is north") {
    const glm::vec3 mc2Pos(120.0f, -340.0f, 7.5f);   // x east, y north, z elevation
    for (float yaw : {0.0f, 37.0f, 90.0f, 215.0f}) {
        const glm::mat4 boneT = mechShapeToWorld(mc2Pos, yaw);

        // mech_tier1.vert:169: worldMC2 = mc2ToRender(boneT * position). Must land on the raw MC2 world point
        // that worldToClipGL expects: z = elevation.
        const glm::vec3 worldMC2 = mc2ToRender(glm::vec3(boneT * glm::vec4(0, 0, 0, 1)));
        CHECK(worldMC2.x == doctest::Approx(mc2Pos.x));
        CHECK(worldMC2.y == doctest::Approx(mc2Pos.y));
        CHECK(worldMC2.z == doctest::Approx(mc2Pos.z));

        // mech_tier1.vert:173-174: Y-up model normal (sky) -> normalStuff -> worldNormal.
        const glm::vec3 normalStuff = glm::mat3(boneT) * glm::vec3(0, 1, 0);
        const glm::vec3 worldNormal = glm::normalize(mc2ToRender(normalStuff));
        CHECK(worldNormal.z == doctest::Approx(1.0f));   // up
        CHECK(worldNormal.y == doctest::Approx(0.0f));   // north: what mech_tier1.vert:201 / frag:100 read as "up"
        CHECK(worldNormal.x == doctest::Approx(0.0f));
    }
}

TEST_CASE("mech: a north-facing side surface is +1 in .y (not up)") {
    const glm::mat4 boneT = mechShapeToWorld(glm::vec3(0.0f), 0.0f);
    // Model +z is north in the Y-up frame (x = -east, y = up, z = north).
    const glm::vec3 worldNormal = glm::normalize(mc2ToRender(glm::mat3(boneT) * glm::vec3(0, 0, 1)));
    CHECK(worldNormal.y == doctest::Approx(1.0f));
    CHECK(worldNormal.z == doctest::Approx(0.0f));
}

TEST_CASE("prop path: nMC2.z (prop_night_vk.glsl:18) is up for the same sky-facing surface") {
    // color_prop_tier1.vert:232: nMC2 = normalize(mc2ToRender(worldNormalStuff)); a sky-facing Stuff normal is (0,1,0).
    const glm::vec3 nMC2 = glm::normalize(mc2ToRender(glm::vec3(0, 1, 0)));
    CHECK(nMC2.z == doctest::Approx(1.0f));
    // Mech and prop agree: the same component carries "up" in both, so sceneNightSunWeight needs .z from both callers.
    const glm::mat4 boneT = mechShapeToWorld(glm::vec3(5.0f, 6.0f, 7.0f), 123.0f);
    const glm::vec3 mechN = glm::normalize(mc2ToRender(glm::mat3(boneT) * glm::vec3(0, 1, 0)));
    CHECK(mechN.z == doctest::Approx(nMC2.z));
    CHECK(mechN.y == doctest::Approx(nMC2.y));
}

TEST_CASE("worldToClipGL consumes raw MC2 world: elevation (z) becomes GL up (y)") {
    // Why worldMC2 above is the frame whose .z is up: the matrix the shaders multiply it by.
    const glm::mat4 A = mc2xform::makeAxisSwapMC2toGL();
    const glm::vec4 up = glm::vec4(0, 0, 1, 0) * A;   // row-vector convention, as test_xform_convention.cpp
    CHECK(up.y == doctest::Approx(1.0f));
}

}
