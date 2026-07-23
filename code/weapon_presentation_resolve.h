#ifndef WEAPON_PRESENTATION_RESOLVE_H
#define WEAPON_PRESENTATION_RESOLVE_H
// ONE-WEAPON-PRESENTATION-PROOF-1 (equipment arc, engine slice E8) -- PURE helper.
//
// The ONE pure resolution rule E8 proves: given a weapon's data-side
// WeaponPresentationProfile and the LEGACY presentation asset the engine would
// otherwise use, decide WHICH asset id drives the OnFire / OnMuzzleFlash cue.
//
// ★ REDLINE (E8): this resolves ONLY the presentation ASSET CHOICE. It NEVER
// touches the fire decision, hit/miss, damage, heat, ammo, recycle, projectile /
// beam behavior, or timing. Legacy firing stays fully authoritative; this helper
// only answers "which effect id does the OnFire/OnMuzzleFlash cue use?".
//
// The MC2 weapon-fire VFX asset (the muzzle/bolt "special effect" id passed to
// ObjectManager->createWeaponBolt) is a small integer effect type. The data-side
// presentation carries the same asset as a STRING id (WeaponStringId) in the
// event grid; this helper parses that string to the legacy integer effect id.
//
// RESOLUTION RULE (data-present -> data asset; data-absent -> legacy):
//   * If the profile maps a Vfx asset for OnMuzzleFlash (or, as a fallback, for
//     OnFire) whose string PARSES to a non-negative integer -> return that int
//     and set fromData = true. This is the "data drives it" path.
//   * Otherwise (event unmapped, empty, or non-numeric) -> return the caller's
//     legacyEffectType unchanged and set fromData = false. This is the
//     "legacy fallback" path. Stock defs carry no populated presentation assets,
//     so stock ALWAYS takes this branch -> byte-identical.
//
// Pure, side-effect-free, GL/IO/engine-free -> compiles into the offline
// mc2_tests target and is doctest-tested (tests/unit/test_weapon_presentation_
// resolve.cpp). It reuses ONLY weapon_definition.h's pure presentation schema.

#include "weapon_definition.h"   // WeaponPresentationProfile, WeaponEvent, assetForEvent
#include <cstdlib>               // strtol

// Parse a WeaponStringId to a non-negative int effect id, or return -1 when the
// string is empty / not a clean non-negative integer. Pure.
inline int weaponPresentationAssetToEffectId(const WeaponStringId& asset) {
    if (asset.empty())
        return -1;
    const char* s = asset.c_str();
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (end == s || (end && *end != '\0'))
        return -1;                 // not a clean number
    if (v < 0)
        return -1;
    return (int)v;
}

// THE E8 resolution rule. Returns the effect id to use for the weapon's
// OnFire/OnMuzzleFlash VFX cue.
//   profile          : the weapon's data-side presentation profile.
//   legacyEffectType : the effect id the LEGACY path would use (the fallback).
//   fromData [out]   : true iff the returned id came from the data profile;
//                      false iff it fell back to legacyEffectType.
// Data-present (a numeric Vfx asset on OnMuzzleFlash, else OnFire) -> data id.
// Data-absent / non-numeric -> legacyEffectType (byte-identical for stock).
inline int resolveWeaponFireEffectType(const WeaponPresentationProfile& profile,
                                       int legacyEffectType,
                                       bool& fromData) {
    // Prefer the muzzle-flash cue (the visual that fires from the weapon node),
    // then the generic OnFire cue.
    int id = weaponPresentationAssetToEffectId(
                 assetForEvent(profile, WeaponEvent::OnMuzzleFlash, WeaponAssetKind::Vfx));
    if (id < 0)
        id = weaponPresentationAssetToEffectId(
                 assetForEvent(profile, WeaponEvent::OnFire, WeaponAssetKind::Vfx));

    if (id >= 0) {
        fromData = true;
        return id;
    }
    fromData = false;
    return legacyEffectType;
}

#endif // WEAPON_PRESENTATION_RESOLVE_H
