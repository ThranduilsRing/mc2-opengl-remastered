#ifndef WATER_SHORE_ANIMATION_GLSL
#define WATER_SHORE_ANIMATION_GLSL
#include "../../include/water_shore_animation.hglsl"

// The terrain set is already bound by the water draw, including this bake.
layout(set = TERRAIN_RESOURCE_SET, binding = 10, std430) readonly buffer ShoreVisualHeight {
    float shoreVisualHeights[];
};

float wsa_visualDepth(vec2 worldXY, float waterElevation, float fallbackDepth)
{
    if (uT.visualMode == 0 || uT.visualSide < 2) return fallbackDepth;
    vec2 grid = vec2(worldXY.x + uT.halfMap, uT.halfMap - worldXY.y) / 32.0;
    if (any(lessThan(grid, vec2(0.0))) || any(greaterThan(grid, vec2(uT.visualSide - 1))))
        return fallbackDepth;
    ivec2 cell = min(ivec2(grid), ivec2(uT.visualSide - 2));
    vec2 f = grid - vec2(cell);
    int i = cell.x + cell.y * uT.visualSide;
    float h = mix(mix(shoreVisualHeights[i], shoreVisualHeights[i + 1], f.x),
                  mix(shoreVisualHeights[i + uT.visualSide], shoreVisualHeights[i + uT.visualSide + 1], f.x), f.y);
    return waterElevation - h;
}

// A separate diffuse normal: reflection sampling and its normal remain owned
// by the reflection lane. The wash is lit, shadowed and fogged with the water.
float wsa_normalLight(vec2 depthGradient, float slope, float phase, float wash)
{
    vec2 tilt = depthGradient / slope * (0.12 * cos(phase) * wash);
    vec3 n = normalize(vec3(tilt, 1.0));
    vec3 l = normalize(uT.terrainLightDir.xyz + vec3(0.0, 0.0, 1e-5));
    return clamp(1.0 + 0.4 * (max(dot(n, l), 0.0) - max(l.z, 0.0)), 0.85, 1.15);
}
#endif
