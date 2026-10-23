// Static-prop material INPUTS: the authored per-slot row + normal/ORM arrays -> SurfaceMaterial.
// No lighting here: composition (ambient, hemisphere, SH, sun, specular) lives in
// prop_lighting_compat_vk.glsl, owned by the lighting lane.
#include "../../include/surface_material.hglsl"
#include "../../include/material_gpu.hglsl"
struct PropMaterialGpu {
    MaterialGpu material;
    vec2 uvScale;
    float normalStrength;
    float aoStrength;
    uvec4 metadata;          // x = SURF_CLASS_*, y = SURF_ALPHA_*
};
layout(set = 0, binding = 10) uniform sampler2DArray uPropNormal;
layout(set = 0, binding = 11) uniform sampler2DArray uPropOrm;
layout(set = 0, binding = 12, std430) readonly buffer PropMaterials {
    PropMaterialGpu materials[];
} uPropMaterials;

vec3 propDecodeColor(vec3 c) { return surfaceDecodeSrgb(c); }
vec3 propEncodeColor(vec3 c) { return surfaceEncodeSrgb(c); }

// Derivatives use continuous detail UVs (before repeat), world positions and a
// render-space normal. Solving both axes preserves mirrored UV handedness.
vec3 propDetailNormal(vec3 geometric, vec2 uv, vec3 sampled, float strength) {
    vec3 px = dFdx(vWorldPos), py = dFdy(vWorldPos);
    vec2 ux = dFdx(uv), uy = dFdy(uv);
    float det = ux.x * uy.y - ux.y * uy.x;
    if (abs(det) < 1e-10) return geometric;
    vec3 t = (px * uy.y - py * ux.y) / det;
    vec3 b = (py * ux.x - px * uy.x) / det;
    t -= geometric * dot(geometric, t);
    float len2 = dot(t,t);
    if (len2 < 1e-12) return geometric;
    t *= inversesqrt(len2);
    float handedness = dot(cross(geometric, t), b) < 0.0 ? -1.0 : 1.0;
    b = cross(geometric, t) * handedness;
    vec3 n = sampled * 2.0 - 1.0;
    n.xy *= strength;
    if (dot(n,n) < 1e-8) return geometric;
    return normalize(mat3(t,b,geometric) * normalize(n));
}

SurfaceMaterial propSurface(int id, vec3 albedo) {
    PropMaterialGpu row = uPropMaterials.materials[id];
    vec2 uv = vUv * row.uvScale;
    SurfaceMaterial s;
    s.baseColor = propDecodeColor(albedo);
    s.normal = normalize(mc2ToRender(vNormalStuff));
    if (row.material.normalTex != 0xffffffffu) {
        vec3 sampleN = texture(uPropNormal, vec3(uv, float(row.material.normalTex))).rgb;
        s.normal = propDetailNormal(s.normal, uv, sampleN, row.normalStrength);
    }
    // No ORM map: the class scalars (row factors) with neutral AO and full coverage.
    vec3 orm = vec3(1);
    if (row.material.metallicRoughnessTex != 0xffffffffu)
        orm = texture(uPropOrm, vec3(uv, float(row.material.metallicRoughnessTex))).rgb;
    s.ao = mix(1.0, orm.r, row.aoStrength);
    s.roughness = clamp(orm.g * row.material.roughnessFactor, 0.3, 1.0);
    s.metallic = clamp(orm.b * row.material.metallicFactor, 0.0, 1.0);
    // Ordinary baked vertex light is a lighting input, not material emission.
    // Window nodes expose their emitted radiance directly for the future L6 consumer.
    s.emission = (vMaterialFlags & 2u) != 0u ? s.baseColor * propDecodeColor(vMaterialSeed) : vec3(0);
    s.alphaMode = row.metadata.y;
    s.materialClass = row.metadata.x;
    return s;
}

// Only a decoded ORM sidecar authorizes specular; class scalars and normal maps
// alone do not. Explicit legacy materials remain diffuse even with an ORM map.
bool propHasAuthoredOrm(int id) {
    PropMaterialGpu row = uPropMaterials.materials[id];
    return row.metadata.x != SURF_CLASS_LEGACY_PRELIT
        && row.material.metallicRoughnessTex != 0xffffffffu;
}

// ORM alpha is authored coverage, separate from the stock albedo's cutout alpha.
// Zero coverage keeps the legacy material response, including zero new specular.
float propMaterialCoverage(int id) {
    PropMaterialGpu row = uPropMaterials.materials[id];
    if (row.material.metallicRoughnessTex == 0xffffffffu) return 1.0;
    return texture(uPropOrm, vec3(vUv * row.uvScale, float(row.material.metallicRoughnessTex))).a;
}
