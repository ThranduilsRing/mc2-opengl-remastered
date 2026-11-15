// Offline tests
// for the PURE registry logic: slug derivation (must mirror the converter's
// tools/unit_converter/equipment_extract.py `_slug`), form->kind mapping, and the
// lookup/query API. No engine, no MasterComponent -- the population walk that
// reads MasterComponent lives in equipment_registry_engine.cpp (not built here).

#include "doctest.h"
#include <utility>
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
    CHECK(componentFormToKind(std::to_underlying(CFORM_WEAPON_ENERGY)) == EQ_WEAPON);
    CHECK(componentFormToKind(std::to_underlying(CFORM_WEAPON_BALLISTIC)) == EQ_WEAPON);
    CHECK(componentFormToKind(std::to_underlying(CFORM_WEAPON_MISSILE)) == EQ_WEAPON);
    CHECK(componentFormToKind(std::to_underlying(CFORM_WEAPON)) == EQ_WEAPON);
    CHECK(componentFormToKind(std::to_underlying(CFORM_AMMO)) == EQ_AMMO);
    CHECK(componentFormToKind(std::to_underlying(CFORM_SENSOR)) == EQ_SENSOR);
    CHECK(componentFormToKind(std::to_underlying(CFORM_PROBE)) == EQ_SENSOR);
    CHECK(componentFormToKind(std::to_underlying(CFORM_ECM)) == EQ_ECM);
    CHECK(componentFormToKind(std::to_underlying(CFORM_JAMMER)) == EQ_ECM);
    CHECK(componentFormToKind(std::to_underlying(CFORM_JUMPJET)) == EQ_JUMPJET);
    CHECK(componentFormToKind(std::to_underlying(CFORM_HEATSINK)) == EQ_HEATSINK);
    CHECK(componentFormToKind(std::to_underlying(CFORM_BULK)) == EQ_STRUCTURE);
    CHECK(componentFormToKind(std::to_underlying(CFORM_CASE)) == EQ_STRUCTURE);
    CHECK(componentFormToKind(std::to_underlying(CFORM_COCKPIT)) == EQ_COCKPIT);
    CHECK(componentFormToKind(std::to_underlying(CFORM_LIFESUPPORT)) == EQ_COCKPIT);
    CHECK(componentFormToKind(std::to_underlying(CFORM_ACTUATOR)) == EQ_ACTUATOR);
    CHECK(componentFormToKind(std::to_underlying(CFORM_GYROSCOPE)) == EQ_ACTUATOR);
    CHECK(componentFormToKind(std::to_underlying(CFORM_ENGINE)) == EQ_ENGINE);
    CHECK(componentFormToKind(std::to_underlying(CFORM_SIMPLE)) == EQ_OTHER);
    CHECK(componentFormToKind(std::to_underlying(CFORM_POWER_AMPLIFIER)) == EQ_OTHER);
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

// ---------------------------------------------------------------------------
// The pure enabled->purchase force rule.
// Full (defEnabled, legacyMasterId, rowPopulated) combo grid: the ONLY combo
// that flips available[] is enabled + in-range ordinal + populated row.
// ---------------------------------------------------------------------------
TEST_CASE("EQUIP-ENABLE-WIRE-1: equipmentEnableWireShouldForce combo grid") {
    // The one forcing combo.
    CHECK(equipmentEnableWireShouldForce(true, 113, true));
    CHECK(equipmentEnableWireShouldForce(true, 0, true));      // low bound ok
    CHECK(equipmentEnableWireShouldForce(true, 254, true));    // high bound ok

    // Not def-enabled (stock defs: Enabled key absent -> false) -> never force,
    // regardless of ordinal/population. Stock availability reproduced exactly.
    CHECK_FALSE(equipmentEnableWireShouldForce(false, 113, true));
    CHECK_FALSE(equipmentEnableWireShouldForce(false, 113, false));
    CHECK_FALSE(equipmentEnableWireShouldForce(false, -1, true));

    // Ordinal outside the 8-bit purchase array (available[255]) -> never force.
    CHECK_FALSE(equipmentEnableWireShouldForce(true, -1, true));   // no alias
    CHECK_FALSE(equipmentEnableWireShouldForce(true, 255, true));  // 0xff sentinel
    CHECK_FALSE(equipmentEnableWireShouldForce(true, 300, true));  // wide ordinal

    // Unpopulated ("undefined" compbas row): enabling it is the ADD fill pass's job.
    CHECK_FALSE(equipmentEnableWireShouldForce(true, 39, false));
    CHECK_FALSE(equipmentEnableWireShouldForce(true, 254, false));

    // ENABLE-ONLY contract: false result means "leave available[] alone", never
    // "clear it" -- there is no disabling combo by construction (pure predicate).
}

// Trigger: code/{equipment_registry.h,equipment_registry_engine.cpp,logisticsdata_init.cpp}.
TEST_CASE("equipment access: cold logistics and availability") {
    struct Case { bool legacy, capability, off, allowed; };
    for (const auto c : {Case{false, false, false, false}, // stock
                         Case{false, true, false, true},   // package-owned
                         Case{false, true, true, false},   // kill-switch
                         Case{true, false, false, true},   // legacy mode
                         Case{true, true, true, true}}) {
        const bool access = equipmentPackageAccessAllowed(c.legacy, c.capability, c.off);
        CHECK(access == c.allowed); // same predicate before cold master-list load
        bool available[255] = {};
        available[7] = true; // mission-purchased stock stays available
        if (access && equipmentEnableWireShouldForce(true, 60, true))
            available[60] = true;
        CHECK(available[7]);
        CHECK(available[60] == c.allowed);
        CHECK_FALSE(access && equipmentEnableWireShouldForce(false, 8, true));
        CHECK_FALSE(access && equipmentEnableWireShouldForce(true, 60, false));
    }
}

TEST_CASE("equipment registry looks up bounded IDs") {
    EquipmentRegistry reg;
    reg.add("core:laser", 7, EQ_OTHER, true);
    const char raw[] = {'c','o','r','e',':','l','a','s','e','r','!'};
    const std::string_view id(raw, 10);
    CHECK(reg.byQualifiedId(id) != nullptr);
    CHECK(reg.legacyMasterIdFor(id) == 7);
    CHECK(reg.byQualifiedId(std::string_view(raw, sizeof(raw))) == nullptr);
    CHECK(equipmentDefPathForQualifiedId(id) == "data/defs/equipment/core/laser/equipmentdef.fit");
}
