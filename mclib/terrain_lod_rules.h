// Pure terrain LOD rules shared by terrain_update.cpp and tests/unit/test_terrain_lod_rules.cpp. No engine includes.
//
// Stitch rule: LOD_STEPS {1,2,4,5,10,20} are not nested (4 and 5), so "snap the finer edge to the coarser stride" leaves a crack
// when the two strides do not divide. Both blocks of an adjacent pair therefore snap their shared edge to the same stride,
// lcm(a, b) (capped at the edge length): the shader lerps every edge vertex between multiples of that stride, so both sides draw
// the identical polyline. Nested pairs reduce to the old rule (the finer block snaps to the coarser stride, the coarser block is
// untouched).
#pragma once
#include <algorithm>
#include <array>
#include <span>
#include <optional>
#include <cmath>
#include <cstdint>
#include <limits>

namespace mc2lod {

// Visit the contiguous vertex rows of a block, clipped to the map.
template <typename Fn>
void forEachBlockVertexRow(long block, long blocksMapSide, long verticesBlockSide,
                           long verticesMapSide, Fn fn)
{
    const long rowStart = (block / blocksMapSide) * verticesBlockSide;
    const long colStart = (block % blocksMapSide) * verticesBlockSide;
    const long count = std::min(verticesBlockSide, verticesMapSide - colStart);
    if (count <= 0) return;
    for (long row = rowStart; row < rowStart + verticesBlockSide && row < verticesMapSide; ++row)
        fn(row * verticesMapSide + colStart, count);
}

// Exact inputs of the CPU angular activity mask. Heights, recipes and object
// membership are not inputs; those consumers still run against live data.
struct ActivityKey {
    std::array<float, 13> camera; // position, frame i/j/k, horizontal clip
    std::array<long, 4> grid;     // block count/side, vertices/block, half map
    float vertexScale;
    bool operator==(const ActivityKey& other) const {
        return camera == other.camera && grid == other.grid && vertexScale == other.vertexScale;
    }
};

constexpr int kLodStepCount = 6;
constexpr int kLodSteps[kLodStepCount] = {1, 2, 4, 5, 10, 20};

constexpr int gcdInt(int a, int b) { return b == 0 ? a : gcdInt(b, a % b); }
constexpr int lcmInt(int a, int b) { return (a / gcdInt(a, b)) * b; }

// Stride the shared edge of two blocks (strides ownStep, otherStep; edge length ext quads) is drawn at. 0 means "no snap".
constexpr int stitchStride(int ownStep, int otherStep, int ext)
{
    if (ownStep <= 0 || otherStep <= 0 || ownStep == otherStep)
        return 0;
    int s = lcmInt(ownStep, otherStep);
    if (ext > 0 && s > ext)
        s = ext;
    return (s == ownStep) ? 0 : s;  // own vertices already sit on the shared stride
}

// Sample positions along an edge of ext quads at stride step (multiples of step plus the endpoint), as getOrBuildPatch emits.
inline int edgeSamples(int step, int ext, int* out, int cap)
{
    int n = 0;
    for (int q = 0; q < ext && n < cap; q += step)
        out[n++] = q;
    if (n < cap)
        out[n++] = ext;
    return n;
}

// The height the shader draws at edge position along for a block at stride ownStep whose edge is snapped to snap (0 = none).
// h is the height function along the edge (any callable int -> float).
template <class H>
inline float edgeHeight(int along, int ownStep, int snap, int ext, H h)
{
    (void)ownStep;
    if (snap > 0 && (along % snap) != 0) {
        const int c0 = (along / snap) * snap;
        const int c1 = std::min(c0 + snap, ext);
        const float t = static_cast<float>(along - c0) / static_cast<float>(c1 - c0);
        return h(c0) * (1.0f - t) + h(c1) * t;
    }
    return h(along);
}

// Conservative-but-real z range of a block: gameplay min/max elevation, widened by the baked visual displacement (the visual
// height is up to 4x sampling density but stays inside the same elevation range) and a margin for skirts (capped 256 wu) and
// water/decal lift.
struct ZRange { float lo, hi; };
inline ZRange blockZRange(float minElev, float maxElev, float visualMargin)
{
    return { minElev - visualMargin, maxElev + visualMargin };
}

// Widen [lo, hi] by every baked visual height over a block's fine footprint (fine = 4 * vertex index, inclusive of the far edge,
// clamped to the bake). vis is fineSide x fineSide.
inline ZRange widenByVisualFootprint(ZRange r, const float* vis, int fineSide, int originX, int originY, int qcX, int qcY)
{
    if (!vis || fineSide <= 1)
        return r;
    const int fx0 = std::clamp(originX * 4, 0, fineSide - 1), fx1 = std::clamp((originX + qcX) * 4, 0, fineSide - 1);
    const int fy0 = std::clamp(originY * 4, 0, fineSide - 1), fy1 = std::clamp((originY + qcY) * 4, 0, fineSide - 1);
    for (int fy = fy0; fy <= fy1; ++fy)
        for (int fx = fx0; fx <= fx1; ++fx) {
            const float v = vis[static_cast<size_t>(fx) + static_cast<size_t>(fy) * static_cast<size_t>(fineSide)];
            r.lo = std::min(r.lo, v);
            r.hi = std::max(r.hi, v);
        }
    return r;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Screen-space-error LOD (terrain-sse-lod-2). Each block carries, per level, the largest vertical distance between the level's
// drawn surface and the reference surface (the 4x visual bake when it is loaded, else the coarse height field). The chooser
// projects those errors with the live camera and picks the coarsest level under a pixel tolerance; a block morphs its vertices
// toward the next coarser level's surface over the band before that level takes over, so a switch moves no vertex.
// ---------------------------------------------------------------------------------------------------------------------------
constexpr float kSseTolerancePx = 1.0f;           // refine above this projected error
constexpr float kSseConcreteTolerancePx = 0.25f;  // runway/cement contact blocks
constexpr float kSseCoarsenRatio = 0.9f;          // hysteresis: coarsen only below 0.9 x tolerance

using LevelErrors = std::array<float, kLodStepCount>;

// The flat water plane turns a small vertical error into a large coastline shift on a gentle shore (owner 11-25, mc2_02 at high
// RTS: coarse levels lifted the lakebed above the water in stride-sized stair steps). A level that puts any reference sample on
// the other side of the water plane is never within tolerance. NaN waterElev = no water.
inline float waterCoverageError(float reference, float drawn, float waterElev, float verticalError)
{
    return (reference < waterElev) != (drawn < waterElev) ? std::numeric_limits<float>::infinity() : verticalError;
}

// Per-level error of one block. ref is refSide x refSide; refPerCoarse reference samples per coarse cell (4 = bake, 1 = coarse
// heights). Level k draws coarse vertices at stride kLodSteps[k] (plus the block's far edge) with the TL-BR diagonal, the mode
// 0/2 triangulation. Level 0 is the reference itself. The result is made non-decreasing so "coarser" never means "better".
inline LevelErrors levelErrors(const float* ref, int refSide, int refPerCoarse, int originX, int originY, int qcX, int qcY,
                               float waterElev = std::numeric_limits<float>::quiet_NaN())
{
    LevelErrors e{};
    if (!ref || refSide <= 1 || refPerCoarse <= 0 || qcX <= 0 || qcY <= 0)
        return e;
    const int r = refPerCoarse;
    auto at = [&](int fx, int fy) {  // block-local reference coords
        const int x = std::clamp(originX * r + fx, 0, refSide - 1), y = std::clamp(originY * r + fy, 0, refSide - 1);
        return ref[static_cast<size_t>(x) + static_cast<size_t>(y) * static_cast<size_t>(refSide)];
    };
    const int fxN = qcX * r, fyN = qcY * r;
    for (int k = 1; k < kLodStepCount; ++k) {
        const int s = kLodSteps[k];
        float err = 0.0f;
        for (int fy = 0; fy <= fyN; ++fy) {
            int y0 = (fy / (s * r)) * s;
            if (y0 >= qcY) y0 = ((qcY - 1) / s) * s;
            const int y1 = std::min(y0 + s, qcY);
            const float v = static_cast<float>(fy - y0 * r) / static_cast<float>((y1 - y0) * r);
            for (int fx = 0; fx <= fxN; ++fx) {
                int x0 = (fx / (s * r)) * s;
                if (x0 >= qcX) x0 = ((qcX - 1) / s) * s;
                const int x1 = std::min(x0 + s, qcX);
                const float u = static_cast<float>(fx - x0 * r) / static_cast<float>((x1 - x0) * r);
                const float tl = at(x0 * r, y0 * r), tr = at(x1 * r, y0 * r), bl = at(x0 * r, y1 * r), br = at(x1 * r, y1 * r);
                const float c = (u >= v) ? tl + u * (tr - tl) + v * (br - tr) : tl + v * (bl - tl) + u * (br - bl);
                err = std::max(err, waterCoverageError(at(fx, fy), c, waterElev, std::fabs(at(fx, fy) - c)));
            }
        }
        e[k] = std::max(err, e[k - 1]);
    }
    return e;
}

// Per-edge level errors of one block (edge order N, S, W, E, as the stitch word): the largest distance between the reference
// along the edge and the edge drawn at stride kLodSteps[k] (multiples of the stride plus the edge end, mc2lod::edgeHeight).
// A stitched edge is exactly such a polyline, so these bound the stitch error.
using EdgeErrors = std::array<LevelErrors, 4>;
inline EdgeErrors edgeErrors(const float* ref, int refSide, int refPerCoarse, int originX, int originY, int qcX, int qcY,
                             float waterElev = std::numeric_limits<float>::quiet_NaN())
{
    EdgeErrors out{};
    if (!ref || refSide <= 1 || refPerCoarse <= 0 || qcX <= 0 || qcY <= 0)
        return out;
    const int r = refPerCoarse;
    auto at = [&](int fx, int fy) {
        const int x = std::clamp(originX * r + fx, 0, refSide - 1), y = std::clamp(originY * r + fy, 0, refSide - 1);
        return ref[static_cast<size_t>(x) + static_cast<size_t>(y) * static_cast<size_t>(refSide)];
    };
    for (int edge = 0; edge < 4; ++edge) {
        const bool alongX = edge < 2;
        const int ext = alongX ? qcX : qcY;
        const int fixedF = (edge == 0 || edge == 2) ? 0 : (alongX ? qcY : qcX) * r;
        auto h = [&](int alongF) { return alongX ? at(alongF, fixedF) : at(fixedF, alongF); };
        for (int k = 1; k < kLodStepCount; ++k) {
            const int s = kLodSteps[k];
            float err = 0.0f;
            for (int f = 0; f <= ext * r; ++f) {
                int c0 = (f / (s * r)) * s;
                if (c0 >= ext) c0 = ((ext - 1) / s) * s;
                const int c1 = std::min(c0 + s, ext);
                const float t = static_cast<float>(f - c0 * r) / static_cast<float>((c1 - c0) * r);
                const float drawn = h(c0 * r) * (1.0f - t) + h(c1 * r) * t;
                err = std::max(err, waterCoverageError(h(f), drawn, waterElev, std::fabs(h(f) - drawn)));
            }
            out[edge][k] = std::max(err, out[edge][k - 1]);
        }
    }
    return out;
}

// Projected size in pixels of a vertical error e. numerator/w/absAw come from the camera (see terrain_update.cpp): the bound is
// e * numerator / (w * (w - e * absAw)), conservative over the block's box. Non-positive denominator = unbounded.
inline float projectedErrorPx(float e, float numerator, float w, float absAw)
{
    const float den = w * (w - e * absAw);
    if (!(den > 1e-8f) || !std::isfinite(e))
        return std::numeric_limits<float>::infinity();
    return e * numerator / den;
}

struct LodPick { int level; float morph; };

// Pick the coarsest level whose projected error is within tolerance (levels coarser than prevLevel must be within
// kSseCoarsenRatio x tolerance), and the morph weight toward level+1. Morph is 1 once level+1's error is within tolerance (the
// band where the block is about to coarsen, or has just refined), 0 by the error at which this level itself was entered, so
// both sides of a switch draw the same surface.
inline LodPick chooseSseLevel(const LevelErrors& e, int prevLevel, float tolPx, float numerator, float w, float absAw)
{
    int level = 0;
    for (int k = kLodStepCount - 1; k > 0; --k) {
        const float tol = (k <= prevLevel) ? tolPx : tolPx * kSseCoarsenRatio;
        if (projectedErrorPx(e[k], numerator, w, absAw) <= tol) { level = k; break; }
    }
    float morph = 0.0f;
    if (level + 1 < kLodStepCount) {
        const float p = projectedErrorPx(e[level + 1], numerator, w, absAw);
        float hi = 2.0f * tolPx;
        if (e[level] > 0.0f)
            hi = std::clamp(kSseCoarsenRatio * tolPx * e[level + 1] / e[level], tolPx * 1.001f, 2.0f * tolPx);
        morph = std::isfinite(p) ? std::clamp((hi - p) / (hi - tolPx), 0.0f, 1.0f) : 0.0f;
        // Morph error is in the bound: the drawn surface mix(L, L+1, m) is off the reference by at most (1-m)pL + m p, so m
        // is capped where that reaches the tolerance (it is 1 again once p <= tol, so both switch points are unchanged).
        const float pL = projectedErrorPx(e[level], numerator, w, absAw);
        if (p > tolPx && std::isfinite(p) && p > pL)
            morph = std::min(morph, std::clamp((tolPx - pL) / (p - pL), 0.0f, 1.0f));
    }
    return {level, morph};
}

// Stitch error is in the bound: two blocks at different levels draw their shared edge at stitchStride (the lcm, capped at the
// edge), which can be coarser than either level (4 and 5 -> 20). That edge equals the edge of the first level whose stride is
// at least the stitch stride, so its error is bounded by that level's block error. Returns that level, or -1 when the pair
// draws the edge at its own vertices (same level, or nested strides).
constexpr std::optional<int> stitchErrorLevel(int levelA, int levelB, int ext)
{
    if (levelA == levelB || levelA < 0 || levelB < 0 || levelA >= kLodStepCount || levelB >= kLodStepCount)
        return std::nullopt;
    const int a = kLodSteps[levelA], b = kLodSteps[levelB];
    int s = lcmInt(a, b);
    if (ext > 0 && s > ext)
        s = ext;
    if (s <= std::min(a, b))
        return std::nullopt;
    for (int k = 0; k < kLodStepCount; ++k)
        if (kLodSteps[k] >= s)
            return k;
    return kLodStepCount - 1;
}

// Morph weight at one block corner. touching[i] = the up-to-four blocks sharing the grid corner. Any of them off the map, culled
// or at another level pins the corner to 0 (that edge is stitched or skirted, and the skirt follows the unmorphed polyline);
// otherwise the corner takes the smallest morph of the blocks sharing it, so a shared edge
// interpolates the same two corner values from both sides.
struct CornerBlock { bool onMap; bool visible; int level; float morph; };
inline float cornerMorph(std::span<const CornerBlock> touching, int ownLevel)
{
    float m = 1.0f;
    for (const CornerBlock& b : touching) {
        if (!b.onMap || !b.visible || b.level != ownLevel) return 0.0f;
        m = std::min(m, b.morph);
    }
    return m;
}

// Packs four corner morphs (NW, NE, SW, SE; 6 bits each) and the morph target stride (the next coarser level's step, coarse
// units) into the patch record word the vertex shaders read.
inline uint32_t packMorph(const float corner[4], int targetStep)
{
    uint32_t w = 0;
    for (int i = 0; i < 4; ++i)
        w |= static_cast<uint32_t>(std::clamp(corner[i], 0.0f, 1.0f) * 63.0f + 0.5f) << (i * 6);
    return w | (static_cast<uint32_t>(targetStep & 0xFF) << 24);
}

// Height of the stride-T TL-BR surface at block-local coarse position (lx, ly); h(x, y) is the coarse vertex height in
// block-local coords. Mirrors terrainParentH in shaders/vulkan/include/terrain_hires_near_vs.glsl (fine units there).
template <class H>
inline float parentSurfaceHeight(int lx, int ly, int qcX, int qcY, int T, H h)
{
    int x0 = (lx / T) * T, y0 = (ly / T) * T;
    if (x0 >= qcX) x0 = ((qcX - 1) / T) * T;
    if (y0 >= qcY) y0 = ((qcY - 1) / T) * T;
    const int x1 = std::min(x0 + T, qcX);
    const int y1 = std::min(y0 + T, qcY);
    const float u = static_cast<float>(lx - x0) / static_cast<float>(x1 - x0), v = static_cast<float>(ly - y0) / static_cast<float>(y1 - y0);
    const float tl = h(x0, y0), tr = h(x1, y0), bl = h(x0, y1), br = h(x1, y1);
    return (u >= v) ? tl + u * (tr - tl) + v * (br - tr) : tl + v * (bl - tl) + u * (br - bl);
}

}  // namespace mc2lod
