#ifndef UNIT_DEFINITION_H
#define UNIT_DEFINITION_H
// SINGLE-SOURCE-UNIT-DEFINITION-1 (Phase 2, slice 7 of the UNIT-PROFILE arc).
//
// A UnitDefinition is the thin, non-owning FACADE the roadmap's north-star names
// (roadmap "North-star shape"): the modder-facing package that *references* the
// facets, NOT a new parallel data blob. It aggregates a unit's Identity + the
// existing facets already living on `UnitProfile` (baselineCapabilities /
// locomotion / damageSchema / ui) behind ONE accessor object, so every reader
// (logistics/mechlab UI AND mission/sim) resolves facet values from the SAME
// place instead of each re-deriving them (the "dual-reader divergence" the recon
// PRESENTATION-LOADOUT-STRANGLER-RECON-1.md flagged, esp. UIProfile).
//
// PURE + HEADER-ONLY (a struct of const refs/ids + a couple of pure resolution
// helpers, no engine/GL/IO drag) so it compiles into the offline `mc2_tests`
// target exactly like ui_profile.h / hardpoint.h / damage_schema.h, and the
// aggregation/resolution logic is doctest-covered in isolation.
//
// EXPLICITLY OUT OF SCOPE (Phase 3, LOGISTICS-MISSION-SINGLE-READER-1): this
// slice does NOT unify the CSV double-parse. `LogisticsChassis`/`LogisticsVariant`
// (UI reader) and `BattleMech`/`Mover` (sim reader) still each parse the chassis
// CSV. This slice makes the FACADE the single FACET source: facet READS route
// through the facade + the shared resolution helpers below, so the two parsers
// can no longer diverge on facet values even while both still run.

#include "unitprofile.h"         // UnitProfile (owns the facets this facade views)
#include "capability.h"          // CapabilitySet, Capability, UnitAction
#include "locomotion_facet.h"    // LocomotionModel
#include "damage_schema.h"       // DamageSchemaId
#include "ui_profile.h"          // UIProfile + chooseUIAsset/chooseUIStringId

// ---------------------------------------------------------------------------
// mergeUIProfile -- the SINGLE UI facet resolution point (the slice-6 split fix).
// ---------------------------------------------------------------------------
// Slice 6 left the UI facet resolved in TWO independent places that could
// diverge:
//   * logistics side (LogisticsChassis::init) resolved the additive-merge
//     REGISTRY (uiProfileForChassis) into its own uiIconOverride[]/id fields, and
//   * mover/sim side (populateProfileFromFit) populated UnitProfile.ui from the
//     per-unit `.fit [UIProfile]` block ONLY -- it never consulted the registry.
// So a chassis with a registry entry was visible to the mechlab but INVISIBLE to
// the sim-side facet: two UI resolutions, one unit.
//
// This is the one deterministic merge BOTH readers now use. Two optional sources:
//   base  = the per-unit inline `.fit [UIProfile]` block (nearest to the unit)
//   over  = the additive-merge registry entry (the shared data source)
// A declared (non-empty / set) field wins; the registry (`over`) takes precedence
// over the inline base for any field it declares, matching the additive-merge
// override-registry semantics the recon sanctioned (registry is the merge layer
// authored on top of stock data). Every unset field falls through to the other
// source, and an all-unset result means "use legacy accessors" downstream. PURE:
// no engine state, no atlas math -- only which declared choice wins.
inline UIProfile mergeUIProfile(const UIProfile* base, const UIProfile* over) {
    UIProfile out = base ? *base : UIProfile();
    if (over) {
        if (!over->icon.empty())       out.icon       = over->icon;
        if (!over->portrait.empty())   out.portrait   = over->portrait;
        if (!over->minimap.empty())    out.minimap    = over->minimap;
        if (!over->silhouette.empty()) out.silhouette = over->silhouette;
        if (over->nameStringId != UI_STRING_ID_UNSET)
            out.nameStringId = over->nameStringId;
        if (over->encyclopediaStringId != UI_STRING_ID_UNSET)
            out.encyclopediaStringId = over->encyclopediaStringId;
    }
    return out;
}

// ---------------------------------------------------------------------------
// UnitDefinition -- the non-owning facet aggregator/accessor.
// ---------------------------------------------------------------------------
// Holds a const reference to the type's UnitProfile (which already owns the
// facets) + the unit identity label. It is a VIEW: it never copies or re-stores
// facet data, so there is exactly one source of truth (the UnitProfile on the
// ObjectType). Both readers construct a UnitDefinition over the SAME UnitProfile
// and read facets through `.ui()` / `.capabilities()` / `.locomotion()` /
// `.damageSchema()` -- making the facade authoritative for both.
class UnitDefinition {
    const char*        identity_;   // human/modder label (the .fit filename); never owned
    const UnitProfile& profile_;    // the facet owner on the ObjectType; never owned
  public:
    UnitDefinition(const char* identity, const UnitProfile& profile)
        : identity_(identity ? identity : ""), profile_(profile) {}

    // Identity join label (Phase 3 formalizes namespaced ids; today = .fit name).
    const char* identity() const { return identity_; }

    // Facet accessors -- authoritative for BOTH the logistics and sim readers.
    const UIProfile&     ui()           const { return profile_.ui; }
    const CapabilitySet& capabilities() const { return profile_.baselineCapabilities; }
    LocomotionModel      locomotion()   const { return profile_.locomotion; }
    DamageSchemaId       damageSchema() const { return profile_.damageSchema; }

    // Convenience: resolve a UI asset filename through the single ui() facet +
    // the shared chooseUIAsset decision. `legacy` is the accessor's own legacy
    // slot value (iconFileNames[]/atlas). Keeps the "declared-else-legacy" verdict
    // in ONE place so logistics and sim never diverge on it.
    const char* iconAsset(const char* legacy)       const { return chooseUIAsset(ui().icon, legacy); }
    const char* portraitAsset(const char* legacy)   const { return chooseUIAsset(ui().portrait, legacy); }
    const char* minimapAsset(const char* legacy)    const { return chooseUIAsset(ui().minimap, legacy); }
    const char* silhouetteAsset(const char* legacy) const { return chooseUIAsset(ui().silhouette, legacy); }

    long nameStringId(long legacy)         const { return chooseUIStringId(ui().nameStringId, legacy); }
    long encyclopediaStringId(long legacy) const { return chooseUIStringId(ui().encyclopediaStringId, legacy); }
};

#endif // UNIT_DEFINITION_H
