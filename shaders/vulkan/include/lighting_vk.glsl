#ifndef MC2_LIGHTING_VK_GLSL
#define MC2_LIGHTING_VK_GLSL
float computeNdotLBand(vec3 N, vec3 lightDir)
{
    float NdotL   = dot(N, lightDir);
    float diffuse = clamp(NdotL, 0.02, 1.0);
    return mix(0.35, 1.20, diffuse);
}

// --- Shared prop/mech PBR: GGX + Schlick-Smith + Lambert diffuse. ---------
// given surface frame vectors already in a COMMON space (the caller is
// responsible for space consistency — static_prop.frag converts N/L/V to GL
// world before calling; this function does NO space swap). All vectors are
// expected NORMALIZED. `metallic`/`roughness` are the MaterialGpu scalars
// (metallic default 0.0, roughness default 1.0 per docs/material-m0-contract.md).
// `lightColor` is the sun color; ambient is added by the caller.
//
// Props use both terms; mechs reuse the specular term alongside their existing
// diffuse composition. Environment irradiance is composed once by the caller.
struct LsvkPbrOut {
    vec3 diffuse;   // Lambert * NdotL * lightColor (albedo NOT yet applied)
    vec3 specular;  // Cook-Torrance GGX * NdotL * lightColor
};

// Returns diffuse/specular light terms (albedo applied by caller for diffuse;
// F0 for specular already folds albedo via metallic).
LsvkPbrOut computePBR(vec3 N, vec3 V, vec3 L, vec3 albedo,
                      float metallic, float roughness, vec3 lightColor)
{
    LsvkPbrOut o;
    vec3  H          = L + V;
    H *= inversesqrt(max(dot(H,H), 1e-8));
    float NdotL      = max(0.0, dot(N, L));
    float NdotH      = max(0.0, dot(N, H));
    float NdotV      = max(0.0, dot(N, V));

    // Schlick-Fresnel with albedo-tinted F0 for metallics (static_prop.frag:430-433).
    const vec3 kF0Dielectric = vec3(0.04);
    vec3 F0 = mix(kF0Dielectric, albedo, metallic);
    vec3 F  = F0 + (vec3(1.0) - F0) * pow(1.0 - clamp(dot(V, H), 0.0, 1.0), 5.0);

    // Cook-Torrance GGX + Schlick-Smith masking. Shared by props and mechs.
    float r = clamp(roughness, 0.08, 1.0);
    float a2 = r*r*r*r;
    float d = NdotH*NdotH*(a2-1.0)+1.0;
    float D = a2 / max(3.14159265*d*d, 1e-8);
    float k = (r+1.0)*(r+1.0)/8.0;
    float G = NdotV/max(NdotV*(1.0-k)+k,1e-5)
            * NdotL/max(NdotL*(1.0-k)+k,1e-5);
    o.specular = lightColor * F * D * G * NdotL / max(4.0*NdotV*NdotL, 1e-5);

    // Lambert diffuse (energy-split by (1-F), non-metal only). Diffuse albedo is
    // applied by the caller so the same function serves props (albedo texture)
    // and mechs (material albedo) without re-tinting here.
    vec3 kd  = (vec3(1.0) - F) * (1.0 - metallic);
    o.diffuse = lightColor * kd * NdotL / 3.14159265;
    return o;
}

// --- Baked static sun shadow (shadow.hglsl:33-78, calcShadow, 16-tap). -------
// Native receiver filter; projection/bias retain the reference convention. `shadowMap` is a sampler2DShadow bound to a depth-compare image
// (HW LINEAR PCF, compareOp=LESS_OR_EQUAL, CLAMP_TO_BORDER white=1.0). `lightSpaceMatrix`
// is the STATIC light-space matrix (row-major GL_FALSE upload; z already [0,1]
// ZERO_TO_ONE light ortho, so NO *0.5+0.5 on Z). Returns visibility in [0.4, 1.0].
// `shadowSoftness` = penumbra radius in texels (GL default 2.5). Fail-soft: a caller
// that has no map should pass enableShadows=0 (or just skip the call) -> 1.0.

#endif
