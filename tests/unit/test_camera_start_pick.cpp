// Regression: owner crash (2026-11-01) -- GameCamera::activate dereferenced Mover::getCommander()
// (null for a commander slot that was never created) while choosing the camera start mover.
#include "doctest.h"
#include "camera_start_pick.h"
#include <vector>

namespace {
struct FakeMover {
    long commander;
    bool onGui;
    long getCommanderId() const { return commander; }
    bool isOnGUI() const { return onGui; }
};
}

TEST_CASE("camera start pick: mover with an empty commander slot is skipped") {
    std::vector<FakeMover> movers{{2, true}, {-1, true}, {0, false}, {0, true}};
    auto get = [&](long i) { return &movers[static_cast<size_t>(i)]; };
    auto* m = pickCameraStartMover<FakeMover>(static_cast<long>(movers.size()), 0, get);
    REQUIRE(m != nullptr);
    CHECK(m == &movers[3]);
}

TEST_CASE("camera start pick: no match falls through to last mover, empty list gives null") {
    std::vector<FakeMover> movers{{2, true}, {-1, true}};
    auto get = [&](long i) { return &movers[static_cast<size_t>(i)]; };
    CHECK(pickCameraStartMover<FakeMover>(2, 0, get) == &movers[1]);
    CHECK(pickCameraStartMover<FakeMover>(0, 0, get) == nullptr);
}
