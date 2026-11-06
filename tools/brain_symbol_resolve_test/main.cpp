// unit test for the pure brain symbol resolver.
// Table-driven. No engine link — compiles code/brain_symbol_resolve.cpp only.
// Exit 0 = all pass; exit 1 = at least one failure (CI gate).
#include "brain_symbol_resolve.h"
#include <cstdio>
#include <cstring>

static int g_fail = 0;
static int g_pass = 0;

// Expect resolve SUCCESS with a specific value.
static void okExpr(const char* expr, long want) {
    const auto result = brainResolveIntExpr(expr);
    const long got = result.value_or(0xDEAD);
    const bool ok = result.has_value();
    if (!ok || got != want) {
        std::fprintf(stderr, "FAIL expr \"%s\": ok=%d got=%ld want=%ld\n", expr, (int)ok, got, want);
        ++g_fail;
    } else { ++g_pass; }
}

// Expect resolve FAILURE (no value).
static void badExpr(const char* expr) {
    const auto got = brainResolveIntExpr(expr);
    if (got) {
        std::fprintf(stderr, "FAIL expr \"%s\": expected failure, got=%ld\n", expr, *got);
        ++g_fail;
    } else { ++g_pass; }
}

// Single-token resolver (subset of expr resolver).
static void okTok(const char* tok, long want) {
    const auto result = brainResolveSymbolToken(tok);
    const long got = result.value_or(0xDEAD);
    const bool ok = result.has_value();
    if (!ok || got != want) {
        std::fprintf(stderr, "FAIL token \"%s\": ok=%d got=%ld want=%ld\n", tok, (int)ok, got, want);
        ++g_fail;
    } else { ++g_pass; }
}
static void badTok(const char* tok) {
    const auto got = brainResolveSymbolToken(tok);
    if (got) {
        std::fprintf(stderr, "FAIL token \"%s\": expected failure, got=%ld\n", tok, *got);
        ++g_fail;
    } else { ++g_pass; }
}

int main() {
    // --- single tokens: bare ints ---
    okTok("0", 0);
    okTok("2", 2);
    okTok("200", 200);
    okTok("-1", -1);
    okTok("  7  ", 7);            // surrounding whitespace ignored

    // --- single tokens: TARGET_PRIORITY_* (enum order, warrior.h) ---
    okTok("TARGET_PRIORITY_NONE", 0);
    okTok("TARGET_PRIORITY_GAMEOBJECT", 1);
    okTok("TARGET_PRIORITY_MOVER", 2);
    okTok("TARGET_PRIORITY_BUILDING", 3);
    okTok("TARGET_PRIORITY_CURTARGET", 4);
    okTok("TARGET_PRIORITY_SKIP", 19);

    // --- single tokens: CONTACT_CRITERIA_* (flags, dcontact.h) ---
    okTok("CONTACT_CRITERIA_NONE", 0);
    okTok("CONTACT_CRITERIA_ENEMY", 1);
    okTok("CONTACT_CRITERIA_VISUAL", 2);
    okTok("CONTACT_CRITERIA_SENSOR", 16);
    okTok("CONTACT_CRITERIA_VISUAL_OR_SENSOR", 32);
    okTok("CONTACT_CRITERIA_NOT_DISABLED", 64);
    okTok("CONTACT_CRITERIA_ARMED", 128);

    // --- single tokens: unknown / malformed ---
    badTok("BOGUS_SYMBOL");
    badTok("TARGET_PRIORITY_NOPE");
    badTok("");
    badTok("12x");                // trailing junk after int
    badTok("1 2");                // two ints, not a single token

    // --- expressions: single term passthrough ---
    okExpr("0", 0);
    okExpr("-1", -1);
    okExpr("TARGET_PRIORITY_MOVER", 2);

    // --- expressions: '+'-joined flag sums (the carver shape) ---
    okExpr("CONTACT_CRITERIA_ENEMY + CONTACT_CRITERIA_VISUAL_OR_SENSOR + CONTACT_CRITERIA_NOT_DISABLED", 97); // 1+32+64
    okExpr("CONTACT_CRITERIA_ENEMY+CONTACT_CRITERIA_SENSOR", 17);  // no spaces: 1+16
    okExpr("  CONTACT_CRITERIA_ARMED  ", 128);                     // outer whitespace
    okExpr("CONTACT_CRITERIA_ENEMY + 2 + 4", 7);                   // mixed symbol + bare ints

    // --- expressions: malformed ---
    badExpr("");
    badExpr("CONTACT_CRITERIA_ENEMY +");          // trailing '+': empty term
    badExpr("+ CONTACT_CRITERIA_ENEMY");          // leading '+': empty term
    badExpr("CONTACT_CRITERIA_ENEMY + BOGUS");    // unresolved term
    badExpr("A + + B");                           // empty middle term

    std::printf("brain_symbol_resolve_test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
