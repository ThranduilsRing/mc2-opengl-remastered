#version 450
// Sample a 2D texture at vUV. Used for RGBA8 / BGRA / BC7 / sRGB / LINEAR cases.
layout(set = 0, binding = 0) uniform sampler2D srcTex;
layout(location = 0) in vec2 vUV;
layout(location = 1) in float vLayer;
layout(location = 0) out vec4 o;
void main() { o = texture(srcTex, vUV); }
