// VK-FIELD-VFX-1 -- textured billboard FS for the VK particle island: the
// default-mode body of shaders/particle_billboard.frag (sprite sample times the
// per-particle tint, colour-key discard, head brighten, low-alpha discard). The
// GL debug views and the soft / lit / distortion / blackbody options are not
// ported; each is off by default in GL.
//
// Straight-alpha output (rgb, a): the pipeline blend factors are
//   alpha    group: SRC_ALPHA / ONE_MINUS_SRC_ALPHA  (matches GL alpha)
//   additive group: SRC_ALPHA / ONE                  (matches GL additive)
// so the FS emits straight color; the src-alpha factor premultiplies in HW.
#version 450
#extension GL_GOOGLE_include_directive : require
#define TERRAIN_RESOURCE_SET 3
#include "include/terrain_frame_vk.glsl"
#define MC2_FOG_GPU
#define MC2_FOG_SET 0
#define MC2_FOG_BINDING 27
#include "../include/fog_policy.hglsl"
layout(set = 0, binding = 0, std140) uniform VfxVsParams {
    mat4 worldToClipGL;
    vec4 camRight;
    vec4 camUp;
} U;
layout(location = 7) in vec3 v_worldPos;

layout(location = 6) flat in uint v_blend;

#ifdef VFX_INDEXED
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 1, binding = 0) uniform sampler2D uPages[64];
layout(location = 5) flat in uint v_page;
#define uAtlas uPages[nonuniformEXT(v_page)]
// Premultiplied fold: one pipeline (ONE, ONE_MINUS_SRC_ALPHA) draws blend modes 0/1/2/5; the record's mode picks
//   0 alpha SRC_ALPHA/1-SRC_ALPHA: (rgb*a, a)   1 SRC_ALPHA/ONE: (rgb*a, 0)
//   2 ONE/ONE: (rgb, 0)                         5 ONE/1-SRC_ALPHA: (rgb, a)   [3 opaque and 4 ONE/SRC_ALPHA keep their pipelines]
// Scene alpha is never read after this pass (every consumer reads rgb), so only the rgb result must match the old modes.
vec4 foldBlend(vec4 c) {
    if (v_blend == 3u || v_blend == 4u) return c;
    float a = clamp(c.a, 0.0, 1.0);
    return vec4(c.rgb * ((v_blend == 0u || v_blend == 1u) ? a : 1.0), (v_blend == 0u || v_blend == 5u) ? a : 0.0);
}
#define VFX_OUT(c) foldBlend(c)
#else
layout(set = 1, binding = 0) uniform sampler2D uAtlas;   // the draw group's sprite page
#define VFX_OUT(c) (c)
#endif

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) flat in uint v_flags;   // bit 0 = head sprite, bit 1 = pure-additive (OneOne) group

layout(location = 0) out vec4 o_color;   // shared scene color0
layout(location = 1) out vec4 o_normal;  // shared scene normal (writemask 0 -> inert)

// Additive radiance and beam heads are emission. Alpha-blended smoke/dust
// receives the mission's diffuse night scale and ambient chroma.
bool effectIsEmissive() {
    return (v_flags & 1u) != 0u || v_blend == 1u || v_blend == 2u;
}

// Smoke has an isotropic diffuse response; only direct sun receives shadows.
vec3 effectIncidentLight() {
    vec3 N = vec3(0, 0, 1);
    float shadow = min(terrainStaticShadow(v_worldPos), terrainDynamicShadow(v_worldPos, N));
    vec3 light = incidentLight(uEnv.ambLive.rgb, 0.5 * uEnv.sunLive.rgb, vec3(1), shadow);
    if (uT.numPoolLights > 0) {
        float contactUnused = 0.0;
        light += tlp_terrainPoolLight(v_worldPos, uT.numPoolLights,
            uT.lightGridDim, vec2(uT.lightGridMinX, uT.lightGridMinY),
            vec2(uT.lightGridInvCellX, uT.lightGridInvCellY),
            uT.lightPoolSoft, uT.lightPoolMoverDim, uT.lightPoolMoverScale,
            uT.lightPoolMoverMax, uT.lightPoolCompress, 0.0, contactUnused, N, N, false, 0.0);
    }
    return light;
}

vec4 fogEffect(vec4 c) {
    vec2 ndc = gl_FragCoord.xy * vec2(U.camRight.w, U.camUp.w) - 1.0;
    vec4 fog = mc2WorldFog(v_worldPos, ndc);
    // Additive light loses radiance to extinction but introduces no fog coverage.
    c.rgb = mix(c.rgb, (v_blend == 1u || v_blend == 2u) ? vec3(0) : fog.rgb, fog.a);
    return c;
}

void main() {
    // Swept tubes advance U per profile beyond 1.0. Clamp only when authored:
    // clamping a repeating trail stretches its edge texel into a flat sheet.
    // Bit 4 carries the tube's MLR texture-address policy; meshes keep clamp.
    vec2 uv = v_uv;
    if ((v_flags & 16u) != 0u) {
        vec2 edge = 0.5 / vec2(textureSize(uAtlas, 0));
        uv = clamp(uv, edge, 1.0 - edge);
    }
    vec4 tex = (v_flags & 32u) != 0u ? vec4(1.0) : textureLod(uAtlas, uv, 0.0);
    if ((v_flags & 12u) != 0u) {
        vec4 c = tex * v_color;
        if (!effectIsEmissive()) c.rgb *= effectIncidentLight();
        if ((v_flags & 8u) != 0u) {
            if ((v_flags & 2u) != 0u) {
                if (max(max(c.r, c.g), c.b) <= 0.0001) discard;
            // ONE/srcA and ONE/1-srcA retain RGB even at alpha zero.
            } else if ((v_flags & 1536u) == 0u && c.a <= 0.0039) discard;
            // Bit 1024: ONE/ZERO opaque tube. Alpha is irrelevant; write full coverage.
            if ((v_flags & 1024u) != 0u) c.a = 1.0;
        }
        // Mesh SRC_ALPHA/ONE keeps its authored alpha. Tube ONE/ONE uses RGB.
        o_color = VFX_OUT(fogEffect(c));
        o_normal = vec4(0.0);
        return;
    }
    // MC2 sprite pages mark transparent texels with magenta (0xFF00FF).
    if (tex.r > 0.9 && tex.g < 0.1 && tex.b > 0.9) discard;
    vec4 c = tex * v_color;
    if (!effectIsEmissive()) c.rgb *= effectIncidentLight();
    // Head sprites (the bright leading particle of a beam / trail) draw 1.5x brighter.
    if ((v_flags & 1u) != 0u) c.rgb *= 1.5;
    // OneOne (ONE,ONE) ignores alpha: the MLR path adds the sprite colour whatever the vertex alpha is
    // (the halo cards carry alpha 0), so such a group neither multiplies by nor discards on alpha.
    if ((v_flags & 2u) != 0u) c.a = 1.0;
    else if (c.a < 0.01) discard;
    o_color  = VFX_OUT(fogEffect(c));
    o_normal = vec4(0.0);   // GL VFX writes nothing to the normal target
}
