// hitch_stats.h -- per-frame hitch record inputs. Cheap counters bumped from the engine, read by gos_smoke.cpp::samplePerf, which logs a
// [HITCH v2] line for every frame over twice the running median (diag tag HITCH). Render thread owns every writer except simJoin (main thread);
// all are relaxed atomics, so a stale read only blurs one line.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>

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
    // Slot-wait split (F2): worker WSI acquire and composite fence time (cumulative ns, plus the last acquire), the GPU span/busy of the last four frames
    // read back by gpuprof (MC2_VK_GPU_TIMESTAMPS=1; -1 = no timers), and the other-mc2 process count sampled once per second off the frame threads.
    std::atomic<uint64_t> acquireNs{0}, acquireLastNs{0}, compositeWaitNs{0};
    std::atomic<int64_t> gpuSpanNs[4]{-1, -1, -1, -1}, gpuBusyNs[4]{-1, -1, -1, -1};
    std::atomic<uint32_t> gpuIdx{0};
    std::atomic<int> otherMc2{-1};
};
using WaitKind = int;   // index into Stats::wait
inline constexpr WaitKind kOneShot = 0, kSlot = 1;
inline Stats& stats() { static Stats s; return s; }

struct CauseRecord { const char* name = "none"; double ms = 0; };
struct Causes {
    std::mutex mutex;
    uint64_t frame = 1;
    CauseRecord largest;
};
inline Causes& causes() { static Causes c; return c; }
inline CauseRecord takeCause() {
    auto& c = causes();
    std::lock_guard<std::mutex> lock(c.mutex);
    const auto result = c.largest;
    c.largest = {};
    ++c.frame;
    return result;
}
// Each call site owns its throttle; recording is shared with the frame summary.
struct CauseScope {
    const char* name;
    uint64_t& last;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    CauseScope(const char* n, uint64_t& l) : name(n), last(l) {}
    ~CauseScope() {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (ms <= 8.0) return;
        auto& c = causes();
        std::lock_guard<std::mutex> lock(c.mutex);
        if (ms > c.largest.ms) c.largest = {name, ms};
        if (!last || c.frame - last >= 60) {
            last = c.frame;
            std::fprintf(stderr, "[HITCH_CAUSE v1] zone=%s ms=%.3f\n", name, ms);
        }
    }
};
inline void noteStaged(uint64_t bytes) { stats().stagedBytes.fetch_add(bytes, std::memory_order_relaxed); }
inline uint64_t pendingUploadBytes() { Stats& s = stats(); return s.uploadBytes.load(std::memory_order_relaxed) - s.uploadBytesFlushed.load(std::memory_order_relaxed); }
inline void noteUploadsFlushed() { Stats& s = stats(); s.uploadBytesFlushed.store(s.uploadBytes.load(std::memory_order_relaxed), std::memory_order_relaxed); }
inline void noteAcquire(uint64_t ns) { Stats& s = stats(); s.acquireNs.fetch_add(ns, std::memory_order_relaxed); s.acquireLastNs.store(ns, std::memory_order_relaxed); }
inline void noteGpuFrame(double spanMs, double busyMs) {   // busy = sum of timed scopes (nested scopes overlap, so busy can exceed span)
    Stats& s = stats();
    const uint32_t i = s.gpuIdx.fetch_add(1, std::memory_order_relaxed) & 3u;
    s.gpuSpanNs[i].store(static_cast<int64_t>(spanMs * 1e6), std::memory_order_relaxed);
    s.gpuBusyNs[i].store(static_cast<int64_t>(busyMs * 1e6), std::memory_order_relaxed);
}
// Max GPU span / busy over the last four read-back frames, in ms; -1 when no timestamps were read. A slot wait at frame F waits on frame F-2's GPU work,
// which the read-back ring reports within the next two frames, so the four-frame max covers it.
inline void gpuMax4(double& spanMs, double& busyMs) {
    Stats& s = stats();
    int64_t sp = -1, bu = -1;
    for (int i = 0; i < 4; ++i) { sp = std::max<int64_t>(sp, s.gpuSpanNs[i].load(std::memory_order_relaxed)); bu = std::max<int64_t>(bu, s.gpuBusyNs[i].load(std::memory_order_relaxed)); }
    spanMs = sp < 0 ? -1.0 : sp / 1e6;
    busyMs = bu < 0 ? -1.0 : bu / 1e6;
}
// Names what a slot wait of `ms` may have waited on, claiming only what the evidence carries: gpu_span (a recent frame's timestamp span covers at least
// half the wait) or unknown. The samples are the last four the profiler resolved, not the waited frame (unavailable queries are dropped, the span includes
// gaps, and the island's own submits are outside it), so a short span never proves the GPU idle: no WSI or contention verdict without per-ticket evidence.
inline const char* slotClass(double ms, double spanMs, int /*otherMc2: reported on the line, not a verdict*/) {
    return spanMs >= 0.0 && spanMs >= 0.5 * ms ? "gpu_span" : "unknown";
}

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
    if (ms >= (kind == kOneShot ? 1.0 : 5.0) && s.missionReady.load(std::memory_order_relaxed) && w.logged.fetch_add(1, std::memory_order_relaxed) < 24 && mc2_diag::tagRequested("HITCH")) {
        if (kind != kSlot) {
            std::fprintf(stderr, "[HITCH v2] fenceWait why=%.*s ms=%.2f pending_upload_kb=%llu\n", static_cast<int>(why.size()), why.data(), ms,
                         static_cast<unsigned long long>(pendingUpload / 1024));
        } else {
            double span = -1.0, busy = -1.0;
            gpuMax4(span, busy);
            const int om = s.otherMc2.load(std::memory_order_relaxed);
            std::fprintf(stderr, "[HITCH v2] slotWait why=%.*s ms=%.2f pending_upload_kb=%llu gpu_span4_ms=%.2f gpu_busy4_ms=%.2f acquire_last_ms=%.2f other_mc2=%d class=%s\n",
                         static_cast<int>(why.size()), why.data(), ms, static_cast<unsigned long long>(pendingUpload / 1024), span, busy,
                         s.acquireLastNs.load(std::memory_order_relaxed) / 1e6, om, slotClass(ms, span, om));
        }
    }
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

#define MC2_CAUSE_JOIN_(a, b) a##b
#define MC2_CAUSE_JOIN(a, b) MC2_CAUSE_JOIN_(a, b)
#define MC2_HITCH_CAUSE(name) \
    static thread_local uint64_t MC2_CAUSE_JOIN(causeLast_, __LINE__) = 0; \
    mc2_hitch2::CauseScope MC2_CAUSE_JOIN(cause_, __LINE__)(name, MC2_CAUSE_JOIN(causeLast_, __LINE__))
