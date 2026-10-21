// tests/unit/test_shadow_bake_math.cpp
// The static shadow bake's footprint and reach math: the mask layout, the sun clamp, the caster reach, the shadow footprint a caster can touch, the
// per-tile caster window (it must hold every receiver of the tile and every caster that can shadow one) and the window matrix.
#include "doctest.h"
#include "gameos/shadow_bake_math.h"

#include <cmath>

using namespace mc2shadow;

namespace {
Vec3 sunAt(float altDeg, float azDeg) {
    const float a = altDeg * 0.01745329252f, z = azDeg * 0.01745329252f;
    return {std::cos(a) * std::cos(z), std::cos(a) * std::sin(z), std::sin(a)};
}
}  // namespace

TEST_CASE("shadow bake: mask layout is max(target, span / cap) in whole tiles") {
    // mc2_01-sized: 101 vertices -> 12,800 wu at the 1.5 wu target.
    MaskLayout m = maskLayout(101, 101 * 64.0f, 1.5f, 12288);
    CHECK(m.span == doctest::Approx(12800.0f));
    CHECK(m.texels % kTileTexels == 0);
    CHECK(m.texelWorld <= 1.5f);
    CHECK(m.texelWorld > 1.4f);
    CHECK(m.tilesPerSide * kTileTexels == m.texels);
    CHECK(m.originX == doctest::Approx(-101 * 64.0f));
    // A 900-cell map hits the cap: 38,400 wu / 12,288 = 3.125 wu.
    MaskLayout big = maskLayout(901, 901 * 64.0f, 1.5f, 12288);
    CHECK(big.texels == 12288);
    CHECK(big.texelWorld == doctest::Approx(3.125f));
    // No terrain, no mask.
    CHECK(maskLayout(1, 0.0f, 1.5f, 12288).texels == 0);
}

TEST_CASE("shadow bake: the sun clamp keeps the azimuth and lifts the altitude to the minimum") {
    const Vec3 horizon = clampSunAltitude({1.0f, 0.0f, 0.0f}, 15.0f);
    CHECK(horizon.z == doctest::Approx(std::sin(15.0f * 0.01745329252f)));
    CHECK(horizon.y == doctest::Approx(0.0f));
    CHECK(horizon.x > 0.9f);
    const Vec3 high = sunAt(60.0f, 30.0f);
    const Vec3 kept = clampSunAltitude(high, 15.0f);
    CHECK(kept.x == doctest::Approx(high.x));
    CHECK(kept.z == doctest::Approx(high.z));
}

TEST_CASE("shadow bake: reach is h * cot(altitude), at most 3.73 h at the 15 degree clamp") {
    CHECK(reachWorld(10.0f, sunAt(45.0f, 0.0f)) == doctest::Approx(10.0f).epsilon(1e-4));
    CHECK(reachWorld(10.0f, sunAt(15.0f, 77.0f)) == doctest::Approx(37.32f).epsilon(1e-3));
    CHECK(reachWorld(10.0f, sunAt(90.0f, 0.0f)) == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(penumbraWorld(100.0f, std::tan(2.0f * 0.01745329252f)) == doctest::Approx(3.492f).epsilon(1e-3));
}

TEST_CASE("shadow bake: a caster's footprint covers where its top lands, away from the sun") {
    // Sun in the east at 45 degrees: a 20 wu tall box throws its shadow 20 wu west.
    const Rect r = shadowFootprint({0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 20.0f}, sunAt(45.0f, 0.0f), 0.0f, 0.0f);
    CHECK(r.x0 == doctest::Approx(-20.0f).epsilon(1e-3));
    CHECK(r.x1 == doctest::Approx(2.0f));
    CHECK(r.y0 == doctest::Approx(0.0f));
    CHECK(r.y1 == doctest::Approx(2.0f));
    // A penumbra widens it on every side.
    const Rect soft = shadowFootprint({0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 20.0f}, sunAt(45.0f, 0.0f), 0.0f, 0.05f);
    CHECK(soft.x0 < r.x0);
    CHECK(soft.y1 > r.y1);
}

TEST_CASE("shadow bake: the tile window holds every receiver and every caster that can shadow one") {
    const MaskLayout m = maskLayout(101, 101 * 64.0f, 1.5f, 12288);
    const float tanT = std::tan(kSunAngularRadiusDeg * 0.01745329252f);
    for (float alt : {15.0f, 35.0f, 70.0f}) {
        const Vec3 sun = sunAt(alt, 123.0f);
        const LightBasis b = lightBasis(sun);
        const Rect region = tileRegionWorld(m, 3, 5);
        const float hMin = 10.0f, hMax = 60.0f, top = 160.0f;
        const Window w = casterWindow(b, region, hMin, hMax, top, tanT);
        float mtx[16];
        windowMatrix(b, w, mtx);
        CHECK(w.texels <= kWindowMaxTexels);
        CHECK(w.texelWorld >= kWindowTexelWorld);
        // Receivers land inside the window, between its depth planes.
        for (int c = 0; c < 8; ++c) {
            const Vec3 p{(c & 1) ? region.x1 : region.x0, (c & 2) ? region.y1 : region.y0, (c & 4) ? hMax : hMin};
            const Vec3 q = applyMatrix(mtx, p);
            CHECK(std::fabs(q.x) < 1.0f);
            CHECK(std::fabs(q.y) < 1.0f);
            CHECK(q.z > 0.0f);
            CHECK(q.z < 1.0f);
            // The highest caster on this receiver's ray is in the window too, nearer the sun (smaller depth).
            const float ray = (top - p.z) / sun.z;
            const Vec3 caster{p.x + sun.x * ray, p.y + sun.y * ray, p.z + sun.z * ray};
            const Vec3 cq = applyMatrix(mtx, caster);
            CHECK(cq.x == doctest::Approx(q.x).epsilon(1e-3));
            CHECK(cq.y == doctest::Approx(q.y).epsilon(1e-3));
            CHECK(cq.z >= 0.0f);
            CHECK(cq.z < q.z);
            // Its light box is binned to the window.
            CHECK(casterInWindow(lightBox(b, {caster.x - 1, caster.y - 1, caster.z - 30}, {caster.x + 1, caster.y + 1, caster.z}), w));
        }
        // A caster far beside the tile is not.
        CHECK_FALSE(casterInWindow(lightBox(b, {region.x1 + 5000, region.y1 + 5000, 0}, {region.x1 + 5010, region.y1 + 5010, 50}), w));
    }
}

TEST_CASE("shadow bake: neighbouring tiles share the window texel grid") {
    const MaskLayout m = maskLayout(101, 101 * 64.0f, 1.5f, 12288);
    const LightBasis b = lightBasis(sunAt(30.0f, 40.0f));
    const Window a = casterWindow(b, tileRegionWorld(m, 2, 2), 0.0f, 40.0f, 120.0f, 0.03f);
    const Window c = casterWindow(b, tileRegionWorld(m, 3, 2), 0.0f, 40.0f, 120.0f, 0.03f);
    REQUIRE(a.texelWorld == c.texelWorld);
    const float shift = (c.lx0 - a.lx0) / a.texelWorld;
    CHECK(shift == doctest::Approx(std::round(shift)).epsilon(1e-4));
}
