# MC2 CPU Worker/Job System — Definitive Architecture & Slice Plan

**Status:** architecture ruling + execution ladder. Synthesizes 6 recon lanes. **Rev 2** — folds the adversarial review (B1 always-compiled enforcement; M1 sanitizer gate; M2 IO-pool guard scope; M3 sub-floor slice cut; M4 per-invocation adaptive fallback + honest barrier cost; M5 long SIM soak; M6 slice re-scoping; M7 alloc-free proof; M8 marquee measure-first spike; MINORs folded).
**Standing constraints honored:** VK = features-not-perf (no "port to VK for speed"); no pass reordering (depth-tie parity); Tracy 100 ns floor / no fine zones; cull-cascade 5-way contract; Tier-C txmmgr not worker-safe; StaticSceneProxy is the sanctioned static-prop endgame (this arc does not compete with it, it complements the sites StaticSceneProxy does not cover).

---

## 0. Build-config reality (read first — it invalidated Rev 1's safety spine)

**RelWithDebInfo is the only build a human or the smoke gate ever runs**, and CLAUDE.md mandates it (Release crashes on the GL debug callback). In that config:

- `_ARMOR` is defined **only** in `CMAKE_CXX_FLAGS_DEBUG` (`CMakeLists.txt:68`). `gosASSERT(x)` expands to `((void)0)` without `_ARMOR` (`gameos.hpp:103/105`). **`gosASSERT` is dead in RelWithDebInfo.**
- CMake's default `/DNDEBUG` is present in `CMAKE_CXX_FLAGS_RELWITHDEBINFO` and nothing strips it → **standard C `assert()` is also compiled out.**

**Consequence for this arc:** any safety mechanism expressed as `assert`/`gosASSERT` is a no-op in the shipped/tested build. Rev 1's "*Enforced, not just disciplined*" claim rested on exactly such asserts and was therefore fiction. **This revision replaces every enforcement check on the worker / heap / reentrancy / allocation paths with an always-compiled hard check (`JOBS_FATAL_IF`, §2.0) that is independent of `NDEBUG` and `_ARMOR`.** Asserts remain allowed only for *pure* invariant documentation that carries no safety weight.

`MC2_ASAN` (`/fsanitize=address`, `CMakeLists.txt:103`) is a **real, buildable config** in this repo and is promoted here to a required verification gate for every adoption slice (§4.3) — because a value oracle cannot see a race that happens to produce identical bytes (M1).

---

## 1. Architecture

### 1.1 Verdict: EXTEND `FRAME-JOBS-1`, do not replace

The existing pool (`code/frame_jobs.cpp/.h`, compiled into `mclib`) is the right core and must be kept:

- **Dispatch core is correct for this workload.** Epoch-bump-under-mutex wake → workers steal chunks via `fetch_add` on a shared cursor → **main thread participates** in stealing → `workersDone` counter + `cvDone` condvar barrier. This is textbook bursty fork/join, which is exactly the workload shape (see §1.4). Replacing it discards the parity evidence (tier1 5/5 both gate states, `worker_resubmit_calls==0` over 1967 frames) and the established **whitelist-virtual** (`isRecalcBoundsWorkerSafe`/`isTouchSplitSafe`) and **split-phase** (lock-free worker prepass → serial commit) idioms.
- **It is already GREEN/portable** (`gameos-platform-boundary-recon-1`): pure `std::thread`/`mutex`/`atomic`, zero `CreateThread`/`CRITICAL_SECTION`. macOS-ready by construction.

What is **missing** and is added by this arc (evolving `frame_jobs.*` → `code/job_system.*`, keeping `parallelForRange` as a back-compat shim):

1. A **domain-tagged API** (`Sim` vs `Render`) that selects partitioning + determinism policy.
2. `parallel_reduce` with **ordered merge** (float-add is non-associative).
3. **Per-worker bump arenas** so no worker ever allocates through `UserHeap`.
4. A **reentrancy guard** that **logs-once and serializes** (the pool is single/non-reentrant; nested dispatch currently deadlocks silently — degrade gracefully for the player, stay detectable in the trace).
5. A separate **async IO pool** (`submitIO`/`Latch`) for the load-time decode case — a producer/consumer shape, *not* fork/join. **This is its own slice (§3, slice 3), not part of CORE.**
6. An **always-compiled fatal-check macro** (`JOBS_FATAL_IF`, §2.0) replacing every safety `assert` (per §0).
7. Header-drift fixes (comment says workers "1–8"/batch 128; code clamps to `hw_concurrency` / batch 64 — code wins).

### 1.2 Pool sizing

- **`hw_concurrency − 1` persistent workers, main thread participates.** During a fork/join barrier the main thread has nothing else to do, so it must steal too (the pool already does this). `−1` leaves a core for the OS/GL driver/Tracy/SDL-mixer audio thread. This is the shipped setting; keep it. Env override `MC2_FRAME_JOBS_WORKERS`, clamped `[1..hw]`.
- **Two pools, two shapes.** The compute fork/join pool (above) and a **1–2 thread async IO pool** for decode. They are architecturally separate: the fork/join pool is non-reentrant and frame-barriered; the IO pool is a long-lived MPMC queue that runs concurrently with the whole frame and is drained opportunistically. **Both pools' threads register as pool threads (§1.6/§2.0) so the heap/alloc guards cover them equally.**

### 1.3 Queue design — pick single shared cursor, **not** per-worker work-stealing deques

For **this** workload — flat `parallel_for` over a known-count homogeneous array, dispatched from one thread, **no nested spawns** (nesting is banned) — a **single shared atomic cursor** (`fetch_add` chunk steal) is the correct choice:

- Chase-Lev per-worker deques earn their complexity on *recursive task-spawning DAGs* with unpredictable nested spawns. We have none of that.
- The shared-cursor steal already gives dynamic load balancing across uneven item costs at ~tens-of-ns per steal, with zero deque bookkeeping.

The **only** MPMC queue in the system is the async IO pool's job queue (heterogeneous `submitIO` closures), which is genuinely multi-producer/multi-consumer. Keep it off the hot fork/join path.

### 1.4 Job granularity (respect the Tracy 100 ns floor ruling) — and the honest barrier cost

The 100 ns floor is about *profiling zones*, but the same physics bounds job size. **Barrier cost must be modeled honestly:** waking N condvar-parked workers on Windows is realistically **10–30 µs** per region (scheduler quantum + wake latency), and each `parallel_for` pays it **twice** — dispatch wake + `cvDone` join. Rev 1's "~µs dispatch" was optimistic and is corrected here. The frame model has ~5 sequential parallel regions (§1.7); at ~20 µs each that is **~0.10–0.30 ms of pure barrier tax per frame**, which can *exceed* the small-mission win column. Rules:

- **Only fork/join a loop whose total body is > ~150 µs** (raised from 100 µs to cover the doubled-barrier reality). Below that, serial wins. Every candidate below is gated so this is measured, not assumed.
- **Per-invocation adaptive fallback is mandatory and lives inside `parallel_for` itself:** `if ((int64_t)count * estCostPerItemNs < kDispatchFloorNs) { body(0,count); return; }`. A small `count` at runtime never pays the barrier even when the site's gate is ON. `estCostPerItemNs` is a per-call-site constant seeded from the slice's `COST_SPLIT` capture; `kDispatchFloorNs` is calibrated once on the 8-core target (see below).
- **Publish the real break-even before sizing any win.** CORE-1's harness measures actual dispatch+join latency on the 8-core dev target and writes `kDispatchFloorNs`; all ROI numbers below are provisional until that lands.
- **Batch so each chunk ≈ several µs of work.** Light per-item (bounds recalc, `Touch()` field write): `batch = 64–128`. Heavy per-item (mech FK chain ~5–20 µs): `batch = 1–4`. Never one job per sub-µs item.
- **Tracy: one coarse zone per dispatch on the main thread**, optionally one per worker-batch when batch ≥ ~5 µs. **Never per-item.** Use `MC2_*_COST_SPLIT` buckets (10-frame cadence, <10 % overhead) for per-loop timing.

### 1.5 The two-domain split

| | **SIM domain** | **RENDER domain** |
|---|---|---|
| Partitioning | **Static contiguous** index ranges → item→worker mapping is deterministic and worker-count-independent | Dynamic chunk-steal (order irrelevant) |
| RNG | **No shared-LCG draws.** Counter-based `mc2_rand_keyed(turn, stableKey, salt)` only | N/A (no sim RNG) |
| Reduction | **Ordered merge** in canonical key order (WatchID / handle / block-index); float sums reduce in fixed order | Relaxed; per-object writes consumed after barrier |
| Diagnostics | Per-worker shards, merged in fixed order | Relaxed atomics OK |
| Oracle | **Three-way determinism gate + ASan region run + long OFF-vs-ON campaign soak** (§4) + tier1 | Pixel-parity bookmark PNG + ASan region run + tier1 |
| Gate default | OFF, needs its own long soak before any flip | OFF, byte-identical when OFF |

**Design principle for SIM jobs:** bodies are **pure per-item** — read only frozen frame state, write only the item's own slot + a per-worker scratch shard. Purity makes output bit-identical regardless of partitioning or steal order; static partitioning + ordered merge is belt-and-suspenders for any residual float accumulation.

### 1.6 Allocation policy on workers — and why "workers never touch UserHeap" is load-bearing, not belt-and-suspenders

The heap hazard is **broader than the heap stack** (correcting Rev 1's narrow framing). `UserHeap::Malloc` (`heap.cpp:697`):

1. mutates the global heap stack (`g_heapsStack`/`g_heapStackPointer`, unsynchronized), **and**
2. **walks and splits an unsynchronized `firstNearBlock` free list** (`heap.cpp:710–848`, no lock anywhere in the file).

So two workers in the same heap corrupt **the allocator's free list**, not merely the stack pointer. This is a genuine memory-corruption hazard, not a bookkeeping nuisance. Policy:

- **Pool threads (fork/join *and* IO) never allocate through `UserHeap`/gos heap.** Output/handle buffers are pre-sized on the main thread; transient scratch comes from a **per-worker bump arena** (`jobs::workerArena()`), reset each frame.
- **Also avoid the process heap in hot bodies.** Incidental `std::vector`/`std::string`/`std::function` growth hits the Windows process heap, which is internally locked → a **mutex convoy** across workers even though it is "thread-safe." Bodies pre-size all containers on the main thread and use the arena for transient allocation. This is enforced by the **alloc-free proof obligation** (§4.4), not left to discipline.
- **Enforced with an always-compiled check:** `MC2-JOBS-HEAP-GUARD-1` makes `g_heapStackPointer`/`g_heapsStack` `thread_local` **and** adds `JOBS_FATAL_IF(jobs::isPoolThread())` (not `assert`) at the top of `gos_PushCurrentHeap`. `isPoolThread()` returns true for **both** fork/join and IO workers (M2). (`gos_Malloc` → plain CRT `malloc` is thread-safe; the corruption hazard is the `UserHeap` free-list + stack, which the guard closes.)
- **`thread_local` semantic documented (m3):** a pool thread that *does* reach `UserHeap::Malloc` now reads an empty per-thread stack → null current heap → a **deterministic fail-fast crash**, not a silent race. This is the intended, diagnosable failure mode; the `JOBS_FATAL_IF` fires first and names the site in the trace.

### 1.7 Frame phase model — where barriers sit, what overlaps

`Mission::update` is a strictly ordered serial pipeline (`code/mission.cpp:903-1064`). The fork/join pool is single + non-reentrant, so within a frame the parallel regions are **sequential w.r.t. each other** (a join barrier between each). True cross-phase pipelining is out of scope — the wins are *within-phase fan-out*. The one concurrent element is the async IO pool. Note the `framesSinceActive` sweep is **not** shown as a fork region — it was cut for being sub-floor (M3, §3 slice 7-REJECTED).

```
FRAME HEAD (serial):  advance turn/frameLength; freeze camera + frustum-plane cache;
                      pre-warm two-step static gate caches; build flat handle lists.

land->geometry:       [serial one-shot hoist: tex-resolve frameBegin, frustum cache]
                      ── FORK (Render): per terrain-block LOD select ──▶ JOIN ──▶ serial consume
                         (guarded by adaptive fallback — likely below floor, may run serial)

ObjectManager::update:
   framesSinceActive   ══ SERIAL — sub-floor, NOT parallelized (measurement-only) ══
   static touch        [serial: shared-type / light-order hoist]
                      ── FORK (Render): touchWorkerPrepass ──▶ JOIN ──▶ touchSerialCommit (main, CANONICAL order)
   TGL transform       [serial: myType->SetTextureHandle per TYPE]
                      ── FORK (Render): per-actor FK ──▶ JOIN ──▶ serial GPU-record commit (main)
   mover->update       ══ SEQUENTIAL (sim, order-load-bearing) — NOT parallelized this arc ══
   SensorSystem scan   ── FORK (Sim): per-sensor scan ──▶ JOIN ──▶ serial ordered contact merge

HUD cursor pick:      ── FORK (Render): read-only inverseProject over verts ──▶ JOIN

ASYNC (spans whole frame / load): texture decode on IO pool; drained opportunistically.
```

Barriers sit at every `parallel_for` return (acquire on `workersDone`). GL-issuing / shared-ring / light-order-sensitive work is always the **serial tail on main**. The per-invocation adaptive fallback (§1.4) means any of these regions collapses to serial for free when its runtime `count` is small.

---

## 2. Code Sketches

### 2.0 Always-compiled fatal check (`code/jobs_fatal.h`) — the enforcement spine (B1)

```cpp
// Independent of NDEBUG and _ARMOR: compiled in EVERY config, including
// RelWithDebInfo (the shipped/tested build). Do NOT route through gosASSERT.
namespace mc2::jobs::detail {
    [[noreturn]] void fatal(const char* cond, const char* file, int line);
    void logOnce(const char* tag, const char* file, int line);  // trace, no abort
}

// Hard stop: writes {cond, thread id, file:line} to MC2_DIAGNOSTIC_TRACE_FILE
// via the LIVE ErrorHandler path, then std::abort(). Never elided.
#define JOBS_FATAL_IF(cond) \
    do { if (cond) ::mc2::jobs::detail::fatal(#cond, __FILE__, __LINE__); } while (0)

// Log-once + continue: for the reentrancy guard, which must DEGRADE not crash.
#define JOBS_LOGONCE(tag) \
    ::mc2::jobs::detail::logOnce(tag, __FILE__, __LINE__)
```

`fatal()` writes through the same live diagnostic-trace sink the smoke gate already consumes (`MC2_DIAGNOSTIC_TRACE_FILE`), so a tripped guard is visible in artifacts even on a headless run. The reentrancy guard uses `JOBS_LOGONCE` + serial fallback (graceful for the player, detectable in the trace); the heap/alloc/GL guards use `JOBS_FATAL_IF` (fail-fast — the hazard is memory corruption).

### 2.1 Job API header (`code/job_system.h`, evolves `frame_jobs.h`)

```cpp
namespace mc2::jobs {

enum class Domain { Render, Sim };          // drives partitioning + determinism policy

bool enabled();                             // gate on AND pool present (mission-scoped)
int  workerCount();                         // hw-1; main participates on top
bool isWorkerThread();                      // TLS tripwire, fork/join workers (was g_isFrameJobsWorker)
bool isPoolThread();                        // TRUE for fork/join AND IO threads (heap/alloc guard scope)

// Break-even, calibrated by CORE-1 harness on the 8-core target. Used by the
// per-invocation adaptive fallback so small counts never pay the barrier.
extern int64_t kDispatchFloorNs;

// Flat fork/join over [0,count). body runs a [begin,end) sub-range.
//  Adaptive: if count*estCostPerItemNs < kDispatchFloorNs -> body(0,count) inline, no dispatch.
//  Render: dynamic chunk-steal; batch tunes granularity.
//  Sim:    static contiguous partition -> worker-count-deterministic mapping.
void parallel_for(Domain dom, int count, int batch, int estCostPerItemNs,
                  const std::function<void(int begin, int end)>& body);

// Per-worker T scratch, merged on MAIN in ascending partition order.
// merge() is called with shards in fixed index order -> deterministic float sums.
template <class T>
T parallel_reduce(Domain dom, int count, int batch, int estCostPerItemNs, T identity,
                  const std::function<void(int begin, int end, T& shard)>& body,
                  const std::function<void(T& acc, const T& shard)>& merge);

// Per-worker bump arena, reset each frame. Workers take transient memory HERE,
// never through UserHeap/gos heap (see HEAP-GUARD-1 + alloc-free proof §4.4).
struct Arena { void* alloc(size_t bytes, size_t align); void reset(); };
Arena& workerArena();                       // indexed by current worker id

// Counter-based deterministic RNG for SIM bodies (no shared LCG draw).
uint32_t rand_keyed(uint32_t turn, uint32_t stableKey, uint32_t salt);

} // namespace mc2::jobs
```

The async IO pool API (`Latch`/`submitIO`) is **not** declared here — it lives in `code/jobs_io.h` and ships in its own slice (§3, slice 3). Sketch:

```cpp
// code/jobs_io.h  (SLICE 3, precondition of async decode)
namespace mc2::jobs::io {
class Latch {
public: explicit Latch(int n); void count_down(); void wait(); // main drains
private: std::atomic<int> n_; std::mutex m_; std::condition_variable cv_;
};
// IO threads register via isPoolThread()==true so the heap/alloc guards cover them.
void submitIO(std::function<void()> job, Latch* done);          // e.g. KTX2/BC7 decode
} // namespace mc2::jobs::io
```

### 2.2 Worker main loop (keep the proven epoch core)

```cpp
void Pool::workerMain(int workerId) {
    detail::t_workerId = workerId;
    detail::t_isWorker = true;                       // isWorkerThread() tripwire
    detail::t_isPoolThread = true;                   // isPoolThread() -> heap/alloc guard scope
    uint64_t seen = 0;
    for (;;) {
        { std::unique_lock<std::mutex> lk(mu_);
          cvWake_.wait(lk, [&]{ return epoch_ != seen || shutdown_; });
          if (shutdown_) return;
          seen = epoch_; }
        runChunks();                                 // steal until drained
        if (workersDone_.fetch_add(1, std::memory_order_acq_rel) + 1 == workerTarget_)
            cvDone_.notify_one();                    // last worker wakes dispatcher
    }
}

void Pool::runChunks() {                             // Render: dynamic steal
    const int n = count_, b = batch_;
    for (;;) {
        int begin = cursor_.fetch_add(b, std::memory_order_relaxed);
        if (begin >= n) break;
        body_(begin, std::min(begin + b, n));
    }
}

// dispatch(dom, count, batch, estCostPerItemNs, body):
//   // (a) adaptive fallback FIRST — small counts never pay the barrier
//   if ((int64_t)count * estCostPerItemNs < kDispatchFloorNs) { body(0, count); return; }
//   // (b) reentrancy guard: degrade gracefully, stay detectable
//   if (detail::t_isWorker) { JOBS_LOGONCE("jobs.reentrant_dispatch"); body(0, count); return; }
//   // (c) bump epoch_, notify_all, MAIN ALSO calls runChunks() (or its static Sim
//   //     partition), then waits on cvDone_.
```

The IO-pool worker loop (slice 3) sets `t_isPoolThread = true` but **not** `t_isWorker` — it must never be mistaken for a fork/join worker, but it must be covered by the heap/alloc guards.

### 2.3 Fully-worked adoption — `TG_MultiShape::TransformMultiShape` (top steady-state RENDER candidate)

Site: `mclib/msl.cpp:1433-1522`, driven per visible actor from `GameObjectManager` render prep. It is the largest remaining RENDER-side CPU loop and per-actor independent **after two hazards are removed**: the shared-type write (`myType->SetTextureHandle`, `msl.cpp:1486-1490`, all actors of a chassis share `myType`) and the `TG_Shape` class-static light/matrix scratch + global pools.

**This adoption is gated on a measure-first spike (§3, slice 11a) proving the win exists before the two refactor preconditions (slices 9+10) are spent** (M8).

```cpp
// Preconditions: MC2-JOBS-TGL-TYPEHOIST-1 + MC2-JOBS-TGL-SCRATCH-1 shipped,
//                AND the XFORM-SPIKE-0 number cleared the (calibrated) break-even.
using namespace mc2::jobs;

// (1) SERIAL HEAD — one write per shared TYPE per frame (was per actor).
for (TypeKey t : s_visibleTypesThisFrame)
    t.shape->myType->SetTextureHandle(currentTextureFor(t), currentAlpha(t));
warmStaticGates();                        // pre-warm cold two-step static caches

// (2) FORK — pure per-actor FK. Render domain: dynamic steal, batch=4.
//     Body writes ONLY actor->listOfShapes[i].shapeToWorld (own slot) and pulls
//     transient nodes from workerArena() / per-worker TGL pool (thread_local scratch).
//     estCostPerItemNs seeded from XFORM-SPIKE-0 (~5000-20000 ns/actor).
parallel_for(Domain::Render, (int)s_visibleActors.size(), /*batch*/4, /*estNs*/8000,
  [&](int begin, int end) {
    for (int a = begin; a < end; ++a)
        s_visibleActors[a]->TransformMultiShape_Hoisted(&workerArena(), s_frameXform);
  });                                     // implicit join barrier (acquire)

// (3) SERIAL TAIL — GL-adjacent / shared-ring work on MAIN, canonical actor order.
for (int a = 0; a < (int)s_visibleActors.size(); ++a)
    commitActorGpuRecords(s_visibleActors[a]);   // emitGpuCullRecord, light resubmit
```

`TransformMultiShape_Hoisted` is the refactored entry that (a) does **not** write `myType`, (b) takes an `Arena*` + a frozen `worldToClip` instead of touching `TG_Shape::s_worldToClip`/`s_listOfLights` statics. This is exactly why `TGL-TYPEHOIST-1` and `TGL-SCRATCH-1` are precondition slices.

---

## 3. Slice Ladder

Naming: core/new sites `MC2-JOBS-*`; extensions of the shipped touch pipeline `FRAME-JOBS-2*`. All gates default-OFF; OFF path byte-identical. **Every adoption slice (any slice with a runtime gate) additionally requires: (i) an `MC2_ASAN` build run over its parallel region green (§4.3), and (ii) a per-body alloc-free proof (§4.4).** tier1 smoke both gate states (5/5, Δ≤0) is table stakes and omitted per-line except where extra oracle is needed. Rollback for every slice = flip gate OFF (default) or revert the additive commit; no slice mutates the OFF path.

| # | Slice | Goal | Files | Gate | Size | Extra oracle | Rollback |
|---|---|---|---|---|---|---|---|
| 1 | **MC2-JOBS-CORE-1** | Evolve `frame_jobs`→`job_system`: `parallel_for(Domain, estCostNs)` **with per-invocation adaptive fallback**, `parallel_reduce` ordered merge, `Arena`, **`JOBS_FATAL_IF`/`JOBS_LOGONCE` (always-compiled)**, reentrancy log+serialize, `rand_keyed`, `isPoolThread`, header-drift fixes. **Calibrate `kDispatchFloorNs` on the 8-core target.** **Offline unit harness.** (No IO pool — that's slice 3.) | `code/job_system.{h,cpp}`, `code/jobs_fatal.{h,cpp}`, `code/frame_jobs.h` (shim), `tools/build_contract_harnesses.py` | none (dormant; no new callers) | **L** | Harness: `serial==parallel` over randomized inputs; `parallel_reduce` bit-identical across worker counts 1/2/8; negative control (unordered merge) **must diverge**; **measured dispatch+join latency published as `kDispatchFloorNs`**; **`JOBS_FATAL_IF(true)` unit proves it aborts + writes trace in a RelWithDebInfo harness build** | revert; `parallelForRange` unchanged |
| 2 | **MC2-JOBS-HEAP-GUARD-1** | `thread_local` heap stack + **`JOBS_FATAL_IF(isPoolThread())`** (not assert) in `gos_PushCurrentHeap`; covers **fork/join AND IO** threads. Unblocks any worker-alloc-adjacent site. | `GameOS/gameos/gameos.cpp`, `mclib/heap.cpp` | fatal always-compiled | S | Negative control: a test pool thread that allocates via `UserHeap` **actually aborts** (verified in RelWithDebInfo, not vacuous); document empty-stack→null-heap fail-fast semantic | revert |
| 3 | **MC2-JOBS-IO-POOL-1** | Async 1–2 thread MPMC IO pool: `Latch`/`submitIO`, threads set `isPoolThread()==true`. Producer/consumer, **not** frame-barriered. Precondition of decode. | `code/jobs_io.{h,cpp}` | none (dormant; no callers) | M | Harness: N producers / M consumers, `Latch` count-down correctness; ASan clean under contention | revert |
| 4 | **MC2-JOBS-ASYNC-DECODE-1** | Async KTX2/BC7 + TGA **decode** on IO pool; `glTex*` upload stays serial on main via completion queue. **Decode allocates only into pre-sized / CRT-malloc buffers, never `UserHeap`** (proof required). Load-time only, zero sim coupling. | `mclib/txmmgr.cpp` (`KtxLoader`, `:4397`), `code/jobs_io.cpp` | `MC2_JOBS_ASYNC_DECODE` | M | Mission-load wall-clock before/after; pixel-identical first-frame bookmark; **ASan over decode+upload**; decode alloc-free-of-UserHeap proof | gate OFF → synchronous decode |
| 5 | **MC2-JOBS-DIAG-SHARD-1** | Shard file-static telemetry/cycle accumulators (objmgr conflation counters; registry/batcher `s_*_cyc`, `s_total_*`) into per-worker shards + serial merge, or gate-off during fan-out. Precondition for counter-touching loops. | `code/objmgr.cpp`, `GameOS/gameos/gos_static_prop_registry.cpp`, `..._batcher.cpp` | folds into consumer gates | S | Counter values identical serial vs parallel | revert |
| 6 | **MC2-JOBS-HUD-PICK-1** | Parallelize `inverseProject` cursor pick (read-only math over terrain verts; affects only input, determinism-safe). mc2_01-heavy (399 µs — clears the floor). | HUD/`code` pick path (`baseline-A-post-8z.md:124`) | `MC2_JOBS_HUD_PICK` | S | Pick result identical serial vs parallel; Tracy MIF split before/after; ASan over region | gate OFF |
| 7 | **~~MC2-JOBS-GOM-SWEEP-1~~ — REJECTED (sub-floor, M3)** | `framesSinceActive` sweep is **34–72 µs**, below the ~150 µs break-even; the −0.03 ms "win" is inside a single barrier's noise. **Not parallelized.** Retained only as a `MC2_*_COST_SPLIT` measurement to confirm it stays sub-floor as missions grow. Do not spend a determinism-gate soak on it. | `code/objmgr.cpp` (measurement only) | none | — | COST_SPLIT confirms sub-floor | n/a |
| 8 | **FRAME-JOBS-2H-PATHB-TOUCH-1** | Extend shipped split-touch to serial Path-B terrain-block static touch (2G diagnostics already in place). **`touchSerialCommit` iterates CANONICAL object order, never shard/completion order** (m2). **Excludes gates/special buildings** (they call full `update()` w/ `MC2_DESTROY` → SIM, stay serial). | `code/objmgr.cpp:2667-2803`, `mclib/bdactor.cpp` | `MC2_FRAME_JOBS_TOUCH` (+ sub-gate) | M | **Pixel-parity 8/8 bookmarks — mc2_24 PINNED into the set** (the overlapping-opaque scene that exposed the frame-graph depth-tie reorder); Tracy Path-B µs before/after; ASan over prepass | gate OFF → legacy `touch()` |
| 9 | **MC2-JOBS-TGL-SCRATCH-1** | Make `TG_Shape` class-static light/matrix scratch (`tgl.h:846-865`) `thread_local`/context-struct; per-worker TGL pool arenas (`tgl.cpp:122-126`) with deterministic (actor-index) merge. Hazard fix; single-worker path byte-identical. **Re-rated L** — threads the context struct through every transform method in `tgl.cpp`. | `mclib/tgl.{h,cpp}` | none (refactor) | **L** | Single-worker output byte-identical to pre-refactor; ASan clean | revert |
| 10 | **MC2-JOBS-TGL-TYPEHOIST-1** | Hoist shared `myType->SetTextureHandle`/alpha (`msl.cpp:1486-1490`) + `TickImportedMechs` shared-type re-bake to serial per-type per-frame pass. | `mclib/msl.cpp`, `mclib/mech_anim_runtime.*` | none (refactor) | S | Pixel-parity; per-type write count == distinct visible types | revert |
| 11a | **MC2-JOBS-TGL-XFORM-SPIKE-0** (measure-first, M8) | **Throwaway** spike: capture `TransformMultiShape` cost at frame head with `MC2_CPU_PROJ_COST_SPLIT`, and bound the achievable win with a single scratch thread. **Gate slices 9/10/11b on this number clearing the calibrated break-even.** If it's memory-bandwidth-bound not CPU-bound, STOP — do not spend 9/10. | `mclib/msl.cpp` (temp instrumentation) | `MC2_JOBS_XFORM_SPIKE` (temp) | S | Published µs/actor + single-thread-scaling estimate; go/no-go verdict recorded | delete spike |
| 11b | **MC2-JOBS-TGL-XFORM-1** | The §2.3 per-actor `TransformMultiShape` fork/join. Respect cull cascade: parallelize transform only, **never skip `update()`** (setExists destroy). **Only if 11a cleared break-even.** | `code/objmgr.cpp`, `mclib/msl.cpp` | `MC2_JOBS_TGL_TRANSFORM` | L | Pixel-parity all bookmarks; `MC2_CPU_PROJ_COST_SPLIT` TGL_TRANSFORM bucket before/after; ASan over region | gate OFF → serial |
| 12 | **MC2-JOBS-TERRAIN-BLOCK-1** | Per terrain-block LOD select after sequential head hoist (`terrain.cpp:2318-2373`). **Measure-first** — likely <150 µs; adaptive fallback (§1.4) will run it serial if so, may **reject** outright. | `mclib/terrain.cpp` | `MC2_JOBS_TERRAIN_CULL` | M | Draw-cmd list identical; may **reject** under floor | gate OFF |
| 13 | **MC2-JOBS-SENSOR-SCAN-1** | Per-sensor `updateScan` (`contact.cpp:786-827`) → per-sensor staging → **serial ordered contact merge**. Best SIM candidate but hazard-heavy → **deferred** until the write-confinement audit + `moverLineOfSightTable` idempotency proof. | `code/contact.cpp`, `code/objmgr.cpp:886` | `MC2_JOBS_SENSOR_SCAN` | L | **Full SIM gate: three-way determinism + ASan + long campaign-length OFF-vs-ON end-state soak** (§4.2/4.5); SENSOR JSONL scan µs before/after | gate OFF |

**Explicitly deferred / out of scope (do-not-repropose respected):** `framesSinceActive` parallelism (rejected, sub-floor); mover `update()` parallelism (needs *total* `MC2_BRAIN_INTENT_QUEUE` coverage — multi-arc, not a slice); pathfinding worker (only if PATHFINDING-FACTS telemetry proves the 6/frame cap starves); collision broadphase (N=300, skip); pass reordering (permanently off); Tier-C txmmgr-on-workers (deferred, hazard-gated); VK sync-architecture (§7).

---

## 4. Verification Contract

### 4.1 Rules a SIM job must satisfy

1. **No shared-RNG draws in the body.** No `gos_rand()`/`RandomNumber`/`Stuff::Random` inside a SIM `parallel_for`. Randomness only via `jobs::rand_keyed(turn, stableObjectKey, salt)` — a stateless hash, so draw *order* is irrelevant. (Root cause: single global LCG at `gameos.cpp:284`, drawn inside `Mech3DAppearance` etc. at `mech3d.cpp:1278`.)
2. **No allocation-order-dependent IDs.** No creating handles/objects whose ID depends on allocation order; no appends to a shared list that assign sequence numbers.
3. **Ordered reduce.** Any cross-item accumulation is staged per-worker/per-item then merged on **main** in canonical key order (ascending WatchID / object handle / block index). Float sums reduce in fixed order (non-associative).
4. **Fixed partitioning independent of worker count.** SIM `parallel_for` uses static contiguous index partition → deterministic item→worker mapping. Combined with pure-per-item bodies, output is bit-identical for any worker count.
5. **Frozen frame state.** Body reads only frame-constant snapshots (`turn`, `frameLength`, camera + frustum-plane cache captured at frame head). No `gos_GetElapsedTime`/wall-clock in sim-affecting code.
6. **Pre-warmed static caches.** All two-step `static bool s_checked` gate caches (`routines.cpp:194`, `gameos.cpp:282`) force-warmed on main before first fan-out.
7. **Barrier before serial read.** No serial consumer reads worker output before the join (happens-before via `workersDone` acquire).

### 4.2 Automated determinism gate (value oracle — necessary, not sufficient; see §4.3)

A script wrapper (`scripts/jobs_determinism_gate.py`) around the bookmark sweep, run under `MC2_SMOKE_FIXED_TIMESTEP` with `MC2_DEBUG_STATE_DUMP=1` + `MC2_DIAGNOSTIC_TRACE_FILE`. A SIM slice must pass **all three**:

- **(a) OFF-vs-ON:** debug-state dump byte-identical across the whole sweep, gate-OFF vs gate-ON.
- **(b) ON reproducibility:** run1 vs run2 with gate ON, byte-identical (proves threading introduces no nondeterminism).
- **(c) Pixel parity:** bookmark PNGs byte-identical OFF vs ON.

**Negative control (required in the harness):** intentionally break a rule (unordered reduce, or a shared-LCG draw) → the gate must **fire**. Precedent: the VK arc root-caused a 10.68 % "nondeterminism" to an unfrozen gait tick — reproducibility regressions are bugs to root-cause, never noise.

**Explicit limitation (M1):** this is a **value** oracle. A data race that happens to produce identical bytes on this hardware/compiler is **invisible** to it — a benign-looking double-write to a lazy-init flag, a relaxed counter that lands on the same total, two workers computing the same constant into shared scratch. All are UB that can tear or miscompile on other targets (MoltenVK / ARM macOS — the stated portability target). Therefore the value oracle is **backstopped by ASan (§4.3) and, for SIM, a long soak (§4.5)** and is never the sole gate.

### 4.3 Sanitizer gate (M1) — required for every adoption slice

The repo already has `MC2_ASAN` (`/fsanitize=address`, `CMakeLists.txt:103`). For every slice with a runtime gate:

- Build the `MC2_ASAN` config and **run the bookmark sweep with the slice's gate ON**, exercising the parallel region. Must be **clean**. ASan catches exactly the `g_heapsStack` OOB-index and `firstNearBlock` free-list corruption the hazard appendix flags — the failure classes the value oracle cannot see.
- **True data-race coverage:** MSVC has **no** TSan. Options, in order of preference: (i) a clang-cl or clang/Linux CI job with `-fsanitize=thread` over the job-system unit harness (portable code, compiles cross-toolchain); (ii) if (i) is infeasible, **explicitly record** that the value-oracle + ASan combination is the accepted (incomplete) substitute, and why — so the gap is a documented decision, not an oversight.

### 4.4 Alloc-free proof obligation (M7) — mirrors the GL-free trace

Every fork/join body carries **two** transitive-call-set proofs, not one:

1. **GL-free** (existing): no `gl*`/`gos_SetRenderState` reachable from the body.
2. **Alloc-free** (new): no `UserHeap`/`gos_PushCurrentHeap` reachable, and no incidental process-heap allocation (`std::vector`/`std::string`/`std::function` growth, `new`) in the hot path — all containers pre-sized on main, transient memory from `workerArena()`.

Backed by a **debug global `operator new` / `malloc` hook** compiled into the harness and the `MC2_ASAN` config that calls `JOBS_FATAL_IF(isPoolThread())` (the always-compiled check, since `_ARMOR`/ASan hooks are config-specific) when a pool thread allocates inside a region. This makes the alloc-free claim *checked*, not asserted-by-reading.

### 4.5 Long SIM soak (M5)

MC2 sim is a chaotic cascade (sensor ping → target select → fire → damage → death → contact-set change). A one-frame timing shift can diverge a mission **many** frames later — after the 30 s smoke window closes (CLAUDE.md caps smoke at 30 s). Therefore a SIM slice (6-rejected notwithstanding; live case is slice 13 sensor scan, and any future SIM adoption) is a flip candidate only after a **campaign-length OFF-vs-ON end-state soak**: run a full mission/campaign gate-OFF and gate-ON under `MC2_SMOKE_FIXED_TIMESTEP`, compare **final object/damage/position state**, require identical. The 30 s three-way gate (§4.2) is the fast pre-filter; the long soak is the flip bar. (This runs on the user's instances / as a dedicated long job, never inside the 30 s tier1 gate.)

---

## 5. Expected Wins Table

Reference scenarios: **mc2_24-class** big mission (~1–2 k objects, 30–100 movers) and **mc2_01** small. Effective parallel width ≈ `hw−1 + main` (7 on 8-core, 15 on 16-core); realistic efficiency 0.5–0.7 (dispatch + memory-bandwidth). **All Δframe numbers are provisional until `kDispatchFloorNs` is calibrated (CORE-1) and the honest ~0.10–0.30 ms/frame aggregate barrier tax (§1.4) is subtracted.** Numbers tagged **[est]** lack a committed recon capture and gate a `*_COST_SPLIT` run inside their slice.

| Site (slice) | Current cost (recon) | Parallel fraction | Speedup 8→16c (on ‖ part) | mc2_24 Δframe | mc2_01 Δframe |
|---|---|---|---|---|---|
| TGL `TransformMultiShape` (11b, behind 11a spike) | ~1.5–3 ms **[est]** (largest RENDER loop; stale 0.3 ms×6 mech) | ~0.85 | 3–4× → 5–6× | **−1.0 to −1.8 ms** | −0.3 ms |
| HUD `inverseProject` pick (6) | 205 µs / **399 µs** | ~0.95 | 3–4× | −0.13 ms | **−0.25 ms** |
| Path-B static touch (8) | prepass-eligible portion of 754–1450 µs (busy); ~600 µs commit stays serial | ~0.4 | 2–3× | −0.2 to −0.4 ms | −0.05 ms |
| Sensor scan (13, SIM) | ~50–150 µs **[est]** (inside mover_update) | ~0.7 (merge serial) | 2–3× | −0.05 to −0.1 ms | ~0 |
| ~~`framesSinceActive` sweep~~ (REJECTED) | 34–72 µs — **sub-floor** | — | — | **not parallelized** | — |
| Terrain block LOD select (12) | <150 µs **[est]** | ~0.9 | — | **may reject** (under floor → adaptive-serial) | — |
| VK mech feed build/memcpy (§7) | mech `stage_us` 522 µs / 150 µs | ~0.6 (after double-buffer) | 2× | ≤−0.3 ms (VK only) | ≤−0.1 ms |
| Async texture decode (4) | hundreds of tex/load | ~0.9 | 4–6× | **load-time only** (seconds off cold load) | load-time only |

**Honest caveats:** (1) TransformMultiShape is the dominant estimate and is gated behind the **11a measure-first spike** before its two refactor preconditions (9/10) are spent — if it's memory-bandwidth-bound the whole marquee is dropped for zero sunk refactor cost. (2) The per-frame barrier tax (~0.10–0.30 ms, §1.4) is **subtracted from** the win column; on mc2_01 the small-mission wins (~0.02–0.25 ms) can be near or below that tax, which is exactly why the per-invocation adaptive fallback (§1.4) exists — small counts collapse to serial and pay no barrier. (3) Path-B ROI competes with the StaticSceneProxy arc (which *eliminates* the walk); compute against the post-`STABLE_LIGHT_SKIP` remainder, and treat it as a bridge, not a destination. (4) Steady-state frame wins are real but modest vs the load-time decode win, which is the biggest wall-clock UX gain per unit risk.

---

## 6. Hazard Remediation Appendix

| Hazard (lane 3 unless noted) | Site | Chosen remediation |
|---|---|---|
| **Safety asserts compiled out in RelWithDebInfo** (the only shipped/tested config) | `_ARMOR`-gated `gosASSERT` (`gameos.hpp:103`); `/DNDEBUG` strips C `assert` | **All worker/heap/reentrancy/alloc guards use `JOBS_FATAL_IF`/`JOBS_LOGONCE` (`jobs_fatal.h`)** — always-compiled, `NDEBUG`/`_ARMOR`-independent, writes offending thread+site to `MC2_DIAGNOSTIC_TRACE_FILE`. Asserts kept only for non-safety invariant docs. |
| Value oracle blind to same-byte races | §4.2 | **`MC2_ASAN` region run required per adoption slice** (§4.3) + clang/CI TSan note or documented substitute. |
| Shared global LCG drawn inside per-object updates; parallel iteration reorders draws | `gameos.cpp:284`, `mech3d.cpp:1278`, `bdactor.cpp:1807`, `gvactor.cpp:1507` | **Counter-based `rand_keyed(turn, objectHandle, salt)`** — stateless, order-independent. Ban `gos_rand`/`Stuff::Random` in SIM bodies via **`check-*.py` lint** (grep = immediate-body lint only, not a transitive guarantee — m1; the run1-vs-run2 gate + long soak backstop transitive reaches). Alternative for legacy sites: hoist all draws to a serial pre-pass. |
| `gos_PushCurrentHeap/Pop` unsynchronized `g_heapsStack`/`g_heapStackPointer` **and** `UserHeap::Malloc` unsynchronized `firstNearBlock` free-list walk/split (`heap.cpp:710–848`) | `gameos.cpp:173-216`, `heap.cpp:697-848` | **`thread_local` heap stack** + **`JOBS_FATAL_IF(isPoolThread())`** in `gos_PushCurrentHeap` (HEAP-GUARD-1, covers fork/join **and** IO). Pool threads use `jobs::workerArena()`. Free-list corruption (not just stack) is why "workers never touch UserHeap" is load-bearing (m4). Empty-stack→null-heap fail-fast documented (m3). |
| Incidental process-heap allocation in bodies → cross-worker mutex convoy | any body using `std::vector`/`std::string`/`std::function` growth | **Alloc-free proof (§4.4)** + debug `operator new`/malloc hook fataling on pool-thread alloc; pre-size all containers on main; transient via arena. |
| IO-pool threads outside fork/join guard scope | `submitIO` decode threads | **`isPoolThread()` true for IO threads too** (M2); slice 3 registers them; slice 4 decode proves alloc-free-of-UserHeap. |
| `TG_Shape` class-static light/matrix scratch + global vertex/face/shadow pools clobbered across concurrent transforms | `tgl.h:846-865`, `tgl.cpp:122-126` | **`thread_local` scratch / context-struct** passed to `TransformMultiShape_Hoisted`; **per-worker pool arenas**, merged in actor-index order (TGL-SCRATCH-1, re-rated L). |
| File-static telemetry/cycle accumulators → silent lost updates | `objmgr.cpp:2407-2476`, `gos_static_prop_registry.cpp:79-102`, `..._batcher.cpp:245-269` | **Per-worker shards + serial merge**, or hard-gate the telemetry OFF during fan-out (DIAG-SHARD-1). |
| Shared-type `myType->SetTextureHandle` / `TickImportedMechs` re-bake written per-actor | `msl.cpp:1486-1490`, `mech_anim_runtime.*` | **Hoist to one write per shared TYPE per frame** in the serial head (TYPEHOIST-1). |
| `worldLights[0]->aRGB` mutated per-actor (blocks widening light commit) | `bdactor.cpp:3477-3480, 4191` | **Per-actor light snapshots** instead of mutating shared `worldLights[]`. Until done, light resubmit **stays in the serial commit tail** (shipped 2D contract). |
| `emitGpuCullRecord` append-to-shared-ring | GOM update loops | **Per-worker staging** merged in **canonical order**, or atomic ring-slot reservation; keep record emission on the serial tail for the first adoptions. |
| Touch commit reorder reintroduces depth-tie parity break | `objmgr.cpp:2667-2803` | **`touchSerialCommit` iterates canonical object order** (m2), never shard/completion order; **mc2_24 pinned in the 8/8 bookmark set** to expose the exact overlapping-opaque failure the frame-graph arc rejected. |
| `masterTextureNodes[i].lastUsed` stamp race (Tier-C only) | `txmmgr.cpp:4809, 4999, 5038` | **`relaxed atomic<long>`** stamp, or per-worker touched-handle list replayed in serial tail. `renderLists`/eviction stay serial. Deferred (Tier-C not in this arc). |
| `g_numShadowShapes` may exceed `MAX_SHADOW_SHAPES=512` by design | `txmmgr.cpp:387-421` | **Every reader clamps `std::min(load, MAX)`**; never store the counter back on overflow (racy). Invariant added to `check-*.py`. |
| ABL global VM stack — any two ABL-running updates concurrently corrupt | `ablexec.cpp:42-61` | **Sequential-by-design.** ABL-executing updates (mover `update()`, `missionBrain`) are the SIM partition boundary; never enter the pool. |
| Cold two-step static gate caches racy on first parallel touch | `routines.cpp:194`, `gameos.cpp:282` | **Force-warm on main** in the frame-head warmup before first fan-out. |
| Pool non-reentrant → nested dispatch deadlocks | `frame_jobs.cpp:129-171` | **Reentrancy guard = `JOBS_LOGONCE` + serial fallback** in `dispatch()` (degrade gracefully for the player, stay detectable in trace — not `assert`, not abort). |
| Small `count` still pays full barrier → net regression on small missions | all sites, esp. mc2_01 | **Per-invocation adaptive fallback** inside `parallel_for`: `count*estCostPerItemNs < kDispatchFloorNs → run serial inline`. Break-even calibrated in CORE-1 (M4). |
| Cull cascade load-bearing 5 ways (setExists destroy, TG vertex-pool exhaustion) | `objmgr.cpp:1731,1748`, `mech3d.cpp:4256` | Parallelize **transform only**; **never** force/skip `update()` on out-of-block objects; keep pool sizing post-cull. |
| gos render-state cache single `stateCacheValid_` bit, ~14 manual invalidation sites, not thread-aware | `gameos_graphics.cpp:1565` | **No GL on workers.** All `gl*`/`gos_SetRenderState` stay on the render thread; each new worker body gets a **GL-free transitive call-set trace (§4.4 proof #1)**. GlStateGuard debt is orthogonal. |

---

## 7. Interaction Notes

**macOS / Metal.** The new core uses **only** `std::thread`/`std::mutex`/`std::condition_variable`/`std::atomic` — no `CreateThread`/`CRITICAL_SECTION`/`SRWLOCK`. `frame_jobs` is already rated GREEN/portable; keep it that way. `gos_CreateThread` (`toolos.hpp:24`) is dead — do not revive it. Editor threads (`EditorTaskRunner`, `EditorWatchdog`) are separate binaries and out of scope. A portable job core is *aligned with* the VK north star (threading is one of VK's stated feature justifications). **The value-oracle's blindness to same-byte races (§4.2) matters most here** — UB that is benign on x86/MSVC can tear on ARM/MoltenVK, which is precisely why the ASan gate (§4.3) is non-negotiable per adoption slice.

**VK arc — what the job system absorbs vs. what stays out.** *Absorbs:* the VK CPU **feed-production** costs — mech instance/bone/run snapshot builds (`gos_mech_batcher.cpp:2596-2829`) + `LightsData`/instance/bone `memcpy`s (`vulkan_mech_island.cpp:964-986`), the bulk of mech `stage_us` (150 µs mc2_01 / 522 µs mc2_24). These fork/join **only after double-buffering the file-static `s_vkMech*` snapshot vectors** (they're reused per frame and consumed by the island later the same frame — the double-buffer is the hazard fix). ROI ≤0.5 ms mc2_24. *Out of scope:* terrain fragment cost (GPU-bound, parity-locked); **per-island `vkQueueSubmit`+same-frame `vkWaitForFences`** stalls (~370 µs terrain + ~274 µs mech) — the CPU is *blocked on the GPU*, so no number of threads helps; that needs a **sync-architecture change** (collapse islands into one submit chain with N-buffered fences / timeline semaphores, per the fenceless direct-chain pattern already proven in the 4 post passes) which the VK backlog rules a structural multi-slice change, **not** a job-system fix; interop semaphore ping-pong (already dead in strict native). Do not touch the shadow-producer gating (P0 `62707331` lineage).

**Tracy integration.** Zone-**per-batch**, never per-item — honor the 100 ns floor and the `fdc47bc` strip ruling (do not reintroduce `PatchStream.AppendQuad`/`TerrainObject::update` sub-phase zones). One coarse `ZoneScoped` per dispatch on the main thread; optionally one per worker-batch when batch ≥ ~5 µs. For per-loop timing use `MC2_*_COST_SPLIT` buckets (10-frame cadence, <10 % overhead) and **within-run phase deltas**, not cross-run smoke FPS (observer-effect-dominated at fine granularity; other lanes perturb cross-run FPS).

---

**Bottom line:** extend the proven `FRAME-JOBS` pool into a domain-tagged `job_system` (single shared-cursor fork/join + a separate async IO pool), but land the safety spine correctly this time: **always-compiled `JOBS_FATAL_IF` (not asserts, which are dead in RelWithDebInfo), an `MC2_ASAN` region gate on every adoption, a heap/alloc guard that covers IO threads too, and a per-invocation adaptive fallback so small missions never pay the barrier.** Ladder: CORE-1 (with calibrated break-even + always-on fatal check) → heap-guard → IO pool → isolated async decode and read-only HUD pick early → the `TransformMultiShape` per-actor loop as the marquee, but **gated behind a throwaway measure-first spike before its two refactor preconditions are spent** → SIM sites (sensor scan) only behind the three-way determinism gate **plus ASan plus a campaign-length end-state soak**. `framesSinceActive` is **cut** as sub-floor. Everything default-OFF, byte-identical OFF, measured before flip, with mover/ABL/pass-reordering permanently out of scope.