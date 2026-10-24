#ifndef MC2_MOVER_SHADOW_GLSL
#define MC2_MOVER_SHADOW_GLSL
// Fixed receiver seam. The producer may replace the union map with an atlas
// without changing terrain, water, object, overlay or vegetation callers.
layout(set = 0, binding = 24) uniform sampler2D moverShadowMap;
layout(std140, set = 0, binding = 25) uniform MoverShadowFrame {
    mat4 matrix;
    vec4 params; // world texel, reciprocal depth span, normal offset, reciprocal map side
    vec4 sun;
    vec4 mask; // west, north, reciprocal world span, ready
    ivec4 flags; // active, coverage enabled, quality, reserved
    vec4 light; // x: night fade of every directional sun shadow (0 = day strength, 1 = none)
    uvec4 coverage[512]; // packed min/max UNORM8, two cells per word
} moverFrame;

// Floor of a directional sun shadow for this mission's light: the day floor by day, rising toward 1
// (no shadow) as the night profile takes the sun away. Every sun-shadow receiver uses it.
float sunShadowFloor(float dayFloor) { return mix(dayFloor, 1.0, moverFrame.light.x); }

float moverCompare(vec2 uv, float depth) {
    vec4 taps = step(vec4(depth), textureGather(moverShadowMap, uv, 0));
    vec2 f = fract(uv / moverFrame.params.w - 0.5);
    return mix(mix(taps.w, taps.z, f.x), mix(taps.x, taps.y, f.x), f.y);
}

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
    // Match the bake's 1.5 degree sun, short-blocker lift and linear 480 wu
    // fade. Raw depth shares the existing image view and descriptor slot.
    float tanT = moverFrame.sun.w;
    float worldToUV = moverFrame.params.w / moverFrame.params.x;
    float search = max(480.0 * tanT, 1.5 * moverFrame.params.x);
    float sumD = 0.0, found = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 offset = vec2(x, y) * search;
            vec4 depths = textureGather(moverShadowMap, pc.xy + offset * worldToUV, 0);
            for (int k = 0; k < 4; ++k) {
                float d = (pc.z - depths[k]) / moverFrame.params.y;
                float lift = 1.0 + 2.0 * (1.0 - clamp(d / 200.0, 0.0, 1.0));
                if (depths[k] < pc.z - bias && d * tanT * lift >= length(offset) - moverFrame.params.x) {
                    sumD += d;
                    found += 1.0;
                }
            }
        }
    }
    if (found == 0.0) return 1.0;
    float meanD = sumD / found;
    float lift = 1.0 + 2.0 * (1.0 - clamp(meanD / 200.0, 0.0, 1.0));
    float radius = max(meanD * tanT * lift, 0.75 * moverFrame.params.x);
    int count = moverFrame.flags.z == 0 ? 12 : 32;
    // World-anchored rotation breaks up repeated edge bands without a temporal
    // sequence or a camera-dependent pattern.
    vec2 cell = floor(worldPos.xy / max(0.5 * moverFrame.params.x, 1.0));
    float rotation = fract(sin(dot(cell, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
    float visibility = 0.0;
    for (int i = 0; i < count; ++i) {
        float r = sqrt((float(i) + 0.5) / float(count)) * radius;
        float angle = float(i) * 2.39996323 + rotation;
        visibility += moverCompare(pc.xy + r * vec2(cos(angle), sin(angle)) * worldToUV, pc.z - bias);
    }
    float soft = 1.0 - smoothstep(0.21, 0.57, moverFrame.sun.z);
    float fadeMax = 0.65 * (0.25 + 0.75 * soft);
    return mix(visibility / float(count), 1.0, fadeMax * clamp(meanD / 480.0, 0.0, 1.0));
}
#endif
