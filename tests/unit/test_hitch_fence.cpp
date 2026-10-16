// The hitch trace's Vulkan fence-wait record: what it accumulates and the line it prints.
#include "doctest.h"

#include "mc2_hitch_fence.h"

#include <cmath>
#include <limits>
#include <string>

using mc2_hitch::FenceWaitStats;

TEST_CASE("hitch fence: samples accumulate into total, max and count") {
    FenceWaitStats s;
    CHECK(s.record(1.5));
    CHECK(s.record(4.25));
    CHECK(s.record(0.0));
    CHECK(s.count == 3);
    CHECK(s.totalMs == doctest::Approx(5.75));
    CHECK(s.maxMs == doctest::Approx(4.25));
}

TEST_CASE("hitch fence: a negative or non-finite sample is refused and changes nothing") {
    FenceWaitStats s;
    CHECK(s.record(2.0));
    CHECK_FALSE(s.record(-0.5));
    CHECK_FALSE(s.record(std::numeric_limits<double>::quiet_NaN()));
    CHECK_FALSE(s.record(std::numeric_limits<double>::infinity()));
    CHECK(s.count == 1);
    CHECK(s.totalMs == doctest::Approx(2.0));
}

TEST_CASE("hitch fence: the record format") {
    FenceWaitStats s;
    s.record(3.0);
    s.record(5.0);
    char buf[160];
    const int n = s.format(buf, sizeof buf, 42u);
    REQUIRE(n > 0);
    CHECK(std::string(buf) == "[HITCH_VK] frame=42 fenceWaitMs=8.00 fenceWaitMax=5.00 fenceWaits=2\n");
}

TEST_CASE("hitch fence: a buffer too small to hold the line is not treated as success with garbage") {
    FenceWaitStats s;
    s.record(1.0);
    char buf[8];
    const int n = s.format(buf, sizeof buf, 1u);
    CHECK(n > 0);                        // the length the full line would have had
    CHECK(std::string(buf).size() < sizeof buf);
}
