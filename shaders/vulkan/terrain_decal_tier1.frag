// VK-PARITY-OVERLAY-1 TERRAIN DECAL TIER-1 -- FS.
// =============================================================================
// Port of shaders/decal.frag (bomb craters, mech footprints, ground-contact blobs, dynamic
// impact decals): texture x vertex colour, inline sun shadow = min(static, dynamic) unless
// the GROUND-CONTACT-BLOB self-darkening marker (Texcoord.y < -0.5), untextured when
// Texcoord.x < -0.5, vertex-fog mix against the renderer fog colour. Pipeline: SRC_ALPHA /
// ONE_MINUS_SRC_ALPHA on MRT0, depth test GEQUAL / no depth write, MRT1 REPLACED with
// shadowHandled flat-up (0.5,0.5,1,1) (GL: SRC_ALPHA = 1 of that value => replace).
#version 450
#extension GL_GOOGLE_include_directive : require
#ifndef PREC
#define PREC highp
#endif
#include "include/terrain_frame_vk.glsl"

layout(location = 0) in vec3  WorldPos;
layout(location = 1) in vec2  Texcoord;
layout(location = 2) in float FogValue;
layout(location = 3) in vec4  Color;

layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec4 GBuffer1;

layout(set = 1, binding = 0) uniform sampler2D tex1;

layout(push_constant) uniform OverlayPC {
    vec4  fogColor;
    float artFlatExempt;
    float mapHalfExtent;
    int   biasLegacy;
    int   cementAdopt;
    int   seamFix;
    int   _pad0;
    int   _pad1;
    int   _pad2;
} pc;

void main() {
    vec4 tex_color = (Texcoord.x < -0.5) ? vec4(1.0) : texture(tex1, Texcoord);
    vec4 c = Color * tex_color;

    bool skipInlineShadow = (Texcoord.y < -0.5);
    if (!skipInlineShadow) {
        const vec3 shadowN = vec3(0.0, 0.0, 1.0);
        vec3 lightDir3 = uT.terrainLightDir.xyz;
        LsvkShadowKnobs shKnobs = mv_shadowKnobs();
        float staticShadow = 1.0;
        if (uT.enableShadows != 0)
            staticShadow = lsvk_calcShadow(WorldPos, shadowN, lightDir3, u_staticShadow,
                                           uT.staticLightSpaceMatrix, uT.shadowSoftness, 8, shKnobs);
        float dynShadow = 1.0;
        if (uT.csmEnabled != 0) {
            vec3 shSun = lsvk_shadowSun(lightDir3, shKnobs);
            float NdotLdyn = max(dot(shadowN, shSun), 0.0);
            dynShadow = (NdotLdyn < 0.05) ? 1.0
                      : mv_calcDynamicShadowCsm(WorldPos, NdotLdyn, clamp(shSun.z, 0.0, 1.0), 4);
        } else if (uT.enableDynShadows != 0) {
            dynShadow = lsvk_calcDynamicShadowLegacy(WorldPos, shadowN, lightDir3, u_dynamicShadow,
                                                     uT.dynamicLightSpaceMatrix, uT.shadowSoftness, 4, shKnobs);
        }
        c.rgb *= min(staticShadow, dynShadow);
    }

    if (pc.fogColor.x > 0.0 || pc.fogColor.y > 0.0 || pc.fogColor.z > 0.0 || pc.fogColor.w > 0.0)
        c.rgb = mix(pc.fogColor.rgb, c.rgb, FogValue);

    FragColor = c;
    GBuffer1  = vec4(0.5, 0.5, 1.0, 1.0);
}
