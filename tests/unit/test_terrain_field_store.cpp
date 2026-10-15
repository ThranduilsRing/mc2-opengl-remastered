// The terrain field store (GameOS/gameos/terrain_field_store.cpp): the CPU terrain fields both backends draw from.

#include "doctest.h"

#include "terrain_field_store.h"

#include <vector>

TEST_CASE("terrain fields: a height patch lands row by row and changes the version") {
    mc2terrain::TerrainFieldStore store;
    std::vector<float> map(4 * 4, 0.0f);
    REQUIRE(store.setHeight(map.data(), 4));
    const uint64_t v0 = store.heightVersion();

    const float block[] = {1, 2, 3, 4};   // 2 x 2 at (2, 1)
    REQUIRE(store.patchHeight(block, 2, 1, 1, 1, 4));
    CHECK(store.heightVersion() != v0);
    CHECK(store.height()[1 * 4 + 2] == 1);
    CHECK(store.height()[1 * 4 + 3] == 2);
    CHECK(store.height()[2 * 4 + 2] == 3);
    CHECK(store.height()[2 * 4 + 3] == 4);
    CHECK(store.height()[0] == 0);
}

TEST_CASE("terrain fields: malformed writes are rejected and keep the previous field") {
    mc2terrain::TerrainFieldStore store;
    std::vector<float> map(3 * 3, 5.0f);
    REQUIRE(store.setHeight(map.data(), 3));
    const uint64_t v = store.heightVersion();
    const float block[] = {9, 9, 9, 9};
    CHECK_FALSE(store.patchHeight(block, 2, 0, 1, 1, 3));    // runs off the right edge
    CHECK_FALSE(store.patchHeight(block, 0, 0, 1, 1, 4));    // a different map
    CHECK_FALSE(store.patchHeight(nullptr, 0, 0, 1, 1, 3));
    CHECK_FALSE(store.setHeight(nullptr, 3));
    CHECK_FALSE(store.setHeight(map.data(), 0));
    CHECK(store.heightVersion() == v);
    CHECK(store.height()[8] == 5.0f);

    CHECK_FALSE(store.setCementWords(nullptr, 4));
    CHECK_FALSE(store.setVisualHeight(map.data(), 3, nullptr, 2));   // mips promised, none given
    CHECK(store.cementVersion() == 0);
    CHECK(store.visualHeightVersion() == 0);
}

TEST_CASE("terrain fields: the visual bake is the fine field followed by the mip maxes") {
    mc2terrain::TerrainFieldStore store;
    const float fine[] = {1, 2, 3, 4};
    const float mips[] = {7, 8};
    REQUIRE(store.setVisualHeight(fine, 2, mips, 2));
    CHECK(store.visualSide() == 2);
    CHECK(store.visualMipFloats() == 2);
    CHECK(store.visualHeight() == std::vector<float>{1, 2, 3, 4, 7, 8});

    const uint32_t words[] = {0x80000001u, 0};
    REQUIRE(store.setCementWords(words, 2));
    CHECK(store.cementWords().size() == 2);
    CHECK(store.cementVersion() != 0);
}

TEST_CASE("terrain fields: the shore sidecars keep their bytes and reject a bad size") {
    mc2terrain::TerrainFieldStore store;
    const uint8_t mask[2 * 1 * 4] = {1, 2, 3, 4, 5, 6, 7, 8};
    REQUIRE(store.setShorelineMask(mask, 2, 1));
    CHECK(store.shorelineMask().width == 2);
    CHECK(store.shorelineMask().texels == std::vector<uint8_t>(mask, mask + 8));
    const uint64_t v = store.shorelineMask().version;
    CHECK(v != 0);
    CHECK_FALSE(store.setShorelineMask(mask, 0, 1));
    CHECK_FALSE(store.setShorelineMask(nullptr, 2, 1));
    CHECK(store.shorelineMask().version == v);

    const float heights[3] = {1.5f, 2.5f, 3.5f};
    REQUIRE(store.setWaterShoreHeight(heights, 3, 1));
    CHECK(store.waterShoreHeight().texels == std::vector<float>{1.5f, 2.5f, 3.5f});
    CHECK_FALSE(store.setWaterShoreHeight(heights, 1, 70000));
}

TEST_CASE("terrain fields: a recreated store never repeats a version an earlier store handed out") {
    std::vector<float> map(2 * 2, 0.0f);
    uint64_t first = 0;
    {
        mc2terrain::TerrainFieldStore store;
        REQUIRE(store.setHeight(map.data(), 2));
        first = store.heightVersion();
    }
    mc2terrain::TerrainFieldStore again;
    REQUIRE(again.setHeight(map.data(), 2));
    CHECK(again.heightVersion() > first);
}
