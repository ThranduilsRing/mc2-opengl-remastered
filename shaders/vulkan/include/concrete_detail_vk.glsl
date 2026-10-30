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

// Continuous fine tiling (48 wu, so a pebble or crack is pebble-sized next to a mech).
// Cell-wise UV, luminance and roughness jumps made flat pads read as large polygons.
// `dx`/`dy` are the screen derivatives of the world XY, taken in uniform control flow by the caller (this runs in divergent branches).
MaterialDetail cdEval(int layer, vec2 wp, vec2 dx, vec2 dy) {
    const float kScale = 1.0 / 48.0;
    vec2 uv = wp * kScale;
    float lod = log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-8)) * 0.5 + log2(kScale * 0.5 * float(textureSize(u_concreteDetail, 0).x));
    vec4 a = textureLod(u_concreteDetail, vec3(uv, float(layer)), max(lod, 0.0));
    // Image rows run top-down while world v runs north, so the GL-convention green channel is negated.
    vec2 nxy = vec2(a.x * 2.0 - 1.0, -(a.y * 2.0 - 1.0)) * uT.concreteDetailStrength;
    MaterialDetail d;
    d.n = normalize(vec3(nxy, 1.0));
    d.roughness = clamp(a.z, 0.05, 1.0);
    // DEV experiment: derivative normal variance, same roughness moment as props.
    if (kExperimentToksvig) {
        vec3 nx=dFdx(d.n), ny=dFdy(d.n);
        d.roughness=pow(min(1.0,pow(d.roughness,4.0)+min(0.25,0.5*(dot(nx,nx)+dot(ny,ny)))),0.25);
    }
    d.ao = a.w;
    d.lum = 1.0;
    return d;
}

// Relative sun shading of the detail normal against the flat slab MC2 already lights (mean ~1, so the base colour keeps its
// brightness) plus a small roughness-driven sheen. `L` = unit direction to the sun, `dayGate` 0..1 keeps the sheen off at night.
void cdShade(MaterialDetail d, vec3 L, float dayGate, out float diff, out vec3 spec) {
    float lz = max(L.z, 0.35);
    float rel = 1.0 + (dot(d.n, L) - L.z) / lz;
    diff = clamp(mix(1.0, rel, 0.8), 0.85, 1.2);
    spec = vec3(0.0);
    // At midnight L can be exactly -up: its half-vector is undefined.
    if (dayGate <= 0.0) return;
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
void cdApply(int layer, vec2 wp, vec2 dx, vec2 dy, vec3 L, float w, inout vec3 color, inout float nl, inout vec3 spec, inout vec3 N) {
    MaterialDetail d = cdEval(layer, wp, dx, dy);
    float diff; vec3 sp;
    cdShade(d, normalize(L), uT.ambientDayGate, diff, sp);
    color *= mix(1.0, d.ao, 0.5 * w) * mix(1.0, d.lum, w);
    nl *= mix(1.0, diff, w);
    spec += sp * w;
    N = normalize(mix(N, d.n, w));
}
#endif
