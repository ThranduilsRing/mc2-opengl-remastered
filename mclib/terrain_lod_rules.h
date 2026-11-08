// Pure terrain LOD rules shared by terrain_update.cpp and tests/unit/test_terrain_lod_rules.cpp. No engine includes.
//
// Stitch rule: LOD_STEPS {1,2,4,5,10,20} are not nested (4 and 5), so "snap the finer edge to the coarser stride" leaves a crack
// when the two strides do not divide. Both blocks of an adjacent pair therefore snap their shared edge to the same stride,
// lcm(a, b) (capped at the edge length): the shader lerps every edge vertex between multiples of that stride, so both sides draw
// the identical polyline. Nested pairs reduce to the old rule (the finer block snaps to the coarser stride, the coarser block is
// untouched).
#pragma once
#include <algorithm>
#include <cstdint>

namespace mc2lod {

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

}  // namespace mc2lod
