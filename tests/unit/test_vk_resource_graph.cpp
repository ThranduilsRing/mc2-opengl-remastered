#include "doctest.h"
#include "../../RenderCore/vk_resource_graph.h"
using namespace mc2vk::graph;
namespace {
const State write{1, 1, 1, true}, read{2, 2, 2, false};
Image scratch(unsigned width = 64) { return {{width, 64, 37, 1, 20, 0}}; }
}
TEST_CASE("VkResourceGraph/producer consumer barriers and uninitialized reads") {
    auto p = compile({scratch()}, {{{{0, write}}}, {{{0, read}}}, {{{0, read}}}});
    REQUIRE(p.valid);
    CHECK(p.barriers[0].size() == 1);
    REQUIRE(p.barriers[1].size() == 1);
    CHECK(p.barriers[1][0].before.access == write.access);
    CHECK(p.barriers[1][0].after.layout == read.layout);
    CHECK(p.barriers[2].empty());
    CHECK_FALSE(compile({scratch()}, {{{{0, read}}}}).valid);
    CHECK_FALSE(compile({{{}, true, {}}}, {{{{0, read}}}}).valid);
    CHECK_FALSE(compile({scratch()}, {{{{1, write}}}}).valid);
    CHECK_FALSE(compile({scratch()}, {{{{0, write}, {0, read}}}}).valid);
}
TEST_CASE("VkResourceGraph/alias only exact compatible disjoint lifetimes") {
    // Reverse declaration order exercises allocation by first use.
    auto p = compile({scratch(), scratch()},
                     {{{{1, write}}}, {{{1, read}}}, {{{0, write}}}, {{{0, read}}}});
    REQUIRE(p.valid);
    CHECK(p.slots == 1);
    CHECK(p.lifetimes[0].slot == p.lifetimes[1].slot);
    REQUIRE(p.barriers[2].size() == 1);
    CHECK(p.barriers[2][0].before.layout == 0);
    CHECK(p.barriers[2][0].before.stages == read.stages);
    CHECK(p.barriers[2][0].before.access == read.access);
    CHECK(compile({scratch(), scratch(128)},
                  {{{{0, write}}}, {{{1, write}}}}).slots == 2);
    // SMAA weights consumes edges while writing weights: inclusive overlap.
    p = compile({scratch(), scratch()}, {{{{0, write}}}, {{{0, read}, {1, write}}}, {{{1, read}}}});
    REQUIRE(p.valid);
    CHECK(p.slots == 2);
}
TEST_CASE("VkResourceGraph/readers accumulate and imported images never alias") {
    State secondReader{2, 4, 4, false};
    auto p = compile({{{}, true, write}},
                     {{{{0, read}}}, {{{0, secondReader}}}, {{{0, write}}}});
    REQUIRE(p.valid);
    CHECK(p.slots == 0);
    CHECK(p.lifetimes[0].slot == -1);
    CHECK(p.barriers[2][0].before.stages == 6);
    CHECK(p.barriers[2][0].before.access == 6);
    p = compile({scratch(), scratch(), scratch()}, {{{{0, write}}}, {{{1, write}}}, {{{2, write}}}});
    REQUIRE(p.valid);
    CHECK(p.slots == 1);
    CHECK(p.barriers[2][0].before.access == write.access);
}
TEST_CASE("VkResourceGraph/every creation field participates in slot compatibility") {
    for (unsigned field = 0; field < 6; ++field) {
        Image a = scratch(), b = a;
        ++b.key[field];
        const auto p = compile({a, b}, {{{{0, write}}}, {{{1, write}}}});
        REQUIRE(p.valid);
        CHECK(p.slots == 2);
    }
    auto p = compile({scratch()}, {{{{0, write}}}, {{{0, write}}}});
    REQUIRE(p.valid);
    CHECK(p.barriers[1].size() == 1); // Same-layout WAW still needs visibility.
    p = compile({scratch(), scratch()}, {});
    REQUIRE(p.valid);
    CHECK(p.slots == 0);
}
