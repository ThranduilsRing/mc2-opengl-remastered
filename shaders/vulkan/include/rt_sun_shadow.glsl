// Opaque non-swaying props only (scene::kMaskSunProp). Raster retains all other casters.
#ifndef MC2_RT_SUN_SET
#define MC2_RT_SUN_SET 1
#endif
layout(set=MC2_RT_SUN_SET, binding=0) uniform accelerationStructureEXT sunPropTlas;
float rtPropSunVisibility(vec3 worldPos, vec3 N, vec3 sun) {
    if (sunShadowFloor(0.4) >= 0.999 || dot(N, sun) <= 0.0) return 1.0;
    rayQueryEXT query;
    rayQueryInitializeEXT(query, sunPropTlas,
        gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsCullNoOpaqueEXT,
        0x04, worldPos + N * 0.5, 0.5, normalize(sun), 512.0);
    while (rayQueryProceedEXT(query)) {}
    return rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}
