// Trigger: component_rules.h and the mech/loadout/projectile component consumers.
#include "doctest.h"
#include "component_rules.h"

// Trigger: mechlabscreen.cpp and component_rules.h (same-grid drag completion).
TEST_CASE("components/cooling refusal leaves required heat sink installed") {
    CHECK_FALSE(mc2::components::coolingAllowsRemoval(30, 10, 24));
    CHECK(mc2::components::coolingAllowsRemoval(30, 10, 20));
    CHECK(mc2::components::coolingAllowsRemoval(30, 10, 19));
}

TEST_CASE("components/grid move places once and rejected drop restores source") {
    for (bool reject : {false, true}) {
        long x = 2, y = 3;
        int calls = 0, count = 2, cost = 200, heat = 10, weight = 4;
        bool source = false, target = false;
        auto add = [&](long& px, long& py) {
            ++calls;
            if (reject && px == 2 && py == 3) return 1; // occupied/invalid
            source = px == 0 && py == 1;
            target = px == 2 && py == 3;
            ++count; cost += 100; heat += 5; weight += 2;
            return 0;
        };
        REQUIRE(mc2::components::finishGridDrop(x, y, true, 0, 1, add));
        CHECK(calls == (reject ? 2 : 1));
        CHECK(source == reject);
        CHECK(target == !reject);
        CHECK(x == (reject ? 0 : 2));
        CHECK(y == (reject ? 1 : 3));
        CHECK(count == 3);
        CHECK(cost == 300);
        CHECK(heat == 15);
        CHECK(weight == 6);
    }
}

TEST_CASE("components/launcher size aliases use the base model art") {
    using mc2::components::launcherIcon;
    CHECK(std::string_view(launcherIcon("mcl_mc_clanlrm20rack.tga")) == "mcl_mc_clanlrmrack.tga");
    CHECK(std::string_view(launcherIcon("mcl_mc_lrm15rack.tga")) == "mcl_mc_lrmrack.tga");
    CHECK(std::string_view(launcherIcon("mcl_mc_swarmlrmrack.tga")) == "mcl_mc_swarmlrmrack.tga");
    CHECK(std::string_view(launcherIcon("mcl_mc_clanstreaksrmpack.tga")) == "mcl_mc_clanstreaksrmpack.tga");
    CHECK(std::string_view(launcherIcon("mcl_mc_streaksrm6pack.tga")) == "mcl_mc_streaksrmpack.tga");
    CHECK(std::string_view(launcherIcon("mcl_mc_clansrm6pack.tga")) == "mcl_mc_srmpack.tga");
    CHECK(launcherIcon("mcl_mc_thunderboltmissile.tga") == nullptr);
    CHECK(launcherIcon("mcl_mc_gaussrifle.tga") == nullptr);
}

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
