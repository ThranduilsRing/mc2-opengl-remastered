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
// [PERF_COUNTERS v1] one helper: per-frame means over the run so far (no frames -> no line).
inline void emitPerfCounters() {
    const uint64_t n = totFrames.load();
    if (!n) return;
    char line[1024];
    int len = std::snprintf(line, sizeof line, "[PERF_COUNTERS v1] frames=%llu", static_cast<unsigned long long>(n));
    for (unsigned f = 0; f < FieldCount && len > 0 && len < static_cast<int>(sizeof line); ++f) {
        const bool ns = f == WaitSealedNs || f == WaitSimNs || f == WaitGpuMainNs;
        const double mean = static_cast<double>(tot[f].load()) / static_cast<double>(n) / (ns ? 1.0e6 : 1.0);
        std::string name = kFieldNames[f];
        if (ns) { name.resize(name.size() - 2); name += "ms"; }
        len += std::snprintf(line + len, sizeof line - len, " %s=%.4f", name.c_str(), mean);
    }
    if (len > 0 && len < static_cast<int>(sizeof line))
        std::snprintf(line + len, sizeof line - len, " first_vk_wait_site=%s first_vk_wait_ms_into_frame=%.4f",
                      *firstVkWaitSite ? firstVkWaitSite : "none", firstVkWaitMs);
    std::fprintf(stderr, "%s\n", line);
    std::fflush(stderr);
}
inline void beginFrame() {
    static const bool exitHook = (std::atexit(emitPerfCounters), true);
    (void)exitHook;
    onMainThread = true;
    if (frame) {   // fold the finished frame into the run totals
        for (unsigned f = 0; f < FieldCount; ++f) tot[f] += cur[f].exchange(0);
        const uint64_t n = totFrames.fetch_add(1) + 1;
        if (n == 1 || n % 300 == 0) emitPerfCounters();
    }
    frameStart = Clock::now();
    firstVkWaitSite = ""; firstVkWaitMs = -1.0;
    ++frame; ticks = 0; tickMs = 0;
    cpuLeaves = propLeaves = mechBones = particleRecords = particleSixths = 0;
    nativePropLeaves = nativeMechLeaves = 0;
}
inline unsigned long long u(Field f) { return static_cast<unsigned long long>(cur[f].load()); }
inline void endFrame(double ms) {
    if (output) std::fprintf(output,
        "{\"type\":\"cpu\",\"frame\":%llu,\"ms\":%.6f,\"ticks\":%u,\"tick_total_ms\":%.6f,"
        "\"cpu_leaves\":%llu,\"dynamic_prop_leaves\":%llu,\"mech_bones\":%llu,"
        "\"particle_records\":%llu,\"particle_sixths\":%llu,"
        "\"native_prop_leaves\":%llu,\"native_mech_leaves\":%llu,"
        "\"wait_sealed_ms\":%.6f,\"wait_sim_ms\":%.6f,\"wait_gpu_main_ms\":%.6f,"
        "\"first_vk_wait_site\":\"%s\",\"first_vk_wait_ms_into_frame\":%.6f,"
        "\"extract_bytes\":%llu,\"heap_allocs\":%llu,"
        "\"draws\":{\"props\":%llu,\"terrain\":%llu,\"water\":%llu,\"mech\":%llu,\"legacy\":%llu,\"sky\":%llu,\"vfx\":%llu,\"shadow\":%llu},"
        "\"walk\":{\"visited\":%llu,\"recalc\":%llu,\"ground\":%llu,\"touch\":%llu,\"update\":%llu}}\n",
        static_cast<unsigned long long>(frame), ms, ticks.load(), tickMs.load(),
        static_cast<unsigned long long>(cpuLeaves), static_cast<unsigned long long>(propLeaves),
        static_cast<unsigned long long>(mechBones), static_cast<unsigned long long>(particleRecords),
        static_cast<unsigned long long>(particleSixths),
        static_cast<unsigned long long>(nativePropLeaves), static_cast<unsigned long long>(nativeMechLeaves),
        cur[WaitSealedNs].load() / 1.0e6, cur[WaitSimNs].load() / 1.0e6, cur[WaitGpuMainNs].load() / 1.0e6,
        *firstVkWaitSite ? firstVkWaitSite : "none", firstVkWaitMs,
        u(ExtractBytes), u(HeapAllocs),
        u(DrawProps), u(DrawTerrain), u(DrawWater), u(DrawMech), u(DrawLegacy), u(DrawSky), u(DrawVfx), u(DrawShadow),
        u(WalkVisited), u(WalkRecalc), u(WalkGround), u(WalkTouch), u(WalkUpdate));
}
} // namespace mc2::battlebench
