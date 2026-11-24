#include "present_thread.h"
#include "gos_render.h"
#include "present_thread_wait.h"
#include "engine_present_vk.h"
#include "vulkan_context.h"
#include "vk_render_infra.h"
#include "vk_texture_residency.h"
#include "render_backend_select.h"
#include "../../mclib/sim_overlap.h"
#include "../../mclib/battle_bench_wait.h"
#include "Gates/gate.h"
#include "diagnostic_trace.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include <cassert>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mc2vk::present_thread {
namespace {
struct Packet {
    uint32_t width, height, renderFrame, fireFrame;
    bool verify;
    int fps;
    std::vector<ResidentTextureRef> retiredTextures;
    std::vector<uint32_t> retiredHudTextures;
};
std::mutex mutex;
std::condition_variable changed;
std::thread worker;
thread_local bool isWorker = false;
std::atomic<std::thread::id> mainThread{};
bool running = false, quitting = false, pending = false, busy = false, sealed = true, ready = false;
bool rollback = false;
bool failed = false;          // the worker's present path is broken; main recovers at the next frame top
std::string failure;
Packet packet{};
int paceFps = 0;
std::vector<ResidentTextureRef> retiredTextures;
std::vector<uint32_t> retiredHudTextures;

// Past kStall the session rolls back to the inline present (as before). Past kHung the worker is not coming back: a frozen game is worse
// than an exit that names the wait, so leave unless a debugger holds the worker.
constexpr auto kStall = std::chrono::seconds(5);
constexpr auto kHung = std::chrono::seconds(30);

// Cap sleep. std::this_thread::sleep_until rounds to the 1 ms system tick: a 165 fps cap (6.06 ms) slept to 7.0 ms (143 fps), which main
// saw as ~4 ms of Frame.WaitSealed per frame. A high-resolution waitable timer covers all but the last 300 us, a short spin the rest.
void sleepUntilPrecise(detail::Clock::time_point due) {
#ifdef _WIN32
    static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    constexpr auto spin = std::chrono::microseconds(300);
    const auto now = detail::Clock::now();
    if (due - now <= spin) {
    } else if (timer) {
        LARGE_INTEGER t;
        t.QuadPart = -std::chrono::duration_cast<std::chrono::nanoseconds>(due - spin - now).count() / 100;
        if (SetWaitableTimerEx(timer, &t, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer, INFINITE);
    } else {
        std::this_thread::sleep_until(due - spin);
    }
    while (detail::Clock::now() < due) YieldProcessor();
#else
    std::this_thread::sleep_until(due);
#endif
}

bool debuggerAttached() {
#ifdef _WIN32
    return IsDebuggerPresent() != FALSE;
#else
    return false;
#endif
}

// Main-side wait on the worker, `lock` held. Pumps the window thread's sent messages throughout (see present_thread_wait.h).
template <class Pred>
void waitWorker(std::unique_lock<std::mutex>& lock, Pred pred, std::string_view hungReason) {
    if (detail::waitPumping(lock, changed, pred, pumpWindowMessages, kStall)) return;
    std::fprintf(stderr, "[PRESENT_THREAD] STALL >5s; inline rollback after owner quiescence\n");
    rollback = true;
    while (!detail::waitPumping(lock, changed, pred, pumpWindowMessages, kHung - kStall)) {
        if (debuggerAttached()) continue;
        lock.unlock();
        mc2::fatalBackendFailure(std::string(hungReason).c_str());
    }
}

// start(): the worker only flags itself ready, so no stall rollback; past kHung it never started.
template <class Pred>
void waitStarted(std::unique_lock<std::mutex>& lock, Pred pred) {
    while (!detail::waitPumping(lock, changed, pred, pumpWindowMessages, kHung)) {
        if (debuggerAttached()) continue;
        lock.unlock();
        mc2::fatalBackendFailure("present worker never started");
    }
}

void run() {
    isWorker = true;
    mc2::overlap::registerThread(mc2::overlap::Owner::Render);
    {
        std::lock_guard lock(mutex);
        mc2::overlap::setRenderWorkerOwns(false);
        ready = true;
    }
    changed.notify_all();
    for (;;) {
        Packet next;
        bool discard = false;
        {
            std::unique_lock lock(mutex);
            changed.wait(lock, [] { return pending || quitting; });
            if (!pending && quitting) break;
            next = std::move(packet);
            pending = false;
            busy = true;
            discard = detail::discardPresent(failed);
        }
        mc2::overlap::registerThread(mc2::overlap::Owner::Render);
        next.retiredTextures.clear();
        const auto started = detail::Clock::now();
        auto& present = getOrCreateEnginePresentVk();
        present.retireHudTextures(std::move(next.retiredHudTextures));
        if (infra::deviceLostLatched()) {
            // No further queue work on a lost device: decline the frame and let main end the session.
            reportFailure("Vulkan device lost");
            seal();
        } else {
            present.setCaptureFrame(next.renderFrame, next.fireFrame);
            if (!present.presentFrame(discard ? 0 : next.width, discard ? 0 : next.height, !discard && next.verify)) {
                std::fprintf(stderr, "[PRESENT_THREAD] present failed: %s\n", present.health().fallback_reason.c_str());
            }
        }
        // The cap spaces present STARTS, so a late frame cannot buy a short interval for the one behind it. Main records the next frame
        // during this sleep: the frame above is already sealed.
        sleepUntilPrecise(detail::presentNotBefore(started, detail::Clock::now(), next.fps));
        {
            std::lock_guard lock(mutex);
            busy = false;
        }
        changed.notify_all();
    }
}
}

void pumpWindowMessages() {
    graphics::pump_loading_events();
    detail::serviceWindow(std::this_thread::get_id() == mainThread.load(std::memory_order_relaxed), [] {
#ifdef _WIN32
        MSG m;
        PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);   // runs sent messages only; posted input stays for SDL
#endif
    }, graphics::note_window_pumped);
}

void start() {
    mc2::overlap::assertMainThread("present_thread::start");
    std::unique_lock lock(mutex);
    if (running) return;
    mainThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
    running = true;
    quitting = false;
    ready = false;
    mc2::overlap::setPresentWorkerRegistered(true);
    worker = std::thread(run);
    waitStarted(lock, [] { return ready; });
    if (mc2_diag::tagRequested("PRESENT_THREAD"))   // the worker ships ON: the start line is opt-in (stall / failure lines stay fail-loud)
        std::fprintf(stderr, "[PRESENT_THREAD] enabled mailbox_depth=1\n");
}
bool enabled() { return running; }
void hungWait(std::string_view reason) {
    if (!debuggerAttached()) mc2::fatalBackendFailure(std::string(reason).c_str());
}
bool onWorker() { return isWorker; }
void waitSealed(std::source_location site) {
    if (!running || isWorker) return;
    std::unique_lock lock(mutex);
    const auto began = std::chrono::steady_clock::now();
    if (!sealed) mc2::battlebench::noteVkWait(site.function_name(), began);
    mc2::battlebench::WaitScope timed(mc2::battlebench::WaitSealedNs);
    waitWorker(lock, [] { return sealed; }, "Vulkan present worker hung: no seal for 30 s");
    if (rollback && std::this_thread::get_id() == mainThread.load(std::memory_order_relaxed)) {
        lock.unlock();
        stop();
        std::fprintf(stderr, "[PRESENT_THREAD] INLINE FALLBACK active\n");
    }
}
void waitIdle() {
    if (!running || isWorker) return;
    std::unique_lock lock(mutex);
    waitWorker(lock, [] { return detail::drained(pending, busy); }, "Vulkan present worker hung: not idle for 30 s");
}
void enqueue(uint32_t width, uint32_t height, bool verify, uint32_t renderFrame, uint32_t fireFrame) {
    mc2::overlap::assertMainThread("present_thread::enqueue");
    // Recorded islands still reserved record here, on main, before the worker can queue anything: the worker's composite submit must never overtake an
    // unrecorded entry (it only ever queues a prefix, VulkanContext::flushPendingLocked).
    if (engineVulkanContextValid()) getOrCreateEngineVulkanContext().resolveRecorded();
    std::lock_guard lock(mutex);
    assert(detail::canEnqueue(pending, sealed));
    packet = {width, height, renderFrame, fireFrame, verify, paceFps, std::move(retiredTextures), std::move(retiredHudTextures)};
    sealed = false;
    pending = true;
    mc2::overlap::setRenderWorkerOwns(true);
    changed.notify_all();
}
void seal() {
    if (!isWorker) return;
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "present_thread::seal");
    std::lock_guard lock(mutex);
    assert(!sealed);
    mc2::overlap::setRenderWorkerOwns(false);
    mc2::overlap::registerThread(mc2::overlap::Owner::Present);
    sealed = true;
    changed.notify_all();
}
void pace(int fps) {
    mc2::overlap::assertMainThread("present_thread::pace");
    paceFps = fps;
}
void forgetTexture(uint32_t handle) {
    mc2::overlap::assertMainThread("present_thread::forgetTexture");
    retiredHudTextures.push_back(handle);
    auto retired = getOrCreateEngineVulkanContext().textures().detachLegacyUi(handle);
    for (auto& texture : retired) retiredTextures.push_back(std::move(texture));
}
void reportFailure(std::string_view reason) {
    if (!isWorker) return;   // the inline present returns false to its host, which recovers on the spot
    std::lock_guard lock(mutex);
    if (!failed) failure = reason.empty() ? std::string("(unspecified)") : std::string(reason);
    failed = true;
}
bool takeFailure(std::string& reason) {
    mc2::overlap::assertMainThread("present_thread::takeFailure");
    std::unique_lock lock(mutex);
    if (!failed) return false;
    // Post-seal failure may race a new enqueue. The worker retires that packet without WSI; teardown waits for both packets to drain.
    if (running) waitWorker(lock, [] { return detail::drained(pending, busy); }, "Vulkan present worker hung: not idle for 30 s after a failure");
    failed = false;
    reason = std::move(failure);
    failure.clear();
    return true;
}
void stop() {
    mc2::overlap::assertMainThread("present_thread::stop");
    if (!running) return;
    waitIdle();
    {
        std::lock_guard lock(mutex);
        quitting = true;
    }
    changed.notify_all();
    worker.join();
    running = false;
    mc2::overlap::setRenderWorkerOwns(false);
    mc2::overlap::setPresentWorkerRegistered(false);
    retiredTextures.clear();
    for (const auto handle : retiredHudTextures) getOrCreateEnginePresentVk().releaseHudTextures(handle);
    retiredHudTextures.clear();
}
}
