#include "doctest.h"
#include "../../GameOS/gameos/cloud_threshold_band.h"

TEST_CASE("cloudThreshBand: coverage 0 keeps legacy band") {
    const CloudThreshBand b = cloudThreshBand(0.0f, 0.12f);
    CHECK(b.lo == doctest::Approx(0.3f));
    CHECK(b.hi == doctest::Approx(0.7f));
}

TEST_CASE("cloudThreshBand: coverage > 0 centres on 0.27 + 0.5*cov with half-width softness") {
    const CloudThreshBand b = cloudThreshBand(0.2f, 0.12f);
    CHECK(b.lo == doctest::Approx(0.37f - 0.12f));
    CHECK(b.hi == doctest::Approx(0.37f + 0.12f));
}
