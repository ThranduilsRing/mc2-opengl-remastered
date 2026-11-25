#include "doctest.h"
#include "terrain_forest_mask.h"

TEST_CASE("forest mask: one tree is a soft disc, empty input is all zero") {
    std::vector<uint8_t> m;
    // 100x100 grid over 1000 wu starting at (0, 1000): texel = 10 wu. Tree at (500, 500).
    const float tree[2] = {500.0f, 500.0f};
    mc2terrain::splatForestMask(tree, 0.0f, 1000.0f, 1000.0f, 100, 100, 60.0f, m);
    REQUIRE(m.size() == 10000);
    CHECK(m[50 * 100 + 50] > 200);                 // under the trunk
    CHECK(m[50 * 100 + 53] > 0);                   // inside the canopy
    CHECK(m[50 * 100 + 53] < m[50 * 100 + 50]);    // soft falloff
    CHECK(m[50 * 100 + 57] == 0);                  // beyond 60 wu
    CHECK(m[10 * 100 + 10] == 0);                  // far away
    mc2terrain::splatForestMask({}, 0.0f, 1000.0f, 1000.0f, 100, 100, 60.0f, m);
    CHECK(m == std::vector<uint8_t>(10000, 0));
}

TEST_CASE("forest mask: overlapping trees union without exceeding 255, edge trees clip") {
    std::vector<uint8_t> m;
    const float grove[8] = {500.0f, 500.0f, 510.0f, 500.0f, 505.0f, 508.0f, -20.0f, 1010.0f};
    mc2terrain::splatForestMask(grove, 0.0f, 1000.0f, 1000.0f, 100, 100, 60.0f, m);
    CHECK(m[50 * 100 + 50] >= 250);
    CHECK(m[0] > 0);   // the tree outside the map still reaches the corner texel
}
