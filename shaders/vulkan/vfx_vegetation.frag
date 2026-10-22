// Opaque vegetation cutout. Coverage comes from the cooked mip chain, never
// from alpha blending or spatial/temporal stipple.
#version 450
#extension GL_GOOGLE_include_directive : require
#define PREC highp
#include "../include/render_contract.hglsl"
layout(set = 1, binding = 0) uniform sampler2DArray uAtlas;
layout(location = 0) in vec2 v_uv;
layout(location = 3) in vec4 v_vegData;
layout(location = 5) in vec3 v_cardNormal;
layout(location = 8) flat in uint v_atlasLayer;
layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_normal;
void main() {
    vec4 col = texture(uAtlas, vec3(v_uv, float(v_atlasLayer)));
    if (col.a < 0.5) discard;
    // Retain the existing colour until P3 connects shared ground lighting.
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    col.rgb = mix(col.rgb, vec3(lum), 0.30);
    col.r += (v_vegData.x - 0.5) * 0.07;
    col.b -= v_vegData.x * 0.05;
    float NdotL = v_vegData.w;
    col.rgb *= (0.60 + 0.38 * NdotL) * mix(1.0, 0.80, v_vegData.z);
    col.rgb += vec3(v_vegData.y * 0.08);
    o_color = vec4(min(col.rgb, vec3(1.0)), 1.0);
    vec3 normal = normalize(gl_FrontFacing ? v_cardNormal : -v_cardNormal);
    o_normal = rc_gbuffer1_shadowHandled(normal);
}
