// Instrumentation sink shared with simulation; no renderer or time-source dependency.
#pragma once
#include <cstdio>
#include <atomic>
#include <cstdint>

namespace mc2::battlebench {
inline FILE* output = nullptr;
inline uint64_t frame = 0;
// atomic: with the G7 overlap the tick runs on the sim worker while the main thread writes the frame line
inline std::atomic<unsigned> ticks{0};
inline std::atomic<double> tickMs{0.0};
inline bool attack = false;
// Draw payload census, accumulated only while the existing benchmark sink is armed.
// These count submitted work, before GPU occlusion; bones and particle records are not leaves/particles.
inline uint64_t cpuLeaves = 0, propLeaves = 0, mechBones = 0;
inline uint64_t particleRecords = 0, particleSixths = 0;
inline uint64_t (*beginTick)() = nullptr;
inline void (*endTick)(uint64_t) = nullptr;
struct Tick {
    uint64_t sample = beginTick ? beginTick() : 0;
    ~Tick() { if (sample && endTick) endTick(sample); }
};
} // namespace mc2::battlebench
