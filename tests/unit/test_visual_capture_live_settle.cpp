#include "doctest.h"
#include "GameOS/gameos/visual_capture_live_settle.h"

using namespace mc2::visual_capture;

TEST_CASE("live settle: off (0 sec) freezes immediately, default behaviour") {
    CHECK(liveSettleStep(10.0, 10.0, 0.0, 0, 600) == LiveSettleStep::Freeze);
}

TEST_CASE("live settle: runs until N sim-seconds elapsed, then freezes") {
    CHECK(liveSettleStep(10.0, 10.0, 3.0, 0, 1000) == LiveSettleStep::Run);
    CHECK(liveSettleStep(12.9, 10.0, 3.0, 87, 1000) == LiveSettleStep::Run);
    CHECK(liveSettleStep(13.0, 10.0, 3.0, 90, 1000) == LiveSettleStep::Freeze);
}

TEST_CASE("live settle: a stalled sim clock is cut off at maxFrames") {
    CHECK(liveSettleStep(10.0, 10.0, 3.0, 999, 1000) == LiveSettleStep::Run);
    CHECK(liveSettleStep(10.0, 10.0, 3.0, 1000, 1000) == LiveSettleStep::Freeze);
    CHECK(liveSettleMaxFrames(3.0) == 960);
}
