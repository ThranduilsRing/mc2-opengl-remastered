// VK-PARITY-OVERLAY-1 TERRAIN DECAL TIER-1 -- FS.
// =============================================================================
// Port of shaders/decal.frag (bomb craters, mech footprints, ground-contact blobs, dynamic
// impact decals): texture x vertex colour, inline sun shadow = min(static, dynamic) unless
// the GROUND-CONTACT-BLOB self-darkening marker (Texcoord.y < -0.5), untextured when
// Texcoord.x < -0.5, vertex-fog mix against the renderer fog colour. Pipeline: SRC_ALPHA /
// ONE_MINUS_SRC_ALPHA on MRT0, depth test GEQUAL / no depth write, MRT1 REPLACED with
// shadowHandled flat-up (0.5,0.5,1,1) (GL: SRC_ALPHA = 1 of that value => replace).
#version 460
#ifdef MC2_RT_SUN_SHADOWS
#extension GL_EXT_ray_query : require
#endif
#extension GL_GOOGLE_include_directive : require
#ifndef PREC
#define PREC highp
#endif
#include "include/terrain_frame_vk.glsl"
#ifdef MC2_RT_SUN_SHADOWS
#include "include/rt_sun_shadow.glsl"
#endif

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
        vec3 lightDir3 = uEnv.sunDirLive.xyz;
        float staticS=uT.enableShadows!=0?terrainStaticShadow(WorldPos):1.0;
#ifdef MC2_RT_SUN_SHADOWS
        if (uT.enableShadows!=0 && WorldPos.z>=uT.shorelineWaterElev)
            staticS=min(staticS,mix(sunShadowFloor(0.4),1.0,rtPropSunVisibility(WorldPos,shadowN,moverFrame.sun.xyz)));
#endif
        c.rgb *= min(staticS,terrainDynamicShadow(WorldPos,shadowN));
    }
    c.rgb *= uEnv.sceneLight.x * uEnv.sceneLight.yzw;   // LOCAL-LIGHTS-4: same night scale as the ground under it

    // Opaque screen fog owns distance/height attenuation.

    FragColor = c;
    GBuffer1  = vec4(0.5, 0.5, 1.0, 1.0);
}
