#include "doctest.h"
#include "mech_death_policy.h"
#include <initializer_list>

TEST_CASE("mech wreck: salvage rejects reactor and self destruct even with salvage all") {
    for (bool all : {false, true}) {
        for (bool destroyed : {false, true}) {
            CHECK_FALSE(mc2wreck::salvageable(true, destroyed, CORE_EXPLOSION_DEATH, all));
            CHECK_FALSE(mc2wreck::salvageable(true, destroyed, SELF_DESTRUCT_DEATH, all));
        }
        for (int cause : {DEBUGGER_DEATH, ENGINE_DEATH, PILOT_DEATH, EJECTION_DEATH, UNDETERMINED_DEATH})
            CHECK(mc2wreck::salvageable(true, false, cause, all));
        CHECK_FALSE(mc2wreck::salvageable(false, false, -1, all));
    }
    CHECK_FALSE(mc2wreck::salvageable(true, true, ENGINE_DEATH));
}

TEST_CASE("mech wreck: retain fall animation unless a posed replacement hulk exists") {
    CHECK(mc2wreck::animate(false, false));
    CHECK(mc2wreck::animate(true, false));
    CHECK_FALSE(mc2wreck::animate(true, true));
}
