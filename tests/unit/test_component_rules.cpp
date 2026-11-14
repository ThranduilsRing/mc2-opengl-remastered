// Trigger: component_rules.h and the mech/loadout/projectile component consumers.
#include "doctest.h"
#include "component_rules.h"

TEST_CASE("components/rack sizes and single-missile weapons") {
    using mc2::components::rackSize;
    CHECK(rackSize("LRM5 Rack") == 5);
    CHECK(rackSize("LR Missile/10") == 10);
    CHECK(rackSize("LR Missile/15") == 15);
    CHECK(rackSize("C LR Missile/20") == 20);
    CHECK(rackSize("Streak SRM2 Pack") == 2);
    CHECK(rackSize("C ST-SR Missile/6") == 6);
    CHECK(rackSize("Thunderbolt Missile") == 1);
    CHECK(rackSize("Anti-Missile System") == 1);
    CHECK(rackSize("LRM99999999") == 1);
}

TEST_CASE("components/XL moves exactly one speed class") {
    using mc2::components::nextSpeedClass;
    const float base[] = {5, 10, 15, 19, 23, 27, 31, 35};
    for (int i = 0; i < 8; ++i)
        CHECK(nextSpeedClass(base[i]) == base[i == 7 ? 7 : i + 1]);
}

TEST_CASE("components/AMS honors cooldown ammo and damage") {
    using namespace mc2::components;
    CHECK(defensive(0, 255));
    CHECK_FALSE(defensive(1, 255));
    CHECK_FALSE(defensive(0, 2));
    CHECK(canIntercept(false, 10, 10, 1));
    CHECK_FALSE(canIntercept(false, 10, 9, 1));
    CHECK_FALSE(canIntercept(false, 10, 10, 0));
    CHECK_FALSE(canIntercept(true, 10, 10, 1));
}
