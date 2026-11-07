// Camera motion by depth reprojection (docs/plans/2026-11-06-temporal-aa.md).
// Output 0: previousUV - currentUV in UV units on the unjittered grid; sky (reverse-Z
// depth 0) reprojects by view rotation only; zero on history reset (identity reprojection). The scene depth was drawn with the
// frame's render jitter jitterClip, so it is removed before reprojecting (plan: depth algebra).
// Output 1: reactive/transparency mask for the temporal consumer; cleared to 0 here, writers per
// coverage row land with the resolve slices.
#version 450

layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec2 Motion;
layout(location = 1) out float Reactive;

layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(set = 0, binding = 1) uniform sampler2D objectMv;   // per-object vectors (mech island); (1e4, 1e4) = none

layout(push_constant) uniform Params {
    mat4 previousFromCurrentClip;
    vec4 skyRowX;   // rotation-only reprojection rows (x, y, w) for depth-0 pixels
    vec4 skyRowY;
    vec4 skyRowW;
    vec2 jitterClip;
    uint objectValid;   // 1 = objectMv was written this frame
} u;

void main() {
    float depth = texture(depthTex, TexCoord).r;
    vec2 ndcU = TexCoord * 2.0 - 1.0 - u.jitterClip;
    vec4 oldClip = u.previousFromCurrentClip * vec4(ndcU, depth, 1.0);
    Motion = oldClip.w > 0.0 ? 0.5 * oldClip.xy / oldClip.w + 0.5 - (TexCoord - 0.5 * u.jitterClip) : vec2(0.0);
    if (depth == 0.0) {
        // Sky: a direction at infinity; only the view rotation moves it. skyRow* map (ndc, 0.5, 1) to
        // previous clip of the world direction through this pixel (translation dropped on the CPU).
        vec4 v = vec4(ndcU, 0.5, 1.0);
        float w = dot(u.skyRowW, v);
        Motion = w > 0.0 ? 0.5 * vec2(dot(u.skyRowX, v), dot(u.skyRowY, v)) / w + 0.5 - (TexCoord - 0.5 * u.jitterClip) : vec2(0.0);
    }
    if (u.previousFromCurrentClip == mat4(1.0)) Motion = vec2(0.0);
    if (u.objectValid != 0u) {
        // Mechs and vehicles co-wrote their own vector under the same depth test; the sentinel marks every other pixel.
        vec2 objectMotion = texelFetch(objectMv, ivec2(gl_FragCoord.xy), 0).xy;
        if (objectMotion.x < 1000.0) Motion = objectMotion;
    }
    Reactive = 0.0;
}
