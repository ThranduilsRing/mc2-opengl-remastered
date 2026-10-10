#include "unitprofile.h"
#include "Gates/gate.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cctype>
bool mc2UnitProfileDataEnabled() {
    static const bool s = mc2::gate::get<"MC2_UNIT_PROFILE_DATA">();
    return s;
}

// UNIT-SOURCE-FLIP-SWITCH-1: `generated` only on the token "generated" (case-insensitive); anything else is legacy.
bool mc2UnitDefinitionSourceIsGenerated() {
    static const bool s = mc2::gate::is<"MC2_UNIT_DEFINITION_SOURCE", "generated">();
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
