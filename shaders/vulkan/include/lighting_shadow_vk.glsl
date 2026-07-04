// lighting_shadow_vk.glsl — REUSABLE VK GLSL lighting/shadow helper.
// =============================================================================
// VK-native port of the GL terrain lighting math (terrain_lod_chunk.frag:889-928)
// + the baked-shadow PCF (shaders/include/shadow.hglsl:33-78, calcShadow). Written
// as PARAMETERIZED free functions (no global uniforms) so props/mechs can #include
// this same file later and call just the parts they need:
//
//   * computeNdotLBand(N, lightDir)                         — the diffuse relief band.
//     Cheap, sampler-free. Props/mechs reuse THIS immediately (no shadow map needed).
//   * computeBakedShadow(worldPos, lightDir, sMap, lsMat)   — the 16-tap Poisson PCF
//     against a sampler2DShadow (HW LINEAR compare, compareOp=LESS_OR_EQUAL). The
//     caller passes the depth-compare sampler + light-space matrix as arguments, so
//     the same body serves terrain today and props/mechs later against THEIR maps.
//
// PARITY CONTRACT: both functions reproduce the GL math VERBATIM. The ONLY VK-specific
// difference is that `sampler2DShadow` + its matrix are function ARGUMENTS instead of
// file-scope `uniform`s (GL shadow.hglsl declares them global; VK descriptor sets bind
// them and main() forwards them in). The numeric result is identical.
//
// Consumers MUST enable GL_GOOGLE_include_directive and #include this after their
// #version. This file declares NO uniforms, NO bindings, NO in/out — it is pure math
// so it composes into any VK descriptor layout.
// =============================================================================
#ifndef LIGHTING_SHADOW_VK_GLSL
#define LIGHTING_SHADOW_VK_GLSL

// 16-sample Poisson disk — BIT-IDENTICAL to shaders/include/shadow.hglsl:10-27.
const vec2 lsvk_poissonDisk[16] = vec2[](
    vec2(-0.94201624, -0.39906216),
    vec2( 0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870),
    vec2( 0.34495938,  0.29387760),
    vec2(-0.91588581,  0.45771432),
    vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845),
    vec2( 0.97484398,  0.75648379),
    vec2( 0.44323325, -0.97511554),
    vec2( 0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023),
    vec2( 0.79197514,  0.19090188),
    vec2(-0.24188840,  0.99706507),
    vec2(-0.81409955,  0.91437590),
    vec2( 0.19984126,  0.78641367),
    vec2( 0.14383161, -0.14100790)
);

// --- Diffuse relief band (terrain_lod_chunk.frag:891-893). ------------------
// diffuse = clamp(dot(N,lightDir), 0.02, 1.0); band = mix(0.35, 1.20, diffuse).
// Props/mechs can call this alone (no shadow map) for the sun relief term.
float computeNdotLBand(vec3 N, vec3 lightDir)
{
    float NdotL   = dot(N, lightDir);
    float diffuse = clamp(NdotL, 0.02, 1.0);
    return mix(0.35, 1.20, diffuse);
}

// --- Baked static sun shadow (shadow.hglsl:33-78, calcShadow, 16-tap). -------
// VERBATIM port. `shadowMap` is a sampler2DShadow bound to a depth-compare image
// (HW LINEAR PCF, compareOp=LESS_OR_EQUAL, CLAMP_TO_BORDER white=1.0). `lightSpaceMatrix`
// is the STATIC light-space matrix (row-major GL_FALSE upload; z already [0,1]
// ZERO_TO_ONE light ortho, so NO *0.5+0.5 on Z). Returns visibility in [0.4, 1.0].
// `shadowSoftness` = penumbra radius in texels (GL default 2.5). Fail-soft: a caller
// that has no map should pass enableShadows=0 (or just skip the call) -> 1.0.
float computeBakedShadow(vec3 worldPos, vec3 normal, vec3 lightDir,
                         sampler2DShadow shadowMap, mat4 lightSpaceMatrix,
                         float shadowSoftness, int numTaps)
{
    vec4 lsPos = lightSpaceMatrix * vec4(worldPos, 1.0);
    vec3 projCoords = lsPos.xyz / lsPos.w;
    projCoords.xy = projCoords.xy * 0.5 + 0.5;  // z already [0,1] (ZERO_TO_ONE light ortho)

    // Outside shadow map — fully lit.
    if (projCoords.z > 1.0 || projCoords.z < 0.0) return 1.0;
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0) return 1.0;

    // Back-face guard.
    float NdotL = max(dot(normal, lightDir), 0.0);
    if (NdotL < 0.05) return 1.0;

    // Slope-dependent depth bias.
    float bias = max(0.005 * (1.0 - NdotL), 0.002);
    float currentDepth = projCoords.z - bias;

    // Per-pixel rotation to break banding (stratified sampling).
    float angle = 6.2831853 * fract(sin(dot(worldPos.xz, vec2(12.9898, 78.233))) * 43758.5453);
    float ca = cos(angle), sa = sin(angle);
    mat2 rot = mat2(ca, sa, -sa, ca);

    // Adaptive PCF radius based on light-space depth gradient.
    float depthGradient = length(vec2(dFdx(projCoords.z), dFdy(projCoords.z)));
    float hardness = clamp(depthGradient * 180.0, 0.0, 1.0);
    float adaptiveScale = mix(2.4, 0.8, hardness);

    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));
    float radius = max(shadowSoftness, 0.5) * adaptiveScale;
    float shadow = 0.0;
    int taps = clamp(numTaps, 1, 16);
    for (int i = 0; i < taps; i++) {
        vec2 offset = rot * lsvk_poissonDisk[i] * radius * texelSize;
        shadow += texture(shadowMap, vec3(projCoords.xy + offset, currentDepth));
    }
    shadow /= float(taps);

    return mix(0.4, 1.0, shadow);
}

#endif // LIGHTING_SHADOW_VK_GLSL
