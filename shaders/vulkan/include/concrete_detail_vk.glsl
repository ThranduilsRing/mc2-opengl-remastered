// concrete_detail_vk.glsl -- CONCRETE-ROAD-PBR-DETAIL.
// Material DETAIL (normal, roughness, AO) for MC2's concrete pads, roads and runways. It never carries colour: the caller keeps
// MC2's own albedo and only applies the occlusion and a sun shading term on top of it. Shared by terrain_tier1.frag (cement pads)
// and terrain_overlay_tier1.frag (road / runway / cement overlay tiles).
//
// Material inputs, as the lighting library (packets L4/L5) will want them: cdEval() returns a MaterialDetail (tangent-space normal in
// the world frame x east / y north / z up, roughness, ambient occlusion). cdShade() is the only part that touches lighting and is the
// piece L5 replaces; the evaluation above it moves over unchanged.
// Includer must already have included terrain_frame_vk.glsl (uT) and enabled GL_GOOGLE_include_directive.
#ifndef CONCRETE_DETAIL_VK_GLSL
#define CONCRETE_DETAIL_VK_GLSL

// RGBA = (normal.x, normal.y, roughness, ao); layer 0 = poured concrete (pads), layer 1 = asphalt (roads, runway).
layout(set = 0, binding = 18) uniform sampler2DArray u_concreteDetail;

#define CD_LAYER_PAD  0
#define CD_LAYER_ROAD 1

struct MaterialDetail {
    vec3  n;          // unit normal, world frame (x east, y north, z up)
    float roughness;  // 0..1
    float ao;         // 0..1, 1 = unoccluded
    float lum;        // macro tonal variation, mean 1 (stains, wear zones, slab-to-slab)
};

float cdHash(vec2 p) { p = fract(p * vec2(0.1031, 0.1030)); p += dot(p, p.yx + 33.33); return fract((p.x + p.y) * p.x); }
vec2 cdHash2(vec2 p) { return vec2(cdHash(p), cdHash(p + 17.0)); }
float cdVNoise(vec2 p) {   // value noise 0..1, four hashes
    vec2 i = floor(p), f = fract(p); f = f * f * (3.0 - 2.0 * f);
    return mix(mix(cdHash(i), cdHash(i + vec2(1.0, 0.0)), f.x), mix(cdHash(i + vec2(0.0, 1.0)), cdHash(i + vec2(1.0, 1.0)), f.x), f.y);
}

// Micro detail: two fine tilings (60 wu and a rotated 38 wu patch, so a pebble or crack is pebble-sized next to a mech), each with a
// hashed per-cell UV offset so the repeat does not read at RTS zoom (no extra fetches). Macro variation is ALU-only value noise at
// slab-to-patch scales (tens to hundreds of wu) plus a per-slab (128 wu) hash: small luminance / roughness / AO shifts, never colour.
// `dx`/`dy` are the screen derivatives of the world XY, taken in uniform control flow by the caller (this runs in divergent branches).
MaterialDetail cdEval(int layer, vec2 wp, vec2 dx, vec2 dy) {
    const mat2 R = mat2(0.8, -0.6, 0.6, 0.8);
    const float kScaleA = 1.0 / 60.0;
    const float kScaleB = 1.0 / 38.0;
    vec2 uvA = wp * kScaleA + cdHash2(floor(wp / 700.0));
    vec2 uvB = R * wp * kScaleB + 0.37 + cdHash2(floor(wp / 450.0) + 31.0);
    vec4 a = textureGrad(u_concreteDetail, vec3(uvA, float(layer)), dx * kScaleA * 0.5, dy * kScaleA * 0.5);
    vec4 b = textureGrad(u_concreteDetail, vec3(uvB, float(layer)), R * dx * kScaleB * 0.5, R * dy * kScaleB * 0.5);
    // Image rows run top-down while world v runs north, so the GL-convention green channel is negated.
    vec2 na = vec2(a.x * 2.0 - 1.0, -(a.y * 2.0 - 1.0));
    vec2 nb = vec2(b.x * 2.0 - 1.0, -(b.y * 2.0 - 1.0));
    nb = transpose(R) * nb;                                   // back from the rotated tiling into the world frame
    vec2 nxy = (na + nb) * 0.7071 * uT.concreteDetailStrength;
    float macro = (0.6 * cdVNoise(wp / 220.0) + 0.4 * cdVNoise(wp / 70.0 + 5.0)) * 2.0 - 1.0;   // -1..1
    float slab = cdHash(floor(wp / 128.0)) * 2.0 - 1.0;                                          // per-slab shift
    MaterialDetail d;
    d.n = normalize(vec3(nxy, 1.0));
    d.roughness = clamp(0.5 * (a.z + b.z) + 0.12 * macro + 0.06 * slab, 0.05, 1.0);
    d.ao = clamp(0.5 * (a.w + b.w) + 0.03 * macro, 0.0, 1.0);
    d.lum = 1.0 + 0.07 * macro + 0.03 * slab;                                                    // mean 1, at most +-10 %
    return d;
}

// Relative sun shading of the detail normal against the flat slab MC2 already lights (mean ~1, so the base colour keeps its
// brightness) plus a small roughness-driven sheen. `L` = unit direction to the sun, `dayGate` 0..1 keeps the sheen off at night.
void cdShade(MaterialDetail d, vec3 L, float dayGate, out float diff, out vec3 spec) {
    float lz = max(L.z, 0.35);
    float rel = 1.0 + (dot(d.n, L) - L.z) / lz;
    diff = clamp(mix(1.0, rel, 0.8), 0.85, 1.2);
    // top-down view: half vector between the sun and straight up; GGX lobe, dielectric F0 0.04
    vec3 H = normalize(L + vec3(0.0, 0.0, 1.0));
    float nh = max(dot(d.n, H), 0.0);
    float a2 = pow(clamp(d.roughness, 0.08, 1.0), 4.0);
    float den = nh * nh * (a2 - 1.0) + 1.0;
    float D = a2 / (3.14159265 * den * den);
    // signed against the flat slab's lobe so the mean colour does not shift: glints brighten, facing-away texels darken
    float den0 = H.z * H.z * (a2 - 1.0) + 1.0;
    float D0 = a2 / (3.14159265 * den0 * den0);
    spec = vec3((D - D0) * 0.25 * clamp(dayGate, 0.0, 1.0));
}

// One call for a surface: evaluates the detail and applies it to the caller's colour (AO), sun-shading term and sheen, weighted by w.
void cdApply(int layer, vec2 wp, vec2 dx, vec2 dy, vec3 L, float w, inout vec3 color, inout float nl, inout vec3 spec) {
    MaterialDetail d = cdEval(layer, wp, dx, dy);
    float diff; vec3 sp;
    cdShade(d, normalize(L), uT.ambientDayGate, diff, sp);
    color *= mix(1.0, d.ao, 0.5 * w) * mix(1.0, d.lum, w);
    nl *= mix(1.0, diff, w);
    spec += sp * w;
}
#endif
