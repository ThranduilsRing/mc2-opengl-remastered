// Offline doctest for the GL-free deterministic RNG kernel.
// Proves: same seed => same sequence; output range; mission-name hash stability.
#include "doctest.h"
#include "mclib/det_rng.h"

#include <vector>
#include <cstdint>

using mc2_det_rng::next15;
using mc2_det_rng::hashMissionName;

TEST_CASE("det_rng: same seed yields identical sequence") {
    uint32_t a = 0xCAFEBABEu;
    uint32_t b = 0xCAFEBABEu;
    for (int i = 0; i < 10000; ++i) {
        CHECK(next15(a) == next15(b));
    }
    CHECK(a == b);
}

TEST_CASE("det_rng: different seeds diverge") {
    uint32_t a = 0xCAFEBABEu;
    uint32_t b = 0xDEADBEEFu;
    bool diverged = false;
    for (int i = 0; i < 100 && !diverged; ++i) {
        if (next15(a) != next15(b)) diverged = true;
    }
    CHECK(diverged);
}

TEST_CASE("det_rng: output stays within 15-bit range") {
    uint32_t s = 1u;
    for (int i = 0; i < 100000; ++i) {
        uint32_t v = next15(s);
        CHECK(v <= 0x7FFFu);
    }
}

TEST_CASE("det_rng: sequence is replayable from recorded seed") {
    uint32_t seed = 12345u;
    std::vector<uint32_t> first;
    uint32_t s1 = seed;
    for (int i = 0; i < 256; ++i) first.push_back(next15(s1));
    // Replay from same seed
    uint32_t s2 = seed;
    for (int i = 0; i < 256; ++i) CHECK(next15(s2) == first[i]);
}

TEST_CASE("det_rng: mission-name hash is stable and name-sensitive") {
    CHECK(hashMissionName("mc2_01") == hashMissionName("mc2_01"));
    CHECK(hashMissionName("mc2_01") != hashMissionName("mc2_24"));
    CHECK(hashMissionName(nullptr) == hashMissionName(""));  // both = FNV offset basis
}
