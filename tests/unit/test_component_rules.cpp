// Trigger: component_rules.h and the mech/loadout/projectile component consumers.
#include "doctest.h"
#include "component_rules.h"

TEST_CASE("components/reset ignores stale storage and preserves unmapped live items") {
    struct Item { unsigned char masterID; };
    struct Slot { bool clearOut; };
    const Item inventory[] = {{1}, {120}, {11}, {122}, {161}};
    const int locations[] = {0, 1, -1, -1, -1};
    const Slot table[] = {{false}, {true}, {true}, {true}, {true}};
    int32_t snapshot[5];
    mc2::components::resetSnapshot(snapshot, inventory, 3, locations, table);
    CHECK(snapshot[0] == 1);
    CHECK(snapshot[1] == 255);
    CHECK(snapshot[2] == 11); // Missing mapping must never access table[-1].
    CHECK(snapshot[3] == 255);
    CHECK(snapshot[4] == 255);
}

TEST_CASE("components/rack sizes and single-missile weapons") {
    using mc2::components::rackSize;
    CHECK(rackSize("LRM5 Rack") == 5);
    CHECK(rackSize("LR Missile/10") == 10);
    CHECK(rackSize("LR Missile/15") == 15);
    CHECK(rackSize("C LR Missile/20") == 20);
    CHECK(rackSize("Streak SRM2 Pack") == 2);
    CHECK(rackSize("SRM2 Pack") == 2);
    CHECK(rackSize("SRM4 Pack") == 4);
    CHECK(rackSize("SRM6 Pack") == 6);
    CHECK(rackSize("Swarm LRM5 Rack") == 5);
    CHECK(rackSize("C SR Missile Pack") == 1);
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
