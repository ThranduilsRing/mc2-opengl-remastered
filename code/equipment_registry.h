#ifndef EQUIPMENT_REGISTRY_H
#define EQUIPMENT_REGISTRY_H
// EQUIPMENT-REGISTRY-FACADE-1 (equipment arc, engine slice E2).
//
// A runtime, queryable registry that lets the engine load/query equipment by a
// canonical `core:<slug>` STRING id AND by the legacy 8-bit masterID (row
// ordinal), sitting PURELY BESIDE the legacy MasterComponent::masterList array.
// Legacy MasterComponent stays authoritative for all firing/loadout/inventory
// behavior; this is an ADDITIVE parallel lookup layer only.
//
// WHAT IT IS NOT: it does NOT widen masterID, does NOT touch InventoryItem, save
// format, or the ~180 `masterList[..masterID]` index sites (see
// EQUIPMENT-MASTERID-USAGE-RECON-1.md). It maps `qualifiedId <-> legacyMasterId`
// over the SAME array; new entries with legacyMasterId 0..254 stay addressable by
// the unchanged 8-bit path. E4 (the widen) is deferred.
//
// PURITY: this header is GL/IO/engine-FREE. It reuses only qualified_id.h
// (std::string) and equipment_definition.h's EquipmentKind enum. The population
// walk (which reads MasterComponent) lives in equipment_registry_engine.cpp; this
// header is the pure registry + query API + the pure id/kind derivation rules,
// so slug/lookup logic is doctest-testable offline in mc2_tests.
//
// ID DERIVATION MUST MATCH the offline converter
// (tools/unit_converter/equipment_extract.py `_slug` + `core:<slug>`) so registry
// ids line up with generated def ids. slugifyComponentName() below mirrors that
// rule exactly (lowercase, strip apostrophes, non-alnum runs -> '_', strip '_';
// empty -> "comp<id>"; leading-non-alpha -> "c_<s>").

#include "qualified_id.h"          // mc2id::QualifiedId, parseQualifiedId
#include "equipment_definition.h"  // EquipmentKind + component-form mapping target

#include <string>
#include <vector>
#include <map>
#include <cctype>
#include <cstdio>                  // snprintf (EQUIP-ADD-FILL-1 CSV builder)
#include <cstdlib>                 // strtol
#include <cstring>                 // strchr

// -------------------------------------------------------------------------
// Pure id derivation -- MIRROR of tools/unit_converter/equipment_extract.py.
// -------------------------------------------------------------------------
// Slugify a component Name into the `<slug>` portion of `core:<slug>`.
// Deterministic. Lowercase, drop apostrophes, non-[a-z0-9] runs -> '_', strip
// leading/trailing '_'. Names that end up empty or start with a non-letter get a
// stable fallback so the id always parses under the qualified-id charset:
//   empty            -> "comp<masterId>"
//   starts non-alpha -> "c_<s>"
// (Exactly the Python `_slug(name, master_id)` branch order.)
inline std::string slugifyComponentName(const char* name, long masterId) {
    std::string cleaned;
    if (name) {
        for (const char* p = name; *p; ++p) {
            char c = *p;
            if (c == '\'') continue;                 // .replace("'", "")
            cleaned.push_back((char)std::tolower((unsigned char)c));
        }
    }
    // re.sub(r"[^a-z0-9]+", "_", cleaned): collapse non-alnum runs to a single '_'
    std::string s;
    bool prevWasSep = false;
    for (char c : cleaned) {
        bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (alnum) { s.push_back(c); prevWasSep = false; }
        else if (!prevWasSep) { s.push_back('_'); prevWasSep = true; }
    }
    // .strip("_")
    size_t b = s.find_first_not_of('_');
    size_t e = s.find_last_not_of('_');
    if (b == std::string::npos) s.clear();
    else s = s.substr(b, e - b + 1);

    // Fallback branch: empty -> comp<id>; leading non-alpha -> c_<s>.
    bool firstAlpha = !s.empty() && (s[0] >= 'a' && s[0] <= 'z');
    if (s.empty() || !firstAlpha) {
        if (s.empty()) {
            s = std::string("comp") + std::to_string(masterId);
        } else {
            s = std::string("c_") + s;
        }
    }
    return s;
}

// Canonical `core:<slug>` qualified id string for a component name+ordinal.
inline std::string coreQualifiedIdFor(const char* name, long masterId) {
    return std::string("core:") + slugifyComponentName(name, masterId);
}

// -------------------------------------------------------------------------
// Component-form -> EquipmentKind. Mirror of the converter's TYPE_TO_KIND
// grouping, keyed on the engine's ComponentFormType enum ordinals (cmponent.h).
// Kept as raw int so this header does NOT include cmponent.h (engine drag). The
// caller passes MasterComponent::getForm() cast to int; the ordinals are the
// ComponentFormType enum values, which are contractually stable (they index the
// on-disk ComponentFormString table).
// -------------------------------------------------------------------------
enum ComponentFormOrdinal {
    CFORM_SIMPLE = 0,
    CFORM_COCKPIT,
    CFORM_SENSOR,
    CFORM_ACTUATOR,
    CFORM_ENGINE,
    CFORM_HEATSINK,
    CFORM_WEAPON,
    CFORM_WEAPON_ENERGY,
    CFORM_WEAPON_BALLISTIC,
    CFORM_WEAPON_MISSILE,
    CFORM_AMMO,
    CFORM_JUMPJET,
    CFORM_CASE,
    CFORM_LIFESUPPORT,
    CFORM_GYROSCOPE,
    CFORM_POWER_AMPLIFIER,
    CFORM_ECM,
    CFORM_PROBE,
    CFORM_JAMMER,
    CFORM_BULK
};

// -------------------------------------------------------------------------
// EQUIP-ADD-FILL-1 divergence fix: THE populated-row test, shared by the
// registry build and the ADD fill pass (semantically same as the debug-state
// witness's). A masterList slot is POPULATED iff initEXCEL fully parsed it:
// masterID >= 0 AND form is a valid ComponentFormType ordinal.
//
// Why the form check (and why NOT a name check): initEXCEL early-returns -1 on
// an unknown Type token ("undefined" is not in ComponentFormString) WITHOUT
// resetting masterID/name, and name comes from the string table
// (cLoadString(32000+id)) BEFORE that bail -- so on installs with a full
// string table an undefined row used to carry masterID==ordinal + a NON-EMPTY
// name + a garbage form, and any "masterID>=0 && name[0]" test misclassified
// EVERY free ordinal as populated (live witness failure 2026-07-05: all ADD
// defs refused EQADD_SKIP_POPULATED). loadMasterList now restores the sentinel
// on parse failure (cmponent.cpp), and this helper keeps the form-validity
// check as belt-and-braces against any other masterList producer. Conversely a
// name check would let a populated row with a MISSING string-table entry be
// misread as free (clobberable). Pure -> doctest target.
// -------------------------------------------------------------------------
enum { kComponentFormOrdinalCount = 20 };   // == NUM_COMPONENT_FORMS (cmponent.h)

inline bool mc2ComponentRowPopulated(long masterId, int formOrdinal) {
    if (masterId < 0) return false;
    return formOrdinal >= 0 && formOrdinal < kComponentFormOrdinalCount;
}

inline EquipmentKind componentFormToKind(int form) {
    switch (form) {
        case CFORM_WEAPON:
        case CFORM_WEAPON_ENERGY:
        case CFORM_WEAPON_BALLISTIC:
        case CFORM_WEAPON_MISSILE:      return EQ_WEAPON;
        case CFORM_AMMO:                return EQ_AMMO;
        case CFORM_SENSOR:
        case CFORM_PROBE:               return EQ_SENSOR;   // Optics/Probe -> sensor family
        case CFORM_ECM:
        case CFORM_JAMMER:              return EQ_ECM;
        case CFORM_JUMPJET:             return EQ_JUMPJET;
        case CFORM_HEATSINK:            return EQ_HEATSINK;
        case CFORM_BULK:
        case CFORM_CASE:                return EQ_STRUCTURE;
        case CFORM_COCKPIT:
        case CFORM_LIFESUPPORT:         return EQ_COCKPIT;
        case CFORM_ACTUATOR:
        case CFORM_GYROSCOPE:           return EQ_ACTUATOR;  // gyro = chassis actuator-class
        case CFORM_ENGINE:              return EQ_ENGINE;
        case CFORM_SIMPLE:
        case CFORM_POWER_AMPLIFIER:     return EQ_OTHER;
        default:                        return EQ_OTHER;
    }
}

// -------------------------------------------------------------------------
// EQUIPMENT-SOURCE-FLIP-SWITCH-1 -- the equipment-definition SOURCE selector.
//
// SEPARATE gate from MC2_UNIT_PROFILE_DATA / MC2_UNIT_DEFINITION_SOURCE. Selects
// WHERE the green weapon stat facets (damage / heat / recycle / range bucket /
// combat value / ammo linkage) come from when the engine populates
// MasterComponent::masterList:
//   legacy    (DEFAULT) -> stats stay exactly as loadMasterList parsed the
//                          compbas CSV; byte-identical, no overlay pass runs.
//   generated           -> AFTER loadMasterList, a bounded post-load overlay
//                          walks the generated EquipmentDefinition sidecars and
//                          OVERWRITES only those green fields per matched row; a
//                          component with no generated def keeps its legacy value.
// Firing/loadout/inventory code is UNTOUCHED -- it still reads masterList exactly
// as before; only the stat VALUES may be sourced from generated data. The shadow
// compare proved generated values == legacy, so generated is behavior-identical.
//
// `generated` ONLY on the exact token "generated" (case-insensitive); anything
// else (unset / "legacy" / "0" / junk) resolves to legacy. Pure parser is
// testable; the cached accessor reads getenv once.
inline bool mc2_equipment_definition_source_is_generated(const char* e) {
    if (!e || !e[0]) return false;
    const char* want = "generated";
    for (int i = 0; ; ++i) {
        const char a = (char)std::tolower((unsigned char)e[i]);
        const char b = want[i];
        if (b == '\0') return a == '\0';   // matched all of "generated", no junk
        if (a != b)    return false;
    }
}
inline bool mc2EquipmentDefinitionSourceIsGenerated() {
    static const bool s =
        mc2_equipment_definition_source_is_generated(getenv("MC2_EQUIPMENT_DEFINITION_SOURCE"));
    return s;
}

// -------------------------------------------------------------------------
// EQUIPMENT-SOURCE-FLIP-SWITCH-1 -- pure overlay-value logic (doctest target).
//
// The generated sidecar stores the weapon range as a NAMED bucket token
// ("short"/"medium"/"long"); the legacy engine stores it as the WeaponRangeType
// enum ordinal (WEAPON_RANGE_SHORT=0 / MEDIUM=1 / LONG=2, cmponent.h). This maps
// the generated bucket string onto that ordinal. An unrecognized/empty bucket
// yields the caller-supplied legacy fallback so a partial def degrades per-field.
// Kept engine-enum-FREE (raw ints) so this header stays IO/engine-free.
inline int mc2GeneratedRangeBucketToOrdinal(const char* bucket, int legacyFallback) {
    if (!bucket || !bucket[0]) return legacyFallback;
    // case-insensitive compare, no engine string table dependency
    char b[16] = {0};
    unsigned n = 0;
    for (const char* p = bucket; *p && n + 1 < sizeof(b); ++p)
        b[n++] = (char)std::tolower((unsigned char)*p);
    if (std::strcmp(b, "short")  == 0) return 0;   // WEAPON_RANGE_SHORT
    if (std::strcmp(b, "medium") == 0) return 1;   // WEAPON_RANGE_MEDIUM
    if (std::strcmp(b, "long")   == 0) return 2;   // WEAPON_RANGE_LONG
    return legacyFallback;
}

// The set of green stat values sourced from a generated EquipmentDefinition, plus
// a per-field "was this actually present in the def" flag so the caller keeps the
// legacy value for any absent field (per-field fallback). Pure POD; the engine
// overlay fills it from FitIniFile reads and applies present-fields onto the row.
struct GeneratedWeaponStats {
    bool  hasDamage = false;      float damage = 0.0f;
    bool  hasHeat = false;        float heat = 0.0f;
    bool  hasRecycle = false;     float recycle = 0.0f;
    bool  hasRangeBucket = false; int   rangeOrdinal = 0;   // already mapped
    bool  hasCombatValue = false; float combatValue = 0.0f; // CV
    bool  hasAmmoMasterId = false; long ammoMasterId = 0;   // ammo linkage
    // True iff at least one green field was sourced from the generated def.
    bool any() const {
        return hasDamage || hasHeat || hasRecycle || hasRangeBucket ||
               hasCombatValue || hasAmmoMasterId;
    }
};

// -------------------------------------------------------------------------
// Registry entry -- one per component row that the load loop populated.
// -------------------------------------------------------------------------
struct EquipmentRegistryEntry {
    std::string   qualifiedId;    // canonical core:<slug> (the identity string)
    long          legacyMasterId; // row ordinal == 8-bit masterID alias
    EquipmentKind kind;           // from component form
    bool          enabled;        // false = default-off placeholder ("undefined"
                                  // row: masterID stayed -1 / empty name). The
                                  // load loop has NO enabled filter, so this
                                  // surfaces the enabled/default-off state.
    EquipmentRegistryEntry()
        : legacyMasterId(-1), kind(EQ_OTHER), enabled(false) {}
};

// -------------------------------------------------------------------------
// EquipmentRegistry -- lightweight lookup structure.
//   map qualifiedId -> entry index
//   map legacyMasterId -> qualifiedId
// It OWNS no gameplay state; it is a parallel index over masterList that future
// slices (E5 id-alias) query. It is READ under the gate; nothing in the firing/
// loadout/inventory path consults it.
// -------------------------------------------------------------------------
class EquipmentRegistry {
public:
    void clear() {
        entries_.clear();
        byId_.clear();
        byOrdinal_.clear();
    }

    // Add one component row. `enabled=false` for default-off placeholders.
    // First writer per qualifiedId wins (dup ids are a data condition; the
    // ordinal index still maps every ordinal to its own id).
    void add(const std::string& qualifiedId, long legacyMasterId,
             EquipmentKind kind, bool enabled) {
        EquipmentRegistryEntry e;
        e.qualifiedId    = qualifiedId;
        e.legacyMasterId = legacyMasterId;
        e.kind           = kind;
        e.enabled        = enabled;
        size_t idx = entries_.size();
        entries_.push_back(e);
        if (byId_.find(qualifiedId) == byId_.end())
            byId_[qualifiedId] = idx;
        byOrdinal_[legacyMasterId] = qualifiedId;
    }

    // --- Query API ---
    const EquipmentRegistryEntry* byQualifiedId(const std::string& id) const {
        auto it = byId_.find(id);
        return (it == byId_.end()) ? nullptr : &entries_[it->second];
    }
    const EquipmentRegistryEntry* byLegacyMasterId(long ord) const {
        auto it = byOrdinal_.find(ord);
        if (it == byOrdinal_.end()) return nullptr;
        return byQualifiedId(it->second);
    }
    // core:<slug> for an ordinal (empty if none).
    std::string qualifiedIdFor(long ord) const {
        auto it = byOrdinal_.find(ord);
        return (it == byOrdinal_.end()) ? std::string() : it->second;
    }
    // Ordinal for a qualified id (-1 if none).
    long legacyMasterIdFor(const std::string& id) const {
        const EquipmentRegistryEntry* e = byQualifiedId(id);
        return e ? e->legacyMasterId : -1;
    }

    size_t count() const { return entries_.size(); }
    size_t enabledCount() const {
        size_t n = 0;
        for (const auto& e : entries_) if (e.enabled) ++n;
        return n;
    }
    size_t disabledCount() const { return entries_.size() - enabledCount(); }
    // Entries carrying a real legacy alias (ordinal >= 0).
    size_t withLegacyMasterIdCount() const {
        size_t n = 0;
        for (const auto& e : entries_) if (e.legacyMasterId >= 0) ++n;
        return n;
    }

    const std::vector<EquipmentRegistryEntry>& entries() const { return entries_; }

private:
    std::vector<EquipmentRegistryEntry> entries_;
    std::map<std::string, size_t>       byId_;      // qualifiedId -> entry index
    std::map<long, std::string>         byOrdinal_; // legacyMasterId -> qualifiedId
};

// -------------------------------------------------------------------------
// LOADOUT-EQUIPMENT-ID-ALIAS-1 (equipment arc, engine slice E5) -- the pure
// resolution rule for a loadout equipment reference.
//
// A loadout slot may reference equipment either the legacy way (a numeric
// masterID ordinal, e.g. "145") OR by qualified string id (e.g.
// "core:medium_laser"). This helper maps EITHER form to the SAME 8-bit
// addressable masterID space (0..254) so the caller can drop the result into
// the UNCHANGED inventory[].masterID (uint8) path -- no widen, no parser
// rewrite, no firing change.
//
// Rules (a ':' anywhere in `ref` selects the qualified-id branch):
//   qualified id  -> registry.byQualifiedId(ref).legacyMasterId
//   numeric        -> strtol(ref)
//   miss / bad     -> kEquipmentRefNotFound  (caller falls back / warns)
//
// A resolved ordinal that would not fit the existing 8-bit path (>= 255, i.e.
// the future-widen case, or the 0xff empty sentinel) is REJECTED with
// kEquipmentRefNeedsWiden so the caller warns rather than truncating/crashing.
// This helper NEVER widens and NEVER touches gameplay -- it is a pure lookup
// over the registry, offline-doctest-testable in mc2_tests.
// -------------------------------------------------------------------------
enum {
    kEquipmentRefNotFound   = -1,  // not a resolvable ref (miss / malformed)
    kEquipmentRefNeedsWiden = -2   // resolves to an id that needs the E4 widen
};

// True if `ref` is the qualified-id form (contains ':'), else the legacy
// numeric-ordinal form. Pure.
inline bool isQualifiedEquipmentRef(const char* ref) {
    return ref && std::strchr(ref, ':') != nullptr;
}

inline int resolveLoadoutEquipmentRef(const EquipmentRegistry& reg,
                                      const char* ref) {
    if (!ref || ref[0] == '\0')
        return kEquipmentRefNotFound;

    long ord;
    if (isQualifiedEquipmentRef(ref)) {
        // Qualified string id: resolve through the registry.
        const EquipmentRegistryEntry* e = reg.byQualifiedId(std::string(ref));
        if (!e)
            return kEquipmentRefNotFound;   // unknown id -> caller warns/falls back
        ord = e->legacyMasterId;
    } else {
        // Legacy numeric ordinal.
        char* end = nullptr;
        ord = std::strtol(ref, &end, 10);
        if (end == ref || (end && *end != '\0'))
            return kEquipmentRefNotFound;   // not a clean number
    }

    // Only ids in the EXISTING 8-bit space (0..254) are addressable by the
    // unchanged masterID path. 0xff (255) is the empty sentinel; >=255 is the
    // future-widen case. Do NOT widen here.
    if (ord < 0)
        return kEquipmentRefNotFound;
    if (ord >= 255)
        return kEquipmentRefNeedsWiden;
    return (int)ord;
}

// -------------------------------------------------------------------------
// EQUIP-ENABLE-WIRE-1 (equipment arc, ADD-ladder S1) -- the pure decision rule
// for the enabled->purchase wire: should a def-declared enable FORCE a purchase
// ordinal into the availability set?
//
// ENABLE-ONLY by design: the hook may only ADD availability (union with the
// mission purchase.fit), never remove it -- purchase.fit stays the authoritative
// gate and stock defs (which carry no Enabled key -> defEnabled=false) reproduce
// stock availability EXACTLY. Rules:
//   * defEnabled false              -> never force (stock / absent Enabled key)
//   * legacyMasterId outside 0..254 -> never force (available[255] bound;
//                                      255 = 0xff empty sentinel, E4-widen land)
//   * row not populated             -> never force (an "undefined" compbas row
//                                      has no stats/name; surfacing it is S2
//                                      EQUIP-ADD-FILL-1's job, not S1's)
// Pure + engine-free -> doctest target.
// -------------------------------------------------------------------------
inline bool equipmentEnableWireShouldForce(bool defEnabled, long legacyMasterId,
                                           bool rowPopulated) {
    if (!defEnabled) return false;
    if (legacyMasterId < 0 || legacyMasterId >= 255) return false;
    return rowPopulated;
}

// -------------------------------------------------------------------------
// EQUIP-ADD-FILL-1 (equipment arc, ADD-ladder S2) -- pure logic for the
// new-equipment ADD path: a mod's NEW EquipmentDefinition with an EXPLICIT
// LegacyMasterId naming an UNDEFINED compbas ordinal materializes as a live
// MasterComponent row at load. Everything here is pure/engine-free (doctest
// target); the impure fill pass lives in equipment_registry_engine.cpp.
//
// Gate: MC2_EQUIPMENT_ADD=any-non-"0" AND MC2_UNIT_PROFILE_DATA (family gate).
// Both default OFF -> stock byte-identical.
// -------------------------------------------------------------------------
inline bool mc2_equipment_add_parse_enabled(const char* e) {
    // Same rule as MC2_UNIT_PROFILE_DATA: unset/empty/"0" -> OFF; else ON.
    if (!e || !e[0]) return false;
    return !(e[0] == '0' && e[1] == '\0');
}
inline bool mc2EquipmentAddEnabled() {
    static const bool s = mc2_equipment_add_parse_enabled(getenv("MC2_EQUIPMENT_ADD"));
    return s;
}

// Def sidecar path for a qualified id, namespace-aware:
//   "sample:plasma_rifle" -> "data/defs/equipment/sample/plasma_rifle/equipmentdef.fit"
//   "core:laser"          -> "data/defs/equipment/core/laser/equipmentdef.fit"
//   no ':' at all         -> treated as core/<ref> (defensive; ids always have one)
inline std::string equipmentDefPathForQualifiedId(const std::string& qid) {
    std::string ns = "core", slug = qid;
    size_t colon = qid.find(':');
    if (colon != std::string::npos) {
        ns   = qid.substr(0, colon);
        slug = qid.substr(colon + 1);
    }
    if (ns.empty()) ns = "core";
    return "data/defs/equipment/" + ns + "/" + slug + "/equipmentdef.fit";
}

// The fill decision for one ADD-def. First-wins on duplicate ordinal claims; a
// def targeting a POPULATED stock row is a conflict (overrides are the overlay/
// enable-wire's job, keyed by the row's OWN core:<slug> id -- that self-override
// case is a silent skip, not a finding).
enum EquipmentAddFillDecision {
    EQADD_FILL = 0,            // ordinal free -> populate the row
    EQADD_SKIP_NO_ORDINAL,     // def carries no explicit LegacyMasterId (finding)
    EQADD_SKIP_ORDINAL_RANGE,  // ordinal <0, >=numComponents, or ==255 sentinel (finding)
    EQADD_SKIP_POPULATED,      // ordinal names a POPULATED stock row, not an override (finding)
    EQADD_SKIP_SELF_OVERRIDE,  // def IS that populated row's own core def (silent -- S1/overlay land)
    EQADD_SKIP_DUPLICATE       // another ADD-def already claimed this ordinal (first-wins finding)
};

inline EquipmentAddFillDecision
equipmentAddFillDecide(bool hasOrdinal, long ord, long numComponents,
                       bool rowPopulated, bool selfOverride, bool alreadyClaimed) {
    if (!hasOrdinal)                       return EQADD_SKIP_NO_ORDINAL;
    if (ord < 0 || ord >= numComponents ||
        ord >= 255 /* 0xff sentinel */)    return EQADD_SKIP_ORDINAL_RANGE;
    if (rowPopulated)
        return selfOverride ? EQADD_SKIP_SELF_OVERRIDE : EQADD_SKIP_POPULATED;
    if (alreadyClaimed)                    return EQADD_SKIP_DUPLICATE;
    return EQADD_FILL;
}

// Def Kind (+ weapon Category) -> legacy ComponentFormType ordinal (the CFORM_*
// values above; they index the on-disk ComponentFormString table). Mirrors the
// converter's kind/category vocabulary. Unknown kind -> CFORM_SIMPLE.
inline int equipmentAddKindToFormOrdinal(const char* kind, const char* category) {
    auto ieq = [](const char* a, const char* b) {
        if (!a || !b) return false;
        for (;; ++a, ++b) {
            char ca = (char)std::tolower((unsigned char)*a);
            char cb = (char)std::tolower((unsigned char)*b);
            if (ca != cb) return false;
            if (ca == '\0') return true;
        }
    };
    if (ieq(kind, "Weapon")) {
        if (ieq(category, "Energy"))    return CFORM_WEAPON_ENERGY;
        if (ieq(category, "Ballistic")) return CFORM_WEAPON_BALLISTIC;
        if (ieq(category, "Missile"))   return CFORM_WEAPON_MISSILE;
        return CFORM_WEAPON_ENERGY;    // category-less weapon: energy is the schema default
    }
    if (ieq(kind, "Ammo"))      return CFORM_AMMO;
    if (ieq(kind, "Sensor"))    return CFORM_SENSOR;
    if (ieq(kind, "ECM"))       return CFORM_ECM;
    if (ieq(kind, "JumpJet"))   return CFORM_JUMPJET;
    if (ieq(kind, "Heatsink"))  return CFORM_HEATSINK;
    if (ieq(kind, "Structure")) return CFORM_BULK;
    if (ieq(kind, "Cockpit"))   return CFORM_COCKPIT;
    if (ieq(kind, "Actuator"))  return CFORM_ACTUATOR;
    if (ieq(kind, "Engine"))    return CFORM_ENGINE;
    return CFORM_SIMPLE;
}

// AllowedLocations token list ("CenterTorso,LeftArm,...") -> the 8 legacy crit
// Yes/No columns in compbas column order: Head, CenterTorso, LeftTorso,
// RightTorso, LeftArm, RightArm, LeftLeg, RightLeg. Empty/absent list -> the
// permissive torso+arms default (matches most stock weapon rows). Pure.
inline void equipmentAddAllowedLocationsToCrit(const char* list, bool out[8]) {
    static const char* kNames[8] = {
        "Head", "CenterTorso", "LeftTorso", "RightTorso",
        "LeftArm", "RightArm", "LeftLeg", "RightLeg"
    };
    if (!list || !list[0]) {
        // default: torso + arms yes, head/legs no
        for (int i = 0; i < 8; ++i) out[i] = (i >= 1 && i <= 5);
        return;
    }
    for (int i = 0; i < 8; ++i) out[i] = false;
    std::string tok;
    const char* p = list;
    for (;; ++p) {
        char c = *p;
        if (c == ',' || c == '\0') {
            if (!tok.empty()) {
                for (int i = 0; i < 8; ++i) {
                    // case-insensitive equal
                    const char* n = kNames[i];
                    size_t j = 0;
                    for (; j < tok.size() && n[j]; ++j)
                        if (std::tolower((unsigned char)tok[j]) !=
                            std::tolower((unsigned char)n[j])) break;
                    if (j == tok.size() && n[j] == '\0') { out[i] = true; break; }
                }
                tok.clear();
            }
            if (c == '\0') break;
        } else if (c != ' ' && c != '\t') {
            tok.push_back(c);
        }
    }
}

// The field set for one ADD row -- mirrors the shadow-compare field set (name/
// damage/heat/recycle/range/CV/ammo/critSlots/icon/kind->form) plus the
// logistics-consumer columns (icon/picture/stringID/iconXY).
struct EquipmentAddRowFields {
    long        ordinal        = -1;
    int         formOrdinal    = CFORM_SIMPLE; // CFORM_* (ComponentFormString index)
    std::string displayName;                   // def DisplayName (both consumers' UI name)
    int         critSlots      = 1;            // size / crit spaces
    float       recycle        = 0.f;
    float       heat           = 0.f;
    float       tonnage        = 0.f;
    float       damage         = 0.f;
    float       combatValue    = 0.f;          // CV
    long        cost           = 0;            // resourcePoints / logistics cost
    int         rangeOrdinal   = -1;           // 0/1/2 = short/medium/long; -1 = none
    bool        crit[8]        = { false, true, true, true, true, true, false, false };
    long        ammoMasterId   = 0;            // 0 = none/unlimited
    int         specialEffect  = 0;            // legacy firing VFX cue
    std::string iconFile;                      // MANDATORY (logistics skips row without it)
    std::string pictureFile;                   // defaults to icon stem if empty
    long        stringId       = 0;            // logistics name string (overridden by displayName)
    long        helpStringId   = 0;
    int         iconX          = 1;
    int         iconY          = 1;
};

// Build the ONE synthesized compbas-format CSV line that BOTH consumers parse:
//   MasterComponent::initEXCEL:  id,form,name,size,recycle,heat,tonnage,damage,
//                                CV,cost,range,crit0..crit7,ammoTok,flags,
//                                specialEffect,ammoMasterId
//   LogisticsComponent::init:    id,type,name,(crit),recycle,heat,weight,damage,
//                                (BR),cost,range,head,torso,(4),legs,(4)=the same
//                                ammoTok/flags/specialEffect columns,Ammo=
//                                ammoMasterId,icon,picture,stringID,helpID,
//                                iconX,iconY
// Column-order equality of the two parsers was verified against stock row 156.
inline std::string buildEquipmentAddCsvLine(const EquipmentAddRowFields& f) {
    // ComponentFormString spellings (cmponent.cpp table order == CFORM_*).
    static const char* kFormNames[] = {
        "Simple", "Cockpit", "Sensor", "Actuator", "Engine", "HeatSink",
        "Weapon", "EnergyWeapon", "BallisticWeapon", "MissileWeapon", "Ammo",
        "JumpJet", "Case", "LifeSupport", "Gyroscope", "PowerAmplifier",
        "ECM", "Probe", "Jammer", "Bulk"
    };
    static const char* kRangeNames[] = { "short", "medium", "long" };
    char num[64];
    std::string line;
    auto addF = [&](float v) {
        std::snprintf(num, sizeof(num), "%.2f", (double)v);
        line += num; line += ",";
    };
    auto addI = [&](long v) {
        std::snprintf(num, sizeof(num), "%ld", v);
        line += num; line += ",";
    };
    addI(f.ordinal);
    line += (f.formOrdinal >= 0 && f.formOrdinal < 20) ? kFormNames[f.formOrdinal]
                                                       : "Simple";
    line += ",";
    // Name column: initEXCEL ignores it (string table), logistics ignores it too
    // (stringID); keep the display name for a human reading the log. Commas are
    // stripped -- a ',' here would shift every following column.
    for (char c : f.displayName) if (c != ',') line += c;
    line += ",";
    addI(f.critSlots);
    addF(f.recycle);
    addF(f.heat);
    addF(f.tonnage);
    addF(f.damage);
    addF(f.combatValue);
    addI(f.cost);
    line += (f.rangeOrdinal >= 0 && f.rangeOrdinal <= 2) ? kRangeNames[f.rangeOrdinal]
                                                         : "0";
    line += ",";
    for (int i = 0; i < 8; ++i) { line += f.crit[i] ? "Yes" : "No"; line += ","; }
    // ammoType token: '1' = finite ammo when an ammo row is linked, else '0'.
    line += (f.ammoMasterId > 0) ? "1" : "0"; line += ",";
    line += "0,";                                  // flags (streak derived from ammoTok)
    addI(f.specialEffect);
    addI(f.ammoMasterId);
    line += f.iconFile; line += ",";
    line += f.pictureFile.empty()
                ? (f.iconFile.size() > 4 ? f.iconFile.substr(0, f.iconFile.size() - 4)
                                         : f.iconFile)
                : f.pictureFile;
    line += ",";
    addI(f.stringId);
    addI(f.helpStringId);
    addI(f.iconX);
    std::snprintf(num, sizeof(num), "%d", f.iconY);
    line += num;
    return line;
}

// One row the fill pass materialized -- consumed by the SECOND consumer
// (LogisticsData::initComponents append) and by the registry rebuild (qualified
// id override so the filled row keeps its namespaced mod id, not core:<slug>).
struct EquipmentAddFillRow {
    long        ordinal = -1;
    std::string qualifiedId;   // e.g. "sample:hot_medium_laser"
    std::string displayName;   // def DisplayName (UI-name fallback for new ids)
    std::string description;   // optional def Description (UI desc fallback; S3)
    std::string csvLine;       // the dual-consumer synthesized compbas line
    long        ammoMasterId = 0;
};

// -------------------------------------------------------------------------
// EQUIP-ADD-UI-NAME-1 (equipment arc, ADD-ladder S3) -- pure resolution of the
// legacy component STRING-TABLE id blocks for ADD-filled rows.
//
// Several UI surfaces compute a string id from the masterID ordinal AT DISPLAY
// TIME (they never read MasterComponent::name, which S2 already fixed):
//   32000 + id  component name          (COMPONENT_NAME_START, cmponent.cpp)
//   33000 + id  component abbreviation  (COMPONENT_ABBR_START, infowindow)
//   34000 + id  encyclopedia description (LogisticsComponent helpStringID)
//   46000 + id  mechlab/purchase hover help (IDS_HELP_COMP0, resource.h)
// A NEW ADD id has no entry in any block -> "missing string" marker garbage.
// These helpers decode such an id back to (kind, ordinal) and resolve the text
// from the ADD fill rows. A miss returns nullptr and the caller MUST fall back
// to the legacy cLoadString path -- so with the gates OFF (rows empty) every
// surface is byte-identical to stock. Pure -> doctest target.
// -------------------------------------------------------------------------
enum EquipmentUiStringKind {
    EQUISTR_NONE = 0,   // id not in any component string block
    EQUISTR_NAME,       // 32000..32254
    EQUISTR_ABBREV,     // 33000..33254
    EQUISTR_DESC        // 34000..34254 or 46000..46254 (both are descriptions)
};

inline EquipmentUiStringKind equipmentUiStringDecode(long stringId, long* ordOut) {
    // masterID space is 0..254 (255 = 0xff sentinel, never a component).
    struct Block { long base; EquipmentUiStringKind kind; };
    static const Block kBlocks[] = {
        { 32000, EQUISTR_NAME   },   // COMPONENT_NAME_START
        { 33000, EQUISTR_ABBREV },   // COMPONENT_ABBR_START / IDS_COMP_ABBR0
        { 34000, EQUISTR_DESC   },   // encyclopedia helpStringID block
        { 46000, EQUISTR_DESC   },   // IDS_HELP_COMP0 hover-help block
    };
    for (const Block& b : kBlocks) {
        if (stringId >= b.base && stringId <= b.base + 254) {
            if (ordOut) *ordOut = stringId - b.base;
            return b.kind;
        }
    }
    if (ordOut) *ordOut = -1;
    return EQUISTR_NONE;
}

inline const EquipmentAddFillRow*
equipmentAddFindRow(const std::vector<EquipmentAddFillRow>& rows, long ordinal) {
    for (const EquipmentAddFillRow& r : rows)
        if (r.ordinal == ordinal) return &r;
    return nullptr;
}

// nullptr = "not an ADD row's string" -> caller uses legacy cLoadString.
// DESC falls back to displayName when the def carries no Description so the
// info window never shows the missing-string marker for an ADD row.
inline const char*
equipmentAddUiTextForStringIdPure(const std::vector<EquipmentAddFillRow>& rows,
                                  long stringId) {
    long ord = -1;
    EquipmentUiStringKind kind = equipmentUiStringDecode(stringId, &ord);
    if (kind == EQUISTR_NONE) return nullptr;
    const EquipmentAddFillRow* row = equipmentAddFindRow(rows, ord);
    if (!row) return nullptr;
    if (kind == EQUISTR_DESC)
        return row->description.empty() ? row->displayName.c_str()
                                        : row->description.c_str();
    return row->displayName.c_str();   // NAME/ABBREV both show the DisplayName
}

#endif // EQUIPMENT_REGISTRY_H
