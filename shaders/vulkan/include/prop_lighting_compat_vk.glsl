// Static-prop compatibility lighting: one environment ambient, directional light
// and local pools. Specular requires a valid authored ORM sidecar.
vec3 propAuthoredColor(SurfaceMaterial s, bool alphaTest, bool authoredOrm) {
    if ((vMaterialFlags & 2u) != 0u)
        return propEncodeColor(s.baseColor * s.emission) + vHighlight.rgb * vHighlight.a;
    vec3 nStuff = renderToMc2(s.normal);
    vec3 L = normalize(uFrame.terrainSunMC2.xyz);
    float raw = dot(s.normal, L);
    bool twoSided = (vMaterialFlags & 4u) != 0u;
    float ndl = twoSided ? abs(raw) : max(raw, 0.0);
    float wrap = twoSided ? 1.0 : mix(0.55, 1.0, 0.5 + 0.5 * raw);
    // R2: the mission environment SH is the only ambient. ObjectLights ambient and the fixed
    // prop hemispheres no longer stack on top of it.
    vec3 sh[9];
    for (int k=0; k<9; ++k) sh[k] = uFrame.iblSh[k].xyz;
    vec3 ibl = uFrame.iblShStrength > 0.0 ? evalShL2(normalize(nStuff), sh)*uFrame.iblShStrength : vec3(0);
    // AO affects ambient fill only; emission, direct sun and local pools survive.
    // SH is irradiance; legacy sun/seed/highlight inputs are authored sRGB.
    // Sum in linear light and apply Lambert once, never decode the sum.
    vec3 lit = s.emission * wrap
        + (max(ibl,vec3(0))*s.ao + propDecodeColor(vMaterialSun)*ndl) / 3.14159265
        + propDecodeColor(vHighlight.rgb);
    if (alphaTest) lit = max(lit,vec3(0.21404114)); // legacy 0.5 encoded floor
    vec3 color = s.baseColor * lit * (1.0 - 0.65*s.metallic);
    vec3 V = uFrame.cameraWorldPos.xyz - vWorldPos;
    if (authoredOrm && dot(V,V) > 1e-8 && dot(vSunColor,vSunColor) > 0.0) {
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
