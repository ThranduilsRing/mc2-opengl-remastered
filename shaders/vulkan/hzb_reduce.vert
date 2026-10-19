// Fullscreen triangle for native Vulkan depth reduction.
#version 450

layout(location = 0) out vec2 TexCoord;

void main() {
    TexCoord = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(TexCoord * 2.0 - 1.0, 0.0, 1.0);
}
