// VULKAN-EDGE-FOG-ISLAND-2a -- Vulkan port of shaders/edge_fog.frag.
//
// Fog policy scales distances and suppresses steep-view range whiteout.
// The map-boundary concealment retains its outside-fill semantics.
// Uniform/sampler plumbing uses explicit Vulkan set/binding layouts:
//   set=0,binding=0  depthTex        -> sampler2D (the scene depth, uploaded to a
//                                       D32_SFLOAT image; sampling returns depth in .r,
//                                       same as sampling a GL depth texture's .r)
//   set=0,binding=1  EdgeFogParams   -> std140 UBO mirroring the GL uniforms. The
//                                       CPU packs invViewProj as the SAME 16 floats
//                                       the GL path uploads (row-major direct upload,
//                                       GL_FALSE) so the unprojection matches GL.
//
// invViewProj depth convention: reverse-Z [0,1]. wNear uses ndc-depth 1.0 (near),
// wFar uses ndc-depth 0.0 (far) -- identical to the GL shader. No Y-flip is applied
// because the pass reads TexCoord (interpolated [0,1] UV), not gl_FragCoord, and the
// island uploads the depth image so texel (u,v) maps the same way as the GL sample.
#version 450
#extension GL_GOOGLE_include_directive : require
#define MC2_FOG_GPU 1
#define MC2_FOG_SET 0
#define MC2_FOG_BINDING 1
#include "../include/fog_policy.hglsl"

layout(location = 0) in  vec2 TexCoord;
layout(location = 0) out vec4 outFog;

layout(set = 0, binding = 0) uniform sampler2D depthTex;

// std140. Offsets (bytes) chosen to match the CPU-side POD in
// vulkan_edge_fog_island.cpp:
//   invViewProj      @  0  (mat4, 64B)
//   u_fogColor       @ 64  (vec3, 12B) + pad float @ 76
//   u_halfExtent     @ 80
//   u_fogStart       @ 84
//   u_fogHeight      @ 88
//   u_fogMax         @ 92
//   u_waterElevation @ 96
//   u_horizonClampEnabled @100, u_horizonFadeStartSin @104, u_horizonFadeEndSin @108
// column_major (std140 default): the CPU packs invViewProj as the SAME 16 raw floats
// the GL path uploads (glUniformMatrix4fv GL_FALSE). inverseViewProj_ is column-major
// on the CPU (index=col*4+row), so GL_FALSE => GLSL reads it column-major => mul_colmajor
// is the CORRECT unprojection (ground truth: GL-Z=-15.39). row_major here reads
// transpose(M) (=669.57) and breaks world reconstruction -- the L6.5 transpose
// regression, proved via oblique fog bookmarks (cliff_side_on/oblique_fog/oblique_horizon
// DIFF; top-down views gate fog to vec4(0) and hid it). Same convention as ssao.frag
// (N0 fix ac96e31d). Do NOT qualify row_major.
void main() {
    float rawDepth = texture(depthTex, TexCoord).r;
    if (rawDepth <= 0.0) { outFog = vec4(0.0); return; }
    vec2 ndc = TexCoord * 2.0 - 1.0;
    vec4 wp = invViewProj * vec4(ndc, rawDepth, 1.0);
    outFog = mc2WorldFog(wp.xyz / wp.w, ndc);
}
