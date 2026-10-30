#version 450
#extension GL_GOOGLE_include_directive : require
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 outColor;
layout(set=0,binding=0) uniform sampler2D sceneTex;
layout(push_constant) uniform Params { vec4 metrics; } aa;
#define A_GPU 1
#define A_GLSL 1
#define FSR_RCAS_F 1
#include "../../3rdparty/fsr1/ffx_a.h"
#include "../../3rdparty/fsr1/ffx_fsr1.h"
AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(sceneTex, clamp(p, ivec2(0), textureSize(sceneTex,0)-1), 0); }
void FsrRcasInputF(inout AF1 r,inout AF1 g,inout AF1 b) {}
void main() {
    uvec4 con; FsrRcasCon(con, 0.5);
    vec3 color; FsrRcasF(color.r,color.g,color.b,uvec2(gl_FragCoord.xy),con);
    outColor=vec4(color,1);
}
