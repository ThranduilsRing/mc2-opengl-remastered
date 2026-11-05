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
    CHECK(p.barriers[1].size() == 1); // a reader in a new stage needs its own visibility barrier
    CHECK(p.barriers[2][0].before.stages == 6);
    CHECK(p.barriers[2][0].before.access == 6);
    p = compile({{{}, true, write}}, {{{{0, read}}}, {{{0, read}}}});
    REQUIRE(p.valid);
    CHECK(p.barriers[1].empty()); // same stage/access reader: still no barrier
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
TEST_CASE("VkResourceGraph/imported read image first read is no barrier (R7), wide masks, buffers") {
    const State lut{2, 0x8000000000ull, 0x8000000000ull, false};   // a 64-bit sync2 stage
    auto p = compile({{{}, true, lut}}, {{{{0, lut}}}, {{{0, lut}}}});
    REQUIRE(p.valid);
    CHECK(p.barriers[0].empty());
    CHECK(p.barriers[1].empty());
    Image buf{{}, true, write};
    buf.buffer = true;
    p = compile({buf}, {{{{0, read}}}, {{{0, write}}}});
    REQUIRE(p.valid);
    CHECK(p.barriers[0].size() == 1);
    CHECK(p.barriers[1].size() == 1);
}
TEST_CASE("VkResourceGraph/mip levels merge into runs and state carries across frames") {
    Image chain = scratch();
    chain.mips = 4;
    const State storage{3, 4, 4, true};
    // The first write covers every mip, then mip 0 is read alone, then mips 1-3 are read together.
    auto p = compile({chain}, {{{{0, storage}}}, {{{0, read, 0, 1}}}, {{{0, read, 1, 3}}}});
    REQUIRE(p.valid);
    REQUIRE(p.barriers[0].size() == 1);
    CHECK(p.barriers[0][0].mipCount == 4);
    REQUIRE(p.barriers[1].size() == 1);
    CHECK(p.barriers[1][0].baseMip == 0);
    CHECK(p.barriers[1][0].mipCount == 1);
    REQUIRE(p.barriers[2].size() == 1);
    CHECK(p.barriers[2][0].baseMip == 1);
    CHECK(p.barriers[2][0].mipCount == 3);
    CHECK_FALSE(compile({chain}, {{{{0, storage, 0, 2}}}}).valid);   // a fresh image's first write covers every mip
    // Frame N+1 starts where frame N ended: the layout round trip is one transition, not a transition from UNDEFINED.
    Image imported = chain;
    imported.imported = true;
    imported.initial = write;
    const std::vector<graph::State> carried = p.finalStates;
    auto frame2 = compile({imported}, {{{{0, write}}}}, &carried);
    REQUIRE(frame2.valid);
    REQUIRE(frame2.barriers[0].size() == 1);   // every mip ended in the same read state: one barrier covers all four
    CHECK(frame2.barriers[0][0].mipCount == 4);
    CHECK(frame2.barriers[0][0].before.layout == read.layout);
}
