#ifndef MC2_MODDER_FINDING_H
#define MC2_MODDER_FINDING_H
// MODDER-FIRST-ERROR-SURFACE-1 (MODDABILITY phase, unit-profile arc).
//
// ONE shared shape + ONE line format for every modder-facing failure, so a
// modder always reads WHICH source (mod id / "core" / "generated"), WHICH file,
// WHICH id, WHICH field, what was EXPECTED, what was ACTUALLY found, and what
// FALLBACK the engine used instead. Reuses the EXISTING stable reason-token
// conventions (UPV_* / EQV_* / MODMAN_* / MODCONFLICT_*) in `reason` — this
// header introduces NO parallel code system; loader paths that have no enum
// today simply leave `reason` empty and keep their surface prefix
// ([UNIT-SOURCE=generated] / [EQUIP-SOURCE=generated] / [EQUIP-IDALIAS]).
//
// Pure by construction: no IO, no engine includes — doctest-covered in
// mc2_tests and shared verbatim with the standalone mc2_modlint CLI. Emitters
// prefix the formatted line with their surface tag; the CONTEXT SUFFIX is the
// consistent part: `(source=.. file=.. id=.. field=.. expected=.. actual=..
// fallback=..)` with empty fields omitted.
#include <string>

struct ModderFinding {
    std::string reason;    // stable machine token (UPV_/EQV_/MODMAN_/MODCONFLICT_) or empty
    std::string message;   // human headline: what is wrong, in modder terms
    std::string source;    // "core", "generated", or the owning mod id
    std::string file;      // the file the fact came from (data-relative or abs)
    std::string id;        // qualified id, e.g. "core:hunchback" / "sample:hot_medium_laser"
    std::string field;     // the offending field/key, e.g. "Locomotion"
    std::string expected;  // what would have been accepted
    std::string actual;    // what was actually found
    std::string fallback;  // what the engine/tool used instead (never silent)
};

// The consistent context suffix. Empty fields are omitted; all fields empty ->
// empty string (no dangling "()").
inline std::string modderFindingContext(const ModderFinding& f) {
    std::string ctx;
    auto add = [&ctx](const char* key, const std::string& val) {
        if (val.empty()) return;
        if (!ctx.empty()) ctx += " ";
        ctx += key;
        ctx += "=";
        ctx += val;
    };
    add("source",   f.source);
    add("file",     f.file);
    add("id",       f.id);
    add("field",    f.field);
    add("expected", f.expected);
    add("actual",   f.actual);
    add("fallback", f.fallback);
    if (ctx.empty()) return ctx;
    return "(" + ctx + ")";
}

// Full line (no surface prefix — the emitter owns that): "[REASON] message
// (context)" with the reason bracket and/or context omitted when empty.
inline std::string formatModderFinding(const ModderFinding& f) {
    std::string line;
    if (!f.reason.empty())
        line += "[" + f.reason + "] ";
    line += f.message;
    const std::string ctx = modderFindingContext(f);
    if (!ctx.empty())
        line += " " + ctx;
    return line;
}

#endif // MC2_MODDER_FINDING_H
