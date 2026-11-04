// hitch_stats.h -- per-frame hitch record inputs. Cheap counters bumped from the engine, read by gos_smoke.cpp::samplePerf, which logs a
// [HITCH v2] line for every frame over twice the running median (diag tag HITCH). Render thread owns every writer except simJoin (main thread);
// all are relaxed atomics, so a stale read only blurs one line.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>

namespace mc2_hitch2 {

struct Stats {
    std::atomic<uint64_t> waitIdleCount{0}, waitIdleNs{0}, pipelineCreates{0}, uploadBytes{0}, objMapRecords{0}, dirtyBatches{0}, simJoinNs{0};
    std::atomic<const char*> bakeState{"none"};      // cold | warm | idle | object | dirty (set by ShadowBake)
    std::atomic<const char*> bakeCache{"unknown"};   // hit | miss (the content-key cache read)
    std::atomic<bool> missionReady{false};
    std::atomic<int> waitIdleLogged{0};
};
inline Stats& stats() { static Stats s; return s; }

// A coarse named scope for the per-frame "worst scopes" list (one steady_clock pair, ~40 ns; keep it at region level, never per element).
struct ScopeTable {
    struct Slot { const char* name; double ms; };
    static constexpr int kMax = 24;
    Slot slots[kMax];
    int n = 0;
    void add(const char* name, double ms) {
        for (int i = 0; i < n; ++i) if (slots[i].name == name) { slots[i].ms += ms; return; }
        if (n < kMax) slots[n++] = {name, ms};
    }
};
inline ScopeTable& scopes() { static ScopeTable t; return t; }   // render thread only

struct Scope {
    const char* name;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    explicit Scope(const char* n) : name(n) {}
    ~Scope() { scopes().add(name, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()); }
};

}  // namespace mc2_hitch2
