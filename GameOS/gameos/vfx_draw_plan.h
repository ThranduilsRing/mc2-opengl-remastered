#pragma once
// VFX island draw packing (no Vulkan; doctest: tests/unit/test_vfx_draw_plan.cpp).
// The particle, mesh and tube bridges hand the island one group per emitter, instance or ribbon. Groups with equal state (blend, page,
// sprite rect, atlas mode) become one vkCmdDraw when their records sit back to back in the ring.
// Indexed draws carry page and atlas parameters per record. All paths keep submission order, including additive groups:
// finite-format blend clamping can make even an additive run order dependent.
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

// Indexed path: blend modes 0 (alpha), 1 (SRC_ALPHA/ONE), 2 (ONE/ONE) and 5 (ONE/1-SRC_ALPHA) share one premultiplied pipeline
// (the fragment shader weights each record); opaque (3) and ONE/SRC_ALPHA (4) keep their own.
inline uint32_t plan_pipe(uint32_t blend) { return blend == 3u || blend == 4u ? blend : 0u; }

inline auto plan_key(const PlanItem& i, bool indexed = false) {
    return std::make_tuple(indexed ? plan_pipe(i.blend) : i.blend, indexed ? uint64_t{0} : i.page,
                           indexed && i.atlas != 0xffffffffu ? 0u : i.atlas,
                           indexed ? 0u : std::bit_cast<uint32_t>(i.u0), indexed ? 0u : std::bit_cast<uint32_t>(i.v0),
                           indexed ? 0u : std::bit_cast<uint32_t>(i.us), indexed ? 0u : std::bit_cast<uint32_t>(i.vs));
}

// items: drawable groups in submission order; reordered in place. Item k's packed start is the sum of the counts before it.
inline void plan_draws(std::vector<PlanItem>& items, std::vector<PlanDraw>& draws, bool indexed = false) {
    draws.clear();
    uint32_t at = 0;
    for (uint32_t k = 0; k < items.size(); ++k) {
        if (!draws.empty() && plan_key(items[draws.back().item], indexed) == plan_key(items[k], indexed)) draws.back().count += items[k].count;
        else draws.push_back({k, at, items[k].count});
        at += items[k].count;
    }
}

}  // namespace mc2vfx
