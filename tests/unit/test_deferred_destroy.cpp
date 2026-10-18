// tests/unit/test_deferred_destroy.cpp
// The deferred-destroy queue (vk_deferred_destroy.h): an owner parked by retire() is not destroyed before its ticket completes and is destroyed after.
// The "GPU" is the completion tracker counter, advanced by hand.
#include "doctest.h"
#include "vk_deferred_destroy.h"

#include <vector>

using namespace mc2vk;

namespace {
int g_live = 0;
std::vector<int> g_destroyOrder;
struct Probe {   // stands in for an OwnedBuffer: move-only, counts its live instances
    int id;
    explicit Probe(int i) : id(i) { ++g_live; }
    Probe(Probe&& o) noexcept : id(o.id) { o.id = -1; ++g_live; }
    Probe(const Probe&) = delete;
    ~Probe() { --g_live; if (id >= 0) g_destroyOrder.push_back(id); }
};
// The negative control: the same interface with the eager policy (destroy at retire). A use-after-retire check must catch it.
struct EagerQueue {
    bool usedAfterRetireOk = true;
    template <class O> void retire(O&& o) { O dead(std::move(o)); usedAfterRetireOk = false; }   // the owner dies here, while the GPU may still read it
};
}  // namespace

TEST_SUITE("DeferredDestroy") {

TEST_CASE("an entry is not destroyed before its ticket and is destroyed after") {
    g_live = 0; g_destroyOrder.clear();
    CompletionTracker ct;
    DeferredDestroy dd;
    ct.beginFrame();
    ct.noteSubmit(QueueKind::Graphics);                  // value 1: the frame work
    dd.retire(ct, Probe(7), 64);
    CHECK(g_live == 1);
    dd.collect(ct);
    CHECK(g_live == 1);                                  // the frame is still open: unsealed entries never go
    ct.endFrame();
    dd.sealFrame(ct.frameTicket(ct.frameNo()));
    dd.collect(ct);
    CHECK(g_live == 1);                                  // sealed with value 1, the GPU is at 0
    ct.setCompleted(QueueKind::Graphics, 1);
    CHECK(dd.collect(ct) == 1);
    CHECK(g_live == 0);
    CHECK(dd.size() == 0);
    CHECK(dd.bytes() == 0);
}

TEST_CASE("an owner retired in a frame waits for work submitted after the retire in that frame") {
    g_live = 0;
    CompletionTracker ct;
    DeferredDestroy dd;
    ct.noteSubmit(QueueKind::Graphics);                  // 1: an earlier frame submit, already done
    ct.setCompleted(QueueKind::Graphics, 1);
    ct.beginFrame();
    dd.retire(ct, Probe(1));                             // retired first ...
    ct.noteSubmit(QueueKind::Graphics);                  // ... and the frame still records a use before this submit (value 2)
    ct.endFrame();
    dd.sealFrame(ct.frameTicket(ct.frameNo()));
    dd.collect(ct);
    CHECK(g_live == 1);                                  // an outside-frame ticket (1) would have freed it here: the frame ticket (2) does not
    ct.setCompleted(QueueKind::Graphics, 2);
    dd.collect(ct);
    CHECK(g_live == 0);
}

TEST_CASE("outside a frame the ticket is everything submitted so far") {
    g_live = 0;
    CompletionTracker ct;
    DeferredDestroy dd;
    ct.noteSubmit(QueueKind::Graphics);
    ct.noteSubmit(QueueKind::Graphics);
    dd.retire(ct, Probe(1));
    dd.collect(ct);
    CHECK(g_live == 1);
    ct.setCompleted(QueueKind::Graphics, 2);
    dd.collect(ct);
    CHECK(g_live == 0);
}

TEST_CASE("destruction runs newest insertion first, and drainAll empties the queue in that order") {
    g_live = 0; g_destroyOrder.clear();
    CompletionTracker ct;
    DeferredDestroy dd;
    ct.noteSubmit(QueueKind::Graphics);
    dd.retire(ct, Probe(1));
    dd.retire(ct, Probe(2));
    dd.retire(ct, Probe(3));
    ct.setCompleted(QueueKind::Graphics, 1);
    dd.collect(ct);
    CHECK(g_destroyOrder == std::vector<int>({3, 2, 1}));   // views are retired after their image, so they go first
    dd.retire(ct, Probe(4));
    dd.retire(ct, Probe(5));
    CHECK(dd.drainAll() == 2);
    CHECK(g_live == 0);
    CHECK(g_destroyOrder == std::vector<int>({3, 2, 1, 5, 4}));
}

TEST_CASE("retireAfter honours an explicit ticket and stats count bytes") {
    g_live = 0;
    CompletionTracker ct;
    DeferredDestroy dd;
    for (int f = 0; f < 3; ++f) { ct.beginFrame(); ct.noteSubmit(QueueKind::Graphics); ct.endFrame(); }
    dd.retireAfter(ct, GpuTicket{9, 0}, Probe(1), 100);
    CHECK(dd.stats(ct).entries == 1);
    CHECK(dd.stats(ct).bytes == 100);
    ct.setCompleted(QueueKind::Graphics, 8);
    dd.collect(ct);
    CHECK(g_live == 1);
    ct.setCompleted(QueueKind::Graphics, 9);
    dd.collect(ct);
    CHECK(g_live == 0);
}

TEST_CASE("negative control: destroying at retire fails the use-after-retire check the queue passes") {
    g_live = 0;
    CompletionTracker ct;
    DeferredDestroy dd;
    ct.noteSubmit(QueueKind::Graphics);
    dd.retire(ct, Probe(1));
    const bool deferredStillAlive = (g_live == 1);       // the GPU may still read it
    EagerQueue eager;
    eager.retire(Probe(2));
    CHECK(deferredStillAlive);
    CHECK_FALSE(eager.usedAfterRetireOk);                // the eager policy is caught by the same assertion
    ct.setCompleted(QueueKind::Graphics, 1);
    dd.collect(ct);
    CHECK(g_live == 0);
}

}  // TEST_SUITE
