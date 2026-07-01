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
//   * proof gate MC2_EQUIP_PRESENTATION_PROOF ON populates the target def's
//     OnMuzzleFlash/Vfx with the SAME id the legacy path uses -> the data path
//     resolves the (legacy-equal) asset (source=DATA) with NOTHING seen/heard
//     changing. This proves data DRIVES it when present, legacy when absent.
//
// The pure resolution rule lives in weapon_presentation_resolve.h (offline
// doctest-tested); this TU is the thin engine binding that knows the ONE target
// weapon and does the one-shot proof logging.

#include "weapon_presentation_resolve.h"   // resolveWeaponFireEffectType (pure)
#include "equipment_definition.h"          // EquipmentDefinition + WeaponPresentationProfile
#include "unitprofile.h"                   // mc2UnitProfileDataEnabled()
#include "cmponent.h"                      // MasterComponent + masterList + getName

#include <cstdlib>                         // getenv
#include <cstdio>                          // printf

// The ONE target weapon's canonical qualified id. Its legacy alias (compbas row
// ordinal) is resolved at runtime by NAME so a row shift never mis-targets.
static const char* kTargetWeaponQualifiedId = "core:ppc";

// Throwaway proof gate: SEPARATE env from MC2_UNIT_PROFILE_DATA, default OFF.
// When ON, the target weapon's EquipmentDefinition presentation is populated with
// the SAME id the legacy path uses (parity-safe), so the data path can be proven
// to drive the (identical) asset. Remove once the arc has a real content asset.
static bool mc2EquipPresentationProofEnabled() {
    static const bool on = mc2_unitprofile_parse_enabled(
        getenv("MC2_EQUIP_PRESENTATION_PROOF"));
    return on;
}

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
// carries NO populated presentation assets, so by default this is EMPTY (every
// event unmapped) -> the resolver falls back to legacy. Under the proof gate the
// OnMuzzleFlash/Vfx is populated with the caller's legacy id (parity-safe).
static WeaponPresentationProfile targetWeaponPresentation(int legacyEffectType) {
    WeaponPresentationProfile p;   // stock: nothing mapped -> legacy fallback
    if (mc2EquipPresentationProofEnabled()) {
        char idBuf[16];
        std::snprintf(idBuf, sizeof(idBuf), "%d", legacyEffectType);
        // Parity-safe: populate with the SAME id the legacy path uses, so the
        // resolved asset is IDENTICAL -- data drives it, nothing changes visually.
        p.setEventAsset(WeaponEvent::OnMuzzleFlash, WeaponAssetKind::Vfx, idBuf);
    }
    return p;
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

    WeaponPresentationProfile p = targetWeaponPresentation(legacyEffectType);
    bool fromData = false;
    int resolved = resolveWeaponFireEffectType(p, legacyEffectType, fromData);

    // One-shot proof line (throttled to first fire) so a soak run isn't spammed.
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        printf("[EQUIP-PRESENTATION-PROOF] %s OnMuzzleFlash vfx resolved from data = %d (source=%s)\n",
               kTargetWeaponQualifiedId, resolved,
               fromData ? "DATA" : "LEGACY-FALLBACK");
    }

    return resolved;
}

// -------------------------------------------------------------------------
// Load-time proof dump (gated MC2_UNIT_PROFILE_DATA). Mirrors the E2/E7 proof
// dumps: it exercises the SAME pure resolver on the SAME target weapon's data at
// load, so the proof is deterministically observable without waiting for a live
// PPC to fire in-mission. It resolves the target weapon's OnMuzzleFlash asset:
//   * proof gate OFF (stock, no data asset) -> source=LEGACY-FALLBACK
//   * proof gate ON  (parity-safe id populated) -> source=DATA (value identical)
// This proves the data path DRIVES the asset when present and legacy when absent,
// with NOTHING seen/heard changing (parity-safe id == legacy id). ★ Resolves an
// ASSET id only; fires nothing, changes no firing/damage/heat/ammo/timing.
// Gate-OFF: no-op -> byte-identical.
// -------------------------------------------------------------------------
void dumpPresentationProofIfEnabled() {
    if (!mc2UnitProfileDataEnabled())
        return;
    if (!MasterComponent::masterList)
        return;

    // Find the ONE target weapon (core:ppc) by NAME among loaded components.
    long targetOrd = -1;
    for (long i = 0; i < MasterComponent::numComponents; ++i) {
        if (isTargetWeaponMasterId(i)) { targetOrd = i; break; }
    }
    if (targetOrd < 0) {
        printf("[EQUIP-PRESENTATION-PROOF] %s not found in loaded masterList (no proof)\n",
               kTargetWeaponQualifiedId);
        return;
    }

    // The legacy asset id the fire path would use for this weapon.
    int legacyEffectType = (int)MasterComponent::masterList[targetOrd].getWeaponSpecialEffect();

    WeaponPresentationProfile p = targetWeaponPresentation(legacyEffectType);
    bool fromData = false;
    int resolved = resolveWeaponFireEffectType(p, legacyEffectType, fromData);

    printf("[EQUIP-PRESENTATION-PROOF] %s OnMuzzleFlash vfx resolved from data = %d (source=%s) [legacy=%d]\n",
           kTargetWeaponQualifiedId, resolved,
           fromData ? "DATA" : "LEGACY-FALLBACK", legacyEffectType);
}
