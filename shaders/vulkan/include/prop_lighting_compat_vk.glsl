// Per-pixel static-prop PBR: one SH ambient, shared GGX sun and local pools.
#include "prop_night_vk.glsl"
vec3 propAddWindowEmission(vec3 encodedColor, vec3 emission) {
    return propEncodeColor(propDecodeColor(encodedColor) + emission);
}
// Reuse the legacy path's pool walk when blending authored coverage.
vec3 propAuthoredColor(SurfaceMaterial s, vec3 poolLight) {
    if ((vMaterialFlags & 2u) != 0u)
        return propEncodeColor(s.emission) + vHighlight.rgb * vHighlight.a;   // window emission already carries the albedo
    vec3 nStuff = renderToMc2(s.normal);
    vec3 L = normalize(uFrame.terrainSunMC2.xyz);
    bool twoSided = (vMaterialFlags & 4u) != 0u;
    vec3 V = uFrame.cameraWorldPos.xyz - vWorldPos;
    V = dot(V,V) > 1e-8 ? normalize(V) : s.normal;
    vec3 N = twoSided && dot(s.normal,V) < 0.0 ? -s.normal : s.normal;
    // R2: the mission environment SH is the only ambient. ObjectLights ambient and the fixed
    // prop hemispheres no longer stack on top of it.
    vec3 sh[9];
    for (int k=0; k<9; ++k) sh[k] = uFrame.iblSh[k].xyz;
    vec3 ibl = uFrame.iblShStrength > 0.0 ? evalShL2(normalize(nStuff), sh)*uFrame.iblShStrength : vec3(0);
    // Some stock night skies have almost no SH irradiance. Use the mission's
    // Camera ambient/floor and the stock night-dimmed environment bake as a
    // lower bound, replacing missing SH energy rather than stacking a fill.
    vec3 missionAmbient = propDecodeColor(clamp(max(uFrame.ambientColor.rgb,
        vec3(uFrame.ambientColor.w))/255.0,0.0,1.0)) * 3.14159265 * uFrame.nightDarkness;
    missionAmbient = max(missionAmbient, propDecodeColor(vMaterialSeed)
        * (3.14159265 * uFrame.nightDarkness));
    ibl = max(ibl,missionAmbient);
    // No baked vertex-light seed, hemisphere or cutout light floor on top of SH.
    // AO occludes indirect only. Sun shadows and scene tonemap remain in the
    // common downstream passes, exactly once, with the existing GBuffer class.
    vec3 F0 = mix(vec3(0.04),s.baseColor,s.metallic);
    float nv = clamp(dot(N,V),0.0,1.0);
    vec3 F = F0 + (max(vec3(1.0-s.roughness),F0)-F0)*pow(1.0-nv,5.0);
    vec3 diffuseWeight = (vec3(1)-F)*(1.0-s.metallic);
    vec3 color = s.baseColor * diffuseWeight * max(ibl,vec3(0)) * s.ao / 3.14159265;
    // The mission supplies diffuse SH, not a prefiltered specular cubemap. For
    // these rough surfaces use its broad reflected lobe as the environment
    // approximation, fading toward diffuse irradiance with roughness. Metals
    // retain environment reflection even when their diffuse response is zero.
    vec3 reflected = uFrame.iblShStrength > 0.0
        ? evalShL2(renderToMc2(reflect(-V,N)),sh)*uFrame.iblShStrength : vec3(0);
    reflected = max(reflected,missionAmbient);
    color += mix(max(reflected,vec3(0)),max(ibl,vec3(0)),s.roughness*s.roughness)
           * F * s.ao / 3.14159265;
    LsvkPbrOut sun = computePBR(N,V,L,s.baseColor,s.metallic,s.roughness,
                              propDecodeColor(vMaterialSun));
    color += (s.baseColor*sun.diffuse + sun.specular) * propNightDirectShare();
    float nightFill = propNightSunWeight(0.0, N);
    color += s.baseColor * diffuseWeight * propDecodeColor(vMaterialSun) * nightFill / 3.14159265;
    // Pool attenuation/compression already returns a linear light weight.
    color += s.baseColor * max(poolLight,vec3(0));
    color += propDecodeColor(vHighlight.rgb);
    return propEncodeColor(color) + vHighlight.rgb*vHighlight.a;
}
