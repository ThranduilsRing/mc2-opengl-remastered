#pragma once
// VFX island draw packing (no Vulkan; doctest: tests/unit/test_vfx_draw_plan.cpp).
// The particle, mesh and tube bridges hand the island one group per emitter, instance or ribbon. Groups with equal state (blend, page,
// sprite rect, atlas mode) become one vkCmdDraw when their records sit back to back in the ring:
//  - a run of consecutive additive groups (dst factor ONE for colour and alpha, no depth write) is order-independent, so it is stably
//    sorted by state first;
//  - alpha and premultiplied groups keep submission order and merge only with an equal neighbour (primitive order within a draw is
//    the submission order, so the blend result is unchanged).
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <vector>

namespace mc2vfx {

struct PlanItem {
    uint32_t src = 0, count = 0;   // record range in the caller's array
    uint32_t blend = 0;            // VfxParticleDraw::additive
    uint32_t atlas = 0;            // VfxParticleDraw::atlasColumns
    uint64_t page = 0;             // bound texture page (descriptor set key)
    float u0 = 0, v0 = 0, us = 0, vs = 0;
    uint32_t tag = 0;              // caller's group index
};

struct PlanDraw {
    uint32_t item = 0;             // first item of the draw (its state is the draw's state)
    uint32_t start = 0, count = 0; // packed record range
};

inline bool plan_commutes(uint32_t blend) { return blend == 1u || blend == 2u; }

inline auto plan_key(const PlanItem& i) {
    return std::make_tuple(i.blend, i.page, i.atlas, std::bit_cast<uint32_t>(i.u0), std::bit_cast<uint32_t>(i.v0),
                           std::bit_cast<uint32_t>(i.us), std::bit_cast<uint32_t>(i.vs));
}

// items: drawable groups in submission order; reordered in place. Item k's packed start is the sum of the counts before it.
inline void plan_draws(std::vector<PlanItem>& items, std::vector<PlanDraw>& draws) {
    for (size_t b = 0; b < items.size();) {
        size_t e = b + 1;
        if (plan_commutes(items[b].blend)) {
            while (e < items.size() && plan_commutes(items[e].blend)) ++e;
            std::stable_sort(items.begin() + static_cast<std::ptrdiff_t>(b), items.begin() + static_cast<std::ptrdiff_t>(e),
                             [](const PlanItem& x, const PlanItem& y) { return plan_key(x) < plan_key(y); });
        }
        b = e;
    }
    draws.clear();
    uint32_t at = 0;
    for (uint32_t k = 0; k < items.size(); ++k) {
        if (!draws.empty() && plan_key(items[draws.back().item]) == plan_key(items[k])) draws.back().count += items[k].count;
        else draws.push_back({k, at, items[k].count});
        at += items[k].count;
    }
}

}  // namespace mc2vfx
