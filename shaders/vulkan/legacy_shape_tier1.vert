// VK-PARITY-F1 LEGACY SHAPE TIER-1 -- VS.
// =============================================================================
// Port of shaders/gos_tex_vertex.vert: the basic gos_VERTEX material that
// gosRenderer::drawIndexedTris uses for the legacy MLR solid-object loop (ground
// vehicles, every TG_Shape whose triangles are CPU-transformed + CPU-lit into
// gos_VERTEX arrays). The vertices are PRE-TRANSFORMED screen-space:
//   a_pos = (x_px, y_px, z, rhw)
//   clip  = (proj * vec4(xyz,1)) / rhw     (=> NDC (ndcx, ndcy, z) after the
//                                              hardware divide, perspective-correct uv)
// where proj is gosRenderer::projection_ (row-major screen->NDC, uploaded by GL with
// transpose=GL_TRUE -- hence the transpose() below to reproduce the exact GLSL matrix).
#version 450

layout(location = 0) in vec4 a_pos;      // x, y, z, rhw
layout(location = 1) in vec4 a_color;    // R8G8B8A8_UNORM of the DWORD argb: x=B y=G z=R w=A (GL attrib order)
layout(location = 2) in vec4 a_fog;      // R8G8B8A8_UNORM of the DWORD frgb: .w = fog value
layout(location = 3) in vec2 a_uv;

layout(set = 0, binding = 0, std140) uniform LegacyFrameUBO {
    vec4 proj0, proj1, proj2, proj3;     // gos_GetProj2ScreenMat4() rows (row-major, GL_TRUE)
    vec4 fogColor;                       // gosRenderer fog_color_ (0 = no fog)
} uFrame;

layout(location = 0) out vec4  vColor;   // already .bgra-swizzled to (R,G,B,A)
layout(location = 1) out float vFogValue;
layout(location = 2) out vec2  vUv;

void main() {
    mat4 proj = transpose(mat4(uFrame.proj0, uFrame.proj1, uFrame.proj2, uFrame.proj3));
    vec4 p = proj * vec4(a_pos.xyz, 1.0);
    gl_Position = p / a_pos.w;
    vColor = a_color.zyxw;               // Color.bgra
    vFogValue = a_fog.w;
    vUv = a_uv;
}
