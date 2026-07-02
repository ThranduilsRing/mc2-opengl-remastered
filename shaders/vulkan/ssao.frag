// VK-REGION-SSAO-1 -- Vulkan port of shaders/ssao.frag (SSAO-GTAO-LITE-MVP-1
// occlusion pass, half-res world-space hemisphere AO).
//
// MATH is bit-for-bit identical to the GL ssao.frag. ONLY the uniform/sampler
// plumbing changed: the GL default-block uniforms became explicit Vulkan
// set/binding layouts:
//   set=0,binding=0  sceneDepthTex   -> sampler2D (full-res scene depth)
//   set=0,binding=1  sceneNormalTex  -> sampler2D (GBuffer1 world normal)
//   set=0,binding=2  SSAOParams      -> std140 UBO mirroring the GL uniforms.
//     invViewProj/viewProj are the SAME 16 floats the GL path uploads
//     (row-major direct upload, GL_FALSE) -- qualify row_major (the
//     VULKAN-EDGE-FOG-ISLAND-2b matrix bug applies here too).
//
// NOTE ON THE BAKED SPIR-V FAMILY: shaders/spv/ssao.frag.default.*.spv
// (SPIRV-POSTPROCESS-FAMILY-1) is NOT reusable here -- it was compiled
// without explicit `layout(binding=)` in the GLSL source, so BOTH
// sceneDepthTex and sceneNormalTex decorate to (set=0,binding=0) (verified
// via spirv-dis: two OpDecorate ... Binding 0 on distinct sampler ids). That
// artifact is valid for the GL glShaderBinary/glSpecializeShader consumer
// (which resolves samplers by reflection, not descriptor binding) but is a
// genuine descriptor-set collision for a Vulkan pipeline -- it cannot be
// consumed as-is. This file is therefore a hand-port (same discipline as
// edge_fog/fog_oob), baked to its own ssao.frag.spv under MC2_VULKAN.
//
// Convention matches edge_fog.frag / shadow_screen.frag: reverse-Z, far=0,
// near=1, larger=closer; sky/cleared depth ~0.0 (NOT 1.0). No Y-flip needed:
// the pass reads TexCoord (interpolated [0,1] UV), not gl_FragCoord, and the
// island uploads the depth/normal images so texel (u,v) matches the GL sample.
#version 450

layout(location = 0) in  vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D sceneDepthTex;
layout(set = 0, binding = 1) uniform sampler2D sceneNormalTex;

// std140. Offsets (bytes) match the CPU-side POD in vulkan_ssao_island.cpp:
//   invViewProj  @  0   (mat4, 64B)
//   viewProj     @ 64   (mat4, 64B)
//   screenSize   @128   (vec2, 8B)
//   aoRadius     @136
//   aoBias       @140
//   aoStrength   @144
//   aoPower      @148
layout(set = 0, binding = 2, std140, row_major) uniform SSAOParams {
    mat4  invViewProj;
    mat4  viewProj;
    vec2  screenSize;
    float aoRadius;
    float aoBias;
    float aoStrength;
    float aoPower;
} u;

// 16-sample cosine-ish hemisphere kernel (z > 0). Fixed (deterministic).
// IDENTICAL to shaders/ssao.frag's kKernel.
const vec3 kKernel[16] = vec3[](
    vec3( 0.024, -0.012,  0.040), vec3(-0.045,  0.039,  0.052),
    vec3( 0.071,  0.061,  0.090), vec3(-0.083, -0.052,  0.071),
    vec3( 0.012,  0.114,  0.140), vec3( 0.131, -0.078,  0.121),
    vec3(-0.151,  0.026,  0.180), vec3( 0.067,  0.175,  0.205),
    vec3(-0.092, -0.196,  0.171), vec3( 0.226,  0.041,  0.241),
    vec3(-0.198,  0.158,  0.276), vec3( 0.046, -0.281,  0.252),
    vec3( 0.258,  0.182,  0.330), vec3(-0.314, -0.072,  0.301),
    vec3( 0.121,  0.341,  0.388), vec3(-0.279,  0.252,  0.421)
);

vec3 reconstructWorldPos(vec2 uv, float depth)
{
    vec2 ndc_xy = uv * 2.0 - 1.0;
    float ndc_z = depth;  // ZERO_TO_ONE: window depth == NDC z
    vec4 w = u.invViewProj * vec4(ndc_xy, ndc_z, 1.0);
    return w.xyz / w.w;
}

float hash12(vec2 p)
{
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

void main()
{
    float depth = texture(sceneDepthTex, TexCoord).r;

    if (depth <= 0.0001) {
        FragColor = vec4(1.0);
        return;
    }

    vec3 P = reconstructWorldPos(TexCoord, depth);
    vec3 N = normalize(texture(sceneNormalTex, TexCoord).xyz * 2.0 - 1.0);

    float rnd = hash12(TexCoord * u.screenSize);
    float ca = cos(rnd * 6.2831853);
    float sa = sin(rnd * 6.2831853);
    vec3 randomVec = normalize(vec3(ca, sa, 0.5));
    vec3 tangent   = normalize(randomVec - N * dot(randomVec, N));
    vec3 bitangent = cross(N, tangent);
    mat3 TBN = mat3(tangent, bitangent, N);

    float occlusion = 0.0;
    for (int i = 0; i < 16; ++i) {
        vec3 sampleWorld = P + (TBN * kKernel[i]) * u.aoRadius;

        vec4 clip = u.viewProj * vec4(sampleWorld, 1.0);
        if (clip.w <= 0.0) continue;
        vec3 ndc = clip.xyz / clip.w;
        vec2 sUV = ndc.xy * 0.5 + 0.5;
        if (sUV.x < 0.0 || sUV.x > 1.0 || sUV.y < 0.0 || sUV.y > 1.0) continue;

        float sampleProjDepth = ndc.z;
        float storedDepth = texture(sceneDepthTex, sUV).r;
        if (storedDepth <= 0.0001) continue;

        bool occluded = storedDepth > sampleProjDepth + u.aoBias;

        vec3 storedWorld = reconstructWorldPos(sUV, storedDepth);
        float dist = length(storedWorld - P);
        float rangeCheck = smoothstep(0.0, 1.0, u.aoRadius / max(dist, 1e-4));

        occlusion += (occluded ? 1.0 : 0.0) * rangeCheck;
    }

    float ao = 1.0 - (occlusion / 16.0) * u.aoStrength;
    ao = pow(clamp(ao, 0.0, 1.0), max(u.aoPower, 0.0001));
    FragColor = vec4(ao, ao, ao, 1.0);
}
