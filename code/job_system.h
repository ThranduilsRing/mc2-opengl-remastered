// code/job_system.h
// MC2-JOBS-CORE-1 — domain-tagged fork/join job system (evolves frame_jobs).
//
// This header adds the new `mc2::jobs` API on top of the proven FRAME-JOBS-1
// epoch-bump / shared-cursor pool. The legacy free-function API
// (parallelForRange / frameJobsInit / ... / g_isFrameJobsWorker) is preserved
// verbatim in frame_jobs.h (a back-compat shim) and implemented in the same TU
// (job_system.cpp) with ZERO behavior change for existing callers.
//
// Render and Sim have independent dispatch state so both can fork concurrently.
// startPool splits its thread budget between the domains; reductions keep canonical block order.
//
// SAFETY: worker bodies must be GL-free AND alloc-free (never touch
// UserHeap/gos_PushCurrentHeap; pre-size containers on main; take transient
// memory from workerArena()). Enforcement is the always-compiled JOBS_FATAL_IF
// (jobs_fatal.h) — never assert (dead in RelWithDebInfo). See plan §1.6/§4.4.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace mc2 { namespace jobs {

// Domain selects partitioning + determinism policy (plan §1.5).
//  Render — order-irrelevant; dynamic chunk-steal is fine.
//  Sim    — determinism-critical; reductions merge in canonical (block-index)
//           order so float sums are bit-identical regardless of worker count.
enum class Domain { Render, Sim };

// ---- Pool lifecycle -------------------------------------------------------
// The engine wires these through frameJobsInit()/Shutdown() (legacy). The
// harness drives them directly to control worker count for the determinism
// proof. startPool(w): w = number of persistent worker threads (main thread
// participates on top, so effective width is w+1). w<=0 => serial-only (no
// threads); parallel_for/parallel_reduce still run correctly, inline on main.
void startPool(int workerCount);
void stopPool();

bool enabled();       // true iff a pool with >=1 worker is running
int  workerCount();   // persistent workers (main participates on top)

// TLS tripwires.
bool isWorkerThread();  // true on fork/join worker threads (was g_isFrameJobsWorker)
bool isPoolThread();    // true on fork/join AND (future) IO threads — heap/alloc guard scope

// Break-even (dispatch+join latency), calibrated by the CORE-1 harness on the
// 8-core target. The per-invocation adaptive fallback uses it so small counts
// never pay the barrier: if count*estCostPerItemNs < kDispatchFloorNs the body
// runs inline on the calling thread with no dispatch.
extern int64_t kDispatchFloorNs;

// Flat fork/join over [0,count); body runs a [begin,end) sub-range.
//   Adaptive fallback: count*estCostPerItemNs < kDispatchFloorNs -> inline.
//   Reentrancy guard: nested dispatch logs-once + runs inline (never deadlock).
void parallel_for(Domain dom, int count, int batch, int estCostPerItemNs,
                  const std::function<void(int begin, int end)>& body);

// Per-block T shard, merged on MAIN in ascending block index. Block boundaries
// depend only on (count, batch) — NOT on worker count — so the merge grouping
// (and therefore a non-associative float sum) is bit-identical for any worker
// count. body(begin,end,shard) accumulates its block into a fresh shard;
// merge(acc,shard) folds shards in canonical order. Same grouping on the serial
// and the dispatched path, so serial==parallel is exact.
template <class T>
T parallel_reduce(Domain dom, int count, int batch, int estCostPerItemNs,
                  T identity,
                  const std::function<void(int begin, int end, T& shard)>& body,
                  const std::function<void(T& acc, const T& shard)>& merge);

// Per-worker bump arena, reset each frame. Workers take transient memory HERE,
// never through UserHeap/gos heap (plan §1.6). Indexed by the current thread's
// participant id (workers 0..N-1, main = N).
struct Arena {
    void* alloc(std::size_t bytes, std::size_t align);
    void  reset();

    // Internals (public so the pool can own the storage); do not use directly.
    unsigned char* base_ = nullptr;
    std::size_t    cap_  = 0;
    std::size_t    used_ = 0;
};
Arena& workerArena();
void    resetAllArenas();  // main-thread, between frames

// Counter-based deterministic RNG for SIM bodies (no shared-LCG draw). Stateless
// hash of (turn, stableKey, salt) — draw ORDER is irrelevant, so parallel
// iteration cannot perturb it. See plan §4.1 rule 1.
std::uint32_t rand_keyed(std::uint32_t turn, std::uint32_t stableKey,
                         std::uint32_t salt);

// --- test/measurement hooks (harness only) ---------------------------------
// Median dispatch+join latency in ns over `iters` no-op dispatches on the
// live pool. This is the number written to kDispatchFloorNs. Requires a pool
// with >=1 worker (else returns 0 — serial has no barrier).
int64_t measureDispatchFloorNs(int iters);

}} // namespace mc2::jobs

// --------------------------------------------------------------------------
// parallel_reduce is a template, so its definition lives here. It delegates the
// dispatch to the non-template parallel_for_blocks() implemented in the .cpp,
// which invokes a per-block callback with the (fixed) block index.
namespace mc2 { namespace jobs { namespace detail {
// Runs `blockBody(blockIndex, begin, end)` over the fixed batch-blocks of
// [0,count). Dispatched across the pool when worthwhile, else inline; block
// boundaries and indices are identical either way. Adaptive fallback +
// reentrancy guard applied inside (same policy as parallel_for).
void parallel_for_blocks(
    Domain dom, int count, int batch, int estCostPerItemNs,
    const std::function<void(int blockIndex, int begin, int end)>& blockBody);
}}} // namespace mc2::jobs::detail

namespace mc2 { namespace jobs {

template <class T>
T parallel_reduce(Domain dom, int count, int batch, int estCostPerItemNs,
                  T identity,
                  const std::function<void(int begin, int end, T& shard)>& body,
                  const std::function<void(T& acc, const T& shard)>& merge) {
    if (count <= 0) return identity;
    batch = std::max(batch, 1);
    const int numBlocks = (count + batch - 1) / batch;

    // Pre-sized on the calling (main) thread — never allocated on a worker.
    std::vector<T> partials(static_cast<std::size_t>(numBlocks), identity);

    detail::parallel_for_blocks(
        dom, count, batch, estCostPerItemNs,
        [&](int blockIndex, int begin, int end) {
            body(begin, end, partials[static_cast<std::size_t>(blockIndex)]);
        });

    // Canonical merge: ascending block index, on main. Fixed grouping => a
    // non-associative float sum is bit-identical across worker counts.
    T acc = identity;
    for (int i = 0; i < numBlocks; ++i)
        merge(acc, partials[static_cast<std::size_t>(i)]);
    return acc;
}

}} // namespace mc2::jobs
