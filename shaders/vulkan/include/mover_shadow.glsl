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
    vec4 light; // x: night fade; y: spatial filter sample count
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
    // Movers need a dense local blocker search. Spacing the taps for a 480-wu
    // building missed short mech penumbras and returned lit before PCF ran.
    float search = max(64.0 * tanT, 1.5 * moverFrame.params.x);
    float sumD = 0.0, found = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 offset = vec2(x, y) * search;
            vec4 depths = textureGather(moverShadowMap, pc.xy + offset * worldToUV, 0);
            // Mask the gathered vector instead of looping over dynamic components.
            // Keep the scalar accumulation order so blocker depth and count agree.
            vec4 d = (vec4(pc.z) - depths) / moverFrame.params.y;
            vec4 lift = 1.0 + 2.0 * (1.0 - clamp(d / 200.0, 0.0, 1.0));
            bvec4 near = lessThan(depths, vec4(pc.z - bias));
            bvec4 reach = greaterThanEqual(d * tanT * lift, vec4(length(offset) - moverFrame.params.x));
            bvec4 accept = bvec4(near.x && reach.x, near.y && reach.y, near.z && reach.z, near.w && reach.w);
            vec4 ds = mix(vec4(0.0), d, accept);
            vec4 ns = mix(vec4(0.0), vec4(1.0), accept);
            sumD += ds.x; sumD += ds.y; sumD += ds.z; sumD += ds.w;
            found += ns.x; found += ns.y; found += ns.z; found += ns.w;
        }
    }
    if (found == 0.0) return 1.0;
    float meanD = sumD / found;
    float lift = 1.0 + 2.0 * (1.0 - clamp(meanD / 200.0, 0.0, 1.0));
    float radius = max(meanD * tanT * lift, max(1.5 * moverFrame.params.x, 1.0));
    int count = min(moverFrame.flags.z == 0 ? 12 : 32, int(moverFrame.light.y));
    // World-anchored rotation breaks up repeated edge bands without a temporal
    // sequence or a camera-dependent pattern.
    // A continuous world-space phase avoids square filter-rotation cells on
    // magnified shadows. The penumbra always covers more than one depth texel.
    float rotation = dot(worldPos.xy, vec2(0.067, 0.113)) / max(moverFrame.params.x, 1.0);
    float visibility = 0.0;
    // Rotate the same Vogel direction by its constant step; evaluate the
    // pixel's sine/cosine once instead of once for every filter sample.
    vec2 direction = vec2(cos(rotation), sin(rotation));
    const vec2 turn = vec2(cos(2.39996323), sin(2.39996323));
    for (int i = 0; i < count; ++i) {
        float r = sqrt((float(i) + 0.5) / float(count)) * radius;
        visibility += moverCompare(pc.xy + r * direction * worldToUV, pc.z - bias);
        direction = vec2(direction.x * turn.x - direction.y * turn.y,
                         direction.x * turn.y + direction.y * turn.x);
    }
    float soft = 1.0 - smoothstep(0.21, 0.57, moverFrame.sun.z);
    float fadeMax = 0.65 * (0.25 + 0.75 * soft);
    return mix(visibility / float(count), 1.0, fadeMax * clamp(meanD / 480.0, 0.0, 1.0));
}
#endif
