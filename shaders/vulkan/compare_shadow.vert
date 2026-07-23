// VK-COMPARE-SAMPLER-FIXTURE-1 -- fullscreen-triangle vertex shader for the
// depth-COMPARE sampler fixture. Mirrors hzb_reduce.vert / ssao.vert exactly:
// emits a single oversized triangle covering the viewport with no vertex
// buffer. The interpolated TexCoord is the [0,1] screen UV; each output texel
// maps 1:1 onto one depth texel (fixture renders at the depth image's exact
// dimensions), so the sampled tap lands on a texel center under NEAREST.
#version 450

layout(location = 0) out vec2 TexCoord;

void main() {
    TexCoord = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(TexCoord * 2.0 - 1.0, 0.0, 1.0);
}
