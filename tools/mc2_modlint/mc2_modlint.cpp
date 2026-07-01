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
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "unit_profile_validator.h"
#include "modlint_input.h"
#include "modlint_report.h"
#include "modlint_registry.h"

#include "equipment_validator.h"
#include "modlint_equipment_input.h"
#include "modlint_equipment_report.h"

static void usage() {
    std::fprintf(stderr,
        "mc2_modlint — offline unit + equipment validator\n"
        "  mc2_modlint --unit <facts.json>               lint one unit's facts\n"
        "  mc2_modlint --roster <report.json>            lint a unit converter roster\n"
        "  mc2_modlint --equipment <facts.json>          lint one EquipmentDefinition\n"
        "  mc2_modlint --equipment-roster <report.json>  lint an equipment converter --json report\n"
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

// --unit: one facts JSON -> findings. Returns count of findings.
static int lintUnit(const std::string& text, bool* parseOk) {
    MlJson root = parseModlintJson(text, parseOk);
    const ActionExecutorRegistry reg = modlintExecutorRegistry();
    std::string label;
    UnitDefinitionValidationInputs in = factsFromJson(root, &label);
    std::vector<UnitProfileFinding> findings = validateUnitDefinition(in, reg);
    std::cout << formatUnitFindings(label, findings);
    return (int)findings.size();
}

// --roster: a converter report -> per-unit jump-facet findings + summary.
static int lintRoster(const std::string& text, bool* parseOk, int* unitsOut) {
    MlJson root = parseModlintJson(text, parseOk);
    const ActionExecutorRegistry reg = modlintExecutorRegistry();
    std::vector<RosterUnitFacts> units = parseRosterUnits(root);
    *unitsOut = (int)units.size();
    int totalFindings = 0;
    for (const RosterUnitFacts& u : units) {
        std::vector<UnitProfileFinding> f = validateUnitDefinition(u.inputs, reg);
        std::cout << formatUnitFindings(u.label, f);
        totalFindings += (int)f.size();
    }
    return totalFindings;
}

// --equipment: one equipment facts JSON -> findings. Returns count of findings.
static int lintEquipment(const std::string& text, bool* parseOk) {
    MlJson root = parseModlintJson(text, parseOk);
    std::string label;
    EquipmentValidationInputs in = equipmentFactsFromJson(root, &label);
    std::vector<EquipmentFinding> findings = validateEquipmentDefinition(in);
    std::cout << formatEquipmentFindings(label, findings);
    return (int)findings.size();
}

// --equipment-roster: an equipment converter --json report -> per-component findings.
static int lintEquipmentRoster(const std::string& text, bool* parseOk, int* compsOut) {
    MlJson root = parseModlintJson(text, parseOk);
    std::vector<RosterEquipmentFacts> comps = parseEquipmentRoster(root);
    *compsOut = (int)comps.size();
    int totalFindings = 0;
    for (const RosterEquipmentFacts& c : comps) {
        std::vector<EquipmentFinding> f = validateEquipmentDefinition(c.inputs);
        std::cout << formatEquipmentFindings(c.label, f);
        totalFindings += (int)f.size();
    }
    return totalFindings;
}

int main(int argc, char** argv) {
    std::string mode, path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--unit" || a == "--roster" ||
             a == "--equipment" || a == "--equipment-roster") && i + 1 < argc) {
            mode = a; path = argv[++i];
        } else if (a == "-h" || a == "--help") {
            usage(); return 0;
        } else {
            std::fprintf(stderr, "mc2_modlint: unexpected argument '%s'\n", a.c_str());
            usage(); return 2;
        }
    }
    if (mode.empty() || path.empty()) { usage(); return 2; }

    std::string text;
    if (!readAll(path, &text)) {
        std::fprintf(stderr, "mc2_modlint: cannot read '%s'\n", path.c_str());
        return 2;
    }

    bool parseOk = true;
    int findings = 0, count = 1;
    const char* noun = "unit";
    if (mode == "--unit") {
        findings = lintUnit(text, &parseOk);
    } else if (mode == "--roster") {
        findings = lintRoster(text, &parseOk, &count);
    } else if (mode == "--equipment") {
        noun = "component";
        findings = lintEquipment(text, &parseOk);
    } else {  // --equipment-roster
        noun = "component";
        findings = lintEquipmentRoster(text, &parseOk, &count);
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
