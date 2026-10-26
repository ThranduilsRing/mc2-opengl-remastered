#ifndef MC2_MOD_CONFLICT_H
#define MC2_MOD_CONFLICT_H
// MOD-LOAD-ORDER-CONFLICT-REPORT-1 (MODDABILITY phase, unit-profile arc).
//
// Pure, never-silent conflict classification over the mod overlay index.
// The engine (mclib/file.cpp InitModSearchPaths, after indexing) and the
// offline validator (mc2_modlint --mod <dir> ...) both feed this the SAME
// facts and print the SAME findings, so a modder catches conflicts without
// launching the game.
//
// Checks (one line per finding, stable MODCONFLICT_* tokens):
//   * DUPLICATE_DEF_ID    — two mods ship the same data/defs/... key (whole-file
//                           overlay => same qualified def id); higher-priority wins.
//   * DUPLICATE_FILE      — two mods ship the same non-def data/ file.
//   * RESERVED_CORE_NS    — a mod ships a def in the reserved `core:` namespace
//                           (data/defs/{units,equipment}/core/...). That is a
//                           deliberate stock-override channel — surfaced, never silent.
//   * STOCK_OVERRIDE      — a mod def key also exists in the stock loose data/
//                           tree (mod X overrides stock def Y).
//
// Pure by construction: callers gather the facts (index walk, disk existence
// probes); this header only classifies + formats. No IO, no engine includes —
// doctest-covered and shared with the standalone modlint CLI.
#include <string>
#include <string_view>
#include <vector>
#include <utility>

// One indexed overlay file, as facts.
struct ModIndexedEntry {
    std::string key;                          // normalized key, e.g. "data/defs/units/sample/atlas/laserboat/unit.fit"
    std::string winnerModId;                  // mod that owns the slot (highest priority)
    std::vector<std::string> shadowedModIds;  // lower-priority mods that also shipped this key
    bool stockExists = false;                 // same key exists in the stock loose data/ tree
};

enum class ModConflictCode : int {
    MODCONFLICT_DUPLICATE_FILE = 0,
    MODCONFLICT_DUPLICATE_DEF_ID,
    MODCONFLICT_RESERVED_CORE_NAMESPACE,
    MODCONFLICT_STOCK_OVERRIDE,
    MODCONFLICT__COUNT
};
using enum ModConflictCode;

inline const char* modConflictToken(ModConflictCode c) {
    switch (c) {
        case MODCONFLICT_DUPLICATE_FILE:          return "MODCONFLICT_DUPLICATE_FILE";
        case MODCONFLICT_DUPLICATE_DEF_ID:        return "MODCONFLICT_DUPLICATE_DEF_ID";
        case MODCONFLICT_RESERVED_CORE_NAMESPACE: return "MODCONFLICT_RESERVED_CORE_NAMESPACE";
        case MODCONFLICT_STOCK_OVERRIDE:          return "MODCONFLICT_STOCK_OVERRIDE";
        case MODCONFLICT__COUNT: break;
    }
    return "MODCONFLICT_UNKNOWN";
}

struct ModConflictFinding {
    ModConflictCode code;
    std::string line;   // fully-formed modder-readable detail (no [token] prefix)
};

// --- def-key helpers --------------------------------------------------------
// True for keys under the generated-definition roots.
inline bool modKeyIsDef(std::string_view key) {
    return key.rfind("data/defs/units/", 0) == 0 ||
           key.rfind("data/defs/equipment/", 0) == 0;
}

// Best-effort qualified id from a def key:
//   data/defs/equipment/<ns>/<slug>/equipmentdef.fit      -> ns:slug
//   data/defs/units/<ns>/<chassis>/<variant>/unit.fit     -> ns:chassis_variant
// Empty string for non-def keys or malformed paths.
inline std::string modDefQualifiedId(std::string_view key) {
    if (!modKeyIsDef(key)) return std::string();
    const size_t nsStart = (key.rfind("data/defs/units/", 0) == 0)
        ? sizeof("data/defs/units/") - 1
        : sizeof("data/defs/equipment/") - 1;
    size_t nsEnd = key.find('/', nsStart);
    if (nsEnd == std::string::npos) return std::string();
    std::string ns(key.substr(nsStart, nsEnd - nsStart));
    size_t lastSlash = key.rfind('/');
    if (lastSlash <= nsEnd) return std::string();
    std::string mid(key.substr(nsEnd + 1, lastSlash - nsEnd - 1));  // dirs between ns and filename
    if (mid.empty()) return std::string();
    for (char& c : mid) if (c == '/') c = '_';
    return ns + ":" + mid;
}

// Reserved namespace: mods must not squat `core:` for NEW content; shipping a
// core: def path is only legitimate as a deliberate stock override.
inline bool modKeyUsesReservedCoreNamespace(std::string_view key) {
    return key.rfind("data/defs/units/core/", 0) == 0 ||
           key.rfind("data/defs/equipment/core/", 0) == 0;
}

// --- classifier --------------------------------------------------------------
// Entries should be pre-sorted by key by the caller for deterministic output
// (the engine's index is an unordered_map).
inline std::vector<ModConflictFinding> classifyModConflicts(
    const std::vector<ModIndexedEntry>& entries
) {
    std::vector<ModConflictFinding> out;
    for (const ModIndexedEntry& e : entries) {
        const bool isDef = modKeyIsDef(e.key);
        const std::string qid = modDefQualifiedId(e.key);

        if (!e.shadowedModIds.empty()) {
            std::string losers;
            for (size_t i = 0; i < e.shadowedModIds.size(); ++i) {
                if (i) losers += ", ";
                losers += "'" + e.shadowedModIds[i] + "'";
            }
            if (isDef) {
                out.push_back({ MODCONFLICT_DUPLICATE_DEF_ID,
                    "def " + (qid.empty() ? e.key : qid) + " (" + e.key +
                    ") shipped by '" + e.winnerModId + "' AND " + losers +
                    " — '" + e.winnerModId + "' wins (higher load priority)" });
            } else {
                out.push_back({ MODCONFLICT_DUPLICATE_FILE,
                    e.key + " shipped by '" + e.winnerModId + "' AND " + losers +
                    " — '" + e.winnerModId + "' wins (higher load priority)" });
            }
        }

        bool reserved = false;
        if (modKeyUsesReservedCoreNamespace(e.key)) {
            reserved = true;
            out.push_back({ MODCONFLICT_RESERVED_CORE_NAMESPACE,
                "mod '" + e.winnerModId + "' ships a def in the reserved core: namespace" +
                (qid.empty() ? "" : " (" + qid + ")") + " at " + e.key +
                " — this replaces STOCK content; only do this for a deliberate override" });
        }

        // Stock override visibility. Suppressed when the reserved-core line above
        // already announced the same override (one line per finding, not two).
        if (e.stockExists && !reserved) {
            out.push_back({ MODCONFLICT_STOCK_OVERRIDE,
                "mod '" + e.winnerModId + "' overrides stock " +
                (isDef && !qid.empty() ? "def " + qid + " (" + e.key + ")" : e.key) });
        }
    }
    return out;
}

// --- formatter ---------------------------------------------------------------
// Returns finding lines "[TOKEN] detail" capped at `cap`, a "+N more" marker if
// capped, and always ONE trailing summary line. Callers prefix each line for
// their surface (engine: "[mod] conflict: ...").
inline std::vector<std::string> formatModConflictReport(
    const std::vector<ModConflictFinding>& findings, size_t cap = 20
) {
    std::vector<std::string> lines;
    size_t shown = findings.size() < cap ? findings.size() : cap;
    for (size_t i = 0; i < shown; ++i)
        lines.push_back(std::string("[") + modConflictToken(findings[i].code) + "] " +
                        findings[i].line);
    if (findings.size() > cap)
        lines.push_back("(+" + std::to_string(findings.size() - cap) +
                        " more finding(s) suppressed)");
    size_t counts[std::to_underlying(ModConflictCode::MODCONFLICT__COUNT)] = {};
    for (const ModConflictFinding& f : findings)
        if (f.code >= static_cast<ModConflictCode>(0) && f.code < MODCONFLICT__COUNT)
            ++counts[std::to_underlying(f.code)];
    lines.push_back("conflict report: " + std::to_string(findings.size()) +
        " finding(s) — " +
        std::to_string(counts[std::to_underlying(MODCONFLICT_DUPLICATE_FILE)] +
                       counts[std::to_underlying(MODCONFLICT_DUPLICATE_DEF_ID)]) + " duplicate, " +
        std::to_string(counts[std::to_underlying(MODCONFLICT_RESERVED_CORE_NAMESPACE)]) + " reserved-core, " +
        std::to_string(counts[std::to_underlying(MODCONFLICT_STOCK_OVERRIDE)]) + " stock-override");
    return lines;
}

#endif // MC2_MOD_CONFLICT_H
