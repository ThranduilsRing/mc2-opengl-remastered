// tests/unit/test_shadow_bake_math.cpp
// The static shadow bake's footprint and reach math: the mask layout, the sun clamp, the caster reach, the shadow footprint a caster can touch, the
// per-tile caster window (it must hold every receiver of the tile and every caster that can shadow one) and the window matrix.
#include "doctest.h"
#include "gameos/shadow_bake_math.h"
#include "gameos/shadow_dirty_tiles.h"
#include "RenderCore/GpuResidencyPolicy.h"
#include "../../shaders/vulkan/include/shadow_bake_common.glsl"

#include <algorithm>

#include <cmath>

using namespace mc2shadow;

TEST_CASE("shadow bake: mc2_01 shore blob is local bank occlusion") {
    // blob-2 diagnostic snapshots, saved_205644 pixel (488,216), owner_lowwide (488,420).
    // Height grid: 397^2 @ 32 wu, water 350; sun is the actual altitude-clamped bake input.
    const float horizontal = std::hypot(0.443912417f, 0.658127189f);
    const float tanAltitude = 0.608120441f / horizontal;
    const float cone = kTerrainConeScale * std::tan(kSunAngularRadiusDeg * 0.01745329252f) / (horizontal * horizontal);
    struct Sample { float receiver, blocker, distance, ray, visibility; };
    const Sample samples[] = {
        // Catmull-Rom bake: both bookmarks reach umbra on the second sample.
        {361.807077f, 368.839285f, 7.359198f, 367.944550f, 0.0f},
        {361.072248f, 368.540864f, 7.967225f, 367.675496f, 0.0f},
        // Actual fine triangles also block the centre ray, but retain part of the sun disk.
        {362.424713f, 381.189973f, 22.082154f, 379.840625f, 0.205886f},
        {361.946045f, 381.057217f, 22.690181f, 379.827732f, 0.239193f},
        // Farther up the same ray the bank clears the entire cone.
        {361.807077f, 405.588520f, 99.595286f, 438.601494f, 1.0f},
    };
    for (const auto& sample : samples) {
        const float ray = bakeTerrainRayHeight(sample.receiver, sample.distance, tanAltitude);
        CHECK(ray == doctest::Approx(sample.ray).epsilon(0.000001));
        CHECK(bakeTerrainMarchVisibility(ray, sample.blocker, sample.distance, cone)
              == doctest::Approx(sample.visibility).epsilon(0.001));
    }
    CHECK(samples[2].blocker - samples[2].ray > 1.3f);
    CHECK(samples[3].blocker - samples[3].ray > 1.2f);
}

TEST_CASE("GPU residency: physical headroom, preset ceiling and overlap admission") {
    using namespace RenderCore;
    CHECK(gpuBudget(512*gpuMiB,512*gpuMiB)==384*gpuMiB);   // 256 MiB margin, capped at a quarter of a 512 MiB heap (128 MiB)
    CHECK(gpuBudget(2048*gpuMiB,UINT64_MAX)==1792*gpuMiB);  // 256 MiB absolute margin
    CHECK(gpuBudget(256*gpuMiB,512*gpuMiB)==192*gpuMiB);   // margin capped at a quarter of a small heap
    CHECK(gpuBudget(16*gpuMiB,512*gpuMiB)==12*gpuMiB);
    CHECK(gpuBudget(24*1024*gpuMiB,UINT64_MAX)==24*1024*gpuMiB-24*1024*gpuMiB/10); // large heaps keep 10%
    // Device-local heap availability: discrete 4/6/8 GiB with and without VK_EXT_memory_budget, and UMA.
    for (uint64_t g : {4ull, 6ull, 8ull}) {
        const uint64_t heap = g*1024*gpuMiB;
        CHECK(gpuHeapAvailable(heap, 0, 0, false, false)==heap/2);                    // no extension: heap/2
        CHECK(gpuBudget(gpuHeapAvailable(heap, 0, 0, false, false), UINT64_MAX) < heap/2);
        CHECK(gpuHeapAvailable(heap, heap-heap/8, heap/8, true, false)==heap-heap/4); // extension: budget minus usage
        CHECK(gpuHeapAvailable(heap, heap, 0, false, true)==heap/4);                  // UMA caps further
        CHECK(gpuHeapAvailable(heap, heap, 0, true, true)==heap/4);
    }
    CHECK(gpuHeapAvailable(4096*gpuMiB, 1024*gpuMiB, 2048*gpuMiB, true, false)==0);   // over-committed
    CHECK(gpuBudget(gpuHeapAvailable(4096*gpuMiB, 0, 0, false, false), 8192*gpuMiB) + 0 >= 1536*gpuMiB); // 4 GiB card still gets >=1.5 GiB
    CHECK(gpuBudget(24*1024*gpuMiB,512*gpuMiB)==512*gpuMiB);
    CHECK(gpuResidencyPolicy(0).total==512*gpuMiB);
    CHECK(gpuResidencyPolicy(0).textureUpload >= 96*gpuMiB); // HD 630 frame ring, including flight copies
    CHECK(gpuResidencyPolicy(0).vegetation==0);
    CHECK(gpuResidencyPolicy(1).total==1024*gpuMiB);
    CHECK(gpuResidencyPolicy(2).total==8192*gpuMiB);
    CHECK(gpuFits(512,400,80,32));
    CHECK_FALSE(gpuFits(512,400,80,33));
    CHECK_FALSE(gpuFits(512,513,0,1));
    CHECK_FALSE(gpuFits(512,400,UINT64_MAX,1));
    CHECK_FALSE(gpuFits(512,400,0,UINT64_MAX));
}

namespace {
Vec3 sunAt(float altDeg, float azDeg) {
    const float a = altDeg * 0.01745329252f, z = azDeg * 0.01745329252f;
    return {std::cos(a) * std::cos(z), std::cos(a) * std::sin(z), std::sin(a)};
}
}  // namespace

TEST_CASE("shadow bake: mask layout is max(target, span / cap) in whole tiles") {
    // mc2_01-sized: 101 vertices -> 12,800 wu at the 1.5 wu target.
    MaskLayout m = maskLayout(101, 101 * 64.0f, 1.5f, 12288);
    CHECK(m.span == doctest::Approx(12800.0f));
    CHECK(m.texels % kTileTexels == 0);
    CHECK(m.texelWorld <= 1.5f);
    CHECK(m.texelWorld > 1.4f);
    CHECK(m.tilesPerSide * kTileTexels == m.texels);
    CHECK(m.originX == doctest::Approx(-101 * 64.0f));
    // A 900-cell map (1 cell = 128/3 wu, 301 vertices) hits the cap: 38,400 wu / 12,288 = 3.125 wu.
    MaskLayout big = maskLayout(301, 301 * 64.0f, 1.5f, 12288);
    CHECK(big.texels == 12288);
    CHECK(big.texelWorld == doctest::Approx(3.125f));
    // The design table's 1024 cells (128/3 wu each: 43,690 wu, 342 vertices) also hits the cap: 24 x 24 tiles at about 3.56 wu.
    MaskLayout editorMax = maskLayout(342, 342 * 64.0f, 1.5f, 12288);
    CHECK(editorMax.texels == 12288);
    CHECK(editorMax.tilesPerSide == 24);
    CHECK(editorMax.texelWorld == doctest::Approx(43648.0f / 12288.0f));
    // Its tile window still fits the 4096 caster window (the window texel grows past 0.5 wu instead of clipping casters).
    const LightBasis lb = lightBasis(sunAt(15.0f, 45.0f));
    const Window ew = casterWindow(lb, tileRegionWorld(editorMax, 23, 23), 0.0f, 400.0f, 600.0f, std::tan(kSunAngularRadiusDeg * 0.01745329252f));
    CHECK(ew.texels == kWindowMaxTexels);
    CHECK(ew.texelWorld > kWindowTexelWorld);
    CHECK(ew.texelWorld < 1.5f);
    // The owner's editor maps: 1024 cells of 128 wu (1025 vertices, about 131 km) still fit the cap, at about 10.7 wu a texel, and the tile
    // window grows its texel past 0.5 wu instead of clipping casters.
    MaskLayout big1k = maskLayout(1025, 1025 * 64.0f, 1.5f, 12288);
    CHECK(big1k.texels == 12288);
    CHECK(big1k.tilesPerSide == 24);
    CHECK(big1k.texelWorld == doctest::Approx(131072.0f / 12288.0f));
    const Window w1k = casterWindow(lightBasis(sunAt(15.0f, 45.0f)), tileRegionWorld(big1k, 12, 12), 0.0f, 800.0f, 1200.0f,
                                    std::tan(kSunAngularRadiusDeg * 0.01745329252f));
    CHECK(w1k.texels == kWindowMaxTexels);
    CHECK(w1k.texelWorld < 4.0f);
    // A different map size is a different layout, so a cooked mask of one never loads for the other.
    CHECK(maskLayout(1024, 1024 * 64.0f, 1.5f, 12288).span != big1k.span);
    // No terrain, no mask.
    CHECK(maskLayout(1, 0.0f, 1.5f, 12288).texels == 0);
}

TEST_CASE("shadow bake: the sun clamp keeps the azimuth and lifts the altitude to the minimum") {
    const Vec3 horizon = clampSunAltitude({1.0f, 0.0f, 0.0f}, 15.0f);
    CHECK(horizon.z == doctest::Approx(std::sin(15.0f * 0.01745329252f)));
    CHECK(horizon.y == doctest::Approx(0.0f));
    CHECK(horizon.x > 0.9f);
    const Vec3 high = sunAt(60.0f, 30.0f);
    const Vec3 kept = clampSunAltitude(high, 15.0f);
    CHECK(kept.x == doctest::Approx(high.x));
    CHECK(kept.z == doctest::Approx(high.z));
}

TEST_CASE("shadow bake: reach is h * cot(altitude), at most 3.73 h at the 15 degree clamp") {
    CHECK(reachWorld(10.0f, sunAt(45.0f, 0.0f)) == doctest::Approx(10.0f).epsilon(1e-4));
    CHECK(reachWorld(10.0f, sunAt(15.0f, 77.0f)) == doctest::Approx(37.32f).epsilon(1e-3));
    CHECK(reachWorld(10.0f, sunAt(90.0f, 0.0f)) == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(penumbraWorld(100.0f, std::tan(2.0f * 0.01745329252f)) == doctest::Approx(3.492f).epsilon(1e-3));
}

TEST_CASE("shadow bake: a caster's footprint covers where its top lands, away from the sun") {
    // Sun in the east at 45 degrees: a 20 wu tall box throws its shadow 20 wu west.
    const Rect r = shadowFootprint({0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 20.0f}, sunAt(45.0f, 0.0f), 0.0f, 0.0f);
    CHECK(r.x0 == doctest::Approx(-20.0f).epsilon(1e-3));
    CHECK(r.x1 == doctest::Approx(2.0f));
    CHECK(r.y0 == doctest::Approx(0.0f));
    CHECK(r.y1 == doctest::Approx(2.0f));
    // A penumbra widens it on every side.
    const Rect soft = shadowFootprint({0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 20.0f}, sunAt(45.0f, 0.0f), 0.0f, 0.05f);
    CHECK(soft.x0 < r.x0);
    CHECK(soft.y1 > r.y1);
}

TEST_CASE("shadow bake: the tile window holds every receiver and every caster that can shadow one") {
    const MaskLayout m = maskLayout(101, 101 * 64.0f, 1.5f, 12288);
    const float tanT = std::tan(kSunAngularRadiusDeg * 0.01745329252f);
    for (float alt : {15.0f, 35.0f, 70.0f}) {
        const Vec3 sun = sunAt(alt, 123.0f);
        const LightBasis b = lightBasis(sun);
        const Rect region = tileRegionWorld(m, 3, 5);
        const float hMin = 10.0f, hMax = 60.0f, top = 160.0f;
        const Window w = casterWindow(b, region, hMin, hMax, top, tanT);
        float mtx[16];
        windowMatrix(b, w, mtx);
        CHECK(w.texels <= kWindowMaxTexels);
        CHECK(w.texelWorld >= kWindowTexelWorld);
        // Receivers land inside the window, between its depth planes.
        for (int c = 0; c < 8; ++c) {
            const Vec3 p{(c & 1) ? region.x1 : region.x0, (c & 2) ? region.y1 : region.y0, (c & 4) ? hMax : hMin};
            const Vec3 q = applyMatrix(mtx, p);
            CHECK(std::fabs(q.x) < 1.0f);
            CHECK(std::fabs(q.y) < 1.0f);
            CHECK(q.z > 0.0f);
            CHECK(q.z < 1.0f);
            // The highest caster on this receiver's ray is in the window too, nearer the sun (smaller depth).
            const float ray = (top - p.z) / sun.z;
            const Vec3 caster{p.x + sun.x * ray, p.y + sun.y * ray, p.z + sun.z * ray};
            const Vec3 cq = applyMatrix(mtx, caster);
            CHECK(cq.x == doctest::Approx(q.x).epsilon(1e-3));
            CHECK(cq.y == doctest::Approx(q.y).epsilon(1e-3));
            CHECK(cq.z >= 0.0f);
            CHECK(cq.z < q.z);
            // Its light box is binned to the window.
            CHECK(casterInWindow(lightBox(b, {caster.x - 1, caster.y - 1, caster.z - 30}, {caster.x + 1, caster.y + 1, caster.z}), w));
        }
        // A caster far beside the tile is not.
        CHECK_FALSE(casterInWindow(lightBox(b, {region.x1 + 5000, region.y1 + 5000, 0}, {region.x1 + 5010, region.y1 + 5010, 50}), w));
    }
}

TEST_CASE("shadow bake: neighbouring tiles share the window texel grid") {
    const MaskLayout m = maskLayout(101, 101 * 64.0f, 1.5f, 12288);
    const LightBasis b = lightBasis(sunAt(30.0f, 40.0f));
    const Window a = casterWindow(b, tileRegionWorld(m, 2, 2), 0.0f, 40.0f, 120.0f, 0.03f);
    const Window c = casterWindow(b, tileRegionWorld(m, 3, 2), 0.0f, 40.0f, 120.0f, 0.03f);
    REQUIRE(a.texelWorld == c.texelWorld);
    const float shift = (c.lx0 - a.lx0) / a.texelWorld;
    CHECK(shift == doctest::Approx(std::round(shift)).epsilon(1e-4));
}

TEST_CASE("shadow bake dirty tiles: a footprint covers its sub-tiles plus one of margin, clipped to the mask") {
    const MaskLayout m = maskLayout(101, 101 * 64.0f, 1.5f, 12288);
    const float st = static_cast<float>(kSubTileTexels) * m.texelWorld;
    // A rect inside sub-tile (3, 4): 3x3 sub-tiles with the margin.
    const float x = m.originX + 3.5f * st, y = m.originYNorth - 4.5f * st;
    const auto tiles = subTilesForRect(m, {x - 1.0f, y - 1.0f, x + 1.0f, y + 1.0f});
    CHECK(tiles.size() == 9);
    CHECK(std::find(tiles.begin(), tiles.end(), SubTile{3, 4}) != tiles.end());
    // The map corner clips the margin.
    CHECK(subTilesForRect(m, {m.originX, m.originYNorth - 1.0f, m.originX + 1.0f, m.originYNorth}).size() == 4);
    // Off the map: nothing.
    CHECK(subTilesForRect(m, {m.originX - 5000.0f, 0.0f, m.originX - 4000.0f, 10.0f}).empty());
    // A sub-tile's region is its texels plus the apron on every side.
    const Rect r = subTileRegionWorld(m, {0, 0});
    CHECK(r.x1 - r.x0 == doctest::Approx(static_cast<float>(kSubTileTexels + 2 * kTileApron) * m.texelWorld));
}

TEST_CASE("shadow bake dirty tiles: the queue keeps each sub-tile once, in order, under a budget") {
    DirtyQueue q;
    q.reset(8);
    CHECK(q.add({{1, 1}, {2, 1}, {1, 1}}) == 2);
    CHECK(q.add({{2, 1}, {3, 3}, {9, 9}}) == 1);   // a repeat and an out-of-range tile are dropped
    CHECK(q.size() == 3);
    const auto first = q.take(2);
    REQUIRE(first.size() == 2);
    CHECK(first[0] == SubTile{1, 1});
    CHECK(first[1] == SubTile{2, 1});
    CHECK(q.add({{1, 1}}) == 1);   // taken tiles can be queued again
    CHECK(q.take(4).size() == 2);
    CHECK(q.empty());
}

#include "shadow_quality.h"
TEST_CASE("Shadow preset dimensions bound potato resident memory") {
    using namespace mc2shadow;
    const auto p=qualityForLevel(0), m=qualityForLevel(1), h=qualityForLevel(2);
    CHECK(p.mapSide==2048); CHECK(p.windowSide==2048); CHECK(p.maskCap==4096);
    CHECK(m.maskTarget==2.0f); CHECK(m.maskCap==8192); CHECK(h.maskTarget==1.5f);
    CHECK(qualityForLimit(2,8192).level==1);
    CHECK(qualityForLimit(2,8192).maskCap==8192);
    CHECK(qualityForLimit(2,2048).maskCap==2048);
    CHECK(qualityForLimit(2,2048).mapSide==2048);
    const uint64_t resident=uint64_t(p.maskCap)*p.maskCap*2/3+uint64_t(p.mapSide)*p.mapSide*4;
    CHECK(resident<=32*1024*1024);
    const auto basis=lightBasis({1,0,1});
    const auto window=casterWindow(basis,{-256,-256,256,256},0,100,200,0.03f,p.windowSide);
    CHECK(window.texels==2048); CHECK(window.texelWorld>0);
}

TEST_CASE("shadow bake dirty tiles: aborted batches requeue until uploads commit") {
    DirtyQueue q; q.reset(4); q.add({{0, 0}, {1, 1}});
    for (int failure = 0; failure < 3; ++failure) {
        { DirtyBatch aborted(q, 2); CHECK(aborted.tiles.size() == 2); CHECK(q.empty()); }
        CHECK(q.size() == 2);
    }
    { DirtyBatch uploaded(q, 2); uploaded.commit(); }
    CHECK(q.empty());
}

TEST_CASE("shadow bake: heights are bounded by mask resolution and preserve a planar surface") {
    std::vector<float> visual(17 * 17);
    for (int y = 0; y < 17; ++y) for (int x = 0; x < 17; ++x) visual[y * 17 + x] = float(x + 2 * y);
    const auto grid = maskHeightGrid(visual, 17, 4);
    REQUIRE(grid.size() == 25);
    for (int y = 0; y < 5; ++y) for (int x = 0; x < 5; ++x) CHECK(grid[y * 5 + x] == float(4 * x + 8 * y));
}

TEST_CASE("GPU residency: vegetation thins by stride instead of refusing") {
    using namespace RenderCore;
    // mc2_03 at the default quality: 2.5M instances, ~30k regions in the packed model (16 B instances): about 48 MiB resident.
    const auto q2 = gpuResidencyPolicy(2);
    CHECK(q2.vegetation == vegetationResidentCeiling);   // the upload's own 160 MiB ceiling is the policy
    CHECK(vegetationBytes(2512462, 30000) < 60 * gpuMiB);
    CHECK(vegetationStride(2512462, 30000, q2.vegetation, 2000 * gpuMiB) == 1);
    CHECK(vegetationStride(9000000, 40000, q2.vegetation, 2000 * gpuMiB) == 1);   // mc2_01 meadow field fits whole
    // a dense mod past the 160 MiB ceiling thins by stride instead of losing every instance
    const uint32_t sDense = vegetationStride(14000000, 40000, q2.vegetation, 2000 * gpuMiB);
    CHECK(sDense > 1);
    CHECK(vegetationBytes((14000000 + sDense - 1) / sDense, 40000) <= q2.vegetation);
    // quality 1 (32 MiB ceiling) thins rather than refuses; the thinned population fits.
    const auto q1 = gpuResidencyPolicy(1);
    const uint32_t s1 = vegetationStride(2512462, 30000, q1.vegetation, 2000 * gpuMiB);
    CHECK(s1 > 1);
    const uint64_t n = (2512462 + s1 - 1) / s1;
    const uint64_t r1 = n < 30000 ? n : 30000;
    CHECK(vegetationBytes(n, r1) <= q1.vegetation);
    // a tight live headroom thins too (staging counts); potato (ceiling 0) never fits.
    CHECK(vegetationStride(2512462, 30000, q2.vegetation, 60 * gpuMiB) > 1);
    CHECK(vegetationStride(2512462, 30000, gpuResidencyPolicy(0).vegetation, 2000 * gpuMiB) == 0);
}

TEST_CASE("q2 ledger admits the 16384x8192 BC6H HDRI mip chain plus its staging copy; the edge is the case the sky island degrades on") {
    using namespace RenderCore;
    const auto q2 = gpuResidencyPolicy(2);
    const uint64_t mip0 = 16384ull * 8192ull;          // BC6H: 1 byte per texel
    const uint64_t chain = mip0 + mip0 / 3;            // full mip chain, about 4/3 of mip 0
    const uint64_t budget = gpuBudget(24 * 1024 * gpuMiB, q2.total);
    // vulkan_sky_island.cpp admits the image, then the staging buffer, through the OwnedImage/OwnedBuffer ledger (gpuFits).
    CHECK(gpuFits(budget, 0, 0, chain));
    CHECK(gpuFits(budget, 0, chain, chain));
    // Headroom for the image but not image + staging: the staging refusal must take the 1x1 dummy path, not fail the island.
    const uint64_t live = budget - chain - chain / 2;
    CHECK(gpuFits(budget, live, 0, chain));
    CHECK_FALSE(gpuFits(budget, live, chain, chain));
}
