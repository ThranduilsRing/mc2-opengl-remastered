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
// Per-object maps (kMatFlagPerTexture rows): every file is its own image with its own extent and mip chain.
// Slots are written only for textures a material names (partially bound); a row indexes this table.
layout(constant_id = 1) const int kPropTexSlots = 1024;
layout(set = 0, binding = 13) uniform sampler2D uPropTex[kPropTexSlots];
// kPropTexSlots == 1 means no per-object maps (Low/Potato, or a GPU with tight sampler limits such as the HD 630's 64):
// the array shrinks to one entry and the path folds away.
const bool kPerTexMaps = kPropTexSlots > 1;
layout(set = 0, binding = 12, std430) readonly buffer PropMaterials {
    PropMaterialGpu materials[];
} uPropMaterials;

vec3 propDecodeColor(vec3 c) { return surfaceDecodeSrgb(c); }
vec3 propEncodeColor(vec3 c) { return surfaceEncodeSrgb(c); }

// Derivatives use continuous detail UVs (before repeat), world positions and a
// render-space normal. Solving both axes preserves mirrored UV handedness.
vec3 propDetailNormal(vec3 geometric, vec2 uv, vec3 sampled, float strength) {
    // vWorldPos and geometric already share the vertex shader's Stuff-to-world swizzle.
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

// coverage = the ORM map's alpha: authored coverage, separate from the stock albedo's cutout alpha
// (zero keeps the legacy material response, including zero new specular); 1 without an ORM map.
// It comes from the same fetch as orm.rgb, so the material path samples the ORM texture once.
SurfaceMaterial propSurface(int id, vec3 albedo, out float coverage) {
    PropMaterialGpu row = uPropMaterials.materials[id];
    vec2 uv = vUv * row.uvScale;
    SurfaceMaterial s;
    s.baseColor = propDecodeColor(albedo);
    s.normal = normalize(mc2ToRender(vNormalStuff));
    const bool perTex = kPerTexMaps && (row.material.flags & kMatFlagPerTexture) != 0u;
    if (row.material.normalTex != 0xffffffffu) {
        vec3 sampleN = perTex ? texture(uPropTex[nonuniformEXT(row.material.normalTex)], uv).rgb
                              : texture(uPropNormal, vec3(uv, float(row.material.normalTex))).rgb;
        if (row.metadata.z != 0u) {
            vec2 xy = sampleN.xy * 2.0 - 1.0;
            sampleN.z = 0.5 + 0.5 * sqrt(max(0.0, 1.0 - dot(xy, xy)));
        }
        s.normal = propDetailNormal(s.normal, uv, sampleN, row.normalStrength);
    }
    // No ORM map: the class scalars (row factors) with neutral AO and full coverage.
    vec4 orm = vec4(1);
    if (row.material.metallicRoughnessTex != 0xffffffffu)
        orm = perTex ? texture(uPropTex[nonuniformEXT(row.material.metallicRoughnessTex)], uv)
                     : texture(uPropOrm, vec3(uv, float(row.material.metallicRoughnessTex)));
    coverage = orm.a;
    s.ao = mix(1.0, orm.r, row.aoStrength);
    s.roughness = clamp(orm.g * row.material.roughnessFactor, 0.3, 1.0);
    s.metallic = clamp(orm.b * row.material.metallicFactor, 0.0, 1.0);
    // Ordinary baked vertex light is a lighting input, not material emission.
    // Window nodes and authored lamp materials supply emitted radiance separately.
    s.emission = (vMaterialFlags & 2u) != 0u ? s.baseColor * propDecodeColor(vMaterialSeed)
        : row.metadata.x == SURF_CLASS_EMISSIVE && (vMaterialFlags & 1u) == 0u
            ? s.baseColor * uFrame.nightDarkness : vec3(0);
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

SurfaceMaterial propSurface(int id, vec3 albedo) {
    float coverage;
    return propSurface(id, albedo, coverage);
}

bool propHasObjectAlbedo(int id) {
    if (id < 0 || id >= uPropMaterials.materials.length()) return false;
    PropMaterialGpu row = uPropMaterials.materials[id];
    return kPerTexMaps && (row.material.flags & kMatFlagPerTexture) != 0u && row.material.albedoTex != 0xffffffffu;
}

// Authored mask R/G/B select base/highlight1/highlight2. Stock mission setup passes
// those colours as psBlue/psRed/psGreen (mission_movers.cpp), not in mask RGB order.
// under the mask the albedo keeps its own relative luminance, stripes and dirt and only its chroma follows the team.
vec3 propTeamTint(int id, vec3 albedo, uvec3 team) {
    if (id < 0 || id >= uPropMaterials.materials.length()) return albedo;
    PropMaterialGpu row = uPropMaterials.materials[id];
    if (team == uvec3(0u)) return albedo;
    if ((row.material.flags & kMatFlagTeamMask) == 0u || row.material.emissiveTex == 0xffffffffu) {
        // Unmasked vehicle kits bake the stock key colours into the albedo.
        // Keep one registered body: late CPU palette variants have no GPU recipe.
        if (row.metadata.x == SURF_CLASS_GLASS || row.metadata.x == SURF_CLASS_EMISSIVE) return albedo;
        int channel = albedo.r >= albedo.g && albedo.r >= albedo.b ? 0 : (albedo.g >= albedo.b ? 1 : 2);
        float dominant = albedo[channel];
        float other = max(albedo[(channel + 1) % 3], albedo[(channel + 2) % 3]);
        if (dominant < 16.0 / 255.0 || dominant < 1.5 * other) return albedo;
        uint packed = team[channel]; // stock R/G/B -> psRed/psGreen/psBlue
        vec3 paint = vec3((packed >> 16) & 255u, (packed >> 8) & 255u, packed & 255u) / 255.0;
        float shade = 1.0 - (1.0 - dominant) * (1.0 - dominant);
        float cover = clamp((dominant / (other + 1.0 / 255.0) - 1.5) / 0.15, 0.0, 1.0);
        return mix(albedo, paint * shade, cover);
    }
    vec3 w = texture(uPropTex[nonuniformEXT(row.material.emissiveTex)], vUv * row.uvScale).rgb;
    float cover = clamp(w.r + w.g + w.b, 0.0, 1.0);
    if (cover <= 0.0) return albedo;
    vec3 red   = vec3((team.x >> 16) & 255u, (team.x >> 8) & 255u, team.x & 255u) / 255.0;
    vec3 green = vec3((team.y >> 16) & 255u, (team.y >> 8) & 255u, team.y & 255u) / 255.0;
    vec3 blue  = vec3((team.z >> 16) & 255u, (team.z >> 8) & 255u, team.z & 255u) / 255.0;
    vec3 paint = (w.r * blue + w.g * red + w.b * green) / max(w.r + w.g + w.b, 1e-4);
    const vec3 kLum = vec3(0.299, 0.587, 0.114);
    // The albedo's luminance relative to a typical painted panel (0.3 encoded) scales the paint, so panel shading,
    // stripes and dirt survive and an average panel lands on the paint colour itself.
    vec3 tinted = clamp(paint * (dot(albedo, kLum) / 0.3), 0.0, 1.0);
    return mix(albedo, tinted, cover);
}

// A per-object albedo replaces the tiered tgl texture of its packet (full resolution, own mips, not chosen by
// ObjectTextureRes). Returns false when the material names none.
bool propAlbedoOverride(int id, out vec3 albedo) {
    albedo = vec3(0);
    if (id < 0 || id >= uPropMaterials.materials.length()) return false;
    PropMaterialGpu row = uPropMaterials.materials[id];
    if (!kPerTexMaps || (row.material.flags & kMatFlagPerTexture) == 0u || row.material.albedoTex == 0xffffffffu) return false;
    albedo = texture(uPropTex[nonuniformEXT(row.material.albedoTex)], vUv * row.uvScale).rgb;
    return true;
}
