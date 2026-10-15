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
}
