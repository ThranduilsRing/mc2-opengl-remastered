// WATER-MVP-ORACLE-1 -- fullscreen-triangle vertex shader for the water-MVP
// convention oracle. Mirrors shaders/vulkan/edge_fog.vert / fullscreen.vert
// exactly: emits a single oversized triangle covering the viewport with no vertex
// buffer. The interpolated TexCoord is the [0,1] screen UV; the fragment shader
// uses gl_FragCoord (pixel index) to select which KNOWN worldPos test point it
// projects. All the transform math under test lives in water_mvp.frag so the
// convention (row_major vs column_major pack) is fully controlled there.
#version 450

layout(location = 0) out vec2 TexCoord;

void main() {
    TexCoord = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(TexCoord * 2.0 - 1.0, 0.0, 1.0);
}
