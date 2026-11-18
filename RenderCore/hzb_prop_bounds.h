// Conservative static-prop bounds for the Vulkan HZB color-pass consumer.
#pragma once
#include "gpu_abi/GpuStaticPropInstance.h"
#include "axis_convert.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <span>

namespace mc2hzb {
// Small-instance rejection (mirrors hzb_cull.comp): the unwidened screen box is snapped outward to a 1/256 px grid; a box that covers
// no pixel centre (centres at k+0.5, half-open [lo,hi)) in either axis cannot rasterize a fragment and is culled without a pyramid
// fetch. A sub-pixel box that does straddle a centre is kept (it still draws), so far props no longer blink on the 1 px threshold.
constexpr float kSnapGrid = 256.0f;
inline bool covers_pixel_centre(float lo, float hi) {
    return std::ceil(lo - 0.5f) + 0.5f < hi;
}
inline bool tiny_box(float x0, float y0, float x1, float y1) {
    const float sx0 = std::floor(x0 * kSnapGrid) / kSnapGrid, sx1 = std::ceil(x1 * kSnapGrid) / kSnapGrid;
    const float sy0 = std::floor(y0 * kSnapGrid) / kSnapGrid, sy1 = std::ceil(y1 * kSnapGrid) / kSnapGrid;
    return sx1 - sx0 <= 0.0f || sy1 - sy0 <= 0.0f || !covers_pixel_centre(sx0, sx1) || !covers_pixel_centre(sy0, sy1);
}
struct PropBounds { float lo[3], hi[3]; bool valid = false; };
struct alignas(16) ProjectedProp { float pixels[4]; float nearest; uint32_t lod, keep, tiny; };
static_assert(sizeof(ProjectedProp) == 32);
static_assert(offsetof(ProjectedProp, nearest) == 16 && offsetof(ProjectedProp, lod) == 20 && offsetof(ProjectedProp, keep) == 24);

inline ProjectedProp project_prop(const GpuStaticPropInstance& inst, const float* worldToClip, float depthBias,
                           std::span<const PropBounds> bounds, int w, int h, int levels) {
    ProjectedProp out{};
    out.keep = 1;
    if (inst.typeID >= bounds.size() || !bounds[inst.typeID].valid) return out;
    const auto& b = bounds[inst.typeID];
    float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX, nearest = 0;
    float viewLo[2] = {FLT_MAX, FLT_MAX}, viewHi[2] = {-FLT_MAX, -FLT_MAX}, swayPad[4] = {};
    bool occlusionSafe = true;
    bool projectable = true;
    float maxClipW = -FLT_MAX, maxClipZ = -FLT_MAX, maxClipFar = -FLT_MAX;
    float maxSide[4] = {-FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX}; // w+x, w-x, w+y, w-y, padded (mirrors hzb_cull.comp)
    if ((inst.flags & 4u) != 0u)
        for (int a = 0; a < 4; ++a) swayPad[a] = 1.5f * (std::abs(worldToClip[a]) + 0.6f * std::abs(worldToClip[4+a]));
    for (int corner = 0; corner < 8; ++corner) {
        const float p[4] = {corner & 1 ? b.hi[0] : b.lo[0], corner & 2 ? b.hi[1] : b.lo[1], corner & 4 ? b.hi[2] : b.lo[2], 1};
        float stuff[3] = {};
        for (int a = 0; a < 3; ++a)
            for (int k = 0; k < 4; ++k) stuff[a] += p[k] * inst.modelMatrix[a*4+k];
        struct AxisPoint { float x, y, z; };
        const auto render = mc2ToRender(AxisPoint{stuff[0], stuff[1], stuff[2]});
        const float world[4] = {render.x, render.y, render.z, 1};
        float clip[4] = {};
        for (int a = 0; a < 4; ++a)
            for (int k = 0; k < 4; ++k) clip[a] += worldToClip[k*4+a] * world[k];
        for (float v : clip) if (!std::isfinite(v)) return out;
        const float biasedZ = clip[2] + depthBias * clip[3];
        const float padZ = swayPad[2] + std::abs(depthBias) * swayPad[3];
        maxClipW = (std::max)(maxClipW, clip[3] + swayPad[3]);
        maxClipZ = (std::max)(maxClipZ, biasedZ + padZ);
        maxClipFar = (std::max)(maxClipFar, clip[3] - biasedZ + swayPad[3] + padZ);
        {   // side planes before the near-plane keep (mirrors hzb_cull.comp): all corners outside one side plane => cannot rasterize
            const float pw = std::abs(clip[3]) + swayPad[3];
            const float padX = swayPad[0] + pw * 4.0f / w + swayPad[3], padY = swayPad[1] + pw * 4.0f / h + swayPad[3];
            maxSide[0] = (std::max)(maxSide[0], clip[3] + clip[0] + padX); maxSide[1] = (std::max)(maxSide[1], clip[3] - clip[0] + padX);
            maxSide[2] = (std::max)(maxSide[2], clip[3] + clip[1] + padY); maxSide[3] = (std::max)(maxSide[3], clip[3] - clip[1] + padY);
        }
        if (!(clip[3] > 1e-5f)) { projectable = false; continue; }
        const float x = clip[0]/clip[3], y = clip[1]/clip[3], z = clip[2]/clip[3] + depthBias;
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || z < 0 || z >= 1) { projectable = false; continue; }
        if (clip[3] > swayPad[3] + 1e-5f) {
            const float marginX = (swayPad[0] + std::abs(x) * swayPad[3]) / (clip[3] - swayPad[3]) + 4.0f/w;
            const float marginY = (swayPad[1] + std::abs(y) * swayPad[3]) / (clip[3] - swayPad[3]) + 4.0f/h;
            viewLo[0] = (std::min)(viewLo[0], x-marginX); viewHi[0] = (std::max)(viewHi[0], x+marginX);
            viewLo[1] = (std::min)(viewLo[1], y-marginY); viewHi[1] = (std::max)(viewHi[1], y+marginY);
        } else { viewLo[0] = viewLo[1] = -FLT_MAX; viewHi[0] = viewHi[1] = FLT_MAX; }
        if (x <= -1 || x >= 1 || y <= -1 || y >= 1) occlusionSafe = false;
        x0 = (std::min)(x0, (x+1)*0.5f*w); x1 = (std::max)(x1, (x+1)*0.5f*w);
        y0 = (std::min)(y0, (y+1)*0.5f*h); y1 = (std::max)(y1, (y+1)*0.5f*h);
        nearest = (std::max)(nearest, z);
    }
    if (maxClipW < -1e-5f || maxClipZ < -1e-5f || maxClipFar < -1e-5f ||
        maxSide[0] < -1e-5f || maxSide[1] < -1e-5f || maxSide[2] < -1e-5f || maxSide[3] < -1e-5f) { out.keep = 0; out.tiny = 1; return out; }
    if (!projectable) return out;
    if (viewHi[0] < -1 || viewHi[1] < -1 || viewLo[0] > 1 || viewLo[1] > 1) { out.keep = 0; out.tiny = 1; return out; }
    if (!occlusionSafe) return out;
    out.tiny = tiny_box(x0, y0, x1, y1) ? 1u : 0u;
    // +-0.5 px: the pyramid depth was drawn with sub-pixel TAA jitter (mirrors hzb_cull.comp).
    out.pixels[0] = (std::max)(0.0f, std::floor(x0-0.5f)-1); out.pixels[1] = (std::max)(0.0f, std::floor(y0-0.5f)-1);
    out.pixels[2] = (std::min)(static_cast<float>(w-1), std::ceil(x1+0.5f)+1); out.pixels[3] = (std::min)(static_cast<float>(h-1), std::ceil(y1+0.5f)+1);
    const float extent = (std::max)(out.pixels[2]-out.pixels[0], out.pixels[3]-out.pixels[1]);
    while (out.lod+1 < static_cast<uint32_t>(levels) && static_cast<float>(1u << out.lod) < extent) ++out.lod;
    out.nearest = nearest;
    out.keep = 0;
    return out;
}

// CPU reference of the per-instance mip test in shaders/vulkan/hzb_cull.comp (same constants, same clamps).
// `fetch(level, x, y)` returns the reverse-Z HZB texel (a MIN reduction: the farthest occluder in the footprint).
// Level n is the n-th halving (rounded up) of baseW x baseH. Returns true when the prop is occluded (GPU keep bit 0).
template <class Fetch>
inline bool hzb_hidden(const ProjectedProp& p, int baseW, int baseH, Fetch&& fetch) {
    if (p.keep != 0) return false;
    if (p.tiny != 0) return true;
    int lw = baseW, lh = baseH;
    for (uint32_t l = 0; l < p.lod; ++l) { lw = (std::max)(1, (lw + 1) / 2); lh = (std::max)(1, (lh + 1) / 2); }
    const float scale = static_cast<float>(1u << p.lod);
    const int x0 = (std::clamp)(static_cast<int>(std::floor(p.pixels[0] / scale)), 0, lw - 1);
    const int y0 = (std::clamp)(static_cast<int>(std::floor(p.pixels[1] / scale)), 0, lh - 1);
    const int x1 = (std::clamp)(static_cast<int>(std::floor(p.pixels[2] / scale)), 0, lw - 1);
    const int y1 = (std::clamp)(static_cast<int>(std::floor(p.pixels[3] / scale)), 0, lh - 1);
    float farthest = 1.0f;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) farthest = (std::min)(farthest, fetch(static_cast<int>(p.lod), x, y));
    return p.nearest < farthest - 0.00002f;
}

} // namespace mc2hzb
