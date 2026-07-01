// tests/unit/test_equipment_validator.cpp
// WEAPON-VALIDATOR-1 (equipment arc, slice 6).
//
// Pure-logic doctest coverage for equipment_validator.h: the construction-rules
// validator for EquipmentDefinition / WeaponFacet. Every LIVE reason code must be
// reachable, a coherent stock-shaped weapon must yield ZERO findings, and the
// EQV_* vocabulary size is locked. Header-only, drags no engine/GL/IO. Mirrors
// test_unit_profile_validator.cpp.
#include "doctest.h"
#include "equipment_validator.h"

// -------------------------------------------------------------------------
// A coherent, stock-shaped weapon: valid id, weapon kind + facet, authored
// category, an executor (as the converter defaults), sane monotone ranges,
// positive damage, and -- for the ballistic case -- a named ammo category.
// -------------------------------------------------------------------------
static EquipmentDefinition goodEnergyWeapon() {
    EquipmentDefinition e = makeEquipment("core:medium_laser", EQ_WEAPON);
    WeaponFacet w;
    w.weaponId     = makeWeaponId("core:medium_laser");
    w.category     = WTAG_ENERGY;
    w.fireExecutor = makeFireExecutorId("beam_direct");
    w.combat.damage     = 5.f;
    w.combat.rangeShort = 90.f;
    w.combat.rangeMed   = 180.f;
    w.combat.rangeLong  = 270.f;
    // energy: no ammo (ammoPerShot stays 0)
    setWeaponFacet(e, w);
    return e;
}

static EquipmentDefinition goodBallisticWeapon() {
    EquipmentDefinition e = makeEquipment("core:ac20", EQ_WEAPON);
    WeaponFacet w;
    w.weaponId     = makeWeaponId("core:ac20");
    w.category     = WTAG_BALLISTIC;
    w.fireExecutor = makeFireExecutorId("projectile_ballistic");
    w.combat.damage     = 20.f;
    w.combat.rangeShort = 30.f;
    w.combat.rangeMed   = 60.f;
    w.combat.rangeLong  = 90.f;
    w.ammo.ammoPerShot  = 1;
    w.ammo.ammoCategory = WeaponStringId("core:ammo_ac20");
    setWeaponFacet(e, w);
    return e;
}

// -------------------------------------------------------------------------
// Stock parity: a coherent weapon yields ZERO findings.
// -------------------------------------------------------------------------

TEST_CASE("equip: coherent stock energy weapon -> ZERO findings") {
    EquipmentValidationInputs in;
    in.def = goodEnergyWeapon();
    CHECK(validateEquipmentDefinition(in).empty());
}

TEST_CASE("equip: coherent stock ballistic weapon (ammo named) -> ZERO findings") {
    EquipmentValidationInputs in;
    in.def = goodBallisticWeapon();
    CHECK(validateEquipmentDefinition(in).empty());
}

TEST_CASE("equip: coherent weapon accepted by supplied hardpoint -> ZERO findings") {
    EquipmentValidationInputs in;
    in.def = goodEnergyWeapon();
    HardpointProfile hp;
    hp.id = makeHardpointId("RightTorso_Energy_1");
    addWeaponTag(hp, WTAG_ENERGY);   // accepts energy -> coherent
    in.hardpointContext.push_back(hp);
    CHECK(validateEquipmentDefinition(in).empty());
}

// -------------------------------------------------------------------------
// LIVE reason codes -- each reachable.
// -------------------------------------------------------------------------

TEST_CASE("equip: weapon with no fire executor -> EQV_WEAPON_NO_FIRE_EXECUTOR") {
    EquipmentValidationInputs in;
    in.def = goodEnergyWeapon();
    in.def.weapon.fireExecutor = WeaponFireExecutorId();   // unset
    std::vector<EquipmentFinding> f = validateEquipmentDefinition(in);
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == EQV_WEAPON_NO_FIRE_EXECUTOR);
    CHECK(f[0].message != nullptr);
}

TEST_CASE("equip: weapon with no authored category -> EQV_WEAPON_NO_CATEGORY") {
    EquipmentValidationInputs in;
    in.def = goodEnergyWeapon();
    in.categoryAuthored = false;
    std::vector<EquipmentFinding> f = validateEquipmentDefinition(in);
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == EQV_WEAPON_NO_CATEGORY);
}

TEST_CASE("equip: ammo-using weapon with no ammo category -> EQV_AMMO_CATEGORY_MISSING") {
    EquipmentValidationInputs in;
    in.def = goodBallisticWeapon();
    in.def.weapon.ammo.ammoCategory = WeaponStringId();   // fires ammo but no category
    std::vector<EquipmentFinding> f = validateEquipmentDefinition(in);
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == EQV_AMMO_CATEGORY_MISSING);
}

TEST_CASE("equip: AMMO-kind item with no named category -> EQV_AMMO_CATEGORY_MISSING") {
    EquipmentValidationInputs in;
    in.def = makeEquipment("core:ammo_ac20", EQ_AMMO);
    in.def.hasAmmo = true;   // AmmoFacet default: empty category
    std::vector<EquipmentFinding> f = validateEquipmentDefinition(in);
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == EQV_AMMO_CATEGORY_MISSING);
}

TEST_CASE("equip: negative range -> EQV_WEAPON_RANGE_INVALID") {
    EquipmentValidationInputs in;
    in.def = goodEnergyWeapon();
    in.def.weapon.combat.rangeShort = -1.f;
    std::vector<EquipmentFinding> f = validateEquipmentDefinition(in);
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == EQV_WEAPON_RANGE_INVALID);
}

TEST_CASE("equip: disordered ranges (short > med) -> EQV_WEAPON_RANGE_INVALID") {
    EquipmentValidationInputs in;
    in.def = goodEnergyWeapon();
    in.def.weapon.combat.rangeShort = 500.f;   // > med (180)
    std::vector<EquipmentFinding> f = validateEquipmentDefinition(in);
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == EQV_WEAPON_RANGE_INVALID);
}

TEST_CASE("equip: negative damage -> EQV_WEAPON_RANGE_INVALID") {
    EquipmentValidationInputs in;
    in.def = goodEnergyWeapon();
    in.def.weapon.combat.damage = -3.f;
    std::vector<EquipmentFinding> f = validateEquipmentDefinition(in);
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == EQV_WEAPON_RANGE_INVALID);
}

TEST_CASE("equip: kind/facet mismatch -- weapon kind, no facet -> EQV_KIND_FACET_MISMATCH") {
    EquipmentValidationInputs in;
    in.def = makeEquipment("core:broken", EQ_WEAPON);   // kind weapon, hasWeapon=false
    std::vector<EquipmentFinding> f = validateEquipmentDefinition(in);
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == EQV_KIND_FACET_MISMATCH);
}

TEST_CASE("equip: kind/facet mismatch -- facet present, kind not weapon -> EQV_KIND_FACET_MISMATCH") {
    EquipmentValidationInputs in;
    in.def = goodEnergyWeapon();
    in.def.kind = EQ_SENSOR;   // facet present but kind lies
    std::vector<EquipmentFinding> f = validateEquipmentDefinition(in);
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == EQV_KIND_FACET_MISMATCH);
}

TEST_CASE("equip: claimed presentation event with no asset -> EQV_PRESENTATION_ASSET_MISSING") {
    EquipmentValidationInputs in;
    in.def = goodEnergyWeapon();
    // Claim OnImpact but map no asset for it -> declared-but-empty.
    in.claimedEvents.push_back(ClaimedWeaponEvent(WeaponEvent::OnImpact));
    std::vector<EquipmentFinding> f = validateEquipmentDefinition(in);
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == EQV_PRESENTATION_ASSET_MISSING);
}

TEST_CASE("equip: claimed event WITH an asset -> ZERO findings") {
    EquipmentValidationInputs in;
    in.def = goodEnergyWeapon();
    in.def.weapon.presentation.setEventAsset(
        WeaponEvent::OnImpact, WeaponAssetKind::Vfx, "core:vfx_scorch");
    in.claimedEvents.push_back(ClaimedWeaponEvent(WeaponEvent::OnImpact));
    CHECK(validateEquipmentDefinition(in).empty());
}

TEST_CASE("equip: weapon rejected by supplied hardpoint -> EQV_WEAPON_HARDPOINT_REJECTED") {
    EquipmentValidationInputs in;
    in.def = goodBallisticWeapon();   // WTAG_BALLISTIC
    HardpointProfile hp;
    hp.id = makeHardpointId("RightTorso_Energy_1");
    addWeaponTag(hp, WTAG_ENERGY);    // energy-only -> ballistic rejected
    in.hardpointContext.push_back(hp);
    std::vector<EquipmentFinding> f = validateEquipmentDefinition(in);
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == EQV_WEAPON_HARDPOINT_REJECTED);
}

// -------------------------------------------------------------------------
// A deliberately-broken weapon trips several codes at once.
// -------------------------------------------------------------------------

TEST_CASE("equip: multiply-broken weapon accumulates the expected findings") {
    EquipmentValidationInputs in;
    in.def = goodBallisticWeapon();
    in.def.weapon.fireExecutor = WeaponFireExecutorId();       // NO_FIRE_EXECUTOR
    in.def.weapon.ammo.ammoCategory = WeaponStringId();        // AMMO_CATEGORY_MISSING
    in.def.weapon.combat.rangeShort = 999.f;                   // RANGE_INVALID (short>med)
    in.categoryAuthored = false;                               // NO_CATEGORY

    std::vector<EquipmentFinding> f = validateEquipmentDefinition(in);
    bool seen[EQV__COUNT] = {false};
    for (const EquipmentFinding& x : f) seen[x.code] = true;
    CHECK(seen[EQV_WEAPON_NO_FIRE_EXECUTOR]);
    CHECK(seen[EQV_WEAPON_NO_CATEGORY]);
    CHECK(seen[EQV_AMMO_CATEGORY_MISSING]);
    CHECK(seen[EQV_WEAPON_RANGE_INVALID]);
    CHECK(f.size() == 4);
}

// -------------------------------------------------------------------------
// Every LIVE reason code reachable + vocabulary-size lock.
// -------------------------------------------------------------------------

TEST_CASE("equip: every LIVE reason code is reachable + vocabulary-size lock") {
    // Reachability is proven individually above; here we assert coverage and lock
    // the vocabulary size so a future slice APPENDS (never renumbers).
    // EQV codes: NO_FIRE_EXECUTOR, PRESENTATION_ASSET_MISSING, NO_CATEGORY,
    // AMMO_CATEGORY_MISSING, RANGE_INVALID, KIND_FACET_MISMATCH, HARDPOINT_REJECTED.
    CHECK(EQV__COUNT == 7);
}
