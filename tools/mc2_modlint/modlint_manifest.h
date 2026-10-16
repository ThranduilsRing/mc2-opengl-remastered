#ifndef MC2_MODLINT_MANIFEST_H
#define MC2_MODLINT_MANIFEST_H
//
// Pure mod.json manifest validation for mc2_modlint --manifest. This validates
// the EXISTING mod package manifest (mods/<id>/mod.json — the one the engine
// loader mclib/file.cpp ReadModJson() and the launcher mc2_launcher.cpp both
// read). It does NOT invent a second manifest; the rules here encode what those
// consumers actually require:
//   * engine loader consumes: id, dependencies
//   * launcher consumes:      name, type, description, version, radioGroup, env
//   * packer (mc2mod.py):     schema, id, version
//   * author is OPTIONAL metadata (near-term consumer: launcher tooltip /
//     conflict-report attribution) — never a finding when absent.
//
// Same shape as the unit/equipment validators: a plain facts struct, a pure
// validate() returning findings with stable MODMAN_* reason tokens (grep-able
// by CI / mod tooling), and a formatter. Header-only, no IO, doctest-covered.
#include <string>
#include <vector>

#include "modlint_input.h"   // MlJson / parseModlintJson

// --- reason codes ----------------------------------------------------------
enum ModManifestReasonCode {
    MODMAN_NOT_OBJECT = 0,        // mod.json top level is not a JSON object
    MODMAN_SCHEMA_MISSING,        // "schema" absent/empty
    MODMAN_SCHEMA_UNKNOWN,        // "schema" present but not "mc2-mod/1"
    MODMAN_ID_MISSING,            // "id" absent/empty (loader falls back to folder name)
    MODMAN_ID_INVALID,            // "id" fails ^[a-z0-9][a-z0-9._-]*$
    MODMAN_NAME_MISSING,          // "name" absent/empty (launcher shows raw id)
    MODMAN_VERSION_MISSING,       // "version" absent/empty
    MODMAN_VERSION_INVALID,       // "version" not dotted digits (e.g. 1.0.0)
    MODMAN_TYPE_MISSING,          // "type" absent (launcher heuristics may misbucket)
    MODMAN_TYPE_UNKNOWN,          // "type" not assets|campaign|dependency
    MODMAN_DESCRIPTION_MISSING,   // "description" absent/empty (launcher pane blank)
    MODMAN_DEP_INVALID,           // a dependencies[] entry fails the id token rule
    MODMAN_DEP_SELF,              // a mod depends on its own id
    MODMAN_DEP_DUPLICATE,         // same dependency listed twice
    MODMAN__COUNT
};

inline const char* modManifestReasonToken(ModManifestReasonCode c) {
    switch (c) {
        case MODMAN_NOT_OBJECT:          return "MODMAN_NOT_OBJECT";
        case MODMAN_SCHEMA_MISSING:      return "MODMAN_SCHEMA_MISSING";
        case MODMAN_SCHEMA_UNKNOWN:      return "MODMAN_SCHEMA_UNKNOWN";
        case MODMAN_ID_MISSING:          return "MODMAN_ID_MISSING";
        case MODMAN_ID_INVALID:          return "MODMAN_ID_INVALID";
        case MODMAN_NAME_MISSING:        return "MODMAN_NAME_MISSING";
        case MODMAN_VERSION_MISSING:     return "MODMAN_VERSION_MISSING";
        case MODMAN_VERSION_INVALID:     return "MODMAN_VERSION_INVALID";
        case MODMAN_TYPE_MISSING:        return "MODMAN_TYPE_MISSING";
        case MODMAN_TYPE_UNKNOWN:        return "MODMAN_TYPE_UNKNOWN";
        case MODMAN_DESCRIPTION_MISSING: return "MODMAN_DESCRIPTION_MISSING";
        case MODMAN_DEP_INVALID:         return "MODMAN_DEP_INVALID";
        case MODMAN_DEP_SELF:            return "MODMAN_DEP_SELF";
        case MODMAN_DEP_DUPLICATE:       return "MODMAN_DEP_DUPLICATE";
        case MODMAN__COUNT: break;
    }
    return "MODMAN_UNKNOWN";
}

inline const char* modManifestReasonMessage(ModManifestReasonCode c) {
    switch (c) {
        case MODMAN_NOT_OBJECT:
            return "mod.json must be a JSON object";
        case MODMAN_SCHEMA_MISSING:
            return "missing \"schema\" — add \"schema\": \"mc2-mod/1\"";
        case MODMAN_SCHEMA_UNKNOWN:
            return "unknown \"schema\" — this tooling understands \"mc2-mod/1\"";
        case MODMAN_ID_MISSING:
            return "missing \"id\" — the engine will fall back to the folder name; declare it explicitly";
        case MODMAN_ID_INVALID:
            return "invalid \"id\" — must be a lowercase token: start with [a-z0-9], then [a-z0-9._-]";
        case MODMAN_NAME_MISSING:
            return "missing \"name\" — the launcher will show the raw id instead of a display name";
        case MODMAN_VERSION_MISSING:
            return "missing \"version\" — declare a dotted version like \"1.0.0\"";
        case MODMAN_VERSION_INVALID:
            return "invalid \"version\" — must be dotted digits like \"1.0\" or \"1.0.0\"";
        case MODMAN_TYPE_MISSING:
            return "missing \"type\" — declare \"assets\", \"campaign\", or \"dependency\" so the launcher buckets it correctly";
        case MODMAN_TYPE_UNKNOWN:
            return "unknown \"type\" — must be \"assets\", \"campaign\", or \"dependency\"";
        case MODMAN_DESCRIPTION_MISSING:
            return "missing \"description\" — the launcher description pane will be blank";
        case MODMAN_DEP_INVALID:
            return "invalid dependencies[] entry — each dependency must be a mod id token";
        case MODMAN_DEP_SELF:
            return "mod lists itself in dependencies[]";
        case MODMAN_DEP_DUPLICATE:
            return "duplicate dependencies[] entry";
        case MODMAN__COUNT: break;
    }
    return "unknown finding";
}

// --- facts -----------------------------------------------------------------
struct ModManifestFacts {
    bool isObject = false;
    // presence flags distinguish "absent" from "present but empty" (both flagged).
    bool hasSchema = false, hasId = false, hasName = false,
         hasVersion = false, hasType = false, hasDescription = false;
    std::string schema, id, name, version, type, description, author;
    std::vector<std::string> dependencies;
};

inline ModManifestFacts manifestFactsFromJson(const MlJson& root) {
    ModManifestFacts f;
    if (root.type != MlJson::Obj) return f;
    f.isObject = true;
    auto grab = [&root](const char* key, bool* has, std::string* out) {
        const MlJson* v = root.find(key);
        if (v && v->type == MlJson::Str) { *has = true; *out = v->s; }
    };
    grab("schema",      &f.hasSchema,      &f.schema);
    grab("id",          &f.hasId,          &f.id);
    grab("name",        &f.hasName,        &f.name);
    grab("version",     &f.hasVersion,     &f.version);
    grab("type",        &f.hasType,        &f.type);
    grab("description", &f.hasDescription, &f.description);
    bool hasAuthor = false;
    grab("author", &hasAuthor, &f.author);
    if (const MlJson* deps = root.find("dependencies"))
        if (deps->type == MlJson::Arr)
            for (const MlJson& d : deps->arr)
                f.dependencies.push_back(d.type == MlJson::Str ? d.s : std::string());
    return f;
}

// --- rules -----------------------------------------------------------------
// Same token rule the launcher/packer enforce: ^[a-z0-9][a-z0-9._-]*$
inline bool modIdTokenValid(const std::string& id) {
    if (id.empty()) return false;
    auto headOk = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    if (!headOk(id[0])) return false;
    for (char c : id)
        if (!headOk(c) && c != '.' && c != '_' && c != '-') return false;
    return true;
}

// Dotted digits: "1", "1.0", "1.0.0", ... (no empty segments).
inline bool modVersionValid(const std::string& v) {
    if (v.empty()) return false;
    bool segHasDigit = false;
    for (char c : v) {
        if (c >= '0' && c <= '9') { segHasDigit = true; }
        else if (c == '.') { if (!segHasDigit) return false; segHasDigit = false; }
        else return false;
    }
    return segHasDigit;   // must not end on a dot
}

struct ModManifestFinding {
    ModManifestReasonCode code;
    std::string detail;   // instance detail (e.g. the offending value)
};

inline std::vector<ModManifestFinding> validateModManifest(const ModManifestFacts& f) {
    std::vector<ModManifestFinding> out;
    if (!f.isObject) { out.push_back({ MODMAN_NOT_OBJECT, "" }); return out; }

    if (!f.hasSchema || f.schema.empty()) out.push_back({ MODMAN_SCHEMA_MISSING, "" });
    else if (f.schema != "mc2-mod/1")     out.push_back({ MODMAN_SCHEMA_UNKNOWN, f.schema });

    if (!f.hasId || f.id.empty())         out.push_back({ MODMAN_ID_MISSING, "" });
    else if (!modIdTokenValid(f.id))      out.push_back({ MODMAN_ID_INVALID, f.id });

    if (!f.hasName || f.name.empty())     out.push_back({ MODMAN_NAME_MISSING, "" });

    if (!f.hasVersion || f.version.empty()) out.push_back({ MODMAN_VERSION_MISSING, "" });
    else if (!modVersionValid(f.version))   out.push_back({ MODMAN_VERSION_INVALID, f.version });

    if (!f.hasType || f.type.empty())     out.push_back({ MODMAN_TYPE_MISSING, "" });
    else if (f.type != "assets" && f.type != "campaign" && f.type != "dependency")
        out.push_back({ MODMAN_TYPE_UNKNOWN, f.type });

    if (!f.hasDescription || f.description.empty())
        out.push_back({ MODMAN_DESCRIPTION_MISSING, "" });

    std::vector<std::string> seen;
    for (const std::string& d : f.dependencies) {
        if (!modIdTokenValid(d)) { out.push_back({ MODMAN_DEP_INVALID, d }); continue; }
        if (f.hasId && d == f.id) out.push_back({ MODMAN_DEP_SELF, d });
        bool dup = false;
        for (const std::string& s : seen) if (s == d) { dup = true; break; }
        if (dup) out.push_back({ MODMAN_DEP_DUPLICATE, d });
        else seen.push_back(d);
    }
    return out;
}

// --- formatter (mirrors formatUnitFindings' modder-first shape) -------------
inline std::string formatManifestFindings(const std::string& label,
                                          const std::vector<ModManifestFinding>& findings,
                                          const std::string& source = "") {
    std::string out = "manifest " + (label.empty() ? std::string("<unnamed>") : label);
    if (!source.empty()) out += " [" + source + "]";
    if (findings.empty()) { out += ": OK (0 findings)\n"; return out; }
    out += ": " + std::to_string(findings.size()) + " finding(s)\n";
    for (const ModManifestFinding& f : findings) {
        out += "  [";
        out += modManifestReasonToken(f.code);
        out += "] ";
        out += modManifestReasonMessage(f.code);
        if (!f.detail.empty()) { out += ": '"; out += f.detail; out += "'"; }
        out += "\n";
    }
    return out;
}

#endif // MC2_MODLINT_MANIFEST_H
