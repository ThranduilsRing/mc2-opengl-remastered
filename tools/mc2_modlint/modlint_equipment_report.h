#ifndef MC2_MODLINT_EQUIPMENT_REPORT_H
#define MC2_MODLINT_EQUIPMENT_REPORT_H
// WEAPON-MODLINT-1 (equipment arc, slice 7). Pure EquipmentFinding -> modder text.
//
// Turns the authoritative validator's std::vector<EquipmentFinding> into the lines
// a modder reads, using the ONE EQV_ reason-code -> message table
// (equipment_findings_text.h, next to the validator). No IO -- returns strings the
// CLI prints -- so it is doctest-covered without a process. The validator itself
// is the single source of the RULES; this only formats what it emitted. Direct
// mirror of the unit arc's modlint_report.h.
#include <string>
#include <vector>

#include "equipment_validator.h"
#include "equipment_findings_text.h"

// One formatted finding line: "  [EQV_TOKEN] generic message: instance detail".
inline std::string formatEquipmentFinding(const EquipmentFinding& f) {
    std::string line = "  [";
    line += equipmentReasonToken(f.code);
    line += "] ";
    line += equipmentReasonMessage(f.code);
    if (f.message && f.message[0]) {
        line += ": ";
        line += f.message;
    }
    return line;
}

// Full per-component block: a header + one line per finding + a clean/ok line.
inline std::string formatEquipmentFindings(const std::string& label,
                                           const std::vector<EquipmentFinding>& findings) {
    std::string out = "component " + (label.empty() ? std::string("<unnamed>") : label);
    if (findings.empty()) {
        out += ": OK (0 findings)\n";
        return out;
    }
    out += ": " + std::to_string(findings.size()) + " finding(s)\n";
    for (const EquipmentFinding& f : findings)
        out += formatEquipmentFinding(f) + "\n";
    return out;
}

#endif // MC2_MODLINT_EQUIPMENT_REPORT_H
