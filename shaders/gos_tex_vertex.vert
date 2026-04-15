//#version 300 es

layout(location = 0) in vec4 pos;
layout(location = 1) in vec4 color;
layout(location = 2) in vec4 fog;
layout(location = 3) in vec2 texcoord;

uniform mat4 mvp;

#ifdef IS_OVERLAY
// Overlay vertices with rhw=1.0 carry MC2 world coords and need GPU projection.
uniform mat4 terrainMVP;
out vec3 MC2WorldPos;
out float OverlayUsesWorldPos;
#endif

out vec4 Color;
out float FogValue;
out vec2 Texcoord;

void main(void)
{
#ifdef IS_OVERLAY
    OverlayUsesWorldPos = abs(pos.w - 1.0) < 0.0001 ? 1.0 : 0.0;
    if (OverlayUsesWorldPos > 0.5) {
        // terrainMVP = axisSwap * worldToClip already owns the MC2->view axis swap.
        // Pass MC2 world coords directly — no manual swap here.
        MC2WorldPos = vec3(pos.x, pos.y, pos.z);
        gl_Position = terrainMVP * vec4(MC2WorldPos, 1.0);
    } else {
        MC2WorldPos = vec3(0.0);
        vec4 p = mvp * vec4(pos.xyz, 1.0);
        gl_Position = p / pos.w;
    }
#else
    vec4 p = mvp * vec4(pos.xyz, 1.0);
    gl_Position = p / pos.w;
#endif
    Color = color;
    FogValue = fog.w;
    Texcoord = texcoord;
}
