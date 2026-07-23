// VK-COMPARE-SAMPLER-FIXTURE-1 -- fullscreen-triangle vertex shader for the CSM
// 2D_ARRAY depth-compare fixture. Identical construction to compare_shadow.vert
// (no vertex buffer, TexCoord = [0,1] screen UV, 1:1 output<->depth texel).
#version 450

layout(location = 0) out vec2 TexCoord;

void main() {
    TexCoord = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(TexCoord * 2.0 - 1.0, 0.0, 1.0);
}
