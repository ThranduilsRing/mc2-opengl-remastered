// VK-REGION-HZB-1 -- Vulkan port of shaders/hzb_reduce.frag (HAND-MIRROR, same
// stopgap discipline as edge_fog/fog_oob/ssao until L6.5 folds the forks onto
// the single-source dual-target bake).
//
// MATH IS BIT-FOR-BIT THE GL SHADER'S:
//   * Reverse-Z, GL_ZERO_TO_ONE: near = 1.0, far = 0.0, larger depth = closer.
//   * Conservative occlusion reduction = MIN (parent stores the FARTHEST
//     occluder = smallest reverse-Z value). NOT max.
//   * Ceil mip ladder + clamped 2x2 fetch: CLAMP_TO_EDGE on the source sampler
//     makes the +/-0.5-texel taps safe on odd extents (no source texel dropped).
//   * min() of floats is bit-portable across the driver-GLSL and glslang
//     compilers -- this pass is byte-exact-provable (no transcendental caveat).
//
// Set/binding layout follows the postprocess-family convention (ladder §5a):
//   set=0 binding=0  uSrc sampler (GL texture unit 0 -> binding 0)
//   set=0 binding=1  params UBO (N samplers -> binding N = 1); std140, the
//                    pass's loose GL uniforms (uSrcTexel, uReduce) gathered in
//                    GL declaration order. No mat4 -> no row_major question.
//   no push constants; no vertex input (fullscreen triangle, TexCoord only).
#version 450

layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D uSrc;  // scene depth (seed) or HZB level i-1

layout(std140, set = 0, binding = 1) uniform HzbParams {
    vec2 uSrcTexel;   // @0: 1.0 / source-level dimensions
    int  uReduce;     // @8: 0 = seed copy (1:1), 1 = 2x2 MIN reduction
} params;

void main()
{
    if (params.uReduce == 0) {
        // Seed HZB level 0 with the raw reverse-Z scene depth (pass-through).
        FragColor = vec4(textureLod(uSrc, TexCoord, 0.0).r);
        return;
    }

    // 2x2 block centered on the destination texel center. NEAREST + the +/-0.5
    // source-texel offsets land squarely in the four source texels; on odd
    // extents the out-of-range tap clamps to the edge texel (conservative).
    vec2 t = params.uSrcTexel;
    float d00 = textureLod(uSrc, TexCoord + vec2(-0.5, -0.5) * t, 0.0).r;
    float d10 = textureLod(uSrc, TexCoord + vec2( 0.5, -0.5) * t, 0.0).r;
    float d01 = textureLod(uSrc, TexCoord + vec2(-0.5,  0.5) * t, 0.0).r;
    float d11 = textureLod(uSrc, TexCoord + vec2( 0.5,  0.5) * t, 0.0).r;

    // MIN = farthest occluder = conservative reverse-Z HZB.
    FragColor = vec4(min(min(d00, d10), min(d01, d11)));
}
