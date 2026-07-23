#include "unitprofile.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cctype>
bool mc2_unitprofile_parse_enabled(const char* e) {
    return e && e[0] != '\0' && e[0] != '0';
}
bool mc2UnitProfileDataEnabled() {
    static const bool s = mc2_unitprofile_parse_enabled(getenv("MC2_UNIT_PROFILE_DATA"));
    return s;
}

// UNIT-SOURCE-FLIP-SWITCH-1: `generated` ONLY on the exact token "generated"
// (case-insensitive). Everything else -> legacy (the default), so an unset var,
// "legacy", "0", or any typo is safe: legacy stays byte-identical.
bool mc2_unit_definition_source_is_generated(const char* e) {
    if (!e || !e[0]) return false;
    // case-insensitive compare against "generated"
    const char* want = "generated";
    for (int i = 0; ; ++i) {
        const char a = (char)std::tolower((unsigned char)e[i]);
        const char b = want[i];
        if (b == '\0') return a == '\0';     // matched all of "generated" and no trailing junk
        if (a != b)    return false;
    }
}
bool mc2UnitDefinitionSourceIsGenerated() {
    static const bool s =
        mc2_unit_definition_source_is_generated(getenv("MC2_UNIT_DEFINITION_SOURCE"));
    return s;
}

// UNIT-SOURCE-FLIP-SWITCH-1: pure slug — mirrors extract.py::_variant_slug.
const char* mc2GeneratedVariantSlug(const char* variantName, int index,
                                    char* out, unsigned long cap) {
    if (!out || cap == 0) return out;
    unsigned long n = 0;
    bool lastUnderscore = true;   // true so a leading separator run is dropped
    if (variantName) {
        for (const char* p = variantName; *p && n + 1 < cap; ++p) {
            unsigned char c = (unsigned char)*p;
            if (c == '\'')           // apostrophes dropped outright (not turned to '_')
                continue;
            const char lc = (char)std::tolower(c);
            if ((lc >= 'a' && lc <= 'z') || (lc >= '0' && lc <= '9')) {
                out[n++] = lc;
                lastUnderscore = false;
            } else if (!lastUnderscore) {
                out[n++] = '_';       // collapse any non-alnum run to ONE '_'
                lastUnderscore = true;
            }
        }
    }
    // strip a single trailing '_'
    if (n > 0 && out[n - 1] == '_')
        --n;
    out[n] = '\0';
    if (n == 0) {                     // empty -> "v<index>"
        std::snprintf(out, cap, "v%d", index);
    }
    return out;
}

// UNIT-SOURCE-FLIP-SWITCH-1: apply parsed generated tokens onto the green facets.
// Reuse the SAME parse functions the .fit path uses (parseLocomotionModel /
// parseDamageSchema), keeping the current value as the fallback so an
// unrecognized token is a no-op (per-field legacy fallback).
bool mc2ApplyGeneratedLocomotion(const char* token, UnitProfile& p) {
    if (!token || !token[0]) return false;
    p.locomotion = parseLocomotionModel(token, p.locomotion);
    return true;
}
bool mc2ApplyGeneratedDamageSchema(const char* token, UnitProfile& p) {
    if (!token || !token[0]) return false;
    p.damageSchema = parseDamageSchema(token, p.damageSchema);
    return true;
}
