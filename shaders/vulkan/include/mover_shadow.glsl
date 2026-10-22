#ifndef MC2_MOVER_SHADOW_GLSL
#define MC2_MOVER_SHADOW_GLSL
// Fixed receiver seam. The producer may replace the union map with an atlas
// without changing terrain, water, object, overlay or vegetation callers.
layout(set = 0, binding = 24) uniform sampler2DShadow moverShadowMap;
layout(std140, set = 0, binding = 25) uniform MoverShadowFrame {
    mat4 matrix;
    vec4 params; // world texel, reciprocal depth span, normal offset, reciprocal map side
    vec4 sun;
    vec4 mask; // west, north, reciprocal world span, ready
    ivec4 flags; // active, coverage enabled, quality, reserved
    uvec4 coverage[512]; // packed min/max UNORM8, two cells per word
} moverFrame;

float moverVisibility(vec3 worldPos, vec3 N) {
    if (moverFrame.flags.x == 0) return 1.0;
    vec3 samplePos = worldPos + N * moverFrame.params.z;
    vec4 light = moverFrame.matrix * vec4(samplePos, 1.0);
    vec3 pc = light.xyz / light.w;
    pc.xy = pc.xy * 0.5 + 0.5;
    if (pc != clamp(pc, 0.0, 1.0)) return 1.0;
    if (moverFrame.flags.y != 0) {
        uvec2 tile = min(uvec2(pc.xy * 64.0), uvec2(63));
        uint cell = tile.y * 64u + tile.x;
        uint packed = moverFrame.coverage[cell >> 3u][(cell >> 1u) & 3u];
        uint interval = (packed >> ((cell & 1u) * 16u)) & 65535u;
        float lo = float(interval & 255u), hi = float(interval >> 8u);
        if (lo > hi || pc.z * 255.0 < lo || pc.z * 255.0 > hi) return 1.0;
    }
    float bias = 2.0 / 65535.0 + 0.25 * moverFrame.params.x * moverFrame.params.y;
    vec4 taps = textureGather(moverShadowMap, pc.xy, pc.z - bias);
    vec2 f = fract(pc.xy / moverFrame.params.w - 0.5);
    return mix(mix(taps.w, taps.z, f.x), mix(taps.x, taps.y, f.x), f.y);
}
#endif
