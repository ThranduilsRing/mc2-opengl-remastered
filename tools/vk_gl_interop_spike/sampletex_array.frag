#version 450
// Sample a 2D_ARRAY texture at (vUV, vLayer). Used for the 2D_ARRAY (props) case.
layout(set = 0, binding = 0) uniform sampler2DArray srcArr;
layout(location = 0) in vec2 vUV;
layout(location = 1) in float vLayer;
layout(location = 0) out vec4 o;
void main() { o = texture(srcArr, vec3(vUV, vLayer)); }
