// shadow_bake_common.glsl -- the resources and helpers the three static-shadow bake passes share (shadow_bake_pcss / _terrain_march / _resolve.comp).
// One descriptor set for all three: each pass reads the tile parameters and writes its own scratch image; the C++ side is vulkan_shadow_bake.cpp
// (BakeTileParams, LOCKSTEP std140). A "region" is one tile of the mask plus its apron, in mask texels; row 0 is its north edge.
#ifndef SHADOW_BAKE_COMMON_GLSL
#define SHADOW_BAKE_COMMON_GLSL

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
layout(set = 0, binding = 9, std140) uniform BakePass { ivec4 mode; } uP;   // x: the resolve pass's step

layout(set = 0, binding = 1) uniform sampler2D u_windowDepth;          // raw depth, for the blocker search
layout(set = 0, binding = 2) uniform sampler2DShadow u_windowCompare;  // LESS_OR_EQUAL compare, bilinear: 1 = lit
layout(set = 0, binding = 3, std430) readonly buffer BakeHeights { float h[]; } uH;
layout(set = 0, binding = 4, rgba16f) uniform image2D u_props;     // prop visibility, mean blocker distance, blocker weight
layout(set = 0, binding = 5, r32f) uniform image2D u_terrain;      // terrain self-shadow visibility
layout(set = 0, binding = 6, rgba16f) uniform image2D u_blur;      // horizontally blurred (distance x weight, weight)
layout(set = 0, binding = 7, r32f) uniform image2D u_combined;     // final visibility before the resolve
layout(set = 0, binding = 8, std430) writeonly buffer BakeMaskOut { uint texels[]; } uM;   // the tile, 4 R8 texels per word, row-major

vec2 bakeTexelWorld(ivec2 t) {
    return vec2(uB.region.x + (float(t.x) + 0.5) * uB.region.z, uB.region.y - (float(t.y) + 0.5) * uB.region.z);
}

float bakeHeightAt(ivec2 g) {
    int n = int(uB.heightInfo.w);
    g = clamp(g, ivec2(0), ivec2(n - 1));
    return uH.h[g.x + g.y * n];
}
// Bilinear terrain height (the visual height bake when the map has one, else the 128 wu grid).
float bakeTerrainHeight(vec2 xy) {
    vec2 g = vec2(xy.x - uB.heightInfo.x, uB.heightInfo.y - xy.y) / uB.heightInfo.z;
    vec2 f = fract(g);
    ivec2 i = ivec2(floor(g));
    float a = bakeHeightAt(i), b = bakeHeightAt(i + ivec2(1, 0));
    float c = bakeHeightAt(i + ivec2(0, 1)), d = bakeHeightAt(i + ivec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Catmull-Rom terrain height: the cone march's surface. A bilinear 128 wu grid has creases along every cell edge, and a sun near the clamp
// turns each crease into a stair-step on the shadow line; the smooth surface keeps the ridge silhouettes round.
vec4 bakeCatmullW(float t) {
    float t2 = t * t, t3 = t2 * t;
    return 0.5 * vec4(-t3 + 2.0 * t2 - t, 3.0 * t3 - 5.0 * t2 + 2.0, -3.0 * t3 + 4.0 * t2 + t, t3 - t2);
}
float bakeTerrainHeightSmooth(vec2 xy) {
    vec2 g = vec2(xy.x - uB.heightInfo.x, uB.heightInfo.y - xy.y) / uB.heightInfo.z;
    ivec2 i = ivec2(floor(g));
    vec2 f = g - vec2(i);
    vec4 wx = bakeCatmullW(f.x), wy = bakeCatmullW(f.y);
    float h = 0.0;
    for (int y = 0; y < 4; ++y) {
        vec4 row = vec4(bakeHeightAt(i + ivec2(-1, y - 1)), bakeHeightAt(i + ivec2(0, y - 1)), bakeHeightAt(i + ivec2(1, y - 1)), bakeHeightAt(i + ivec2(2, y - 1)));
        h += wy[y] * dot(wx, row);
    }
    return h;
}

// Deterministic per global mask texel (the same texel in two tiles' aprons gets the same value).
float bakeHash(ivec2 t) {
    uvec2 g = uvec2(t + uB.regionInfo.yz + ivec2(65536));
    uint h = g.x * 1664525u + g.y * 1013904223u;
    h ^= h >> 16; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
    return float(h & 0xFFFFFFu) / 16777216.0;
}

#endif
