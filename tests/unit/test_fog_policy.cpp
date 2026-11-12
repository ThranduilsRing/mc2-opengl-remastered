#include "doctest.h"
#include "../../shaders/include/fog_policy.hglsl"
#include <cmath>
#include <initializer_list>
#include <limits>

TEST_CASE("fog_policy/source distances scale once without changing clipping") {
    const float sourceStart = 50000, maxClip = 50000;
    for (int load = 0; load < 20; ++load) {
        CHECK(mc2FogDistance(sourceStart) == 75000);
        CHECK(mc2FogDistance(maxClip) == 75000);
        CHECK(mc2FogHaze(28000, sourceStart, maxClip) == 0);
        CHECK(sourceStart == 50000);
        CHECK(maxClip == 50000);
    }
    CHECK(mc2FogDistance(6000) == 9000);
    CHECK(mc2FogDistance(12000) == 18000);
    CHECK(MC2_FOG_EXP_COEFFICIENT == doctest::Approx(0.00004));
    CHECK(mc2FogHaze(1500, 400, 1500) == doctest::Approx(900.0 / 1650.0));
    CHECK(mc2FogHaze(75000, 50000, 50000) == 0);
    CHECK(mc2FogHaze(75001, 50000, 50000) == 1);
    CHECK(mc2FogHaze(900, 400, 200) == 1);
    CHECK(mc2FogInvSpan(75000, 75000) == 0);
    CHECK(mc2FogInvSpan(600, 2250) == doctest::Approx(1.0 / 1650.0));
}

TEST_CASE("fog_policy/finite bounded degenerate inputs and packed height alpha") {
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (float start : {200.0f, 20.0f, 0.0f, inf, nan}) {
        for (float full : {200.0f, 20.0f, 0.0f, -inf, nan}) {
            for (float elevation : {-1000.0f, 0.0f, 20.0f, 110.0f, 200.0f, 28000.0f, inf, nan}) {
                float alpha = mc2FogHeightAlpha(elevation, start, full);
                CHECK(std::isfinite(alpha));
                CHECK(alpha >= 243);
                CHECK(alpha <= 255);
                CHECK(1.0f - alpha / 255.0f <= MC2_FOG_HEIGHT_MAX_OPACITY);
                CHECK(float(static_cast<unsigned char>(static_cast<unsigned>(alpha))) == alpha);
                float haze = mc2FogHaze(elevation, start, full);
                CHECK(std::isfinite(haze));
                CHECK(haze >= 0);
                CHECK(haze <= 1);
                // Snapshot consumer gets derived endpoints; actor consumer gets sources.
                CHECK(mc2FogRamp(elevation, mc2FogDistance(start), mc2FogDistance(full)) == haze);
            }
        }
    }
    CHECK(mc2FogHeightAlpha(110, 200, 20) == 252);
    CHECK(mc2FogHeightAlpha(-1000, 200, 20) == 243);
    CHECK(mc2FogHeightAlpha(0, 20, 20) == 255);
    CHECK(mc2FogHeightAlpha(0, 20, 200) == 255);
    const float huge = std::numeric_limits<float>::max();
    CHECK(mc2FogHaze(28000, huge, huge) == 0);
    CHECK(mc2FogInvSpan(huge, -huge) == 0);
    CHECK(mc2FogInvSpan(0, std::numeric_limits<float>::denorm_min()) == 0);
    // mc2_01's mission-file override disables height fog on its positive terrain.
    CHECK(mc2FogHeightAlpha(400.9412f, -1, -2) == 255);
    CHECK(mc2FogHeightAlpha(546.2013f, -1, -2) == 255);
}

TEST_CASE("fog_policy/overview range is separate from distance scale and horizon haze") {
    // Near-vertical overview ground ray: both old and scaled distance ramps saturate.
    const float range = 28000;
    CHECK(mc2FogSmooth(mc2FogRamp(range, 6000, 12000)) == 1);
    float scaled = mc2FogSmooth(mc2FogRamp(range, mc2FogDistance(6000), mc2FogDistance(12000)));
    CHECK(scaled == 1);
    CHECK(scaled * mc2FogRangeViewFactor(0.996195f) == 0);
    CHECK(scaled * mc2FogRangeViewFactor(-0.996195f) == 0);
    for (float downSin : {0.0f, 0.1f, 0.3f}) {
        CHECK(mc2FogRangeViewFactor(downSin) == 1);
        CHECK(mc2FogSmooth(mc2FogRamp(15000, 9000, 18000)) * mc2FogRangeViewFactor(downSin)
              == doctest::Approx(0.7407407));
    }
    CHECK(mc2FogDistance(256) == 384);
    CHECK(mc2FogDistance(32) == 48);
}
