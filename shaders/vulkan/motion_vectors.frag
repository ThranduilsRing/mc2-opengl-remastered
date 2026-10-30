#version 450
layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec2 Motion;
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(set = 0, binding = 1) uniform sampler2D corrections;
layout(push_constant) uniform Params { mat4 previousFromCurrentClip; } u;
void main() {
    float depth = texture(depthTex, TexCoord).r;
    vec4 oldClip = u.previousFromCurrentClip * vec4(TexCoord * 2.0 - 1.0, depth, 1.0);
    Motion = oldClip.w > 0.0 ? 0.5 * oldClip.xy / oldClip.w + 0.5 - TexCoord : vec2(0.0);
    if (depth == 0.0 || u.previousFromCurrentClip == mat4(1.0)) Motion = vec2(0.0);
    // Transparent surfaces have their own depth; never reproject the terrain
    // underneath them. B marks a dynamic writer, including a zero-motion one.
    vec4 dynamicMotion = texture(corrections, TexCoord);
    if (dynamicMotion.b != 0.0) Motion = dynamicMotion.rg;
}
