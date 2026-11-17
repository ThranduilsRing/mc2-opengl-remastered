#pragma once
#include "../GameOS/gameos/gos_vegetation.h"
#include "vegetation_budget.h"
#include "../mclib/fast_hash.h"
#include <algorithm>
#include <cmath>
#include <utility>
#include "../GameOS/gameos/cook_cache.h"
#include "../code/save_archive.h"
#include <bit>

namespace mc2::vegetation {
struct SourceBytes {
    std::vector<uint8_t> bytes;
    void word(uint64_t v) { mc2cache::putLE(bytes, v, 8); }
    void scalar(float v) { mc2cache::putLE(bytes, std::bit_cast<uint32_t>(v), 4); }
    void raw(std::span<const uint8_t> v) { word(v.size()); bytes.insert(bytes.end(), v.begin(), v.end()); }
    void text(std::string_view v) { raw({reinterpret_cast<const uint8_t*>(v.data()), v.size()}); }
    auto digest() const { return mc2::save::sha256(bytes); }
};
inline mc2::save::Digest hardSurfaceDigest(int64_t side, int64_t half, float wupv,
    std::span<const int32_t> blocks, std::span<const uint8_t> texels) {
    SourceBytes s; s.word(side); s.word(half); s.scalar(wupv); s.word(blocks.size());
    for (auto b : blocks) mc2cache::putLE(s.bytes, static_cast<uint32_t>(b), 4);
    s.raw(texels); return s.digest();
}
struct PopulationRegion {
    float x, y, z, radius;
    uint32_t first, last, pad0, pad1;
};
static_assert(sizeof(PopulationRegion) == 32);
struct Population {
    std::vector<PackedInstance> instances;
    std::vector<PopulationRegion> regions;
    uint32_t stride = 1;
};
inline auto cell(const GosVegetation::Instance& i) {
    return std::pair{static_cast<int>(std::floor(i.x/128.0f)),static_cast<int>(std::floor(i.y/128.0f))};
}
template<class ChooseStride>
inline Population buildPopulationWith(std::span<const GosVegetation::Instance> data, ChooseStride chooseStride) {
    Population result;
    // Placement is immutable between uploads. Group nearby clumps once so the device walk rejects regions instead of revisiting the whole map.
    std::vector<GosVegetation::Instance> sorted(data.begin(), data.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return cell(a) < cell(b); });
    // Over the preset ceiling or the live GPU headroom: keep every Nth clump (the sort keeps the thinning even per cell) instead of dropping all vegetation.
    {
        size_t regions0 = 0;
        for (size_t k = 0; k < sorted.size(); ++k) if (k == 0 || cell(sorted[k]) != cell(sorted[k - 1])) ++regions0;
        const uint32_t stride = chooseStride(sorted.size(), regions0);
        result.stride = stride;
        if (stride > 1) {
            size_t kept = 0;
            for (size_t k = 0; k < sorted.size(); k += stride) sorted[kept++] = sorted[k];
            sorted.resize(kept);

        }
    }
    auto& gpuInstances = result.instances;
    gpuInstances.resize(sorted.size());
    for (size_t k = 0; k < sorted.size(); ++k) {
        const auto& i = sorted[k];
        gpuInstances[k] = mc2::vegetation::packInstance(i.x, i.y, i.z, i.yaw, i.scale, i.atlasFrame, i.seed);
    }
    auto& gpuRegions = result.regions;
    for (size_t begin = 0; begin < sorted.size();) {
        size_t end = begin + 1;
        const auto key = cell(sorted[begin]);
        while (end < sorted.size() && cell(sorted[end]) == key) ++end;
        float lo = sorted[begin].z, hi = lo, pad = 0.0f;
        for (size_t j = begin; j < end; ++j) {
            lo = std::min(lo, sorted[j].z); hi = std::max(hi, sorted[j].z);
            pad = std::max(pad, 1.5f * sorted[j].scale);
        }
        const auto [cellX, cellY] = key;
        gpuRegions.push_back({(cellX + 0.5f) * 128.0f, (cellY + 0.5f) * 128.0f, (lo + hi) * 0.5f,
            std::sqrt(2.0f * 64.0f * 64.0f + (hi - lo) * (hi - lo) * 0.25f) + pad, static_cast<uint32_t>(begin), static_cast<uint32_t>(end), 0u, 0u});
        begin = end;
    }
    return result;
}
inline Population buildPopulation(std::span<const GosVegetation::Instance> data, uint32_t stride) {
    return buildPopulationWith(data, [stride](size_t, size_t) { return std::max(stride, 1u); });
}
template<class T> inline uint64_t populationHash(const std::vector<T>& v) {
    return mc2::content::fastHash({reinterpret_cast<const uint8_t*>(v.data()), v.size() * sizeof(T)});
}
} // namespace mc2::vegetation
