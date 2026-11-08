#include "doctest.h"
#include "jump_target.h"
#include "tactical_order_queue.h"

TEST_CASE("jump target accepts passable ground in world range and ignores height") {
    const glm::vec3 from{1000, -2000, 800};
    CHECK(jumpTargetValid(true, true, from, {1300, -1600, -900}, 500));
    CHECK_FALSE(jumpTargetValid(true, true, from, {1300, -1599, 800}, 500));
    CHECK_FALSE(jumpTargetValid(false, true, from, from, 500));
    CHECK_FALSE(jumpTargetValid(true, false, from, from, 500));
    CHECK_FALSE(jumpTargetValid(true, true, from, from, 0));
}

TEST_CASE("jump order ingress requires a counted destination") {
    mc2::sim::TacticalOrder order;
    order.kind = mc2::sim::OrderKind::JumpToPoint;
    order.move.emplace();
    mc2::save::Archive missing([](std::string_view) {});
    order.validate(missing);
    CHECK_FALSE(missing.good());
    order.move->wayPath.points.push_back({1300, -1600, 100});
    order.move->wayPath.mode.push_back(mc2::sim::TravelMode::Jump);
    mc2::save::Archive counted;
    order.validate(counted);
    CHECK(counted.good());
}
