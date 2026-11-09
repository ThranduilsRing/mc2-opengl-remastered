#include "doctest.h"
#include "../../RenderCore/water_reactive.h"
#include "../../shaders/include/water_reactive.hglsl"
#include <limits>

using namespace mc2::waterreactive;

TEST_CASE("WaterReactive/wet visible sources only and hidden history removal") {
    Buffer b;
    b.submit(1, 1, 0, 0, 20, false, true, false);
    b.submit(2, 1, 0, 0, 20, true, false, false);
    CHECK(b.size() == 0);
    b.submit(1, 1, 0, 0, 20, true, true, false);
    CHECK(b.size() == 1);
    b.submit(1, 1.1f, 0, 0, 20, true, false, false);
    CHECK(b.size() == 0);
    b.submit(1, 1.2f, 0, 0, 20, true, true, false);
    b.submit(1, 1.3f, 0, 0, 20, false, true, false);
    CHECK(b.size() == 1); // A dry exit leaves the last ring to fade.
    b.advance(4.2f);
    CHECK(b.size() == 0);
}

TEST_CASE("WaterReactive/capacity drops oldest and tile lists keep newest") {
    Buffer b;
    for (uintptr_t i = 1; i <= kCapacity + 5; ++i)
        b.submit(i, 1, static_cast<float>(i), -1, 20, true, true, false);
    CHECK(b.size() == kCapacity);
    GpuFrame gpu;
    b.snapshot(1.25f, gpu);
    CHECK(gpu.stamps[0].positionRadiusAge[0] == 6);
    CHECK(gpu.stamps[127].positionRadiusAge[0] == 133);
    CHECK(gpu.stamps[0].positionRadiusAge[3] == doctest::Approx(0.25));
    const auto t = tile(0, -1);
    CHECK(gpu.counts[t] == kPerTile);
    CHECK(gpu.indices[t * kPerTile] == kCapacity - kPerTile);
    for (uint32_t i = 0; i < kTiles; ++i) {
        REQUIRE(gpu.counts[i] <= kPerTile);
        for (uint32_t j = 0; j < gpu.counts[i]; ++j)
            CHECK(gpu.indices[i * kPerTile + j] < kCapacity);
    }
    CHECK(gpu.counts[tile(10, 10)] == 0);
}

TEST_CASE("WaterReactive/cadence velocity reset invalid input and stationary rings") {
    Buffer b;
    b.submit(1, 1, 0, 0, 20, true, true, true);
    for (int i = 1; i <= 4; ++i)
        b.submit(1, 1 + i * 0.1f, i * 5.0f, 0, 20, true, true, true);
    CHECK(b.size() == 2);
    GpuFrame gpu;
    b.snapshot(1.4f, gpu);
    CHECK(gpu.stamps[1].velocityType[0] == doctest::Approx(50));
    b.submit(1, 1.5f, 2000, 0, 20, true, true, true);
    CHECK(b.size() == 2); // Teleport emits no stretched connecting wake.
    b.submit(2, 1.5f, std::numeric_limits<float>::infinity(), 0, 20, true, true, false);
    CHECK(b.size() == 2);
    b.advance(0); // Replay/mission clock reset.
    CHECK(b.size() == 0);
    b.submit(1, 0, 0, 0, 20, true, true, false);
    for (int i = 1; i <= 7; ++i)
        b.submit(1, i * 0.1f, 0, 0, 20, true, true, false);
    CHECK(b.size() == 2);
    b.clear();
    b.snapshot(1, gpu);
    CHECK(b.size() == 0);
    for (const auto n : gpu.counts) CHECK(n == 0);
}

TEST_CASE("WaterReactive/shared shader ring support symmetry age and pixel fade") {
    const auto sample = [](glm::vec2 p, float age, float pixel = 1.0f) {
        return wr_profile(p, glm::vec2(0), 20, age, 0, pixel);
    };
    CHECK(glm::length(sample({500, 0}, 1)) == 0);
    CHECK(glm::length(sample({50, 0}, 3)) == 0);
    CHECK(glm::length(sample({50, 0}, 0)) == 0);
    CHECK(glm::length(sample({50, 0}, 1, 20)) == 0);
    auto p = sample({53, 0}, 1), n = sample({-53, 0}, 1), y = sample({0, 53}, 1);
    CHECK(std::abs(p.x) > 0.001f);
    CHECK(p.x == doctest::Approx(-n.x));
    CHECK(p.x == doctest::Approx(y.y));
    CHECK(p.y == 0);
    CHECK(p.z == doctest::Approx(n.z));
    // Follow the expanding crest: attenuation is age, independent of radius.
    CHECK(glm::length(sample({20 + 2.8f * 28 + 5, 0}, 2.8f)) < glm::length(p) * 0.05f);
    CHECK(std::isfinite(sample({0, 0}, 1).x));
}

TEST_CASE("WaterReactive/shared shader vehicle wake trails velocity and mirrors") {
    const glm::vec2 v(80, 0);
    const auto ring = wr_profile({-60, 35}, v, 20, 0.5f, 0, 1);
    const auto left = wr_profile({-60, 35}, v, 20, 0.5f, 1, 1);
    const auto right = wr_profile({-60, -35}, v, 20, 0.5f, 1, 1);
    CHECK(glm::length(left - ring) > 0.001f);
    CHECK(left.x == doctest::Approx(right.x));
    CHECK(left.y == doctest::Approx(-right.y));
    CHECK(left.z == doctest::Approx(right.z));
    const auto ahead = wr_profile({60, 35}, v, 20, 0.5f, 1, 1);
    const auto aheadRing = wr_profile({60, 35}, v, 20, 0.5f, 0, 1);
    CHECK(glm::length(ahead - aheadRing) == 0);
    const auto reverse = wr_profile({60, 35}, -v, 20, 0.5f, 1, 1);
    CHECK(reverse.x == doctest::Approx(-left.x));
    CHECK(reverse.y == doctest::Approx(left.y));
}
