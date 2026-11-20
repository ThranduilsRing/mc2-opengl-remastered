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
