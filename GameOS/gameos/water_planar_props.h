#pragma once

#include "water_planar_view.h"
#include "../../RenderCore/hzb_prop_bounds.h"
#include <limits>
#include <span>
#include <vector>

namespace mc2::waterplanar {

inline bool propIntersects(const View& view, const GpuStaticPropInstance& instance,
                           std::span<const mc2hzb::PropBounds> bounds) {
    // Missing geometry bounds retain the instance. The reflected pass must
    // never use main-view HZB results to decide whether a palm is present.
    if (instance.typeID >= bounds.size() || !bounds[instance.typeID].valid) return true;
    const auto& b = bounds[instance.typeID];
    std::array<float, 3> lo{}, hi{};
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(b.lo[axis]) || !std::isfinite(b.hi[axis]) || b.lo[axis] > b.hi[axis]) return true;
        lo[axis] = (std::numeric_limits<float>::max)();
        hi[axis] = -(std::numeric_limits<float>::max)();
    }
    for (int corner = 0; corner < 8; ++corner) {
        const float p[4] = {corner & 1 ? b.hi[0] : b.lo[0], corner & 2 ? b.hi[1] : b.lo[1],
                            corner & 4 ? b.hi[2] : b.lo[2], 1.0f};
        struct Point { float x, y, z; } stuff{};
        float xyz[3]{};
        // Stuff-memcpy matrix: the translation is [3],[7],[11], as in the
        // prop vertex shader's row-vector multiply, not GLM's column [12..14].
        for (int axis = 0; axis < 3; ++axis)
            for (int k = 0; k < 4; ++k) xyz[axis] += p[k] * instance.modelMatrix[axis * 4 + k];
        stuff = {xyz[0], xyz[1], xyz[2]};
        const auto world = mc2ToRender(stuff);
        const float values[] = {world.x, world.y, world.z};
        for (int axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(values[axis])) return true;
            lo[axis] = (std::min)(lo[axis], values[axis]);
            hi[axis] = (std::max)(hi[axis], values[axis]);
        }
    }
    return view.intersects(lo, hi);
}

// Full-registry extraction, including non-shadow-casting alpha cards. The
// range's generation-checked borrow rejects destroyed/replaced actors without
// reading its main-camera visibility, light-row freshness or shadow flags.
template <class Range>
void collectReflectionInstances(std::span<const GpuStaticPropInstance> recipes, std::span<const Range> ranges,
                                const View& view, std::span<const mc2hzb::PropBounds> bounds,
                                std::vector<GpuStaticPropInstance>& out) {
    out.clear();
    std::vector<size_t> order;
    for (const Range& range : ranges) {
        const size_t begin = range.begin, count = range.count;
        if (count == 0 || begin > recipes.size() || count > recipes.size() - begin || !range.multi()) continue;
        for (size_t i = begin; i < begin + count; ++i)
            if (propIntersects(view, recipes[i], bounds)) order.push_back(i);
    }
    // Existing prop packets require contiguous type runs. Stable ordering also
    // keeps the registry order of alpha-tested leaves within each type.
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return recipes[a].typeID < recipes[b].typeID; });
    out.reserve(order.size());
    for (size_t i : order) out.push_back(recipes[i]);
}

void preparePropInstances(const View& reflected, std::span<const mc2hzb::PropBounds> bounds,
                          std::vector<GpuStaticPropInstance>& out);

} // namespace mc2::waterplanar
