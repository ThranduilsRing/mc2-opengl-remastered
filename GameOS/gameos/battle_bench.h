// Buffered frame evidence, armed only by the test command channel.
#pragma once
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <string_view>
#include "../../mclib/battle_bench_sink.h"

namespace mc2::battlebench {
using Clock = std::chrono::steady_clock;
inline double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
inline bool arm(std::string_view path) {
    if (output) return false;
    const std::string filename(path);
    output = std::fopen(filename.c_str(), "w");
    if (!output) return false;
    std::setvbuf(output, nullptr, _IOFBF, 1024 * 1024);
    std::atexit([] { if (output) { std::fclose(output); output = nullptr; } });
    std::fprintf(output, "{\"type\":\"clock\",\"model\":\"fixed_30hz_sim_tick\",\"frame\":%llu}\n", static_cast<unsigned long long>(frame));
    beginTick = [] { return static_cast<uint64_t>(Clock::now().time_since_epoch().count()); };
    endTick = [](uint64_t sample) {
        const double ms = std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch() - Clock::duration(sample)).count();
        ++ticks; tickMs += ms;
        std::fprintf(output, "{\"type\":\"tick\",\"frame\":%llu,\"ms\":%.6f}\n",
                     static_cast<unsigned long long>(frame), ms);
    };
    return true;
}
inline void beginFrame() {
    ++frame; ticks = 0; tickMs = 0;
    cpuLeaves = propLeaves = mechBones = particleRecords = particleSixths = 0;
    nativePropLeaves = nativeMechLeaves = 0;
}
inline void endFrame(double ms) {
    if (output) std::fprintf(output,
        "{\"type\":\"cpu\",\"frame\":%llu,\"ms\":%.6f,\"ticks\":%u,\"tick_total_ms\":%.6f,"
        "\"cpu_leaves\":%llu,\"dynamic_prop_leaves\":%llu,\"mech_bones\":%llu,"
        "\"particle_records\":%llu,\"particle_sixths\":%llu,"
        "\"native_prop_leaves\":%llu,\"native_mech_leaves\":%llu}\n",
        static_cast<unsigned long long>(frame), ms, ticks.load(), tickMs.load(),
        static_cast<unsigned long long>(cpuLeaves), static_cast<unsigned long long>(propLeaves),
        static_cast<unsigned long long>(mechBones), static_cast<unsigned long long>(particleRecords),
        static_cast<unsigned long long>(particleSixths),
        static_cast<unsigned long long>(nativePropLeaves), static_cast<unsigned long long>(nativeMechLeaves));
}
} // namespace mc2::battlebench
