// Instrumentation sink shared with simulation; no renderer or time-source dependency.
#pragma once
#include <cstdio>
#include <cstdint>

namespace mc2::battlebench {
inline FILE* output = nullptr;
inline uint64_t frame = 0;
inline unsigned ticks = 0;
inline double tickMs = 0;
inline bool attack = false;
inline uint64_t (*beginTick)() = nullptr;
inline void (*endTick)(uint64_t) = nullptr;
struct Tick {
    uint64_t sample = beginTick ? beginTick() : 0;
    ~Tick() { if (sample && endTick) endTick(sample); }
};
} // namespace mc2::battlebench
