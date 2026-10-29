// G7 sim/render overlap worker and audit hooks. Design and the shared-structure audit: docs/plans/2026-10-28-g7-overlap-audit.md.
#include "../../mclib/sim_overlap.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
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
constexpr int kUnregistered = -1;
thread_local int t_role = kUnregistered;   // Owner value once the thread registered
thread_local bool t_onSimWorker = false;
thread_local uint32_t t_simFrame = 0;
std::atomic<bool> g_renderThread{false};   // a dedicated render thread registered: Main no longer owns Render
std::atomic<bool> g_inFlight{false};
std::atomic<unsigned> g_violations{0};

struct Worker {
    std::mutex m;
    std::condition_variable cv;        // wakes the worker (job posted) and the joiner (job finished)
    std::function<void()> job;
    bool posted = false;
    bool started = false;
};
Worker g_worker;

void workerMain() {
    registerThread(Owner::Sim);
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

void violation(std::string_view kind, std::string_view site, bool hard = false) {
    const unsigned n = g_violations.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool abortNow = hard || mc2::gate::get<"MC2_SIM_OVERLAP_AUDIT">();
    if (n <= 20 || abortNow) {
        std::fprintf(stderr, "[SIM_OVERLAP v1] VIOLATION %.*s site=%.*s count=%u%s\n",
            static_cast<int>(kind.size()), kind.data(), static_cast<int>(site.size()), site.data(), n, abortNow ? " abort=1" : "");
        std::fflush(stderr);
    }
    if (abortNow) std::abort();
}

const char* ownerName(Owner o) { return o == Owner::Main ? "main" : o == Owner::Sim ? "sim" : "render"; }

// True when the calling thread may enter `domain`. Unregistered threads (job pool, tools) are not judged.
bool owns(Owner domain) {
    if (t_role == kUnregistered) return true;
    const Owner me = static_cast<Owner>(t_role);
    switch (domain) {
    case Owner::Main:   return me == Owner::Main;
    case Owner::Render: return me == Owner::Render || (me == Owner::Main && !g_renderThread.load(std::memory_order_acquire));
    case Owner::Sim:    return me == Owner::Sim || (me == Owner::Main && !g_inFlight.load(std::memory_order_acquire));
    }
    return false;
}

void checkOwner(Owner domain, std::string_view site, bool hard) {
    if (owns(domain)) return;
    char kind[64];
    std::snprintf(kind, sizeof kind, "%s-thread-called-%s-api", ownerName(static_cast<Owner>(t_role)), ownerName(domain));
    violation(kind, site, hard);
}

} // namespace

void registerThread(Owner role) {
    t_role = static_cast<int>(role);
    t_onSimWorker = role == Owner::Sim;
    if (role == Owner::Render) g_renderThread.store(true, std::memory_order_release);
}
void assertOwner(Owner domain, std::string_view site) { checkOwner(domain, site, false); }
void requireOwner(Owner domain, std::string_view site) { checkOwner(domain, site, true); }

void installHooks(const Hooks& hooks) { g_hooks = hooks; }
void extractRender() { if (g_hooks.extract) g_hooks.extract(); }
void beginFrameDraw() {
    if (g_hooks.kick) g_hooks.kick();
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

} // namespace mc2::overlap
