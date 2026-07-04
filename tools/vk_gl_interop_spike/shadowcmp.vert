#version 450
// Fullscreen quad for the shadow-compare parity spike. No inputs needed beyond
// gl_VertexIndex; the fragment shader derives the (uv, refDepth) grid point from
// gl_FragCoord and the grid params in the push constant / uniform.
void main() {
    vec2 c[6] = vec2[6](
        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
        vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
    vec2 q = c[gl_VertexIndex];
    gl_Position = vec4(q.x * 2.0 - 1.0, q.y * 2.0 - 1.0, 0.0, 1.0);
}
