#ifndef MC2_LIGHTING_VK_GLSL
#define MC2_LIGHTING_VK_GLSL
float computeNdotLBand(vec3 N, vec3 lightDir)
{
    float NdotL   = dot(N, lightDir);
    float diffuse = clamp(NdotL, 0.02, 1.0);
    return mix(0.35, 1.20, diffuse);
}

// --- Reusable prop/mech PBR shade (ported from static_prop.frag:420-441). ----
// Schlick-Fresnel + power-lobe (Blinn-Phong-style) specular + Lambert diffuse,
// given surface frame vectors already in a COMMON space (the caller is
// responsible for space consistency — static_prop.frag converts N/L/V to GL
// world before calling; this function does NO space swap). All vectors are
// expected NORMALIZED. `metallic`/`roughness` are the MaterialGpu scalars
// (metallic default 0.0, roughness default 1.0 per docs/material-m0-contract.md).
// `lightColor` is the sun color; ambient is added by the caller.
//
// PARITY NOTE (VK prop Tier-1): static_prop.frag's DEFAULT dominant path is
// NOT this GGX block (that block is gated by u_pbrV1Strength, default 0.0).
// The GL default lit color is the vertex-baked v_argb (hot-color base_light +
// ObjectLights ambient + terrain-sun*NdotL) multiplied by the albedo texture.
// Those inputs (per-type magic SSBO, ObjectLights UBO, terrain sun) are NOT
// fed to the VK island. computePBR is provided as the SHARED, reusable lighting
// primitive (mechs consume the identical body next) and gives a physically
// plausible directional+specular term; it is NOT a byte-parity reproduction of
// the GL vertex-baked lighting. See island report.
struct LsvkPbrOut {
    vec3 diffuse;   // Lambert * NdotL * lightColor (albedo NOT yet applied)
    vec3 specular;  // GGX-ish spec * NdotL * lightColor
};

// Returns diffuse/specular light terms (albedo applied by caller for diffuse;
// F0 for specular already folds albedo via metallic). Mirrors the exact math in
// static_prop.frag:429-439 (smoothness = 1-roughness, specPower = mix(1,512,
// smoothness^2), Schlick F0 = mix(0.04, albedo, metallic)).
LsvkPbrOut computePBR(vec3 N, vec3 V, vec3 L, vec3 albedo,
                      float metallic, float roughness, vec3 lightColor)
{
    LsvkPbrOut o;
    vec3  H          = normalize(L + V);
    float NdotL      = max(0.0, dot(N, L));
    float NdotH      = max(0.0, dot(N, H));

    // Schlick-Fresnel with albedo-tinted F0 for metallics (static_prop.frag:430-433).
    const vec3 kF0Dielectric = vec3(0.04);
    vec3 F0 = mix(kF0Dielectric, albedo, metallic);
    vec3 F  = F0 + (vec3(1.0) - F0) * pow(max(0.0, 1.0 - max(0.0, dot(N, V))), 5.0);

    // Power-lobe specular (static_prop.frag:435-439).
    float smoothness = 1.0 - roughness;
    float specPower  = mix(1.0, 512.0, smoothness * smoothness);
    o.specular       = lightColor * F * pow(NdotH, specPower) * NdotL;

    // Lambert diffuse (energy-split by (1-F), non-metal only). Diffuse albedo is
    // applied by the caller so the same function serves props (albedo texture)
    // and mechs (material albedo) without re-tinting here.
    vec3 kd  = (vec3(1.0) - F) * (1.0 - metallic);
    o.diffuse = lightColor * kd * NdotL;
    return o;
}

// --- Baked static sun shadow (shadow.hglsl:33-78, calcShadow; receivers use one 4-tap textureGather + bilinear mix, not 16 taps). -------
// Native receiver filter; projection/bias retain the reference convention. `shadowMap` is a sampler2DShadow bound to a depth-compare image
// (HW LINEAR PCF, compareOp=LESS_OR_EQUAL, CLAMP_TO_BORDER white=1.0). `lightSpaceMatrix`
// is the STATIC light-space matrix (row-major GL_FALSE upload; z already [0,1]
// ZERO_TO_ONE light ortho, so NO *0.5+0.5 on Z). Returns visibility in [0.4, 1.0].
// `shadowSoftness` = penumbra radius in texels (GL default 2.5). Fail-soft: a caller
// that has no map should pass enableShadows=0 (or just skip the call) -> 1.0.

#endif
