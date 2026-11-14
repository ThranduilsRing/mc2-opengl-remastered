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

vec4 mipSample(vec2 uv, uint level)
{
    uint w = pc.texW >> level, offset = (pc.texW * pc.texW - w*w) * 4u / 3u;
    vec2 p = clamp(uv * float(w) - 0.5, vec2(0), vec2(w-1u)), f = fract(p);
    uvec2 a = uvec2(p), b = min(a+1u, uvec2(w-1u));
    return mix(mix(unpackRgba(tex.texels[offset+a.y*w+a.x]), unpackRgba(tex.texels[offset+a.y*w+b.x]), f.x),
               mix(unpackRgba(tex.texels[offset+b.y*w+a.x]), unpackRgba(tex.texels[offset+b.y*w+b.x]), f.x), f.y);
}

void main()
{
    uint x = uint(clamp(vUv.x, 0.0, 0.999999) * float(pc.texW));
    uint y = uint(clamp(vUv.y, 0.0, 0.999999) * float(pc.texH));
    vec4 sampleColor = unpackRgba(tex.texels[y * pc.texW + x]);
    if (pc.textMode == 2u) {
        vec2 dx = dFdx(vUv) * float(pc.texW), dy = dFdy(vUv) * float(pc.texH);
        float lod = clamp(log2(max(max(length(dx), length(dy)), 1.0)), 0.0, 11.0);
        uint level = uint(lod);
        sampleColor = mix(mipSample(vUv, level), mipSample(vUv, min(level+1u, 11u)), fract(lod));
        sampleColor.rgb /= max(sampleColor.a, 0.000001);
    }
    if (pc.textMode == 1u) {
        outColor = unpackArgb(pc.foreground) * sampleColor.rrrr;
    } else {
        outColor = vColor * sampleColor;
    }
}
