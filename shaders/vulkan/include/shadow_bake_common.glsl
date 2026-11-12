// shadow_bake_common.glsl -- the resources and helpers the three static-shadow bake passes share (shadow_bake_pcss / _terrain_march / _resolve.comp).
// One descriptor set for all three: each pass reads the tile parameters and writes its own scratch image; the C++ side is vulkan_shadow_bake.cpp
// (BakeTileParams, LOCKSTEP std140). A "region" is one tile of the mask plus its apron, in mask texels; row 0 is its north edge.
#ifndef SHADOW_BAKE_COMMON_GLSL
#define SHADOW_BAKE_COMMON_GLSL

#ifdef __cplusplus
#include <algorithm>
namespace mc2shadow {
using std::clamp;
#define SHADOW_BAKE_INLINE inline
#else
#define SHADOW_BAKE_INLINE
#endif

// Shared with the CPU doctest: the march's biased ray and cone clearance.
SHADOW_BAKE_INLINE float bakeTerrainRayHeight(float receiver, float distance, float tanAltitude) {
    return receiver + distance * tanAltitude + 0.5f;
}
SHADOW_BAKE_INLINE float bakeTerrainMarchVisibility(float rayHeight, float terrainHeight, float distance, float cone) {
    return clamp(0.5f + 0.5f * (rayHeight - terrainHeight) / (distance * cone), 0.0f, 1.0f);
}
#undef SHADOW_BAKE_INLINE

#ifdef __cplusplus
} // namespace mc2shadow
#else
layout(set = 0, binding = 0, std140) uniform BakeTile {
    mat4  window;       // world -> caster-window clip (x, y in [-1, 1], z in [0, 1] with 0 nearest the sun)
    vec4  region;       // x: world x of the region's west edge, y: world y of its north edge, z: mask texel (world), w: water elevation
    ivec4 regionInfo;   // x: region texels on a side, y/z: global mask texel of region texel 0 (for the per-texel hash), w: apron
    vec4  sun;          // xyz: toward the sun (altitude-clamped), w: tan(sun angular radius)
    vec4  windowInfo;   // x: dMin, y: dMax (light depth range of the window), z: window texel (world), w: window texels on a side
    vec4  heightInfo;   // x: world x of height sample 0, y: its world y, z: sample spacing (world), w: samples on a side
    vec4  fade;         // x: fade start, y: fade end (blocker distance, world), z: fade strength, w: highest caster top (world z)
    vec4  march;        // x: highest terrain z of the map, y: first march step, z: step growth, w: longest march (world)
} uB;
layout(set = 0, binding = 9, std140) uniform BakePass { ivec4 mode; } uP;   // x: the pass step (resolve: 0 blur H, 1 fused combine+tent; pcss: 0 blocker search, 1 penumbra filter)

layout(set = 0, binding = 1) uniform sampler2D u_windowDepth;          // raw depth, for the blocker search
layout(set = 0, binding = 2) uniform sampler2DShadow u_windowCompare;  // LESS_OR_EQUAL compare, bilinear: 1 = lit
layout(set = 0, binding = 3, std430) readonly buffer BakeHeights { float h[]; } uH;
layout(set = 0, binding = 4, rgba16f) uniform image2D u_props;     // prop visibility, mean blocker distance, blocker weight
layout(set = 0, binding = 5, r32f) uniform image2D u_terrain;      // terrain self-shadow visibility
layout(set = 0, binding = 6, rgba16f) uniform image2D u_blur;      // horizontally blurred (distance x weight, weight)
layout(set = 0, binding = 7, r32f) uniform image2D u_combined;     // final visibility before the resolve
layout(set = 0, binding = 8, std430) buffer BakeMaskOut { uint texels[]; } uM;   // words 0..tile^2/4-1: the tile, 4 R8 texels per word, row-major; from kPenumbraBase: the PCSS penumbra list
const uint kPenumbraBase = 65536u;   // = kTileTexels^2 / 4: header (indirect args x, 1, 1, count) then (texel, mean blocker distance bits) pairs; LOCKSTEP vulkan_shadow_bake.cpp

vec2 bakeTexelWorld(ivec2 t) {
    return vec2(uB.region.x + (float(t.x) + 0.5) * uB.region.z, uB.region.y - (float(t.y) + 0.5) * uB.region.z);
}

float bakeHeightAt(ivec2 g) {
    int n = int(uB.heightInfo.w);
    g = clamp(g, ivec2(0), ivec2(n - 1));
    return uH.h[g.x + g.y * n];
}
// Visible terrain triangles (the shared crest-filtered bake, else the 128 wu grid).
float bakeTerrainHeight(vec2 xy) {
    vec2 g = vec2(xy.x - uB.heightInfo.x, uB.heightInfo.y - xy.y) / uB.heightInfo.z;
    vec2 f = fract(g);
    ivec2 i = ivec2(floor(g));
    float a = bakeHeightAt(i), b = bakeHeightAt(i + ivec2(1, 0));
    float c = bakeHeightAt(i + ivec2(0, 1)), d = bakeHeightAt(i + ivec2(1, 1));
    // Match the draw mesh's raw-bake diagonal choice and filtered vertex heights.
    int n = int(uB.heightInfo.w);
    ivec2 cell = clamp(i, ivec2(0), ivec2(n - 2));
    if (uH.h[n * n + cell.x + cell.y * n] > 0.5)
        return f.x + f.y <= 1.0 ? a + (b - a) * f.x + (c - a) * f.y
                               : d + (c - d) * (1.0 - f.x) + (b - d) * (1.0 - f.y);
    return f.y >= f.x ? a + (d - c) * f.x + (c - a) * f.y
                      : a + (b - a) * f.x + (d - b) * f.y;
}

// Deterministic per global mask texel (the same texel in two tiles' aprons gets the same value).
float bakeHash(ivec2 t) {
    uvec2 g = uvec2(t + uB.regionInfo.yz + ivec2(65536));
    uint h = g.x * 1664525u + g.y * 1013904223u;
    h ^= h >> 16; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
    return float(h & 0xFFFFFFu) / 16777216.0;
}

#endif // !__cplusplus
#endif
