// Instrumentation sink shared with simulation; no renderer or time-source dependency.
#pragma once
#include <cstdio>
#include <atomic>
#include <cstdint>
#include <utility>

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
// Sim ticks started since process start (all threads); the fixed measurement window is a range of these.
inline std::atomic<uint64_t> simTicks{0};
// [PERF_WINDOW v1]: per-frame means over frames that START with simTicks in [kWindowBegin, kWindowEnd), independent of run length.
inline constexpr uint64_t kWindowBegin = 30, kWindowEnd = 120;
struct Tick {
    uint64_t sample = (simTicks.fetch_add(1, std::memory_order_relaxed), beginTick ? beginTick() : 0);
    ~Tick() { if (sample && endTick) endTick(sample); }
};

// [PERF_COUNTERS v1]: per-frame counters, always on (relaxed atomic adds; no env gate, no behaviour change).
// `cur` holds this frame; beginFrame folds it into `tot` (see battle_bench.h). Wait fields are nanoseconds, main thread only.
enum class Field : unsigned {
    DrawProps, DrawTerrain, DrawWater, DrawMechDepth, DrawMechColor, DrawLegacy, DrawSky, DrawVfx, DrawShadow,
    // Local calls overlap mover/static calls; records retain the pre-batch draw census.
    DrawShadowMover, DrawShadowLocal, ShadowMoverRecords, ShadowLocalMoverRecords,
    WaitSealedNs, WaitSimNs, WaitGpuMainNs,
    ExtractBytes, HeapAllocs,
    WalkVisited, WalkRecalc, WalkGround, WalkTouch, WalkUpdate,
    // Present tail, written by the present worker (or main when the worker is off). Nanoseconds; all end in "_ns".
    PresAcquireNs, PresTailNs, PresTailHudNs, PresTailImguiNs, PresTailBlitNs, PresTailSubmitNs,
    PresCollectNs, PresBoundaryNs, PresCompositeWaitNs, PresQueuePresentNs,
    // Terrain-object walks on main (D2 census): presentation walk, extract terrain list, render walk. Nanoseconds.
    WalkPresNs, WalkExtractNs, WalkRenderNs,
    NumFields
};
using enum Field;
inline constexpr unsigned FieldCount = static_cast<unsigned>(Field::NumFields);
inline constexpr const char* kFieldNames[FieldCount] = {
    "draws.props", "draws.terrain", "draws.water", "draws.mech_depth", "draws.mech_color", "draws.legacy", "draws.sky", "draws.vfx", "draws.shadow",
    "draws.shadow_mover", "draws.shadow_local", "shadow.mover_records", "shadow.local_mover_records",
    "wait_sealed_ns", "wait_sim_ns", "wait_gpu_main_ns",
    "extract.bytes", "heap_allocs",
    "walk.visited", "walk.recalc", "walk.ground", "walk.touch", "walk.update",
    "pres.acquire_ns", "pres.tail_ns", "pres.tail_hud_ns", "pres.tail_imgui_ns", "pres.tail_blit_ns", "pres.tail_submit_ns",
    "pres.collect_ns", "pres.boundary_ns", "pres.composite_wait_ns", "pres.queue_present_ns",
    "walk.pres_ns", "walk.extract_ns", "walk.render_ns"};
inline std::atomic<uint64_t> cur[FieldCount]{};
inline std::atomic<uint64_t> tot[FieldCount]{};
inline std::atomic<uint64_t> totFrames{0};
inline std::atomic<uint64_t> winTot[FieldCount]{};
inline std::atomic<uint64_t> winFrames{0};
inline bool curInWindow = false, windowEmitted = false;   // main thread only
inline thread_local bool onMainThread = false;   // set by beginFrame (called on main)
inline void add(Field f, uint64_t n = 1) { cur[std::to_underlying(f)].fetch_add(n, std::memory_order_relaxed); }
// Movers alive and moving or firing, for the bench `world` row; written by the bench fixture.
inline int activeMovers = 0;
} // namespace mc2::battlebench
