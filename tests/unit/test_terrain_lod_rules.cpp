#include "doctest.h"
#include "terrain_lod_rules.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace mc2lod;

TEST_CASE("terrain_activity_rows/equivalent") {
    for (const auto grid : {std::array<long, 3>{120, 20, 6}, {100, 20, 6}, {7, 3, 3}}) {
        const long side = grid[0], span = grid[1], blocks = grid[2];
        for (long b = 0; b < blocks * blocks; ++b) {
            std::vector<long> actual, expected;
            forEachBlockVertexRow(b, blocks, span, side, [&](long first, long count) {
                for (long vn = first; vn < first + count; ++vn) actual.push_back(vn);
            });
            const long rowStart = (b / blocks) * span, colStart = (b % blocks) * span;
            for (long row = rowStart; row < rowStart + span && row < side; ++row)
            for (long col = colStart; col < colStart + span && col < side; ++col) {
                const long vn = row * side + col;
                if (vn < 0 || vn >= side * side) continue;
                expected.push_back(vn);
            }
            CHECK(actual == expected);
            for (long vn : actual) { CHECK(vn >= 0); CHECK(vn < side * side); }
        }
    }
}

TEST_CASE("terrain_activity_rows/clipped") {
    long calls = 0;
    forEachBlockVertexRow(5, 6, 20, 100, [&](long, long) { ++calls; });
    forEachBlockVertexRow(30, 6, 20, 100, [&](long, long) { ++calls; });
    CHECK(calls == 0);
    std::vector<long> firsts, counts;
    forEachBlockVertexRow(5, 3, 3, 7, [&](long first, long count) {
        firsts.push_back(first); counts.push_back(count);
    });
    CHECK(firsts == std::vector<long>{27, 34, 41});
    CHECK(counts == std::vector<long>{1, 1, 1});
}

TEST_CASE("terrain activity cache: sub-block motion, rotation, clip and grid changes invalidate") {
    const ActivityKey key{{100, 200, 300, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0.75f},
                          {900, 30, 20, 300}, 128.0f};
    CHECK(key == key);
    for (size_t i = 0; i < key.camera.size(); ++i) {
        auto changed = key;
        changed.camera[i] = std::nextafter(changed.camera[i], 1000.0f);
        CHECK_FALSE(key == changed);
    }
    for (size_t i = 0; i < key.grid.size(); ++i) {
        auto changed = key;
        ++changed.grid[i];
        CHECK_FALSE(key == changed);
    }
    auto changed = key;
    changed.vertexScale *= 2;
    CHECK_FALSE(key == changed);
    changed = key;
    changed.camera[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(changed == changed); // A non-finite camera must never reuse a mask.
}

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

// ---------------------------------------------------------------------------------------------------------------------------
// terrain-sse-lod-2: screen-space-error chooser, geomorph and the shader-equivalent shared-edge proof.
// ---------------------------------------------------------------------------------------------------------------------------
namespace {

// Literal C++ transcription of the mode-0 vertex height in shaders/vulkan/terrain_tier1.vert (stitch block, then
// terrainMorphH from include/terrain_hires_near_vs.glsl). Same edge priority (N, S, W, E), same partial-edge clamp, same
// morph word decode. H is the global coarse height field (sampleH).
struct ShaderBlock { int ox, oy, qx, qy; uint32_t stitch; uint32_t morph; };

template <class H>
float shaderVertexH(const ShaderBlock& b, int lx, int ly, H sampleH) {
    float h = sampleH(b.ox + lx, b.oy + ly);
    if (b.stitch != 0) {
        int Sc = 0, along = 0;
        bool alongX = true;
        if      (ly == 0    && ((b.stitch)       & 0xFF) > 0) { Sc = (b.stitch)       & 0xFF; along = lx; alongX = true;  }
        else if (ly == b.qy && ((b.stitch >> 8)  & 0xFF) > 0) { Sc = (b.stitch >> 8)  & 0xFF; along = lx; alongX = true;  }
        else if (lx == 0    && ((b.stitch >> 16) & 0xFF) > 0) { Sc = (b.stitch >> 16) & 0xFF; along = ly; alongX = false; }
        else if (lx == b.qx && ((b.stitch >> 24) & 0xFF) > 0) { Sc = (b.stitch >> 24) & 0xFF; along = ly; alongX = false; }
        if (Sc > 0 && (along % Sc) != 0) {
            const int c0 = (along / Sc) * Sc;
            const int c1 = std::min(c0 + Sc, alongX ? b.qx : b.qy);
            const float t = float(along - c0) / float(c1 - c0);
            const float h0 = alongX ? sampleH(b.ox + c0, b.oy + ly) : sampleH(b.ox + lx, b.oy + c0);
            const float h1 = alongX ? sampleH(b.ox + c1, b.oy + ly) : sampleH(b.ox + lx, b.oy + c1);
            h = h0 + (h1 - h0) * t;
        }
    }
    const int T = static_cast<int>((b.morph >> 24) & 0xFF);
    if (T > 0) {
        const float u = float(lx * 4) / float(b.qx * 4), v = float(ly * 4) / float(b.qy * 4);
        const float nw = float(b.morph & 63), ne = float((b.morph >> 6) & 63);
        const float sw = float((b.morph >> 12) & 63), se = float((b.morph >> 18) & 63);
        const float top = nw + (ne - nw) * u, bot = sw + (se - sw) * u;
        const float w = (top + (bot - top) * v) / 63.0f;
        if (w > 0.0f) {
            const float p = parentSurfaceHeight(lx, ly, b.qx, b.qy, T,
                                                [&](int x, int y) { return sampleH(b.ox + x, b.oy + y); });
            h = h + (p - h) * w;
        }
    }
    return h;
}

unsigned lcg(unsigned& s) { s = s * 1664525u + 1013904223u; return s >> 8; }

}  // namespace

TEST_CASE("terrain stitch: production shader partial-edge rule equals mc2lod::edgeHeight") {
    // Astra finding: a 10-quad partial edge with strides 2|4 used to interpolate its last vertex past the edge end.
    auto H = [](int x, int y) { return edgeField(x * 131 + y * 7); };
    for (int ext : {20, 13, 10, 7, 3}) for (int Sc : {2, 4, 5, 10, 20}) {
        const int snap = std::min(Sc, ext);
        const ShaderBlock b{0, 0, ext, ext, static_cast<uint32_t>(snap), 0u};   // N edge stitched
        for (int x = 0; x <= ext; ++x)
            CHECK(std::fabs(shaderVertexH(b, x, 0, H) - edgeHeight(x, 1, snap, ext, [&](int a) { return H(a, 0); })) < 1e-4f);
    }
}

TEST_CASE("terrain SSE: shared edges stay identical through the whole morph, all orientations, partial tiles, any state") {
    // A 3x3 grid of blocks (last row/column partial), random levels, visibility and morph weights per trial (a trial is a
    // camera cut: no state carries over). Words are built exactly as terrain_update.cpp emits them; every shared edge between
    // two drawn blocks is compared at every coarse position from both sides, with the morph swept 0..1.
    unsigned seed = 12345u;
    int edgesChecked = 0, morphedEdges = 0, failures = 0;
    std::string firstFailure;
    for (int partial : {20, 13, 10, 7}) {
        const int qc[3] = {20, 20, partial};
        const int org[3] = {0, 20, 40};
        auto H = [&](int x, int y) {
            return edgeField(std::clamp(x, 0, 40 + partial) * 977 + std::clamp(y, 0, 40 + partial));
        };
        for (int trial = 0; trial < 300; ++trial) {
            int level[9]; bool vis[9]; float chosen[9];
            for (int i = 0; i < 9; ++i) {
                level[i] = static_cast<int>(lcg(seed) % kLodStepCount);
                if (trial % 3 == 0) level[i] = 2 + static_cast<int>(lcg(seed) % 2);   // concentrate on the 4|5 transition
                if (trial % 3 == 1) level[i] = 2;                                      // same level: morphing edges
                vis[i] = (lcg(seed) % 8) != 0;
                chosen[i] = float(lcg(seed) % 1000) / 999.0f;
            }
            for (float sweep : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
                ShaderBlock blk[9];
                for (int by = 0; by < 3; ++by) for (int bx = 0; bx < 3; ++bx) {
                    const int i = bx + by * 3;
                    uint32_t stitch = 0;
                    const int edx[4] = {0, 0, -1, 1}, edy[4] = {-1, 1, 0, 0};
                    for (int e = 0; e < 4; ++e) {
                        const int nx = bx + edx[e], ny = by + edy[e];
                        if (nx < 0 || ny < 0 || nx > 2 || ny > 2 || !vis[nx + ny * 3]) continue;
                        stitch |= static_cast<uint32_t>(stitchStride(kLodSteps[level[i]], kLodSteps[level[nx + ny * 3]],
                                                                     (e < 2) ? qc[bx] : qc[by]) & 0xFF) << (e * 8);
                    }
                    uint32_t morph = 0;
                    if (level[i] + 1 < kLodStepCount) {
                        float corner[4];
                        const int cdx[4] = {0, 1, 0, 1}, cdy[4] = {0, 0, 1, 1};
                        for (int c = 0; c < 4; ++c) {
                            CornerBlock tb[4];
                            for (int t = 0; t < 4; ++t) {
                                const int tx = bx + cdx[c] - 1 + (t & 1), ty = by + cdy[c] - 1 + (t >> 1);
                                const bool on = tx >= 0 && ty >= 0 && tx < 3 && ty < 3;
                                const int j = on ? tx + ty * 3 : 0;
                                tb[t] = {on, on && vis[j], on ? level[j] : 0, on ? chosen[j] * sweep : 0.0f};
                            }
                            corner[c] = cornerMorph(tb, level[i]);
                        }
                        if (corner[0] > 0 || corner[1] > 0 || corner[2] > 0 || corner[3] > 0)
                            morph = packMorph(corner, kLodSteps[level[i] + 1]);
                    }
                    blk[i] = {org[bx], org[by], qc[bx], qc[by], stitch, morph};
                }
                // Drawn edge of block i along edge e (N,S,W,E): its mesh vertices (edgeSamples) through the shader height.
                auto edgeLine = [&](int i, int e, std::vector<int>& pos, std::vector<float>& hgt) {
                    const ShaderBlock& b = blk[i];
                    const int ext = (e < 2) ? b.qx : b.qy;
                    int s[64];
                    const int n = edgeSamples(kLodSteps[level[i]], ext, s, 64);
                    pos.assign(s, s + n);
                    hgt.clear();
                    for (int k = 0; k < n; ++k) {
                        const int lx = (e < 2) ? s[k] : (e == 2 ? 0 : b.qx), ly = (e < 2) ? (e == 0 ? 0 : b.qy) : s[k];
                        hgt.push_back(shaderVertexH(b, lx, ly, H));
                    }
                };
                for (int by = 0; by < 3; ++by) for (int bx = 0; bx < 3; ++bx) {
                    const int i = bx + by * 3;
                    if (!vis[i]) continue;
                    for (int dir = 0; dir < 2; ++dir) {   // E neighbour (A:E vs B:W), S neighbour (A:S vs B:N)
                        const int nx = bx + (dir == 0 ? 1 : 0), ny = by + (dir == 1 ? 1 : 0);
                        if (nx > 2 || ny > 2 || !vis[nx + ny * 3]) continue;
                        const int j = nx + ny * 3;
                        std::vector<int> pa, pb; std::vector<float> ha, hb;
                        edgeLine(i, dir == 0 ? 3 : 1, pa, ha);
                        edgeLine(j, dir == 0 ? 2 : 0, pb, hb);
                        const int ext = dir == 0 ? qc[by] : qc[bx];
                        ++edgesChecked;
                        if (blk[i].morph != 0 && blk[j].morph != 0) ++morphedEdges;
                        for (int x = 0; x <= ext; ++x) {
                            const float d = std::fabs(polylineAt(pa, ha, x) - polylineAt(pb, hb, x));
                            if (!(d < 1e-3f)) {
                                if (failures++ == 0)
                                    firstFailure = "levels " + std::to_string(level[i]) + "|" + std::to_string(level[j]) +
                                                   " dir " + std::to_string(dir) + " partial " + std::to_string(partial) +
                                                   " sweep " + std::to_string(sweep) + " at " + std::to_string(x);
                            }
                        }
                    }
                }
            }
        }
    }
    CHECK_MESSAGE(failures == 0, firstFailure);
    CHECK(edgesChecked > 10000);
    CHECK(morphedEdges > 1000);   // the sweep really exercised morphing shared edges
}

TEST_CASE("terrain SSE: level errors are zero on a plane, monotone, and see the 4x bake") {
    std::vector<float> plane(41 * 41), bump(41 * 41, 0.0f), fine(81 * 81, 0.0f);
    for (int y = 0; y < 41; ++y) for (int x = 0; x < 41; ++x) plane[x + y * 41] = 3.0f * x - 2.0f * y + 7.0f;
    const LevelErrors ep = levelErrors(plane.data(), 41, 1, 0, 0, 20, 20);
    for (float e : ep) CHECK(e < 1e-3f);
    bump[7 + 7 * 41] = 50.0f;   // odd vertex: dropped from stride 2 up
    const LevelErrors eb = levelErrors(bump.data(), 41, 1, 0, 0, 20, 20);
    CHECK(eb[0] == 0.0f);
    CHECK(eb[1] == doctest::Approx(50.0f));
    for (int k = 1; k < kLodStepCount; ++k) CHECK(eb[k] >= eb[k - 1]);
    fine[(7 * 4 + 2) + (7 * 4 + 2) * 81] = 9.0f;   // a bake-only feature between coarse vertices
    const LevelErrors ef = levelErrors(fine.data(), 81, 4, 0, 0, 20, 20);
    CHECK(ef[1] == doctest::Approx(9.0f));
    const EdgeErrors ee = edgeErrors(bump.data(), 41, 1, 0, 0, 20, 20);
    for (int e = 0; e < 4; ++e) CHECK(ee[e][5] == 0.0f);   // the bump is interior: edges are exact
    // Partial block (7 quads) at the far edge of the plane is still exact.
    const LevelErrors epp = levelErrors(plane.data(), 41, 1, 33, 33, 7, 7);
    for (float e : epp) CHECK(e < 1e-3f);
}

TEST_CASE("terrain SSE: chooser goes coarser with distance, has hysteresis, and keeps morph error in the bound") {
    const LevelErrors e{0.0f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f};
    const float num = 1000.0f;
    int prev = 0;
    for (float w = 50.0f; w < 40000.0f; w *= 1.05f) {   // receding block
        const LodPick p = chooseSseLevel(e, prev, 1.0f, num, w, 0.0f);
        CHECK(p.level >= prev);
        CHECK(projectedErrorPx(e[p.level], num, w, 0.0f) <= 1.0f + 1e-5f);
        if (p.level + 1 < kLodStepCount)   // drawn mix(L, L+1, m) stays within tolerance
            CHECK((1.0f - p.morph) * projectedErrorPx(e[p.level], num, w, 0.0f)
                  + p.morph * projectedErrorPx(e[p.level + 1], num, w, 0.0f) <= 1.0f + 1e-4f);
        // A one-level coarsening happens only after the old level was fully morphed onto the new one (no pop).
        if (p.level == prev + 1) CHECK(chooseSseLevel(e, prev, 1.0f, num, w / 1.05f, 0.0f).morph > 0.99f);
        prev = p.level;
    }
    CHECK(prev == kLodStepCount - 1);
    // Hysteresis: in the band 0.9..1.0 x tolerance the level depends on where it came from.
    const float wBand = std::sqrt(num * e[2] / 0.95f);   // projected error = e * num / w^2 -> level 2 at 0.95 px
    CHECK(chooseSseLevel(e, 2, 1.0f, num, wBand, 0.0f).level == 2);
    CHECK(chooseSseLevel(e, 1, 1.0f, num, wBand, 0.0f).level == 1);
    // Camera plane inside the error bound: unbounded projection -> finest.
    CHECK(chooseSseLevel(e, 3, 1.0f, num, 1.0f, 10.0f).level == 0);
}

TEST_CASE("terrain SSE: corner morph, packing and the stitch error level") {
    const CornerBlock same[4] = {{true, true, 2, 0.8f}, {true, true, 2, 0.5f}, {true, true, 2, 1.0f}, {true, true, 2, 0.9f}};
    CHECK(cornerMorph(same, 2) == 0.5f);
    CornerBlock other[4] = {same[0], same[1], same[2], {true, true, 3, 1.0f}};
    CHECK(cornerMorph(other, 2) == 0.0f);
    other[3] = {true, false, 2, 1.0f};
    CHECK(cornerMorph(other, 2) == 0.0f);
    other[3] = {false, false, 0, 0.0f};
    CHECK(cornerMorph(other, 2) == 0.0f);
    const float c[4] = {0.0f, 1.0f, 0.5f, 1.0f};
    const uint32_t w = packMorph(c, 5);
    CHECK((w & 63u) == 0u);
    CHECK(((w >> 6) & 63u) == 63u);
    CHECK(((w >> 12) & 63u) == 32u);
    CHECK(((w >> 18) & 63u) == 63u);
    CHECK((w >> 24) == 5u);
    CHECK(!stitchErrorLevel(2, 2, 20).has_value());
    CHECK(stitchErrorLevel(0, 2, 20) == 2);   // 1|4 nested: the finer block snaps to stride 4, the coarser level
    CHECK(stitchErrorLevel(2, 3, 20) == 5);   // 4|5 -> drawn at stride 20
    CHECK(stitchErrorLevel(3, 4, 20) == 4);   // 5|10 -> 10
    CHECK(stitchErrorLevel(1, 2, 20) == 2);   // 2|4 -> the finer block's edge is at stride 4
    CHECK(stitchErrorLevel(0, 1, 20) == 1);
    CHECK(stitchErrorLevel(2, 3, 7) == 4);    // capped at a 7-quad edge: drawn endpoints only, like the stride-10 level
}
