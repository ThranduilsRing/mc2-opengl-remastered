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
        vec3 mid01 = 0.5 * (vs_WorldPos[0] + vs_WorldPos[1]);
        vec3 mid12 = 0.5 * (vs_WorldPos[1] + vs_WorldPos[2]);
        vec3 mid20 = 0.5 * (vs_WorldPos[2] + vs_WorldPos[0]);
        vec3 center = (vs_WorldPos[0] + vs_WorldPos[1] + vs_WorldPos[2]) / 3.0;

        float d01 = distance(cameraPos.xyz, mid01);
        float d12 = distance(cameraPos.xyz, mid12);
        float d20 = distance(cameraPos.xyz, mid20);
        float dc  = distance(cameraPos.xyz, center);

        float near = tessDistanceRange.x;
        float far  = tessDistanceRange.y;
        float maxTess = max(tessLevel.x, 1.0);

        gl_TessLevelOuter[0] = mix(maxTess, 1.0, smoothstep(near, far, d12));
        gl_TessLevelOuter[1] = mix(maxTess, 1.0, smoothstep(near, far, d20));
        gl_TessLevelOuter[2] = mix(maxTess, 1.0, smoothstep(near, far, d01));
        gl_TessLevelInner[0] = mix(maxTess, 1.0, smoothstep(near, far, dc));
    }
}
