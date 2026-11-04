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
    // One ledger for every CPU wait on the GPU that is not a device idle. kOneShot = synchronous one-shot submits (submitOneShot, submitAndWait, the water
    // one-shot); kSlot = per-frame throttles (VulkanContext::wait when it blocks, the live shadow fence, the color-prop late fence, the present composite).
    // `behind` = the part of a one-shot wait spent with upload bytes still queued ahead of it (VulkanContext::submit flushes the batch first).
    struct WaitLedger {
        std::atomic<uint64_t> count{0}, ns{0}, maxNs{0}, windowMaxNs{0}, behindCount{0}, behindNs{0};
        std::atomic<int> logged{0};
    };
    WaitLedger wait[2];
    std::atomic<uint64_t> stagedBytes{0};          // bytes written into explicit staging buffers (createFilled with TRANSFER_SRC, UploadBatch dedicated); ring bytes are uploadBytes
    std::atomic<uint64_t> uploadBytesFlushed{0};   // uploadBytes at the last UploadBatch flush; uploadBytes - this = bytes queued ahead of the next submit
};
enum WaitKind : int { kOneShot = 0, kSlot = 1 };
inline Stats& stats() { static Stats s; return s; }
inline void noteStaged(uint64_t bytes) { stats().stagedBytes.fetch_add(bytes, std::memory_order_relaxed); }
inline uint64_t pendingUploadBytes() { Stats& s = stats(); return s.uploadBytes.load(std::memory_order_relaxed) - s.uploadBytesFlushed.load(std::memory_order_relaxed); }
inline void noteUploadsFlushed() { Stats& s = stats(); s.uploadBytesFlushed.store(s.uploadBytes.load(std::memory_order_relaxed), std::memory_order_relaxed); }

// A point in time for window math: the window is "now minus this".
struct Snapshot {
    uint64_t waitN[2]{}, waitNs[2]{}, behindNs{}, staged{}, idleN{}, idleNs{};
};
inline Snapshot takeSnapshot() {
    Stats& s = stats();
    Snapshot o;
    for (int k = 0; k < 2; ++k) { o.waitN[k] = s.wait[k].count.load(); o.waitNs[k] = s.wait[k].ns.load(); }
    o.behindNs = s.wait[kOneShot].behindNs.load();
    o.staged = s.stagedBytes.load();
    o.idleN = s.waitIdleCount.load();
    o.idleNs = s.waitIdleNs.load();
    return o;
}
// Starts a new max window: the per-kind window max is what a "first 60 frames" line reports; the lifetime max stays in maxNs.
inline void beginWaitWindow() { for (auto& w : stats().wait) w.windowMaxNs.store(0, std::memory_order_relaxed); }
// A wait inside an open one-shot FenceWaitScope is counted by that scope, not again by VulkanContext::wait.
inline bool& inOneShotScope() { thread_local bool b = false; return b; }
// Records one wait. `why` is the caller tag; the first 24 one-shot waits over 1 ms (slot waits: over 5 ms) after mission_ready name themselves in the log.
inline void noteWait(WaitKind kind, std::string_view why, double ms, uint64_t pendingUpload = 0) {
    Stats& s = stats();
    if (!(ms >= 0.0)) return;   // clock fault or NaN
    if (kind == kSlot && ms < 0.1) return;   // an already-signalled slot fence returns in microseconds; that is not a stall
    Stats::WaitLedger& w = s.wait[kind];
    const uint64_t ns = static_cast<uint64_t>(ms * 1e6);
    w.count.fetch_add(1, std::memory_order_relaxed);
    w.ns.fetch_add(ns, std::memory_order_relaxed);
    if (pendingUpload) { w.behindCount.fetch_add(1, std::memory_order_relaxed); w.behindNs.fetch_add(ns, std::memory_order_relaxed); }
    for (std::atomic<uint64_t>* m : {&w.maxNs, &w.windowMaxNs}) {
        uint64_t prev = m->load(std::memory_order_relaxed);
        while (ns > prev && !m->compare_exchange_weak(prev, ns, std::memory_order_relaxed)) {}
    }
    if (ms >= (kind == kOneShot ? 1.0 : 5.0) && s.missionReady.load(std::memory_order_relaxed) && w.logged.fetch_add(1, std::memory_order_relaxed) < 24 && mc2_diag::tagRequested("HITCH"))
        std::fprintf(stderr, "[HITCH v2] %sWait why=%.*s ms=%.2f pending_upload_kb=%llu\n", kind == kOneShot ? "fence" : "slot", static_cast<int>(why.size()), why.data(), ms,
                     static_cast<unsigned long long>(pendingUpload / 1024));
}
// Times its scope and records it as one wait. A nested one-shot scope on the same thread records nothing (the outer one covers it).
struct FenceWaitScope {
    std::string_view why;
    WaitKind kind;
    uint64_t pending;
    bool active;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    explicit FenceWaitScope(std::string_view w, WaitKind k = kOneShot, uint64_t pendingUpload = 0)
        : why(w), kind(k), pending(pendingUpload), active(k != kOneShot || !inOneShotScope()) { if (active && k == kOneShot) inOneShotScope() = true; }
    ~FenceWaitScope() {
        if (!active) return;
        if (kind == kOneShot) inOneShotScope() = false;
        noteWait(kind, why, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), pending);
    }
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
