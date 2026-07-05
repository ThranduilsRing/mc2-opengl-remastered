// UI-DEFS-LOCKSTEP-HARDEN-1: pure decision logic for the data/defs/ui <-> exe
// lockstep contract (BLACK-MENU-BISECT-1 root cause: version-skewed defs/ui
// packages loaded "successfully" but rendered zero elements, totally silently,
// under an opaque black-clear).
//
// Header-only and dependency-free ON PURPOSE: unit-tested from
// tests/unit/test_ui_defs_lockstep.cpp without dragging in ImGui/GL.

#ifndef MC2R_UI_DEFS_LOCKSTEP_H
#define MC2R_UI_DEFS_LOCKSTEP_H

#include <cstddef>
#include <cstdlib>

namespace UiDefs {

// The defs/ui package schema version this exe was built against.
// Bump when the page .fit schema changes incompatibly; ship a matching
// data/defs/ui/PACKAGE.fit in the same deploy (deploy_payload.py enforces
// presence). A mismatched or missing manifest => every page load fails =>
// LogisticsScreen falls back to the legacy (pre-ImGui) screens: degraded but
// visible, never an opaque black quad.
constexpr int kExpectedUiPackageVersion = 1;

// Parse "Version = N" (case-insensitive key match on "version") out of the
// PACKAGE.fit text. Tolerates FITini framing, [Section] headers, l/ul type
// prefixes, and // comments. Returns -1 when no version key is found.
inline int parseUiPackageVersionText(const char* text)
{
    if (!text)
        return -1;
    auto lowerEq = [](char a, char b) {
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        return a == b;
    };
    for (const char* s = text; *s; ++s) {
        // line-comment: skip to end of line
        if (s[0] == '/' && s[1] == '/') {
            while (*s && *s != '\n') ++s;
            if (!*s) break;
            continue;
        }
        if (lowerEq(s[0], 'v') && lowerEq(s[1], 'e') && lowerEq(s[2], 'r') &&
            lowerEq(s[3], 's') && lowerEq(s[4], 'i') && lowerEq(s[5], 'o') &&
            lowerEq(s[6], 'n')) {
            const char* p = s + 7;
            while (*p == ' ' || *p == '\t') ++p;
            if (*p != '=')
                continue;
            ++p;
            while (*p == ' ' || *p == '\t') ++p;
            if (*p < '0' || *p > '9')
                continue;
            return static_cast<int>(std::strtol(p, nullptr, 10));
        }
    }
    return -1;
}

// Version-compare decision: only an exact match is a healthy package.
// (found == -1 covers both "manifest missing" and "manifest unparseable".)
constexpr bool uiPackageVersionOk(int found, int expected)
{
    return found >= 0 && found == expected;
}

// Zero-element decision: a page that parsed but produced no elements can
// never draw anything; loading it would arm the black-clear over nothing.
constexpr bool pageElementCountAcceptable(std::size_t elementCount)
{
    return elementCount > 0;
}

// Zero-draw fail-soft decision (per frame, evaluated on the NEXT frame):
// a loaded, non-passthrough page that has rendered at least once and emitted
// zero draw commands must yield the screen back to the legacy path.
constexpr bool pageShouldFailSoftToLegacy(bool loaded, bool legacyPassthrough,
                                          bool hasRendered, int lastRenderDrawCount)
{
    return loaded && !legacyPassthrough && hasRendered && lastRenderDrawCount <= 0;
}

} // namespace UiDefs

#endif // MC2R_UI_DEFS_LOCKSTEP_H
