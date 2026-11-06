// Camera motion by depth reprojection (docs/plans/2026-11-06-temporal-aa.md).
// Output 0: previousUV - currentUV in UV units on the unjittered grid; zero on sky (reverse-Z
// depth 0) and on history reset (identity reprojection). The scene depth was drawn with the
// frame's render jitter jitterClip, so it is removed before reprojecting (plan: depth algebra).
// Output 1: reactive/transparency mask for the temporal consumer; cleared to 0 here, writers per
// coverage row land with the resolve slices.
#version 450

layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec2 Motion;
layout(location = 1) out float Reactive;

layout(set = 0, binding = 0) uniform sampler2D depthTex;

layout(push_constant) uniform Params {
    mat4 previousFromCurrentClip;
    vec2 jitterClip;
} u;

void main() {
    float depth = texture(depthTex, TexCoord).r;
    vec2 ndcU = TexCoord * 2.0 - 1.0 - u.jitterClip;
    vec4 oldClip = u.previousFromCurrentClip * vec4(ndcU, depth, 1.0);
    Motion = oldClip.w > 0.0 ? 0.5 * oldClip.xy / oldClip.w + 0.5 - (TexCoord - 0.5 * u.jitterClip) : vec2(0.0);
    if (depth == 0.0 || u.previousFromCurrentClip == mat4(1.0)) Motion = vec2(0.0);
    Reactive = 0.0;
}
