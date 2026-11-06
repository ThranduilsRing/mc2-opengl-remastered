// UNITQUERY-SETTARGETPRIORITY-1: brain symbol resolver (pure, engine-free).
// See brain_symbol_resolve.h. Symbol tables mirror code/warrior.h
// (TargetPriorityType, enum order) and code/dcontact.h (CONTACT_CRITERIA_* flags).
#include "mc2_czstring.h"
#include "brain_symbol_resolve.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <optional>

namespace {

struct SymEntry { const char* name; long value; };

// TARGET_PRIORITY_* — enum order from code/warrior.h (TargetPriorityType).
// CONTACT_CRITERIA_* — bit flags from code/dcontact.h.
// Keep in sync with those headers (see scripts/check-brain-symbol-table.py).
const SymEntry kSymbols[] = {
    {"TARGET_PRIORITY_NONE", 0},
    {"TARGET_PRIORITY_GAMEOBJECT", 1},
    {"TARGET_PRIORITY_MOVER", 2},
    {"TARGET_PRIORITY_BUILDING", 3},
    {"TARGET_PRIORITY_CURTARGET", 4},
    {"TARGET_PRIORITY_TURRET", 5},
    {"TARGET_PRIORITY_TURRET_CONTROL", 6},
    {"TARGET_PRIORITY_GATE", 7},
    {"TARGET_PRIORITY_GATE_CONTROL", 8},
    {"TARGET_PRIORITY_SALVAGE", 9},
    {"TARGET_PRIORITY_MECHBAY", 10},
    {"TARGET_PRIORITY_LOCATION", 11},
    {"TARGET_PRIORITY_EVENT_TARGETED", 12},
    {"TARGET_PRIORITY_EVENT_HIT", 13},
    {"TARGET_PRIORITY_EVENT_DAMAGED", 14},
    {"TARGET_PRIORITY_EVENT_MATE_DIED", 15},
    {"TARGET_PRIORITY_EVENT_FRIENDLY_DISABLED", 16},
    {"TARGET_PRIORITY_EVENT_FRIENDLY_DESTROYED", 17},
    {"TARGET_PRIORITY_EVENT_FRIENDLY_WITHDRAWS", 18},
    {"TARGET_PRIORITY_SKIP", 19},

    {"CONTACT_CRITERIA_NONE", 0},
    {"CONTACT_CRITERIA_ENEMY", 1},
    {"CONTACT_CRITERIA_VISUAL", 2},
    {"CONTACT_CRITERIA_GUARD_BREACH", 4},
    {"CONTACT_CRITERIA_NOT_CHALLENGED", 8},
    {"CONTACT_CRITERIA_SENSOR", 16},
    {"CONTACT_CRITERIA_VISUAL_OR_SENSOR", 32},
    {"CONTACT_CRITERIA_NOT_DISABLED", 64},
    {"CONTACT_CRITERIA_ARMED", 128},
};

// Copy token[b..e) into buf (null-terminated), trimming ASCII whitespace from
// both ends. Returns false if the trimmed span is empty or does not fit.
bool copyTrimmed(mc2::czstring s, size_t len, char* buf, size_t bufsz) {
    size_t b = 0, e = len;
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    if (e == b) return false;               // empty after trim
    if ((e - b) >= bufsz) return false;     // too long
    std::memcpy(buf, s + b, e - b);
    buf[e - b] = '\0';
    return true;
}

std::optional<long> resolveTrimmed(mc2::czstring tok) {
    // Try a bare decimal integer (full-consume required).
    char* endp = nullptr;
    long v = std::strtol(tok, &endp, 10);
    if (endp != tok && *endp == '\0') return v;
    // Fall back to a symbol-table lookup (exact match).
    for (const SymEntry& e : kSymbols) {
        if (std::strcmp(tok, e.name) == 0) return e.value;
    }
    return std::nullopt;
}

} // namespace

std::optional<long> brainResolveSymbolToken(mc2::czstring token) {
    if (!token) return std::nullopt;
    char buf[64];
    if (!copyTrimmed(token, std::strlen(token), buf, sizeof(buf))) return std::nullopt;
    return resolveTrimmed(buf);
}

std::optional<long> brainResolveIntExpr(mc2::czstring expr) {
    if (!expr) return std::nullopt;
    long sum = 0;
    int terms = 0;
    const char* p = expr;
    const char* termStart = expr;
    char buf[64];
    for (;; ++p) {
        if (*p == '+' || *p == '\0') {
            if (!copyTrimmed(termStart, static_cast<size_t>((p - termStart)), buf, sizeof(buf)))
                return std::nullopt;   // empty term (leading/trailing/double '+') or overflow
            const auto value = resolveTrimmed(buf);
            if (!value) return std::nullopt;
            sum += *value;
            ++terms;
            if (*p == '\0') break;
            termStart = p + 1;
        }
    }
    if (terms == 0) return std::nullopt;
    return sum;
}
