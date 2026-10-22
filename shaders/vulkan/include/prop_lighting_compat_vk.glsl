// OWNED BY THE LIGHTING LANE (R2/R5). Pre-L6 compatibility composition for authored
// static-prop materials: SurfaceMaterial (prop_material_vk.glsl) + the vertex light terms
// -> display colour. The ambient/hemisphere/SH fills here are the stacked terms R2 removes;
// material inputs must not be added here and composition must not move into the inputs.
vec3 propAuthoredColor(SurfaceMaterial s, bool alphaTest) {
    if ((vMaterialFlags & 2u) != 0u)
        return propEncodeColor(s.baseColor * s.emission) + vHighlight.rgb * vHighlight.a;
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
