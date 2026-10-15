#include "doctest.h"
#include "tactical_order_queue.h"
#include "terrain.h"

float Terrain::worldUnitsMapSide = 1000;

namespace {
std::string routeText(int type = 2, int count = 2, int stance = 4) {
    auto text = "[Part9]\nl OrderType = " + std::to_string(type) + "\nl OrderStance = " +
        std::to_string(stance) + "\nul WaypointCount = " + std::to_string(count) + "\n";
    for (int i = 0; i < count; ++i) {
        const auto key = "f Waypoint" + std::to_string(i);
        text += key + "X = 1\n" + key + "Y = -2\n" + key + "Z = 3\n";
    }
    return text;
}
auto read(std::string_view text) { return mc2::sim::readAuthoredRoute(text, 2, 9, 7, 1000); }
using namespace mc2::sim;
RouteExecution execution(OrderKind kind = OrderKind::PatrolPath, unsigned count = 2) {
    RouteExecution route;
    route.key = {11, 13}; route.revision = 17; route.pilotIncarnation = 19;
    route.originalPart = 2; route.selectedPart = 9; route.pilot = 7; route.stance = 4;
    route.kind = kind;
    route.path.points.resize(count, {1, 2, 3});
    route.path.mode.resize(count, TravelMode::Fast);
    if (count > 1) route.path.mode.back() = TravelMode::Slow;
    return route;
}
RouteSignal signal(const RouteExecution& route, RouteAction action, Tick tick = 1) {
    return {route.key, route.revision, route.eventSequence + 1, tick, action, true, true, true, route.pilotIncarnation};
}
void start(RouteExecution& route) { REQUIRE(route.transition(signal(route, RouteAction::Ready))); }
}

TEST_SUITE("AuthoredRoute") {
TEST_CASE("editor mapping selected alternate preserved pilot stance and every point") {
    for (const auto [editorId, kind] : {std::pair{0, mc2::sim::OrderKind::None},
        {1, mc2::sim::OrderKind::TraversePath}, {2, mc2::sim::OrderKind::PatrolPath}}) {
        for (int count = editorId ? 1 : 0; count <= (editorId ? 8 : 0); ++count) {
            for (int stance = 0; stance <= 5; ++stance) {
                const auto parsed = read(routeText(editorId, count, stance));
                REQUIRE(parsed); REQUIRE(*parsed);
                const auto& route = **parsed;
                CHECK(route.kind == kind);
                CHECK(route.originalPart == 2); CHECK(route.selectedPart == 9); CHECK(route.pilot == 7);
                CHECK(route.stance == stance); CHECK(route.path.points.size() == count);
                for (int i = 0; i < count; ++i) {
                    CHECK(route.path.points[i] == mc2::save::Vec3{1,-2,3});
                    CHECK(route.path.mode[i] == mc2::sim::TravelMode::Slow);
                }
            }
        }
    }
}
TEST_CASE("absent and None produce no route handoff") {
    REQUIRE(read("[Part9]\nul Pilot=7\n"));
    CHECK_FALSE(*read("[Part9]\nul Pilot=7\n"));
    CHECK((**read(routeText(0, 0))).kind == mc2::sim::OrderKind::None);
}
TEST_CASE("malformed entire orders have one reason and no published prefix") {
    for (const auto text : {"[Part9]\nOrderType = 1\n", "[Part9]\nOrderType=1\n",
                           "[Part9]\nWaypointCount = 1\n"}) {
        const auto parsed = read(text);
        REQUIRE_FALSE(parsed);
        CHECK(parsed.error() == "missing_type");
    }
    for (const auto& text : {routeText(3), routeText(2,0), routeText(2,9), routeText(0,1),
        routeText(2,2,-1), routeText(2,2,6), routeText() + "f Waypoint2X=1\n",
        routeText() + "l OrderMystery=1\n", routeText() + "f Waypoint0X=1\n",
        std::string("[Part9]\nul WaypointCount=2\n"),
        std::string("[Part9]\nl OrderType=2junk\nl OrderStance=2\nul WaypointCount=1\n")}) {
        const auto parsed = read(text);
        REQUIRE_FALSE(parsed);
        CHECK_FALSE(parsed.error().empty());
    }
    for (const auto bad : {"nan", "inf", "-inf", "500", "-500", "501", "1junk"}) {
        auto text = routeText();
        text.replace(text.find("Waypoint0X = 1"), 14, std::string("Waypoint0X = ") + bad);
        CHECK_FALSE(read(text));
    }
    auto missing = routeText();
    missing.erase(missing.find("f Waypoint1Z"));
    CHECK_FALSE(read(missing));
    auto wrongKey = routeText();
    wrongKey.replace(wrongKey.find("Waypoint1Z"), 10, "Waypoint99Z");
    CHECK_FALSE(read(wrongKey));
    CHECK_FALSE(read(routeText() + "[Part9]\n"));
}
TEST_CASE("patrol wraps last to zero including a single point without reversing") {
    for (const auto count : {1u, 2u, 8u}) {
        auto route = execution(OrderKind::PatrolPath, count); start(route);
        for (unsigned i = 0; i < count * 2; ++i) {
            REQUIRE(route.transition(signal(route, RouteAction::Arrived)));
            CHECK(route.path.curPoint == (i + 1) % count);
            CHECK(route.state == RouteState::Active);
        }
    }
}
TEST_CASE("traverse completes without indexing a mode beyond the last arrival") {
    auto route = execution(OrderKind::TraversePath, 1); start(route);
    const auto event = route.transition(signal(route, RouteAction::Arrived, 2));
    REQUIRE(event); CHECK(event->event == RouteEventKind::Completed);
    CHECK(route.path.curPoint == 1); CHECK(route.terminal()); CHECK_FALSE(route.cursorMode());
    CHECK_FALSE(route.transition(signal(route, RouteAction::Arrived, 3)));
}
TEST_CASE("cursor queries and rejected duplicate arrivals cannot advance the route") {
    auto route = execution(); start(route);
    CHECK(route.cursorMode() == TravelMode::Fast); CHECK(route.cursorMode() == TravelMode::Fast);
    CHECK(route.path.curPoint == 0);
    auto arrival = signal(route, RouteAction::Arrived, 2);
    const auto event = route.transition(arrival);
    REQUIRE(event);
    CHECK(route.cursorMode() == TravelMode::Slow);
    CHECK(event->key == route.key); CHECK(event->revision == 17);
    CHECK(event->sequence == 2); CHECK(event->tick == 2); CHECK(event->reason == RouteReason::None);
    CHECK_FALSE(route.transition(arrival)); CHECK(route.path.curPoint == 1);
    auto stale = signal(route, RouteAction::Arrived, 3); --stale.revision;
    CHECK_FALSE(route.transition(stale)); CHECK(route.path.curPoint == 1);
    stale = signal(route, RouteAction::Arrived, 3); ++stale.key.partIncarnation;
    CHECK_FALSE(route.transition(stale)); CHECK(route.path.curPoint == 1);
    stale = signal(route, RouteAction::Arrived, 3); ++stale.key.executionId;
    CHECK_FALSE(route.transition(stale)); CHECK(route.path.curPoint == 1);
    CHECK_FALSE(route.transition(signal(route, RouteAction::Arrived, 1)));
    CHECK(route.eventSequence == 2); CHECK(route.lastTransitionTick == 2);
}
TEST_CASE("start requires both map prerequisites and emits Started once") {
    auto route = execution();
    for (const auto [pathReady, goalReady] : {std::pair{false,false}, {true,false}, {false,true}}) {
        auto input = signal(route, RouteAction::Ready);
        input.pathMapsReady = pathReady; input.goalMapReady = goalReady;
        CHECK_FALSE(route.transition(input)); CHECK(route.state == RouteState::Pending);
    }
    const auto input = signal(route, RouteAction::Ready);
    auto event = route.transition(input);
    REQUIRE(event); CHECK(event->event == RouteEventKind::Started);
    CHECK_FALSE(route.transition(input)); CHECK(route.eventSequence == 1);
}
TEST_CASE("unreachable preserves cursor and fails at count budget or tick deadline") {
    for (const auto deadline : {false, true}) {
        auto route = execution(); start(route);
        auto event = route.transition(signal(route, RouteAction::Unreachable, 2));
        REQUIRE(event); CHECK(event->event == RouteEventKind::Retrying);
        CHECK(route.retryDeadline == 2 + routeRetryWindow);
        CHECK(route.retriesRemaining == routeRetryBudget - 1);
        if (!deadline) {
            for (unsigned i = 1; i < routeRetryBudget; ++i)
                REQUIRE(route.transition(signal(route, RouteAction::Unreachable, 2 + i)));
            CHECK(route.state == RouteState::Active); CHECK(route.retriesRemaining == 0);
        }
        event = route.transition(signal(route, RouteAction::Unreachable, deadline ? route.retryDeadline : 6));
        REQUIRE(event); CHECK(event->event == RouteEventKind::Failed);
        CHECK(event->reason == RouteReason::Unreachable); CHECK(route.path.curPoint == 0);
        CHECK_FALSE(route.transition(signal(route, RouteAction::Ready, 40)));
    }
}
TEST_CASE("accepted arrival resets the failed segment budget") {
    auto route = execution(); start(route);
    REQUIRE(route.transition(signal(route, RouteAction::Unreachable, 2)));
    REQUIRE(route.transition(signal(route, RouteAction::Arrived, 3)));
    CHECK(route.retriesRemaining == routeRetryBudget); CHECK(route.retryDeadline == 0);
}
TEST_CASE("path acceptance rearms retries after a long segment without arrival") {
    auto route = execution(); start(route);
    REQUIRE(route.transition(signal(route, RouteAction::Unreachable, 2)));
    REQUIRE(route.transition(signal(route, RouteAction::PathAccepted, 3)));
    CHECK(route.retriesRemaining == routeRetryBudget); CHECK(route.retryDeadline == 0);
    const auto event = route.transition(signal(route, RouteAction::Unreachable, 230));
    REQUIRE(event); CHECK(event->event == RouteEventKind::Retrying);
    CHECK(route.retryDeadline == 230 + routeRetryWindow); CHECK(route.path.curPoint == 0);
}
TEST_CASE("alarm before readiness suspends until cleared and maps are ready") {
    for (const bool mapsReady : {false, true}) {
        auto route = execution();
        const auto event = route.transition(signal(route, RouteAction::Alarm));
        REQUIRE(event); CHECK(event->event == RouteEventKind::Suspended);
        CHECK(route.state == RouteState::Suspended);
        CHECK_FALSE(route.transition(signal(route, RouteAction::Ready, 2)));
        auto input = signal(route, RouteAction::AlarmCleared, 3);
        input.pathMapsReady = mapsReady;
        REQUIRE(route.transition(input));
        CHECK(route.state == (mapsReady ? RouteState::Active : RouteState::Pending));
        if (!mapsReady) REQUIRE(route.transition(signal(route, RouteAction::Ready, 4)));
        CHECK(route.state == RouteState::Active);
    }
}
TEST_CASE("player and removal cancel permanently even while suspended") {
    for (const auto action : {RouteAction::PlayerOrder, RouteAction::Removed}) {
        for (const auto suspended : {false, true}) {
            auto route = execution(); start(route);
            if (suspended) REQUIRE(route.transition(signal(route, RouteAction::Alarm, 2)));
            const auto event = route.transition(signal(route, action, 3));
            REQUIRE(event); CHECK(event->event == RouteEventKind::Interrupted);
            CHECK(event->reason == (action == RouteAction::PlayerOrder ? RouteReason::PlayerOrder : RouteReason::Removed));
            CHECK(route.state == RouteState::Interrupted);
            const auto sequence = route.eventSequence;
            for (const auto resurrection : {RouteAction::Ready, RouteAction::AlarmCleared, RouteAction::Arrived})
                CHECK_FALSE(route.transition(signal(route, resurrection, 4)));
            CHECK(route.eventSequence == sequence);
        }
    }
}
TEST_CASE("failed resume becomes active and exhausts its retry budget") {
    auto route = execution(); start(route);
    const auto path = route.path;
    auto event = route.transition(signal(route, RouteAction::Alarm, 2));
    REQUIRE(event); CHECK(event->event == RouteEventKind::Suspended); CHECK(event->reason == RouteReason::Alarm);
    CHECK_FALSE(route.transition(signal(route, RouteAction::Arrived, 3)));
    auto input = signal(route, RouteAction::AlarmCleared, 3); ++input.revision;
    CHECK_FALSE(route.transition(input)); CHECK(route.state == RouteState::Suspended);
    input = signal(route, RouteAction::AlarmCleared, 3); input.segmentReachable = false;
    event = route.transition(input);
    REQUIRE(event); CHECK(event->event == RouteEventKind::Retrying);
    CHECK(route.state == RouteState::Active);
    CHECK(route.suspensionReason == RouteReason::None); CHECK(route.retriesRemaining == routeRetryBudget - 1);
    CHECK(route.state == RouteState::Active); CHECK(route.path == path); CHECK(route.stance == 4);
    CHECK_FALSE(route.transition(signal(route, RouteAction::AlarmCleared, 5)));
    for (unsigned i = 1; i <= routeRetryBudget; ++i)
        REQUIRE(route.transition(signal(route, RouteAction::Unreachable, 5 + i)));
    CHECK(route.state == RouteState::Failed);
    CHECK_FALSE(route.transition(signal(route, RouteAction::AlarmCleared, 50)));
}
TEST_CASE("every terminal state rejects every later action without emitting an event") {
    for (const auto terminal : {RouteState::Completed, RouteState::Interrupted, RouteState::Failed}) {
        auto route = execution(); route.state = terminal;
        for (const auto action : {RouteAction::Ready, RouteAction::Arrived, RouteAction::Unreachable,
            RouteAction::PlayerOrder, RouteAction::Alarm,
            RouteAction::AlarmCleared, RouteAction::Removed, RouteAction::PathAccepted}) {
            CHECK_FALSE(route.transition(signal(route, action)));
            CHECK(route.state == terminal); CHECK(route.eventSequence == 0); CHECK(route.path.curPoint == 0);
        }
    }
}
}
