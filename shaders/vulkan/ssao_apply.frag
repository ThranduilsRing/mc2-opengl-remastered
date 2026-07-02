// VK-REGION-SSAO-1 -- Vulkan port of shaders/ssao_apply.frag (apply the
// half-res AO buffer to the scene, 4-tap box upsample + denoise).
//
// MATH is bit-for-bit identical to the GL ssao_apply.frag. ONLY the
// uniform/sampler plumbing changed:
//   set=0,binding=0  ssaoTex   -> sampler2D (half-res AO). The 4-tap box below
//                                 samples at +/-0.5-texel offsets; GL's
//                                 ssaoColorTex_ is GL_LINEAR, so those off-center
//                                 taps get hardware bilinear interpolation ON TOP
//                                 of the box. N0 BUG-2: the island therefore binds
//                                 a LINEAR sampler for this image (vulkan_ssao_
//                                 island.cpp occSampler), NOT NEAREST -- a NEAREST
//                                 sampler snaps each tap to a texel center and
//                                 produces a blockier, non-GL-matching upsample.
//   set=0,binding=1  SSAOApplyParams -> std140 UBO: ssaoTexel + debugMode.
//
// Multiplicative apply is expressed by the CALLER's blend state (Vulkan
// pipeline blend = DST_COLOR/ZERO, matching the GL BLENDMODE-MULTIPLY-1 row
// PostProcessSsaoApply), NOT by this shader reading the destination -- same
// contract as the GL path (glBlendFunc set by pipeline_binder::applyPipeline
// before the draw).
#version 450

layout(location = 0) in  vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D ssaoTex;

layout(set = 0, binding = 1, std140) uniform SSAOApplyParams {
    vec2  ssaoTexel;   // @0  1/halfWidth, 1/halfHeight
    int   debugMode;   // @8  0 = multiply scene, 1 = show AO grayscale
} u;

void main()
{
    float ao = 0.0;
    ao += texture(ssaoTex, TexCoord + vec2(-0.5, -0.5) * u.ssaoTexel).r;
    ao += texture(ssaoTex, TexCoord + vec2( 0.5, -0.5) * u.ssaoTexel).r;
    ao += texture(ssaoTex, TexCoord + vec2(-0.5,  0.5) * u.ssaoTexel).r;
    ao += texture(ssaoTex, TexCoord + vec2( 0.5,  0.5) * u.ssaoTexel).r;
    ao *= 0.25;

    if (u.debugMode != 0) {
        FragColor = vec4(ao, ao, ao, 1.0);
        return;
    }
    FragColor = vec4(ao, ao, ao, 1.0);
}
