//#version 400 (version provided by material prefix)

layout(vertices = 3) out;

in vec4 vs_Color[];
in float vs_FogValue[];
in vec2 vs_Texcoord[];
in float vs_TerrainType[];
in vec3 vs_WorldPos[];
in vec3 vs_WorldNorm[];

out vec4 tcs_Color[];
out float tcs_FogValue[];
out vec2 tcs_Texcoord[];
out float tcs_TerrainType[];
out vec3 tcs_WorldPos[];
out vec3 tcs_WorldNorm[];

uniform vec4 tessLevel;         // x=inner, y=outer
uniform vec4 tessDistanceRange; // x=near, y=far
uniform vec4 cameraPos;

void main()
{
    // Passthrough per-vertex attributes
    tcs_Color[gl_InvocationID]       = vs_Color[gl_InvocationID];
    tcs_FogValue[gl_InvocationID]    = vs_FogValue[gl_InvocationID];
    tcs_Texcoord[gl_InvocationID]    = vs_Texcoord[gl_InvocationID];
    tcs_TerrainType[gl_InvocationID] = vs_TerrainType[gl_InvocationID];
    tcs_WorldPos[gl_InvocationID]    = vs_WorldPos[gl_InvocationID];
    tcs_WorldNorm[gl_InvocationID]   = vs_WorldNorm[gl_InvocationID];

    // Pass through the already-projected position from VS
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;

    // Only invocation 0 sets tessellation levels
    if (gl_InvocationID == 0) {
        // Uniform level — no distance LOD (MC2 terrain is low-poly enough to always tessellate)
        float level = max(tessLevel.x, 1.0);
        gl_TessLevelOuter[0] = level;
        gl_TessLevelOuter[1] = level;
        gl_TessLevelOuter[2] = level;
        gl_TessLevelInner[0] = level;
    }
}
