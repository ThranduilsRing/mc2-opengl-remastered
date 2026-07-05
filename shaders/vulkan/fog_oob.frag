// VULKAN-OOB-FOG-ISLAND-1 -- Vulkan port of shaders/fog_oob.frag (OOB-FOG-7
// sea-of-clouds at the map horizon).
//
// MATH is bit-for-bit identical to the GL fog_oob.frag: 3D FBM sampled on the
// unit sphere (worldDir), solid cloud base + gentle surface variation, filling
// the out-of-bounds void below the horizon. ONLY the uniform/sampler plumbing
// changed: the GL default-block uniforms became explicit Vulkan set/binding
// layouts:
//   set=0,binding=0  depthTex        -> sampler2D (the scene depth, uploaded to a
//                                       D32_SFLOAT image; sampling returns depth in
//                                       .r, same as sampling a GL depth texture's .r)
//   set=0,binding=1  FogOobParams    -> std140 UBO mirroring the GL uniforms. The
//                                       CPU packs invViewProj as the SAME 16 floats
//                                       the GL path uploads (fog_oob GL path builds
//                                       invT = transpose(inverseViewProj_) then
//                                       setMat4 GL_TRUE -> the shader receives the
//                                       row-major inverseViewProj_ bytes; we pack
//                                       those same bytes here and qualify row_major).
//
// invViewProj depth convention: reverse-Z [0,1]. pNear uses ndc-depth 1.0 (near),
// pFar uses ndc-depth 0.0 (far) -- identical to the GL shader. No Y-flip is applied
// because the pass reads TexCoord (interpolated [0,1] UV), not gl_FragCoord, and the
// island uploads the depth image so texel (u,v) maps the same way as the GL sample.
//
// KNOWN DIVERGENCE vs the GL shader (SKYBOX-FOG-EXCLUDE-1/2): the GL fog_oob.frag
// gained a stencil-tagged true-sky exclusion (usampler2D stencilTex +
// u_skyExcludeEnabled, feathered by skyFade in v2). NOT ported here: the island's
// UBO/binding POD (vulkan_oob_fog_island.cpp) has no stencil image, and the island
// parity proof predates the feature. When the Vulkan backend reaches this pass for
// real (Layer 6+), port the v2 FEATHERED form (skyExclude = skyFade on tagged
// pixels), never the v1 hard zero -- v1's tag pass had a frame bug (azimuth wedge,
// see shaders/hdri_skybox_stencil_tag.frag SKYBOX-FOG-EXCLUDE-2 note).
#version 450

layout(location = 0) in  vec2 TexCoord;
layout(location = 0) out vec4 outFog;

layout(set = 0, binding = 0) uniform sampler2D depthTex;

// SKYBOX-FOG-EXCLUDE-VULKAN-PORT-1 (mirrors GL shaders/fog_oob.frag, gate
// MC2_SKYBOX_FOG_EXCLUDE, default OFF -> u_skyExcludeEnabled=0, byte-identical to
// legacy). stencilTex is an R8_UINT view fed by a CPU glGetTexImage(GL_STENCIL_INDEX)
// bridge of the GL stencil-tag pass' output (same bridge shape as depthTex/colorTex --
// see vulkan_postprocess_subgraph.cpp), NOT a native VK_FORMAT_S8_UINT sampled view
// (no universal Vulkan sampled-image guarantee for that format). Raw unsigned index
// values (0..255), not normalized floats -- matches the GL usampler2D contract.
layout(set = 0, binding = 2) uniform usampler2D stencilTex;

// std140. Offsets (bytes) match the CPU-side POD in vulkan_oob_fog_island.cpp:
//   invViewProj         @  0  (mat4, 64B)
//   u_fogColor           @ 64  (vec3, 12B)
//   u_fogOpacity          @ 76  (float; packs into the vec3's trailing slot in std140)
//   u_time                @ 80  (float)
//   u_skyExcludeEnabled   @ 84  (int; SKYBOX-FOG-EXCLUDE-VULKAN-PORT-1)
// row_major: the CPU packs invViewProj as the SAME 16 floats the GL path uploads.
// std140 mat4 defaults to column-major, which would transpose the matrix and break
// the unprojection (the VULKAN-EDGE-FOG-ISLAND-2b matrix bug) -- hence row_major.
layout(set = 0, binding = 1, std140, row_major) uniform FogOobParams {
    mat4  invViewProj;
    vec3  u_fogColor;
    float u_fogOpacity;
    float u_time;
    int   u_skyExcludeEnabled;
};

// ---- 3D value noise (IDENTICAL to the GL fog_oob.frag) ----
float hash31(vec3 p) {
    p = fract(p * vec3(127.1, 311.7, 74.7));
    return fract(sin(dot(p, vec3(269.5, 183.3, 246.1))) * 43758.5453);
}

float vnoise3(vec3 p) {
    vec3 i = floor(p); vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(
        mix(mix(hash31(i),              hash31(i+vec3(1,0,0)), f.x),
            mix(hash31(i+vec3(0,1,0)),  hash31(i+vec3(1,1,0)), f.x), f.y),
        mix(mix(hash31(i+vec3(0,0,1)),  hash31(i+vec3(1,0,1)), f.x),
            mix(hash31(i+vec3(0,1,1)),  hash31(i+vec3(1,1,1)), f.x), f.y),
        f.z);
}

float fbm3D(vec3 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 5; i++) {
        v += vnoise3(p) * a;
        p  = p * 2.1 + vec3(1.7, 0.9, 1.4);
        a *= 0.5;
    }
    return v;
}

void main()
{
    float rawDepth = texture(depthTex, TexCoord).r;
    if (rawDepth > 0.0001) { outFog = vec4(0.0); return; }

    // SKYBOX-FOG-EXCLUDE-VULKAN-PORT-1: hard-exclude true sky (stencil==1) when the
    // gate is on. Mirrors GL fog_oob.frag exactly -- stencil is only ever tagged for
    // depth-unwritten pixels (sky never writes depth), so this check is only reached
    // for the same population the rawDepth test above already narrowed to.
    if (u_skyExcludeEnabled != 0) {
        uint stencilVal = texture(stencilTex, TexCoord).r;
        if (stencilVal != 0u) { outFog = vec4(0.0); return; }
    }

    vec2 ndc = TexCoord * 2.0 - 1.0;
    vec4 pNear = invViewProj * vec4(ndc, 1.0, 1.0);
    vec4 pFar  = invViewProj * vec4(ndc, 0.0, 1.0);
    vec3 worldDir = normalize(pFar.xyz / pFar.w - pNear.xyz / pNear.w);

    // FOG-HORIZON-CLAMP-1 (mirrors GL shaders/fog_oob.frag). elevSin = -worldDir.z
    // is the sine of the ray's elevation above the horizon (z<0 = up in this
    // frame). Full fog at/below the horizon, smooth fade to zero across
    // [START_SIN, END_SIN], clear above -- keeps the cloud bank from bleeding
    // upward into the sky. DIVERGENCE: the GL path passes these band edges + a
    // runtime kill-switch (MC2_FOG_HORIZON_CLAMP / _FADE_START / _FADE_END) via
    // uniforms; this Vulkan island has no such UBO fields (its parity POD +
    // golden proof predate the feature), so the DEFAULT profile is baked as
    // constants here: start=0deg (sin 0), end=5deg (sin 0.08716), clamp ON. When
    // the Vulkan backend reaches this pass for real (Layer 6+), promote these to
    // FogOobParams UBO fields (int u_horizonClampEnabled; float startSin,endSin)
    // to regain the runtime knobs -- same descriptor work the SKYBOX-FOG-EXCLUDE
    // note above defers.
    const float FOG_HORIZON_START_SIN = 0.0;      // sin(0deg)
    const float FOG_HORIZON_END_SIN   = 0.08716;  // sin(5deg)
    float elevSin = -worldDir.z;
    if (elevSin >= FOG_HORIZON_END_SIN) { outFog = vec4(0.0); return; }
    float skyFade = 1.0 - smoothstep(FOG_HORIZON_START_SIN, FOG_HORIZON_END_SIN, elevSin);

    // Slow horizontal drift for cloud animation. Shift worldDir in XY plane.
    vec3 p3 = worldDir * 4.5 + vec3(u_time * 0.008, u_time * 0.002, 0.0);

    // 3D FBM on the sphere — naturally isotropic, no UV artifacts.
    float n = fbm3D(p3);

    // Solid cloud base + gentle surface variation from noise.
    float base   = 0.86;
    float ripple = (n - 0.50) * 0.28;
    float alpha  = clamp((base + ripple) * skyFade * u_fogOpacity, 0.0, 1.0);

    // Sunlit tops bright, shadowed hollows slightly grey.
    float lit = smoothstep(0.38, 0.68, n);
    vec3  col = mix(u_fogColor * 0.78, u_fogColor * 1.05, lit);

    outFog = vec4(clamp(col, 0.0, 1.0), alpha);
}
