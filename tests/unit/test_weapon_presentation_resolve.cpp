// tests/unit/test_weapon_presentation_resolve.cpp
// ONE-WEAPON-PRESENTATION-PROOF-1 (equipment arc, engine slice E8) -- pure doctest.
//
// Covers the ONE pure resolution rule E8 introduces (weapon_presentation_resolve.h):
// resolve the OnFire/OnMuzzleFlash VFX asset id from a data-side
// WeaponPresentationProfile, falling back to the legacy effect id when the data
// profile carries no (numeric) presentation asset. This is the ASSET CHOICE only:
// no firing / damage / heat / ammo / timing is modeled here (nor could be -- pure).
#include "doctest.h"
#include "weapon_presentation_resolve.h"

// ---- weaponPresentationAssetToEffectId: string -> effect id parse ----
TEST_CASE("asset->effectId: empty and non-numeric -> -1; numeric -> value") {
    CHECK(weaponPresentationAssetToEffectId(WeaponStringId()) == -1);        // empty
    CHECK(weaponPresentationAssetToEffectId(WeaponStringId("")) == -1);      // empty
    CHECK(weaponPresentationAssetToEffectId(WeaponStringId("laser")) == -1); // non-numeric
    CHECK(weaponPresentationAssetToEffectId(WeaponStringId("4x")) == -1);    // trailing junk
    CHECK(weaponPresentationAssetToEffectId(WeaponStringId("-1")) == -1);    // negative rejected
    CHECK(weaponPresentationAssetToEffectId(WeaponStringId("0")) == 0);
    CHECK(weaponPresentationAssetToEffectId(WeaponStringId("4")) == 4);
    CHECK(weaponPresentationAssetToEffectId(WeaponStringId("255")) == 255);
}

// ---- data-ABSENT -> legacy fallback (the stock / byte-identical path) ----
TEST_CASE("resolve: empty profile falls back to legacy effect id (fromData=false)") {
    WeaponPresentationProfile p;   // stock shape: nothing mapped
    bool fromData = true;
    int id = resolveWeaponFireEffectType(p, /*legacy=*/4, fromData);
    CHECK(id == 4);                // legacy asset preserved exactly
    CHECK_FALSE(fromData);         // proves it fell back to legacy
}

// ---- data-PRESENT -> data asset drives it (the proof path) ----
TEST_CASE("resolve: OnMuzzleFlash Vfx present -> data asset drives (fromData=true)") {
    WeaponPresentationProfile p;
    p.setEventAsset(WeaponEvent::OnMuzzleFlash, WeaponAssetKind::Vfx, "4");
    bool fromData = false;
    // Parity-safe proof: data id == legacy id (4). Data drives it, value identical.
    int id = resolveWeaponFireEffectType(p, /*legacy=*/4, fromData);
    CHECK(id == 4);
    CHECK(fromData);               // proves the DATA path selected the asset
}

TEST_CASE("resolve: OnFire Vfx used when OnMuzzleFlash unmapped") {
    WeaponPresentationProfile p;
    p.setEventAsset(WeaponEvent::OnFire, WeaponAssetKind::Vfx, "7");
    bool fromData = false;
    int id = resolveWeaponFireEffectType(p, /*legacy=*/4, fromData);
    CHECK(id == 7);                // data value != legacy -> data authoritative for ASSET
    CHECK(fromData);
}

TEST_CASE("resolve: OnMuzzleFlash preferred over OnFire when both mapped") {
    WeaponPresentationProfile p;
    p.setEventAsset(WeaponEvent::OnFire, WeaponAssetKind::Vfx, "7");
    p.setEventAsset(WeaponEvent::OnMuzzleFlash, WeaponAssetKind::Vfx, "9");
    bool fromData = false;
    int id = resolveWeaponFireEffectType(p, /*legacy=*/4, fromData);
    CHECK(id == 9);               // muzzle-flash cue wins
    CHECK(fromData);
}

TEST_CASE("resolve: non-numeric data asset -> legacy fallback (unresolvable)") {
    WeaponPresentationProfile p;
    p.setEventAsset(WeaponEvent::OnMuzzleFlash, WeaponAssetKind::Vfx, "muzzle_ppc");
    bool fromData = true;
    int id = resolveWeaponFireEffectType(p, /*legacy=*/4, fromData);
    CHECK(id == 4);              // can't parse -> keep legacy
    CHECK_FALSE(fromData);
}
