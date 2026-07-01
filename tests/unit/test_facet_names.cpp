// tests/unit/test_facet_names.cpp
// UNIT-DEFINITION-SHADOW-COMPARE-1 (Phase 3): the state-dump emits canonical
// token NAMES for the LocomotionModel / DamageSchemaId facets so the offline
// shadow_compare.py can string-diff them against the converter's DMG_*/LOCO_*
// vocabulary. These tests lock (a) the exact spellings the converter expects and
// (b) the parse*/name* round-trip identity — a mismatch here would make every
// engine-vs-converter comparison spuriously fail. Pure, offline.
#include "doctest.h"
#include "locomotion_facet.h"
#include "damage_schema.h"
#include <cstring>

TEST_CASE("locomotionModelName: exact converter spellings") {
    CHECK(std::strcmp(locomotionModelName(LOCO_MECH_WALKER), "MechWalker") == 0);
    CHECK(std::strcmp(locomotionModelName(LOCO_TRACKED),     "Tracked")    == 0);
    CHECK(std::strcmp(locomotionModelName(LOCO_WHEELED),     "Wheeled")    == 0);
    CHECK(std::strcmp(locomotionModelName(LOCO_HOVER),       "Hover")      == 0);
    CHECK(std::strcmp(locomotionModelName(LOCO_INFANTRY),    "Infantry")   == 0);
}

TEST_CASE("damageSchemaName: exact converter spellings") {
    CHECK(std::strcmp(damageSchemaName(DMG_MECH_8PART),      "Mech8Part")      == 0);
    CHECK(std::strcmp(damageSchemaName(DMG_VEHICLE_FACINGS), "VehicleFacings") == 0);
    CHECK(std::strcmp(damageSchemaName(DMG_SQUAD),           "Squad")          == 0);
    CHECK(std::strcmp(damageSchemaName(DMG_SINGLE_ACTOR),    "SingleActor")    == 0);
}

TEST_CASE("locomotion name<->parse round-trip identity") {
    for (int i = 0; i < LOCO__COUNT; ++i) {
        const LocomotionModel m = static_cast<LocomotionModel>(i);
        // fallback deliberately differs so a no-op parse would be caught
        const LocomotionModel other =
            (m == LOCO_HOVER) ? LOCO_WHEELED : LOCO_HOVER;
        CHECK(parseLocomotionModel(locomotionModelName(m), other) == m);
    }
}

TEST_CASE("damage schema name<->parse round-trip identity") {
    const DamageSchemaId all[] = {
        DMG_MECH_8PART, DMG_VEHICLE_FACINGS, DMG_SQUAD, DMG_SINGLE_ACTOR};
    for (DamageSchemaId s : all) {
        const DamageSchemaId other =
            (s == DMG_SQUAD) ? DMG_SINGLE_ACTOR : DMG_SQUAD;
        CHECK(parseDamageSchema(damageSchemaName(s), other) == s);
    }
}
