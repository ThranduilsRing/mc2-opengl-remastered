// tests/unit/test_record_pools.cpp
// Parallel command recording (vk_record_pools.h): N threads record into their own lanes, the merged list keeps canonical island order, a lane is never
// shared between threads, a slot restart hands the lane's buffers out again. No Vulkan: a "command buffer" is an integer minted by the create callback.
#include "doctest.h"
#include <atomic>
#include <set>
#include <thread>
#include <vector>
#include "job_system.h"
#include "vk_record_pools.h"

using namespace mc2vk;

namespace {
struct JobsScope {
    explicit JobsScope(int workers) { mc2::jobs::startPool(workers); mc2::jobs::kDispatchFloorNs = 0; }
    ~JobsScope() { mc2::jobs::stopPool(); }
};
}  // namespace

TEST_CASE("record_pools/parallel_islands_merge_in_canonical_order") {
    for (const int workers : {0, 1, 3, 7}) {
        CAPTURE(workers);
        JobsScope jobs(workers);
        RecordPools<uint64_t> pools;
        std::atomic<uint64_t> minted{0};
        std::vector<std::atomic<int>> laneUsers(kMaxRecordParticipants);

        constexpr int kIslands = 64;
        std::vector<uint64_t> cbOfIsland(kIslands, 0);      // slot i belongs to island i alone: no synchronisation needed
        std::vector<uint32_t> laneOfIsland(kIslands, kNoParticipant);
        mc2::jobs::parallel_for(mc2::jobs::Domain::Render, kIslands, 1, 1000, [&](int begin, int end) {
            for (int i = begin; i < end; ++i) {
                const uint64_t cb = pools.acquire(1, [&](uint32_t, uint32_t) { return ++minted; });   // "a new pool buffer"
                REQUIRE(cb != 0);
                cbOfIsland[i] = cb;
                laneOfIsland[i] = pools.participantOfCaller();
                // "Record": the lane's owner is the only thread that ever touches it.
                laneUsers[laneOfIsland[i]].fetch_add(1, std::memory_order_relaxed);
            }
        });

        // Canonical merge: island index order, whichever thread recorded each.
        std::vector<uint64_t> merged;
        for (int i = 0; i < kIslands; ++i) merged.push_back(cbOfIsland[i]);
        CHECK(merged.size() == kIslands);
        CHECK(std::set<uint64_t>(merged.begin(), merged.end()).size() == kIslands);   // every island got its own buffer

        // One lane per participant thread, every buffer in exactly one lane, and the lane counts add up.
        uint32_t total = 0;
        for (uint32_t p = 0; p < kMaxRecordParticipants; ++p) {
            CHECK(pools.used(1, p) == pools.owned(1, p));
            total += pools.used(1, p);
        }
        CHECK(total == kIslands);
        CHECK(pools.participants() <= static_cast<uint32_t>(workers) + 1);   // pool workers plus main
        CHECK(pools.used(0, 0) == 0);                                          // another frame slot was never touched
    }
}

TEST_CASE("record_pools/slot_restart_reuses_buffers_and_unproven_slot_does_not") {
    RecordPools<uint64_t> pools;
    uint64_t minted = 0;
    int resets = 0;
    auto create = [&](uint32_t, uint32_t) { return ++minted; };
    const uint64_t a = pools.acquire(2, create);
    const uint64_t b = pools.acquire(2, create);
    CHECK(a != b);
    pools.beginFrame(2, /*restart=*/false, [&](uint32_t, uint32_t) { ++resets; });
    CHECK(resets == 0);
    CHECK(pools.acquire(2, create) == 3);          // keeps allocating past what the unproven frame recorded
    pools.beginFrame(2, true, [&](uint32_t, uint32_t) { ++resets; });
    CHECK(resets == 1);                            // one lane in use
    CHECK(pools.acquire(2, create) == a);          // handed out again in order, nothing new minted
    CHECK(pools.acquire(2, create) == b);
    CHECK(minted == 3);
}

TEST_CASE("record_pools/threads_beyond_the_lane_budget_get_no_lane") {
    RecordPools<uint64_t> pools;
    std::atomic<uint64_t> minted{0};
    std::atomic<int> refused{0}, arrived{0};
    constexpr uint32_t kThreads = kMaxRecordParticipants + 4;
    std::vector<std::thread> threads;
    for (uint32_t t = 0; t < kThreads; ++t)
        threads.emplace_back([&] {
            arrived.fetch_add(1);
            while (arrived.load() < static_cast<int>(kThreads)) std::this_thread::yield();   // all alive at once: thread ids are not reused
            if (pools.acquire(0, [&](uint32_t, uint32_t) { return ++minted; }) == 0) refused.fetch_add(1);
        });
    for (auto& th : threads) th.join();
    CHECK(refused == 4);
    CHECK(pools.participants() == kMaxRecordParticipants);
}
