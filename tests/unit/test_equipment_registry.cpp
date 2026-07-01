// EQUIPMENT-REGISTRY-FACADE-1 (equipment arc, engine slice E2) -- offline tests
// for the PURE registry logic: slug derivation (must mirror the converter's
// tools/unit_converter/equipment_extract.py `_slug`), form->kind mapping, and the
// lookup/query API. No engine, no MasterComponent -- the population walk that
// reads MasterComponent lives in equipment_registry_engine.cpp (not built here).

#include "doctest.h"
#include "equipment_registry.h"

TEST_CASE("slugifyComponentName mirrors converter _slug") {
    // lowercase + non-alnum runs -> single '_' + strip.
    CHECK(slugifyComponentName("Medium Laser", 10) == "medium_laser");
    CHECK(slugifyComponentName("LRM 20", 11) == "lrm_20");
    // apostrophes dropped (not turned into '_').
    CHECK(slugifyComponentName("Gauss's Rifle", 12) == "gausss_rifle");
    // multiple non-alnum collapse and strip leading/trailing.
    CHECK(slugifyComponentName("  AC/20 (LBX) ", 13) == "ac_20_lbx");
    // empty name -> comp<id> fallback.
    CHECK(slugifyComponentName("", 42) == "comp42");
    CHECK(slugifyComponentName(nullptr, 7) == "comp7");
    // starts with a digit -> c_<s> fallback (id still parses under charset).
    CHECK(slugifyComponentName("20mm", 5) == "c_20mm");
    // all-symbol -> stripped empty -> comp<id>.
    CHECK(slugifyComponentName("///", 9) == "comp9");
}

TEST_CASE("coreQualifiedIdFor stamps core: and parses valid") {
    std::string id = coreQualifiedIdFor("Medium Laser", 10);
    CHECK(id == "core:medium_laser");
    CHECK(mc2id::parseQualifiedId(id).valid);
    CHECK(mc2id::parseQualifiedId(id).isCore());
    // fallback ids also parse.
    CHECK(mc2id::parseQualifiedId(coreQualifiedIdFor("", 3)).valid);
    CHECK(mc2id::parseQualifiedId(coreQualifiedIdFor("20mm", 5)).valid);
}

TEST_CASE("componentFormToKind mirrors converter TYPE_TO_KIND grouping") {
    CHECK(componentFormToKind(CFORM_WEAPON_ENERGY) == EQ_WEAPON);
    CHECK(componentFormToKind(CFORM_WEAPON_BALLISTIC) == EQ_WEAPON);
    CHECK(componentFormToKind(CFORM_WEAPON_MISSILE) == EQ_WEAPON);
    CHECK(componentFormToKind(CFORM_WEAPON) == EQ_WEAPON);
    CHECK(componentFormToKind(CFORM_AMMO) == EQ_AMMO);
    CHECK(componentFormToKind(CFORM_SENSOR) == EQ_SENSOR);
    CHECK(componentFormToKind(CFORM_PROBE) == EQ_SENSOR);
    CHECK(componentFormToKind(CFORM_ECM) == EQ_ECM);
    CHECK(componentFormToKind(CFORM_JAMMER) == EQ_ECM);
    CHECK(componentFormToKind(CFORM_JUMPJET) == EQ_JUMPJET);
    CHECK(componentFormToKind(CFORM_HEATSINK) == EQ_HEATSINK);
    CHECK(componentFormToKind(CFORM_BULK) == EQ_STRUCTURE);
    CHECK(componentFormToKind(CFORM_CASE) == EQ_STRUCTURE);
    CHECK(componentFormToKind(CFORM_COCKPIT) == EQ_COCKPIT);
    CHECK(componentFormToKind(CFORM_LIFESUPPORT) == EQ_COCKPIT);
    CHECK(componentFormToKind(CFORM_ACTUATOR) == EQ_ACTUATOR);
    CHECK(componentFormToKind(CFORM_GYROSCOPE) == EQ_ACTUATOR);
    CHECK(componentFormToKind(CFORM_ENGINE) == EQ_ENGINE);
    CHECK(componentFormToKind(CFORM_SIMPLE) == EQ_OTHER);
    CHECK(componentFormToKind(CFORM_POWER_AMPLIFIER) == EQ_OTHER);
}

TEST_CASE("registry query API: byId / byOrdinal / round-trip") {
    EquipmentRegistry reg;
    reg.add("core:medium_laser", 10, EQ_WEAPON, true);
    reg.add("core:lrm_20", 11, EQ_WEAPON, true);
    reg.add("core:comp200", 200, EQ_OTHER, false);  // default-off placeholder

    CHECK(reg.count() == 3);
    CHECK(reg.enabledCount() == 2);
    CHECK(reg.disabledCount() == 1);
    CHECK(reg.withLegacyMasterIdCount() == 3);

    const EquipmentRegistryEntry* e = reg.byQualifiedId("core:medium_laser");
    REQUIRE(e != nullptr);
    CHECK(e->legacyMasterId == 10);
    CHECK(e->kind == EQ_WEAPON);
    CHECK(e->enabled);

    const EquipmentRegistryEntry* e2 = reg.byLegacyMasterId(11);
    REQUIRE(e2 != nullptr);
    CHECK(e2->qualifiedId == "core:lrm_20");

    // round-trip both directions.
    CHECK(reg.qualifiedIdFor(10) == "core:medium_laser");
    CHECK(reg.legacyMasterIdFor("core:lrm_20") == 11);

    // misses.
    CHECK(reg.byQualifiedId("core:nope") == nullptr);
    CHECK(reg.byLegacyMasterId(999) == nullptr);
    CHECK(reg.qualifiedIdFor(999).empty());
    CHECK(reg.legacyMasterIdFor("core:nope") == -1);

    // disabled entry is still queryable (surfaced, not filtered out).
    const EquipmentRegistryEntry* d = reg.byLegacyMasterId(200);
    REQUIRE(d != nullptr);
    CHECK_FALSE(d->enabled);

    reg.clear();
    CHECK(reg.count() == 0);
}
