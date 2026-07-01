#ifndef MC2_MODLINT_INPUT_H
#define MC2_MODLINT_INPUT_H
// UNIT-MODLINT-CLI-1 (Phase-3 slice 3). Facts-JSON -> UnitDefinitionValidationInputs.
//
// The modder feeds modlint a small JSON describing a unit's already-extracted
// facts (the converter did the impure extraction; modlint does the pure
// coherence check by calling the SAME validateUnitDefinition the engine uses).
// This header owns ONLY the parse (JSON text -> the plain POD inputs). It drags
// no engine/GL and no file IO (the caller reads the bytes); it is doctest-covered
// in isolation. A hand-rolled minimal JSON reader keeps the standalone target
// dependency-free — the schema is tiny and fixed.
//
// FACTS-JSON schema (all fields optional; defaults = coherent stock unit):
// {
//   "unit_id": "core:atlas",              // label only (printed, not validated)
//   "jump": true,                          // profile.grantsJump
//   "locomotion": "MechWalker",            // MechWalker|Tracked|Wheeled|Hover|Infantry
//   "loadout_joins": [                     // declared loadout->hardpoint joins
//     { "weapon_tag": "Ballistic",         // Energy|Ballistic|Missile|AntiPersonnel|AMS
//       "accepts": ["Energy"],             // tags the hardpoint accepts
//       "damage_location": "RightArm",     // see damageLocationFromToken below
//       "unit_schema": "VehicleFacings",   // Mech8Part|VehicleFacings|Squad|SingleActor
//       "socket_present": false }
//   ],
//   "declared_sockets": [ {"present": false} ],   // one per declared socket
//   "anim_fallbacks":   [ {"resolved": false} ],  // one per needed anim tag
//   "ui_assets":        [ {"resolves": false} ]   // one per declared UI asset
// }
//
// It also accepts the converter's SUMMARY report json (roster / single unit):
// those carry `jump` + `locomotion` per unit under a top-level "units" array, so
// modlint lints the jump-vs-executor facet across a whole roster. See
// parseRosterUnits() below.
#include <string>
#include <vector>
#include <cstdlib>
#include <cctype>

#include "unit_profile_validator.h"   // UnitDefinitionValidationInputs and facets
#include "hardpoint.h"
#include "damage_schema.h"
#include "action_executor_registry.h" // LocomotionModel
#include "locomotion_facet.h"         // parseLocomotionModel

// --- tiny JSON value (object/array/string/number/bool/null) ---------------
// Purpose-built, permissive-enough for our fixed schema. Not a general JSON lib.
struct MlJson {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool                                  b = false;
    double                                n = 0.0;
    std::string                           s;
    std::vector<MlJson>                   arr;
    std::vector<std::pair<std::string, MlJson>> obj;

    const MlJson* find(const char* key) const {
        for (const auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    bool asBool(bool dflt = false) const { return type == Bool ? b : dflt; }
    std::string asStr(const char* dflt = "") const { return type == Str ? s : std::string(dflt); }
};

// --- minimal recursive parser --------------------------------------------
struct MlParser {
    const char* p;
    const char* end;
    bool ok = true;

    explicit MlParser(const std::string& src) : p(src.c_str()), end(src.c_str() + src.size()) {}

    void skipWs() { while (p < end && (unsigned char)*p <= ' ') ++p; }

    MlJson parse() {
        skipWs();
        MlJson v = parseValue();
        return v;
    }

    MlJson parseValue() {
        skipWs();
        if (p >= end) { ok = false; return MlJson(); }
        char c = *p;
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') { MlJson v; v.type = MlJson::Str; v.s = parseString(); return v; }
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') { p += 4; MlJson v; v.type = MlJson::Null; return v; }
        return parseNumber();
    }

    std::string parseString() {
        std::string out;
        ++p;  // opening quote
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                ++p;
                switch (*p) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    default:  out += *p;   break;   // \" \\ \/ etc.
                }
            } else {
                out += *p;
            }
            ++p;
        }
        if (p < end) ++p;  // closing quote
        return out;
    }

    MlJson parseBool() {
        MlJson v; v.type = MlJson::Bool;
        if (*p == 't') { v.b = true;  p += 4; }
        else           { v.b = false; p += 5; }
        return v;
    }

    MlJson parseNumber() {
        char* q = nullptr;
        MlJson v; v.type = MlJson::Num;
        v.n = std::strtod(p, &q);
        if (q == p) ok = false;
        p = q;
        return v;
    }

    MlJson parseArray() {
        MlJson v; v.type = MlJson::Arr;
        ++p;  // [
        skipWs();
        if (p < end && *p == ']') { ++p; return v; }
        while (p < end) {
            v.arr.push_back(parseValue());
            skipWs();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == ']') { ++p; break; }
            ok = false; break;
        }
        return v;
    }

    MlJson parseObject() {
        MlJson v; v.type = MlJson::Obj;
        ++p;  // {
        skipWs();
        if (p < end && *p == '}') { ++p; return v; }
        while (p < end) {
            skipWs();
            if (*p != '"') { ok = false; break; }
            std::string key = parseString();
            skipWs();
            if (p < end && *p == ':') ++p; else { ok = false; break; }
            MlJson val = parseValue();
            v.obj.emplace_back(key, val);
            skipWs();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == '}') { ++p; break; }
            ok = false; break;
        }
        return v;
    }
};

inline MlJson parseModlintJson(const std::string& src, bool* ok = nullptr) {
    MlParser pr(src);
    MlJson v = pr.parse();
    if (ok) *ok = pr.ok;
    return v;
}

// --- token -> enum helpers (spellings match the converter + engine parsers) ---

inline WeaponTag weaponTagFromToken(const std::string& t, bool* known = nullptr) {
    if (known) *known = true;
    if (t == "Energy")        return WTAG_ENERGY;
    if (t == "Ballistic")     return WTAG_BALLISTIC;
    if (t == "Missile")       return WTAG_MISSILE;
    if (t == "AntiPersonnel") return WTAG_ANTIPERSONNEL;
    if (t == "AMS")           return WTAG_AMS;
    if (known) *known = false;
    return WTAG_ENERGY;   // caller decides how to treat unknown
}

inline DamageSchemaId damageSchemaFromToken(const std::string& t) {
    return parseDamageSchema(t.c_str(), DMG_VEHICLE_FACINGS);
}

// Map a modder-facing damage-location token to a DamageLocationId. Accepts the
// mech-part / vehicle-facing names a modder would write. Unknown -> DLOC_BODY
// (which is NOT a member of the mech/vehicle schemas, so it surfaces as a
// DAMAGE_LOCATION_MISSING finding rather than silently passing).
inline DamageLocationId damageLocationFromToken(const std::string& t) {
    if (t == "Head")        return DLOC_MECH_HEAD;
    if (t == "CenterTorso") return DLOC_MECH_CTORSO;
    if (t == "LeftTorso")   return DLOC_MECH_LTORSO;
    if (t == "RightTorso")  return DLOC_MECH_RTORSO;
    if (t == "LeftArm")     return DLOC_MECH_LARM;
    if (t == "RightArm")    return DLOC_MECH_RARM;
    if (t == "LeftLeg")     return DLOC_MECH_LLEG;
    if (t == "RightLeg")    return DLOC_MECH_RLEG;
    if (t == "Front")       return DLOC_VEH_FRONT;
    if (t == "Left")        return DLOC_VEH_LEFT;
    if (t == "Right")       return DLOC_VEH_RIGHT;
    if (t == "Rear")        return DLOC_VEH_REAR;
    if (t == "Turret")      return DLOC_VEH_TURRET;
    if (t == "Body")        return DLOC_BODY;
    return DLOC_BODY;
}

inline LocomotionModel locomotionFromToken(const std::string& t) {
    return parseLocomotionModel(t.c_str(), LOCO_WHEELED);
}

// --- facts JSON -> UnitDefinitionValidationInputs --------------------------
// Builds the plain POD inputs the authoritative validateUnitDefinition consumes.
// Missing fields default to the coherent stock state (validator returns nothing).
inline UnitDefinitionValidationInputs
factsFromJson(const MlJson& root, std::string* unitLabel = nullptr) {
    UnitDefinitionValidationInputs in;

    if (const MlJson* id = root.find("unit_id"))
        if (unitLabel) *unitLabel = id->asStr();

    if (const MlJson* j = root.find("jump"))
        in.profile.grantsJump = j->asBool();
    if (const MlJson* loco = root.find("locomotion"))
        in.profile.locomotion = locomotionFromToken(loco->asStr("Wheeled"));

    if (const MlJson* joins = root.find("loadout_joins")) {
        for (const MlJson& e : joins->arr) {
            LoadoutHardpointJoin j;
            if (const MlJson* wt = e.find("weapon_tag"))
                j.weaponTag = weaponTagFromToken(wt->asStr("Energy"));
            if (const MlJson* acc = e.find("accepts"))
                for (const MlJson& a : acc->arr)
                    addWeaponTag(j.hardpoint, weaponTagFromToken(a.asStr("Energy")));
            if (const MlJson* dl = e.find("damage_location"))
                j.hardpoint.damageLocation = damageLocationFromToken(dl->asStr("Body"));
            if (const MlJson* us = e.find("unit_schema"))
                j.unitSchema = damageSchemaFromToken(us->asStr("VehicleFacings"));
            j.socketPresent = true;
            if (const MlJson* sp = e.find("socket_present"))
                j.socketPresent = sp->asBool(true);
            in.loadoutJoins.push_back(j);
        }
    }

    if (const MlJson* socks = root.find("declared_sockets")) {
        for (const MlJson& e : socks->arr) {
            DeclaredSocketCheck d;
            d.present = true;
            if (const MlJson* pr = e.find("present")) d.present = pr->asBool(true);
            in.declaredSockets.push_back(d);
        }
    }

    if (const MlJson* anims = root.find("anim_fallbacks")) {
        for (const MlJson& e : anims->arr) {
            bool resolved = true;
            if (const MlJson* r = e.find("resolved")) resolved = r->asBool(true);
            in.animFallbacks.push_back(NeededAnimFallback(resolved));
        }
    }

    if (const MlJson* uis = root.find("ui_assets")) {
        for (const MlJson& e : uis->arr) {
            DeclaredUIAssetCheck u;
            u.resolves = true;
            if (const MlJson* r = e.find("resolves")) u.resolves = r->asBool(true);
            in.declaredUIAssets.push_back(u);
        }
    }

    return in;
}

// One roster unit reduced to the facts modlint can check from a converter SUMMARY
// report (which only carries jump + locomotion per unit, not per-join detail).
struct RosterUnitFacts {
    std::string                    label;
    UnitDefinitionValidationInputs inputs;
};

// Parse the converter's report json (single `build_report` or batch report) into
// the per-unit facts we can lint. Returns empty if there is no "units" array.
inline std::vector<RosterUnitFacts> parseRosterUnits(const MlJson& root) {
    std::vector<RosterUnitFacts> out;
    const MlJson* units = root.find("units");
    if (!units || units->type != MlJson::Arr) return out;
    for (const MlJson& u : units->arr) {
        RosterUnitFacts rf;
        if (const MlJson* id = u.find("unit_id")) rf.label = id->asStr();
        if (const MlJson* j = u.find("jump")) rf.inputs.profile.grantsJump = j->asBool();
        if (const MlJson* loco = u.find("locomotion"))
            rf.inputs.profile.locomotion = locomotionFromToken(loco->asStr("Wheeled"));
        out.push_back(rf);
    }
    return out;
}

#endif // MC2_MODLINT_INPUT_H
