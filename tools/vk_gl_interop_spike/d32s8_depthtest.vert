#version 450
// Fullscreen triangle. Push constant carries the reverse-Z clip depth the VK
// draw is tested at (VK clip space: gl_Position.z in [0,1], no /w since w=1).
layout(push_constant) uniform PC { float vkDepth; } pc;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, pc.vkDepth, 1.0);
}
