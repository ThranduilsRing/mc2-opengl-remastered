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
//   mc2_modlint --unit <facts.json>     lint one unit's facts JSON
//   mc2_modlint --roster <report.json>  lint every unit in a converter report
//   mc2_modlint --unit - < facts.json   read from stdin
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

static void usage() {
    std::fprintf(stderr,
        "mc2_modlint (UNIT-MODLINT-CLI-1) — offline unit validator\n"
        "  mc2_modlint --unit <facts.json>      lint one unit's facts\n"
        "  mc2_modlint --roster <report.json>   lint a converter roster report\n"
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

int main(int argc, char** argv) {
    std::string mode, path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--unit" || a == "--roster") && i + 1 < argc) {
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
    int findings = 0, units = 1;
    if (mode == "--unit") {
        findings = lintUnit(text, &parseOk);
    } else {
        findings = lintRoster(text, &parseOk, &units);
    }

    if (!parseOk) {
        std::fprintf(stderr, "mc2_modlint: malformed JSON input\n");
        return 2;
    }

    std::cout << "\nsummary: " << units << " unit(s) linted, "
              << findings << " finding(s)\n";
    std::cout << (findings == 0 ? "RESULT: clean\n" : "RESULT: findings present\n");
    return findings == 0 ? 0 : 1;
}
