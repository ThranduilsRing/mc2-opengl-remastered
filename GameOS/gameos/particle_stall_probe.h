// PARTICLE-FLUSH-STALL-MEASURE-1 — gated CPU-timing probe for particle/tube flush.
//
// MEASURE, NOT MUTATE. Header-only, all-inline, gated by MC2_PARTICLE_FLUSH_STALL_TRACE
// (default OFF) — byte-for-byte zero cost when off. Adds NO barrier, NO buffer-lifetime
// change, NO ring/readback, NO rendering change. It only times the CPU wall-clock of the
// particle/tube flush bodies so we can decide whether the class-D implicit-sync
// glBufferSubData uploads in those paths actually STALL (a stall surfaces as a max_us
// spike far above avg_us; pure migration debt shows max_us ~= avg_us, both small).
//
//   [PARTICLE_FLUSH_STALL] path=billboard calls=300 avg_us=12.4 max_us=18.0 (max~avg: no stall)
//   [PARTICLE_FLUSH_STALL] path=tube      calls=300 avg_us=9.1  max_us=2100.0 (SPIKE: implicit-sync stall)
#pragma once

#include "mc2_czstring.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "Gates/gate.h"

namespace mc2 {

inline bool particleStallTraceEnabled() {
    static const bool s = false;
    return s;
}

// Per-path accumulator. Index 0=billboard, 1=tube, 2=tube_deferred.
struct ParticleStallAccum {
    unsigned long long calls = 0;
    double total_us = 0.0;
    double max_us   = 0.0;
};

inline ParticleStallAccum& particleStallAccum(int idx) {
    static ParticleStallAccum s_acc[3];
    return s_acc[idx < 0 || idx > 2 ? 0 : idx];
}

inline void particleStallRecord(int idx, mc2::czstring path, double us) {
    ParticleStallAccum& a = particleStallAccum(idx);
    a.calls++;
    a.total_us += us;
    a.max_us = std::max(a.max_us, us);
    // Periodic summary so logs aren't per-flush noisy. One early one-shot at 10 (so
    // low-activity FX paths still report at least once), then every 50 timed flushes.
    if (a.calls == 10ull || (a.calls % 50ull) == 0ull) {
        const double avg = a.total_us / static_cast<double>(a.calls);
        std::fprintf(stderr,
            "[PARTICLE_FLUSH_STALL] path=%s calls=%llu avg_us=%.1f max_us=%.1f%s\n",
            path, a.calls, avg, a.max_us,
            (a.max_us > avg * 8.0 && a.max_us > 500.0) ? " (SPIKE: possible implicit-sync stall)"
                                                       : " (max~avg: no stall)");
        std::fflush(stderr);
    }
}

// RAII flush timer. idx selects the accumulator; path is the label. Cheap: one getenv
// bool check; if disabled the clock reads are skipped entirely.
struct ScopedFlushTimer {
    int idx_; const char* path_; bool on_;
    std::chrono::steady_clock::time_point t0_;
    ScopedFlushTimer(int idx, mc2::czstring path)
        : idx_(idx), path_(path), on_(particleStallTraceEnabled()) {
        if (on_) t0_ = std::chrono::steady_clock::now();
    }
    ~ScopedFlushTimer() {
        if (!on_) return;
        const auto t1 = std::chrono::steady_clock::now();
        const double us =
            std::chrono::duration<double, std::micro>(t1 - t0_).count();
        particleStallRecord(idx_, path_, us);
    }
};

}  // namespace mc2
