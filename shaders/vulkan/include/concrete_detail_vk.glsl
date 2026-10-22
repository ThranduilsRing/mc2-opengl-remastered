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
};

// Two unrelated tilings blended so the repeat does not read: 96 wu patch plus a rotated 61 wu patch.
// `dx`/`dy` are the screen derivatives of the world XY, taken in uniform control flow by the caller (this runs in divergent branches).
MaterialDetail cdEval(int layer, vec2 wp, vec2 dx, vec2 dy) {
    const mat2 R = mat2(0.8, -0.6, 0.6, 0.8);
    const float kScaleA = 1.0 / 96.0;
    const float kScaleB = 1.0 / 61.0;
    vec4 a = textureGrad(u_concreteDetail, vec3(wp * kScaleA, float(layer)), dx * kScaleA, dy * kScaleA);
    vec4 b = textureGrad(u_concreteDetail, vec3(R * wp * kScaleB + 0.37, float(layer)),
                         R * dx * kScaleB, R * dy * kScaleB);
    // Image rows run top-down while world v runs north, so the GL-convention green channel is negated.
    vec2 na = vec2(a.x * 2.0 - 1.0, -(a.y * 2.0 - 1.0));
    vec2 nb = vec2(b.x * 2.0 - 1.0, -(b.y * 2.0 - 1.0));
    nb = transpose(R) * nb;                                   // back from the rotated tiling into the world frame
    vec2 nxy = (na + nb) * 0.7071 * uT.concreteDetailStrength;
    MaterialDetail d;
    d.n = normalize(vec3(nxy, 1.0));
    d.roughness = 0.5 * (a.z + b.z);
    d.ao = 0.5 * (a.w + b.w);
    return d;
}

// Relative sun shading of the detail normal against the flat slab MC2 already lights (mean ~1, so the base colour keeps its
// brightness) plus a small roughness-driven sheen. `L` = unit direction to the sun, `dayGate` 0..1 keeps the sheen off at night.
void cdShade(MaterialDetail d, vec3 L, float dayGate, out float diff, out vec3 spec) {
    float lz = max(L.z, 0.35);
    float rel = 1.0 + (dot(d.n, L) - L.z) / lz;
    diff = clamp(mix(1.0, rel, 0.7), 0.55, 1.5);
    // top-down view: half vector between the sun and straight up; GGX lobe, dielectric F0 0.04
    vec3 H = normalize(L + vec3(0.0, 0.0, 1.0));
    float nh = max(dot(d.n, H), 0.0);
    float a2 = pow(clamp(d.roughness, 0.08, 1.0), 4.0);
    float den = nh * nh * (a2 - 1.0) + 1.0;
    float D = a2 / (3.14159265 * den * den);
    spec = vec3(D * 0.04 * 3.0 * clamp(dayGate, 0.0, 1.0));
}
#endif
