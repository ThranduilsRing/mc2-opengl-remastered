// PROP-ARRAY-BUCKETS: the static-prop alpha-ON albedo group splits into size-bucket arrays (offline, header-only).
#include "doctest.h"
#include "GameOS/gameos/prop_array_buckets.h"
#include "GameOS/gameos/vegetation_cutout.h"
#include <vector>

using namespace mc2::prop_buckets;

TEST_CASE("PropArrayBuckets/mixed 128-256-1024 group: no member is placed in an array above its own size class") {
    // mc2_01's alpha-ON group after stock-tree-upscale: 128 stock cards, a 256 fence, five 1024 tree atlases.
    const std::vector<Dims> members = {{128, 128}, {1024, 1024}, {128, 128}, {256, 256}, {128, 64},
                                       {1024, 1024}, {128, 128}, {1024, 1024}, {256, 128}, {1024, 1024}, {1024, 1024}};
    const Plan plan = planBuckets(members, kMaxBuckets);
    REQUIRE(plan.bucketDims.size() == 3);
    CHECK(plan.bucketDims[0].w == 1024); CHECK(plan.bucketDims[0].h == 1024);
    CHECK(plan.bucketDims[1].w == 256);  CHECK(plan.bucketDims[1].h == 256);
    CHECK(plan.bucketDims[2].w == 128);  CHECK(plan.bucketDims[2].h == 128);
    size_t layers = 0;
    for (size_t i = 0; i < members.size(); ++i) {
        const Dims& d = plan.bucketDims[plan.bucket[i]];
        CHECK(d.w >= members[i].w);
        CHECK(d.h >= members[i].h);
        // The array edge never exceeds the member's own power-of-two class.
        CHECK(sizeClass(d.w, d.h) == sizeClass(members[i].w, members[i].h));
        CHECK(plan.layer[i] >= 0);
        CHECK(plan.layer[i] < static_cast<int32_t>(plan.bucketLayers[plan.bucket[i]]));
    }
    for (uint32_t n : plan.bucketLayers) layers += n;
    CHECK(layers == members.size());
    CHECK(plan.bucketLayers[0] == 5);
    CHECK(plan.bucketLayers[1] == 2);
    CHECK(plan.bucketLayers[2] == 4);
    // Bytes of mip 0: one 1024 array for all eleven would be 11 x 4 MiB; the buckets hold 5 x 4 MiB + 2 x 256 KiB + 4 x 64 KiB.
    size_t bucketed = 0;
    for (size_t b = 0; b < plan.bucketDims.size(); ++b)
        bucketed += static_cast<size_t>(plan.bucketDims[b].w) * plan.bucketDims[b].h * 4u * plan.bucketLayers[b];
    CHECK(bucketed < static_cast<size_t>(1024) * 1024 * 4u * 6u);
}

TEST_CASE("PropArrayBuckets/one bucket keeps the single-array layout") {
    const std::vector<Dims> members = {{64, 64}, {256, 256}, {128, 128}};
    const Plan plan = planBuckets(members, 1);
    REQUIRE(plan.bucketDims.size() == 1);
    CHECK(plan.bucketDims[0].w == 256);
    for (size_t i = 0; i < members.size(); ++i) {
        CHECK(plan.bucket[i] == 0u);
        CHECK(plan.layer[i] == static_cast<int32_t>(i));
        CHECK(encodeLayer(plan.bucket[i], plan.layer[i]) == static_cast<int32_t>(i));   // bucket 0 = the plain layer
    }
}

TEST_CASE("PropArrayBuckets/alpha-OFF mix: non-square members, every bucket holds its members, far fewer texels than one array") {
    // An alpha-OFF-like group: a few 1024 building atlases, many 256 / 256x128 walls, 128s and one 64 (5 classes, 4 buckets).
    std::vector<Dims> members = {{1024, 1024}, {1024, 512}, {64, 64}};
    for (int i = 0; i < 20; ++i) members.push_back({256, i % 2 ? 128 : 256});
    for (int i = 0; i < 15; ++i) members.push_back({128, 128});
    members.push_back({512, 256});
    const Plan plan = planBuckets(members, kMaxBuckets);
    REQUIRE(plan.bucketDims.size() == kMaxBuckets);
    CHECK(plan.bucketDims[0].w == 1024);
    CHECK(plan.bucketDims[0].h == 1024);
    CHECK(plan.bucketDims[1].w == 512);
    CHECK(plan.bucketDims[1].h == 256);   // the extent is the members' max, not the square class
    CHECK(plan.bucketDims[2].w == 256);
    CHECK(plan.bucketDims[3].w == 128);
    CHECK(plan.bucket[2] == 3u);           // the 64 folds into the 128 bucket
    uint64_t bucketTexels = 0;
    std::vector<uint32_t> seen(plan.bucketDims.size(), 0u);
    for (size_t i = 0; i < members.size(); ++i) {
        const Dims& d = plan.bucketDims[plan.bucket[i]];
        CHECK(d.w >= members[i].w);
        CHECK(d.h >= members[i].h);
        CHECK(plan.layer[i] == static_cast<int32_t>(seen[plan.bucket[i]]++));   // dense layers in member order
        const int32_t enc = encodeLayer(plan.bucket[i], plan.layer[i]);
        CHECK(bucketOf(enc) == plan.bucket[i]);
        CHECK(layerOf(enc) == plan.layer[i]);
        bucketTexels += static_cast<uint64_t>(d.w) * static_cast<uint64_t>(d.h);
    }
    for (size_t b = 0; b < seen.size(); ++b) CHECK(plan.bucketLayers[b] == seen[b]);
    const uint64_t singleTexels = static_cast<uint64_t>(members.size()) * 1024u * 1024u;
    CHECK(bucketTexels * 8 < singleTexels);   // 39 layers at 1024^2 vs 2 x 1024^2 + 1 x 512x256 + 20 x 256^2 + 16 x 128^2
}

TEST_CASE("PropArrayBuckets/more size classes than buckets fold the smallest into the smallest kept bucket") {
    const std::vector<Dims> members = {{2048, 2048}, {1024, 1024}, {512, 512}, {256, 256}, {128, 128}, {64, 64}};
    const Plan plan = planBuckets(members, 4);
    REQUIRE(plan.bucketDims.size() == 4);
    CHECK(plan.bucket[0] == 0u); CHECK(plan.bucket[1] == 1u); CHECK(plan.bucket[2] == 2u); CHECK(plan.bucket[3] == 3u);
    CHECK(plan.bucket[4] == 3u); CHECK(plan.bucket[5] == 3u);
    for (size_t i = 0; i < members.size(); ++i) CHECK(plan.bucketDims[plan.bucket[i]].w >= members[i].w);
    CHECK(planBuckets({}, 4).bucketDims.empty());
}

TEST_CASE("PropArrayBuckets/layer index round-trip") {
    for (uint32_t bucket = 0; bucket < kMaxBuckets; ++bucket)
        for (int32_t layer : {0, 1, 30, 62, 1000, 65535}) {
            const int32_t encoded = encodeLayer(bucket, layer);
            CHECK(encoded >= 0);
            CHECK(bucketOf(encoded) == bucket);
            CHECK(layerOf(encoded) == layer);
        }
    CHECK(encodeLayer(2, -1) == -1);   // "no texture" stays the -1 sentinel every consumer tests
    CHECK(layerOf(-1) == -1);
    CHECK(bucketOf(-1) == 0u);
}

TEST_CASE("PropArrayBuckets/a 128 canopy alone in its 128 bucket still reads as a solid cutout (per-image erosion)") {
    // A crown filling the image with a 2-texel transparent border: the erosion radius follows the image (d1bc156c8),
    // so the classification is the same in its own 128 array as in the corner of the former 1024 layer.
    const uint32_t n = 128;
    std::vector<uint8_t> own(static_cast<size_t>(n) * n * 4, 0);
    for (uint32_t y = 2; y + 2 < n; ++y)
        for (uint32_t x = 2; x + 2 < n; ++x) own[(static_cast<size_t>(y) * n + x) * 4 + 3] = 255;
    CHECK(mc2::vegetation::isSolidCutout(own, n, n, n));
    const uint32_t big = 1024;
    std::vector<uint8_t> corner(static_cast<size_t>(big) * big * 4, 0);
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x) corner[(static_cast<size_t>(y) * big + x) * 4 + 3] = own[(static_cast<size_t>(y) * n + x) * 4 + 3];
    CHECK(mc2::vegetation::isSolidCutout(corner, big, big, n));
    // Coverage is a fraction of the layer: the per-mip preservation targets the member's own texels either way.
    CHECK(mc2::vegetation::cutoutCoverage(own) * (n * n) == doctest::Approx(mc2::vegetation::cutoutCoverage(corner) * (big * big)));
}
