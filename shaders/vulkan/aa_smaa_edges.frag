#version 450
#extension GL_GOOGLE_include_directive : require
#include "include/spatial_smaa.glsl"
layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D colorTex;
void main() {
    vec4 offsets[3];
    SMAAEdgeDetectionVS(TexCoord, offsets);
    outColor = vec4(SMAALumaEdgeDetectionPS(TexCoord, offsets, colorTex), 0.0, 0.0);
}
