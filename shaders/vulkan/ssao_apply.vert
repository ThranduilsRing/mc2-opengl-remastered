// VK-REGION-SSAO-1 -- fullscreen-triangle vertex shader for the Vulkan SSAO
// apply pass. Identical to ssao.vert / edge_fog.vert (same fullscreen-triangle
// construction); a separate file only because the CMake bake loop expects one
// vert per basename. Emits a single oversized triangle covering the viewport
// with no vertex buffer.
#version 450

layout(location = 0) out vec2 TexCoord;

void main() {
    TexCoord = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(TexCoord * 2.0 - 1.0, 0.0, 1.0);
}
