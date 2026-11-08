#include "doctest.h"
#include "terrain_lod_rules.h"

#include <cmath>
#include <vector>

using namespace mc2lod;

namespace {

float edgeField(int i) {  // deterministic rough heights; no two neighbours collinear
    unsigned h = static_cast<unsigned>(i) * 2654435761u;
    h ^= h >> 13;
    return float(h % 1000u) * 0.37f;
}

// Height of the polyline through (positions[k], heights[k]) at integer x.
float polylineAt(const std::vector<int>& pos, const std::vector<float>& hgt, int x) {
    for (size_t k = 0; k + 1 < pos.size(); ++k)
        if (x >= pos[k] && x <= pos[k + 1]) {
            const float t = float(x - pos[k]) / float(pos[k + 1] - pos[k]);
            return hgt[k] * (1.0f - t) + hgt[k + 1] * t;
        }
    return 1e30f;
}

// The edge a block of stride `step` draws when its shared edge is snapped to `snap`: its vertices and the heights the shader
// gives them.
void drawnEdge(int step, int snap, int ext, std::vector<int>& pos, std::vector<float>& hgt) {
    int s[64];
    const int n = edgeSamples(step, ext, s, 64);
    pos.assign(s, s + n);
    hgt.clear();
    for (int k = 0; k < n; ++k)
        hgt.push_back(edgeHeight(s[k], step, snap, ext, edgeField));
}

}  // namespace

TEST_CASE("terrain LOD steps: divide the block, increase, and only 4 vs 5 are non-nested neighbours") {
    for (int i = 0; i < kLodStepCount; ++i) {
        CHECK(20 % kLodSteps[i] == 0);
        if (i > 0) CHECK(kLodSteps[i] > kLodSteps[i - 1]);
    }
    int nonNested = 0;
    for (int i = 0; i + 1 < kLodStepCount; ++i)
        if (kLodSteps[i + 1] % kLodSteps[i] != 0) ++nonNested;
    CHECK(nonNested == 1);
}

TEST_CASE("terrain stitch: every stride pair draws an identical shared edge from both sides") {
    for (int ext : {20, 10}) {
        for (int ia = 0; ia < kLodStepCount; ++ia) {
            for (int ib = 0; ib < kLodStepCount; ++ib) {
                const int a = kLodSteps[ia], b = kLodSteps[ib];
                if (a > ext || b > ext) continue;
                std::vector<int> pa, pb;
                std::vector<float> ha, hb;
                drawnEdge(a, stitchStride(a, b, ext), ext, pa, ha);
                drawnEdge(b, stitchStride(b, a, ext), ext, pb, hb);
                for (int x = 0; x <= ext; ++x)
                    CHECK_MESSAGE(std::fabs(polylineAt(pa, ha, x) - polylineAt(pb, hb, x)) < 1e-3f,
                                  "strides " << a << " vs " << b << " ext " << ext << " differ at " << x);
            }
        }
    }
}

TEST_CASE("terrain stitch: the old coarser-stride-only rule cracks 4 vs 5 (regression anchor)") {
    std::vector<int> pa, pb;
    std::vector<float> ha, hb;
    drawnEdge(4, 5, 20, pa, ha);  // fine side snapped to the coarser stride only
    drawnEdge(5, 0, 20, pb, hb);  // coarse side untouched
    float worst = 0.0f;
    for (int x = 0; x <= 20; ++x)
        worst = std::fmax(worst, std::fabs(polylineAt(pa, ha, x) - polylineAt(pb, hb, x)));
    CHECK(worst > 1.0f);
}

TEST_CASE("terrain stitch: nested pairs keep the old behaviour, equal strides never snap") {
    CHECK(stitchStride(1, 2, 20) == 2);
    CHECK(stitchStride(2, 4, 20) == 4);
    CHECK(stitchStride(5, 10, 20) == 10);
    CHECK(stitchStride(10, 5, 20) == 0);   // the coarser block is untouched
    CHECK(stitchStride(4, 4, 20) == 0);
    CHECK(stitchStride(4, 5, 20) == 20);
    CHECK(stitchStride(5, 4, 20) == 20);
    CHECK(stitchStride(4, 20, 20) == 20);
}

TEST_CASE("terrain edge samples: multiples of the stride plus the endpoint") {
    int s[64];
    CHECK(edgeSamples(5, 20, s, 64) == 5);   // 0 5 10 15 20
    CHECK(s[4] == 20);
    CHECK(edgeSamples(5, 13, s, 64) == 4);   // 0 5 10 13
    CHECK(s[3] == 13);
}

TEST_CASE("terrain cull bounds: real per-block range includes the baked heights and the margin") {
    const int fineSide = 60 * 4 + 1;   // a 3x3 block map: 61 vertices a side
    std::vector<float> vis(static_cast<size_t>(fineSide) * fineSide, 100.0f);
    auto at = [&](int fx, int fy) -> float& { return vis[static_cast<size_t>(fx) + static_cast<size_t>(fy) * fineSide]; };
    at(4 * 40, 4 * 20) = 900.0f;    // far corner of block (1,0): fine (160, 80)
    at(4 * 40 + 1, 4 * 20) = 5000.0f;  // one fine sample outside it (block (2,0)'s interior)
    at(4 * 20 + 3, 3) = -50.0f;  // interior of block (1,0)

    ZRange r = widenByVisualFootprint({100.0f, 120.0f}, vis.data(), fineSide, 20, 0, 20, 20);
    CHECK(r.hi == 900.0f);
    CHECK(r.lo == -50.0f);

    ZRange e = widenByVisualFootprint({0.0f, 1.0f}, vis.data(), fineSide, 40, 40, 19, 19);  // map edge block: clamped, in range
    CHECK(e.hi >= 100.0f);

    const ZRange z = blockZRange(r.lo, r.hi, 300.0f);
    CHECK(z.lo == -350.0f);
    CHECK(z.hi == 1200.0f);
    CHECK(widenByVisualFootprint({1.0f, 2.0f}, nullptr, 0, 0, 0, 20, 20).hi == 2.0f);
}
