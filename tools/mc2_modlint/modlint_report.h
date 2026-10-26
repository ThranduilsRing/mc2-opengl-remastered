#ifndef MC2_MODLINT_REPORT_H
#define MC2_MODLINT_REPORT_H
// Pure finding -> modder-readable text.
//
// Turns the authoritative validator's std::vector<UnitProfileFinding> into the
// lines a modder reads, using the ONE reason-code -> message table
// (unit_profile_findings_text.h). No IO — returns strings the CLI prints — so it
// is doctest-covered without a process. The validator itself is the single
// source of the RULES; this only formats what it emitted.
#include <string>
#include <string_view>
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
// `source` (optional) is the file the facts came from; when non-empty it is
// appended so a modder reads WHICH file the finding is about (modder-first error
// surface: which mod/file/id). Empty `source` keeps the legacy header verbatim.
inline std::string formatUnitFindings(std::string_view label,
                                      const std::vector<UnitProfileFinding>& findings,
                                      std::string_view source = "") {
    std::string out = "unit " + (label.empty() ? std::string("<unnamed>") : std::string(label));
    if (!source.empty()) out += std::string(" [").append(source).append("]");
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
