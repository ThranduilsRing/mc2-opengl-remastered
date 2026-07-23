// VK shadow debug overlay: CSM dynamic-array diagnostic path.
#version 450

layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2DArray shadowDebugArray;

layout(push_constant) uniform Params {
    int layer;
    int _pad0;
    int _pad1;
    int _pad2;
} u;

void main() {
    float d = texture(shadowDebugArray, vec3(TexCoord, float(u.layer))).r;

    if (d >= 0.999) {
        FragColor = vec4(1.0, 0.0, 1.0, 1.0);
        return;
    }
    if (d <= 0.001) {
        FragColor = vec4(1.0, 0.0, 0.0, 1.0);
        return;
    }

    float v = pow(d, 0.5);
    FragColor = vec4(v, v, v, 1.0);
}
