// VULKAN-EDGE-FOG-ISLAND-2a -- Vulkan port of shaders/edge_fog.frag.
//
// MATH is bit-for-bit identical to the GL edge_fog.frag (EDGE-FOG-4 cloud bank at
// the map boundary). ONLY the uniform/sampler plumbing changed: the GL default-
// block uniforms became explicit Vulkan set/binding layouts:
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
// column_major (std140 default): the CPU packs invViewProj as the SAME 16 raw floats
// the GL path uploads (glUniformMatrix4fv GL_FALSE). inverseViewProj_ is column-major
// on the CPU (index=col*4+row), so GL_FALSE => GLSL reads it column-major => mul_colmajor
// is the CORRECT unprojection (ground truth: GL-Z=-15.39). row_major here reads
// transpose(M) (=669.57) and breaks world reconstruction -- the L6.5 transpose
// regression, proved via oblique fog bookmarks (cliff_side_on/oblique_fog/oblique_horizon
// DIFF; top-down views gate fog to vec4(0) and hid it). Same convention as ssao.frag
// (N0 fix ac96e31d). Do NOT qualify row_major.
layout(set = 0, binding = 1, std140) uniform EdgeFogParams {
    mat4  invViewProj;
    vec3  u_fogColor;
    float _pad0;
    float u_halfExtent;
    float u_fogStart;
    float u_fogHeight;
    float u_fogMax;
    float u_waterElevation;
};

void main()
{
    float rawDepth = texture(depthTex, TexCoord).r;
    vec2 ndc = TexCoord * 2.0 - 1.0;

    vec4 pNear = invViewProj * vec4(ndc, 1.0, 1.0);
    vec4 pFar  = invViewProj * vec4(ndc, 0.0, 1.0);
    vec3 wNear = pNear.xyz / pNear.w;
    vec3 wFar  = pFar.xyz / pFar.w;

    float geoZ = u_fogHeight;
    if (rawDepth >= 0.0001) {
        vec4 wp = invViewProj * vec4(ndc, rawDepth, 1.0);
        geoZ = wp.z / wp.w;
    }

    if (geoZ <= u_waterElevation + 2.0) { outFog = vec4(0.0); return; }

    float heightFade = smoothstep(u_fogHeight + 20.0, u_fogHeight, geoZ);
    if (heightFade <= 0.0) { outFog = vec4(0.0); return; }

    float dz = wFar.z - wNear.z;
    if (dz >= -0.001) { outFog = vec4(0.0); return; }
    float t = (u_fogHeight - wNear.z) / dz;
    if (t < 0.0 || t > 1.0) { outFog = vec4(0.0); return; }
    vec2 planeXY = wNear.xy + t * (wFar.xy - wNear.xy);

    float distFromEdge = u_halfExtent - max(abs(planeXY.x), abs(planeXY.y));

    float innerRamp   = smoothstep(u_fogStart, 0.0, distFromEdge);
    float outsideFill = step(0.0, -distFromEdge);

    float fogFactor = clamp(max(innerRamp, outsideFill) * heightFade * u_fogMax, 0.0, 1.0);
    outFog = vec4(u_fogColor, fogFactor);
}
