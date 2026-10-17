// tests/unit/test_readback.cpp
// The slot/ticket/tag logic of the Vulkan readback ring (vk_readback.h): a result is readable only after its ticket completed, never for the wrong tag,
// never from a recycled slot. The "GPU" is the completion tracker counter, advanced by hand.
#include "doctest.h"
#include "vk_readback.h"

using namespace mc2vk;

TEST_SUITE("ReadbackCore") {

TEST_CASE("pending until the ticket completes, then ready") {
    CompletionTracker ct;
    ReadbackCore rb;
    const ReadbackToken t = rb.request(/*tag=*/42, ct);
    REQUIRE(t);
    CHECK(rb.poll(t, ct, 42) == ReadbackState::Pending);   // not sealed yet
    ct.noteSubmit(QueueKind::Graphics);
    rb.seal(t, ct.now());
    CHECK(rb.poll(t, ct, 42) == ReadbackState::Pending);   // the GPU has not got there
    ct.setCompleted(QueueKind::Graphics, 1);
    CHECK(rb.poll(t, ct, 42) == ReadbackState::Ready);
}

TEST_CASE("negative control: a result for the wrong tag is stale and is never ready") {
    CompletionTracker ct;
    ReadbackCore rb;
    const ReadbackToken t = rb.request(7, ct);
    ct.noteSubmit(QueueKind::Graphics);
    rb.seal(t, ct.now());
    ct.setCompleted(QueueKind::Graphics, 1);
    CHECK(rb.poll(t, ct, 7) == ReadbackState::Ready);
    CHECK(rb.poll(t, ct, 8) == ReadbackState::Stale);       // the consumer moved on to another view/cursor/frame: discard
    CHECK(rb.poll(t, ct, 6) == ReadbackState::Stale);
}

TEST_CASE("a released slot is recycled with a new generation and the old token is stale") {
    CompletionTracker ct;
    ReadbackCore rb;
    const ReadbackToken a = rb.request(1, ct);
    rb.release(a, ct);
    const ReadbackToken b = rb.request(2, ct);
    CHECK(b.slot == a.slot);
    CHECK(b.gen != a.gen);
    CHECK(rb.poll(a, ct, 1) == ReadbackState::Stale);       // the old token must never read the new request bytes
    CHECK(rb.poll(b, ct, 2) == ReadbackState::Pending);
}

TEST_CASE("all slots busy refuses a request; releasing one frees it") {
    CompletionTracker ct;
    ReadbackCore rb;
    ReadbackToken t[ReadbackCore::kSlots];
    for (auto& x : t) { x = rb.request(0, ct); REQUIRE(x); }
    CHECK_FALSE(rb.request(0, ct));
    CHECK(rb.inUse() == ReadbackCore::kSlots);
    rb.release(t[2], ct);
    CHECK(rb.request(5, ct));
}

TEST_CASE("sealing the wrong generation changes nothing") {
    CompletionTracker ct;
    ReadbackCore rb;
    const ReadbackToken a = rb.request(1, ct);
    rb.release(a, ct);
    const ReadbackToken b = rb.request(1, ct);
    ct.noteSubmit(QueueKind::Graphics);
    rb.seal(a, ct.now());                                   // stale token: ignored
    ct.setCompleted(QueueKind::Graphics, 1);
    CHECK(rb.poll(b, ct, 1) == ReadbackState::Pending);     // b was never sealed
}

TEST_CASE("negative control: a discarded slot with its copy still in flight is not reusable until the ticket completes") {
    CompletionTracker ct;
    ReadbackCore rb;
    ReadbackToken t[ReadbackCore::kSlots];
    for (auto& x : t) { x = rb.request(1, ct); REQUIRE(x); }
    ct.noteSubmit(QueueKind::Graphics);
    rb.seal(t[0], ct.now());
    CHECK(rb.poll(t[0], ct, 99) == ReadbackState::Stale);   // wrong tag: the consumer discards it
    rb.release(t[0], ct);
    CHECK_FALSE(rb.request(2, ct));                         // the copy is still in flight: the slot is not free (its buffer must not be replaced)
    CHECK(rb.inUse() == ReadbackCore::kSlots);
    ct.setCompleted(QueueKind::Graphics, 1);
    const ReadbackToken again = rb.request(2, ct);
    CHECK(again);
    CHECK(again.slot == t[0].slot);
}

}  // TEST_SUITE
