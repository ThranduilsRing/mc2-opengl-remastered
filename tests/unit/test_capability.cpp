// tests/unit/test_capability.cpp
// Tier 2: CapabilitySet — first UnitProfile facet, pure bitmask behind an
// interface that hides representation (UNIT-PROFILE-SEAM-1).
#include "doctest.h"
#include "capability.h"

TEST_CASE("CapabilitySet: default empty") {
    CapabilitySet c;
    for (int i = 0; i < CAP__COUNT; ++i)
        CHECK_FALSE(c.has(static_cast<Capability>(i)));
}
TEST_CASE("CapabilitySet: set then has") {
    CapabilitySet c; c.set(CAP_JUMP, true);
    CHECK(c.has(CAP_JUMP));
}
TEST_CASE("CapabilitySet: set false clears the bit") {
    CapabilitySet c; c.set(CAP_JUMP, true); c.set(CAP_JUMP, false);
    CHECK_FALSE(c.has(CAP_JUMP));
}
TEST_CASE("CapabilitySet: clear empties all") {
    CapabilitySet c; c.set(CAP_JUMP, true); c.clear();
    CHECK_FALSE(c.has(CAP_JUMP));
}
TEST_CASE("CapabilitySet: per-bit isolation") {
    for (int i = 0; i < CAP__COUNT; ++i) {
        CapabilitySet c; c.set(static_cast<Capability>(i), true);
        for (int j = 0; j < CAP__COUNT; ++j)
            CHECK(c.has(static_cast<Capability>(j)) == (i == j));
    }
}

#include "unitprofile.h"
