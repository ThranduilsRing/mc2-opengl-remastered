// EQUIPMENT-SOURCE-FLIP-SWITCH-1 -- offline tests for the PURE gate + overlay
// value logic (equipment_registry.h). No engine, no MasterComponent, no file I/O:
// the engine overlay pass (equipment_registry_engine.cpp) reads FitIniFile and is
// not built here; this covers only the deterministic decision/mapping rules.

#include "doctest.h"
#include "equipment_registry.h"

TEST_CASE("MC2_EQUIPMENT_DEFINITION_SOURCE gate parses only exact 'generated'") {
    // Default legacy: unset / empty / null.
    CHECK(mc2_equipment_definition_source_is_generated(nullptr) == false);
    CHECK(mc2_equipment_definition_source_is_generated("") == false);
    // The explicit legacy token and junk -> legacy (safe default).
    CHECK(mc2_equipment_definition_source_is_generated("legacy") == false);
    CHECK(mc2_equipment_definition_source_is_generated("0") == false);
    CHECK(mc2_equipment_definition_source_is_generated("gen") == false);
    CHECK(mc2_equipment_definition_source_is_generated("generatedX") == false);
    CHECK(mc2_equipment_definition_source_is_generated(" generated") == false);
    // Exact token (case-insensitive) -> generated.
    CHECK(mc2_equipment_definition_source_is_generated("generated") == true);
    CHECK(mc2_equipment_definition_source_is_generated("Generated") == true);
    CHECK(mc2_equipment_definition_source_is_generated("GENERATED") == true);
}

TEST_CASE("range bucket string maps to WeaponRangeType ordinal") {
    // short/medium/long -> 0/1/2 (WEAPON_RANGE_SHORT/MEDIUM/LONG).
    CHECK(mc2GeneratedRangeBucketToOrdinal("short",  -1) == 0);
    CHECK(mc2GeneratedRangeBucketToOrdinal("medium", -1) == 1);
    CHECK(mc2GeneratedRangeBucketToOrdinal("long",   -1) == 2);
    // case-insensitive.
    CHECK(mc2GeneratedRangeBucketToOrdinal("Medium", -1) == 1);
    CHECK(mc2GeneratedRangeBucketToOrdinal("LONG",   -1) == 2);
    // empty / null / unknown -> legacy fallback (per-field fallback).
    CHECK(mc2GeneratedRangeBucketToOrdinal("",       7) == 7);
    CHECK(mc2GeneratedRangeBucketToOrdinal(nullptr,  7) == 7);
    CHECK(mc2GeneratedRangeBucketToOrdinal("huge",   7) == 7);
}

TEST_CASE("GeneratedWeaponStats.any() gates the overlay per row") {
    GeneratedWeaponStats gs;                 // nothing present
    CHECK(gs.any() == false);                // -> row keeps legacy (no overlay)

    gs.hasDamage = true; gs.damage = 6.0f;
    CHECK(gs.any() == true);                 // one present field -> overlay runs

    GeneratedWeaponStats onlyAmmo;
    onlyAmmo.hasAmmoMasterId = true; onlyAmmo.ammoMasterId = 201;
    CHECK(onlyAmmo.any() == true);
}

TEST_CASE("per-field presence models the legacy fallback contract") {
    // A partial def: damage+heat present, recycle/range/CV/ammo absent. The
    // engine overlay applies ONLY present fields; absent ones keep legacy.
    GeneratedWeaponStats gs;
    gs.hasDamage = true;  gs.damage = 8.0f;
    gs.hasHeat   = true;  gs.heat   = 3.0f;
    CHECK(gs.hasDamage);
    CHECK(gs.hasHeat);
    CHECK(gs.hasRecycle == false);           // -> legacy recycle stands
    CHECK(gs.hasRangeBucket == false);       // -> legacy range stands
    CHECK(gs.hasCombatValue == false);       // -> legacy CV stands
    CHECK(gs.hasAmmoMasterId == false);      // -> legacy ammo linkage stands
    CHECK(gs.any());
}
