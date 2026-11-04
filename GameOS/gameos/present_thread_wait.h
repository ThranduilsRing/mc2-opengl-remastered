// present_thread_wait.h - the pure pieces of the present worker's mailbox (no Vulkan, no SDL), so tests/unit can drive them.
//
// WHAT: the main-thread wait that keeps the window thread's message queue serviced, and the present-cap deadline.
// WHY: a Win32 WSI call on the worker (acquire, present, swapchain create) may SendMessage to the window-owning thread, which is main. A main
// thread parked in a plain condition wait never answers, so the worker never returns and the wait never ends. Every wait here hands each
// slice to `pump` with the lock released, and gives up at a limit so the caller can recover or fail instead of hanging.
#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace mc2vk::present_thread::detail {
using Clock = std::chrono::steady_clock;

// Wait for pred() with `lock` held on entry and on return. Returns false only when `limit` passed with pred() still false.
template <class Pred, class Pump>
bool waitPumping(std::unique_lock<std::mutex>& lock, std::condition_variable& cv, Pred pred, Pump&& pump, Clock::duration limit,
                 Clock::duration slice = std::chrono::milliseconds(2)) {
    const auto end = Clock::now() + limit;
    while (!pred()) {
        const auto now = Clock::now();
        if (now >= end) return false;
        if (cv.wait_for(lock, (end - now) < slice ? (end - now) : slice, pred)) return true;
        lock.unlock();
        pump();
        lock.lock();
    }
    return true;
}

// Present-start spacing for a cap, in the clock's own ticks: 144 fps is 6.944 ms, not the 6 ms that 1000/fps gives. 0 = uncapped.
constexpr Clock::duration presentPeriod(int fps) {
    return fps > 0 ? std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(1'000'000'000LL / fps)) : Clock::duration::zero();
}
// When the worker may take the next packet after a present that STARTED at `started`: one full period later, or now if that has passed.
// Spacing starts (not ends) means a late frame never earns a short interval for the frame behind it.
constexpr Clock::time_point presentNotBefore(Clock::time_point started, Clock::time_point now, int fps) {
    const auto due = started + presentPeriod(fps);
    return due > now ? due : now;
}
}  // namespace mc2vk::present_thread::detail
