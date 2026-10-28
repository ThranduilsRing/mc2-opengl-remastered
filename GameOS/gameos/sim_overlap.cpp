// G7 sim/render overlap worker and audit hooks. Design and the shared-structure audit: docs/plans/2026-10-28-g7-overlap-audit.md.
#include "../../mclib/sim_overlap.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <map>
#include <string>
#include <thread>

#include "Gates/gate.h"
#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#endif

namespace mc2::overlap {
namespace {

Hooks g_hooks{};
thread_local bool t_onSimWorker = false;
thread_local uint32_t t_simFrame = 0;
std::atomic<bool> g_inFlight{false};
std::atomic<unsigned> g_violations{0};
std::mutex g_hazardMutex;
std::map<std::string, unsigned> g_hazards;
unsigned g_drawFrames = 0;
bool earlyKick() { return mc2::gate::is<"MC2_SIM_OVERLAP_DIAGNOSTIC", "early">(); }

struct Worker {
    std::mutex m;
    std::condition_variable cv;        // wakes the worker (job posted) and the joiner (job finished)
    std::function<void()> job;
    bool posted = false;
    bool started = false;
};
Worker g_worker;

void workerMain() {
    t_onSimWorker = true;
#ifdef TRACY_ENABLE
    tracy::SetThreadName("SimOverlap");
#endif
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(g_worker.m);
            g_worker.cv.wait(lk, [] { return g_worker.posted; });
            job = std::move(g_worker.job);
            g_worker.posted = false;
        }
        {
#ifdef TRACY_ENABLE
            ZoneScopedN("SimOverlap.Tick");
#endif
            job();
        }
        {
            std::lock_guard<std::mutex> lk(g_worker.m);
            g_inFlight.store(false, std::memory_order_release);
        }
        g_worker.cv.notify_all();
    }
}

#ifdef _WIN32
unsigned __stdcall workerThunk(void*) { workerMain(); return 0; }
#endif

void startWorker() {
#ifdef _WIN32
    // 8 MB stack: the ABL VM and the path/LOS recursion run here instead of on the main thread.
    HANDLE h = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 8u << 20, workerThunk, nullptr, 0, nullptr));
    if (h) CloseHandle(h);
#else
    std::thread(workerMain).detach();
#endif
}

void violation(std::string_view kind, std::string_view site) {
    const unsigned n = g_violations.fetch_add(1, std::memory_order_relaxed) + 1;
    { std::lock_guard<std::mutex> lock(g_hazardMutex); ++g_hazards[std::string(site)]; }
    if (n <= 20) std::fprintf(stderr, "[SIM_OVERLAP v1] VIOLATION %.*s site=%.*s count=%u\n",
        static_cast<int>(kind.size()), kind.data(), static_cast<int>(site.size()), site.data(), n);
    if (mc2::gate::get<"MC2_SIM_OVERLAP_AUDIT">()) std::abort();
}

} // namespace

void installHooks(const Hooks& hooks) { g_hooks = hooks; }
void extractRender() { if (g_hooks.extract) g_hooks.extract(); }
void kickTail() { if (!earlyKick() && g_hooks.kick) g_hooks.kick(); }
void beginFrameDraw() {
    ++g_drawFrames;
    if (earlyKick() && g_hooks.kick) g_hooks.kick();
}
void printHazards() {
    std::lock_guard<std::mutex> lock(g_hazardMutex);
    std::fprintf(stderr, "[G7_HAZARDS v1] mode=%s frames=%u violations=%u sites=", earlyKick() ? "early" : "tail", g_drawFrames, violationCount());
    for (const auto& [site, count] : g_hazards)
        std::fprintf(stderr, "%s:%u/%.6f,", site.c_str(), count, g_drawFrames ? double(count) / g_drawFrames : 0.0);
    std::fprintf(stderr, "\n");
}
void joinTail() { if (g_hooks.join) g_hooks.join(); }

void runOnWorker(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lk(g_worker.m);
        if (!g_worker.started) { g_worker.started = true; startWorker(); }
        g_worker.job = std::move(job);
        g_worker.posted = true;
        g_inFlight.store(true, std::memory_order_release);
    }
    g_worker.cv.notify_all();
}

void waitWorker() {
    if (!g_inFlight.load(std::memory_order_acquire)) return;
    std::unique_lock<std::mutex> lk(g_worker.m);
    g_worker.cv.wait(lk, [] { return !g_inFlight.load(std::memory_order_acquire); });
}

bool jobInFlight() { return g_inFlight.load(std::memory_order_acquire); }
bool onSimWorker() { return t_onSimWorker; }
uint32_t simulationFrame(uint32_t serialFrame) { return t_onSimWorker ? t_simFrame : serialFrame; }
void setSimulationFrame(uint32_t frame) { t_simFrame = frame; }

void assertSimQuiescent(std::string_view site) {
    if (g_inFlight.load(std::memory_order_acquire) && !t_onSimWorker) violation("render-read-while-tick-in-flight", site);
}

void assertMainThread(std::string_view site) {
    if (t_onSimWorker) violation("sim-worker-called-main-thread-api", site);
}

unsigned violationCount() { return g_violations.load(std::memory_order_relaxed); }

} // namespace mc2::overlap
