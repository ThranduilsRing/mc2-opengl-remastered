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
// Any-thread scope for the present tail (the worker, or main when the worker is off). Optionally records the [t0,t1] interval of a worker GPU/WSI wait.
inline std::atomic<int64_t> workerWaitNs[4]{};   // steady_clock ns: acquire t0,t1, composite-wait t0,t1 (last frame written)
struct NsScope {
    Field field;
    int interval;   // -1 none, 0 acquire, 1 composite wait
    std::chrono::steady_clock::time_point t0;
    explicit NsScope(Field f, int iv = -1) : field(f), interval(iv), t0(std::chrono::steady_clock::now()) {}
    ~NsScope() {
        const auto t1 = std::chrono::steady_clock::now();
        add(field, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
        if (interval >= 0) {
            workerWaitNs[interval * 2].store(t0.time_since_epoch().count(), std::memory_order_relaxed);
            workerWaitNs[interval * 2 + 1].store(t1.time_since_epoch().count(), std::memory_order_relaxed);
        }
    }
};
inline double msIntoFrame(int64_t ns) {
    return ns ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(ns)) - frameStart).count() : 0.0;
}
} // namespace mc2::battlebench
