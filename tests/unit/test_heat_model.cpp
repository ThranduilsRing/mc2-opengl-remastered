#include "doctest.h"
#include "heat_model.h"
#include "mech_value_codec.h"
#include <array>

using namespace mc2::heat;

TEST_CASE("heat/alpha admission accounts per-component truncation and wide sums") {
    State state;
    state.capacity = 7000;
    Salvo salvo;
    salvo.include(4, 3.9f); salvo.include(71, 4.8f);
    CHECK(salvo.cost == 7000);
    CHECK(state.admits(salvo));
    state.heatMilli = 1; CHECK_FALSE(state.admits(salvo));
    state.heatMilli = 0; CHECK_FALSE(state.admits(Salvo{}));
    CHECK(weaponCost(0.99f) == 0);
    Salvo huge;
    for (int i = 0; i < inventorySize; ++i) huge.include(i, float(INT32_MAX));
    CHECK(huge.cost > INT32_MAX);
    state.capacity = INT32_MAX; CHECK_FALSE(state.admits(huge));
}

TEST_CASE("heat/alpha arms once and charges only discharged weapons before expiry") {
    State state;
    state.capacity = 10000;
    Salvo salvo; salvo.include(4, 3.9f); salvo.include(71, 4.8f);
    REQUIRE(state.arm(salvo, 10, Profile{}));
    CHECK_FALSE(state.arm(salvo, 10, Profile{})); // same-tick duplicate/replacement
    CHECK(state.alphaExpireTick == 70);
    mc2::save::Archive writer; mc2::save::heatFields(writer, state);
    State reloaded; reloaded.capacity = state.capacity;
    mc2::save::Archive reader(writer.bytes()); mc2::save::heatFields(reader, reloaded);
    REQUIRE(reader.finish());
    CHECK_FALSE(reloaded.arm(salvo, 11, Profile{}));
    CHECK(reloaded.alphaExpireTick == 70);
    CHECK(state.discharge(4, 3.9f, 11) == 3000); // hit or ordinary miss
    CHECK(state.discharge(4, 3.9f, 11) == 0);
    CHECK(state.heatMilli == 3000);
    CHECK_FALSE(state.arm(salvo, 12, Profile{})); // pending salvo cannot be overwritten
    state.capacity = 1000; // lost sink: an already armed shot still pays its heat
    CHECK(state.discharge(71, 4.8f, 69) == 4000);
    CHECK(state.heatMilli == 7000);
    state = {}; state.capacity = 10000;
    REQUIRE(state.arm(salvo, 10, Profile{}));
    state.expire(69); CHECK(state.alphaMask.any());
    // Streak no-fire never calls discharge. Expiry is inclusive and costs nothing.
    CHECK(state.discharge(4, 3.9f, 70) == 0);
    CHECK(state.alphaMask.none()); CHECK(state.heatMilli == 0);
}

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
