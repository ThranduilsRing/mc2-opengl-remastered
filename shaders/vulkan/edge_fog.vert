// VULKAN-EDGE-FOG-ISLAND-2a -- fullscreen-triangle vertex shader for the Vulkan
// edge-fog island. Mirrors shaders/vulkan/fullscreen.vert exactly: emits a single
// oversized triangle covering the viewport with no vertex buffer. The interpolated
// TexCoord is the [0,1] screen UV the GL edge_fog.frag calls "TexCoord" (same as
// the GL postprocess.vert output), so the ported fragment math is bit-for-bit the
// same input as the GL path.
#version 450

layout(location = 0) out vec2 TexCoord;

void main() {
    // gl_VertexIndex in {0,1,2} -> fullscreen triangle. TexCoord in [0,1] over the
    // visible region (identical construction to fullscreen.vert's vUV).
    TexCoord = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(TexCoord * 2.0 - 1.0, 0.0, 1.0);
}
