#pragma once
#include <cstdint>

// DETERMINISTIC-RNG-1: GL-free, header-only LCG kernel.
//
// Pure logic so it can be unit-tested offline (tests/unit/test_det_rng.cpp)
// and survive any renderer rewrite. Used by gos_rand/gos_srand under the
// MC2_DETERMINISTIC_RNG gate (GameOS/gameos/gameos.cpp). When the gate is OFF
// none of this is reached and the engine uses the CRT srand/rand path.
//
// Constants are the Numerical Recipes LCG (1664525 / 1013904223), matching the
// in-repo ScatterBrush LCG (editor/ScatterBrush.cpp) and the per-unit-stream
// direction in DETERMINISTIC-RNG-RECON-1.

namespace mc2_det_rng {

// Advance the 32-bit LCG state in place and return a 15-bit result in [0, 32767],
// shaped to match the legacy CRT path's `rand() % (1<<15)` output range.
// Uses the high bits (>> 17) for better distribution than the low bits.
inline uint32_t next15(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return (state >> 17) & 0x7FFFu;
}

// Deterministic mission seed from a mission name (e.g. "mc2_01").
// FNV-1a 32-bit over the NUL-terminated name; stable across processes/builds.
inline uint32_t hashMissionName(const char* name) {
    uint32_t h = 2166136261u;
    if (name) {
        for (const char* p = name; *p; ++p) {
            h ^= static_cast<uint32_t>(static_cast<unsigned char>(*p));
            h *= 16777619u;
        }
    }
    return h;
}

// ABL seedRandom(seed) value. seed == -1 asks for a fresh seed: under MC2_DETERMINISTIC_RNG that is the deterministic sim
// seed plus a per-mission call index, never the wall clock; with the gate off it is the stock clock value.
inline uint32_t resolveSeedRandom(int requested, bool detRng, uint32_t simSeed, uint32_t clockSeed, uint32_t callIndex = 0) {
    if (requested != -1)
        return static_cast<uint32_t>(requested);
    if (!detRng) return clockSeed;
    // Odd multiplication gives each call a distinct 32-bit seed until the index wraps.
    return simSeed ^ (callIndex * 0x9E3779B9u);
}

} // namespace mc2_det_rng
