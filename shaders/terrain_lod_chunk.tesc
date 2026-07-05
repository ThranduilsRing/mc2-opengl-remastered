// SLICE 3a: LOD-chunk tessellation CONTROL shader (pass-through). Triangle
// patches; tess level from a uniform. No #version (makeProgram prefix supplies
// "#version 430\n" + "#define TERRAIN_TESS").
layout(vertices = 3) out;

in  vec3  v_worldPos[];
in  float v_terrainType[];
in  vec4  v_clip[];

out vec3  tc_worldPos[];
out float tc_terrainType[];
out vec4  tc_clip[];

uniform float u_cliffTessLevel;   // C++ pre-clamps to GL_MAX_TESS_GEN_LEVEL

void main() {
    tc_worldPos[gl_InvocationID]    = v_worldPos[gl_InvocationID];
    tc_terrainType[gl_InvocationID] = v_terrainType[gl_InvocationID];
    tc_clip[gl_InvocationID]        = v_clip[gl_InvocationID];
    if (gl_InvocationID == 0) {
        float L = max(u_cliffTessLevel, 1.0);
        gl_TessLevelOuter[0] = L;
        gl_TessLevelOuter[1] = L;
        gl_TessLevelOuter[2] = L;
        gl_TessLevelInner[0] = L;
    }
}
