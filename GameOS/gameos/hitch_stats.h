// hitch_stats.h -- per-frame hitch record inputs. Cheap counters bumped from the engine, read by gos_smoke.cpp::samplePerf, which logs a
// [HITCH v2] line for every frame over twice the running median (diag tag HITCH). Render thread owns every writer except simJoin (main thread);
// all are relaxed atomics, so a stale read only blurs one line.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

#include "diagnostic_trace.h"
#include <string_view>

namespace mc2_hitch2 {

struct Stats {
    std::atomic<uint64_t> waitIdleCount{0}, waitIdleNs{0}, pipelineCreates{0}, uploadBytes{0}, objMapRecords{0}, dirtyBatches{0}, simJoinNs{0};
    std::atomic<const char*> bakeState{"none"};      // cold | warm | idle | object | dirty (set by ShadowBake)
    std::atomic<const char*> bakeCache{"unknown"};   // hit | miss (the content-key cache read)
    std::atomic<bool> missionReady{false};
    std::atomic<int> waitIdleLogged{0};
    // Direct fence/timeline waits that bypass VulkanContext::waitIdle (one-shot submits, submitAndWait, the water one-shot, the image-resource frame
    // fence), and every TRANSFER_SRC buffer created through OwnedBuffer (UploadBatch's dedicated buffers included; its ring bytes are uploadBytes).
    std::atomic<uint64_t> fenceWaitCount{0}, fenceWaitNs{0}, fenceWaitMaxNs{0}, stagedBytes{0};
    std::atomic<int> fenceWaitLogged{0};
};
inline Stats& stats() { static Stats s; return s; }
inline void noteStaged(uint64_t bytes) { stats().stagedBytes.fetch_add(bytes, std::memory_order_relaxed); }
// Records one direct wait. `why` is the caller tag; the first 24 waits over 1 ms after mission_ready name themselves in the log.
inline void noteFenceWait(std::string_view why, double ms) {
    Stats& s = stats();
    if (!(ms >= 0.0)) return;   // clock fault or NaN
    const uint64_t ns = static_cast<uint64_t>(ms * 1e6);
    s.fenceWaitCount.fetch_add(1, std::memory_order_relaxed);
    s.fenceWaitNs.fetch_add(ns, std::memory_order_relaxed);
    uint64_t prev = s.fenceWaitMaxNs.load(std::memory_order_relaxed);
    while (ns > prev && !s.fenceWaitMaxNs.compare_exchange_weak(prev, ns, std::memory_order_relaxed)) {}
    if (ms >= 1.0 && s.missionReady.load(std::memory_order_relaxed) && s.fenceWaitLogged.fetch_add(1, std::memory_order_relaxed) < 24 && mc2_diag::tagRequested("HITCH"))
        std::fprintf(stderr, "[HITCH v2] fenceWait why=%.*s ms=%.2f\n", static_cast<int>(why.size()), why.data(), ms);
}
// Times its scope and records it as one direct wait.
struct FenceWaitScope {
    std::string_view why;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    explicit FenceWaitScope(std::string_view w) : why(w) {}
    ~FenceWaitScope() { noteFenceWait(why, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()); }
};
inline void noteBake(std::string_view state, int dirtyBatch, int objRecord, std::string_view cache = {}) {   // literals only (data() is stored)
    Stats& s = stats();
    if (!state.empty()) s.bakeState.store(state.data());
    if (!cache.empty()) s.bakeCache.store(cache.data());
    s.dirtyBatches.fetch_add(static_cast<uint64_t>(dirtyBatch), std::memory_order_relaxed);
    s.objMapRecords.fetch_add(static_cast<uint64_t>(objRecord), std::memory_order_relaxed);
}

// A coarse named scope for the per-frame "worst scopes" list (one steady_clock pair, ~40 ns; keep it at region level, never per element).
struct ScopeTable {
    struct Slot { std::string_view name; double ms; };
    static constexpr int kMax = 24;
    Slot slots[kMax];
    int n = 0;
    void add(std::string_view name, double ms) {
        for (int i = 0; i < n; ++i) if (slots[i].name == name) { slots[i].ms += ms; return; }
        if (n < kMax) slots[n++] = {name, ms};
    }
};
inline ScopeTable& scopes() { static ScopeTable t; return t; }   // render thread only

struct Scope {
    std::string_view name;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    explicit Scope(std::string_view n) : name(n) {}
    ~Scope() { scopes().add(name, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()); }
};

}  // namespace mc2_hitch2
