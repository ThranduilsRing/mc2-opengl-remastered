//#version 420 (version provided by prefix)

layout(location = 0) in vec4 pos;       // AMD requires attrib 0 active
layout(location = 3) in vec2 texcoord;
layout(location = 4) in vec3 worldPos;
layout(location = 5) in vec3 worldNorm;

uniform mat4 mvp;  // projection_ for screen-space fallback (TES overrides gl_Position)

out vec3 vs_WorldPos;
out vec3 vs_WorldNorm;
out vec2 vs_Texcoord;

void main()
{
    vs_WorldPos = worldPos;
    vs_WorldNorm = worldNorm;
    vs_Texcoord = texcoord;

    // Screen-space position (TES overrides this with lightSpaceMatrix projection)
    vec4 p = mvp * vec4(pos.xyz, 1);
    gl_Position = p / pos.w;
}
