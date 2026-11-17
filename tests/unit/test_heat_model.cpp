#include "doctest.h"
#include "heat_model.h"
#include <array>

using namespace mc2::heat;

TEST_CASE("heat/capacity matches lab integer accumulation") {
    // Stock chassis Heat Index (atlas.csv row 4 col 5) and single/double/Clan sinks.
    for (const auto sinks : {std::array<float, 3>{4, 6, 8}, {4.9f, 6.9f, 8.2f}}) {
        int lab = 0;
        int quantized = 0;
        for (float damage : sinks) {
            lab += damage; // LogisticsVariant::getMaxHeat's per-component truncation
            quantized += static_cast<int>(damage);
        }
        CHECK(capacityMilli(43, quantized) == 1000 * (43 + lab));
        CHECK(capacityMilli(43, quantized) == 61000);
    }
    CHECK(capacityMilli(0, 0) == 0);
    CHECK(capacityMilli(-10, 2) == 0);
    CHECK(capacityMilli(INT32_MAX, INT32_MAX) == INT32_MAX);
}

TEST_CASE("heat/determinism zero floor and dissipation clamp") {
    State a, b;
    a.recompute(43, 18);
    b.recompute(43, 18);
    a.heatMilli = b.heatMilli = a.capacity;
    for (int tick = 0; tick < 700; ++tick) {
        a.tick(Profile{});
        b.tick(Profile{});
        CHECK(a.heatMilli == b.heatMilli);
        CHECK(a.heatMilli >= 0);
    }
    CHECK(a.heatMilli == 0);
    CHECK(dissipate(61000, 61000, 20) == 60899);
    CHECK(dissipate(1, 61000, 20) == 0);
    CHECK(dissipate(-1, 0, 20) == 0);
    CHECK(dissipate(3, 0, 20) == 2);
    CHECK(dissipate(0, INT32_MAX, 1) == 0);
}

TEST_CASE("heat/lifecycle derived capacity and cold transient state") {
    State state;
    state.recompute(43, 4); // init
    CHECK(state.capacity == 47000);
    state.recompute(43, 10); // replacement loadout
    CHECK(state.capacity == 53000);
    state.recompute(43, 6); // destroyed single sink
    CHECK(state.capacity == 49000);
    state.heatMilli = 1234;
    state.alphaMask.set(71);
    state.alphaExpireTick = 60;
    state = {}; // save load resets transient fields, then derives capacity
    state.recompute(43, 6);
    CHECK(state.capacity == 49000);
    CHECK(state.heatMilli == 0);
    CHECK(state.alphaMask.none());
    CHECK(state.alphaExpireTick == 0);
    for (int tick = 0; tick < 100; ++tick) state.tick(Profile{});
    CHECK(state.heatMilli == 0);
}

TEST_CASE("heat/alpha mask covers all inventory entries") {
    State state;
    CHECK(state.alphaMask.size() == 72);
    for (int index : {0, 63, 64, 71}) state.alphaMask.set(index);
    CHECK(state.alphaMask.count() == 4);
    CHECK(state.alphaMask.test(71));
}

TEST_CASE("heat/profile defaults and phase one validation") {
    Profile p;
    REQUIRE(validatedProfile(p));
    CHECK(p.coolSeconds == 20);
    CHECK(p.alphaWindowTicks == 60);
    for (int invalid : {0, -1, INT32_MAX}) {
        p.coolSeconds = invalid;
        CHECK_FALSE(validatedProfile(p));
    }
    p = {}; p.heatProfile = 2; CHECK_FALSE(validatedProfile(p));
    p = {}; p.alphaWindowTicks = 0; CHECK_FALSE(validatedProfile(p));
    p = {}; p.aiAlpha = 1; CHECK_FALSE(validatedProfile(p));
    p = {}; p.thresholdCount = 1; CHECK_FALSE(validatedProfile(p));
}
