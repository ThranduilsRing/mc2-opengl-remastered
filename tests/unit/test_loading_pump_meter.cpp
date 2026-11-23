// Trigger paths: loading_pump_meter.h and the two SDL loading helpers.
#include "doctest.h"
#include "../../GameOS/gameos/loading_pump_meter.h"
#include <string_view>
using mc2::loadpump::Meter;

TEST_CASE("loading pump: cursor policy is throttled independently of event service") {
    mc2::loadpump::CursorThrottle cursor;
    Meter m;
    const std::uint64_t start = (std::uint64_t{1} << 32);
    REQUIRE(cursor.due(start));
    for (unsigned i = 0; i < 1000; ++i) {
        m.service(start + 49, false);
        CHECK_FALSE(cursor.due(start + 49));
    }
    CHECK(cursor.due(start + 50));
    CHECK_FALSE(cursor.due(start + 50));
    CHECK_FALSE(cursor.due(start + 99));
    CHECK(cursor.due(start + 100));
    CHECK(cursor.due(start + 10000));
    CHECK_FALSE(cursor.due(start + 10049));
    CHECK(cursor.due(start + 10050));
}

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

TEST_CASE("loading pump: MAIN_STALL watch reports one begin and one end per stall") {
    mc2::loadpump::StallWatch w;
    CHECK(w.step(0) == 0);
    CHECK(w.step(3999) == 0);
    CHECK(w.step(4000) == 1);
    CHECK(w.gap == 4000);
    CHECK(w.step(9000) == 0);   // still stalled: no second begin
    CHECK(w.step(20) == -1);
    CHECK(w.gap == 9000);       // end reports the longest gap
    CHECK(w.step(20) == 0);
    CHECK(w.step(5000) == 1);   // a later stall is a new begin
}
