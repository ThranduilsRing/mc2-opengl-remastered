// Dense separable tent PCF. Pair adjacent texels into hardware LINEAR compare
// fetches; unlike a rotated sparse disk, every texel in the footprint contributes.
#ifndef SHADOW_FILTER_VK_GLSL
#define SHADOW_FILTER_VK_GLSL
vec2 mc2ShadowPair(int pairIndex, float fraction, float radius) {
    float first = float(pairIndex * 2 - 8);
    float a = max(radius + 1.0 - abs(first - fraction), 0.0);
    float b = max(radius + 1.0 - abs(first + 1.0 - fraction), 0.0);
    float weight = a + b;
    return vec2(first + (weight > 0.0 ? b / weight : 0.0), weight);
}
float mc2ShadowTent(sampler2DShadow smap, vec3 coord, float radius) {
    vec2 size = vec2(textureSize(smap, 0));
    vec2 texel = coord.xy * size - 0.5;
    vec2 base = floor(texel);
    vec2 fraction = texel - base;
    float sum = 0.0, total = 0.0;
    radius = clamp(radius, 0.5, 7.0);
    for (int y = 0; y < 9; ++y) {
        vec2 py = mc2ShadowPair(y, fraction.y, radius);
        if (py.y == 0.0) continue;
        for (int x = 0; x < 9; ++x) {
            vec2 px = mc2ShadowPair(x, fraction.x, radius);
            float weight = px.y * py.y;
            if (weight == 0.0) continue;
            vec2 uv = (base + vec2(px.x, py.x) + 0.5) / size;
            sum += weight * texture(smap, vec3(uv, coord.z));
            total += weight;
        }
    }
    return sum / total;
}
float mc2ShadowTent(sampler2DArrayShadow smap, vec4 coord, float radius) {
    vec2 size = vec2(textureSize(smap, 0).xy);
    vec2 texel = coord.xy * size - 0.5;
    vec2 base = floor(texel);
    vec2 fraction = texel - base;
    float sum = 0.0, total = 0.0;
    radius = clamp(radius, 0.5, 7.0);
    for (int y = 0; y < 9; ++y) {
        vec2 py = mc2ShadowPair(y, fraction.y, radius);
        if (py.y == 0.0) continue;
        for (int x = 0; x < 9; ++x) {
            vec2 px = mc2ShadowPair(x, fraction.x, radius);
            float weight = px.y * py.y;
            if (weight == 0.0) continue;
            vec2 uv = (base + vec2(px.x, py.x) + 0.5) / size;
            sum += weight * texture(smap, vec4(uv, coord.zw));
            total += weight;
        }
    }
    return sum / total;
}
#endif
