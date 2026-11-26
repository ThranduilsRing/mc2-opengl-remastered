// Trigger: code/economy_rules.h (C-Bill payout multiplier, salvage price factor).
#include "doctest.h"

#include "economy_rules.h"

#include <climits>

using namespace mc2::rules;

TEST_CASE("economy rules: defaults equal today's behaviour exactly") {
    GameRules g;
    CHECK(payoutScaled(1234, g.cbillMultiplier) == 1234);
    CHECK(payoutScaled(0, g.cbillMultiplier) == 0);
    CHECK(payoutScaled(-7, g.cbillMultiplier) == -7);
    CHECK(payoutScaled(INT_MAX, g.cbillMultiplier) == INT_MAX);
    CHECK(salvagePriceFactor(g.salvage) == .8f);
    CHECK(salvagePriceFactor(kSalvageMC2) == .8f);
}

TEST_CASE("economy rules: payout scales the right way, truncating toward zero") {
    CHECK(payoutScaled(1000, 150) == 1500);
    CHECK(payoutScaled(1000, 50) == 500);
    CHECK(payoutScaled(1000, 200) == 2000);
    CHECK(payoutScaled(999, 50) == 499);
    CHECK(payoutScaled(-999, 50) == -499);
    CHECK(payoutScaled(1, 99) == 0);
}

TEST_CASE("economy rules: payout clamps to int") {
    CHECK(payoutScaled(INT_MAX, 200) == INT_MAX);
    CHECK(payoutScaled(INT_MIN, 200) == INT_MIN);
    CHECK(payoutScaled(INT_MAX, 150) == INT_MAX);
}

TEST_CASE("economy rules: salvage factor per mode") {
    CHECK(salvagePriceFactor(kSalvageGenerous) == 0.6f);
    CHECK(salvagePriceFactor(kSalvageScarce) == 1.0f);
    CHECK(salvagePriceFactor(kSalvageGenerous) < salvagePriceFactor(kSalvageMC2));
    CHECK(salvagePriceFactor(kSalvageScarce) > salvagePriceFactor(kSalvageMC2));
    CHECK(salvagePriceFactor(99) == .8f);
}

TEST_CASE("economy rules: limited supply consumes purchases and retains stripped weapons") {
    WeaponSupply supply;
    CHECK(weaponSupplyCap(0) == 0);
    CHECK(weaponSupplyCap(1) == 2);
    CHECK(weaponSupplyCap(2) == 1);
    CHECK(weaponSupplyCap(3) == 3);
    CHECK(supply.allows(0, {{7, 99}}));
    CHECK(supply.allows(2, {{7, 2}}));
    CHECK_FALSE(supply.allows(2, {{7, 3}}));
    supply.settle({{7, 2}});
    CHECK_FALSE(supply.allows(2, {{7, 1}}));
    CHECK(supply.allows(2, {{8, 2}}));
    supply.settle({{7, -3}}); // salvage or an existing loadout stripped in the lab
    CHECK(supply.spare[7] == 3);
    CHECK(supply.allows(2, {{7, 3}}));
    supply.settle({{7, 3}});
    CHECK(supply.spare[7] == 0);
    CHECK(supply.bought[7] == 2);
    CHECK(supply.allows(1, {{7, 0}}));
    CHECK(supply.allows(1, {{7, -1}})); // lowering the cap must still allow stripping
    supply.bought.clear();
    CHECK(supply.allows(2, {{7, 2}}));
}

TEST_CASE("economy rules: vehicle salvage requires both toggles enemy ground and wreck") {
    CHECK(salvageVehicleEligible(true, true, true, false, true, false));
    CHECK(salvageVehicleEligible(true, true, true, false, false, true));
    CHECK_FALSE(salvageVehicleEligible(false, true, true, false, true, true));
    CHECK_FALSE(salvageVehicleEligible(true, false, true, false, true, true));
    CHECK_FALSE(salvageVehicleEligible(true, true, false, false, true, true));
    CHECK_FALSE(salvageVehicleEligible(true, true, true, true, true, true));
    CHECK_FALSE(salvageVehicleEligible(true, true, true, false, false, false));
}
