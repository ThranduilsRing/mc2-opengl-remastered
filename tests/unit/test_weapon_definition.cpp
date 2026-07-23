// tests/unit/test_weapon_definition.cpp
// WEAPONDEF-OFFLINE-HARNESS-1 / WEAPONDEF-SCHEMA-1 (Phase 0 of the WEAPON-DEFINITION arc).
//
// Pure-logic doctest coverage for weapon_definition.h: the data-driven
// "what is a weapon" schema. PURE SCHEMA -- no engine behavior. Covers:
// WeaponId value equality + name round-trip (the 255-cap kill), category tag,
// the weapon<->hardpoint fit join reusing hardpointAcceptsWeapon(), the
// first-class VFX/SFX presentation facet, and the named ammo-category string.
// Header-only, drags no engine/GL/IO. Mirrors test_hardpoint.cpp shape.
#include "doctest.h"
#include "weapon_definition.h"
#include <string>

// -------------------------------------------------------------------------
// WeaponId: value equality + debug-name round-trip (the string-id identity).
// -------------------------------------------------------------------------

TEST_CASE("weapon id: same name => equal, distinct names => not equal") {
    const WeaponId a = makeWeaponId("core:ac20");
    const WeaponId b = makeWeaponId("core:ac20");
    const WeaponId c = makeWeaponId("core:lrm5");
    CHECK(a == b);
    CHECK(a != c);
    CHECK(b != c);
    // Namespaced mod id is distinct from the core id.
    const WeaponId d = makeWeaponId("myMod:heavy_ppc");
    CHECK(a != d);
    CHECK(c != d);
}

TEST_CASE("weapon id: unset default has id 0 and is invalid") {
    const WeaponId unset;
    CHECK(unset.id == 0u);
    CHECK_FALSE(unset.valid());
    const WeaponId set = makeWeaponId("core:ppc");
    CHECK(set.valid());
    CHECK(set.id != 0u);
    // Empty string hashes to the unset id.
    CHECK_FALSE(makeWeaponId("").valid());
}

TEST_CASE("weapon id: debug-name round-trips for names within the buffer") {
    const char* kName = "core:heavy_autocannon_ac20";
    const WeaponId w = makeWeaponId(kName);
    CHECK(std::string(w.debugName()) == std::string(kName));
}

TEST_CASE("weapon id: 255-cap kill -- identity is a name, not an ordinal") {
    // The whole point: adding a new weapon coins a fresh identity WITHOUT
    // disturbing any existing weapon's id (no array-index renumbering). Two
    // arbitrary distinct names never collide; a mod id is independent of core.
    const WeaponId core0 = makeWeaponId("core:weapon_000");
    const WeaponId core254 = makeWeaponId("core:weapon_254");
    const WeaponId modNew = makeWeaponId("myMod:weapon_1000"); // "beyond 255"
    CHECK(core0 != core254);
    CHECK(core0 != modNew);
    CHECK(core254 != modNew);
    // Re-coining the same name is stable (idempotent identity).
    CHECK(makeWeaponId("myMod:weapon_1000") == modNew);
}

// -------------------------------------------------------------------------
// Category tag + the weapon<->hardpoint fit join (reuses hardpointAcceptsWeapon).
// -------------------------------------------------------------------------

// Build an energy-only hardpoint.
static HardpointProfile energyHardpoint() {
    HardpointProfile hp;
    addWeaponTag(hp, WTAG_ENERGY);
    return hp;
}
// Build a ballistic-only hardpoint.
static HardpointProfile ballisticHardpoint() {
    HardpointProfile hp;
    addWeaponTag(hp, WTAG_BALLISTIC);
    return hp;
}

TEST_CASE("weapon category: stored and readable on the definition") {
    WeaponDefinition ppc;
    ppc.weaponId = makeWeaponId("core:ppc");
    ppc.category = WTAG_ENERGY;
    CHECK(ppc.category == WTAG_ENERGY);

    WeaponDefinition ac20;
    ac20.category = WTAG_BALLISTIC;
    CHECK(ac20.category == WTAG_BALLISTIC);
}

TEST_CASE("weaponFitsHardpoint: energy weapon fits energy hardpoint") {
    WeaponDefinition laser;
    laser.weaponId = makeWeaponId("core:large_laser");
    laser.category = WTAG_ENERGY;
    const HardpointProfile hp = energyHardpoint();
    CHECK(weaponFitsHardpoint(laser, hp));
}

TEST_CASE("weaponFitsHardpoint: AC on energy hardpoint => false") {
    WeaponDefinition ac20;
    ac20.weaponId = makeWeaponId("core:ac20");
    ac20.category = WTAG_BALLISTIC;
    const HardpointProfile energyHp = energyHardpoint();
    CHECK_FALSE(weaponFitsHardpoint(ac20, energyHp));
    // ...but the same AC fits a ballistic hardpoint.
    const HardpointProfile ballisticHp = ballisticHardpoint();
    CHECK(weaponFitsHardpoint(ac20, ballisticHp));
}

TEST_CASE("weaponFitsHardpoint: multi-tag hardpoint accepts either class") {
    HardpointProfile omni;
    addWeaponTag(omni, WTAG_ENERGY);
    addWeaponTag(omni, WTAG_MISSILE);

    WeaponDefinition laser; laser.category = WTAG_ENERGY;
    WeaponDefinition lrm;   lrm.category   = WTAG_MISSILE;
    WeaponDefinition ac;    ac.category    = WTAG_BALLISTIC;
    CHECK(weaponFitsHardpoint(laser, omni));
    CHECK(weaponFitsHardpoint(lrm, omni));
    CHECK_FALSE(weaponFitsHardpoint(ac, omni));   // ballistic not in accept-set
}

// -------------------------------------------------------------------------
// Presentation facet: VFX/SFX are first-class named fields (defs -> VFX/SFX).
// -------------------------------------------------------------------------

TEST_CASE("presentation facet: muzzle/impact VFX and fire/impact SFX are present and named") {
    WeaponDefinition ppc;
    ppc.presentation.muzzleVfxId  = WeaponStringId("vfx:ppc_muzzle");
    ppc.presentation.impactVfxId  = WeaponStringId("vfx:ppc_impact");
    ppc.presentation.fireSfxId    = WeaponStringId("sfx:ppc_fire");
    ppc.presentation.impactSfxId  = WeaponStringId("sfx:ppc_hit");

    CHECK(std::string(ppc.presentation.muzzleVfxId.c_str()) == "vfx:ppc_muzzle");
    CHECK(std::string(ppc.presentation.impactVfxId.c_str()) == "vfx:ppc_impact");
    CHECK(std::string(ppc.presentation.fireSfxId.c_str())   == "sfx:ppc_fire");
    CHECK(std::string(ppc.presentation.impactSfxId.c_str()) == "sfx:ppc_hit");

    // Default-constructed presentation ids are empty ("not mapped").
    WeaponDefinition bare;
    CHECK(bare.presentation.muzzleVfxId.empty());
    CHECK(bare.presentation.fireSfxId.empty());
}

// -------------------------------------------------------------------------
// Ammo facet: named category string, NOT an index/offset.
// -------------------------------------------------------------------------

TEST_CASE("ammo facet: named category string replaces index/offset arithmetic") {
    WeaponDefinition ac5;
    ac5.ammo.ammoCategory = WeaponStringId("core:ammo_ac5");
    ac5.ammo.ammoPerShot  = 1;
    CHECK(ac5.ammo.usesAmmo());
    CHECK(std::string(ac5.ammo.ammoCategory.c_str()) == "core:ammo_ac5");

    // Energy weapon: no ammo category => self-powered.
    WeaponDefinition laser;
    CHECK_FALSE(laser.ammo.usesAmmo());
    CHECK(laser.ammo.ammoCategory.empty());
}

// -------------------------------------------------------------------------
// Combat + fit facets: plain numbers / reused DamageLocationId membership.
// -------------------------------------------------------------------------

TEST_CASE("combat stats: plain-number facet round-trips") {
    WeaponDefinition ac20;
    ac20.combat.damage      = 12.0f;
    ac20.combat.heat        = 7.0f;
    ac20.combat.recycleTime = 5.88f;
    ac20.combat.rangeShort  = 100.f;
    ac20.combat.rangeMed    = 200.f;
    ac20.combat.rangeLong   = 300.f;
    ac20.combat.combatValue = 545.11f;
    CHECK(ac20.combat.damage == doctest::Approx(12.0f));
    CHECK(ac20.combat.rangeLong == doctest::Approx(300.f));
    CHECK(ac20.combat.combatValue == doctest::Approx(545.11f));
}

TEST_CASE("fit facet: allowed damage-locations reuse DamageLocationId vocabulary") {
    WeaponDefinition ac20;
    ac20.fit.critSlots = 10;
    ac20.fit.tonnage   = 14.f;
    ac20.fit.allowLocation(DLOC_MECH_RTORSO);
    ac20.fit.allowLocation(DLOC_MECH_RARM);
    CHECK(ac20.fit.allowsLocation(DLOC_MECH_RTORSO));
    CHECK(ac20.fit.allowsLocation(DLOC_MECH_RARM));
    CHECK_FALSE(ac20.fit.allowsLocation(DLOC_MECH_HEAD));

    // Unconstrained ("All"): count 0 accepts anything.
    WeaponDefinition anywhere;
    CHECK(anywhere.fit.allowedCount == 0);
    CHECK(anywhere.fit.allowsLocation(DLOC_MECH_HEAD));
    CHECK(anywhere.fit.allowsLocation(DLOC_VEH_TURRET));
}

// -------------------------------------------------------------------------
// UI facet.
// -------------------------------------------------------------------------

TEST_CASE("ui facet: icon and display name by string id") {
    WeaponDefinition ac20;
    ac20.ui.iconId = WeaponStringId("icon:weapon_ballistic");
    ac20.ui.uiName = WeaponStringId("AC/20");
    CHECK(std::string(ac20.ui.iconId.c_str()) == "icon:weapon_ballistic");
    CHECK(std::string(ac20.ui.uiName.c_str()) == "AC/20");
}
