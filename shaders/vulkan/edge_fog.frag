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
//
// KNOWN DIVERGENCE vs the GL shader (SKYBOX-FOG-EXCLUDE-1/2): the GL edge_fog.frag
// gained a stencil-tagged true-sky exclusion (usampler2D stencilTex +
// u_skyExcludeEnabled, feathered by a worldDir.z smoothstep in v2). NOT ported
// here: the island's UBO/binding POD (vulkan_edge_fog_island.cpp) has no stencil
// image, and the island parity proof predates the feature. When the Vulkan backend
// reaches this pass for real (Layer 6+), port the v2 FEATHERED form, never the v1
// hard zero -- v1's tag pass had a frame bug (azimuth wedge, see
// shaders/hdri_skybox_stencil_tag.frag SKYBOX-FOG-EXCLUDE-2 note).
#version 450

layout(location = 0) in  vec2 TexCoord;
layout(location = 0) out vec4 outFog;

layout(set = 0, binding = 0) uniform sampler2D depthTex;

// SKYBOX-FOG-EXCLUDE-VULKAN-PORT-1 (mirrors GL shaders/edge_fog.frag, gate
// MC2_SKYBOX_FOG_EXCLUDE, default OFF -> u_skyExcludeEnabled=0, byte-identical to
// legacy). stencilTex is an R8_UINT view fed by a CPU glGetTexImage(GL_STENCIL_INDEX)
// bridge of the GL stencil-tag pass' output (same bridge shape as depthTex/colorTex --
// see vulkan_postprocess_subgraph.cpp), NOT a native VK_FORMAT_S8_UINT sampled view
// (no universal Vulkan sampled-image guarantee for that format). Raw unsigned index
// values (0..255), not normalized floats -- matches the GL usampler2D contract.
layout(set = 0, binding = 2) uniform usampler2D stencilTex;

// std140. Offsets (bytes) chosen to match the CPU-side POD in
// vulkan_edge_fog_island.cpp:
//   invViewProj          @  0  (mat4, 64B)
//   u_fogColor            @ 64  (vec3, 12B) + pad float @ 76
//   u_halfExtent          @ 80
//   u_fogStart            @ 84
//   u_fogHeight           @ 88
//   u_fogMax              @ 92
//   u_waterElevation      @ 96
//   u_skyExcludeEnabled   @100  (int; SKYBOX-FOG-EXCLUDE-VULKAN-PORT-1)
// row_major: the CPU packs invViewProj as the SAME 16 floats the GL path uploads
// (row-major direct upload, glUniformMatrix4fv GL_FALSE). std140 mat4 defaults to
// column-major, which would transpose the matrix and break the unprojection
// (VULKAN-EDGE-FOG-ISLAND-2b: GL-Z=-15.39 vs VK-Z=669.57 before this qualifier).
layout(set = 0, binding = 1, std140, row_major) uniform EdgeFogParams {
    mat4  invViewProj;
    vec3  u_fogColor;
    float _pad0;
    float u_halfExtent;
    float u_fogStart;
    float u_fogHeight;
    float u_fogMax;
    float u_waterElevation;
    int   u_skyExcludeEnabled;
};

void main()
{
    float rawDepth = texture(depthTex, TexCoord).r;

    // SKYBOX-FOG-EXCLUDE-VULKAN-PORT-1: hard-exclude true sky (stencil==1) when the
    // gate is on. Mirrors GL edge_fog.frag exactly -- gate this on the same
    // rawDepth<0.0001 population the void-height-plane branch below already targets.
    if (u_skyExcludeEnabled != 0 && rawDepth < 0.0001) {
        uint stencilVal = texture(stencilTex, TexCoord).r;
        if (stencilVal != 0u) { outFog = vec4(0.0); return; }
    }

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

    // FOG-HORIZON-CLAMP-1 (mirrors GL shaders/edge_fog.frag). Fold in the shared
    // elevation profile: elevSin = -viewDir.z; full at/below the horizon, fade to
    // zero across [START_SIN, END_SIN], keeping the cloud bank from bleeding
    // upward. DIVERGENCE (same as fog_oob.frag / the SKYBOX-FOG-EXCLUDE note): the
    // GL path passes the band edges + kill-switch via uniforms; this island's POD
    // predates the feature, so the DEFAULT profile (0deg..5deg, clamp ON) is baked
    // as constants. Promote to EdgeFogParams UBO fields at Layer 6+ for runtime
    // knobs.
    const float FOG_HORIZON_START_SIN = 0.0;      // sin(0deg)
    const float FOG_HORIZON_END_SIN   = 0.08716;  // sin(5deg)
    float elevSin = -normalize(wFar - wNear).z;
    float horizonFactor = 1.0 - smoothstep(FOG_HORIZON_START_SIN, FOG_HORIZON_END_SIN, elevSin);

    float fogFactor = clamp(max(innerRamp, outsideFill) * heightFade * u_fogMax, 0.0, 1.0)
                      * horizonFactor;  // FOG-HORIZON-CLAMP-1
    outFog = vec4(u_fogColor, fogFactor);
}
