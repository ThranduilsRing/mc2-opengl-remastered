#include "doctest.h"
#include "../../code/explosion_light.h"
#include <limits>
using mc2::explosion_light::scale;

TEST_CASE("explosion light: class and geometry scale the pool without a white plateau") {
    const auto vehicle = scale(40, 24, 1, 100, 600);
    const auto turret = scale(44, 32, 1, 100, 600);
    const auto building = scale(45, 96, 1, 100, 600);
    CHECK(vehicle.intensity < turret.intensity);
    CHECK(turret.intensity < building.intensity);
    CHECK(vehicle.outer < turret.outer);
    CHECK(turret.outer < building.outer);
    CHECK(building.intensity < mc2::explosion_light::peakCeiling);
    CHECK(building.inner <= building.outer * mc2::explosion_light::coreShare);
    CHECK(building.outer < 600);
    CHECK(scale(46, 24, 1, 100, 600).intensity == vehicle.intensity);
    CHECK(scale(45, 48, 1, 100, 600).intensity < building.intensity);
    CHECK(scale(45, 192, 1, 100, 600).intensity > building.intensity);
    CHECK(scale(45, 192, 1, 100, 600).outer > building.outer);
}

TEST_CASE("explosion light: compression preserves flash decay and handles stock overdrive") {
    float previous = 0;
    for (float i : {0.0f, 0.1f, 0.5f, 1.0f, 2.0f, 1000.0f}) {
        const auto light = scale(45, 96, i, 100, 600);
        CHECK(light.intensity >= previous);
        CHECK(light.intensity < mc2::explosion_light::peakCeiling);
        CHECK(light.inner < light.outer);
        previous = light.intensity;
    }
    CHECK(scale(45, 96, 0, 100, 600).intensity == 0);
    CHECK(scale(45, 96, 2, 100, 600).intensity > scale(45, 96, 1, 100, 600).intensity);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const auto invalid = scale(45, nan, nan, nan, nan);
    CHECK(invalid.intensity == 0);
    CHECK(invalid.inner == 0);
    CHECK(invalid.outer == 1);
    CHECK(scale(45, 0, 1, 100, 600).outer == scale(45, 96, 1, 100, 600).outer);
    const auto mine = scale(53, 12, 0.8f, 20, 200);
    CHECK(mine.intensity == 0.8f);
    CHECK(mine.inner == 20);
    CHECK(mine.outer == 200);
}

TEST_CASE("explosion light: stock white building flash has bounded peak radiance") {
    // EXPLOSION_LIGHT capture: 0x00ffffff, maxIntensity=1, core=150, reach=500.
    const auto light = scale(45, 96, 1, 150, 500);
    CHECK(light.intensity == doctest::Approx(0.149333333f));
    CHECK(light.outer == doctest::Approx(273.596f).epsilon(0.00001));
    CHECK(light.inner == doctest::Approx(light.outer * 0.05f));
    // Peak pool response with the shipped 1-exp(-1.6*x) compression.
    CHECK(1.0f - std::exp(-1.6f * light.intensity) < 0.22f);
    const auto huge = scale(45, std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max());
    CHECK(std::isfinite(huge.intensity));
    CHECK(std::isfinite(huge.inner));
    CHECK(std::isfinite(huge.outer));
}
