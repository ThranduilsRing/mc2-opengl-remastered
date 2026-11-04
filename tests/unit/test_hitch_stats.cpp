// hitch_stats.h direct-wait counters: count, total, max, staged bytes. Host-side, no Vulkan.
#include "doctest.h"

#include "hitch_stats.h"

TEST_CASE("noteFenceWait counts every wait, keeps the max, refuses a clock fault") {
    auto& s = mc2_hitch2::stats();
    const uint64_t c0 = s.fenceWaitCount.load(), n0 = s.fenceWaitNs.load();
    mc2_hitch2::noteFenceWait("a", 2.0);
    mc2_hitch2::noteFenceWait("b", 5.0);
    mc2_hitch2::noteFenceWait("bad", -1.0);
    CHECK(s.fenceWaitCount.load() - c0 == 2);
    CHECK((s.fenceWaitNs.load() - n0) == 7000000ull);
    CHECK(s.fenceWaitMaxNs.load() >= 5000000ull);
}

TEST_CASE("FenceWaitScope records one wait and noteStaged adds bytes") {
    auto& s = mc2_hitch2::stats();
    const uint64_t c0 = s.fenceWaitCount.load(), b0 = s.stagedBytes.load();
    { mc2_hitch2::FenceWaitScope w("scope"); }
    mc2_hitch2::noteStaged(4096);
    CHECK(s.fenceWaitCount.load() - c0 == 1);
    CHECK(s.stagedBytes.load() - b0 == 4096);
}
