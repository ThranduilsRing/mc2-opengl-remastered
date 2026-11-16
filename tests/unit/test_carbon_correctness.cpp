#include "doctest.h"

TEST_CASE("carbon correctness: leg effectiveness follows leg damage") {
    enum { MECH_ARMOR_LOCATION_LLEG, MECH_ARMOR_LOCATION_RLEG };
    struct Armor { float curArmor, maxArmor; };
    Armor armor[] = {{100, 100}, {100, 100}};
    const float curArm = 0, maxArm = 100;
    float curLeg, maxLeg, armorLeg;
    auto effectiveness = [&] {
#include "carbon_leg_armor.inc"
        return armorLeg;
    };
    CHECK(effectiveness() == doctest::Approx(1.0));
    armor[0].curArmor = armor[1].curArmor = 0;
    CHECK(effectiveness() == doctest::Approx(0.6));
    armor[0].curArmor = 100;
    CHECK(effectiveness() == doctest::Approx(0.8));
}

namespace {
struct Commander { static Commander* home; int getId() const { return 0; } } homeCommander;
Commander* Commander::home = &homeCommander;
}
TEST_CASE("carbon correctness: difficulty distinguishes opposing commanders") {
    bool MPlayer = false;
    constexpr bool TRUE = true, FALSE = false;
    int commander = 0;
    auto getCommanderId = [&] { return commander; };
    auto applyDifficultySkill = [](float chance, bool player) { return chance + (player ? 10 : -10); };
    auto attack = [&] { float attackChance = 50;
#include "carbon_difficulty.inc"
        return attackChance;
    };
    CHECK(attack() == 60);
    commander = 1; CHECK(attack() == 40);
    MPlayer = true; CHECK(attack() == 50);
}
