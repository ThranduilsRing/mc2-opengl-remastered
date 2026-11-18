#pragma once
#include "../GameOS/gameos/gos_vegetation.h"
#include "vegetation_budget.h"
#include "vegetation_clumps.h"
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
inline auto buildIdentity(std::string_view fingerprint, bool dirty, uint64_t size = 0, uint64_t modified = 0) {
    SourceBytes s; s.text(fingerprint); s.word(dirty);
    if (dirty) { s.word(size); s.word(modified); }
    return s.digest();
}
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

struct SourceInput {
    std::string_view mapName;
    uint32_t seed = 0;
    // originXY, mapSpan, wupv, invWupv, water, gridStep, invMapSpan, invCellWU.
    std::array<float, 9> frame{};
    // realVertices, halfVertices, verticesBlockSide, blocksMapSide, mapPresent, mapH, mapW, visualV, density.
    std::array<uint64_t, 9> grid{};
    std::span<const uint32_t> types;
    std::span<const float> elevations, visible;
    std::span<const uint8_t> masks;
    std::array<ClumpRecipe, 4> recipes{};
    std::optional<FieldRecipe> field;
    mc2::save::Digest hardSurface{};
};
struct SourceGroup {
    template<class F> void operator()(const char*, F&& work) const { work(); }
};
template<class Group = SourceGroup>
inline SourceBytes serializeSource(const SourceInput& in, Group group = {}) {
    SourceBytes s; s.text(in.mapName); s.word(in.seed);
    for (auto f : in.frame) s.scalar(f);
    for (auto n : in.grid) s.word(n);
    group("cook.veg.key.types", [&] { s.word(in.types.size()); for (auto t : in.types) mc2cache::putLE(s.bytes,t,4); });
    group("cook.veg.key.heights", [&] { s.word(in.elevations.size()); for (auto f : in.elevations) s.scalar(f); });
    group("cook.veg.key.visual", [&] { s.word(in.visible.size()); for (auto f : in.visible) s.scalar(f); });
    group("cook.veg.key.masks", [&] { s.raw(in.masks); });
    s.raw(in.hardSurface);
    group("cook.veg.key.config", [&] {
    for (const auto& r : in.recipes) { s.scalar(r.radius); s.scalar(r.treeChance); s.scalar(r.moistureBias); s.word(r.grass); }
    s.word(in.field.has_value()); if (in.field) { s.scalar(in.field->spacing); s.scalar(in.field->height); }
    });
    return s;
}
inline constexpr uint32_t kFormat = 1;
// Placement/classification/grid/beach; clumps/noise/hardsurface; packInstance;
// sort key, 128-WU cells and region maths. Compiled build identity also guards code/toolchain changes.
inline constexpr uint32_t kProducer = 1;
using Digest = mc2::save::Digest;
struct CapacityInterval { uint64_t lo = 0, hi = 0; bool contains(uint64_t n) const { return n >= lo && n < hi; } };
inline std::optional<CapacityInterval> capacityInterval(uint64_t c, uint64_t f, uint32_t k, uint32_t k0, uint64_t cap) {
    if (c >= cap || k > k0 || k0 > 8) return std::nullopt;
    const auto max = UINT64_MAX;
    if (f > (max-c)/81) return std::nullopt;
    return CapacityInterval{std::max(c+1, c+static_cast<uint64_t>(k)*k*f),
        f && k < k0 ? c+static_cast<uint64_t>(k+1)*(k+1)*f : max};
}
inline bool cacheAdmission(bool mapPresent, bool legacyAdjustment, bool executableKnown, uint32_t stride = 1) {
    return mapPresent && !legacyAdjustment && executableKnown && stride == 1;
}
struct PopulationRecord {
    Population population;
    CapacityInterval interval;
    std::array<int32_t, 25> counters{};
};
inline constexpr uint64_t kBlobMagic = 0x31424f4c42504756ull;
inline constexpr size_t kBlobHeader = 204;
inline uint64_t fileKey(const Digest& d) { return mc2cache::getLE(d, 0, 8); }
inline Digest sourceDigest(const SourceBytes& inputs, const Digest& exe, uint32_t producer = kProducer, uint32_t format = kFormat) {
    SourceBytes s; s.word(format); s.word(producer); s.raw(exe); s.raw(inputs.bytes); return s.digest();
}
inline std::vector<uint8_t> encode(const PopulationRecord& r, const Digest& source) {
    static_assert(std::endian::native == std::endian::little, "GPU payload requires little endian");
    std::vector<uint8_t> out;
    auto put = [&](uint64_t v, unsigned n) { mc2cache::putLE(out, v, n); };
    put(kBlobMagic,8); put(kFormat,4); put(kProducer,4); out.insert(out.end(),source.begin(),source.end());
    put(r.population.instances.size(),8); put(r.population.regions.size(),8); put(16,4); put(32,4);
    put(r.interval.lo,8); put(r.interval.hi,8);
    put(mc2::content::fastHash({reinterpret_cast<const uint8_t*>(r.population.regions.data()),r.population.regions.size()*32},populationHash(r.population.instances)),8);
    for (auto c : r.counters) put(static_cast<uint32_t>(c),4);
    put(mc2::content::fastHash(out),8);
    auto append = [&](const auto& v) { const auto* p = reinterpret_cast<const uint8_t*>(v.data()); if (!v.empty()) out.insert(out.end(),p,p+v.size()*sizeof(v[0])); };
    append(r.population.instances); append(r.population.regions); return out;
}
inline std::optional<PopulationRecord> decode(std::span<const uint8_t> b, const Digest& source, uint64_t maxInst) {
    if (b.size() < kBlobHeader) return std::nullopt;
    auto get = [&](size_t pos, unsigned n) { return mc2cache::getLE(b,pos,n); };
    if (get(196,8)!=mc2::content::fastHash(b.first(196))) return std::nullopt;
    if (get(0,8)!=kBlobMagic || get(8,4)!=kFormat || get(12,4)!=kProducer || !std::equal(source.begin(),source.end(),b.begin()+16)) return std::nullopt;
    const uint64_t n=get(48,8), m=get(56,8);
    if (n >= (1ull<<31) || m > 65535 || get(64,4)!=16 || get(68,4)!=32) return std::nullopt;
    const auto payload=b.subspan(kBlobHeader);
    if (n > payload.size()/16 || m > (payload.size()-n*16)/32 || n*16+m*32!=payload.size()) return std::nullopt;
    CapacityInterval interval{get(72,8),get(80,8)};
    if (!interval.contains(maxInst) || get(88,8)!=mc2::content::fastHash(payload)) return std::nullopt;
    uint32_t end=0;
    for (uint64_t i=0;i<m;++i) {
        const size_t pos=kBlobHeader+n*16+i*32;
        for (size_t j=0;j<16;j+=4) if (!std::isfinite(std::bit_cast<float>(static_cast<uint32_t>(get(pos+j,4))))) return std::nullopt;
        const auto first=get(pos+16,4), last=get(pos+20,4);
        if (first!=end || last<=first || last>n) return std::nullopt;
        end=static_cast<uint32_t>(last);
    }
    if (end!=n) return std::nullopt;
    PopulationRecord r; r.interval=interval;
    for (size_t i=0;i<r.counters.size();++i) { r.counters[i]=std::bit_cast<int32_t>(static_cast<uint32_t>(get(96+i*4,4))); if(r.counters[i]<0) return std::nullopt; }
    r.population.instances.resize(n); r.population.regions.resize(m);
    if(n) std::memcpy(r.population.instances.data(),payload.data(),n*16);
    if(m) std::memcpy(r.population.regions.data(),payload.data()+n*16,m*32);
    return r;
}
} // namespace mc2::vegetation
