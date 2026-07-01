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

#endif // EQUIPMENT_REGISTRY_H
