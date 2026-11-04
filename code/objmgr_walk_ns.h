// Wall-clock scope for the terrain-object walk counters (walk.*_ns). Presentation/render side only: the sim include
// firewall keeps clocks out of battle_bench_sink.h, so do not include this from simulation code.
#pragma once
#include <chrono>
#include "../mclib/battle_bench_sink.h"

namespace mc2::battlebench {
// Adds the scope's steady_clock nanoseconds to an *Ns field (one pair of clock reads per scope, never per element).
struct ScopeNs {
    Field f;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~ScopeNs() { add(f, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count())); }
};
} // namespace mc2::battlebench
