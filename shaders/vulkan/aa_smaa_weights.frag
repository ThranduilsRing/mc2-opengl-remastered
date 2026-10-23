#version 450
#extension GL_GOOGLE_include_directive : require
#include "include/spatial_smaa.glsl"
layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D edgesTex;
layout(set = 0, binding = 1) uniform sampler2D areaTex;
layout(set = 0, binding = 2) uniform sampler2D searchTex;
void main() {
    vec4 offsets[3]; vec2 pixcoord;
    SMAABlendingWeightCalculationVS(TexCoord, pixcoord, offsets);
    outColor = SMAABlendingWeightCalculationPS(TexCoord, pixcoord, offsets, edgesTex, areaTex, searchTex, vec4(0.0));
}
