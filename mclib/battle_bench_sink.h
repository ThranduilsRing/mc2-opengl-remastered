// Instrumentation sink shared with simulation; no renderer or time-source dependency.
#pragma once
#include <cstdio>
#include <atomic>
#include <chrono>
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
inline uint64_t nativePropLeaves = 0, nativeMechLeaves = 0;
inline uint64_t particleRecords = 0, particleSixths = 0;
inline uint64_t (*beginTick)() = nullptr;
inline void (*endTick)(uint64_t) = nullptr;
struct Tick {
    uint64_t sample = beginTick ? beginTick() : 0;
    ~Tick() { if (sample && endTick) endTick(sample); }
};

// [PERF_COUNTERS v1]: per-frame counters, always on (relaxed atomic adds; no env gate, no behaviour change).
// `cur` holds this frame; beginFrame folds it into `tot` (see battle_bench.h). Wait fields are nanoseconds, main thread only.
enum Field : unsigned {
    DrawProps, DrawTerrain, DrawWater, DrawMech, DrawLegacy, DrawSky, DrawVfx, DrawShadow,
    WaitSealedNs, WaitSimNs, WaitGpuMainNs,
    ExtractBytes, HeapAllocs,
    WalkVisited, WalkRecalc, WalkGround, WalkTouch, WalkUpdate,
    FieldCount
};
inline constexpr const char* kFieldNames[FieldCount] = {
    "draws.props", "draws.terrain", "draws.water", "draws.mech", "draws.legacy", "draws.sky", "draws.vfx", "draws.shadow",
    "wait_sealed_ns", "wait_sim_ns", "wait_gpu_main_ns",
    "extract.bytes", "heap_allocs",
    "walk.visited", "walk.recalc", "walk.ground", "walk.touch", "walk.update"};
inline std::atomic<uint64_t> cur[FieldCount]{};
inline std::atomic<uint64_t> tot[FieldCount]{};
inline std::atomic<uint64_t> totFrames{0};
inline thread_local bool onMainThread = false;   // set by beginFrame (called on main)
inline void add(Field f, uint64_t n = 1) { cur[f].fetch_add(n, std::memory_order_relaxed); }
// Block-time scope: charges steady_clock time to a wait field, main thread only.
struct WaitScope {
    Field field;
    std::chrono::steady_clock::time_point t0;
    bool on;
    explicit WaitScope(Field f) : field(f), on(onMainThread) { if (on) t0 = std::chrono::steady_clock::now(); }
    ~WaitScope() {
        if (on) add(field, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - t0).count()));
    }
};
// First blocking vk-context/present wait of the frame: caller label and ms since beginFrame (main thread only).
inline std::chrono::steady_clock::time_point frameStart{};
inline const char* firstVkWaitSite = "";
inline double firstVkWaitMs = -1.0;
inline void noteVkWait(const char* site, std::chrono::steady_clock::time_point began) {
    if (!onMainThread || firstVkWaitMs >= 0.0) return;
    firstVkWaitSite = site;
    firstVkWaitMs = std::chrono::duration<double, std::milli>(began - frameStart).count();
}
// Movers alive and moving or firing, for the bench `world` row; written by the bench fixture.
inline int activeMovers = 0;
} // namespace mc2::battlebench
