#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include "../../shaders/include/water_planar.hglsl"

namespace mc2::waterplanar {
// Terrain lighting applies rain after the weather-adjusted sun/ambient sum.
inline float weatherLight(float sun, float ambient, float rain) {
    return (sun + ambient) * (std::min)(rain, 1.0f);
}

// Reflection quality the water code runs at. options.cfg ReflectionQuality 0 (Off) is SKY-ONLY (level 1: no bake), never
// level 0, which drops the sky too. Otherwise the legacy tuning value stands.
inline int effectiveReflectionQuality(int option, int legacy) { return option <= 0 ? (std::min)(legacy, 1) : legacy; }

// What each baked Reflection quality level (options.cfg ReflectionQuality 1 Low, 2 Medium, 3 High; Off has no bake) asks
// of the reflection bake: its resolution divisor, whether static props are drawn into it, and how many ray-march steps
// the water shader takes through its height map.
struct BakePlan { int sizeDivisor; bool props; int marchSteps; };
// The shader march step count of the shipped bake; water_geom.frag falls back to this literal when debug.z is 0.
constexpr int kHighSteps = 8;
inline BakePlan bakePlan(int level) {
    switch (level < 1 ? 1 : level > 3 ? 3 : level) {
        case 1: return {4, false, 4};   // Low: quarter-size bake, terrain only
        case 2: return {2, true, 6};    // Medium: half-size bake with props
        default: return {1, true, kHighSteps};   // High: the shipped bake
    }
}
// The value the water UBO carries in WaterFsParams::debug[2] (shader F.debug.z) for a plan.
inline float uploadedDebugZ(const BakePlan& plan) { return static_cast<float>(plan.marchSteps); }
inline int bakeSize(int fullSize, const BakePlan& plan) { return (std::max)(1, fullSize / plan.sizeDivisor); }

// Pixel bounds include every possible ripple lookup and bilinear neighbour.
inline std::array<int,4> paddedPixels(const float* ndc, int w, int h) {
    std::array<int,4> pixels{};
    for (int axis=0; axis<2; ++axis) {
        const int size=axis==0 ? w : h;
        const int pad=static_cast<int>(std::ceil(size*0.009f))+1;
        pixels[axis]=std::clamp(static_cast<int>(std::floor((std::clamp(ndc[axis],-1.0f,1.0f)*0.5f+0.5f)*size))-pad,0,size);
        pixels[axis+2]=std::clamp(static_cast<int>(std::ceil((std::clamp(ndc[axis+2],-1.0f,1.0f)*0.5f+0.5f)*size))+pad,0,size);
    }
    return pixels;
}

// Column-major, column-vector matrix, identical to the terrain/prop vertex
// shader uploads. Right multiplication reflects WORLD Z, before projection.
inline std::array<float, 16> reflectedWorldToClip(const float* main, float waterElevation) {
    std::array<float, 16> reflected{};
    std::copy_n(main, 16, reflected.begin());
    for (int row = 0; row < 4; ++row) {
        reflected[8 + row] = -main[8 + row];
        reflected[12 + row] = main[12 + row] + 2.0f * waterElevation * main[8 + row];
    }
    return reflected;
}

// Mirror determinant is -1: a culling pipeline reverses its authored front
// face exactly once. A double-sided terrain pipeline remains double-sided.
inline constexpr bool reversesWinding = true;

struct View {
    std::array<float, 16> worldToClip;
    std::array<std::array<float, 4>, 6> planes{};
    float waterElevation;

    View(const float* main, float water)
        : worldToClip(reflectedWorldToClip(main, water)), waterElevation(water) {
        // Native Vulkan clip volume: -w <= x,y <= w, 0 <= z <= w.
        // Reverse Z changes which depth face is near, not these half-spaces.
        for (int col = 0; col < 4; ++col) {
            const float* m = worldToClip.data() + col * 4;
            planes[0][col] = m[3] + m[0];
            planes[1][col] = m[3] - m[0];
            planes[2][col] = m[3] + m[1];
            planes[3][col] = m[3] - m[1];
            planes[4][col] = m[2];
            planes[5][col] = m[3] - m[2];
        }
    }

    bool intersects(std::array<float, 3> lo, const std::array<float, 3>& hi) const {
        if (wp_clip_distance(hi[2], waterElevation) < 0.0f) return false;
        lo[2] = (std::max)(lo[2], waterElevation);
        for (const auto& p : planes) {
            float distance = p[3];
            for (int axis = 0; axis < 3; ++axis)
                distance += p[axis] * (p[axis] >= 0.0f ? hi[axis] : lo[axis]);
            if (distance < 0.0f) return false;
        }
        return true;
    }
};

} // namespace mc2::waterplanar
