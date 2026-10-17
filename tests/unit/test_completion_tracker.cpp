// tests/unit/test_completion_tracker.cpp
// The ticket arithmetic behind the Vulkan completion tracker (vk_completion_tracker.h): values in submission order, completion by counter, frame
// sealing. No Vulkan type, no device: the "GPU" is a counter the test advances by hand.
#include "doctest.h"
#include "vk_completion_tracker.h"

using namespace mc2vk;

TEST_SUITE("CompletionTracker") {

TEST_CASE("submit values are 1,2,3 in order and the empty ticket is always complete") {
    CompletionTracker t;
    CHECK(t.lastSubmitted(QueueKind::Graphics) == 0);
    CHECK(t.noteSubmit(QueueKind::Graphics) == 1);
    CHECK(t.noteSubmit(QueueKind::Graphics) == 2);
    CHECK(t.noteSubmit(QueueKind::Graphics) == 3);
    CHECK(t.noteSubmit(QueueKind::Transfer) == 1);   // one counter per queue
    CHECK(t.isComplete(GpuTicket{}));
}

TEST_CASE("a ticket completes exactly when the counter reaches its value") {
    CompletionTracker t;
    for (int i = 0; i < 5; ++i) t.noteSubmit(QueueKind::Graphics);
    const GpuTicket five = t.now();
    CHECK(five.gfx == 5);
    CHECK_FALSE(t.isComplete(five));
    t.setCompleted(QueueKind::Graphics, 4);
    CHECK_FALSE(t.isComplete(five));
    t.setCompleted(QueueKind::Graphics, 5);
    CHECK(t.isComplete(five));
    t.setCompleted(QueueKind::Graphics, 2);          // the counter never moves back
    CHECK(t.completed(QueueKind::Graphics) == 5);
    CHECK(t.isComplete(five));
}

TEST_CASE("a refused submit gives its value back, only the newest one") {
    CompletionTracker t;
    const uint64_t a = t.noteSubmit(QueueKind::Graphics);
    const uint64_t b = t.noteSubmit(QueueKind::Graphics);
    t.unnoteSubmit(QueueKind::Graphics, a);          // not the newest: ignored
    CHECK(t.lastSubmitted(QueueKind::Graphics) == b);
    t.unnoteSubmit(QueueKind::Graphics, b);
    CHECK(t.lastSubmitted(QueueKind::Graphics) == a);
    CHECK(t.noteSubmit(QueueKind::Graphics) == b);   // reused, so the timeline never skips a value the GPU will not signal
}

TEST_CASE("a two-queue ticket needs both components") {
    CompletionTracker t;
    t.noteSubmit(QueueKind::Graphics);
    t.noteSubmit(QueueKind::Transfer);
    t.noteSubmit(QueueKind::Transfer);
    const GpuTicket both = GpuTicket::both(GpuTicket{1, 0}, GpuTicket{0, 2});
    CHECK(both.gfx == 1);
    CHECK(both.xfer == 2);
    t.setCompleted(QueueKind::Graphics, 1);
    CHECK_FALSE(t.isComplete(both));
    t.setCompleted(QueueKind::Transfer, 2);
    CHECK(t.isComplete(both));
}

TEST_CASE("frame sealing: the ticket is the last submit before endFrame, later submits belong to the next frame") {
    CompletionTracker t;
    t.beginFrame();                                  // frame 1
    t.noteSubmit(QueueKind::Graphics);
    t.noteSubmit(QueueKind::Graphics);
    t.endFrame();
    t.beginFrame();                                  // frame 2
    t.noteSubmit(QueueKind::Graphics);
    t.endFrame();
    CHECK(t.frameTicket(1).gfx == 2);
    CHECK(t.frameTicket(2).gfx == 3);
    t.setCompleted(QueueKind::Graphics, 2);
    CHECK(t.isComplete(t.frameTicket(1)));
    CHECK_FALSE(t.isComplete(t.frameTicket(2)));
}

TEST_CASE("frames the ring forgot, or never sealed, answer complete") {
    CompletionTracker t;
    for (int f = 0; f < 20; ++f) { t.beginFrame(); t.noteSubmit(QueueKind::Graphics); t.endFrame(); }
    CHECK(t.frameTicket(1).gfx == 0);                // forgotten: complete
    CHECK(t.isComplete(t.frameTicket(1)));
    CHECK(t.frameTicket(20).gfx == 20);
    CHECK(t.frameTicket(999).gfx == 0);              // never existed
    t.beginFrame();                                  // frame 21 is open, not sealed
    CHECK(t.frameOpen());
    CHECK(t.frameTicket(21).gfx == 0);
}

}  // TEST_SUITE
