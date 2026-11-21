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

// Only the extent has hysteresis; the image is cleared and redrawn every frame.
// No accumulated samples, reprojection, history texture or temporal filtering.
inline float stabilizedMoverExtent(float required, float previous) {
    constexpr float step = 1.41421356237f;
    if (previous >= required && required >= previous / step * 0.9f) return previous;
    return std::pow(step, std::ceil(std::log((std::max)(required, 8.0f)) / std::log(step)));
}

inline Window fitMoverWindow(std::span<const MoverSphere> spheres, const LightBasis& basis,
                             float ground, int mapSide, float& previousExtent) {
    Window w{};
    if (spheres.empty()) return w;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    w.dMin = 1e30f; w.dMax = -1e30f;
    for (const auto& s : spheres) {
        const Vec3 q = toLight(basis, s.center), e = toLight(basis, shadowEnd(s, basis.toward, ground));
        x0 = (std::min)(x0, q.x - s.radius); x1 = (std::max)(x1, q.x + s.radius);
        y0 = (std::min)(y0, q.y - s.radius); y1 = (std::max)(y1, q.y + s.radius);
        w.dMin = (std::min)(w.dMin, e.z - s.radius - 4.0f);
        w.dMax = (std::max)(w.dMax, q.z + s.radius + 4.0f);
    }
    const float required = (std::max)(x1 - x0, y1 - y0) + 16.0f;
    previousExtent = stabilizedMoverExtent(required, previousExtent);
    w.texels = mapSide; w.texelWorld = previousExtent / static_cast<float>(mapSide);
    const float cx = std::round((x0 + x1) * 0.5f / w.texelWorld) * w.texelWorld;
    const float cy = std::round((y0 + y1) * 0.5f / w.texelWorld) * w.texelWorld;
    w.lx0 = cx - previousExtent * 0.5f; w.lx1 = cx + previousExtent * 0.5f;
    w.ly0 = cy - previousExtent * 0.5f; w.ly1 = cy + previousExtent * 0.5f;
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
