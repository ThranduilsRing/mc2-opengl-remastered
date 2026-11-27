// One light-space window for the movers whose swept shadows meet the view.
#pragma once
#include "shadow_bake_math.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <span>

namespace mc2shadow {
// Mover penumbra cone: half the static bake's 1.5 degree radius, so mech shadows stay crisp further from the feet
// (owner 2026-11-09) while the penumbra still grows with blocker distance.
inline constexpr float kMoverSunAngularRadiusDeg = 0.75f;
// Mover shadow sharpness, 0..3 (visual_tuning.json "moverShadowSharpness"): 0 = the old soft look, 1 sharp, 2 sharper, 3 razor.
// Linear between the listed steps; sets the penumbra cone half-angle (the shader derives its filter-radius floors from the same value).
inline float moverSunAngularRadiusDeg(float sharpness) {
    constexpr float deg[4] = {kMoverSunAngularRadiusDeg, 0.4f, 0.2f, 0.05f};
    const float s = (std::clamp)(sharpness, 0.0f, 3.0f);
    const int i = (std::min)(static_cast<int>(s), 2);
    return deg[i] + (deg[i + 1] - deg[i]) * (s - static_cast<float>(i));
}
struct MoverSphere { Vec3 center; float radius; };
// Coverage grid side. shaders/vulkan/include/mover_shadow.glsl hardcodes 120 / 119 / 120.0 and uvec4 coverage[900] (= side^2 / 16 uvec4);
// scripts/tests/test_mover_shadow_crisp.py pins those literals to this constant, so change all together.
inline constexpr int kMoverCoverageSide = 120;
struct alignas(16) MoverShadowFrame {
    float matrix[16]{};
    float params[4]{}; // world texel, reciprocal depth span, normal offset, reciprocal map side
    float sun[4]{};
    float mask[4]{};   // west, north, reciprocal world span, ready
    int32_t flags[4]{}; // active, coverage enabled, quality, union path
    float light[4]{};   // x: night fade (static); y: spatial filter sample count; z: mover night fade; w reserved
    std::array<uint32_t, kMoverCoverageSide * kMoverCoverageSide / 4> coverage{}; // 120x120 cells, one UNORM8 nearest-caster depth per byte; 255 = empty
};
static_assert(kMoverCoverageSide * kMoverCoverageSide % 16 == 0 && kMoverCoverageSide * kMoverCoverageSide / 16 == 900); // GLSL uvec4 coverage[900]
static_assert(sizeof(MoverShadowFrame) == 14544);
static_assert(offsetof(MoverShadowFrame, coverage) == 144);

inline Vec3 shadowEnd(MoverSphere s, Vec3 sun, float ground) {
    const float reach = (std::max)(0.0f, s.center.z + s.radius - ground) / (std::max)(sun.z, 0.001f);
    return {s.center.x - sun.x * reach, s.center.y - sun.y * reach, s.center.z - sun.z * reach};
}

// Camera rows are explicit: clip[i] = dot(rows[i], vec4(world,1)), Z in [0,W].
// The six view planes and their normal lengths, built once per frame (bit-identical to rebuilding them per sphere).
struct ViewPlanes {
    float plane[6][4]{};
    float norm[6]{};
};
inline ViewPlanes makeViewPlanes(const float rows[16]) {
    ViewPlanes v;
    for (int p = 0; p < 6; ++p) {
        for (int k = 0; k < 4; ++k) {
            if (p == 4) v.plane[p][k] = rows[8 + k];
            else v.plane[p][k] = rows[12 + k] + (p % 2 == 0 ? 1.0f : -1.0f) * rows[(p / 2) * 4 + k];
        }
        v.norm[p] = std::sqrt(v.plane[p][0]*v.plane[p][0] + v.plane[p][1]*v.plane[p][1] + v.plane[p][2]*v.plane[p][2]);
    }
    return v;
}
inline bool sweptShadowInView(MoverSphere s, Vec3 sun, float ground, const ViewPlanes& v) {
    const Vec3 end = shadowEnd(s, sun, ground);
    for (int p = 0; p < 6; ++p) {
        const float* plane = v.plane[p];
        const float margin = s.radius * v.norm[p];
        const auto distance = [&](Vec3 q) { return plane[0]*q.x + plane[1]*q.y + plane[2]*q.z + plane[3]; };
        if ((std::max)(distance(s.center), distance(end)) < -margin) return false;
    }
    return true;
}
inline bool sweptShadowInView(MoverSphere s, Vec3 sun, float ground, const float rows[16]) {
    return sweptShadowInView(s, sun, ground, makeViewPlanes(rows));
}

// True when the swept shadow of a static caster (its sphere dragged along the sun ray to the ground) comes within reach of a mover sphere. A caster
// that fails this for every mover in view cannot shadow any mover; the baked static mask already carries its shadow on the ground.
inline bool propShadowReachesMover(MoverSphere prop, MoverSphere mover, Vec3 sun, float ground) {
    const Vec3 a = prop.center, b = shadowEnd(prop, sun, ground);
    const Vec3 ab{b.x - a.x, b.y - a.y, b.z - a.z}, am{mover.center.x - a.x, mover.center.y - a.y, mover.center.z - a.z};
    const float len2 = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
    const float t = len2 > 1e-6f ? (std::clamp)((am.x * ab.x + am.y * ab.y + am.z * ab.z) / len2, 0.0f, 1.0f) : 0.0f;
    const Vec3 d{am.x - ab.x * t, am.y - ab.y * t, am.z - ab.z * t};
    const float reach = prop.radius + mover.radius;
    return d.x * d.x + d.y * d.y + d.z * d.z <= reach * reach;
}

// Clip the ground/map rectangle to the camera, including horizon-crossing views.
// This also clamps far-row ground rays to the map rather than to camera altitude.
inline MoverSphere moverGroundFootprint(const float rows[16], float ground, float halfMap, Vec3 focus = {}) {
    std::vector<Vec3> polygon{{-halfMap,-halfMap,ground},{halfMap,-halfMap,ground},
                              {halfMap,halfMap,ground},{-halfMap,halfMap,ground}};
    const auto view = makeViewPlanes(rows);
    for (const auto& plane : view.plane) {
        std::vector<Vec3> clipped;
        if (polygon.empty()) break;
        Vec3 a = polygon.back();
        const auto distance = [&](Vec3 p) { return plane[0]*p.x+plane[1]*p.y+plane[2]*p.z+plane[3]; };
        float da = distance(a);
        for (const auto b : polygon) {
            const float db = distance(b);
            if ((da >= 0) != (db >= 0)) {
                const float t = da / (da-db);
                clipped.push_back({a.x+t*(b.x-a.x),a.y+t*(b.y-a.y),ground});
            }
            if (db >= 0) clipped.push_back(b);
            a=b; da=db;
        }
        polygon=std::move(clipped);
    }
    // A circle about the focus rotates with the frustum; an XY bounding-box
    // midpoint changes both the centre and the radius when yaw changes.
    MoverSphere region{{focus.x,focus.y,ground},4};
    for (const auto p : polygon) {
        const Vec3 d{p.x-region.center.x,p.y-region.center.y,0};
        region.radius=(std::max)(region.radius,std::sqrt(dot(d,d)));
    }
    return region;
}

inline float moverRadiusBand(float radius, float& band) {
    radius=(std::max)(radius,4.0f);
    band=radius;
    return band;
}

// XY follows the ground footprint continuously; depth covers the admitted
// casters and their ground reach, preserving D16 and coverage-grid precision.
inline Window fitMoverWindow(std::span<const MoverSphere> spheres, const LightBasis& basis,
                             float ground, int mapSide, MoverSphere region) {
    Window w{};
    if (spheres.empty()) return w;
    w.dMin = 1e30f; w.dMax = -1e30f;
    for (const auto& s : spheres) {
        const Vec3 q = toLight(basis, s.center), e = toLight(basis, shadowEnd(s, basis.toward, ground));
        w.dMin = (std::min)(w.dMin, e.z - s.radius - 4.0f);
        w.dMax = (std::max)(w.dMax, q.z + s.radius + 4.0f);
    }
    const float extent = 2.0f * (std::max)(region.radius, 4.0f) + 16.0f;
    const Vec3 center = toLight(basis, region.center);
    w.texels = mapSide; w.texelWorld = extent / static_cast<float>(mapSide);
    const float cx = std::round(center.x / w.texelWorld) * w.texelWorld;
    const float cy = std::round(center.y / w.texelWorld) * w.texelWorld;
    w.lx0 = cx - extent * 0.5f; w.lx1 = cx + extent * 0.5f;
    w.ly0 = cy - extent * 0.5f; w.ly1 = cy + extent * 0.5f;
    w.dMin = std::floor(w.dMin); w.dMax = std::ceil((std::max)(w.dMax, w.dMin + 16.0f));
    return w;
}

// A cell holds the nearest caster depth (UNORM8, 254 max) of every sphere whose light-space footprint,
// grown by the receiver's blocker-search reach, touches it; 255 = no caster. mover_shadow.glsl skips the
// blocker search where nothing can sit in front of the receiver.
inline void buildMoverCoverage(MoverShadowFrame& frame, std::span<const MoverSphere> spheres,
                                const LightBasis& basis, const Window& w) {
    frame.coverage.fill(0xffffffffu);
    const float reach = 128.0f * std::tan(kMoverSunAngularRadiusDeg * 0.01745329252f) + 4.0f * w.texelWorld
                        + 1.5f * w.texelWorld;
    const int n = kMoverCoverageSide;
    const float invX = static_cast<float>(n) / (w.lx1 - w.lx0), invY = static_cast<float>(n) / (w.ly1 - w.ly0);
    for (const auto& s : spheres) {
        const Vec3 q = toLight(basis, s.center);
        const float r = s.radius + reach;
        const int x0 = (std::clamp)(static_cast<int>(std::floor((q.x - r - w.lx0) * invX)), 0, n - 1);
        const int y0 = (std::clamp)(static_cast<int>(std::floor((q.y - r - w.ly0) * invY)), 0, n - 1);
        const int x1 = (std::clamp)(static_cast<int>(std::floor((q.x + r - w.lx0) * invX)), 0, n - 1);
        const int y1 = (std::clamp)(static_cast<int>(std::floor((q.y + r - w.ly0) * invY)), 0, n - 1);
        const uint32_t nearZ = static_cast<uint32_t>((std::clamp)(std::floor((w.dMax - q.z - s.radius) / (w.dMax - w.dMin) * 255.0f) - 1.0f, 0.0f, 254.0f));
        for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
            const unsigned cell = static_cast<unsigned>(y * n + x), shift = (cell & 3u) * 8u;
            auto& word = frame.coverage[cell / 4];
            const uint32_t lo = (std::min)((word >> shift) & 255u, nearZ);
            word = (word & ~(255u << shift)) | (lo << shift);
        }
    }
}

// Per-frame mech actor table and its draw-run masks, stamped with the shadow frame epoch. A mask requested for another epoch is all-ones (never culls on stale data).
struct MechActorTable {
    struct Actor { MoverSphere sphere; bool inView; };
    std::vector<Actor> actors;
    std::vector<int32_t> instActor;
    uint64_t epoch = ~0ull;
    void begin(uint64_t e) { actors.clear(); instActor.clear(); epoch = e; }
    template <class Run, class Pred>
    static bool maskRuns(const std::vector<Run>& runs, const std::vector<int32_t>& instActor, Pred keep, std::vector<uint8_t>& mask) {
        mask.assign(runs.size(), 1);
        if (instActor.empty()) return false;   // unknown: draw everything
        for (size_t k = 0; k < runs.size(); ++k) {
            const auto& r = runs[k];
            if (static_cast<uint64_t>(r.instanceBase) + r.instanceCount > instActor.size()) continue;   // cannot map: keep
            bool any = false;
            for (uint32_t i = 0; i < r.instanceCount && !any; ++i) {
                const int32_t a = instActor[r.instanceBase + i];
                any = a < 0 || keep(a);   // an unmapped instance is kept
            }
            mask[k] = any ? 1 : 0;
        }
        return true;
    }
    template <class Run>
    bool viewMask(uint64_t e, const std::vector<Run>& runs, std::vector<uint8_t>& mask) const {
        if (e != epoch) { mask.assign(runs.size(), 1); return false; }
        return maskRuns(runs, instActor, [&](int32_t a) { return actors[a].inView; }, mask);
    }
    template <class Run>
    bool lightMask(uint64_t e, const std::vector<Run>& runs, const float pos[3], float farDistance, std::vector<uint8_t>& mask) const {
        if (e != epoch) { mask.assign(runs.size(), 1); return false; }
        return maskRuns(runs, instActor, [&](int32_t a) {
            const auto& sp = actors[a].sphere;
            const float dx = sp.center.x - pos[0], dy = sp.center.y - pos[1], dz = sp.center.z - pos[2];
            const float reach = farDistance + sp.radius + 2.0f;
            return dx * dx + dy * dy + dz * dz <= reach * reach;
        }, mask);
    }
};
} // namespace mc2shadow
