// tests/unit/test_equipment_definition.cpp
// EQUIPMENT-DEFINITION-SCHEMA-1 (equipment arc, slice 1).
//
// Pure-logic doctest coverage for equipment_definition.h: the top facet-aggregator
// type over the whole compbas component family. PURE SCHEMA -- no engine behavior.
// Covers: EquipmentKind + tags, the legacyMasterId-is-an-ALIAS-not-identity
// guarantee (two defs sharing a legacyMasterId but with different modId:name are
// DISTINCT; identity is the qualified string id), hasFacet/isWeapon, the
// weapon-equipment <-> hardpoint fit join THROUGH the WeaponFacet, and optional-
// facet presence flags. Header-only, drags no engine/GL/IO.
#include "doctest.h"
#include "equipment_definition.h"
#include <string>

// -------------------------------------------------------------------------
// Kind + tags.
// -------------------------------------------------------------------------

TEST_CASE("equipment kind: stored and readable; default is Other/-1/no facets") {
    EquipmentDefinition bare;
    CHECK(bare.kind == EQ_OTHER);
    CHECK(bare.legacyMasterId == -1);
    CHECK_FALSE(bare.hasWeapon);
    CHECK_FALSE(bare.hasAmmo);
    CHECK_FALSE(bare.hasSensor);
    CHECK_FALSE(bare.hasECM);
    CHECK_FALSE(bare.hasJumpJet);

    EquipmentDefinition jj = makeEquipment("core:jump_jet", EQ_JUMPJET);
    CHECK(jj.kind == EQ_JUMPJET);
}

TEST_CASE("equipment tags: OR-able set independent of kind") {
    EquipmentDefinition e = makeEquipment("core:clan_gauss", EQ_WEAPON);
    addEquipmentTag(e, ETAG_CLAN);
    addEquipmentTag(e, ETAG_EXPLOSIVE);
    CHECK(equipmentHasTag(e, ETAG_CLAN));
    CHECK(equipmentHasTag(e, ETAG_EXPLOSIVE));
    CHECK_FALSE(equipmentHasTag(e, ETAG_OMNI));
}

// -------------------------------------------------------------------------
// Identity is the modId:name STRING; legacyMasterId is an ALIAS, not identity.
// -------------------------------------------------------------------------

TEST_CASE("equipment identity: canonical modId:name string is the identity") {
    EquipmentDefinition a = makeEquipment("core:ac20", EQ_WEAPON);
    EquipmentDefinition b = makeEquipment("core:ac20", EQ_WEAPON);
    EquipmentDefinition c = makeEquipment("myMod:heavy_ppc", EQ_WEAPON);
    CHECK(a.valid());
    CHECK(a.idStr() == "core:ac20");
    CHECK(c.idStr() == "mymod:heavy_ppc");   // lowercase-canonicalized
    CHECK(sameEquipmentIdentity(a, b));      // same name => same identity
    CHECK_FALSE(sameEquipmentIdentity(a, c));

    // Invalid id string => not valid, no identity.
    EquipmentDefinition bad = makeEquipment("", EQ_OTHER);
    CHECK_FALSE(bad.valid());
    CHECK(bad.idStr().empty());
    CHECK_FALSE(sameEquipmentIdentity(bad, bad));  // invalid never has identity
}

TEST_CASE("legacyMasterId is an ALIAS, not identity: shared ordinal, distinct ids => distinct") {
    // Two DIFFERENT equipment (different modId:name) that happen to share the SAME
    // legacy compbas ordinal must remain DISTINCT -- identity is the string id, the
    // ordinal is only a shadow-path compatibility bridge. Mod load order (which
    // sets ordinals) must never change identity.
    EquipmentDefinition stock = makeEquipment("core:medium_laser", EQ_WEAPON);
    stock.legacyMasterId = 12;
    EquipmentDefinition modded = makeEquipment("myMod:medium_laser", EQ_WEAPON);
    modded.legacyMasterId = 12;   // SAME ordinal, on purpose

    CHECK(stock.legacyMasterId == modded.legacyMasterId);   // alias collides...
    CHECK_FALSE(sameEquipmentIdentity(stock, modded));      // ...identity does NOT
    CHECK(stock.idStr() != modded.idStr());

    // Conversely: the SAME equipment keeps its identity regardless of ordinal.
    EquipmentDefinition sameName1 = makeEquipment("core:ppc", EQ_WEAPON);
    sameName1.legacyMasterId = 7;
    EquipmentDefinition sameName2 = makeEquipment("core:ppc", EQ_WEAPON);
    sameName2.legacyMasterId = 99;   // different ordinal, same name
    CHECK(sameEquipmentIdentity(sameName1, sameName2));

    // A natively-authored mod component with no compbas row keeps the -1 default.
    EquipmentDefinition native = makeEquipment("myMod:plasma_rifle", EQ_WEAPON);
    CHECK(native.legacyMasterId == -1);
    CHECK(native.valid());
}

// -------------------------------------------------------------------------
// WeaponDefinition folds into WeaponFacet: one top type, weapon is a facet.
// -------------------------------------------------------------------------

TEST_CASE("weapon equipment: WeaponFacet composes, isWeapon/hasFacet/accessor") {
    EquipmentDefinition ac20 = makeEquipment("core:ac20", EQ_WEAPON);

    // WeaponFacet IS the folded-in weapon_definition.h content (== WeaponDefinition).
    WeaponFacet w;
    w.weaponId = makeWeaponId("core:ac20");
    w.category = WTAG_BALLISTIC;
    w.combat.damage = 12.0f;
    setWeaponFacet(ac20, w);

    CHECK(isWeapon(ac20));
    CHECK(hasFacet(ac20, EQ_WEAPON));
    CHECK_FALSE(hasFacet(ac20, EQ_AMMO));
    CHECK(ac20.kind == EQ_WEAPON);
    CHECK(weaponFacet(ac20).category == WTAG_BALLISTIC);
    CHECK(weaponFacet(ac20).combat.damage == doctest::Approx(12.0f));

    // The alias proves ONE top type: WeaponDefinition and WeaponFacet are the same.
    WeaponDefinition asDef = weaponFacet(ac20);
    CHECK(asDef.category == WTAG_BALLISTIC);
}

// -------------------------------------------------------------------------
// The unit-spine join THROUGH the WeaponFacet.
// -------------------------------------------------------------------------

static HardpointProfile ballisticHardpoint() {
    HardpointProfile hp;
    addWeaponTag(hp, WTAG_BALLISTIC);
    return hp;
}
static HardpointProfile energyHardpoint() {
    HardpointProfile hp;
    addWeaponTag(hp, WTAG_ENERGY);
    return hp;
}

TEST_CASE("equipmentFitsHardpoint: weapon-equipment fits via its WeaponFacet") {
    EquipmentDefinition ac20 = makeEquipment("core:ac20", EQ_WEAPON);
    WeaponFacet w; w.category = WTAG_BALLISTIC;
    setWeaponFacet(ac20, w);

    CHECK(equipmentFitsHardpoint(ac20, ballisticHardpoint()));
    CHECK_FALSE(equipmentFitsHardpoint(ac20, energyHardpoint()));

    // Non-weapon equipment never fits a weapon hardpoint.
    EquipmentDefinition sink = makeEquipment("core:heat_sink", EQ_HEATSINK);
    CHECK_FALSE(isWeapon(sink));
    CHECK_FALSE(equipmentFitsHardpoint(sink, ballisticHardpoint()));
}

// -------------------------------------------------------------------------
// Optional non-weapon facet presence flags.
// -------------------------------------------------------------------------

TEST_CASE("optional facet presence: ammo/sensor/ecm/jumpjet stubs flag independently") {
    EquipmentDefinition ammo = makeEquipment("core:ammo_ac20", EQ_AMMO);
    ammo.hasAmmo = true;
    ammo.ammo.ammoCategory = WeaponStringId("core:ammo_ac20");
    ammo.ammo.roundsPerTon = 5;
    CHECK(hasFacet(ammo, EQ_AMMO));
    CHECK_FALSE(hasFacet(ammo, EQ_WEAPON));
    CHECK(ammo.ammo.roundsPerTon == 5);

    EquipmentDefinition probe = makeEquipment("core:active_probe", EQ_SENSOR);
    probe.hasSensor = true;
    probe.sensor.sensorRange = 200.f;
    CHECK(hasFacet(probe, EQ_SENSOR));
    CHECK(probe.sensor.sensorRange == doctest::Approx(200.f));

    EquipmentDefinition ecm = makeEquipment("core:guardian_ecm", EQ_ECM);
    ecm.hasECM = true;
    CHECK(hasFacet(ecm, EQ_ECM));
    CHECK_FALSE(hasFacet(ecm, EQ_SENSOR));

    EquipmentDefinition jj = makeEquipment("core:jump_jet", EQ_JUMPJET);
    jj.hasJumpJet = true;
    jj.jumpJet.thrust = 1.0f;
    CHECK(hasFacet(jj, EQ_JUMPJET));
    CHECK(jj.jumpJet.thrust == doctest::Approx(1.0f));

    // Metadata + presentation slots default-empty and independently settable.
    EquipmentDefinition e = makeEquipment("core:xl_engine", EQ_ENGINE);
    CHECK(e.meta.heat == doctest::Approx(0.f));
    e.meta.cost = 100000.f;
    e.presentation.activateVfxId = WeaponStringId("vfx:engine_hum");
    CHECK(e.meta.cost == doctest::Approx(100000.f));
    CHECK(std::string(e.presentation.activateVfxId.c_str()) == "vfx:engine_hum");
}
