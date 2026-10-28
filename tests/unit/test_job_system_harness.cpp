#include "doctest.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include "frame_jobs.h"
#include "job_system.h"
#include "sim_overlap.h"
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

#include "Gates/gate_cold.h"
#include "contract_test_support.h"
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <process.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace {
namespace jobs = mc2::jobs;

struct JobsFixture {
    const int64_t dispatchFloor = jobs::kDispatchFloorNs;
    JobsFixture() { mc2::gate::markMainEntered(); }
    ~JobsFixture() {
        frameJobsShutdown();
        jobs::stopPool();
        jobs::kDispatchFloorNs = dispatchFloor;
    }
};

TEST_CASE_FIXTURE(JobsFixture, "job_system_harness/startup_cannot_release_first_dispatch_early") {
    mc2::gate::ScopedOverride<"MC2_FRAME_JOBS"> enabled("1");
    mc2::gate::ScopedOverride<"MC2_FRAME_JOBS_WORKERS"> workers("4");
    jobs::kDispatchFloorNs = 0;
    bool legacy = false;
    SUBCASE("frame jobs") { legacy = true; }
    SUBCASE("domain jobs") { legacy = false; }

    for (int iteration = 0; iteration < 256; ++iteration) {
        std::array<std::atomic<int>, 256> visits{};
        std::atomic<int> completed{0};
        auto body = [&](int begin, int end) {
            for (int i = begin; i < end; ++i) {
                std::this_thread::yield();
                visits[i].fetch_add(1, std::memory_order_relaxed);
                completed.fetch_add(1, std::memory_order_release);
            }
        };
        if (legacy) {
            frameJobsInit();
            parallelForRange(256, 1, body);
        } else {
            jobs::startPool(4);
            jobs::parallel_for(jobs::Domain::Render, 256, 1, 1000, body);
        }
        const int completedAtReturn = completed.load(std::memory_order_acquire);
        // Captures stay alive until every worker is joined, even if the barrier fails.
        if (legacy) frameJobsShutdown();
        else jobs::stopPool();
        CAPTURE(iteration);
        REQUIRE(completedAtReturn == 256);
        for (const auto& visit : visits)
            REQUIRE(visit.load(std::memory_order_relaxed) == 1);
    }
}


static std::uint64_t dbits(double d) {
    std::uint64_t u = 0;
    std::memcpy(&u, &d, sizeof(u));
    return u;
}

TEST_CASE_FIXTURE(JobsFixture, "job_system_harness/render_and_sim_dispatch_overlap") {
    mc2::gate::ScopedOverride<"MC2_SIM_OVERLAP_AUDIT"> audit("1");
    jobs::startPool(4);
    jobs::kDispatchFloorNs = 0;
    REQUIRE(jobs::workerCount() == 4);
    std::array<std::atomic<int>, 257> sim{}, render{};
    std::atomic<bool> simEntered{false}, renderEntered{false}, timedOut{false};
    std::atomic<uint32_t> workerFrame{0};
    auto rendezvous = [&](std::atomic<bool>& own, std::atomic<bool>& peer) {
        own.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!peer.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                timedOut.store(true);
                break;
            }
            std::this_thread::yield();
        }
    };
    mc2::overlap::runOnWorker([&] {
        mc2::overlap::setSimulationFrame(73);
        workerFrame.store(mc2::overlap::simulationFrame(5));
        jobs::parallel_for(jobs::Domain::Sim, 257, 1, 1000, [&](int b, int e) {
            rendezvous(simEntered, renderEntered);
            for (int i = b; i < e; ++i) ++sim[i];
        });
    });
    jobs::parallel_for(jobs::Domain::Render, 257, 1, 1000, [&](int b, int e) {
        rendezvous(renderEntered, simEntered);
        for (int i = b; i < e; ++i) ++render[i];
    });
    mc2::overlap::waitWorker();
    CHECK_FALSE(timedOut.load());
    CHECK(workerFrame.load() == 73);
    CHECK(mc2::overlap::simulationFrame(5) == 5);
    for (int i = 0; i < 257; ++i) {
        CHECK(sim[i].load() == 1);
        CHECK(render[i].load() == 1);
    }
}

static int itemValue(int i) { return (i * 2654435761u) ^ (i >> 3); }

static std::vector<int> computeParallelFor(int count, int workers, int batch) {
    std::vector<int> out(static_cast<size_t>(count), 0);
    jobs::startPool(workers);

    const int64_t savedFloor = jobs::kDispatchFloorNs;
    jobs::kDispatchFloorNs = 0;
    jobs::parallel_for(jobs::Domain::Render, count, batch, 1000,
        [&](int b, int e) {
            for (int i = b; i < e; ++i) out[static_cast<size_t>(i)] = itemValue(i);
        });
    jobs::kDispatchFloorNs = savedFloor;
    jobs::stopPool();
    return out;
}

TEST_CASE_FIXTURE(JobsFixture, "job_system_harness/serial_equals_parallel") {
    const int count = 100003;
    const int batch = 64;

    std::vector<int> golden(static_cast<size_t>(count), 0);
    for (int i = 0; i < count; ++i) golden[static_cast<size_t>(i)] = itemValue(i);

    for (int workers : {0, 1, 2, 8}) {
        std::vector<int> got = computeParallelFor(count, workers, batch);
        CHECK((got == golden));
        if (got != golden) {
            std::fprintf(stderr, "  mismatch at workers=%d\n", workers);
            break;
        }
    }
    return;
}

static std::vector<double> magnitudeVariedInput() {
    std::vector<double> v;
    v.reserve(1001);
    v.push_back(1.0e16);
    for (int i = 0; i < 1000; ++i) v.push_back(1.0);
    return v;
}

static double orderedReduce(const std::vector<double>& v, int workers, int batch) {
    jobs::startPool(workers);
    const int64_t savedFloor = jobs::kDispatchFloorNs;
    jobs::kDispatchFloorNs = 0;
    double sum = jobs::parallel_reduce<double>(
        jobs::Domain::Sim, static_cast<int>(v.size()), batch, 1000,
        0.0,
        [&](int b, int e, double& shard) {
            for (int i = b; i < e; ++i) shard += v[static_cast<size_t>(i)];
        },
        [](double& acc, const double& shard) { acc += shard; });
    jobs::kDispatchFloorNs = savedFloor;
    jobs::stopPool();
    return sum;
}

TEST_CASE_FIXTURE(JobsFixture, "job_system_harness/reduce_bit_identical_across_worker_counts") {
    const auto v = magnitudeVariedInput();
    const int batch = 8;
    const double r1 = orderedReduce(v, 1, batch);
    const double r2 = orderedReduce(v, 2, batch);
    const double r8 = orderedReduce(v, 8, batch);
    const double r0 = orderedReduce(v, 0, batch);

    CHECK((dbits(r1) == dbits(r2)));
    CHECK((dbits(r1) == dbits(r8)));
    CHECK((dbits(r1) == dbits(r0)));
    if (dbits(r1) != dbits(r2) || dbits(r1) != dbits(r8) || dbits(r1) != dbits(r0)) {
        std::fprintf(stderr, "  r0=%.20g r1=%.20g r2=%.20g r8=%.20g\n",
                     r0, r1, r2, r8);
    }
    return;
}

TEST_CASE_FIXTURE(JobsFixture, "job_system_harness/negative_control_unordered_diverges") {

    const auto v = magnitudeVariedInput();
    double fwd = 0.0;
    for (size_t i = 0; i < v.size(); ++i) fwd += v[i];
    double rev = 0.0;
    for (size_t i = v.size(); i-- > 0;) rev += v[i];

    CHECK((dbits(fwd) != dbits(rev)));
    if (dbits(fwd) == dbits(rev))
        std::fprintf(stderr, "  NEGATIVE CONTROL VACUOUS: fwd==rev (%.20g)\n", fwd);
    return;
}

TEST_CASE_FIXTURE(JobsFixture, "job_system_harness/reentrancy_degrades_not_deadlock") {

    jobs::startPool(4);
    const int64_t savedFloor = jobs::kDispatchFloorNs;
    jobs::kDispatchFloorNs = 0;
    std::vector<int> out(256, 0);
    jobs::parallel_for(jobs::Domain::Render, 256, 16, 1000, [&](int b, int e) {

        jobs::parallel_for(jobs::Domain::Render, e - b, 4, 1000,
            [&](int ib, int ie) {
                for (int i = b + ib; i < b + ie && i < e; ++i)
                    out[static_cast<size_t>(i)] = i;
            });
    });
    jobs::kDispatchFloorNs = savedFloor;
    jobs::stopPool();

    bool ok = true;
    for (int i = 0; i < 256; ++i) if (out[static_cast<size_t>(i)] != i) { ok = false; break; }
    CHECK((ok));
    return;
}

TEST_CASE_FIXTURE(JobsFixture, "job_system_harness/adaptive_fallback_small_count_runs_serial") {

    jobs::startPool(8);
    jobs::kDispatchFloorNs = 1000000;
    std::vector<int> out(10, -1);
    jobs::parallel_for(jobs::Domain::Render, 10, 4, 10, [&](int b, int e) {
        for (int i = b; i < e; ++i) out[static_cast<size_t>(i)] = itemValue(i);
    });
    jobs::stopPool();
    bool ok = true;
    for (int i = 0; i < 10; ++i) if (out[static_cast<size_t>(i)] != itemValue(i)) { ok = false; break; }
    CHECK((ok));
    return;
}

TEST_CASE_FIXTURE(JobsFixture, "job_system_harness/rand_keyed_order_independent") {

    std::uint32_t a = jobs::rand_keyed(7, 100, 3);
    std::uint32_t b = jobs::rand_keyed(7, 101, 3);
    std::uint32_t a2 = jobs::rand_keyed(7, 100, 3);
    CHECK((a == a2));
    CHECK((a != b));
    return;
}

TEST_CASE_FIXTURE(JobsFixture, "job_system_harness/measure_dispatch_floor") {
    jobs::startPool(std::max(1, 7));
    const int64_t floorNs = jobs::measureDispatchFloorNs(2000);
    jobs::stopPool();
    std::fprintf(stderr, "  [MEASURE] kDispatchFloorNs (median dispatch+join) = %lld ns\n",
                 static_cast<long long>(floorNs));
    CHECK((floorNs > 0));
    return;
}

// A fresh process proves the release-build fatal path without aborting the rest of the corpus.
static int runFatalChild() {
#ifdef _WIN32
    std::wstring executable(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    REQUIRE((size > 0 && size < executable.size()));
    executable.resize(size);
    std::wstring command = L"\"" + executable + L"\" --test-case=job_system_harness/fatal_if_aborts_and_traces --no-breaks";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    DWORD consoles[2]{};
    const DWORD flags = GetConsoleProcessList(consoles, 2) ? 0 : CREATE_NO_WINDOW;
    REQUIRE(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                          flags, nullptr, nullptr, &startup, &process) != FALSE);
    struct CloseProcess {
        PROCESS_INFORMATION p;
        ~CloseProcess() { CloseHandle(p.hThread); CloseHandle(p.hProcess); }
    } owner{process};
    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    if (wait != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 125);
    REQUIRE(wait == WAIT_OBJECT_0);
    DWORD code{};
    REQUIRE(GetExitCodeProcess(process.hProcess, &code) != FALSE);
    return static_cast<int>(code);
#else
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> executable(size);
    REQUIRE(_NSGetExecutablePath(executable.data(), &size) == 0);
    const std::string path = executable.data();
#else
    const std::string path = std::filesystem::read_symlink("/proc/self/exe").string();
#endif
    const pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        execl(path.c_str(), path.c_str(),
              "--test-case=job_system_harness/fatal_if_aborts_and_traces", "--no-breaks", nullptr);
        _exit(125);
    }
    int status{};
    REQUIRE(waitpid(child, &status, 0) == child);
    return status;
#endif
}

TEST_CASE_FIXTURE(JobsFixture, "job_system_harness/fatal_if_aborts_and_traces") {
    if (const char* child = std::getenv("DOCTEST_JOBS_FATAL_CHILD"); child && std::strcmp(child, "1") == 0) {
#ifdef _WIN32
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
        JOBS_FATAL_IF(true);
    }
    namespace fs = std::filesystem;
#ifdef _WIN32
    const long pid = static_cast<long>(_getpid());
#else
    const long pid = static_cast<long>(getpid());
#endif
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path trace = fs::temp_directory_path() /
        ("mc2_jobs_fatal_" + std::to_string(pid) + "_" + std::to_string(nonce) + ".jsonl");
    struct RemoveTrace {
        fs::path path;
        ~RemoveTrace() { std::error_code ec; fs::remove(path, ec); }
    } traceOwner{trace};
    ContractEnvironment sink{"MC2_DIAGNOSTIC_TRACE_FILE", trace.string().c_str()};
    ContractEnvironment child{"DOCTEST_JOBS_FATAL_CHILD", "1"};
    mc2::gate::markMainEntered();
    const int code = runFatalChild();
    CHECK(code != 0);
    std::ifstream input(trace);
    const std::string body{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    CAPTURE(code);
    CAPTURE(trace.string());
    CAPTURE(body);
    CHECK(body.find("JOBS_FATAL") != std::string::npos);
    CHECK(body.find("\"cond\":\"true\"") != std::string::npos);
}

}
