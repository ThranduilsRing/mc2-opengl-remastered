// code/job_system.cpp
// MC2-JOBS-CORE-1 — implementation of both the legacy FRAME-JOBS-1 free-function
// API (parallelForRange / frameJobsInit / ... — evolved verbatim from the old
// code/frame_jobs.cpp, ZERO behavior change) and the new domain-tagged
// `mc2::jobs` API. The new API is DORMANT: no engine caller, exercised only by
// tools/job_system_harness.
//
// Both share the same proven epoch-bump / shared-cursor fork/join idiom. The
// legacy pool is created by frameJobsInit() (gate MC2_FRAME_JOBS); the new pool
// is created only by mc2::jobs::startPool() (harness / future adoption wiring).
#include "frame_jobs.h"
#include "job_system.h"
#include "jobs_fatal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>
#include "Gates/gate.h"
#include "mc2fmt.h"
#ifdef TRACY_ENABLE
#include <string>
#include <tracy/Tracy.hpp>
#define MC2_JOBS_THREAD_NAME(prefix, id) tracy::SetThreadName((std::string(prefix) + std::to_string(id)).c_str())
#define MC2_JOBS_ZONE(name) ZoneScopedN(name)
#else
#define MC2_JOBS_THREAD_NAME(prefix, id) ((void)0)
#define MC2_JOBS_ZONE(name) ((void)0)
#endif

// ===========================================================================
//  LEGACY FRAME-JOBS-1 API  (verbatim behavior — do not change the OFF path)
// ===========================================================================

// FRAME-JOBS-2D: thread-local flag — set true on workers during touchWorkerPrepass.
thread_local bool g_isFrameJobsWorker = false;

namespace {

struct Chunk { int begin, end; };

struct Pool {
    std::vector<Chunk>            chunks;
    std::function<void(int,int)>  fn;
    std::atomic<int>              nextChunk{0};
    int                           totalChunks{0};

    std::mutex                    mu;
    std::condition_variable       cvWork;
    std::condition_variable       cvDone;
    int                           epoch{0};
    std::vector<int>              workerEpoch;
    std::atomic<int>              workersDone{0};
    int                           numWorkers{0};
    bool                          quit{false};

    std::vector<std::thread>      threads;
};

Pool* g_pool      = nullptr;
bool  g_enabled   = false;
bool  g_touchEnabled = false;
int   g_batch     = 64;
bool  g_trace     = false;

std::atomic<int> g_chunksThisFrame{0};
std::atomic<int> g_workersCount{0};
bool             g_serialFallback = false;

void workerFn(Pool* pool, int workerId) {
    MC2_JOBS_THREAD_NAME("FrameJobs ", workerId);
    while (true) {
        {
            std::unique_lock<std::mutex> lk(pool->mu);
            pool->cvWork.wait(lk, [&] {
                return pool->quit || pool->workerEpoch[workerId] != pool->epoch;
            });
            if (pool->quit) return;
            pool->workerEpoch[workerId] = pool->epoch;
        }

        {
            MC2_JOBS_ZONE("FrameJobs.Worker");
            while (true) {
                int idx = pool->nextChunk.fetch_add(1, std::memory_order_relaxed);
                if (idx >= pool->totalChunks) break;
                pool->fn(pool->chunks[idx].begin, pool->chunks[idx].end);
                g_chunksThisFrame.fetch_add(1, std::memory_order_relaxed);
            }
        }

        int done = pool->workersDone.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (done == pool->numWorkers) {
            std::lock_guard<std::mutex> lk(pool->mu);
            pool->cvDone.notify_one();
        }
    }
}

} // namespace

static void startSimPool() {
    // Sim batches (LOS rays) publish in original order, so the result never depends on the width. Default: up to 3
    // workers (the host budget), never more than hardware allows.
    const int hw = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    int nw = std::min(3, hw - 1);
    if (mc2::gate::isSet<"MC2_FRAME_JOBS_WORKERS">())
        nw = std::clamp(mc2::gate::get<"MC2_FRAME_JOBS_WORKERS">(), 1, hw);
    mc2::jobs::startPool(nw);
}

void frameJobsInit() {
    startSimPool();
    g_enabled = mc2::gate::get<"MC2_FRAME_JOBS">();

    if (mc2::gate::isSet<"MC2_FRAME_JOBS_BATCH">())
        g_batch = std::max(1, mc2::gate::get<"MC2_FRAME_JOBS_BATCH">());
    g_trace = mc2::gate::get<"MC2_FRAME_JOBS_TRACE">();

    g_touchEnabled = g_enabled && mc2::gate::get<"MC2_FRAME_JOBS_TOUCH">();

    if (!g_enabled) return;

    int hw = static_cast<int>(std::thread::hardware_concurrency());
    int nw = (hw > 1) ? (hw - 1) : 1;
    if (mc2::gate::isSet<"MC2_FRAME_JOBS_WORKERS">())
        nw = mc2::gate::get<"MC2_FRAME_JOBS_WORKERS">();
    const int hwMax = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    nw = std::clamp(nw, 1, hwMax);

    if (nw <= 1) { g_enabled = false; return; } // serial fallback

    g_pool = new Pool();
    g_pool->numWorkers = nw;
    // Startup must not count as a completed dispatch.
    g_pool->workerEpoch.assign(nw, g_pool->epoch);
    for (int i = 0; i < nw; ++i)
        g_pool->threads.emplace_back(workerFn, g_pool, i);

    g_workersCount.store(nw, std::memory_order_relaxed);

    if (g_touchEnabled && g_trace) {
        mc2::fmt::print_out("FRAME_JOBS_TOUCH: enabled workers={:d} batch={:d}\n", mc2::fmt::as_i32(nw), mc2::fmt::as_i32(g_batch));
    }
}

void frameJobsShutdown() {
    mc2::jobs::stopPool();
    if (!g_pool) return;
    {
        std::lock_guard<std::mutex> lk(g_pool->mu);
        g_pool->quit = true;
    }
    g_pool->cvWork.notify_all();
    for (auto& t : g_pool->threads) t.join();
    delete g_pool;
    g_pool = nullptr;
    g_enabled = false;
}

void parallelForRange(int count, int batchSize,
                      const std::function<void(int, int)>& fn) {
    if (count <= 0) return;

    if (!g_enabled || !g_pool) {
        fn(0, count);
        g_serialFallback = true;
        return;
    }

    g_pool->chunks.clear();
    for (int i = 0; i < count; i += batchSize)
        g_pool->chunks.push_back({i, std::min(i + batchSize, count)});
    g_pool->fn           = fn;
    g_pool->totalChunks  = static_cast<int>(g_pool->chunks.size());
    g_pool->nextChunk.store(0, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk(g_pool->mu);
        g_pool->workersDone.store(0, std::memory_order_relaxed);
        ++g_pool->epoch;
    }
    g_pool->cvWork.notify_all();

    while (true) {
        int idx = g_pool->nextChunk.fetch_add(1, std::memory_order_relaxed);
        if (idx >= g_pool->totalChunks) break;
        fn(g_pool->chunks[idx].begin, g_pool->chunks[idx].end);
        g_chunksThisFrame.fetch_add(1, std::memory_order_relaxed);
    }

    {
        std::unique_lock<std::mutex> lk(g_pool->mu);
        g_pool->cvDone.wait(lk, [pool = g_pool] {
            return pool->workersDone.load(std::memory_order_acquire)
                   >= pool->numWorkers;
        });
    }
}

bool frameJobsEnabled()      { return g_enabled;      }
bool frameJobsTouchEnabled() { return g_touchEnabled; }
int  frameJobsBatch()        { return g_batch;        }
bool frameJobsTrace()        { return g_trace;        }

FrameJobsFrameStats frameJobsGetFrameStats() {
    return {
        g_chunksThisFrame.load(std::memory_order_relaxed),
        g_workersCount.load(std::memory_order_relaxed),
        g_serialFallback
    };
}

void frameJobsResetFrameStats() {
    g_chunksThisFrame.store(0, std::memory_order_relaxed);
    g_serialFallback = false;
}

// ===========================================================================
//  NEW mc2::jobs API  (DORMANT — no engine caller; harness-driven)
// ===========================================================================

namespace mc2 { namespace jobs {

// MEASURED by the CORE-1 harness (tools/job_system_harness, measure_dispatch_floor
// test) on the dev machine: median dispatch+join latency ~15 us (range
// 14.3-16.2 us over runs, 7 workers + main). The plan §1.4 records the same
// number so the floor and the determinism proof come from one run. The
// per-invocation adaptive fallback uses this so small counts never pay the
// barrier: count*estCostPerItemNs < kDispatchFloorNs -> run serial inline.
int64_t kDispatchFloorNs = 15000;  // ~15 us MEASURED (was provisional 20 us)

namespace {

// Per-participant TLS. Workers set these true; the calling (main) thread leaves
// t_isWorker false but is still a participant (t_participantId assigned lazily).
thread_local bool t_isWorker     = false;
thread_local bool t_isPoolThread = false;
// True while THIS thread is inside a dispatch (main participates in the fork,
// so a body that nests a dispatch re-enters on main too — not only on workers).
thread_local bool t_inDispatch   = false;

// ---- New fork/join pool (block dispatch) ----------------------------------
struct NewPool {
    // Set by dispatch before notify; read-only during execution.
    std::function<void(int /*blockIndex*/, int /*begin*/, int /*end*/)> body;
    int                     count = 0;
    int                     batch = 1;
    int                     numBlocks = 0;
    std::atomic<int>        nextBlock{0};

    std::mutex              mu;
    std::condition_variable cvWork;
    std::condition_variable cvDone;
    int                     epoch = 0;
    std::vector<int>        workerEpoch;
    std::atomic<int>        workersDone{0};
    int                     numWorkers = 0;
    bool                    quit = false;
    std::vector<std::thread> threads;
};

NewPool* g_np = nullptr;

inline void runBlock(NewPool* p, int idx) {
    const int begin = idx * p->batch;
    const int end   = std::min(begin + p->batch, p->count);
    p->body(idx, begin, end);
}

void npWorker(NewPool* p, int workerId) {
    MC2_JOBS_THREAD_NAME("Jobs ", workerId);
    t_isWorker     = true;
    t_isPoolThread = true;
    while (true) {
        {
            std::unique_lock<std::mutex> lk(p->mu);
            p->cvWork.wait(lk, [&] {
                return p->quit || p->workerEpoch[workerId] != p->epoch;
            });
            if (p->quit) return;
            p->workerEpoch[workerId] = p->epoch;
        }
        {
            MC2_JOBS_ZONE("Jobs.Worker");
            for (;;) {
                int idx = p->nextBlock.fetch_add(1, std::memory_order_relaxed);
                if (idx >= p->numBlocks) break;
                runBlock(p, idx);
            }
        }
        int done = p->workersDone.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (done == p->numWorkers) {
            std::lock_guard<std::mutex> lk(p->mu);
            p->cvDone.notify_one();
        }
    }
}

// Unconditional fork/join across the block range (no adaptive/reentrancy
// checks — the callers apply those). Main thread participates.
void dispatchBlocksRaw(
    int count, int batch,
    const std::function<void(int, int, int)>& blockBody) {
    NewPool* p = g_np;
    p->body      = blockBody;
    p->count     = count;
    p->batch     = batch;
    p->numBlocks = (count + batch - 1) / batch;
    p->nextBlock.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(p->mu);
        p->workersDone.store(0, std::memory_order_relaxed);
        ++p->epoch;
    }
    p->cvWork.notify_all();

    // Main steals blocks too. Mark main as in-dispatch so a body that nests a
    // dispatch re-enters serial (the reentrancy guard below), never deadlocks.
    t_inDispatch = true;
    for (;;) {
        int idx = p->nextBlock.fetch_add(1, std::memory_order_relaxed);
        if (idx >= p->numBlocks) break;
        runBlock(p, idx);
    }
    t_inDispatch = false;
    {
        std::unique_lock<std::mutex> lk(p->mu);
        p->cvDone.wait(lk, [p] {
            return p->workersDone.load(std::memory_order_acquire) >= p->numWorkers;
        });
    }
    p->body = nullptr;  // drop captured refs before returning
}

// Serial, in-order block walk. IDENTICAL grouping/indices to the dispatched
// path, so serial==parallel is exact for reductions.
void runBlocksSerial(
    int count, int batch,
    const std::function<void(int, int, int)>& blockBody) {
    const int numBlocks = (count + batch - 1) / batch;
    for (int i = 0; i < numBlocks; ++i) {
        const int begin = i * batch;
        const int end   = std::min(begin + batch, count);
        blockBody(i, begin, end);
    }
}

// ---- Per-thread bump arenas -----------------------------------------------
thread_local Arena t_arena;
thread_local bool  t_arenaRegistered = false;
std::mutex               g_arenaMu;
std::vector<Arena*>      g_arenaRegistry;

constexpr std::size_t kArenaBytes = 1u << 20;  // 1 MiB per participant

} // namespace

// ---- pool lifecycle -------------------------------------------------------
void startPool(int workerCount) {
    stopPool();
    if (workerCount < 0) workerCount = 0;
    g_np = new NewPool();
    g_np->numWorkers = workerCount;
    // Startup must not count as a completed dispatch.
    g_np->workerEpoch.assign(std::max(0, workerCount), g_np->epoch);
    for (int i = 0; i < workerCount; ++i)
        g_np->threads.emplace_back(npWorker, g_np, i);
}

void stopPool() {
    if (!g_np) return;
    {
        std::lock_guard<std::mutex> lk(g_np->mu);
        g_np->quit = true;
    }
    g_np->cvWork.notify_all();
    for (auto& t : g_np->threads) t.join();
    delete g_np;
    g_np = nullptr;
}

bool enabled()     { return g_np != nullptr && g_np->numWorkers >= 1; }
int  workerCount() { return g_np ? g_np->numWorkers : 0; }

bool isWorkerThread() { return t_isWorker; }
bool isPoolThread()   { return t_isPoolThread; }

// ---- arena ----------------------------------------------------------------
void* Arena::alloc(std::size_t bytes, std::size_t align) {
    if (align < 1) align = 1;
    if (!base_) {
        cap_  = kArenaBytes;
        used_ = 0;
    }
    std::size_t p = (used_ + (align - 1)) & ~(align - 1);
    // Fail-fast on overflow — arenas are frame-scoped and pre-sized (plan §1.6).
    JOBS_FATAL_IF(p + bytes > cap_);
    void* out = base_ + p;
    used_ = p + bytes;
    return out;
}

void Arena::reset() { used_ = 0; }

Arena& workerArena() {
    if (!t_arenaRegistered) {
        std::lock_guard<std::mutex> lk(g_arenaMu);
        g_arenaRegistry.push_back(&t_arena);
        t_arenaRegistered = true;
    }
    return t_arena;
}

void resetAllArenas() {
    std::lock_guard<std::mutex> lk(g_arenaMu);
    for (Arena* a : g_arenaRegistry) a->reset();
}

// ---- counter-based RNG (order-independent) --------------------------------
std::uint32_t rand_keyed(std::uint32_t turn, std::uint32_t stableKey,
                         std::uint32_t salt) {
    // A small stateless mix (splitmix-style finalizer over the three inputs).
    // Draw ORDER is irrelevant: identical (turn,key,salt) -> identical result,
    // so parallel iteration cannot perturb a SIM body's randomness.
    std::uint64_t x = 0x9E3779B97F4A7C15ull;
    x ^= (std::uint64_t)turn      * 0xFF51AFD7ED558CCDull;
    x ^= (std::uint64_t)stableKey * 0xC4CEB9FE1A85EC53ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x ^= (std::uint64_t)salt      * 0x94D049BB133111EBull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x =  x ^ (x >> 31);
    return (std::uint32_t)(x >> 32);
}

// ---- block dispatch entry (the shared core of parallel_for/parallel_reduce) -
namespace detail {
void parallel_for_blocks(
    Domain /*dom*/, int count, int batch, int estCostPerItemNs,
    const std::function<void(int, int, int)>& blockBody) {
    if (count <= 0) return;
    if (batch < 1) batch = 1;

    // (a) Per-invocation adaptive fallback — small counts never pay the barrier.
    if (static_cast<int64_t>(count) * static_cast<int64_t>(estCostPerItemNs) < kDispatchFloorNs) {
        runBlocksSerial(count, batch, blockBody);
        return;
    }
    // (b) Reentrancy guard — nested dispatch degrades to serial, stays visible.
    //     Covers BOTH worker threads and main-while-participating (t_inDispatch).
    if (t_isWorker || t_inDispatch) {
        JOBS_LOGONCE("jobs.reentrant_dispatch");
        runBlocksSerial(count, batch, blockBody);
        return;
    }
    // (c) No pool / no workers -> serial inline.
    if (!enabled()) {
        runBlocksSerial(count, batch, blockBody);
        return;
    }
    dispatchBlocksRaw(count, batch, blockBody);
}
} // namespace detail

void parallel_for(Domain dom, int count, int batch, int estCostPerItemNs,
                  const std::function<void(int, int)>& body) {
    detail::parallel_for_blocks(
        dom, count, batch, estCostPerItemNs,
        [&](int /*blockIndex*/, int begin, int end) { body(begin, end); });
}

// ---- dispatch-floor measurement (harness) ---------------------------------
int64_t measureDispatchFloorNs(int iters) {
    if (!enabled() || iters < 1) return 0;
    using clk = std::chrono::steady_clock;
    // count = participants, batch = 1, empty body -> pure wake + join cost.
    const int participants = workerCount() + 1;
    auto noop = [](int, int, int) {};
    std::vector<int64_t> samples;
    samples.reserve(iters);
    for (int i = 0; i < iters; ++i) {
        auto t0 = clk::now();
        dispatchBlocksRaw(participants, 1, noop);
        auto t1 = clk::now();
        samples.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];  // median
}

}} // namespace mc2::jobs
