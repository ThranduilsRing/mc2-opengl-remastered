// tests/unit/test_vfx_draw_plan.cpp
// VFX island draw packing: stable primitive order, descriptor-indexed pages and blend equivalence audit.
#include "doctest.h"
#include "vfx_draw_plan.h"
#include <array>
#include <cmath>

using namespace mc2vfx;

namespace {
PlanItem item(uint32_t src, uint32_t count, uint32_t blend, uint64_t page, uint32_t tag) {
    PlanItem i;
    i.src = src; i.count = count; i.blend = blend; i.page = page; i.atlas = 0xffffffffu; i.tag = tag;
    return i;
}
}

TEST_SUITE("VfxDrawPlan") {

TEST_CASE("indexed additive pages merge with primitive order unchanged") {
    std::vector<PlanItem> items{item(0, 3, 1, 7, 0), item(3, 3, 1, 9, 1), item(6, 3, 1, 7, 2), item(9, 3, 1, 9, 3)};
    std::vector<PlanDraw> draws;
    plan_draws(items, draws, true);
    REQUIRE(draws.size() == 1);
    CHECK(items[0].tag == 0); CHECK(items[1].tag == 1); CHECK(items[2].tag == 2); CHECK(items[3].tag == 3);
    CHECK(draws[0].start == 0); CHECK(draws[0].count == 12);
    plan_draws(items, draws, false);
    REQUIRE(draws.size() == 4);
    CHECK(items[1].tag == 1); CHECK(items[2].tag == 2);
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
    plan_draws(items, draws, true);   // indexed: the sprite rect and blend 1/2 ride per record
    REQUIRE(draws.size() == 1);
    CHECK(draws[0].count == 3);
}

TEST_CASE("indexed alpha pages and sprite rects merge while geometry stays separate") {
    auto a = item(0, 2, 0, 7, 0), b = item(2, 3, 0, 9, 1), c = item(5, 3, 0, 7, 2);
    a.atlas = 1; b.atlas = 4; b.u0 = 0.5f;
    std::vector<PlanItem> items{a, b, c};
    std::vector<PlanDraw> draws;
    plan_draws(items, draws, true);
    REQUIRE(draws.size() == 2);
    CHECK(draws[0].count == 5); CHECK(draws[1].start == 5);
    CHECK(items[0].tag == 0); CHECK(items[1].tag == 1); CHECK(items[2].tag == 2);
}

TEST_CASE("a mixed additive/alpha run is one draw on the indexed path, split into runs without it") {
    // alpha(0), SRC_ALPHA/ONE(1), ONE/ONE(2), ONE/1-SRC_ALPHA(5), alpha(0): submission order kept, one pipeline.
    std::vector<PlanItem> items{item(0, 2, 0, 7, 0), item(2, 2, 1, 9, 1), item(4, 2, 2, 7, 2), item(6, 2, 5, 9, 3), item(8, 2, 0, 7, 4)};
    std::vector<PlanDraw> draws;
    plan_draws(items, draws, true);
    REQUIRE(draws.size() == 1);
    CHECK(draws[0].count == 10);
    for (uint32_t k = 0; k < 5; ++k) CHECK(items[k].tag == k);
    plan_draws(items, draws, false);
    CHECK(draws.size() == 5);
}

TEST_CASE("opaque and ONE/SRC_ALPHA groups keep their own pipeline on the indexed path") {
    std::vector<PlanItem> items{item(0, 1, 0, 7, 0), item(1, 1, 3, 7, 1), item(2, 1, 4, 7, 2), item(3, 1, 4, 9, 3), item(4, 1, 1, 7, 4)};
    std::vector<PlanDraw> draws;
    plan_draws(items, draws, true);
    REQUIRE(draws.size() == 4);   // fold | opaque | ONE/SRC_ALPHA x2 | fold
    CHECK(draws[2].count == 2);
}

// Old per-mode blend factors (vulkan_vfx_island.cpp make_pipeline; pinned by check-vfx-blend-distinction.py) against the folded
// equation: shader outputs (rgb*w, alphaOut), hardware ONE / ONE_MINUS_SRC_ALPHA. Scene alpha is never read after the VFX pass
// (scripts/tests/test_vfx_scene_alpha_unread.py), so rgb is the contract; the alpha channel is left unequal by design.
TEST_CASE("folded premultiplied equation matches modes 0/1/2/5 in rgb within 1/255") {
    constexpr std::array<unsigned, 4> modes{0, 1, 2, 5};
    constexpr std::array<float, 8> values{0.0f, 1.0f / 255.0f, 0.2f, 0.5f, 0.8f, 254.0f / 255.0f, 1.0f, 0.37f};
    for (const auto mode : modes) {
        for (const float a : values) for (const float c : values) for (const float d : values) {
            const float src = mode == 0 || mode == 1 ? a : 1.0f;                 // old src colour factor
            const float dst = mode == 1 || mode == 2 ? 1.0f : 1.0f - a;          // old dst colour factor
            const float oldRgb = c * src + d * dst;
            const float weight = mode == 0 || mode == 1 ? a : 1.0f;              // foldBlend in vfx_particle.frag
            const float outAlpha = mode == 0 || mode == 5 ? a : 0.0f;
            const float foldedRgb = c * weight + d * (1.0f - outAlpha);
            CHECK(std::abs(oldRgb - foldedRgb) <= 1.0f / 255.0f);
        }
    }
}

}
