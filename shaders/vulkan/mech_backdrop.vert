#version 450
// Mech-lab panel backdrop: one fullscreen triangle over the panel viewport. uv (0..1) is framebuffer-oriented
// (row 0 = panel bottom, the target stores rows bottom-up); the fragment stage flips it to panel y-down.
layout(location = 0) out vec2 vUv;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    vUv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
