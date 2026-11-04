// Static-prop instance origin: the translation of a Stuff-memcpy'd matrix lives at [3],[7],[11], never [12..14].
#include "RenderCore/StaticPropOrigin.h"
#include "RenderCore/axis_convert.h"
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

TEST_CASE("static_prop_origin: the stored translation is already render space (no second axis swap)") {
    // BldgAppearance writes xlatPosition = (-x, elevation, y) for mc2 position (x=100 east, y=200 north, elevation 30)
    // (mclib/bdactor.cpp), the same mapping as mc2ToRender, so the render origin is the stored translation as-is.
    StaticPropOrigin mc2{100.f, 200.f, 30.f}, xlat{};
    mc2ToRender(xlat, mc2);
    float m[16] = {1, 0, 0, xlat.x, 0, 1, 0, xlat.y, 0, 0, 1, xlat.z, 0, 0, 0, 1};
    const StaticPropOrigin r = staticPropOriginRender(m);
    CHECK(r.x == -100.f);
    CHECK(r.y == 30.f);
    CHECK(r.z == 200.f);
}
