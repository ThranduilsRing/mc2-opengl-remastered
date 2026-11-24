// Present worker waits and pacing (present_thread_wait.h): main keeps the window thread's queue serviced while it waits on the worker,
// gives up at a limit instead of hanging, sees a worker failure at the seal, and the cap spaces present starts by a full period.
#include "doctest.h"
#include "present_thread_wait.h"
#include "present_thread.h"
#include "gos_render.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;
namespace d = mc2vk::present_thread::detail;

// Trigger paths: code/loadscreen.cpp, GameOS/gameos/gos_render.h.
TEST_CASE("loading label: wall-time dots and named stages") {
    CHECK(graphics::loading_dots(0) == 0);
    CHECK(graphics::loading_dots(333) == 0);
    CHECK(graphics::loading_dots(334) == 1);
    CHECK(graphics::loading_dots(667) == 2);
    CHECK(graphics::loading_dots(1000) == 3);
    CHECK(graphics::loading_dots(1334) == 0);
    CHECK(graphics::loading_label("mission_settings_ready") == "Terrain");
    CHECK(graphics::loading_label("terrain_textures") == "Terrain textures");
    CHECK(graphics::loading_label("vegetation_pack") == "Vegetation");
    CHECK(graphics::loading_label("prop_materials") == "Materials");
    CHECK(graphics::loading_label("mission_ready") == "Ready");
}

// Trigger paths: gos_render.cpp, present_thread_wait.h, vk_render_infra.cpp, terrtxm2_load.cpp.
TEST_CASE("loading work: pumps while blocked and joins before returning") {
    std::atomic<bool> release = false;
    int pumps = 0, output = 0;
    const auto mainThread = std::this_thread::get_id();
    d::workPumping([&] {
        CHECK(std::this_thread::get_id() != mainThread);
        while (!release.load()) std::this_thread::yield();
        output = 42;
    }, [&] {
        CHECK(std::this_thread::get_id() == mainThread);
        if (++pumps == 3) release = true;
    });
    CHECK(pumps == 3);
    CHECK(output == 42);
}

TEST_CASE("loading work: propagates a worker failure after joining") {
    CHECK_THROWS_AS(d::workPumping([] { throw std::runtime_error("decode failed"); }, [] {}), std::runtime_error);
}

// Trigger paths: GameOS/gameos/gos_render.{h,cpp}, gameosmain.cpp.
TEST_CASE("loading pump: longest actual interval retains its phase and resets") {
    graphics::LoadingPumpStats stats;
    stats.begin(1000);
    stats.pumped(1050);
    stats.phase = "terrain";
    stats.pumped(6250);
    stats.phase = "models";
    stats.pumped(6300);
    CHECK(stats.maxGap == 5200);
    CHECK(stats.maxPhase == "terrain");
    stats.phase = {};
    stats.pumped(50000);
    CHECK(stats.maxGap == 5200);
    stats.begin(60000);
    stats.pumped(60099);
    CHECK(stats.maxGap == 99);
    CHECK(stats.maxPhase == "mission_load_start");
}

TEST_CASE("present split: blocked acquire releases Render after exactly one seal") {
    std::mutex mutex;
    std::condition_variable cv;
    bool finished = false, sealed = false, acquireEntered = false, releaseAcquire = false;
    int seals = 0;
    std::thread worker([&] {
        CHECK(d::afterRenderTail([&] {
            std::lock_guard lock(mutex);
            finished = true;
            return true;
        }, [&] {
            std::lock_guard lock(mutex);
            CHECK(finished);
            sealed = true;
            ++seals;
            cv.notify_all();
        }, [&] {
            std::unique_lock lock(mutex);
            acquireEntered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return releaseAcquire; });
            return true;
        }));
    });
    std::unique_lock lock(mutex);
    const bool entered = d::waitPumping(lock, cv, [&] { return acquireEntered; }, [] {}, 5s);
    CHECK(entered);
    CHECK(finished);
    CHECK(sealed);
    CHECK(seals == 1);
    CHECK(d::canEnqueue(false, sealed));
    releaseAcquire = true;
    cv.notify_all();
    lock.unlock();
    worker.join();
}

TEST_CASE("present split: late failure drains a pending packet before teardown") {
    // Main enqueues N+1 during N's acquire. N then fails; the same production predicates keep teardown waiting for N+1.
    bool pending = false, busy = true, failed = false;
    int seals = 0, wsi = 0;
    CHECK_FALSE(d::afterRenderTail([] { return true; }, [&] { ++seals; }, [&] {
        REQUIRE(d::canEnqueue(pending, true));
        pending = true;
        ++wsi;
        failed = true;
        return false;
    }));
    CHECK_FALSE(d::drained(pending, busy));
    busy = false;
    CHECK_FALSE(d::drained(pending, busy));
    CHECK(d::discardPresent(failed));
    pending = false;
    busy = true;
    CHECK(d::afterRenderTail([] { return true; }, [&] { ++seals; }, [&] {
        if (!d::discardPresent(failed)) ++wsi;
        return true;
    }));
    CHECK_FALSE(d::drained(pending, busy));
    busy = false;
    CHECK(d::drained(pending, busy));
    CHECK(seals == 2);
    CHECK(wsi == 1);
    CHECK_FALSE(d::canEnqueue(true, true));
    CHECK_FALSE(d::canEnqueue(false, false));
}

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
    // A requested 2ms wait may sleep a full Windows timer tick. The deadline
    // bounds the wait, not the number of times this thread gets scheduled.
    CHECK(pumps >= 1);

    // Exercise repeated slices independently of the timer resolution. Each
    // callback must run unlocked; the fifth pump satisfies the predicate.
    pumps = 0;
    CHECK(d::waitPumping(lock, cv, [&] { return pumps == 5; }, [&] {
        CHECK_FALSE(lock.owns_lock());
        ++pumps;
    }, 5s, 2ms));
    CHECK(pumps == 5);
    CHECK(lock.owns_lock());
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

TEST_CASE("present acquire: bounded timeout and cumulative drop reports survive repeated occlusion") {
    CHECK(d::acquireTimeoutNs == 250000000);
    uint64_t dropped = 0;
    int reports = 0;
    for (uint64_t i = 1; i <= 65; ++i) {
        const bool report = d::noteAcquireDrop(dropped);
        CHECK(dropped == i);
        CHECK(report == (i == 1 || i % 16 == 0));
        if (report) ++reports;
    }
    CHECK(reports == 5);
}
