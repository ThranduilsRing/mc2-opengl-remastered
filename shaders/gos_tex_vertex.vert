//#version 300 es

layout(location = 0) in vec4 pos;
layout(location = 1) in vec4 color;
layout(location = 2) in vec4 fog;
layout(location = 3) in vec2 texcoord;

uniform mat4 mvp;

#ifdef IS_OVERLAY
// Overlay vertices with rhw=1.0 carry MC2 world coords and need GPU projection.
uniform mat4 terrainMVP;
uniform vec4 terrainViewport;  // (vmx, vmy, vax, vay) — same as TES
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
        // terrainMVP outputs screen-pixel-space homogeneous coords (not clip-space).
        // Replicate the full TES projection chain exactly:
        //   MC2 world -> screen pixels -> NDC via mvp
        MC2WorldPos = vec3(pos.x, pos.y, pos.z);
        vec4 clip4 = terrainMVP * vec4(MC2WorldPos, 1.0);
        float rhw_c = 1.0 / clip4.w;
        vec3 screenPx;
        screenPx.x = clip4.x * rhw_c * terrainViewport.x + terrainViewport.z;
        screenPx.y = clip4.y * rhw_c * terrainViewport.y + terrainViewport.w;
        screenPx.z = clip4.z * rhw_c;
        vec4 ndc = mvp * vec4(screenPx, 1.0);
        float absW = abs(clip4.w);
        gl_Position = vec4(ndc.xyz * absW, absW);
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
