#version 450
#extension GL_GOOGLE_include_directive : require
#include "include/spatial_smaa.glsl"
layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D colorTex;
layout(set = 0, binding = 1) uniform sampler2D weightsTex;
void main() {
    vec4 offset;
    SMAANeighborhoodBlendingVS(TexCoord, offset);
    outColor = SMAANeighborhoodBlendingPS(TexCoord, offset, colorTex, weightsTex);
    // colorTex is an sRGB view. Re-encode the linear blend for the UNORM present attachment.
    vec3 linearColor = max(outColor.rgb, vec3(0.0));
    outColor.rgb = mix(1.055 * pow(linearColor, vec3(1.0 / 2.4)) - 0.055,
                      12.92 * linearColor, lessThanEqual(linearColor, vec3(0.0031308)));
    outColor.a = 1.0;
}
