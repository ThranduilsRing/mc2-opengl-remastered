// Mech material INPUTS -> SurfaceMaterial (the schema props use). Imported BT2018 mechs feed AO and a
// tangent-space normal through the package feature bits (bit1 HAS_AO, bit2 HAS_NORMAL); stock mechs are
// legacy_prelit unless bit3 selects authored stock ORM in the existing AO binding.
// Requires vUv, vTangentGL, vTangentW, uMechAo, uMechNrm, uFrame (mech_tier1.frag) and mech_shading.hglsl.
#include "../../include/surface_material.hglsl"

vec4 sampleMechMaterial(sampler2D map, vec2 uv) {
#ifdef MECH_PREVIEW
    // Close-up panels retain the authored atlas detail, independent of the world mip bias.
    return textureLod(map, uv, 0.0);
#else
    return texture(map, uv);
#endif
}

SurfaceMaterial mechSurface(vec3 texRgb, vec3 geometricNormal, uint importedBits, bool alphaTest) {
    SurfaceMaterial s;
    s.baseColor = surfaceDecodeSrgb(texRgb);
    s.normal = geometricNormal;
    s.roughness = 1.0;
    s.metallic = 0.0;
    s.ao = 1.0;
    s.emission = vec3(0.0);
    s.alphaMode = alphaTest ? SURF_ALPHA_MASK : SURF_ALPHA_OPAQUE;
    s.materialClass = SURF_CLASS_LEGACY_PRELIT;
    if ((importedBits & 8u) != 0u) {
        vec3 orm = sampleMechMaterial(uMechAo, vUv).rgb;
        s.ao = orm.r;
        s.roughness = clamp(orm.g, 0.04, 1.0);
        s.metallic = orm.b;
        s.materialClass = SURF_CLASS_PAINTED_METAL;
    }
    else if ((importedBits & 2u) != 0u)
        s.ao = msh_aoFactor(sampleMechMaterial(uMechAo, vUv).r, uFrame.shadeC.x);
    if ((importedBits & 4u) != 0u && dot(vTangentGL, vTangentGL) > 1e-8)
        s.normal = msh_normalMapped(geometricNormal, vTangentGL, vTangentW, sampleMechMaterial(uMechNrm, vUv).xyz, (importedBits & 8u) != 0u ? 1.0 : uFrame.shadeC.y);
    return s;
}
