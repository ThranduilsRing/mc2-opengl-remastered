// tests/unit/test_render_snap.cpp
// UNIT-VISUAL-SNAP-2: the render-only delta lifts effect anchors and the selection box, and only the copy passed in.
#include "doctest.h"
#include "render_snap.h"

TEST_CASE("render snap: effect anchor lifts on render y only")
{
    const glm::vec3 sim(10.0f, 20.0f, 30.0f);
    glm::vec3 anchor = sim;
    mc2snap::liftRenderY(anchor, 12.5f);
    CHECK(anchor.x == sim.x);
    CHECK(anchor.z == sim.z);
    CHECK(anchor.y == doctest::Approx(32.5f));
}

TEST_CASE("render snap: zero delta (gate off or no bake) is an exact no-op")
{
    glm::vec3 anchor(1.0f, 2.0f, 3.0f);
    mc2snap::liftRenderY(anchor, 0.0f);
    CHECK(anchor == glm::vec3(1.0f, 2.0f, 3.0f));
}

TEST_CASE("render snap: selection box lifts all 8 corners on mc2 z only")
{
    glm::vec3 box[8];
    for (int i = 0; i < 8; ++i) box[i] = glm::vec3(float(i), float(-i), 100.0f);
    mc2snap::liftBoxZ(box, 8, -9.0f);
    for (int i = 0; i < 8; ++i) {
        CHECK(box[i].x == float(i));
        CHECK(box[i].y == float(-i));
        CHECK(box[i].z == doctest::Approx(91.0f));
    }
}
