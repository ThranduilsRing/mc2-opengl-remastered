// Present worker waits and pacing (present_thread_wait.h): main keeps the window thread's queue serviced while it waits on the worker,
// gives up at a limit instead of hanging, sees a worker failure at the seal, and the cap spaces present starts by a full period.
#include "doctest.h"
#include "present_thread_wait.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;
namespace d = mc2vk::present_thread::detail;

TEST_CASE("present wait: the pump unblocks a worker stuck in a WSI SendMessage to main") {
    // The worker cannot finish until main services its sent message (the pump). A plain cv wait here would deadlock.
    std::mutex m;
    std::condition_variable cv;
    bool sealed = false;
    std::atomic<bool> serviced{false};
    std::atomic<int> pumps{0};
    std::thread worker([&] {
        while (!serviced.load()) std::this_thread::yield();   // blocked in vkQueuePresentKHR -> SendMessage(main)
        { std::lock_guard l(m); sealed = true; }
        cv.notify_all();
    });
    std::unique_lock lock(m);
    const bool ok = d::waitPumping(lock, cv, [&] { return sealed; }, [&] { ++pumps; serviced = true; }, 5s);
    CHECK(lock.owns_lock());
    lock.unlock();
    worker.join();
    CHECK(ok);
    CHECK(pumps.load() >= 1);
}

TEST_CASE("present wait: bounded, pumping every slice, lock held on return") {
    std::mutex m;
    std::condition_variable cv;
    int pumps = 0;
    std::unique_lock lock(m);
    const auto t0 = d::Clock::now();
    const bool ok = d::waitPumping(lock, cv, [] { return false; }, [&] { ++pumps; }, 40ms, 2ms);
    const auto took = d::Clock::now() - t0;
    CHECK_FALSE(ok);
    CHECK(lock.owns_lock());
    CHECK(took >= 40ms);
    CHECK(took < 2s);
    CHECK(pumps >= 5);
}

TEST_CASE("present wait: an already-true predicate returns without pumping") {
    std::mutex m;
    std::condition_variable cv;
    int pumps = 0;
    std::unique_lock lock(m);
    CHECK(d::waitPumping(lock, cv, [] { return true; }, [&] { ++pumps; }, 0s));
    CHECK(pumps == 0);
}

TEST_CASE("present handoff: a worker failure is visible to main as soon as the seal wait returns") {
    // Production order (present_thread.cpp run(), engine_present_vk.cpp presentFrame): reportFailure under the mailbox mutex, then seal.
    // Main's waitSealed is followed by recoverAsyncPresentFailure, so it must never see the seal without the failure.
    for (int i = 0; i < 200; ++i) {
        std::mutex m;
        std::condition_variable cv;
        bool sealed = false, failed = false;
        std::string failure;
        std::thread worker([&] {
            { std::lock_guard l(m); failed = true; failure = "vkAcquireNextImageKHR failed (-4)"; }
            { std::lock_guard l(m); sealed = true; }
            cv.notify_all();
        });
        std::unique_lock lock(m);
        REQUIRE(d::waitPumping(lock, cv, [&] { return sealed; }, [] {}, 5s));
        CHECK(failed);
        CHECK(failure == "vkAcquireNextImageKHR failed (-4)");
        lock.unlock();
        worker.join();
    }
}

TEST_CASE("present pacing: the period keeps sub-millisecond precision") {
    CHECK(d::presentPeriod(0) == d::Clock::duration::zero());
    CHECK(d::presentPeriod(-1) == d::Clock::duration::zero());
    CHECK(std::chrono::duration_cast<std::chrono::nanoseconds>(d::presentPeriod(60)).count() == 16'666'666);
    const auto p144 = std::chrono::duration_cast<std::chrono::microseconds>(d::presentPeriod(144)).count();
    CHECK(p144 == 6944);   // 1000/144 truncated to 6 ms would run the cap at 166 fps
}

TEST_CASE("present pacing: a late frame never earns a short interval for the next") {
    const int fps = 100;   // 10 ms
    const auto period = d::presentPeriod(fps);
    d::Clock::time_point t{};
    d::Clock::time_point lastStart = t;
    // Alternate a 25 ms present (late) with a 1 ms one; every packet is already waiting when the worker frees up.
    for (int i = 0; i < 20; ++i) {
        const auto presentTook = (i % 2 == 0) ? 25ms : 1ms;
        const auto end = lastStart + presentTook;
        const auto nextStart = d::presentNotBefore(lastStart, end, fps);
        CHECK(nextStart - lastStart >= period);
        CHECK(nextStart >= end);
        lastStart = nextStart;
    }
    // Uncapped: next start is now.
    CHECK(d::presentNotBefore(t, t + 3ms, 0) == t + 3ms);
}
