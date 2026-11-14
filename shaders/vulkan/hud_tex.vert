#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in uint inArgb;
layout(location = 2) in vec2 inUv;

layout(push_constant) uniform HudPush {
    vec2 extent;
    uint texW;
    uint texH;
    uint textMode;
    uint foreground;
} pc;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUv;
layout(location = 2) noperspective out vec2 vSceneUv;

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
    gl_Position = vec4(ndc, 0.0, 1.0) / max(inPos.z, 0.000001);
    vSceneUv = ndc * 0.5 + 0.5;
    vColor = unpackArgb(inArgb);
    vUv = inUv;
}
