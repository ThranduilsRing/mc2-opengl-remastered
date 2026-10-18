// tests/unit/test_frame_fence_core.cpp
// The slot decision behind vkImageResourceBeginFrame / CancelFrame (RenderCore/FrameFenceCore.h), including the failed fence re-create.
#include "doctest.h"
#include "FrameFenceCore.h"

using RenderCore::FrameFenceCore;

TEST_SUITE("FrameFenceCore") {

TEST_CASE("begin alternates the two slots") {
    FrameFenceCore c;
    const auto a = c.begin(), b = c.begin(), d = c.begin();
    CHECK(a.slot == 0); CHECK(b.slot == 1); CHECK(d.slot == 0);
    CHECK(a.mayWait); CHECK(b.mayWait); CHECK(d.mayWait);
}

TEST_CASE("a cancelled frame gives the parity back") {
    FrameFenceCore c;
    const auto a = c.begin();
    c.cancel(a.slot, /*recreated=*/true);
    CHECK(c.parity() == 0);
    CHECK_FALSE(c.disabled());
    CHECK(c.begin().slot == a.slot);
}

TEST_CASE("a failed fence re-create disables the resource: no wait is ever allowed again") {
    FrameFenceCore c;
    c.begin();                       // slot 0
    const auto b = c.begin();        // slot 1: its fence is now reset and unsubmitted
    CHECK(b.mayWait);                // negative control: an enabled core allows the wait
    c.cancel(b.slot, /*recreated=*/false);
    CHECK(c.disabled());
    CHECK(c.parity() == b.slot);     // parity restored, so no other slot is handed out either
    for (int i = 0; i < 4; ++i) {
        const auto n = c.begin();
        CHECK_FALSE(n.mayWait);
        CHECK(n.slot == b.slot);
    }
}

TEST_CASE("an out-of-range slot is ignored") {
    FrameFenceCore c;
    c.begin();
    c.cancel(7, false);
    CHECK_FALSE(c.disabled());
    CHECK(c.parity() == 1);
}

}
