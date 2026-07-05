// mc2_verify.h — MC2-VERIFY-LIVE-1
//
// Live data-contract guard primitive. Unlike gosASSERT (compiled to
// ((void)0) in every build the project ships — _ARMOR is Debug-only), the
// MC2_VERIFY family is COMPILED IN ALL CONFIGS including RelWithDebInfo.
//
// Three runtime modes via env MC2_VERIFY_MODE (resolved once, first use):
//   log    (DEFAULT) shadow-log: "[VERIFY] file:line (cond) msg" to stderr +
//          OutputDebugString + crashbundle ring (gos_crashbundle last_trace),
//          then CONTINUE. Counter line "[VERIFY] ..." printed at mission end.
//   fatal  STOP with message: same logging, then InternalFunctionStop routing
//          + a non-continuable raise so the crash-bundle SEH filter captures
//          stack + ring, then hard process termination. Never returns.
//   off    exactly-legacy: the failure handler returns TRUE (as if the check
//          passed) so guarded degradation blocks are skipped and legacy
//          behavior — including the legacy corruption/crash — is preserved.
//          No output, no counters.
//
// Cost contract: zero overhead when the condition is TRUE (single branch,
// short-circuit; the handler call is the cold path only).
//
// Usage:
//   MC2_VERIFY(ptr != NULL, "widget %d has no frame", i);          // observe
//   if (!MC2_VERIFY_BOUNDS(id, MAX_TEAMS, "TeamId"))               // degrade
//       id = MAX_TEAMS - 1;   // reached in log mode only; off = legacy path
//
// RULES (see docs/verify-primitive.md and the adversarial census in
// .claude/MODERNIZATION-ROADMAP-1-ADVERSARIAL.md §2c/§2d):
//   * Condition expressions MUST be side-effect-free — they evaluate in every
//     mode, including off.
//   * NO mechanical sweeps. The 80 always-fire gosASSERT(0) sites, the 32
//     _DEBUG-entangled sites, and all mp*.cpp/multplyr.cpp sites are
//     untouchable by any blanket conversion.
//   * Each reclassified site carries the comment
//     "MC2_VERIFY reclassified from gosASSERT (slice <NAME>)".
#pragma once

namespace mc2verify {

// Cold-path failure handler (printf-style message). Return contract:
//   true  -> behave as if the check PASSED (mode=off: exactly-legacy)
//   false -> caller may degrade gracefully (mode=log)
//   never returns in mode=fatal
bool Fail(const char* file, int line, const char* cond, const char* fmt, ...);

// "[VERIFY] mission-end fires=<n> ..." counter line (stderr + crashbundle
// ring). Silent in mode=off. Resets the per-mission fire counter.
void MissionSummary(const char* label);

} // namespace mc2verify

// Returns bool: true = condition held (or mode=off pretends it did).
#define MC2_VERIFY(cond, ...) \
    ((!!(cond)) || ::mc2verify::Fail(__FILE__, __LINE__, #cond, __VA_ARGS__))

// Bounds guard: 0 <= idx < count.
#define MC2_VERIFY_BOUNDS(idx, count, what) \
    MC2_VERIFY(((long)(idx) >= 0) && ((long)(idx) < (long)(count)), \
               "%s index %ld out of bounds [0,%ld)", \
               (what), (long)(idx), (long)(count))

// Null-pointer guard.
#define MC2_VERIFY_NOTNULL(ptr, what) \
    MC2_VERIFY((ptr) != 0, "%s is NULL", (what))
