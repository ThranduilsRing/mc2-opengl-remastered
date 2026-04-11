//#version 420 (version provided by material prefix)

layout(vertices = 3) out;

in vec3 vs_WorldPos[];
in vec3 vs_WorldNorm[];
in vec2 vs_Texcoord[];

out vec3 tcs_WorldPos[];
out vec3 tcs_WorldNorm[];
out vec2 tcs_Texcoord[];

uniform vec4 tessLevel;         // x=inner, y=outer
uniform vec4 tessDistanceRange; // x=near, y=far
uniform vec4 cameraPos;

void main()
{
    tcs_WorldPos[gl_InvocationID]  = vs_WorldPos[gl_InvocationID];
    tcs_WorldNorm[gl_InvocationID] = vs_WorldNorm[gl_InvocationID];
    tcs_Texcoord[gl_InvocationID]  = vs_Texcoord[gl_InvocationID];

    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;

    if (gl_InvocationID == 0) {
        // Match main terrain TCS — uniform level, no distance LOD
        float level = max(tessLevel.x, 1.0);
        gl_TessLevelOuter[0] = level;
        gl_TessLevelOuter[1] = level;
        gl_TessLevelOuter[2] = level;
        gl_TessLevelInner[0] = level;
    }
}
