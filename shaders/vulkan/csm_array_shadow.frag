// VK-COMPARE-SAMPLER-FIXTURE-1 -- de-risks the CSM 2D_ARRAY depth-compare
// sampler that L7.3 Terrain / dynamic-CSM shadows need (the Vulkan analog of GL
// sampler2DArrayShadow with per-layer cascade select). Same hand-mirror stopgap
// discipline as compare_shadow.frag.
//
// The GL consumer is shaders/include/shadow.hglsl calcDynamicShadow() CSM path
// (MC2_SHADOW_CSM), csmSampleLayer():
//     texture(dynamicShadowArray, vec4(projCoords.xy + off, float(layer), ref))
// where `dynamicShadowArray` is a sampler2DArrayShadow. The compare hardware is
// identical to the 2D case, only with an extra array-layer coordinate: the
// 4-vector is (s, t, layer, ref). Under compareOp LESS -> ref < stored ? 1 : 0,
// filtered.
//
// BIT-TIGHT design: renders 1:1, samples at the exact texel center with a
// NEAREST compare sampler (one covered texel -> deterministic 0/1). The frag
// does a per-texel cascade SELECT (exercising the per-layer path) and derives
// both layer and ref from the integer texel index so the CPU mirrors it exactly.
// Tolerance 0.
//
// Set/binding layout (ladder §5a):
//   set=0 binding=0  uShadowArray sampler2DArrayShadow (N-layer depth + compare)
//   set=0 binding=1  params UBO (std140): image dims + layer count.
#version 450

layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2DArrayShadow uShadowArray;

layout(std140, set = 0, binding = 1) uniform CsmParams {
    vec2 uDims;     // @0: (width, height) of each array layer (== output dims)
    int  uLayers;   // @8: number of cascade layers
    int  _pad0;     // @12
} params;

void main()
{
    ivec2 texel = ivec2(floor(TexCoord * params.uDims));
    int   w     = int(params.uDims.x);
    int   idx   = texel.y * w + texel.x;

    // Per-texel cascade select: cycles through all layers so every layer's
    // compare path is exercised. Mirrored on the CPU.
    int   layer = idx % params.uLayers;

    // Deterministic reference pattern (exact binary fraction k/256). Uses layer
    // in the mix so different layers get distinct refs against distinct stored
    // patterns.
    float ref = float(((idx * 37) + (layer * 91)) & 0xFF) / 256.0;

    vec2 uv = (vec2(texel) + 0.5) / params.uDims;

    // sampler2DArrayShadow: (s, t, layer, ref). compareOp LESS.
    float result = texture(uShadowArray, vec4(uv, float(layer), ref));
    FragColor = vec4(result);
}
