// SLICE 3a: LOD-chunk tessellation EVALUATION shader. Barycentric interpolation
// of the corner outputs -> at any tess level, with no displacement, lands on the
// SAME plane as the original triangle (structurally equivalent; strict byte-
// identity is a gate-OFF property only). Crack-free (no displacement moves shared
// edges). ccw matches the CCW index winding. 3b will add N*detail displacement.
layout(triangles, equal_spacing, ccw) in;

in  vec3  tc_worldPos[];
in  float tc_terrainType[];
in  vec4  tc_clip[];

out vec3  v_worldPos;
out float v_terrainType;

void main() {
    vec3 b = gl_TessCoord;
    v_worldPos    = b.x * tc_worldPos[0]    + b.y * tc_worldPos[1]    + b.z * tc_worldPos[2];
    v_terrainType = b.x * tc_terrainType[0] + b.y * tc_terrainType[1] + b.z * tc_terrainType[2];
    gl_Position   = b.x * tc_clip[0]        + b.y * tc_clip[1]        + b.z * tc_clip[2];
}
