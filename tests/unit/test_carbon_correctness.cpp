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
