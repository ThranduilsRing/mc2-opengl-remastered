// EQUIPMENT-REGISTRY-FACADE-1 (equipment arc, engine slice E2) -- engine side.
//
// Populates a process-global EquipmentRegistry by walking the ALREADY-LOADED
// MasterComponent::masterList[0..numComponents), one registry entry per row.
// PURELY ADDITIVE: reads masterList; mutates nothing in it; touches no
// InventoryItem / save / firing / loadout path. Built + dumped only when the
// MC2_UNIT_PROFILE_DATA gate is ON, so gate-OFF is byte-identical.
//
// The pure registry + id/kind derivation live in equipment_registry.h (offline
// doctest-tested in mc2_tests). This TU is the ONLY place that reads the engine
// MasterComponent type.

#include "equipment_registry.h"
#include "unitprofile.h"           // mc2UnitProfileDataEnabled()
#include "cmponent.h"              // MasterComponent + masterList
#include "inifile.h"               // FitIniFile (EQUIPMENT-SOURCE-FLIP-SWITCH-1 overlay)
#include "file.h"                  // fileExists

#include <cstdio>

// Process-global registry (parallel index; queried by future slices, not by the
// runtime firing/loadout path). Empty until buildEquipmentRegistryFromMasterList
// runs under the gate.
static EquipmentRegistry g_equipmentRegistry;

const EquipmentRegistry& getEquipmentRegistry() {
    return g_equipmentRegistry;
}

// Walk masterList and (re)build the registry. Additive: reads only.
// Returns the number of entries added.
size_t buildEquipmentRegistryFromMasterList() {
    g_equipmentRegistry.clear();

    if (!MasterComponent::masterList)
        return 0;

    for (long i = 0; i < MasterComponent::numComponents; ++i) {
        MasterComponent& mc = MasterComponent::masterList[i];

        // A default-off / placeholder row is one the load loop left empty: the
        // "undefined" compbas rows reset masterID to -1 and never set a name
        // (cmponent.cpp initEXCEL). Everything else is an enabled component.
        long   ord     = mc.getMasterID();
        const char* nm = mc.getName();
        bool   enabled = (ord >= 0) && nm && nm[0] != '\0';

        // legacyMasterId is the ROW ORDINAL (the 8-bit addressable id), which is
        // the loop index i for populated rows. For placeholder rows getMasterID()
        // is -1; use the row index i as the ordinal alias so every slot is still
        // addressable, and mark it disabled.
        long legacyMasterId = i;

        EquipmentKind kind = componentFormToKind((int)mc.getForm());
        std::string qid    = coreQualifiedIdFor(nm, legacyMasterId);

        g_equipmentRegistry.add(qid, legacyMasterId, kind, enabled);
    }

    return g_equipmentRegistry.count();
}

// Gated build + verification dump. Called after loadMasterList. Gate-OFF: no-op
// (registry stays empty, nothing printed) -> byte-identical parity.
void buildAndDumpEquipmentRegistryIfEnabled() {
    if (!mc2UnitProfileDataEnabled())
        return;

    size_t n = buildEquipmentRegistryFromMasterList();
    const EquipmentRegistry& reg = g_equipmentRegistry;

    printf("[EQUIPMENT-REGISTRY-FACADE-1] populated: entries=%zu  withLegacyMasterId=%zu  enabled=%zu  defaultOff=%zu\n",
           n, reg.withLegacyMasterIdCount(), reg.enabledCount(), reg.disabledCount());

    // A few sample ordinal->core:id pairs (first enabled entries) so we can
    // eyeball that ids match the converter's core:<slug> output.
    int shown = 0;
    for (const auto& e : reg.entries()) {
        if (!e.enabled) continue;
        printf("[EQUIPMENT-REGISTRY-FACADE-1]   sample %ld -> %s\n",
               e.legacyMasterId, e.qualifiedId.c_str());
        if (++shown >= 8) break;
    }
    if (shown == 0)
        printf("[EQUIPMENT-REGISTRY-FACADE-1]   (no enabled entries -- masterList not loaded?)\n");
}

// ---------------------------------------------------------------------------
// EQUIPMENT-SOURCE-FLIP-SWITCH-1 -- gated stat OVERLAY (engine-only file I/O).
//
// When MC2_EQUIPMENT_DEFINITION_SOURCE=generated, this runs ONCE right after
// loadMasterList and OVERWRITES only the green weapon stat fields on each matched
// masterList row from the deployed generated EquipmentDefinition sidecars under
// data/defs/equipment/core/<slug>/equipmentdef.fit. It is the ONLY new write path
// and is bounded (one pass over masterList). Firing/loadout/inventory code is
// UNTOUCHED and none of the ~180 masterList read sites change -- they simply read
// the (possibly generated-sourced) stat values as always.
//
// Matching: for each populated row we derive its core:<slug> id EXACTLY as the
// registry/converter do, open that def, and REQUIRE its `l LegacyMasterId` to
// equal the row ordinal before applying -- so the overlay is keyed by
// legacyMasterId, never by slug alone. A row with no generated def, a def whose
// LegacyMasterId mismatches, or a non-weapon def keeps its legacy stats
// (per-field / per-row fallback). Gate-OFF -> function returns immediately with
// zero file I/O -> byte-identical.
//
// The pure gate / range-bucket / value-selection logic lives in
// equipment_registry.h (doctest target); this TU owns only file open + block
// seek + the per-component source-flip log line.
namespace {
const char* kGeneratedEquipmentRoot = "data/defs/equipment/core/";

// Read the green weapon stats present in an already-opened [WeaponCombatStats]/
// [WeaponAmmo] generated def into `out`. Absent fields leave has*=false so the
// caller keeps the legacy value. Assumes the file cursor is fresh (pre-seek).
bool readGeneratedWeaponStats(FitIniFile& gen, GeneratedWeaponStats& out) {
    // WeaponCombatStats block: damage / heat / recycle / range bucket / CV.
    if (gen.seekBlock("WeaponCombatStats") == NO_ERR) {
        float f;
        if (gen.readIdFloat("Damage", f) == NO_ERR)  { out.hasDamage = true;  out.damage = f; }
        if (gen.readIdFloat("Heat", f) == NO_ERR)    { out.hasHeat = true;    out.heat = f; }
        if (gen.readIdFloat("Recycle", f) == NO_ERR) { out.hasRecycle = true; out.recycle = f; }
        char bucket[32] = {0};
        if (gen.readIdString("RangeBucket", bucket, sizeof(bucket)) == NO_ERR && bucket[0]) {
            int ord = mc2GeneratedRangeBucketToOrdinal(bucket, -1);
            if (ord >= 0) { out.hasRangeBucket = true; out.rangeOrdinal = ord; }
        }
        if (gen.readIdFloat("CombatValue", f) == NO_ERR) { out.hasCombatValue = true; out.combatValue = f; }
    }
    // WeaponAmmo block: ammo linkage ordinal.
    if (gen.seekBlock("WeaponAmmo") == NO_ERR) {
        long l;
        if (gen.readIdLong("AmmoMasterId", l) == NO_ERR) { out.hasAmmoMasterId = true; out.ammoMasterId = l; }
    }
    return out.any();
}
} // namespace

// Returns the number of rows whose stats were overlaid from generated data.
size_t overlayGeneratedEquipmentStatsIfEnabled() {
    if (!mc2EquipmentDefinitionSourceIsGenerated())   // gate legacy (default) -> no-op
        return 0;
    if (!MasterComponent::masterList)
        return 0;

    size_t overlaid = 0;
    int    logged   = 0;
    for (long i = 0; i < MasterComponent::numComponents; ++i) {
        MasterComponent& mc = MasterComponent::masterList[i];

        long        ord = mc.getMasterID();
        const char* nm  = mc.getName();
        if (ord < 0 || !nm || !nm[0])        // default-off placeholder row
            continue;
        // Only weapon rows carry the green weapon stats we overlay.
        if (componentFormToKind((int)mc.getForm()) != EQ_WEAPON)
            continue;

        // core:<slug>; sidecar dir is core/<slug>/equipmentdef.fit (slug == id
        // after the "core:" prefix, mirroring the converter's write path).
        std::string qid  = coreQualifiedIdFor(nm, ord);
        std::string slug = qid.substr(qid.find(':') + 1);

        char path[256];
        std::snprintf(path, sizeof(path), "%s%s/equipmentdef.fit",
                      kGeneratedEquipmentRoot, slug.c_str());
        if (!fileExists(path))               // no generated def -> keep legacy
            continue;

        FitIniFile gen;
        if (gen.open(path) != NO_ERR)
            continue;

        // Key the overlay on legacyMasterId: require the def's LegacyMasterId to
        // equal this row's ordinal before touching any stat.
        long genLegacyId = -1;
        if (gen.seekBlock("Equipment") != NO_ERR ||
            gen.readIdLong("LegacyMasterId", genLegacyId) != NO_ERR ||
            genLegacyId != ord)
            continue;

        GeneratedWeaponStats gs;
        if (!readGeneratedWeaponStats(gen, gs))
            continue;

        // Capture legacy values (for the match-legacy log) then OVERWRITE only
        // present green fields. Firing reads these getters unchanged.
        const float legDamage  = mc.getWeaponDamage();
        const float legHeat    = mc.getWeaponHeat();
        const float legRecycle = mc.getWeaponRecycleTime();
        const long  legRange   = mc.getWeaponRange();
        const float legCV      = mc.getCV();
        const long  legAmmo    = (long)mc.getWeaponAmmoMasterId();

        if (gs.hasDamage)       mc.setWeaponDamage(gs.damage);
        if (gs.hasHeat)         mc.setWeaponHeat(gs.heat);
        if (gs.hasRecycle)      mc.setWeaponRecycleTime(gs.recycle);
        if (gs.hasRangeBucket)  mc.setWeaponRange(gs.rangeOrdinal);
        if (gs.hasCombatValue)  mc.setCV(gs.combatValue);
        if (gs.hasAmmoMasterId) mc.setWeaponAmmoMasterId(gs.ammoMasterId);

        ++overlaid;

        // The shadow compare proved generated VALUES == legacy for all 37
        // weapons, so match-legacy is expected YES; compute it honestly.
        const bool match =
            (!gs.hasDamage      || gs.damage       == legDamage)  &&
            (!gs.hasHeat        || gs.heat         == legHeat)    &&
            (!gs.hasRecycle     || gs.recycle      == legRecycle) &&
            (!gs.hasRangeBucket || gs.rangeOrdinal == legRange)   &&
            (!gs.hasCombatValue || gs.combatValue  == legCV)      &&
            (!gs.hasAmmoMasterId|| gs.ammoMasterId == legAmmo);

        if (logged < 12) {
            printf("[EQUIP-SOURCE=generated] core:%s masterID=%ld "
                   "damage=%.1f heat=%.1f recycle=%.2f range=%ld cv=%.1f ammo=%ld "
                   "from generated (match-legacy=%s)\n",
                   slug.c_str(), ord,
                   mc.getWeaponDamage(), mc.getWeaponHeat(), mc.getWeaponRecycleTime(),
                   mc.getWeaponRange(), mc.getCV(), (long)mc.getWeaponAmmoMasterId(),
                   match ? "YES" : "NO");
            ++logged;
        }
    }

    printf("[EQUIP-SOURCE=generated] overlay complete: rows_overlaid=%zu "
           "(rows with no generated def kept legacy)\n", overlaid);
    return overlaid;
}
