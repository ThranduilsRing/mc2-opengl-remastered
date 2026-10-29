#include "doctest.h"
#include "../../GameOS/gameos/runtime_observation.h"

TEST_CASE("runtime_observation/frame reset and repeated zones") {
    using namespace mc2::observation;
    current = {};
    beginFrame();
    current.censusValid = true;
    current.movers = 12;
    current.pilots = 10;
    current.brains = 8;
    recordZone(0, "land_update", 100);
    recordZone(0, "land_update", 50);
    recordZone(16, "overflow", 1);
    CHECK(current.zones[0].ns == 150);
    CHECK(current.overflow == 1);
    CHECK(json() == "{\"version\":1,\"mission_frame\":1,\"census_valid\":true,\"movers\":12,\"pilots\":10,\"brains\":8,\"zone_overflow\":1,\"zone_ns\":{\"land_update\":150}}");
    beginFrame();
    CHECK(current.frame == 2);
    CHECK_FALSE(current.censusValid);
    CHECK(current.movers == 0);
    CHECK(current.zones[0].name == nullptr);
    CHECK(current.overflow == 0);
}
