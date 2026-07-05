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
#include "mod_conflict.h"   // MOD-LOAD-ORDER-CONFLICT-REPORT-1: pure classifier shared with the engine

static void usage() {
    std::fprintf(stderr,
        "mc2_modlint — offline unit + equipment validator\n"
        "  mc2_modlint --unit <facts.json>               lint one unit's facts\n"
        "  mc2_modlint --roster <report.json>            lint a unit converter roster\n"
        "  mc2_modlint --equipment <facts.json>          lint one EquipmentDefinition\n"
        "  mc2_modlint --equipment-roster <report.json>  lint an equipment converter --json report\n"
        "  mc2_modlint --manifest <mod.json>             lint a mod package manifest (mods/<id>/mod.json)\n"
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
        if (readAll((dir / "mod.json").string(), &text)) {
            bool ok = true;
            MlJson root = parseModlintJson(text, &ok);
            if (ok) {
                ModManifestFacts f = manifestFactsFromJson(root);
                if (!f.id.empty()) return f.id;
            }
        }
        return dir.filename().string();   // loader fallback: folder name as id
    };

    struct Slot { std::string winner; std::vector<std::string> losers; };
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
            if (existing == index.end()) index[key] = { modId, {} };
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
    std::cout << (findings.empty() ? "RESULT: clean\n" : "RESULT: findings present\n");
    return findings.empty() ? 0 : 1;
}

int main(int argc, char** argv) {
    std::string mode, path;
    std::vector<std::string> modDirs;
    std::string stockRoot;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--unit" || a == "--roster" ||
             a == "--equipment" || a == "--equipment-roster" ||
             a == "--manifest") && i + 1 < argc) {
            mode = a; path = argv[++i];
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
    } else {  // --equipment-roster
        noun = "component";
        findings = lintEquipmentRoster(text, &parseOk, &count, src);
    }

    if (!parseOk) {
        std::fprintf(stderr, "mc2_modlint: malformed JSON input\n");
        return 2;
    }

    std::cout << "\nsummary: " << count << " " << noun << "(s) linted, "
              << findings << " finding(s)\n";
    std::cout << (findings == 0 ? "RESULT: clean\n" : "RESULT: findings present\n");
    return findings == 0 ? 0 : 1;
}
