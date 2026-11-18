#include "doctest.h"
#include "../../RenderCore/vk_resource_graph.h"
using namespace mc2vk::graph;
namespace {
const State wrState{1, 1, 1, true}, rdState{2, 2, 2, false};
Image scratch(unsigned width = 64) { return {{width, 64, 37, 1, 20, 0}}; }
}
TEST_CASE("VkResourceGraph/producer consumer barriers and uninitialized reads") {
    auto p = compile({scratch()}, {{{{0, wrState}}}, {{{0, rdState}}}, {{{0, rdState}}}});
    REQUIRE(p.valid);
    CHECK(p.barriers[0].size() == 1);
    REQUIRE(p.barriers[1].size() == 1);
    CHECK(p.barriers[1][0].before.access == wrState.access);
    CHECK(p.barriers[1][0].after.layout == rdState.layout);
    CHECK(p.barriers[2].empty());
    CHECK_FALSE(compile({scratch()}, {{{{0, rdState}}}}).valid);
    CHECK_FALSE(compile({{{}, true, {}}}, {{{{0, rdState}}}}).valid);
    CHECK_FALSE(compile({scratch()}, {{{{1, wrState}}}}).valid);
    CHECK_FALSE(compile({scratch()}, {{{{0, wrState}, {0, rdState}}}}).valid);
}
TEST_CASE("VkResourceGraph/alias only exact compatible disjoint lifetimes") {
    // Reverse declaration order exercises allocation by first use.
    auto p = compile({scratch(), scratch()},
                     {{{{1, wrState}}}, {{{1, rdState}}}, {{{0, wrState}}}, {{{0, rdState}}}});
    REQUIRE(p.valid);
    CHECK(p.slots == 1);
    CHECK(p.lifetimes[0].slot == p.lifetimes[1].slot);
    REQUIRE(p.barriers[2].size() == 1);
    CHECK(p.barriers[2][0].before.layout == 0);
    CHECK(p.barriers[2][0].before.stages == rdState.stages);
    CHECK(p.barriers[2][0].before.access == rdState.access);
    CHECK(compile({scratch(), scratch(128)},
                  {{{{0, wrState}}}, {{{1, wrState}}}}).slots == 2);
    // SMAA weights consumes edges while writing weights: inclusive overlap.
    p = compile({scratch(), scratch()}, {{{{0, wrState}}}, {{{0, rdState}, {1, wrState}}}, {{{1, rdState}}}});
    REQUIRE(p.valid);
    CHECK(p.slots == 2);
}
TEST_CASE("VkResourceGraph/imported read state seeds visibility") {
    auto p = compile({{{}, true, rdState}}, {{{{0, rdState}}}});
    REQUIRE(p.valid);
    CHECK(p.barriers[0].empty()); // initial read state already visible: no spurious barrier for an identical reader
}
TEST_CASE("VkResourceGraph/imported mip initialization is independent") {
    Image image = scratch(); image.imported = true; image.mips = 2;
    auto p = compile({image}, {{{{0, wrState, 1, 1}}}});
    REQUIRE(p.valid); CHECK(p.slots == 0);
    CHECK(p.finalStates[0].stages == 0);
    CHECK(sameState(p.finalStates[1], wrState));
    auto carry = p.finalStates;
    p = compile({image}, {{{{0, rdState, 1, 1}}}}, &carry);
    REQUIRE(p.valid); REQUIRE(p.barriers[0].size() == 1);
    CHECK(sameState(p.barriers[0][0].before, wrState));
    CHECK_FALSE(compile({image}, {{{{0, rdState, 0, 1}}}}, &carry).valid);
}
TEST_CASE("VkResourceGraph/readers accumulate and imported images never alias") {
    State secondReader{2, 4, 4, false};
    auto p = compile({{{}, true, wrState}},
                     {{{{0, rdState}}}, {{{0, secondReader}}}, {{{0, wrState}}}});
    REQUIRE(p.valid);
    CHECK(p.slots == 0);
    CHECK(p.lifetimes[0].slot == -1);
    CHECK(p.barriers[1].size() == 1); // a reader in a new stage needs its own visibility barrier
    CHECK(p.barriers[2][0].before.stages == 6);
    CHECK(p.barriers[2][0].before.access == 6);
    p = compile({{{}, true, wrState}}, {{{{0, rdState}}}, {{{0, rdState}}}});
    REQUIRE(p.valid);
    CHECK(p.barriers[1].empty()); // same stage/access reader: still no barrier
    p = compile({scratch(), scratch(), scratch()}, {{{{0, wrState}}}, {{{1, wrState}}}, {{{2, wrState}}}});
    REQUIRE(p.valid);
    CHECK(p.slots == 1);
    CHECK(p.barriers[2][0].before.access == wrState.access);
}
TEST_CASE("VkResourceGraph/every creation field participates in slot compatibility") {
    for (unsigned field = 0; field < 6; ++field) {
        Image a = scratch(), b = a;
        ++b.key[field];
        const auto p = compile({a, b}, {{{{0, wrState}}}, {{{1, wrState}}}});
        REQUIRE(p.valid);
        CHECK(p.slots == 2);
    }
    auto p = compile({scratch()}, {{{{0, wrState}}}, {{{0, wrState}}}});
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
    Image buf{{}, true, wrState};
    buf.buffer = true;
    p = compile({buf}, {{{{0, rdState}}}, {{{0, wrState}}}});
    REQUIRE(p.valid);
    CHECK(p.barriers[0].size() == 1);
    CHECK(p.barriers[1].size() == 1);
}
TEST_CASE("VkResourceGraph/mip levels merge into runs and state carries across frames") {
    Image chain = scratch();
    chain.mips = 4;
    const State storage{3, 4, 4, true};
    // The first wrState covers every mip, then mip 0 is rdState alone, then mips 1-3 are rdState together.
    auto p = compile({chain}, {{{{0, storage}}}, {{{0, rdState, 0, 1}}}, {{{0, rdState, 1, 3}}}});
    REQUIRE(p.valid);
    REQUIRE(p.barriers[0].size() == 1);
    CHECK(p.barriers[0][0].mipCount == 4);
    REQUIRE(p.barriers[1].size() == 1);
    CHECK(p.barriers[1][0].baseMip == 0);
    CHECK(p.barriers[1][0].mipCount == 1);
    REQUIRE(p.barriers[2].size() == 1);
    CHECK(p.barriers[2][0].baseMip == 1);
    CHECK(p.barriers[2][0].mipCount == 3);
    CHECK_FALSE(compile({chain}, {{{{0, storage, 0, 2}}}}).valid);   // a fresh image's first wrState covers every mip
    // Frame N+1 starts where frame N ended: the layout round trip is one transition, not a transition from UNDEFINED.
    Image imported = chain;
    imported.imported = true;
    imported.initial = wrState;
    const std::vector<State> carried = p.finalStates;
    auto frame2 = compile({imported}, {{{{0, wrState}}}}, &carried);
    REQUIRE(frame2.valid);
    REQUIRE(frame2.barriers[0].size() == 1);   // every mip ended in the same rdState state: one barrier covers all four
    CHECK(frame2.barriers[0][0].mipCount == 4);
    CHECK(frame2.barriers[0][0].before.layout == rdState.layout);
}

TEST_CASE("VkResourceGraph/a replayed transient's first write sources the previous frame's last reads") {
    // The plan is compiled once and replayed every frame: the first write of a transient re-enters the slot its previous frame's last readers
    // used. A zero source would be emitted as TOP_OF_PIPE / NONE, i.e. no execution dependency on those reads (write-after-read).
    auto p = compile({scratch()}, {{{{0, wrState}}}, {{{0, rdState}}}, {{{0, rdState}}}});
    REQUIRE(p.valid);
    REQUIRE(p.barriers[0].size() == 1);
    CHECK(p.barriers[0][0].before.stages != 0);
    CHECK((p.barriers[0][0].before.stages & rdState.stages) == rdState.stages);
    CHECK((p.barriers[0][0].before.access & rdState.access) == rdState.access);
    CHECK(p.barriers[0][0].after.layout == wrState.layout);
    // Two transients sharing one slot: the first write of either sources the end-of-plan reads of both occupants.
    const State rd2{2, 4, 8, false};
    auto q = compile({scratch(), scratch()}, {{{{0, wrState}}}, {{{0, rdState}}}, {{{1, wrState}}}, {{{1, rd2}}}});
    REQUIRE(q.valid);
    REQUIRE(q.slots == 1);
    CHECK(q.lifetimes[0].slot == q.lifetimes[1].slot);
    REQUIRE(q.barriers[0].size() == 1);
    CHECK(q.barriers[0][0].before.stages != 0);
    CHECK((q.barriers[0][0].before.stages & (rdState.stages | rd2.stages)) == (rdState.stages | rd2.stages));
    CHECK((q.barriers[0][0].before.access & (rdState.access | rd2.access)) == (rdState.access | rd2.access));
}
