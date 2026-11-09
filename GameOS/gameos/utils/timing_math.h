#ifndef GAMEOS_TIMING_MATH_H
#define GAMEOS_TIMING_MATH_H

// SUBSYSTEM-HARNESS-ARC / GAMEOS-TIMING-MATH-HARNESS-1
//
// Pure, header-only time math extracted from the GameOS timing path so it can be
// unit-tested without a platform clock, a window, or the engine. NO platform
// headers, NO globals, NO side effects — production (timing.cpp ticks2ms,
// gameos.cpp gos_GetElapsedTime) delegates to these exact functions.

#include <stdint.h>

namespace timing_math {

// Convert raw performance-counter ticks to milliseconds given the counter
// frequency (ticks/second). A freq of 0 means "ticks are already milliseconds"
// (the non-Windows gettickcount path) and is returned unchanged. Matches the
// historical Windows formula (ticks*1000)/freq exactly.
inline uint64_t ticks_to_ms(uint64_t ticks, uint64_t freq)
{
    if (freq == 0) return ticks;
    return (ticks * 1000) / freq;
}

// Presentation deltas must retain fractions of a millisecond. In particular,
// a sub-ms frame must not become zero and trigger the legacy 250 ms fallback.
inline double ticks_to_ms_fractional(uint64_t ticks, uint64_t freq)
{
    return freq ? static_cast<double>(ticks) * 1000.0 / static_cast<double>(freq)
                : static_cast<double>(ticks);
}

// Frame-elapsed clamp — the gos_GetElapsedTime non-Windows path. Given the
// previously returned elapsed value `prev` in seconds (prev < 0 means "first
// call, no prior sample"), the freshly sampled `now` in seconds, and
// Environment.MaxTimeDelta in seconds (<= 0 disables clamping), return the new
// elapsed value. The caller stores the result back as BOTH the running `prev`
// and the return value (in the original code those are always equal):
//
//   * first call (prev < 0)        -> now                 (seed, no clamp)
//   * gap exceeds maxDelta (> 0)   -> prev + maxDelta      (clamped catch-up)
//   * otherwise                    -> now
//
// The clamp bounds the largest time span handed to the simulation after a
// stutter / debugger pause, so one long frame cannot teleport game logic.
inline double advance_elapsed_clamp(double prev, double now, double maxDelta)
{
    if (prev < 0.0) return now;
    if (maxDelta > 0.0 && now - prev > maxDelta) return prev + maxDelta;
    return now;
}

} // namespace timing_math

#endif // GAMEOS_TIMING_MATH_H
