#version 450
// Fullscreen quad. Emits UV in [0,1] across the target. Push constant carries
// the array layer to sample (float; used only for the 2D_ARRAY case, else 0).
layout(push_constant) uniform PC { float layer; float pad0; float pad1; float pad2; } pc;
layout(location = 0) out vec2 vUV;
layout(location = 1) out float vLayer;
void main() {
    vec2 c[6] = vec2[6](
        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
        vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
    vec2 q = c[gl_VertexIndex];
    vUV = q;
    vLayer = pc.layer;
    // NDC full-screen quad.
    gl_Position = vec4(q.x * 2.0 - 1.0, q.y * 2.0 - 1.0, 0.0, 1.0);
}
