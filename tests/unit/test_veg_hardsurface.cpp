// VEG-CEMENT-1: the coverage-to-density curve, the weed rule and the class-3 union
// (offline, header-only logic).
#include "doctest.h"
#include "GameAdapters/vegetation_hardsurface.h"

using namespace mc2::vegetation;

TEST_SUITE("VegHardSurface") {

TEST_CASE("cover density falls smoothly and monotonically with coverage") {
    CHECK(coverToDensityMul(0.0f) == doctest::Approx(1.0f));
    CHECK(coverToDensityMul(kHardFalloffLo) == doctest::Approx(1.0f));
    CHECK(coverToDensityMul(kHardFalloffHi) == doctest::Approx(0.0f));
    CHECK(coverToDensityMul(1.0f) == doctest::Approx(0.0f));
    float prev = 1.0f;
    for (int i = 0; i <= 100; ++i) {
        const float m = coverToDensityMul(float(i) / 100.0f);
        CHECK(m <= prev + 1e-6f);
        // No staircase: adjacent 1% coverage steps never jump by more than 4%.
        CHECK(prev - m < 0.04f);
        prev = m;
    }
}

TEST_CASE("weeds need real hard surface and favour cracks and edges") {
    CHECK(weedKeepProbability(0.49f, 1.0f) == 0.0f);
    CHECK(weedKeepProbability(0.5f, 0.0f) > 0.0f);
    CHECK(weedKeepProbability(1.0f, 1.0f) == doctest::Approx(kWeedDensity));
    CHECK(weedKeepProbability(1.0f, 1.0f) > weedKeepProbability(1.0f, 0.0f));
    // Interior of a pad (flat coverage, away from seams) stays sparse.
    CHECK(weedKeepProbability(1.0f, crackTerm(0.0f, 50.0f)) < 0.5f * kWeedDensity);
}

TEST_CASE("crack term: coverage gradient and seams") {
    CHECK(crackTerm(0.0f, 50.0f) == 0.0f);
    CHECK(crackTerm(0.1f, 50.0f) == doctest::Approx(1.0f));
    CHECK(crackTerm(0.0f, 1.0f) > 0.0f);
}

TEST_CASE("class-3 union: pure cement (1.0) with alpha transition (<1) takes the max") {
    // The bake writes max(overlay alpha, pure-cement 1.0) per texel; a pure cell
    // therefore removes all normal cover regardless of a neighbouring ramp.
    const float alphaRamp = 0.3f, pure = 1.0f;
    const float cov = std::max(alphaRamp, pure);
    CHECK(coverToDensityMul(cov) == doctest::Approx(0.0f));
    CHECK(weedKeepProbability(cov, 0.0f) > 0.0f);
    CHECK((kWeedFlagBit & 0x3Fu) == 0u);  // never collides with frame + card-role bits
}

}
