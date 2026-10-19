// Reverse-Z: each texel stores the farthest occluder in its exact ceil-ladder tile.
#version 450
layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 0) uniform sampler2D uSrc;
layout(std140, set = 0, binding = 1) uniform HzbParams {
    vec2 uSrcTexel;
    int uReduce;
} params;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    if (params.uReduce == 0) {
        FragColor = vec4(texelFetch(uSrc, p, 0).r);
        return;
    }
    p *= 2;
    ivec2 hi = textureSize(uSrc, 0) - 1;
    float d = texelFetch(uSrc, min(p, hi), 0).r;
    d = min(d, texelFetch(uSrc, min(p + ivec2(1,0), hi), 0).r);
    d = min(d, texelFetch(uSrc, min(p + ivec2(0,1), hi), 0).r);
    d = min(d, texelFetch(uSrc, min(p + ivec2(1,1), hi), 0).r);
    FragColor = vec4(d);
}
