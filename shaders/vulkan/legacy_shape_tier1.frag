// VK-PARITY-F1 LEGACY SHAPE TIER-1 -- FS.
// Port of shaders/gos_tex_vertex.frag (default, non-water -- the
// legacy loops): c = vertexColor(.bgra) * texture, optional ALPHA_TEST discard, then the
// vertex-fog mix when the renderer fog colour is set (off for the additive pass, whose
// loops set gos_State_Fog 0). GBuffer1 = flat-up, screen-shadow-eligible
// (rc_gbuffer1_screenShadowEligible(vec3(0,0,1))); blended passes mask MRT1 off.
#version 450
#extension GL_GOOGLE_include_directive : require

// Per-frame UBO: generated from RenderCore/gpu_abi/GpuLegacyFrameUBO.h.
#include "include/legacy_frame_ubo_vk.glsl"

layout(set = 0, binding = 1) uniform sampler2D uAlbedo;

layout(push_constant) uniform LegacyPassPC {
    uint flags;   // bit0 = ALPHA_TEST (discard a < 0.5), bit1 = fog off, bit2 = opaque (write alpha 1)
} pc;

layout(location = 0) in vec4  vColor;
layout(location = 1) in float vFogValue;
layout(location = 2) in vec2  vUv;

layout(location = 0) out vec4 oColor;     // MRT0
layout(location = 1) out vec4 oGBuffer1;  // MRT1

void main() {
    vec4 tex = texture(uAlbedo, vUv);
    if ((pc.flags & 1u) != 0u && tex.a < 0.5) discard;        // ALPHA_TEST variant
    vec4 c = vColor * tex;
    if ((pc.flags & 2u) == 0u &&
        (uFrame.fogColor.x > 0.0 || uFrame.fogColor.y > 0.0 ||
         uFrame.fogColor.z > 0.0 || uFrame.fogColor.w > 0.0))
        c.rgb = mix(uFrame.fogColor.rgb, c.rgb, vFogValue);
    oColor = vec4(c.rgb, ((pc.flags & 4u) != 0u) ? 1.0 : c.a);
    oGBuffer1 = vec4(0.5, 0.5, 1.0, 0.0);
}
