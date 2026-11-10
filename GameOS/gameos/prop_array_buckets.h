#pragma once
// PROP-ARRAY-BUCKETS: the static-prop alpha-ON albedo group is split into up to kMaxBuckets texture arrays by size
// class, so one 1024 tree atlas no longer makes every 128 layer of the group 1024 (one array is sized to its
// largest member). A packet's layer int carries (bucket, layer-in-bucket); bucket 0 encodes as the plain layer,
// so a one-bucket group (alpha-OFF) keeps its layer values. Shaders decode with shaders/vulkan/include/prop_array_bucket.glsl.
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace mc2::prop_buckets {

inline constexpr uint32_t kMaxBuckets = 4;   // PROP_ALPHA_ON_BUCKETS in prop_array_bucket.glsl
inline constexpr uint32_t kLayerBits = 16;   // PROP_BUCKET_LAYER_BITS in prop_array_bucket.glsl

constexpr int32_t encodeLayer(uint32_t bucket, int32_t layer) {
    return layer < 0 ? -1 : static_cast<int32_t>((bucket << kLayerBits) | static_cast<uint32_t>(layer));
}
constexpr uint32_t bucketOf(int32_t encoded) { return encoded < 0 ? 0u : static_cast<uint32_t>(encoded) >> kLayerBits; }
constexpr int32_t layerOf(int32_t encoded) {
    return encoded < 0 ? -1 : static_cast<int32_t>(static_cast<uint32_t>(encoded) & ((1u << kLayerBits) - 1u));
}

// Power-of-two class of an image: ceil(log2(max(w, h))).
constexpr uint32_t sizeClass(int w, int h) {
    const uint32_t edge = static_cast<uint32_t>(std::max({w, h, 1}));
    uint32_t c = 0;
    while ((1u << c) < edge) ++c;
    return c;
}

struct Dims {
    int w = 0;
    int h = 0;
};

struct Plan {
    std::vector<uint32_t> bucket;      // per member: its bucket (0 = the largest class)
    std::vector<int32_t> layer;        // per member: its layer within that bucket (member order)
    std::vector<Dims> bucketDims;      // per bucket: the array extent (max member w, max member h)
    std::vector<uint32_t> bucketLayers;
};

// The maxBuckets largest size classes each get an array; a member smaller than every kept class joins the smallest
// kept one. A member never lands in an array whose extent is below its own, and never above the extent of the
// smallest kept class that holds it.
inline Plan planBuckets(std::span<const Dims> members, uint32_t maxBuckets) {
    Plan plan;
    if (members.empty()) return plan;
    std::vector<uint32_t> classes;
    for (const Dims& d : members) classes.push_back(sizeClass(d.w, d.h));
    std::vector<uint32_t> kept = classes;
    std::sort(kept.begin(), kept.end(), [](uint32_t a, uint32_t b) { return a > b; });
    kept.erase(std::unique(kept.begin(), kept.end()), kept.end());
    kept.resize(std::min<size_t>(kept.size(), std::max(maxBuckets, 1u)));
    plan.bucketDims.assign(kept.size(), Dims{});
    plan.bucketLayers.assign(kept.size(), 0u);
    for (size_t i = 0; i < members.size(); ++i) {
        uint32_t b = 0;
        while (b + 1 < kept.size() && kept[b + 1] >= classes[i]) ++b;
        plan.bucket.push_back(b);
        plan.layer.push_back(static_cast<int32_t>(plan.bucketLayers[b]++));
        plan.bucketDims[b].w = std::max(plan.bucketDims[b].w, members[i].w);
        plan.bucketDims[b].h = std::max(plan.bucketDims[b].h, members[i].h);
    }
    return plan;
}

}  // namespace mc2::prop_buckets
