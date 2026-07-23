#version 450

layout(location = 0) in vec2 inPos;
layout(location = 1) in uint inArgb;

layout(push_constant) uniform HudPush {
    vec2 extent;
} pc;

layout(location = 0) out vec4 vColor;

vec4 unpackArgb(uint c)
{
    float a = float((c >> 24) & 0xffu) / 255.0;
    float r = float((c >> 16) & 0xffu) / 255.0;
    float g = float((c >>  8) & 0xffu) / 255.0;
    float b = float( c        & 0xffu) / 255.0;
    return vec4(r, g, b, a);
}

void main()
{
    vec2 ndc = vec2((inPos.x / pc.extent.x) * 2.0 - 1.0,
                    1.0 - (inPos.y / pc.extent.y) * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vColor = unpackArgb(inArgb);
}
