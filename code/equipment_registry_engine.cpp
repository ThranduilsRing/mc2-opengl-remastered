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
#include <cstring>

#include "modder_finding.h"   // MODDER-FIRST-ERROR-SURFACE-1: shared context format
#include "platform_str.h"     // S_stricmp (EQUIP-ADD-FILL-1 def-leaf filter)
#include "logisticsscreen.h"  // EQUIP-ADD-UI-NAME-1 (S3): gEquipmentUiTextHook
#include "mc2fmt.h"

namespace {
// One consistent emit for generated-equipment problems. Only reachable when
// MC2_EQUIPMENT_DEFINITION_SOURCE=generated (caller gates first) -> stock
// gate-OFF runs never print.
void reportEquipSourceFinding(const ModderFinding& f) {
    mc2::fmt::print_out("[EQUIP-SOURCE=generated] {}\n", formatModderFinding(f).c_str());
}
} // namespace

// Process-global registry (parallel index; queried by future slices, not by the
// runtime firing/loadout path). Empty until buildEquipmentRegistryFromMasterList
// runs under the gate.
static EquipmentRegistry g_equipmentRegistry;

// EQUIP-ADD-FILL-1 (S2): rows the gated ADD pass materialized. The registry
// rebuild consults the map so a filled row keeps its NAMESPACED mod id (e.g.
// sample:hot_medium_laser) instead of deriving core:<slug> from its name; the
// vector feeds the SECOND consumer (LogisticsData::initComponents append).
static std::vector<EquipmentAddFillRow>  g_addFillRows;
static std::map<long, std::string>       g_addFillQualifiedIds;

const std::vector<EquipmentAddFillRow>& equipmentAddFillRows() {
    return g_addFillRows;
}

// EQUIP-ADD-UI-NAME-1 (S3): resolve a legacy component string-table id
// (32000/33000/34000/46000 + masterID) to the ADD def's DisplayName/Description
// when that masterID is an ADD-filled row. nullptr -> caller uses cLoadString
// unchanged. Gates OFF -> g_addFillRows empty -> always nullptr (byte-identical).
const char* equipmentAddUiTextForStringId(long stringId) {
    return equipmentAddUiTextForStringIdPure(g_addFillRows, stringId);
}

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

        // A default-off / placeholder row is one initEXCEL failed to parse
        // (undefined compbas rows) -- masterID sentinel -1 + valid-form check
        // via the ONE shared populated test. Do NOT test the name: it comes
        // from the string table and is non-empty for undefined rows on installs
        // with a full string table (2026-07-05 live-witness divergence).
        long   ord     = mc.getMasterID();
        const char* nm = mc.getName();
        bool   enabled = mc2ComponentRowPopulated(ord, (int)mc.getForm());

        // legacyMasterId is the ROW ORDINAL (the 8-bit addressable id), which is
        // the loop index i for populated rows. For placeholder rows getMasterID()
        // is -1; use the row index i as the ordinal alias so every slot is still
        // addressable, and mark it disabled.
        long legacyMasterId = i;

        EquipmentKind kind = componentFormToKind((int)mc.getForm());
        std::string qid;
        // EQUIP-ADD-FILL-1: an ADD-filled row keeps its namespaced mod id.
        auto addIt = g_addFillQualifiedIds.find(legacyMasterId);
        if (addIt != g_addFillQualifiedIds.end())
            qid = addIt->second;
        else
            qid = coreQualifiedIdFor(nm, legacyMasterId);

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

    mc2::fmt::print_out("[EQUIPMENT-REGISTRY-FACADE-1] populated: entries={:d}  withLegacyMasterId={:d}  enabled={:d}  defaultOff={:d}\n", mc2::fmt::as_size(n), mc2::fmt::as_size(reg.withLegacyMasterIdCount()), mc2::fmt::as_size(reg.enabledCount()), mc2::fmt::as_size(reg.disabledCount()));

    // A few sample ordinal->core:id pairs (first enabled entries) so we can
    // eyeball that ids match the converter's core:<slug> output.
    int shown = 0;
    for (const auto& e : reg.entries()) {
        if (!e.enabled) continue;
        mc2::fmt::print_out("[EQUIPMENT-REGISTRY-FACADE-1]   sample {:d} -> {}\n", mc2::fmt::as_i32(e.legacyMasterId), e.qualifiedId.c_str());
        if (++shown >= 8) break;
    }
    if (shown == 0)
        mc2::fmt::print_out("[EQUIPMENT-REGISTRY-FACADE-1]   (no enabled entries -- masterList not loaded?)\n");
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
        if (!mc2ComponentRowPopulated(ord, (int)mc.getForm()))   // placeholder row
            continue;
        // Only weapon rows carry the green weapon stats we overlay.
        if (componentFormToKind((int)mc.getForm()) != EQ_WEAPON)
            continue;

        // core:<slug>; sidecar dir is core/<slug>/equipmentdef.fit (slug == id
        // after the "core:" prefix, mirroring the converter's write path).
        std::string qid  = coreQualifiedIdFor(nm, ord);
        std::string slug = qid.substr(qid.find(':') + 1);

        char path[256];
        mc2::fmt::to_buf(path, "{}{}/equipmentdef.fit", mc2::fmt::safe(kGeneratedEquipmentRoot), slug.c_str());
        if (!fileExists(path))               // no generated def -> keep legacy
            continue;

        FitIniFile gen;
        if (gen.open(path) != NO_ERR) {
            // MODDER-FIRST-ERROR-SURFACE-1: the def EXISTS but is unreadable —
            // a silent `continue` here hid a broken mod/generated file.
            ModderFinding f;
            f.message  = "generated equipment def exists but could not be opened/parsed";
            f.source   = "generated"; f.file = path; f.id = qid;
            f.fallback = "legacy weapon stats for this row";
            reportEquipSourceFinding(f);
            continue;
        }

        // Key the overlay on legacyMasterId: require the def's LegacyMasterId to
        // equal this row's ordinal before touching any stat.
        long genLegacyId = -1;
        if (gen.seekBlock("Equipment") != NO_ERR ||
            gen.readIdLong("LegacyMasterId", genLegacyId) != NO_ERR) {
            ModderFinding f;
            f.message  = "generated equipment def is missing [Equipment] LegacyMasterId";
            f.source   = "generated"; f.file = path; f.id = qid;
            f.field    = "LegacyMasterId";
            f.expected = "[Equipment] block with l LegacyMasterId";
            f.fallback = "legacy weapon stats for this row";
            reportEquipSourceFinding(f);
            continue;
        }
        if (genLegacyId != ord) {
            // The overlay key mismatched: the def claims a DIFFERENT legacy row.
            char expectedBuf[16], actualBuf[16];
            mc2::fmt::to_buf_n(expectedBuf, sizeof(expectedBuf), "{:d}", mc2::fmt::as_i32(ord));
            mc2::fmt::to_buf_n(actualBuf, sizeof(actualBuf), "{:d}", mc2::fmt::as_i32(genLegacyId));
            ModderFinding f;
            f.message  = "generated equipment def LegacyMasterId does not match this compbas row";
            f.source   = "generated"; f.file = path; f.id = qid;
            f.field    = "LegacyMasterId";
            f.expected = expectedBuf;
            f.actual   = actualBuf;
            f.fallback = "legacy weapon stats for this row (overlay skipped)";
            reportEquipSourceFinding(f);
            continue;
        }

        GeneratedWeaponStats gs;
        if (!readGeneratedWeaponStats(gen, gs)) {
            ModderFinding f;
            f.message  = "generated equipment def carries no overlayable weapon stat fields";
            f.source   = "generated"; f.file = path; f.id = qid;
            f.expected = "[WeaponCombatStats] Damage/Heat/Recycle/RangeBucket/CombatValue or [WeaponAmmo] AmmoMasterId";
            f.fallback = "legacy weapon stats for this row";
            reportEquipSourceFinding(f);
            continue;
        }

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
            mc2::fmt::print_out("[EQUIP-SOURCE=generated] core:{} masterID={:d} damage={:.1f} heat={:.1f} recycle={:.2f} range={:d} cv={:.1f} ammo={:d} from generated (match-legacy={})\n", slug.c_str(), mc2::fmt::as_i32(ord), mc.getWeaponDamage(), mc.getWeaponHeat(), mc.getWeaponRecycleTime(), mc2::fmt::as_i32(mc.getWeaponRange()), mc.getCV(), mc2::fmt::as_i32((long)mc.getWeaponAmmoMasterId()), mc2::fmt::safe(match ? "YES" : "NO"));
            ++logged;
        }
    }

    mc2::fmt::print_out("[EQUIP-SOURCE=generated] overlay complete: rows_overlaid={:d} (rows with no generated def kept legacy)\n", mc2::fmt::as_size(overlaid));
    return overlaid;
}

// ---------------------------------------------------------------------------
// EQUIP-ENABLE-WIRE-1 (equipment arc, ADD-ladder S1) -- FIRST consumer of the
// registry `enabled` flag: the enabled->purchase wire.
//
// Called at the availability seam in LogisticsData::updateAvailability, AFTER
// the mission purchase.fit populated available[]. For each POPULATED masterList
// row (registry entry with enabled=true, i.e. the load loop filled it) whose
// generated/mod def sidecar (data/defs/equipment/core/<slug>/equipmentdef.fit,
// served at mod precedence by the g_modIndex file-shadow) declares
// `[Equipment] b Enabled=TRUE` with a matching LegacyMasterId, FORCE
// available[ordinal]=true.
//
// ENABLE-ONLY (never disables): purchase.fit stays the authoritative gate; this
// is a strict union on top. Stock generated defs carry NO Enabled key (the
// converter emits it only when explicitly true), so zero mods = zero forces =
// stock availability reproduced exactly. Gate: MC2_UNIT_PROFILE_DATA=1
// (mc2UnitProfileDataEnabled); gate-OFF returns 0 with zero file I/O ->
// byte-identical. The registry is lazily (re)built here if empty (logistics can
// run before the first mission load builds it at the mission.cpp seam).
//
// The pure force decision (enabled x legacyMasterId-range x row-populated) is
// equipmentEnableWireShouldForce() in equipment_registry.h (doctest-covered).
// Returns the number of ordinals newly forced available.
size_t applyEquipmentEnableWireIfEnabled(bool* available, long availableCount) {
    if (!mc2UnitProfileDataEnabled())     // gate OFF (default) -> no-op
        return 0;
    if (!available || availableCount <= 0)
        return 0;
    if (!MasterComponent::masterList)     // no component table yet
        return 0;

    if (g_equipmentRegistry.count() == 0)
        buildEquipmentRegistryFromMasterList();

    size_t forced = 0;
    for (const EquipmentRegistryEntry& e : g_equipmentRegistry.entries()) {
        // e.enabled == "this masterList row is POPULATED" (the registry's
        // enabled flag). Unpopulated rows are S2 EQUIP-ADD-FILL-1 territory.
        if (!e.enabled)
            continue;
        if (e.legacyMasterId < 0 || e.legacyMasterId >= availableCount)
            continue;

        // Derive the def sidecar path, NAMESPACE-aware (an S2 ADD-filled row's
        // registry id is its namespaced mod id, so its def lives under
        // data/defs/equipment/<ns>/<slug>/ -- core rows resolve as before).
        std::string pathStr = equipmentDefPathForQualifiedId(e.qualifiedId);
        const char* path = pathStr.c_str();
        if (!fileExists((char*)path))     // no def -> purchase.fit verdict stands
            continue;

        FitIniFile gen;
        if (gen.open((char*)path) != NO_ERR)
            continue;                     // unreadable def: overlay pass reports it
        if (gen.seekBlock("Equipment") != NO_ERR)
            continue;

        bool defEnabled = false;
        if (gen.readIdBoolean("Enabled", defEnabled) != NO_ERR)
            continue;                     // absent Enabled key (all stock defs) -> no-op

        // Key check: the def must claim THIS row's ordinal (same rule as the
        // stat overlay) -- a mismatch is a modder error, surfaced not silent.
        long genLegacyId = -1;
        if (gen.readIdLong("LegacyMasterId", genLegacyId) != NO_ERR ||
            genLegacyId != e.legacyMasterId) {
            char expectedBuf[16], actualBuf[16];
            mc2::fmt::to_buf_n(expectedBuf, sizeof(expectedBuf), "{:d}", mc2::fmt::as_i32(e.legacyMasterId));
            mc2::fmt::to_buf_n(actualBuf, sizeof(actualBuf), "{:d}", mc2::fmt::as_i32(genLegacyId));
            ModderFinding f;
            f.message  = "equipment def declares Enabled but its LegacyMasterId does not match this compbas row";
            f.source   = "enable-wire"; f.file = path; f.id = e.qualifiedId;
            f.field    = "LegacyMasterId";
            f.expected = expectedBuf;
            f.actual   = actualBuf;
            f.fallback = "purchase availability unchanged for this row";
            mc2::fmt::print_out("[EQUIP-ENABLE-WIRE-1] {}\n", formatModderFinding(f).c_str());
            continue;
        }

        if (!equipmentEnableWireShouldForce(defEnabled, e.legacyMasterId,
                                            /*rowPopulated=*/e.enabled))
            continue;

        if (!available[e.legacyMasterId]) {
            available[e.legacyMasterId] = true;
            ++forced;
            mc2::fmt::print_out("[EQUIP-ENABLE-WIRE-1] enabled {} (masterID={:d}) for purchase (def {})\n", e.qualifiedId.c_str(), mc2::fmt::as_i32(e.legacyMasterId), mc2::fmt::safe(path));
        }
    }

    if (forced > 0)
        mc2::fmt::print_out("[EQUIP-ENABLE-WIRE-1] enable wire forced {:d} component(s) available\n", mc2::fmt::as_size(forced));
    return forced;
}

// ---------------------------------------------------------------------------
// EQUIP-ADD-FILL-1 (equipment arc, ADD-ladder S2) -- new equipment on a FREE
// (undefined) compbas ordinal.
//
// Called right after loadMasterList (+ the S1 overlay), BEFORE the registry
// rebuild, at both seams (mission.cpp / saveload.cpp). Enumerates the ACTIVE
// MOD's equipment defs (g_modIndex, prefix data/defs/equipment/); each def with
// an EXPLICIT LegacyMasterId naming an UNDEFINED masterList row is materialized
// as a live MasterComponent by synthesizing ONE compbas-format CSV line and
// running it through the SAME initEXCEL parser stock rows use -- so form-
// specific stat routing (weapon/ammo/sensor/...) is legacy-exact by
// construction. The same line later feeds LogisticsComponent::init (the second
// consumer) via equipmentAddFillRows(); column-order equality of the two
// parsers is locked by doctest.
//
// Conflicts (first-wins, never silent): duplicate ordinal claim -> finding;
// def targeting a POPULATED stock ordinal that is not the row's own core def ->
// finding, no fill. NO auto-assign: a def without LegacyMasterId is a finding.
// Gate: MC2_EQUIPMENT_ADD + MC2_UNIT_PROFILE_DATA, both default OFF; gate-OFF
// (or no active mod) -> zero I/O, byte-identical. Returns rows filled.
namespace {
void collectEquipDefPath(const char* absPath, void* userData) {
    // Only the def leaf files; the mod index prefix walk may also see other
    // .fit files under data/defs/equipment/ in the future.
    const char* base = absPath;
    for (const char* p = absPath; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    if (S_stricmp(base, "equipmentdef.fit") != 0) return;
    static_cast<std::vector<std::string>*>(userData)->push_back(absPath);
}

void reportAddFinding(const ModderFinding& f) {
    mc2::fmt::print_out("[EQUIP-ADD] {}\n", formatModderFinding(f).c_str());
}
} // namespace

size_t applyEquipmentAddFillIfEnabled(float baseSensorRange) {
    if (!mc2UnitProfileDataEnabled() || !mc2EquipmentAddEnabled())
        return 0;                          // both gates default OFF -> no-op
    if (!MasterComponent::masterList)
        return 0;

    g_addFillRows.clear();
    g_addFillQualifiedIds.clear();

    // EQUIP-ADD-UI-NAME-1 (S3): install the gui-layer help-text resolver hook.
    // Gate-guarded (we are past both gate checks); rows-empty lookups return
    // null so installing before the fill walk is harmless.
    gEquipmentUiTextHook = equipmentAddUiTextForStringId;

    std::vector<std::string> defPaths;
    EnumerateModFitFiles("data/defs/equipment/", collectEquipDefPath, &defPaths);
    if (defPaths.empty())
        return 0;                          // no active mod defs -> nothing to add

    size_t filled = 0;
    for (const std::string& defPath : defPaths) {
        FitIniFile gen;
        if (gen.open((char*)defPath.c_str()) != NO_ERR) {
            ModderFinding f;
            f.message  = "equipment def could not be opened/parsed";
            f.source   = "add-fill"; f.file = defPath;
            f.fallback = "def ignored (no row filled)";
            reportAddFinding(f);
            continue;
        }
        if (gen.seekBlock("Equipment") != NO_ERR)
            continue;                      // not an equipment def shape

        char idBuf[128] = {0}, nameBuf[128] = {0}, kindBuf[32] = {0};
        char descBuf[1024] = {0};   // S3: optional UI description passthrough
        gen.readIdString("EquipmentId", idBuf, sizeof(idBuf) - 1);
        gen.readIdString("DisplayName", nameBuf, sizeof(nameBuf) - 1);
        gen.readIdString("Kind", kindBuf, sizeof(kindBuf) - 1);
        gen.readIdString("Description", descBuf, sizeof(descBuf) - 1);
        std::string qid = idBuf;

        long ord = -1;
        bool hasOrdinal = (gen.readIdLong("LegacyMasterId", ord) == NO_ERR);

        // Row state for the decision. Populated = initEXCEL fully parsed the
        // row (masterID>=0 + VALID form) -- NOT a name test: the name comes
        // from the string table and is non-empty for undefined rows on installs
        // with a full string table (2026-07-05 live-witness divergence: every
        // free ordinal was refused EQADD_SKIP_POPULATED).
        bool rowPopulated = false, selfOverride = false, alreadyClaimed = false;
        if (hasOrdinal && ord >= 0 && ord < MasterComponent::numComponents) {
            MasterComponent& row = MasterComponent::masterList[ord];
            const char* rn = row.getName();
            rowPopulated = mc2ComponentRowPopulated(row.getMasterID(),
                                                    (int)row.getForm());
            if (rowPopulated)
                selfOverride = (qid == coreQualifiedIdFor(rn, ord));
            alreadyClaimed = (g_addFillQualifiedIds.find(ord)
                              != g_addFillQualifiedIds.end());
        }

        EquipmentAddFillDecision d = equipmentAddFillDecide(
            hasOrdinal, ord, MasterComponent::numComponents,
            rowPopulated, selfOverride, alreadyClaimed);

        if (d != EQADD_FILL) {
            if (d == EQADD_SKIP_SELF_OVERRIDE)
                continue;                  // the row's own core def (overlay/enable land)
            ModderFinding f;
            f.source = "add-fill"; f.file = defPath; f.id = qid;
            f.field  = "LegacyMasterId";
            char ordBuf[16];
            mc2::fmt::to_buf(ordBuf, "{:d}", mc2::fmt::as_i32(ord));
            switch (d) {
                case EQADD_SKIP_NO_ORDINAL:
                    f.message  = "ADD def carries no explicit LegacyMasterId (no auto-assign; ids must be explicit)";
                    f.expected = "l LegacyMasterId=<free ordinal 0..254>";
                    break;
                case EQADD_SKIP_ORDINAL_RANGE:
                    f.message  = "ADD def LegacyMasterId outside the 8-bit addressable space (255 = 0xff sentinel)";
                    f.expected = "0..254 and < numComponents";
                    f.actual   = ordBuf;
                    break;
                case EQADD_SKIP_POPULATED:
                    f.message  = "ADD def targets a POPULATED stock ordinal (not an override of that row)";
                    f.expected = "an UNDEFINED (free) compbas ordinal";
                    f.actual   = ordBuf;
                    break;
                case EQADD_SKIP_DUPLICATE:
                    f.message  = "ordinal already claimed by an earlier ADD def (first-wins)";
                    f.actual   = ordBuf;
                    break;
                default: break;
            }
            f.fallback = "row not filled";
            reportAddFinding(f);
            continue;
        }

        // --- Gather the row fields from the def facets. ---
        EquipmentAddRowFields rf;
        rf.ordinal     = ord;
        rf.displayName = nameBuf[0] ? nameBuf : qid;

        char catBuf[32] = {0};
        if (gen.seekBlock("WeaponFacet") == NO_ERR)
            gen.readIdString("Category", catBuf, sizeof(catBuf) - 1);
        rf.formOrdinal = equipmentAddKindToFormOrdinal(kindBuf, catBuf);

        float fv; long lv;
        if (gen.seekBlock("Equipment") == NO_ERR &&
            gen.readIdFloat("Cost", fv) == NO_ERR)
            rf.cost = (long)fv;

        if (gen.seekBlock("WeaponCombatStats") == NO_ERR) {
            if (gen.readIdFloat("Damage", fv) == NO_ERR)       rf.damage = fv;
            if (gen.readIdFloat("Heat", fv) == NO_ERR)         rf.heat = fv;
            if (gen.readIdFloat("Recycle", fv) == NO_ERR)      rf.recycle = fv;
            if (gen.readIdFloat("CombatValue", fv) == NO_ERR)  rf.combatValue = fv;
            char bucket[32] = {0};
            if (gen.readIdString("RangeBucket", bucket, sizeof(bucket) - 1) == NO_ERR)
                rf.rangeOrdinal = mc2GeneratedRangeBucketToOrdinal(bucket, -1);
        }
        if (gen.seekBlock("WeaponAmmo") == NO_ERR &&
            gen.readIdLong("AmmoMasterId", lv) == NO_ERR)
            rf.ammoMasterId = lv;
        if (gen.seekBlock("WeaponFit") == NO_ERR) {
            if (gen.readIdLong("CritSlots", lv) == NO_ERR)  rf.critSlots = (int)lv;
            if (gen.readIdFloat("Tonnage", fv) == NO_ERR)   rf.tonnage = fv;
            char locs[256] = {0};
            if (gen.readIdString("AllowedLocations", locs, sizeof(locs) - 1) == NO_ERR)
                equipmentAddAllowedLocationsToCrit(locs, rf.crit);
        }
        if (gen.seekBlock("Presentation") == NO_ERR) {
            char icon[128] = {0}, pic[128] = {0};
            if (gen.readIdString("Icon", icon, sizeof(icon) - 1) == NO_ERR)
                rf.iconFile = icon;
            if (gen.readIdString("Picture", pic, sizeof(pic) - 1) == NO_ERR)
                rf.pictureFile = pic;
            if (gen.readIdLong("UINameStringId", lv) == NO_ERR) rf.stringId = lv;
            if (gen.readIdLong("SpecialEffect", lv) == NO_ERR)  rf.specialEffect = (int)lv;
            if (gen.readIdLong("IconX", lv) == NO_ERR)          rf.iconX = (int)lv;
            if (gen.readIdLong("IconY", lv) == NO_ERR)          rf.iconY = (int)lv;
        }
        rf.helpStringId = 34000 + ord;      // legacy help-string slot for the ordinal

        // Two-consumer trap: the logistics consumer SKIPS a row whose icon
        // column is empty/'0' -> the item would fill in-sim but vanish from the
        // mechlab. Refuse the half-materialized state; surface it instead.
        if (rf.iconFile.empty() || rf.iconFile == "0") {
            ModderFinding f;
            f.message  = "ADD def has no [Presentation] Icon (mechlab consumer drops icon-less rows)";
            f.source   = "add-fill"; f.file = defPath; f.id = qid;
            f.field    = "Icon";
            f.expected = "st Icon=\"<file>.tga\"";
            f.fallback = "row not filled";
            reportAddFinding(f);
            continue;
        }

        // --- Fill consumer 1: MasterComponent via the STOCK initEXCEL parser. ---
        std::string csv = buildEquipmentAddCsvLine(rf);
        char lineBuf[1024];
        strncpy(lineBuf, csv.c_str(), sizeof(lineBuf) - 1);
        lineBuf[sizeof(lineBuf) - 1] = '\0';

        MasterComponent& row = MasterComponent::masterList[ord];
        if (row.initEXCEL(lineBuf, baseSensorRange) != NO_ERR) {
            ModderFinding f;
            f.message  = "synthesized compbas line rejected by initEXCEL";
            f.source   = "add-fill"; f.file = defPath; f.id = qid;
            f.actual   = csv;
            f.fallback = "row not filled";
            reportAddFinding(f);
            row.init();                    // restore the undefined-placeholder state
            continue;
        }
        // initEXCEL sourced name/abbrev from the string table (garbage for a new
        // id) and never sets uses/techBase on the CSV path -- fix all four.
        row.setName(rf.displayName.c_str());
        row.setAbbreviation(rf.displayName.c_str());
        row.clearUseFlags();
        row.setCanMechUse();
        row.setCanVehicleUse();
        row.setBothTechBase();

        EquipmentAddFillRow out;
        out.ordinal      = ord;
        out.qualifiedId  = qid;
        out.displayName  = rf.displayName;
        out.description  = descBuf;    // may be empty -> UI falls back to name
        out.csvLine      = csv;
        out.ammoMasterId = rf.ammoMasterId;
        g_addFillRows.push_back(out);
        g_addFillQualifiedIds[ord] = qid;
        ++filled;

        mc2::fmt::print_out("[EQUIP-ADD] filled ordinal={:d} id={} name=\"{}\" form={:d} damage={:.2f} heat={:.2f} recycle={:.2f} range={:d} cv={:.2f} ammo={:d} (def {})\n", mc2::fmt::as_i32(ord), qid.c_str(), rf.displayName.c_str(), mc2::fmt::as_i32(rf.formOrdinal), rf.damage, rf.heat, rf.recycle, mc2::fmt::as_i32(rf.rangeOrdinal), rf.combatValue, mc2::fmt::as_i32(rf.ammoMasterId), defPath.c_str());
    }

    if (filled > 0)
        mc2::fmt::print_out("[EQUIP-ADD] add-fill complete: rows_filled={:d} (of {:d} def(s) scanned)\n", mc2::fmt::as_size(filled), mc2::fmt::as_size(defPaths.size()));
    return filled;
}
