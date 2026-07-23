// VK-POST-COMPOSITE-NATIVE-1: fullscreen triangle vertex shader for the
// postprocess composite port. Mirrors shaders/postprocess.vert semantics using
// Vulkan explicit locations and no vertex buffer.
#version 450

layout(location = 0) out vec2 TexCoord;

void main() {
    TexCoord = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(TexCoord * 2.0 - 1.0, 0.0, 1.0);
}
