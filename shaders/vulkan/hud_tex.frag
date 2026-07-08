#version 450

layout(std430, binding = 0) readonly buffer HudTexels {
    uint texels[];
} tex;

layout(push_constant) uniform HudPush {
    vec2 extent;
    uint texW;
    uint texH;
    uint textMode;
    uint foreground;
} pc;

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUv;
layout(location = 0) out vec4 outColor;

vec4 unpackRgba(uint c)
{
    float r = float( c        & 0xffu) / 255.0;
    float g = float((c >>  8) & 0xffu) / 255.0;
    float b = float((c >> 16) & 0xffu) / 255.0;
    float a = float((c >> 24) & 0xffu) / 255.0;
    return vec4(r, g, b, a);
}

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
    uint x = uint(clamp(vUv.x, 0.0, 0.999999) * float(pc.texW));
    uint y = uint(clamp(vUv.y, 0.0, 0.999999) * float(pc.texH));
    vec4 sampleColor = unpackRgba(tex.texels[y * pc.texW + x]);
    if (pc.textMode != 0u) {
        outColor = unpackArgb(pc.foreground) * sampleColor.rrrr;
    } else {
        outColor = vColor * sampleColor;
    }
}
