// tools/mc2_modlint/mc2_modlint.cpp
// UNIT-MODLINT-CLI-1 (Phase-3 slice 3 of the UNIT-PROFILE arc).
//
// Standalone, OFFLINE modder tool: validate a unit's facts WITHOUT launching the
// game. It reads a facts-JSON (or the converter's summary report json), calls the
// SAME pure validateUnitDefinition() the engine uses, and prints the modder-term
// findings + a summary. Exit 0 = clean, nonzero = findings (CI-friendly).
//
// GL/IO-free by construction — the only IO is reading the input file (a plain
// std::ifstream); it links ONLY the pure validator headers + the tiny JSON
// parser, exactly like mc2_tests. No engine, no OpenGL, no build/deploy/smoke.
//
// Usage:
//   mc2_modlint --unit <facts.json>               lint one unit's facts JSON
//   mc2_modlint --roster <report.json>            lint every unit in a converter report
//   mc2_modlint --equipment <facts.json>          lint one EquipmentDefinition's facts
//   mc2_modlint --equipment-roster <report.json>  lint an equipment converter --json report
//   mc2_modlint --unit - < facts.json             read from stdin
//
// The --equipment* paths (WEAPON-MODLINT-1, equipment arc slice 7) wrap the SAME
// authoritative pure validateEquipmentDefinition() the engine will use (single
// source of truth). The --unit* paths are unchanged (UNIT-MODLINT-CLI-1).
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "unit_profile_validator.h"
#include "modlint_input.h"
#include "modlint_report.h"
#include "modlint_registry.h"

#include "equipment_validator.h"
#include "modlint_equipment_input.h"
#include "modlint_equipment_report.h"

#include "modlint_manifest.h"
#include "modlint_variant_csv.h" // UNIT-VARIANT-ADD-1: legacy chassis-CSV variant-block validator
#include "mod_conflict.h"   // MOD-LOAD-ORDER-CONFLICT-REPORT-1: pure classifier shared with the engine
#include "modder_finding.h" // MODDER-FIRST-ERROR-SURFACE-1: shared modder-finding context format

static void usage() {
    std::fprintf(stderr,
        "mc2_modlint — offline unit + equipment validator\n"
        "  mc2_modlint --unit <facts.json>               lint one unit's facts\n"
        "  mc2_modlint --roster <report.json>            lint a unit converter roster\n"
        "  mc2_modlint --equipment <facts.json>          lint one EquipmentDefinition\n"
        "  mc2_modlint --equipment-roster <report.json>  lint an equipment converter --json report\n"
        "  mc2_modlint --manifest <mod.json>             lint a mod package manifest (mods/<id>/mod.json)\n"
        "  mc2_modlint --variant-csv <mech.csv> [--compbas <compbas.csv>]\n"
        "                                                lint a legacy chassis CSV's 23+97*n variant\n"
        "                                                blocks (ADDed variants); --compbas enables the\n"
        "                                                loadout-ref resolution check\n"
        "  mc2_modlint --mod <dir> [--mod <dir> ...] [--stock-root <dir>]\n"
        "                                                offline load-order conflict report; pass mods\n"
        "                                                highest-priority-first (active mod, then deps),\n"
        "                                                same precedence as the engine loader\n"
        "  use '-' as the path to read from stdin\n");
}

static bool readAll(const std::string& path, std::string* out) {
    if (path == "-") {
        std::ostringstream ss; ss << std::cin.rdbuf(); *out = ss.str(); return true;
    }
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf(); *out = ss.str();
    return true;
}

// The source label appended to each finding block so a modder sees WHICH file
// the finding is about (modder-first error surface). stdin ('-') has no path, so
// it stays empty and the header is unchanged.
static std::string sourceLabel(const std::string& path) {
    return path == "-" ? std::string() : path;
}

// --unit: one facts JSON -> findings. Returns count of findings.
static int lintUnit(const std::string& text, bool* parseOk, const std::string& src) {
    MlJson root = parseModlintJson(text, parseOk);
    const ActionExecutorRegistry reg = modlintExecutorRegistry();
    std::string label;
    UnitDefinitionValidationInputs in = factsFromJson(root, &label);
    std::vector<UnitProfileFinding> findings = validateUnitDefinition(in, reg);
    std::cout << formatUnitFindings(label, findings, src);
    return (int)findings.size();
}

// --roster: a converter report -> per-unit jump-facet findings + summary.
static int lintRoster(const std::string& text, bool* parseOk, int* unitsOut, const std::string& src) {
    MlJson root = parseModlintJson(text, parseOk);
    const ActionExecutorRegistry reg = modlintExecutorRegistry();
    std::vector<RosterUnitFacts> units = parseRosterUnits(root);
    *unitsOut = (int)units.size();
    int totalFindings = 0;
    for (const RosterUnitFacts& u : units) {
        std::vector<UnitProfileFinding> f = validateUnitDefinition(u.inputs, reg);
        std::cout << formatUnitFindings(u.label, f, src);
        totalFindings += (int)f.size();
    }
    return totalFindings;
}

// --equipment: one equipment facts JSON -> findings. Returns count of findings.
static int lintEquipment(const std::string& text, bool* parseOk, const std::string& src) {
    MlJson root = parseModlintJson(text, parseOk);
    std::string label;
    EquipmentValidationInputs in = equipmentFactsFromJson(root, &label);
    std::vector<EquipmentFinding> findings = validateEquipmentDefinition(in);
    std::cout << formatEquipmentFindings(label, findings, src);
    return (int)findings.size();
}

// --equipment-roster: an equipment converter --json report -> per-component findings.
static int lintEquipmentRoster(const std::string& text, bool* parseOk, int* compsOut, const std::string& src) {
    MlJson root = parseModlintJson(text, parseOk);
    std::vector<RosterEquipmentFacts> comps = parseEquipmentRoster(root);
    *compsOut = (int)comps.size();
    int totalFindings = 0;
    for (const RosterEquipmentFacts& c : comps) {
        std::vector<EquipmentFinding> f = validateEquipmentDefinition(c.inputs);
        std::cout << formatEquipmentFindings(c.label, f, src);
        totalFindings += (int)f.size();
    }
    return totalFindings;
}

// --manifest: one mod.json -> MODMAN_* findings. Returns count of findings.
static int lintManifest(const std::string& text, bool* parseOk, const std::string& src) {
    MlJson root = parseModlintJson(text, parseOk);
    ModManifestFacts facts = manifestFactsFromJson(root);
    std::vector<ModManifestFinding> findings = validateModManifest(facts);
    std::cout << formatManifestFindings(facts.id, findings, src);
    return (int)findings.size();
}

// --variant-csv: one legacy chassis CSV -> VARC_ findings (UNIT-VARIANT-ADD-1).
// compbasText empty = skip loadout-ref resolution (structure checks still run).
static int lintVariantCsv(const std::string& text, const std::string& compbasText,
                          const std::string& src, const std::string& modId) {
    VariantCsvTable t = parseVariantCsv(text);
    std::set<int> ids;
    const std::set<int>* pIds = nullptr;
    if (!compbasText.empty()) {
        ids = parseCompbasPopulatedIds(compbasText);
        pIds = &ids;
    }
    std::vector<ModderFinding> findings = validateVariantCsvAdd(t, src, modId, pIds);
    std::cout << formatVariantCsvFindings(src, findings, modId);
    return (int)findings.size();
}

// --mod: offline mirror of the engine's load-time conflict report.
// Walks each mod dir's data/ tree (dirs given highest-priority-first, matching
// the loader's active > dep[0] > ... precedence), builds the same first-wins
// key index the engine builds (lowercase, '/', dot-entries skipped), and runs
// the SAME pure classifyModConflicts the engine runs at load. stockRoot
// (optional) enables the stock-override check by probing <stockRoot>/<key>.
static int lintModConflicts(const std::vector<std::string>& modDirs,
                            const std::string& stockRoot) {
    namespace fs = std::filesystem;

    auto modIdOf = [](const fs::path& dir) -> std::string {
        std::string text;
        const std::string jsonPath = (dir / "mod.json").string();
        bool readable = readAll(jsonPath, &text);
        if (readable) {
            bool ok = true;
            MlJson root = parseModlintJson(text, &ok);
            if (ok) {
                ModManifestFacts f = manifestFactsFromJson(root);
                if (!f.id.empty()) return f.id;
            }
        }
        // MODDER-FIRST-ERROR-SURFACE-1: the loader-mirroring fallback (folder
        // name as id) is never silent — say which mod.json failed and why.
        ModderFinding f;
        f.message  = readable
            ? "mod.json is malformed or missing \"id\""
            : "mod.json not found/readable";
        f.file     = jsonPath;
        f.field    = "id";
        f.expected = "mc2-mod/1 manifest with a lowercase \"id\" token";
        f.fallback = "folder name '" + dir.filename().string() + "' used as mod id";
        std::fprintf(stderr, "mc2_modlint: %s\n", formatModderFinding(f).c_str());
        return dir.filename().string();   // loader fallback: folder name as id
    };

    struct Slot { std::string winner; std::string winnerPath; std::vector<std::string> losers; };
    std::unordered_map<std::string, Slot> index;   // first-wins, like g_modIndex

    for (const std::string& d : modDirs) {
        fs::path dir(d);
        std::string modId = modIdOf(dir);
        fs::path dataDir = dir / "data";
        std::error_code ec;
        if (!fs::is_directory(dataDir, ec)) {
            std::fprintf(stderr, "mc2_modlint: '%s' has no data/ directory, skipping\n", d.c_str());
            continue;
        }
        for (fs::recursive_directory_iterator it(dataDir, ec), end; it != end && !ec; it.increment(ec)) {
            const std::string leaf = it->path().filename().string();
            if (!leaf.empty() && leaf[0] == '.') {   // loader ruling C4: skip dot entries
                if (it->is_directory()) it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file()) continue;
            std::string rel = fs::relative(it->path(), dir, ec).generic_string();
            if (ec) continue;
            std::string key = rel;
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            auto existing = index.find(key);
            if (existing == index.end()) index[key] = { modId, it->path().generic_string(), {} };
            else existing->second.losers.push_back(modId);
        }
    }

    std::vector<ModIndexedEntry> entries;
    for (const auto& kv : index) {
        ModIndexedEntry e;
        e.key = kv.first;
        e.winnerModId = kv.second.winner;
        e.shadowedModIds = kv.second.losers;
        if (!stockRoot.empty() && e.key.rfind("data/defs/", 0) == 0) {
            std::error_code ec;
            e.stockExists = fs::exists(fs::path(stockRoot) / e.key, ec);
        }
        entries.push_back(std::move(e));
    }
    std::sort(entries.begin(), entries.end(),
              [](const ModIndexedEntry& a, const ModIndexedEntry& b) { return a.key < b.key; });

    std::vector<ModConflictFinding> findings = classifyModConflicts(entries);
    for (const std::string& line : formatModConflictReport(findings))
        std::cout << line << "\n";

    // UNIT-VARIANT-ADD-1: any shadowed legacy chassis CSV (data/objects/<mech>.csv
    // carrying a "VariantName" label) gets its 23+97*n variant blocks validated.
    // compbas for the loadout-ref check: the mods' own shadowed compbas wins,
    // else <stockRoot>/data/objects/compbas.csv, else the check is skipped.
    int variantFindings = 0;
    {
        std::string compbasText;
        auto cb = index.find("data/objects/compbas.csv");
        if (cb != index.end()) readAll(cb->second.winnerPath, &compbasText);
        else if (!stockRoot.empty())
            readAll((fs::path(stockRoot) / "data/objects/compbas.csv").string(), &compbasText);

        for (const auto& kv : index) {
            const std::string& key = kv.first;
            if (key.rfind("data/objects/", 0) != 0) continue;
            if (key.size() < 4 || key.compare(key.size() - 4, 4, ".csv") != 0) continue;
            std::string text;
            if (!readAll(kv.second.winnerPath, &text)) continue;
            VariantCsvTable t = parseVariantCsv(text);
            if (!looksLikeChassisCsv(t)) continue;   // compbas/pilots/buildings etc.
            std::set<int> ids;
            const std::set<int>* pIds = nullptr;
            if (!compbasText.empty()) { ids = parseCompbasPopulatedIds(compbasText); pIds = &ids; }
            std::vector<ModderFinding> vf = validateVariantCsvAdd(t, key, kv.second.winner, pIds);
            std::cout << formatVariantCsvFindings(key, vf, kv.second.winner);
            variantFindings += (int)vf.size();
        }
    }

    const bool clean = findings.empty() && variantFindings == 0;
    std::cout << (clean ? "RESULT: clean\n" : "RESULT: findings present\n");
    return clean ? 0 : 1;
}

int main(int argc, char** argv) {
    std::string mode, path;
    std::vector<std::string> modDirs;
    std::string stockRoot;
    std::string compbasPath;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--unit" || a == "--roster" ||
             a == "--equipment" || a == "--equipment-roster" ||
             a == "--manifest" || a == "--variant-csv") && i + 1 < argc) {
            mode = a; path = argv[++i];
        } else if (a == "--compbas" && i + 1 < argc) {
            compbasPath = argv[++i];
        } else if (a == "--mod" && i + 1 < argc) {
            mode = a; modDirs.push_back(argv[++i]);
        } else if (a == "--stock-root" && i + 1 < argc) {
            stockRoot = argv[++i];
        } else if (a == "-h" || a == "--help") {
            usage(); return 0;
        } else {
            std::fprintf(stderr, "mc2_modlint: unexpected argument '%s'\n", a.c_str());
            usage(); return 2;
        }
    }
    if (mode == "--mod") return lintModConflicts(modDirs, stockRoot);
    if (mode.empty() || path.empty()) { usage(); return 2; }

    std::string text;
    if (!readAll(path, &text)) {
        std::fprintf(stderr, "mc2_modlint: cannot read '%s'\n", path.c_str());
        return 2;
    }

    bool parseOk = true;
    int findings = 0, count = 1;
    const char* noun = "unit";
    const std::string src = sourceLabel(path);
    if (mode == "--unit") {
        findings = lintUnit(text, &parseOk, src);
    } else if (mode == "--roster") {
        findings = lintRoster(text, &parseOk, &count, src);
    } else if (mode == "--equipment") {
        noun = "component";
        findings = lintEquipment(text, &parseOk, src);
    } else if (mode == "--manifest") {
        noun = "manifest";
        findings = lintManifest(text, &parseOk, src);
    } else if (mode == "--variant-csv") {
        noun = "chassis-csv";
        std::string compbasText;
        if (!compbasPath.empty() && !readAll(compbasPath, &compbasText)) {
            std::fprintf(stderr, "mc2_modlint: cannot read compbas '%s'\n", compbasPath.c_str());
            return 2;
        }
        findings = lintVariantCsv(text, compbasText, src, "");
    } else {  // --equipment-roster
        noun = "component";
        findings = lintEquipmentRoster(text, &parseOk, &count, src);
    }

    if (!parseOk) {
        // MODDER-FIRST-ERROR-SURFACE-1: say WHICH file failed to parse and that
        // nothing was validated (the fallback), one consistent context format.
        ModderFinding f;
        f.message  = "malformed JSON input — file could not be parsed";
        f.file     = (path == "-") ? "<stdin>" : path;
        f.expected = "valid JSON (facts/report/manifest)";
        f.fallback = "nothing validated (exit 2)";
        std::fprintf(stderr, "mc2_modlint: %s\n", formatModderFinding(f).c_str());
        return 2;
    }

    std::cout << "\nsummary: " << count << " " << noun << "(s) linted, "
              << findings << " finding(s)\n";
    std::cout << (findings == 0 ? "RESULT: clean\n" : "RESULT: findings present\n");
    return findings == 0 ? 0 : 1;
}
