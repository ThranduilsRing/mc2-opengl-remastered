// Hex-tiling anti-repetition, after Morten S. Mikkelsen, "Practical Real-Time Hex-Tiling", JCGT 2022 (reference code MIT licence,
// Copyright (c) 2022 Morten S. Mikkelsen; https://github.com/mmikk/hextile-demo). Ported to GLSL: triangle-grid cell weights, per-vertex
// rotation + random offset, variance-preserving blend (the paper's contrast-preserving form, constant mean per channel).
// Sampled with textureGrad and the rotated original derivatives, so the footprint (mip) matches the untiled lookup. Normal maps: tangent
// xy (rg) is rotated back by each tap's rotation before blending.
vec2 hexHash(vec2 p) { return fract(sin(vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)))) * 43758.5453); }
const float HEX_CELL = 1.7;   // hex cell width in texture repeats (paper: ~0.29): bigger cells keep more of each scan's own structure intact
const float HEX_K = 3.4641016 / HEX_CELL;
vec2 hexCen(vec2 v) { return vec2(v.x + 0.5 * v.y, v.y / 1.15470054) / HEX_K; }
mat2 hexRot(vec2 v, float strength) {
    float a = mod(abs(v.x * v.y) + abs(v.x + v.y) + 3.14159265, 6.2831853);
    a = (a > 3.14159265 ? a - 6.2831853 : a) * strength;
    return mat2(cos(a), sin(a), -sin(a), cos(a));
}
vec4 hexTileGrad(sampler2DArray tex, float layer, vec2 uv, vec2 dx, vec2 dy, float rotStrength, bool isNormal, vec4 mean) {
    vec2 sk = uv * HEX_K; sk.y = -0.57735027 * sk.x + 1.15470054 * sk.y;
    vec2 base = floor(sk);
    vec3 t = vec3(fract(sk), 0.0); t.z = 1.0 - t.x - t.y;
    float s = step(0.0, -t.z), s2 = 2.0 * s - 1.0;
    vec3 w = vec3(-t.z * s2, s - t.y * s2, s - t.x * s2);
    w = w * w * w; w /= (w.x + w.y + w.z);   // narrow blend seams: the linear blend averaged crisp ledges away (cliff bar); the cube keeps most of each cell pure
    vec2 v[3] = vec2[3](base + vec2(s, s), base + vec2(s, 1.0 - s), base + vec2(1.0 - s, s));
    vec4 acc = vec4(0.0);
    for (int i = 0; i < 3; ++i) {
        vec2 cen = hexCen(v[i]); mat2 R = hexRot(v[i], rotStrength);
        vec4 c = textureGrad(tex, vec3(R * (uv - cen) + cen + hexHash(v[i]), layer), R * dx, R * dy);
        // Rotate in row-down UV space, then restore the authored NormalGL basis.
        if (isNormal) c.rg = (vec2(1.0, -1.0) * (transpose(R) * ((c.rg * 2.0 - 1.0) * vec2(1.0, -1.0)))) * 0.5 + 0.5;
        acc += w[i] * c;
    }
    return clamp(mean + (acc - mean) / sqrt(dot(w, w)), 0.0, 1.0);
}
const float HEX_ROT_CLIFF = 0.0;   // offset-only on cliffs: rotating the scan turned its horizontal strata diagonal (proof 1, owner bar: cliff look unchanged)
