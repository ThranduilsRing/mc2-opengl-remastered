// shaders/static_prop.frag
// GPU static prop renderer — main fragment shader.
// NOTE: no #version directive here — makeProgram() prepends "#version 430\n".
// All formerly-uint uniforms are declared `int` because this project's
// shader_builder crashes on `uniform uint` (see memory/uniform_uint_crash.md).

#define PREC highp

#include <include/render_contract.hglsl>

// [RENDER_CONTRACT]
//   Pass:           StaticProp
//   Color0:         RGBA, opaque (alpha-test for ALPHA_TEST_BIT materials)
//   GBuffer1:       rc_gbuffer1_screenShadowEligible (production path)
//                   rc_gbuffer1_legacyDebugSentinelScreenShadowEligible (debug)
//   ShadowContract: castsStatic=true, castsDynamic=true,
//                   skipsPostScreenShadow=false (post-shadow applies)
//   StateContract:  depthTest=true, depthWrite=true, blend=Opaque,
//                   requiresMRT=true

in vec3  v_normal;
in vec2  v_uv;
flat in uint v_flags;
in vec4  v_highlight;
in vec4  v_fog;
in vec4  v_argb;
flat in uint v_localVertexID;

uniform sampler2D u_tex;
uniform int   u_materialFlags;   // bit 0: ALPHA_TEST
uniform float u_fogValue;        // 1.0 = clear, 0.0 = fully fogged
uniform int   u_debugAddrMode;   // 0 normal, 1 gradient, 2 hash, 3 white, 4 argb-only, 5 tex-only, 6 highlight-only, 7 tex+highlight
uniform int   u_maxLocalVertexID;
uniform int   u_packetID;

layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec4 GBuffer1;

const int ALPHA_TEST_BIT = 1;

uint hash_u(uint x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

void main() {
    vec4 tex_color = texture(u_tex, v_uv);

    if ((u_materialFlags & ALPHA_TEST_BIT) != 0 && tex_color.a < 0.5) {
        discard;
    }

    if (u_debugAddrMode == 1) {
        float t = float(v_localVertexID) / max(float(u_maxLocalVertexID), 1.0);
        FragColor = vec4(t, t, t, 1.0);
        GBuffer1  = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible();
        return;
    }
    if (u_debugAddrMode == 2) {
        uint h = hash_u(uint(u_packetID) * 2654435761u + v_localVertexID);
        FragColor = vec4(
            float((h >>  0) & 0xFFu) / 255.0,
            float((h >>  8) & 0xFFu) / 255.0,
            float((h >> 16) & 0xFFu) / 255.0,
            1.0);
        GBuffer1  = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible();
        return;
    }
    // Bisection modes (RAlt+9 cycles 0..7).
    if (u_debugAddrMode == 3) { FragColor = vec4(1.0); GBuffer1 = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible(); return; }
    if (u_debugAddrMode == 4) { FragColor = vec4(v_argb.rgb,    1.0); GBuffer1 = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible(); return; }
    if (u_debugAddrMode == 5) { FragColor = vec4(tex_color.rgb, 1.0); GBuffer1 = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible(); return; }
    if (u_debugAddrMode == 6) { FragColor = vec4(v_highlight.rgb * v_highlight.a, 1.0); GBuffer1 = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible(); return; }
    if (u_debugAddrMode == 7) {
        vec3 rgb = tex_color.rgb + v_highlight.rgb * v_highlight.a;
        FragColor = vec4(rgb, 1.0);
        GBuffer1 = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible();
        return;
    }

    vec4 c = tex_color * v_argb;
    c.rgb += v_highlight.rgb * v_highlight.a;
    c.rgb = mix(v_fog.rgb, c.rgb, u_fogValue);

    FragColor = c;
    GBuffer1  = rc_gbuffer1_screenShadowEligible(normalize(v_normal));
}
