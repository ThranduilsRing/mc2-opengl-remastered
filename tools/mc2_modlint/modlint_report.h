#ifndef MC2_MODLINT_REPORT_H
#define MC2_MODLINT_REPORT_H
// UNIT-MODLINT-CLI-1 (Phase-3 slice 3). Pure finding -> modder-readable text.
//
// Turns the authoritative validator's std::vector<UnitProfileFinding> into the
// lines a modder reads, using the ONE reason-code -> message table
// (unit_profile_findings_text.h). No IO — returns strings the CLI prints — so it
// is doctest-covered without a process. The validator itself is the single
// source of the RULES; this only formats what it emitted.
#include <string>
#include <vector>

#include "unit_profile_validator.h"
#include "unit_profile_findings_text.h"

// One formatted finding line: "  [UPV_TOKEN] generic message: instance detail".
inline std::string formatFinding(const UnitProfileFinding& f) {
    std::string line = "  [";
    line += unitProfileReasonToken(f.code);
    line += "] ";
    line += unitProfileReasonMessage(f.code);
    if (f.message && f.message[0]) {
        line += ": ";
        line += f.message;
    }
    return line;
}

// Full per-unit block: a header + one line per finding + a clean/ok line.
inline std::string formatUnitFindings(const std::string& label,
                                      const std::vector<UnitProfileFinding>& findings) {
    std::string out = "unit " + (label.empty() ? std::string("<unnamed>") : label);
    if (findings.empty()) {
        out += ": OK (0 findings)\n";
        return out;
    }
    out += ": " + std::to_string(findings.size()) + " finding(s)\n";
    for (const UnitProfileFinding& f : findings)
        out += formatFinding(f) + "\n";
    return out;
}

#endif // MC2_MODLINT_REPORT_H
