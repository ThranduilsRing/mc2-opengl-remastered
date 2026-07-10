// code/jobs_fatal.h
// MC2-JOBS-CORE-1 (§2.0) — the always-compiled enforcement spine.
//
// These checks are compiled in EVERY build config, including RelWithDebInfo
// (the only config a human or the smoke gate ever runs) AND MC2_ASAN. They are
// independent of NDEBUG and _ARMOR. Do NOT route safety checks through
// gosASSERT / assert: both are compiled out in RelWithDebInfo (gosASSERT needs
// _ARMOR which is Debug-only; C assert is stripped by /DNDEBUG), so any safety
// mechanism expressed as an assert is a no-op in the shipped/tested build
// (see plan §0).
//
// JOBS_FATAL_IF  — hard stop: writes {cond, thread id, file:line} to the live
//                  diagnostic-trace sink (MC2_DIAGNOSTIC_TRACE_FILE, the JSONL
//                  the smoke gate already consumes) then std::abort(). Use for
//                  memory-corruption-class hazards (heap/alloc guards) where the
//                  only safe action is to fail fast and be visible in artifacts.
// JOBS_LOGONCE   — log-once + continue: for the reentrancy guard, which must
//                  DEGRADE (serial fallback), not crash — graceful for the
//                  player, still detectable in the trace.
#pragma once

namespace mc2 { namespace jobs { namespace detail {

// Writes a FATAL trace record to MC2_DIAGNOSTIC_TRACE_FILE then aborts.
// [[noreturn]]: never returns.
[[noreturn]] void fatal(const char* cond, const char* file, int line);

// Writes a LOGONCE trace record the first time a given (tag) is seen; cheap
// no-op on subsequent calls. Never aborts.
void logOnce(const char* tag, const char* file, int line);

}}} // namespace mc2::jobs::detail

// Hard stop — always compiled, NDEBUG/_ARMOR-independent. Never elided.
#define JOBS_FATAL_IF(cond) \
    do { if (cond) ::mc2::jobs::detail::fatal(#cond, __FILE__, __LINE__); } while (0)

// Log-once + continue — for the graceful-degrade reentrancy guard.
#define JOBS_LOGONCE(tag) \
    ::mc2::jobs::detail::logOnce((tag), __FILE__, __LINE__)
