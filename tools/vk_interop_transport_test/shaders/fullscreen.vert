#version 450
// Fullscreen triangle (no vertex buffer). Emits UV in [0,1] over the screen.
layout(location = 0) out vec2 vUV;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vUV = p;                         // (0,0) (2,0) (0,2)
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
