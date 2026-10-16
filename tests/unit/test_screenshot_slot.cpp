// The screenshot request slot: one request at a time, and a request that never gets a readback expires.
#include "doctest.h"

#include "gos_screenshot_slot.h"

#include <string>

using gos::screenshot::Slot;

namespace {
int g_doneCalls = 0;
void onDone(const char*, bool, int, int) { ++g_doneCalls; }
}  // namespace

TEST_CASE("screenshot slot: a second request is refused while one waits") {
    Slot s;
    CHECK(s.begin("a.tga", onDone));
    CHECK(s.pending());
    CHECK_FALSE(s.begin("b.tga", nullptr));
    CHECK(s.path() == "a.tga");
}

TEST_CASE("screenshot slot: a request expires after the bounded number of frames and frees the slot") {
    Slot s;
    CHECK(s.begin("a.tga", onDone));
    Slot::Request expired;
    for (unsigned i = 0; i + 1 < Slot::kMaxFramesPending; ++i) CHECK_FALSE(s.tick(&expired));
    CHECK(s.pending());
    CHECK(s.tick(&expired));
    CHECK_FALSE(s.pending());
    CHECK(expired.path == "a.tga");
    CHECK(expired.done == onDone);
    CHECK(s.begin("c.tga", nullptr));   // not blocked for the rest of the run
}

TEST_CASE("screenshot slot: take hands the request over and restarts the wait for the next one") {
    Slot s;
    Slot::Request none;
    CHECK_FALSE(s.tick(&none));          // nothing pending: ticking is a no-op
    CHECK(s.begin("a.tga", nullptr));
    Slot::Request expired;
    for (unsigned i = 0; i < Slot::kMaxFramesPending - 2; ++i) s.tick(&expired);
    CHECK(s.take().path == "a.tga");
    CHECK(s.begin("b.tga", nullptr));
    for (unsigned i = 0; i + 1 < Slot::kMaxFramesPending; ++i) CHECK_FALSE(s.tick(&expired));   // the count restarted
    CHECK(s.tick(&expired));
}

TEST_CASE("screenshot slot: refusal names each reason a request cannot be queued") {
    Slot s;
    CHECK(s.refusal("a.tga", true) == nullptr);
    CHECK(std::string(s.refusal(nullptr, true)) == "empty path");
    CHECK(std::string(s.refusal("", true)) == "empty path");
    CHECK(std::string(s.refusal("a.tga", false)) == "the Vulkan backend is not drawing");   // a GL run
    CHECK(s.begin("a.tga", nullptr));
    CHECK(std::string(s.refusal("b.tga", true)) == "a request is still pending");
}
