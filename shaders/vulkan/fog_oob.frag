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
//   set=0,binding=1  FogOobParams    -> std140 UBO mirroring the GL uniforms.
//                                       (invViewProj packing convention is documented
//                                       at the layout declaration below -- column_major,
//                                       do NOT qualify row_major.)
//
// invViewProj depth convention: reverse-Z [0,1]. pNear uses ndc-depth 1.0 (near),
// pFar uses ndc-depth 0.0 (far) -- identical to the GL shader. No Y-flip is applied
// because the pass reads TexCoord (interpolated [0,1] UV), not gl_FragCoord, and the
// island uploads the depth image so texel (u,v) maps the same way as the GL sample.
#version 450

layout(location = 0) in  vec2 TexCoord;
layout(location = 0) out vec4 outFog;

layout(set = 0, binding = 0) uniform sampler2D depthTex;

// std140. Offsets (bytes) match the CPU-side POD in vulkan_oob_fog_island.cpp:
//   invViewProj  @  0  (mat4, 64B)
//   u_fogColor   @ 64  (vec3, 12B)
//   u_fogOpacity @ 76  (float; packs into the vec3's trailing slot in std140)
//   u_time       @ 80  (float)
// column_major (std140 default): the subgraph/island packs invViewProj as RAW
// inverseViewProj_ (column-major CPU bytes, index=col*4+row) -- the same bytes the GL
// net upload yields. GLSL reading them column-major = mul_colmajor is the correct
// unprojection; row_major reads transpose(M) and breaks it (L6.5 transpose regression).
// Same convention as edge_fog.frag / ssao.frag (N0). Do NOT qualify row_major.
layout(set = 0, binding = 1, std140) uniform FogOobParams {
    mat4  invViewProj;
    vec3  u_fogColor;
    float u_fogOpacity;
    float u_time;
    // VK-PARITY-F3: FOG-HORIZON-CLAMP-1 (default ON in GL). @84/88/92.
    int   u_horizonClampEnabled;
    float u_horizonFadeStartSin;
    float u_horizonFadeEndSin;
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

    vec2 ndc = TexCoord * 2.0 - 1.0;
    vec4 pNear = invViewProj * vec4(ndc, 1.0, 1.0);
    vec4 pFar  = invViewProj * vec4(ndc, 0.0, 1.0);
    vec3 worldDir = normalize(pFar.xyz / pFar.w - pNear.xyz / pNear.w);

    // VK-PARITY-F3: FOG-HORIZON-CLAMP-1 (verbatim from shaders/fog_oob.frag).
    float skyFade;
    if (u_horizonClampEnabled != 0) {
        float elevSin = -worldDir.z;
        if (elevSin >= u_horizonFadeEndSin) { outFog = vec4(0.0); return; }
        skyFade = 1.0 - smoothstep(u_horizonFadeStartSin, u_horizonFadeEndSin, elevSin);
    } else {
        // Legacy: worldDir.z < 0 = sky. Fade in from just-above-horizon.
        if (worldDir.z < -0.22) { outFog = vec4(0.0); return; }
        skyFade = smoothstep(-0.22, -0.01, worldDir.z);
    }

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
