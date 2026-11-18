// Buffered frame evidence, armed only by the test command channel.
#pragma once
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <string_view>
#include "../../mclib/battle_bench_wait.h"
#include "diagnostic_trace.h"

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
    if (!mc2_diag::tagRequested("PERF_COUNTERS")) return;   // quiet by default; counters above are still collected (MC2_DIAG_TAGS=PERF_COUNTERS prints)
    char line[4096];
    int len = std::snprintf(line, sizeof line, "[PERF_COUNTERS v1] frames=%llu", static_cast<unsigned long long>(n));
    for (unsigned f = 0; f < FieldCount && len > 0 && len < static_cast<int>(sizeof line); ++f) {
        const auto fe = static_cast<Field>(f);
        const bool ns = fe == WaitSealedNs || fe == WaitSimNs || fe == WaitGpuMainNs || fe >= PresAcquireNs;
        const double mean = static_cast<double>(tot[f].load()) / static_cast<double>(n) / (ns ? 1.0e6 : 1.0);
        std::string name = kFieldNames[f];
        if (ns) { name.resize(name.size() - 2); name += "ms"; }
        len += std::snprintf(line + len, sizeof line - len, " %s=%.4f", name.c_str(), mean);
    }
    if (len > 0 && len < static_cast<int>(sizeof line))
        std::snprintf(line + len, sizeof line - len, " first_vk_wait_site=%s first_vk_wait_ms_into_frame=%.4f",
                      firstVkWaitSite.empty() ? "none" : firstVkWaitSite.data(), firstVkWaitMs);
    std::fprintf(stderr, "%s\n", line);
    std::fflush(stderr);
}
// [PERF_WINDOW v1]: draw/extract/heap counters over only the frames that began inside the fixed sim-tick window; printed once when it closes
// (or at exit). complete=1 only if the run reached kWindowEnd. Always printed: the smoke gate reads it, unlike the tag-gated run-mean line.
inline void emitPerfWindow() {
    const uint64_t n = winFrames.load();
    if (windowEmitted || !n) return;
    windowEmitted = true;
    char line[2048];
    int len = std::snprintf(line, sizeof line, "[PERF_WINDOW v1] ticks=%llu..%llu complete=%d frames=%llu",
                            static_cast<unsigned long long>(kWindowBegin), static_cast<unsigned long long>(kWindowEnd),
                            simTicks.load() >= kWindowEnd ? 1 : 0, static_cast<unsigned long long>(n));
    for (unsigned f = 0; f < FieldCount && len > 0 && len < static_cast<int>(sizeof line); ++f) {
        const auto fe = static_cast<Field>(f);
        if (fe >= WaitSealedNs && fe != ExtractBytes && fe != HeapAllocs) continue;   // counts only; timings stay in the run-mean line
        len += std::snprintf(line + len, sizeof line - len, " %s=%.4f", kFieldNames[f], static_cast<double>(winTot[f].load()) / static_cast<double>(n));
    }
    std::fprintf(stderr, "%s\n", line);
    std::fflush(stderr);
}
inline void beginFrame() {
    static const bool exitHook = (std::atexit(emitPerfCounters), std::atexit(emitPerfWindow), true);
    (void)exitHook;
    onMainThread = true;
    if (frame) {   // fold the finished frame into the run totals
        for (unsigned f = 0; f < FieldCount; ++f) {
            const uint64_t v = cur[f].exchange(0);
            tot[f] += v;
            if (curInWindow) winTot[f] += v;
        }
        if (curInWindow) ++winFrames;
        const uint64_t n = totFrames.fetch_add(1) + 1;
        if (n == 1 || n % 300 == 0) emitPerfCounters();
    }
    const uint64_t st = simTicks.load();
    curInWindow = st >= kWindowBegin && st < kWindowEnd;
    if (st >= kWindowEnd) emitPerfWindow();
    frameStart = Clock::now();
    firstVkWaitSite = ""; firstVkWaitMs = -1.0;
    ++frame; ticks = 0; tickMs = 0;
    cpuLeaves = propLeaves = mechBones = particleRecords = particleSixths = 0;
    nativePropLeaves = nativeMechLeaves = 0;
}
inline unsigned long long u(Field f) { return static_cast<unsigned long long>(cur[std::to_underlying(f)].load()); }
inline void endFrame(double ms) {
    if (output) std::fprintf(output,
        "{\"type\":\"cpu\",\"frame\":%llu,\"ms\":%.6f,\"ticks\":%u,\"tick_total_ms\":%.6f,"
        "\"cpu_leaves\":%llu,\"dynamic_prop_leaves\":%llu,\"mech_bones\":%llu,"
        "\"particle_records\":%llu,\"particle_sixths\":%llu,"
        "\"native_prop_leaves\":%llu,\"native_mech_leaves\":%llu,"
        "\"wait_sealed_ms\":%.6f,\"wait_sim_ms\":%.6f,\"wait_gpu_main_ms\":%.6f,"
        "\"first_vk_wait_site\":\"%s\",\"first_vk_wait_ms_into_frame\":%.6f,"
        "\"extract_bytes\":%llu,\"heap_allocs\":%llu,"
        "\"draws\":{\"props\":%llu,\"terrain\":%llu,\"water\":%llu,\"mech_depth\":%llu,\"mech_color\":%llu,\"legacy\":%llu,\"sky\":%llu,\"vfx\":%llu,\"shadow\":%llu},"
        "\"walk\":{\"visited\":%llu,\"recalc\":%llu,\"ground\":%llu,\"touch\":%llu,\"update\":%llu,\"pres_ms\":%.6f,\"extract_ms\":%.6f,\"render_ms\":%.6f},"
        "\"dirty\":{\"dynamic\":%llu,\"falling\":%llu,\"frame_lights\":%llu,\"light_bake\":%llu,\"effects\":%llu},"
        "\"movers\":{\"in_view\":%llu,\"offscreen\":%llu},"
        "\"pres\":{\"acquire_ms\":%.6f,\"tail_ms\":%.6f,\"hud_ms\":%.6f,\"imgui_ms\":%.6f,\"blit_ms\":%.6f,\"submit_ms\":%.6f,"
        "\"collect_ms\":%.6f,\"boundary_ms\":%.6f,\"composite_wait_ms\":%.6f,\"queue_present_ms\":%.6f,"
        "\"acquire_t0\":%.4f,\"acquire_t1\":%.4f,\"cwait_t0\":%.4f,\"cwait_t1\":%.4f}}\n",
        static_cast<unsigned long long>(frame), ms, ticks.load(), tickMs.load(),
        static_cast<unsigned long long>(cpuLeaves), static_cast<unsigned long long>(propLeaves),
        static_cast<unsigned long long>(mechBones), static_cast<unsigned long long>(particleRecords),
        static_cast<unsigned long long>(particleSixths),
        static_cast<unsigned long long>(nativePropLeaves), static_cast<unsigned long long>(nativeMechLeaves),
        cur[std::to_underlying(WaitSealedNs)].load() / 1.0e6, cur[std::to_underlying(WaitSimNs)].load() / 1.0e6, cur[std::to_underlying(WaitGpuMainNs)].load() / 1.0e6,
        firstVkWaitSite.empty() ? "none" : firstVkWaitSite.data(), firstVkWaitMs,
        u(ExtractBytes), u(HeapAllocs),
        u(DrawProps), u(DrawTerrain), u(DrawWater), u(DrawMechDepth), u(DrawMechColor), u(DrawLegacy), u(DrawSky), u(DrawVfx), u(DrawShadow),
        u(WalkVisited), u(WalkRecalc), u(WalkGround), u(WalkTouch), u(WalkUpdate),
        u(WalkPresNs) / 1.0e6, u(WalkExtractNs) / 1.0e6, u(WalkRenderNs) / 1.0e6,
        u(WalkDirtyDynamic), u(WalkDirtyFalling), u(WalkDirtyFrameLights), u(WalkDirtyLightBake), u(WalkDirtyEffects),
        u(MoversInView), u(MoversOffscreen),
        u(PresAcquireNs) / 1.0e6, u(PresTailNs) / 1.0e6, u(PresTailHudNs) / 1.0e6, u(PresTailImguiNs) / 1.0e6, u(PresTailBlitNs) / 1.0e6,
        u(PresTailSubmitNs) / 1.0e6, u(PresCollectNs) / 1.0e6, u(PresBoundaryNs) / 1.0e6, u(PresCompositeWaitNs) / 1.0e6, u(PresQueuePresentNs) / 1.0e6,
        msIntoFrame(workerWaitNs[0].load(std::memory_order_relaxed)), msIntoFrame(workerWaitNs[1].load(std::memory_order_relaxed)),
        msIntoFrame(workerWaitNs[2].load(std::memory_order_relaxed)), msIntoFrame(workerWaitNs[3].load(std::memory_order_relaxed)));
}
} // namespace mc2::battlebench
