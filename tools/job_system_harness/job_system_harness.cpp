// tools/job_system_harness/job_system_harness.cpp
// MC2-JOBS-CORE-1 — offline unit harness for the domain-tagged job system.
//
// Links the REAL production core (code/job_system.cpp + code/jobs_fatal.cpp) and
// proves the CORE-1 oracle (plan §3 slice-1 row) game-free:
//   * serial == parallel equivalence over randomized inputs,
//   * parallel_reduce bit-identical across worker counts 1 / 2 / 8,
//   * a negative control (unordered float reduce) that MUST diverge,
//   * JOBS_FATAL_IF(true) aborts + writes a trace record in a RelWithDebInfo
//     build (proven by re-spawning THIS exe with --test fatal_trip),
//   * measures + prints kDispatchFloorNs (dispatch+join latency on this machine).
//
// STDOUT is owned by the harness framework (--json). Diagnostics go to stderr.
#include "contract_harness.h"
#include "job_system.h"
#include "jobs_fatal.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Gates/gate_cold.h"   // invalidateAll() after _putenv
#ifdef _WIN32
#include <process.h>   // _getpid
#else
#include <unistd.h>    // getpid
#endif

using namespace contract_harness;
namespace jobs = mc2::jobs;

// Captured in main() so the fatal subprocess test can re-spawn this exe.
static const char* g_argv0 = "job_system_harness";

// ---- helpers --------------------------------------------------------------

static std::uint64_t dbits(double d) {
    std::uint64_t u = 0;
    std::memcpy(&u, &d, sizeof(u));
    return u;
}

// Deterministic value function used by the equivalence test (pure per-item).
static int itemValue(int i) { return (i * 2654435761u) ^ (i >> 3); }

// Compute per-index outputs via parallel_for with a given worker count.
static std::vector<int> computeParallelFor(int count, int workers, int batch) {
    std::vector<int> out(static_cast<size_t>(count), 0);
    jobs::startPool(workers);
    // Force the dispatched path (bypass the adaptive fallback) so we truly test
    // the parallel machinery, not the serial inline shortcut.
    const int64_t savedFloor = jobs::kDispatchFloorNs;
    jobs::kDispatchFloorNs = 0;
    jobs::parallel_for(jobs::Domain::Render, count, batch, /*estNs*/1000,
        [&](int b, int e) {
            for (int i = b; i < e; ++i) out[static_cast<size_t>(i)] = itemValue(i);
        });
    jobs::kDispatchFloorNs = savedFloor;
    jobs::stopPool();
    return out;
}

// ---- tests ----------------------------------------------------------------

static bool test_serial_equals_parallel(TestCtx& t) {
    const int count = 100003;   // prime-ish, not a batch multiple
    const int batch = 64;
    // Ground truth: pure serial (no pool).
    std::vector<int> golden(static_cast<size_t>(count), 0);
    for (int i = 0; i < count; ++i) golden[static_cast<size_t>(i)] = itemValue(i);

    for (int workers : {0, 1, 2, 8}) {
        std::vector<int> got = computeParallelFor(count, workers, batch);
        CH_CHECK(t, got == golden);
        if (got != golden) {
            std::fprintf(stderr, "  mismatch at workers=%d\n", workers);
            break;
        }
    }
    return t.failures == 0;
}

// A wide-magnitude input where float summation ORDER changes the result.
static std::vector<double> magnitudeVariedInput() {
    std::vector<double> v;
    v.reserve(1001);
    v.push_back(1.0e16);                 // dominant term: swamps lone +1.0 adds
    for (int i = 0; i < 1000; ++i) v.push_back(1.0);
    return v;
}

static double orderedReduce(const std::vector<double>& v, int workers, int batch) {
    jobs::startPool(workers);
    const int64_t savedFloor = jobs::kDispatchFloorNs;
    jobs::kDispatchFloorNs = 0;  // force the dispatched path
    double sum = jobs::parallel_reduce<double>(
        jobs::Domain::Sim, static_cast<int>(v.size()), batch, /*estNs*/1000,
        /*identity*/0.0,
        [&](int b, int e, double& shard) {
            for (int i = b; i < e; ++i) shard += v[static_cast<size_t>(i)];
        },
        [](double& acc, const double& shard) { acc += shard; });
    jobs::kDispatchFloorNs = savedFloor;
    jobs::stopPool();
    return sum;
}

static bool test_reduce_bit_identical_across_worker_counts(TestCtx& t) {
    const auto v = magnitudeVariedInput();
    const int batch = 8;
    const double r1 = orderedReduce(v, 1, batch);
    const double r2 = orderedReduce(v, 2, batch);
    const double r8 = orderedReduce(v, 8, batch);
    const double r0 = orderedReduce(v, 0, batch);  // serial path, same grouping

    CH_CHECK(t, dbits(r1) == dbits(r2));
    CH_CHECK(t, dbits(r1) == dbits(r8));
    CH_CHECK(t, dbits(r1) == dbits(r0));   // serial == parallel, bit-exact
    if (t.failures) {
        std::fprintf(stderr, "  r0=%.20g r1=%.20g r2=%.20g r8=%.20g\n",
                     r0, r1, r2, r8);
    }
    return t.failures == 0;
}

static bool test_negative_control_unordered_diverges(TestCtx& t) {
    // The load-bearing guarantee is ORDER. Prove that an UNORDERED reduce of the
    // SAME input diverges bitwise — i.e. if the merge order were relaxed, the
    // determinism (and the across-worker-count identity above) would break.
    const auto v = magnitudeVariedInput();
    double fwd = 0.0;
    for (size_t i = 0; i < v.size(); ++i) fwd += v[i];
    double rev = 0.0;
    for (size_t i = v.size(); i-- > 0;) rev += v[i];

    // MUST diverge — else the input is not order-sensitive and the test is vacuous.
    CH_CHECK(t, dbits(fwd) != dbits(rev));
    if (dbits(fwd) == dbits(rev))
        std::fprintf(stderr, "  NEGATIVE CONTROL VACUOUS: fwd==rev (%.20g)\n", fwd);
    return t.failures == 0;
}

static bool test_reentrancy_degrades_not_deadlock(TestCtx& t) {
    // Nested dispatch from inside a worker body must log-once + run serial,
    // never deadlock. If this test returns at all, the guard worked.
    jobs::startPool(4);
    const int64_t savedFloor = jobs::kDispatchFloorNs;
    jobs::kDispatchFloorNs = 0;
    std::vector<int> out(256, 0);
    jobs::parallel_for(jobs::Domain::Render, 256, 16, 1000, [&](int b, int e) {
        // Nested dispatch: guarded -> runs inline on this worker.
        jobs::parallel_for(jobs::Domain::Render, e - b, 4, 1000,
            [&](int ib, int ie) {
                for (int i = b + ib; i < b + ie && i < e; ++i)
                    out[static_cast<size_t>(i)] = i;
            });
    });
    jobs::kDispatchFloorNs = savedFloor;
    jobs::stopPool();
    // Outer range covered [0,256); each element set to its index by the nested call.
    bool ok = true;
    for (int i = 0; i < 256; ++i) if (out[static_cast<size_t>(i)] != i) { ok = false; break; }
    CH_CHECK(t, ok);
    return t.failures == 0;
}

static bool test_adaptive_fallback_small_count_runs_serial(TestCtx& t) {
    // With a nonzero floor, a small count*est must NOT dispatch (no barrier).
    // We can't directly observe "no dispatch", but we can prove correctness of
    // the fallback path and that it works with a pool present.
    jobs::startPool(8);
    jobs::kDispatchFloorNs = 1000000;  // 1 ms floor
    std::vector<int> out(10, -1);
    jobs::parallel_for(jobs::Domain::Render, 10, 4, /*estNs*/10, [&](int b, int e) {
        for (int i = b; i < e; ++i) out[static_cast<size_t>(i)] = itemValue(i);
    });
    jobs::stopPool();
    bool ok = true;
    for (int i = 0; i < 10; ++i) if (out[static_cast<size_t>(i)] != itemValue(i)) { ok = false; break; }
    CH_CHECK(t, ok);
    return t.failures == 0;
}

static bool test_rand_keyed_order_independent(TestCtx& t) {
    // Same key -> same value, regardless of call order; different keys differ.
    std::uint32_t a = jobs::rand_keyed(7, 100, 3);
    std::uint32_t b = jobs::rand_keyed(7, 101, 3);
    std::uint32_t a2 = jobs::rand_keyed(7, 100, 3);
    CH_CHECK(t, a == a2);     // deterministic
    CH_CHECK(t, a != b);      // key-sensitive (statistically; distinct here)
    return t.failures == 0;
}

// The measurement "test": always passes; prints kDispatchFloorNs to stderr so
// the harness run publishes the number the plan §1.4 records.
static bool test_measure_dispatch_floor(TestCtx& t) {
    jobs::startPool(std::max(1, 7));  // ~8-core target: hw-1 = 7 workers
    const int64_t floorNs = jobs::measureDispatchFloorNs(2000);
    jobs::stopPool();
    std::fprintf(stderr, "  [MEASURE] kDispatchFloorNs (median dispatch+join) = %lld ns\n",
                 static_cast<long long>(floorNs));
    CH_CHECK(t, floorNs > 0);
    return t.failures == 0;
}

// ---- fatal-check proof ----------------------------------------------------
// fatal_trip: NOT in the default suite. When named via --test fatal_trip it
// trips JOBS_FATAL_IF(true), which writes a trace record then std::abort()s.
// The parent test test_fatal_if_aborts spawns THIS exe with that arg and checks
// (a) the child exited abnormally (nonzero) and (b) the trace file got a record.
static bool test_fatal_trip(TestCtx& /*t*/) {
    JOBS_FATAL_IF(true);   // never returns
    return true;           // unreachable
}

static bool test_fatal_if_aborts_and_traces(TestCtx& t) {
    namespace fs = std::filesystem;
#ifdef _WIN32
    const long pid = static_cast<long>(_getpid());
#else
    const long pid = static_cast<long>(::getpid());
#endif
    const fs::path trace = fs::temp_directory_path() /
        ("jobs_fatal_probe_" + std::to_string(pid) + ".jsonl");
    std::error_code ec;
    fs::remove(trace, ec);

    // Child inherits this env var; jobs_fatal.cpp appends its record there.
#ifdef _WIN32
    _putenv_s("MC2_DIAGNOSTIC_TRACE_FILE", trace.string().c_str());
    const char* devnull = "nul";
#else
    setenv("MC2_DIAGNOSTIC_TRACE_FILE", trace.string().c_str(), 1);
    const char* devnull = "/dev/null";
#endif
    mc2::gate::invalidateAll();   // the variable above was set after static initialisers cached gates

    std::ostringstream cmd;
    cmd << '"' << g_argv0 << "\" --test fatal_trip > " << devnull << " 2>&1";
    const int rc = std::system(cmd.str().c_str());

    // (a) Abnormal termination: std::system returns nonzero when the child
    // aborts (Windows: the abort exit code; POSIX: encodes the signal).
    CH_CHECK(t, rc != 0);

    // (b) The fatal path wrote a JOBS_FATAL record to the trace sink.
    std::ifstream in(trace);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string body = ss.str();
    CH_CHECK(t, body.find("JOBS_FATAL") != std::string::npos);
    CH_CHECK(t, body.find("\"cond\":\"true\"") != std::string::npos);
    if (t.failures)
        std::fprintf(stderr, "  rc=%d trace='%s' body-len=%zu\n",
                     rc, trace.string().c_str(), body.size());

    fs::remove(trace, ec);
    return t.failures == 0;
}

int main(int argc, char** argv) {
    if (argc > 0 && argv[0]) g_argv0 = argv[0];

    Harness h("job_system_harness");
    h.add("serial_equals_parallel",                 test_serial_equals_parallel);
    h.add("reduce_bit_identical_across_worker_counts",
                                                    test_reduce_bit_identical_across_worker_counts);
    h.add("negative_control_unordered_diverges",    test_negative_control_unordered_diverges);
    h.add("reentrancy_degrades_not_deadlock",       test_reentrancy_degrades_not_deadlock);
    h.add("adaptive_fallback_small_count_runs_serial",
                                                    test_adaptive_fallback_small_count_runs_serial);
    h.add("rand_keyed_order_independent",           test_rand_keyed_order_independent);
    h.add("measure_dispatch_floor",                 test_measure_dispatch_floor);
    h.add("fatal_if_aborts_and_traces",             test_fatal_if_aborts_and_traces);
    // Sub-process-only: trips the always-compiled fatal check. Not in default suite.
    h.add("fatal_trip",                             test_fatal_trip, /*inDefault=*/false);
    return h.run(argc, argv);
}
