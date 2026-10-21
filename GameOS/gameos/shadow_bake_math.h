// shadow_bake_math.h -- the geometry of the static shadow bake: the mask layout, the sun basis, the per-tile caster window and the caster reach.
//
// WHAT: pure CPU math (no Vulkan, no engine state) shared by the bake (vulkan_shadow_bake.cpp), its doctest and the later dirty-tile work. World is MC2
// (x east, y north, z up). The mask covers the terrain grid, vertex 0 to vertex side-1 on both axes; its row 0 is the north edge, as the terrain shader's
// grid coordinate g = ((x + halfMap) / 128, (halfMap - y) / 128) has it.
// WHY: the window and the reach decide which casters a tile sees and how much of a shadow fits; getting them wrong cuts shadows at tile seams. Kept here so
// a doctest pins them without a GPU.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mc2shadow {

struct Vec3 { float x = 0.0f, y = 0.0f, z = 0.0f; };

inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline Vec3 normalized(Vec3 v) {
    const float l = std::sqrt(dot(v, v));
    return l > 1e-12f ? Vec3{v.x / l, v.y / l, v.z / l} : Vec3{0.0f, 0.0f, 1.0f};
}

// The art constants of the bake (owner ruling D2, 2026-10-21): the sun's angular radius, which sets every penumbra, and the long-shadow fade.
inline constexpr float kSunAngularRadiusDeg = 1.5f;   // calibrated on the owner views: 2.0 washed the night tree streaks out
// The terrain's own shadow is a wide, low ridge, not a sharp caster: its cone is this many times the sun's (soft gradients, as pre-T15 had).
inline constexpr float kTerrainConeScale = 2.5f;
inline constexpr float kFadeStartWorld = 0.0f;   // the fade ramps from contact ...
inline constexpr float kFadeEndWorld = 240.0f;   // ... to full at this blocker distance along the ray (pre-T15 80; at the 15 deg night clamp a tree's
                                                 // canopy sits 150+ wu up its ray, and 80 or 120 erased the streaks pre-T15 still showed)
inline constexpr float kFadeMax = 0.65f;          // the pre-T15 MC2_SHADOW_DIST_FADE_MAX
inline constexpr float kWorldUnitsPerVertex = 128.0f;

// Tiling: each tile resolves 512^2 mask texels and computes an apron around them, so the blurred fade and the 3x3 resolve read real neighbours.
inline constexpr int kTileTexels = 512;
inline constexpr int kTileApron = 32;
inline constexpr int kRegionTexels = kTileTexels + 2 * kTileApron;
// The transient caster window: light-space texels of this size, at most this many on a side (the texel grows when a window would not fit).
inline constexpr float kWindowTexelWorld = 0.5f;
inline constexpr int kWindowMaxTexels = 4096;

// The mask: `texels` on a side, square, covering `span` world units from (originX, originYNorth) east and south.
struct MaskLayout {
    int texels = 0;
    float texelWorld = 0.0f;
    float originX = 0.0f;       // world x of the west edge
    float originYNorth = 0.0f;  // world y of the north edge
    float span = 0.0f;
    int tilesPerSide = 0;
};

// d = max(target, span / cap), with the texel count rounded up to whole tiles (the texel then shrinks a little). An empty layout for a map without terrain.
inline MaskLayout maskLayout(int mapSide, float halfMap, float targetTexelWorld, int capTexels) {
    MaskLayout m;
    if (mapSide < 2 || targetTexelWorld <= 0.0f || capTexels < kTileTexels) return m;
    m.span = static_cast<float>(mapSide - 1) * kWorldUnitsPerVertex;
    const float d = (std::max)(targetTexelWorld, m.span / static_cast<float>(capTexels));
    int n = static_cast<int>(std::ceil(m.span / d));
    n = ((n + kTileTexels - 1) / kTileTexels) * kTileTexels;
    n = (std::min)(n, (capTexels / kTileTexels) * kTileTexels);
    m.texels = n;
    m.tilesPerSide = n / kTileTexels;
    m.texelWorld = m.span / static_cast<float>(n);
    m.originX = -halfMap;
    m.originYNorth = halfMap;
    return m;
}

// The toward-sun direction with its altitude raised to at least `minAltDeg` (azimuth kept): the same clamp the shadow matrices take
// (mc2ClampShadowSunAltitude), written for the toward-sun vector. A zero or negative minimum leaves the sun as it is.
inline Vec3 clampSunAltitude(Vec3 towardSun, float minAltDeg) {
    Vec3 l = normalized(towardSun);
    if (minAltDeg <= 0.0f) return l;
    const float minUp = std::sin(minAltDeg * 0.01745329252f);
    if (l.z >= minUp) return l;
    const float horiz = std::sqrt(l.x * l.x + l.y * l.y);
    const float ch = std::sqrt((std::max)(1.0f - minUp * minUp, 0.0f));
    if (horiz > 1e-6f) return {l.x / horiz * ch, l.y / horiz * ch, minUp};
    return {ch, 0.0f, minUp};
}

// The light-space basis: `right` and `up` span the plane the window is rasterised on, `toward` points at the sun. Light depth d = dot(p, toward) grows
// toward the sun, so along one sun ray the caster has the larger d and d(caster) - d(receiver) is the distance between them.
struct LightBasis { Vec3 right, up, toward; };
inline LightBasis lightBasis(Vec3 towardSun) {
    LightBasis b;
    b.toward = normalized(towardSun);
    Vec3 hint = std::fabs(b.toward.z) > 0.9f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{0.0f, 0.0f, 1.0f};
    b.right = normalized(cross(hint, b.toward));
    b.up = cross(b.toward, b.right);
    return b;
}
inline Vec3 toLight(const LightBasis& b, Vec3 p) { return {dot(p, b.right), dot(p, b.up), dot(p, b.toward)}; }

// How far (horizontally) a caster of height h above the ground throws its shadow: h * cot(altitude).
inline float reachWorld(float casterHeight, Vec3 towardSun) {
    const Vec3 l = normalized(towardSun);
    const float horiz = std::sqrt(l.x * l.x + l.y * l.y);
    return l.z > 1e-6f ? (std::max)(casterHeight, 0.0f) * horiz / l.z : 0.0f;
}
// The penumbra half-width (in light space) a blocker `rayDistance` away along the sun ray gives.
inline float penumbraWorld(float rayDistance, float tanTheta) { return (std::max)(rayDistance, 0.0f) * tanTheta; }

struct Rect {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    bool overlaps(const Rect& o) const { return x0 <= o.x1 && o.x0 <= x1 && y0 <= o.y1 && o.y0 <= y1; }
};

// The world-XY rectangle tile (tx, ty) computes, apron included (x0 west, y0 south).
inline Rect tileRegionWorld(const MaskLayout& m, int tx, int ty) {
    const float d = m.texelWorld;
    const float west = m.originX + static_cast<float>(tx * kTileTexels - kTileApron) * d;
    const float north = m.originYNorth - static_cast<float>(ty * kTileTexels - kTileApron) * d;
    const float size = static_cast<float>(kRegionTexels) * d;
    return {west, north - size, west + size, north};
}

// The ground footprint (world XY) whose shadow a caster box can touch: the box swept down-sun to `groundZ`, grown by the widest penumbra it can make
// there. It is what a dirty rect starts from when the caster changes (P5) and what bins a caster to tiles.
inline Rect shadowFootprint(Vec3 boxMin, Vec3 boxMax, Vec3 towardSun, float groundZ, float tanTheta) {
    const Vec3 l = normalized(towardSun);
    const float drop = (std::max)(boxMax.z - groundZ, 0.0f);
    const float kx = l.z > 1e-6f ? l.x / l.z : 0.0f, ky = l.z > 1e-6f ? l.y / l.z : 0.0f;
    // The top of the box lands -drop * k away from where it stands (shadows fall away from the sun).
    Rect r{boxMin.x, boxMin.y, boxMax.x, boxMax.y};
    r.x0 = (std::min)(r.x0, boxMin.x - drop * kx); r.x1 = (std::max)(r.x1, boxMax.x - drop * kx);
    r.y0 = (std::min)(r.y0, boxMin.y - drop * ky); r.y1 = (std::max)(r.y1, boxMax.y - drop * ky);
    const float rayDistance = l.z > 1e-6f ? drop / l.z : 0.0f;
    const float pen = penumbraWorld(rayDistance, tanTheta) / (std::max)(l.z, 0.05f);   // a penumbra lies stretched along the ground by 1/sin(altitude)
    return {r.x0 - pen, r.y0 - pen, r.x1 + pen, r.y1 + pen};
}

// The light-space window of a tile: the receivers of the region (x/y rect, heights hMin..hMax) projected onto the light plane, grown by the widest blocker
// search a caster topping out at `casterTopZ` asks for, on a grid of `texelWorld` (snapped, so neighbouring tiles sample the same texels). d range: from
// below the lowest receiver to above the highest caster.
struct Window {
    float lx0 = 0.0f, ly0 = 0.0f, lx1 = 0.0f, ly1 = 0.0f;
    float dMin = 0.0f, dMax = 1.0f;
    int texels = 0;
    float texelWorld = 0.0f;
};
inline Window casterWindow(const LightBasis& b, Rect region, float hMin, float hMax, float casterTopZ, float tanTheta) {
    Window w;
    float lx0 = 1e30f, ly0 = 1e30f, lx1 = -1e30f, ly1 = -1e30f, d0 = 1e30f, d1 = -1e30f;
    for (int c = 0; c < 8; ++c) {
        const Vec3 p{(c & 1) ? region.x1 : region.x0, (c & 2) ? region.y1 : region.y0, (c & 4) ? hMax : hMin};
        const Vec3 q = toLight(b, p);
        lx0 = (std::min)(lx0, q.x); lx1 = (std::max)(lx1, q.x);
        ly0 = (std::min)(ly0, q.y); ly1 = (std::max)(ly1, q.y);
        d0 = (std::min)(d0, q.z); d1 = (std::max)(d1, q.z);
    }
    const float top = (std::max)(casterTopZ, hMax);
    const float sinAlt = (std::max)(b.toward.z, 0.05f);
    const float maxRay = (top - hMin) / sinAlt;
    const float margin = 2.0f * penumbraWorld(maxRay, tanTheta) + 4.0f;
    lx0 -= margin; ly0 -= margin; lx1 += margin; ly1 += margin;
    const float extent = (std::max)(lx1 - lx0, ly1 - ly0);
    w.texelWorld = (std::max)(kWindowTexelWorld, extent / static_cast<float>(kWindowMaxTexels - 2));
    // Snap the origin to the texel grid so the overlapping aprons of two tiles see identical texels.
    w.lx0 = std::floor(lx0 / w.texelWorld) * w.texelWorld;
    w.ly0 = std::floor(ly0 / w.texelWorld) * w.texelWorld;
    // Always the full window: the raster grid is then the same texel grid in every tile.
    w.texels = kWindowMaxTexels;
    w.lx1 = w.lx0 + static_cast<float>(w.texels) * w.texelWorld;
    w.ly1 = w.ly0 + static_cast<float>(w.texels) * w.texelWorld;
    // A caster that shadows a receiver sits on its ray at most maxRay toward the sun from it; d grows toward the sun.
    w.dMin = d0 - 2.0f;
    w.dMax = (std::max)(d1 + maxRay, w.dMin + 1.0f) + 2.0f;
    return w;
}

// World -> window clip, column-major (out[c * 4 + r]), as a GLSL mat4 multiplies a column vector: x, y in [-1, 1] over the window, z in [0, 1] with 0 at
// dMax (nearest the sun), so a LESS depth test keeps the caster closest to the sun.
inline void windowMatrix(const LightBasis& b, const Window& w, float out[16]) {
    const float sx = 2.0f / (w.lx1 - w.lx0), sy = 2.0f / (w.ly1 - w.ly0);
    const float sz = 1.0f / (w.dMax - w.dMin);
    const float row[3][4] = {
        {b.right.x * sx, b.right.y * sx, b.right.z * sx, -(w.lx0 + w.lx1) / (w.lx1 - w.lx0)},
        {b.up.x * sy, b.up.y * sy, b.up.z * sy, -(w.ly0 + w.ly1) / (w.ly1 - w.ly0)},
        {-b.toward.x * sz, -b.toward.y * sz, -b.toward.z * sz, w.dMax * sz},
    };
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 3; ++r) out[c * 4 + r] = row[r][c];
        out[c * 4 + 3] = c == 3 ? 1.0f : 0.0f;
    }
}
inline Vec3 applyMatrix(const float m[16], Vec3 p) {
    return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13], m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
}

// A caster's light-space bounds (lx, ly, d) from its world box; it can shadow the window's receivers only when its x/y bounds meet the window and its
// top reaches above the window's lowest receiver.
struct LightBox { float lx0, ly0, lx1, ly1, d0, d1; };
inline LightBox lightBox(const LightBasis& b, Vec3 boxMin, Vec3 boxMax) {
    LightBox r{1e30f, 1e30f, -1e30f, -1e30f, 1e30f, -1e30f};
    for (int c = 0; c < 8; ++c) {
        const Vec3 q = toLight(b, {(c & 1) ? boxMax.x : boxMin.x, (c & 2) ? boxMax.y : boxMin.y, (c & 4) ? boxMax.z : boxMin.z});
        r.lx0 = (std::min)(r.lx0, q.x); r.lx1 = (std::max)(r.lx1, q.x);
        r.ly0 = (std::min)(r.ly0, q.y); r.ly1 = (std::max)(r.ly1, q.y);
        r.d0 = (std::min)(r.d0, q.z); r.d1 = (std::max)(r.d1, q.z);
    }
    return r;
}
inline bool casterInWindow(const LightBox& c, const Window& w) {
    return c.lx0 <= w.lx1 && w.lx0 <= c.lx1 && c.ly0 <= w.ly1 && w.ly0 <= c.ly1 && c.d1 >= w.dMin;
}

// 64-bit FNV-1a, the bake key's hash.
inline uint64_t fnv1a(const void* data, size_t bytes, uint64_t h = 1469598103934665603ull) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

}  // namespace mc2shadow
