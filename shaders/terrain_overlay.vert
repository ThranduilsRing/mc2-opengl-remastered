// terrain_overlay.vert
// Shared vertex shader for world-space overlay batches (TerrainOverlayBatch and DecalBatch).
// Inputs are typed WorldOverlayVert (wx,wy,wz, u,v, fog, argb) — no rhw sentinel.
// Projection chain is identical to the TES: terrainMVP → perspective divide + viewport → mvp.

layout(location=0) in vec3  worldPos;   // MC2 world space (x=east, y=north, z=elev)
layout(location=1) in vec2  texcoord;
layout(location=2) in float fogIn;      // [0,1], 1=clear
layout(location=3) in vec4  colorIn;    // RGBA [0,1], unpacked from BGRA uint on CPU

uniform mat4 terrainMVP;       // row-major, uploaded GL_FALSE — same as TES
uniform vec4 terrainViewport;  // (vmx, vmy, vax, vay)
uniform mat4 mvp;              // screen-pixel → NDC, uploaded GL_TRUE

out vec3  WorldPos;
out vec2  Texcoord;
out float FogValue;
out vec4  Color;

void main()
{
    WorldPos  = worldPos;
    Texcoord  = texcoord;
    FogValue  = fogIn;
    // The VBO attrib is GL_UNSIGNED_BYTE BGRA (byte0=B, byte1=G, byte2=R, byte3=A).
    // Swizzle here so fragment shaders receive proper RGBA.
    Color     = colorIn.bgra;

    // Replicate the TES projection chain unconditionally (no rhw detection).
    // terrainMVP * worldPos outputs screen-pixel-space homogeneous coords.
    vec4 clip4 = terrainMVP * vec4(worldPos, 1.0);
    float rhw  = 1.0 / clip4.w;
    vec3  px;
    px.x = clip4.x * rhw * terrainViewport.x + terrainViewport.z;
    px.y = clip4.y * rhw * terrainViewport.y + terrainViewport.w;
    px.z = clip4.z * rhw;

    // mvp converts screen-pixel coords to NDC.
    vec4 ndc   = mvp * vec4(px, 1.0);
    // Restore perspective so gl_FragCoord.z / depth testing work correctly.
    float absW = abs(clip4.w);
    gl_Position = vec4(ndc.xyz * absW, absW);
}
