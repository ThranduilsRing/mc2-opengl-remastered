// Mech material INPUTS -> SurfaceMaterial (the schema props use). Imported BT2018 mechs feed AO and a
// tangent-space normal through the package feature bits (bit1 HAS_AO, bit2 HAS_NORMAL); stock mechs are
// legacy_prelit. No authored ORM/emissive exists for mechs yet (BT2018 plan P11/P12), so roughness,
// metallic and emission stay neutral and no specular may be derived from these inputs.
// Requires vUv, vTangentGL, vTangentW, uMechAo, uMechNrm, uFrame (mech_tier1.frag) and mech_shading.hglsl.
#include "../../include/surface_material.hglsl"

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
    if ((importedBits & 2u) != 0u)
        s.ao = msh_aoFactor(textureLod(uMechAo, vUv, 0.0).r, uFrame.shadeC.x);
    if ((importedBits & 4u) != 0u && dot(vTangentGL, vTangentGL) > 1e-8)
        s.normal = msh_normalMapped(geometricNormal, vTangentGL, vTangentW, textureLod(uMechNrm, vUv, 0.0).xyz, uFrame.shadeC.y);
    return s;
}
