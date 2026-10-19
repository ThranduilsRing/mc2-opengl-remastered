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
// Lighting and bias follow the GL reference; native receivers use dense tent PCF. The ONLY VK-specific
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
#include "shadow_filter_vk.glsl"

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

// --- Baked static sun shadow (shadow.hglsl:33-78, calcShadow, 16-tap). -------
// Native receiver filter; projection/bias retain the reference convention. `shadowMap` is a sampler2DShadow bound to a depth-compare image
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

    // A fixed receiver kernel avoids unfiltered per-pixel noise on thin palm shadows.
    float angle = 0.0;
    float ca = cos(angle), sa = sin(angle);
    mat2 rot = mat2(ca, sa, -sa, ca);

    // Adaptive PCF radius based on light-space depth gradient.
    float depthGradient = length(vec2(dFdx(projCoords.z), dFdy(projCoords.z)));
    float hardness = clamp(depthGradient * 180.0, 0.0, 1.0);
    float adaptiveScale = mix(2.4, 0.8, hardness);

    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));
    float radius = max(shadowSoftness, 0.5) * adaptiveScale;
    float shadow = mc2ShadowTent(shadowMap, vec3(projCoords.xy, currentDepth), radius);

    return mix(0.4, 1.0, shadow);
}

// =============================================================================
// VK-PARITY-SHADOW-1 (gap #4) -- GL shadow.hglsl calcShadow / calcDynamicShadow
// (+ csmSampleLayer / csmSampleFullMap) sampling math INCLUDING the post-merge
// series: TERRAIN-SHADOW-GUARD-1 clamped-sun, STATIC-SHADOW-BANDING-2 (V2)
// normal-offset receiver, SHADOW-DIST-FADE-1 + FADE-CONT + RAMP0 + DONUT-FIX +
// ALT-SCALE. The pure-math pieces (clamp / gains / fade curve / ladder jitter)
// are the SHARED shaders/include/shadow_fade_model.hglsl that GL shadow.hglsl
// also includes; the structure below is a line-for-line port of the GL bodies
// with the GL compile-time macros replaced by LsvkShadowKnobs (fed from the same
// C++ producer GL injects its #defines from -- mc2ShadowFadeKnobs()).
// The pre-existing computeBakedShadow() above is left untouched (legacy callers).
// =============================================================================
#include "../../include/shadow_fade_model.hglsl"

struct LsvkShadowKnobs {
    float distFadeWu;   // MC2_SHADOW_DIST_FADE_WU (0 -> whole fade block folds away)
    float distFadeMax;  // MC2_SHADOW_DIST_FADE_MAX_F
    float minSunSin;    // MC2_SHADOW_MIN_SUN_SIN (0 -> clamp is the identity)
    int   flags;        // bit0 FADE_CONT, bit1 FADE_RAMP0, bit2 ALT_SCALE, bit3 BANDFIX_V2
};
#define LSVK_KNOB_FADE_CONT  1
#define LSVK_KNOB_FADE_RAMP0 2
#define LSVK_KNOB_ALT_SCALE  4
#define LSVK_KNOB_BANDFIX_V2 8

// TERRAIN-SHADOW-GUARD-1: the sun the shadow MATRICES were built from.
vec3 lsvk_shadowSun(vec3 lightDir, LsvkShadowKnobs k)
{
    return mc2ShadowClampSunAltitudeP(lightDir, k.minSunSin);
}

// SHADOW-DIST-FADE-1 / FADE-CONT / DONUT-FIX: max-over-4-probes occluder-distance
// estimate (dNorm ~ occluder ray distance / fade distance). 2D compare sampler.
float lsvk_occluderDist2D(sampler2DShadow smap, vec2 xy, mat2 rot, vec2 searchR,
                          float refDepth, float fadeNdc, float angle, bool cont)
{
    float occP = 0.0;
    float jit  = mc2ShadowLadderJitter(angle);
    int nSteps = cont ? 8 : 4;
    for (int p = 0; p < 4; ++p) {
        vec2 uv = xy + rot * lsvk_poissonDisk[p] * searchR;
        float o = 0.0;
        for (int q = 0; q < nSteps; ++q) {
            float stepN = cont ? mc2ShadowLadderStepN(q, jit) : 0.25 * float(q + 1);
            o += 1.0 - texture(smap, vec3(uv, refDepth - fadeNdc * stepN));
        }
        occP = max(occP, o / float(nSteps));
    }
    return occP;
}
// Same, array-layer sampler.
float lsvk_occluderDistArray(sampler2DArrayShadow smap, vec2 xy, float layer, mat2 rot,
                             vec2 searchR, float refDepth, float fadeNdc, float angle, bool cont)
{
    float occP = 0.0;
    float jit  = mc2ShadowLadderJitter(angle);
    int nSteps = cont ? 8 : 4;
    for (int p = 0; p < 4; ++p) {
        vec2 uv = xy + rot * lsvk_poissonDisk[p] * searchR;
        float o = 0.0;
        for (int q = 0; q < nSteps; ++q) {
            float stepN = cont ? mc2ShadowLadderStepN(q, jit) : 0.25 * float(q + 1);
            o += 1.0 - texture(smap, vec4(uv, layer, refDepth - fadeNdc * stepN));
        }
        occP = max(occP, o / float(nSteps));
    }
    return occP;
}

// radius/distFade from dNorm (SHADOW-ALT-SCALE-1 + RAMP0).
void lsvk_applyFadeModel(float dNorm, float sunAltSin, LsvkShadowKnobs k,
                         inout float radius, out float distFade)
{
    vec2 fg = mc2ShadowFadeGainsP(sunAltSin, k.distFadeMax, (k.flags & LSVK_KNOB_ALT_SCALE) != 0);
    radius *= 1.0 + fg.x * dNorm;
    distFade = mc2ShadowDistFadeCurveP(dNorm, fg.y, (k.flags & LSVK_KNOB_FADE_RAMP0) != 0);
}

// --- GL calcShadow (STATIC map; no fade). ------------------------------------
float lsvk_calcShadow(vec3 worldPos, vec3 normal, vec3 lightDir,
                      sampler2DShadow shadowMap, mat4 lightSpaceMatrix,
                      float shadowSoftness, int numTaps, LsvkShadowKnobs k)
{
    lightDir = lsvk_shadowSun(lightDir, k);
    if ((k.flags & LSVK_KNOB_BANDFIX_V2) != 0) {
        // STATIC-SHADOW-BANDING-2 normal-offset receiver, then flat-up reset for guard+bias.
        float invR = length(vec3(lightSpaceMatrix[0][0], lightSpaceMatrix[1][0], lightSpaceMatrix[2][0]));
        float texelWorld = (invR > 1e-8) ? 2.0 / (invR * float(textureSize(shadowMap, 0).x)) : 0.0;
        worldPos += normal * texelWorld;
        normal = vec3(0.0, 0.0, 1.0);
    }
    vec4 lsPos = lightSpaceMatrix * vec4(worldPos, 1.0);
    vec3 projCoords = lsPos.xyz / lsPos.w;
    projCoords.xy = projCoords.xy * 0.5 + 0.5;
    if (projCoords.z > 1.0 || projCoords.z < 0.0) return 1.0;
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0) return 1.0;
    float NdotL = max(dot(normal, lightDir), 0.0);
    if (NdotL < 0.05) return 1.0;
    float bias = max(0.005 * (1.0 - NdotL), 0.002);
    float currentDepth = projCoords.z - bias;
    // A fixed receiver kernel avoids unfiltered per-pixel noise on thin palm shadows.
    float angle = 0.0;
    float ca = cos(angle), sa = sin(angle);
    mat2 rot = mat2(ca, sa, -sa, ca);
    float depthGradient = length(vec2(dFdx(projCoords.z), dFdy(projCoords.z)));
    float hardness = clamp(depthGradient * 180.0, 0.0, 1.0);
    float adaptiveScale = mix(2.4, 0.8, hardness);
    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));
    float radius = max(shadowSoftness, 0.5) * adaptiveScale;
    float shadow = mc2ShadowTent(shadowMap, vec3(projCoords.xy, currentDepth), radius);
    return mix(0.4, 1.0, shadow);
}

// --- GL calcDynamicShadow, legacy single-map variant (fade included). --------
float lsvk_calcDynamicShadowLegacy(vec3 worldPos, vec3 normal, vec3 lightDir,
                                   sampler2DShadow dynMap, mat4 dynLightSpace,
                                   float shadowSoftness, int numTaps, LsvkShadowKnobs k)
{
    lightDir = lsvk_shadowSun(lightDir, k);
    vec4 lsPos = dynLightSpace * vec4(worldPos, 1.0);
    vec3 projCoords = lsPos.xyz / lsPos.w;
    projCoords.xy = projCoords.xy * 0.5 + 0.5;
    if (projCoords.z > 1.0 || projCoords.z < 0.0) return 1.0;
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0) return 1.0;
    float NdotL = max(dot(normal, lightDir), 0.0);
    if (NdotL < 0.05) return 1.0;
    float bias = max(0.005 * (1.0 - NdotL), 0.002);
    float currentDepth = projCoords.z - bias;
    // A fixed receiver kernel avoids unfiltered per-pixel noise on thin palm shadows.
    float angle = 0.0;
    float ca = cos(angle), sa = sin(angle);
    mat2 rot = mat2(ca, sa, -sa, ca);
    float depthGradient = length(vec2(dFdx(projCoords.z), dFdy(projCoords.z)));
    float hardness = clamp(depthGradient * 180.0, 0.0, 1.0);
    float adaptiveScale = mix(2.4, 0.8, hardness);
    vec2 texelSize = 1.0 / vec2(textureSize(dynMap, 0));
    float radius = max(shadowSoftness, 0.5) * adaptiveScale;
    float distFade = 0.0;
    if (k.distFadeWu > 0.0) {
        float invSpan = length(vec3(dynLightSpace[0][2], dynLightSpace[1][2], dynLightSpace[2][2]));
        if (invSpan > 1e-8) {
            float fadeNdc = k.distFadeWu * invSpan;
            vec2 searchR = radius * 3.0 * texelSize;
            float dNorm = lsvk_occluderDist2D(dynMap, projCoords.xy, rot, searchR, currentDepth,
                                              fadeNdc, angle, (k.flags & LSVK_KNOB_FADE_CONT) != 0);
            lsvk_applyFadeModel(dNorm, clamp(lightDir.z, 0.0, 1.0), k, radius, distFade);
        }
    }
    float shadow = mc2ShadowTent(dynMap, vec3(projCoords.xy, currentDepth), radius);
    shadow = mix(shadow, 1.0, distFade);
    return mix(0.4, 1.0, shadow);
}

// --- GL csmSampleLayer (array layer; returns visibility incl. fade). ---------
float lsvk_csmSampleLayer(vec3 projCoords, int layer, vec3 worldPos, float NdotL, float sunAltSin,
                          int numTaps, sampler2DArrayShadow arr, float cascadeTexelWorld,
                          float depthSpan, float shadowSoftness, LsvkShadowKnobs k)
{
    float slope = max(1.0 - NdotL, 0.0);
    float texelBias = 1.5 * cascadeTexelWorld / max(depthSpan, 1.0);
    float bias = max(0.0012 * slope, 0.0004) + texelBias;
    float currentDepth = projCoords.z - bias;
    // A fixed receiver kernel avoids unfiltered per-pixel noise on thin palm shadows.
    float angle = 0.0;
    float ca = cos(angle), sa = sin(angle);
    mat2 rot = mat2(ca, sa, -sa, ca);
    float depthGradient = length(vec2(dFdx(projCoords.z), dFdy(projCoords.z)));
    float hardness = clamp(depthGradient * 180.0, 0.0, 1.0);
    float adaptiveScale = mix(2.4, 0.8, hardness);
    vec2 texelSize = 1.0 / vec2(textureSize(arr, 0).xy);
    float radius = max(shadowSoftness, 0.5) * adaptiveScale;
    float distFade = 0.0;
    if (k.distFadeWu > 0.0) {
        float fadeNdc = k.distFadeWu / max(depthSpan, 1.0);
        vec2 searchR = radius * 3.0 * texelSize;
        float dNorm = lsvk_occluderDistArray(arr, projCoords.xy, float(layer), rot, searchR,
                                             currentDepth, fadeNdc, angle,
                                             (k.flags & LSVK_KNOB_FADE_CONT) != 0);
        lsvk_applyFadeModel(dNorm, sunAltSin, k, radius, distFade);
    }
    float shadow = mc2ShadowTent(arr, vec4(projCoords.xy, float(layer), currentDepth), radius);
    return mix(shadow, 1.0, distFade);
}

// --- GL csmSampleFullMap (separate lower-res 2D catch-all cascade). ----------
float lsvk_csmSampleFullMap(vec3 projCoords, vec3 worldPos, float NdotL, float sunAltSin,
                            int numTaps, sampler2DShadow fullMap, float texelWorld,
                            float depthSpan, float shadowSoftness, LsvkShadowKnobs k)
{
    float slope = max(1.0 - NdotL, 0.0);
    float texelBias = 1.5 * texelWorld / max(depthSpan, 1.0);
    float bias = max(0.0012 * slope, 0.0004) + texelBias;
    float currentDepth = projCoords.z - bias;
    // A fixed receiver kernel avoids unfiltered per-pixel noise on thin palm shadows.
    float angle = 0.0;
    float ca = cos(angle), sa = sin(angle);
    mat2 rot = mat2(ca, sa, -sa, ca);
    float depthGradient = length(vec2(dFdx(projCoords.z), dFdy(projCoords.z)));
    float hardness = clamp(depthGradient * 180.0, 0.0, 1.0);
    float adaptiveScale = mix(2.4, 0.8, hardness);
    vec2 texelSize = 1.0 / vec2(textureSize(fullMap, 0).xy);
    float radius = max(shadowSoftness, 0.5) * adaptiveScale;
    float distFade = 0.0;
    if (k.distFadeWu > 0.0) {
        float fadeNdc = k.distFadeWu / max(depthSpan, 1.0);
        vec2 searchR = radius * 3.0 * texelSize;
        float dNorm = lsvk_occluderDist2D(fullMap, projCoords.xy, rot, searchR, currentDepth,
                                          fadeNdc, angle, (k.flags & LSVK_KNOB_FADE_CONT) != 0);
        lsvk_applyFadeModel(dNorm, sunAltSin, k, radius, distFade);
    }
    float shadow = mc2ShadowTent(fullMap, vec3(projCoords.xy, currentDepth), radius);
    return mix(shadow, 1.0, distFade);
}

#endif // LIGHTING_SHADOW_VK_GLSL
