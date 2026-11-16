// Trigger paths: loading_pump_meter.h and the two SDL loading helpers.
#include "doctest.h"
#include "../../GameOS/gameos/loading_pump_meter.h"
#include <string_view>
using mc2::loadpump::Meter;

TEST_CASE("loading pump: boundaries including no service and final tail") {
    Meter m;
    m.mission.begin(100, "start", false);
    m.mission.end(1600);
    CHECK(m.mission.maxGap == 1500);
    CHECK(m.mission.services == 0);
    m.mission.begin(2000, "start", false);
    m.service(2300, false);
    m.mission.mark(2400, "tail");
    m.mission.end(2900);
    CHECK(m.mission.maxGap == 600);
    CHECK(std::string_view(m.mission.worst) == "tail");
}
TEST_CASE("loading pump: nested and logistics-only scopes stay independent") {
    Meter m;
    m.mission.begin(0, "mission", false);
    m.service(100, false);
    m.variants.begin(150, "variants", false);
    m.service(300, false);
    m.variants.end(350);
    m.mission.end(700);
    CHECK(m.variants.maxGap == 150);
    CHECK(m.mission.maxGap == 400);
    m.variants.begin(1000, "lazy", false);
    m.service(1100, false);
    m.variants.end(1200);
    CHECK(m.variants.maxGap == 100);
    CHECK(m.mission.maxGap == 400);
}
TEST_CASE("loading pump: throttled attempts cannot count as service") {
    Meter m;
    m.mission.begin(0, "start", false);
    REQUIRE(m.due(0));
    m.service(0, false);
    CHECK_FALSE(m.due(49));
    CHECK(m.due(50));
    m.service(900, true);
    m.mission.mark(950, "blocked");
    m.mission.end(1200);
    CHECK(m.mission.services == 1);
    CHECK(m.mission.maxGap == 1200);
    m.variants.begin(1300, "headless", true);
    m.service(1400, true);
    m.variants.end(1500);
    CHECK(m.variants.attempted);
    CHECK(m.variants.services == 1);
    CHECK(m.variants.maxGap == 100);
}
