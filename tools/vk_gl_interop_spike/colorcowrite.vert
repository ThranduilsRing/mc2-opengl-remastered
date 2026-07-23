#version 450
// VK "prop" draw into the shared color image. Push constant carries:
//   x0,x1 : NDC x-extent of this prop quad (so we can place it in a column band)
//   depth : reverse-Z clip depth the quad is tested at
// The quad spans full NDC height; x is remapped from [x0,x1].
layout(push_constant) uniform PC {
    float x0;
    float x1;
    float depth;
    float pad;
} pc;
void main() {
    // Two-triangle strip-ish via gl_VertexIndex 0..5 (triangle list, 6 verts).
    // Local unit quad corners.
    vec2 c[6] = vec2[6](
        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
        vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
    vec2 q = c[gl_VertexIndex];
    float ndcX = mix(pc.x0, pc.x1, q.x);
    float ndcY = q.y * 2.0 - 1.0;
    gl_Position = vec4(ndcX, ndcY, pc.depth, 1.0);
}
