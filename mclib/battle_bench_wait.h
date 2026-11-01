// Main-thread wait timing for [PERF_COUNTERS v1]. Render/platform side only: sim code must not include this (wall-clock firewall).
#pragma once
#include <chrono>
#include <string_view>
#include "battle_bench_sink.h"

namespace mc2::battlebench {
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
inline std::string_view firstVkWaitSite = "";
inline double firstVkWaitMs = -1.0;
inline void noteVkWait(std::string_view site, std::chrono::steady_clock::time_point began) {
    if (!onMainThread || firstVkWaitMs >= 0.0) return;
    firstVkWaitSite = site;
    firstVkWaitMs = std::chrono::duration<double, std::milli>(began - frameStart).count();
}
} // namespace mc2::battlebench
