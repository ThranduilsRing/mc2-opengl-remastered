//#version 400 (version provided by material prefix)

layout(location = 0) in vec4 pos;
layout(location = 1) in vec4 color;
layout(location = 2) in vec4 fog;
layout(location = 3) in vec2 texcoord;
layout(location = 4) in vec3 worldPos;
layout(location = 5) in vec3 worldNorm;

uniform mat4 mvp;

out vec4 vs_Color;
out vec2 vs_Texcoord;
out float vs_TerrainType;
out vec3 vs_WorldPos;
out vec3 vs_WorldNorm;

void main(void)
{
    // Pass world-space data to TCS (TES will do MVP projection)
    vs_WorldPos = worldPos;
    vs_WorldNorm = worldNorm;

    vs_Color = color;
    vs_Texcoord = texcoord;
    // Unpack material index from fog R byte (normalized 0-1 -> 0-255)
    vs_TerrainType = floor(fog.x * 255.0 + 0.5);

    // TES owns final projection; keep VS output simple.
    gl_Position = mvp * vec4(pos.xyz, 1.0);
}
