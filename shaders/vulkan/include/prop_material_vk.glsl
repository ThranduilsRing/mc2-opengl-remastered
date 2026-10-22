#include "../../include/surface_material.hglsl"
#include "../../include/material_gpu.hglsl"
struct PropMaterialGpu {
    MaterialGpu material;
    vec2 uvScale;
    float normalStrength;
    float aoStrength;
    uvec4 metadata;
};
layout(set = 0, binding = 10) uniform sampler2DArray uPropNormal;
layout(set = 0, binding = 11) uniform sampler2DArray uPropOrm;
layout(set = 0, binding = 12, std430) readonly buffer PropMaterials {
    PropMaterialGpu materials[];
} uPropMaterials;

vec3 propDecodeColor(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}
vec3 propEncodeColor(vec3 c) {
    c = max(c, vec3(0));
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0/2.4)) - 0.055, greaterThan(c, vec3(0.0031308)));
}

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
    vec3 orm = vec3(1);
    if (row.material.metallicRoughnessTex != 0xffffffffu)
        orm = texture(uPropOrm, vec3(uv, float(row.material.metallicRoughnessTex))).rgb;
    s.ao = mix(1.0, orm.r, row.aoStrength);
    s.roughness = clamp(orm.g * row.material.roughnessFactor, 0.3, 1.0);
    s.metallic = clamp(orm.b * row.material.metallicFactor, 0.0, 1.0);
    // Ordinary baked vertex light is a lighting input, not material emission.
    // Window nodes expose their emitted radiance directly for the future L6 consumer.
    s.emission = (vMaterialFlags & 2u) != 0u ? s.baseColor * propDecodeColor(vMaterialSeed) : vec3(0);
    s.materialClass = row.metadata.x;
    return s;
}

// ORM alpha is authored coverage, separate from the stock albedo's cutout alpha.
// Zero coverage keeps the legacy material response, including zero new specular.
float propMaterialCoverage(int id) {
    PropMaterialGpu row = uPropMaterials.materials[id];
    if (row.material.metallicRoughnessTex == 0xffffffffu) return 1.0;
    return texture(uPropOrm, vec3(vUv * row.uvScale, float(row.material.metallicRoughnessTex))).a;
}

// Compatibility lighting adapter for authored props only. Material evaluation
// above stays independent of this pre-L6 lighting/output convention.
vec3 propAuthoredColor(SurfaceMaterial s, bool alphaTest) {
    if ((vMaterialFlags & 2u) != 0u)
        return propEncodeColor(s.emission) + vHighlight.rgb * vHighlight.a;
    vec3 nStuff = renderToMc2(s.normal);
    vec3 L = normalize(uFrame.terrainSunMC2.xyz);
    float raw = dot(s.normal, L);
    bool twoSided = (vMaterialFlags & 4u) != 0u;
    float ndl = twoSided ? abs(raw) : max(raw, 0.0);
    float wrap = twoSided ? 1.0 : mix(0.55, 1.0, 0.5 + 0.5 * raw);
    vec3 ambient = vMaterialAmbient;
    if (uFrame.lightingReady == 0)
        ambient = mix(vec3(0.14,0.13,0.11), vec3(0.00,0.02,0.08), clamp(0.5+0.5*nStuff.y,0.0,1.0));
    vec3 hemi = mix(vec3(0.10,0.09,0.07), vec3(0.20,0.22,0.28), 0.5+0.5*nStuff.y) * uFrame.ambientV1Strength;
    vec3 sh[9];
    for (int k=0; k<9; ++k) sh[k] = uFrame.iblSh[k].xyz;
    vec3 ibl = uFrame.iblShStrength > 0.0 ? evalShL2(normalize(nStuff), sh)*uFrame.iblShStrength : vec3(0);
    // AO affects ambient fill only; emission, direct sun and local pools survive.
    vec3 lit = vMaterialSeed * wrap + (ambient*wrap + hemi + ibl)*s.ao + vMaterialSun*ndl;
    lit = clamp(clamp(lit,0.0,1.0) + vHighlight.rgb,0.0,1.0);
    if (alphaTest) lit = max(lit,vec3(0.5));
    vec3 color = s.baseColor * propDecodeColor(lit) * (1.0 - 0.65*s.metallic);
    vec3 V = uFrame.cameraWorldPos.xyz - vWorldPos;
    if (dot(V,V) > 1e-8 && dot(vSunColor,vSunColor) > 0.0) {
        V = normalize(V);
        vec3 H = L + V;
        if (dot(H,H) > 1e-8) {
            H = normalize(H);
            float nh = max(dot(s.normal,H),0.0), nv = max(dot(s.normal,V),0.0);
            float a2 = pow(s.roughness,4.0);
            float d = nh*nh*(a2-1.0)+1.0;
            float D = a2 / max(3.14159265*d*d,1e-5);
            float k = (s.roughness+1.0)*(s.roughness+1.0)/8.0;
            float G = nv/(nv*(1.0-k)+k) * ndl/(ndl*(1.0-k)+k);
            vec3 F0 = mix(vec3(0.04),s.baseColor,s.metallic);
            vec3 F = F0 + (1.0-F0)*pow(1.0-clamp(dot(V,H),0.0,1.0),5.0);
            color += propDecodeColor(vSunColor)*D*G*F*ndl/max(4.0*nv*ndl,1e-4);
        }
    }
    return propEncodeColor(color) + vHighlight.rgb*vHighlight.a + propPoolLight(propEncodeColor(s.baseColor));
}
