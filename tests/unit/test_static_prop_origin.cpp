// Static-prop instance origin: the translation of a Stuff-memcpy'd matrix lives at [3],[7],[11], never [12..14].
#include "RenderCore/StaticPropOrigin.h"
#include "doctest.h"

TEST_CASE("static_prop_origin: Stuff layout, translation at [3][7][11]") {
    // Row-vector Stuff 4x4 as memcpy'd into GpuStaticPropInstance::modelMatrix: [12..14] stay 0.
    float m[16] = {1, 0, 0, 100.f,
                   0, 1, 0, 200.f,
                   0, 0, 1, 30.f,
                   0, 0, 0, 1};
    const StaticPropOrigin s = staticPropOriginStuff(m);
    CHECK(s.x == 100.f);
    CHECK(s.y == 200.f);
    CHECK(s.z == 30.f);
    CHECK(m[12] == 0.f);
    CHECK(m[13] == 0.f);
    CHECK(m[14] == 0.f);
}

TEST_CASE("static_prop_origin: render space is (-x, z, y)") {
    float m[16] = {1, 0, 0, 100.f, 0, 1, 0, 200.f, 0, 0, 1, 30.f, 0, 0, 0, 1};
    const StaticPropOrigin r = staticPropOriginRender(m);
    CHECK(r.x == -100.f);
    CHECK(r.y == 30.f);
    CHECK(r.z == 200.f);
}
