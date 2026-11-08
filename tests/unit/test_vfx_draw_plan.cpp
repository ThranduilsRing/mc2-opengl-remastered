// tests/unit/test_vfx_draw_plan.cpp
// VFX island draw packing (GameOS/gameos/vfx_draw_plan.h): equal-state neighbours merge, additive runs sort by state,
// alpha groups never move.
#include "doctest.h"
#include "vfx_draw_plan.h"

using namespace mc2vfx;

namespace {
PlanItem item(uint32_t src, uint32_t count, uint32_t blend, uint64_t page, uint32_t tag) {
    PlanItem i;
    i.src = src; i.count = count; i.blend = blend; i.page = page; i.atlas = 0xffffffffu; i.tag = tag;
    return i;
}
}

TEST_SUITE("VfxDrawPlan") {

TEST_CASE("additive groups sort by page and merge; packed starts follow the new order") {
    // Mesh instances alternating two pages, all additive: 4 groups -> 2 draws.
    std::vector<PlanItem> items{item(0, 3, 1, 7, 0), item(3, 3, 1, 9, 1), item(6, 3, 1, 7, 2), item(9, 3, 1, 9, 3)};
    std::vector<PlanDraw> draws;
    plan_draws(items, draws);
    REQUIRE(draws.size() == 2);
    CHECK(items[0].tag == 0); CHECK(items[1].tag == 2); CHECK(items[2].tag == 1); CHECK(items[3].tag == 3);
    CHECK(draws[0].start == 0); CHECK(draws[0].count == 6);
    CHECK(draws[1].start == 6); CHECK(draws[1].count == 6);
    CHECK(items[draws[1].item].page == 9);
}

TEST_CASE("alpha groups keep submission order and split additive runs") {
    // add(7) alpha(9) add(7): the alpha group is a barrier, nothing merges across it.
    std::vector<PlanItem> items{item(0, 2, 1, 7, 0), item(2, 2, 0, 9, 1), item(4, 2, 1, 7, 2)};
    std::vector<PlanDraw> draws;
    plan_draws(items, draws);
    REQUIRE(draws.size() == 3);
    CHECK(items[0].tag == 0); CHECK(items[1].tag == 1); CHECK(items[2].tag == 2);
}

TEST_CASE("alpha neighbours with equal state merge without reordering") {
    std::vector<PlanItem> items{item(0, 2, 0, 7, 0), item(2, 2, 0, 9, 1), item(4, 2, 0, 9, 2), item(6, 2, 0, 7, 3)};
    std::vector<PlanDraw> draws;
    plan_draws(items, draws);
    REQUIRE(draws.size() == 3);
    CHECK(draws[1].start == 2); CHECK(draws[1].count == 4);
    CHECK(items[3].tag == 3);
}

TEST_CASE("sprite rect and blend mode are part of the state") {
    PlanItem a = item(0, 1, 1, 7, 0), b = item(1, 1, 1, 7, 1), c = item(2, 1, 2, 7, 2);
    b.u0 = 0.5f;
    std::vector<PlanItem> items{a, b, c};
    std::vector<PlanDraw> draws;
    plan_draws(items, draws);
    CHECK(draws.size() == 3);
}

}
