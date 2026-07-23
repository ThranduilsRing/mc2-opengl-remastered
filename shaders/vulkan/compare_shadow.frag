// VK-COMPARE-SAMPLER-FIXTURE-1 -- de-risks the depth-COMPARE sampler capability
// that L7.2 Shadow needs (the Vulkan analog of GL sampler2DShadow /
// GL_COMPARE_REF_TO_TEXTURE). Same hand-mirror stopgap discipline as
// edge_fog / hzb_reduce.
//
// The GL consumer is shaders/include/shadow.hglsl calcShadow():
//     texture(shadowMap, vec3(projCoords.xy, currentDepth))
// where `shadowMap` is a sampler2DShadow bound to a depth texture with
// GL_TEXTURE_COMPARE_MODE = GL_COMPARE_REF_TO_TEXTURE. Under compareOp LESS the
// hardware returns (ref < stored ? 1.0 : 0.0), then the (P)CF filter averages
// the 0/1 results of the covered texels.
//
// BIT-TIGHT design: the fixture renders 1:1 (output texel <-> depth texel) and
// samples at the EXACT texel center with a NEAREST-filtered compare sampler, so
// exactly one stored texel is covered and the compare result is a deterministic
// 0.0 or 1.0 (no ambiguous filter weights). `ref` is derived from the integer
// texel index so the CPU reference mirrors it byte-for-byte. Tolerance 0.
//
// Set/binding layout follows the postprocess-family convention (ladder §5a):
//   set=0 binding=0  uShadow sampler2DShadow (depth + compare)
//   set=0 binding=1  params UBO (std140): image dims (drives texel-center UV +
//                    the deterministic per-texel ref pattern).
#version 450

layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2DShadow uShadow; // depth-compare

layout(std140, set = 0, binding = 1) uniform CompareParams {
    vec2 uDims;   // @0: (width, height) of the depth image (== output dims)
    int  _pad0;   // @8
    int  _pad1;   // @12
} params;

void main()
{
    // Integer texel this fragment owns (1:1 render). floor() of the [0,dim] UV
    // scaled by dims; exact for our power-of-two-ish small dims.
    ivec2 texel = ivec2(floor(TexCoord * params.uDims));
    int   w     = int(params.uDims.x);
    int   idx   = texel.y * w + texel.x;

    // Deterministic reference-depth pattern, exact binary fractions (k/256).
    // Mirrored EXACTLY on the CPU side. Chosen so ref straddles the stored
    // depth pattern -> a mix of pass (1.0) and fail (0.0) outcomes.
    float ref = float((idx * 37) & 0xFF) / 256.0;

    // Texel-center UV for a NEAREST compare tap (lands squarely on one texel).
    vec2 uv = (vec2(texel) + 0.5) / params.uDims;

    // sampler2DShadow: (s, t, ref). compareOp LESS -> ref < stored ? 1 : 0.
    float result = texture(uShadow, vec3(uv, ref));
    FragColor = vec4(result);
}
