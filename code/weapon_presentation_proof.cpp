// code/weapon_presentation_proof.cpp
// ONE-WEAPON-PRESENTATION-PROOF-1 (equipment arc, engine slice E8) -- engine glue.
//
// The FINAL engine slice of the equipment arc. It proves ONE thing, brutally
// scoped: for ONE stock weapon (core:ppc, the PPC energy weapon at compbas
// masterID 145), the OnFire / OnMuzzleFlash presentation ASSET is resolved from
// its EquipmentDefinition's WeaponPresentationProfile data -- while LEGACY FIRING
// stays FULLY authoritative.
//
// ★★ REDLINE (E8): this changes ONLY which VFX asset id the ONE target weapon's
// muzzle/bolt cue uses. It NEVER touches: the fire decision, hit/miss, damage,
// heat, ammo, recycle/cooldown, projectile/beam behavior, or timing -- all of
// that is decided by the unchanged legacy weaponFire path in code/mech.cpp. It
// affects NO other weapon, ever.
//
// FALLBACK CONTRACT (why stock is byte-identical):
//   * gate MC2_UNIT_PROFILE_DATA OFF                       -> resolver returns the
//     legacy effect id unchanged (no data path taken).
//   * gate ON but the target def carries NO presentation asset (stock: none) ->
//     resolver falls back to the legacy effect id (source=LEGACY-FALLBACK).
//
// The pure resolution rule lives in weapon_presentation_resolve.h (offline
// doctest-tested); this TU is the thin engine binding that knows the ONE target
// weapon. (The throwaway MC2_EQUIP_PRESENTATION_PROOF parity-proof gate was
// removed post-E8; a real content asset is the future data source here.)

#include "weapon_presentation_resolve.h"   // resolveWeaponFireEffectType (pure)
#include "equipment_definition.h"          // EquipmentDefinition + WeaponPresentationProfile
#include "unitprofile.h"                   // mc2UnitProfileDataEnabled()
#include "cmponent.h"                      // MasterComponent + masterList + getName

// Is `masterID` the ONE target weapon? Match by NAME (the identity), not ordinal:
// slugify the loaded component's name and compare to core:ppc. Cheap + row-shift
// safe. Returns false for every other component -> no behavior change elsewhere.
static bool isTargetWeaponMasterId(long masterID) {
    if (!MasterComponent::masterList)
        return false;
    if (masterID < 0 || masterID >= MasterComponent::numComponents)
        return false;
    const char* nm = MasterComponent::masterList[masterID].getName();
    // slug of "PPC" == "ppc"; core:<slug> == core:ppc. Reuse the registry's exact
    // slug rule via a direct compare to avoid an extra include here.
    // (coreQualifiedIdFor would drag equipment_registry.h; a lowercase compare of
    //  the stock name is sufficient for the single target and keeps this cheap.)
    if (!nm || nm[0] == '\0')
        return false;
    // Compare case-insensitively against "PPC" -- the stock PPC row name.
    static const char* kName = "PPC";
    const char* a = nm; const char* b = kName;
    for (; *a && *b; ++a, ++b) {
        char ca = *a; if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        char cb = *b; if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return false;
    }
    return *a == '\0' && *b == '\0';
}

// Build the ONE target weapon's EquipmentDefinition presentation profile. Stock
// carries NO populated presentation assets, so this is EMPTY (every event
// unmapped) -> the resolver falls back to legacy. A future content slice
// populates it from real EquipmentDefinition data.
static WeaponPresentationProfile targetWeaponPresentation() {
    return WeaponPresentationProfile();   // stock: nothing mapped -> legacy fallback
}

// -------------------------------------------------------------------------
// THE E8 seam entry. Called from the ONE weapon-fire VFX emit point in
// BattleMech::handleWeaponFire (code/mech.cpp). For the ONE target weapon under
// the gate, resolve the OnFire/OnMuzzleFlash effect id from EquipmentDefinition
// data (falling back to legacy). For every OTHER weapon, or gate-OFF, returns
// legacyEffectType unchanged -> byte-identical, no behavior change.
// -------------------------------------------------------------------------
int resolveTargetWeaponFireEffectType(long masterID, int legacyEffectType) {
    // Gate OFF -> legacy path only (parity). No lookup, no logging.
    if (!mc2UnitProfileDataEnabled())
        return legacyEffectType;

    // Only the ONE target weapon is intercepted; all others keep legacy exactly.
    if (!isTargetWeaponMasterId(masterID))
        return legacyEffectType;

    WeaponPresentationProfile p = targetWeaponPresentation();
    bool fromData = false;
    return resolveWeaponFireEffectType(p, legacyEffectType, fromData);
}
