// hitch_stats.h wait ledger: two kinds, window math, nesting, behind-upload attribution, staged bytes. Host-side, no Vulkan.
#include "doctest.h"

#include "hitch_stats.h"

using namespace mc2_hitch2;

TEST_CASE("one-shot waits count every wait, keep the max, refuse a clock fault") {
    auto& w = stats().wait[kOneShot];
    const uint64_t c0 = w.count.load(), n0 = w.ns.load();
    noteWait(kOneShot, "a", 2.0);
    noteWait(kOneShot, "b", 5.0);
    noteWait(kOneShot, "bad", -1.0);
    CHECK(w.count.load() - c0 == 2);
    CHECK((w.ns.load() - n0) == 7000000ull);
    CHECK(w.maxNs.load() >= 5000000ull);
}

TEST_CASE("slot waits under 0.1 ms are not stalls") {
    auto& w = stats().wait[kSlot];
    const uint64_t c0 = w.count.load();
    noteWait(kSlot, "fast", 0.02);
    CHECK(w.count.load() == c0);
    noteWait(kSlot, "slow", 0.5);
    CHECK(w.count.load() == c0 + 1);
}

TEST_CASE("window is now minus the snapshot; beginWaitWindow resets the window max but not the lifetime max") {
    noteWait(kOneShot, "before", 9.0);
    const Snapshot base = takeSnapshot();
    beginWaitWindow();
    noteWait(kOneShot, "in", 3.0);
    const Snapshot now = takeSnapshot();
    CHECK(now.waitN[kOneShot] - base.waitN[kOneShot] == 1);
    CHECK(now.waitNs[kOneShot] - base.waitNs[kOneShot] == 3000000ull);
    CHECK(stats().wait[kOneShot].windowMaxNs.load() == 3000000ull);
    CHECK(stats().wait[kOneShot].maxNs.load() >= 9000000ull);
}

TEST_CASE("a nested one-shot scope does not double count") {
    auto& w = stats().wait[kOneShot];
    const uint64_t c0 = w.count.load();
    {
        FenceWaitScope outer("outer");
        CHECK(inOneShotScope());
        { FenceWaitScope inner("inner"); }
    }
    CHECK(!inOneShotScope());
    CHECK(w.count.load() - c0 == 1);
}

TEST_CASE("a wait with queued upload bytes is attributed as behind-upload") {
    auto& s = stats();
    noteUploadsFlushed();
    CHECK(pendingUploadBytes() == 0);
    s.uploadBytes.fetch_add(8192);
    CHECK(pendingUploadBytes() == 8192);
    const uint64_t b0 = s.wait[kOneShot].behindNs.load();
    noteWait(kOneShot, "behind", 2.0, pendingUploadBytes());
    CHECK(s.wait[kOneShot].behindNs.load() - b0 == 2000000ull);
    noteUploadsFlushed();
    CHECK(pendingUploadBytes() == 0);
    noteWait(kOneShot, "clear", 2.0, pendingUploadBytes());
    CHECK(s.wait[kOneShot].behindNs.load() - b0 == 2000000ull);
}

TEST_CASE("noteStaged adds bytes") {
    const uint64_t b0 = stats().stagedBytes.load();
    noteStaged(4096);
    CHECK(stats().stagedBytes.load() - b0 == 4096);
}
