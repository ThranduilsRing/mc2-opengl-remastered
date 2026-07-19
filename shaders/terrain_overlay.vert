// terrain_overlay.vert
// Shared vertex shader for world-space overlay batches (TerrainOverlayBatch and DecalBatch).
// Inputs are typed WorldOverlayVert (wx,wy,wz, u,v, fog, argb) — no rhw sentinel.
// Projection chain is identical to the TES: terrainMVP → perspective divide + viewport → mvp.
#include <include/terrain_depth_bias.hglsl>  // single-source OVERLAY/TERRAIN depth bias

layout(location=0) in vec3  worldPos;   // MC2 world space (x=east, y=north, z=elev)
layout(location=1) in vec2  texcoord;
layout(location=2) in float fogIn;      // [0,1], 1=clear
layout(location=3) in vec4  colorIn;    // RGBA [0,1], unpacked from BGRA uint on CPU

uniform mat4 u_worldToClipGL;  // world -> GL clip (kAxisSwapMC2toGL * worldToClip)

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

    // F1 Stage A: direct GL clip emit. OVERLAY_DEPTH_BIAS applied pre-divide
    // so decals/overlays win LEQUAL ties to terrain (replaces removed glPolygonOffset(-1,-1)).
    vec4 clip4 = u_worldToClipGL * vec4(worldPos, 1.0);
    clip4.z   += OVERLAY_DEPTH_BIAS * clip4.w;
    gl_Position = clip4;

    // Behind/at-camera (and past-far-plane) reject — raster-sheet guard.
    // Under ZERO_TO_ONE: in-frustum = clip4.w > 0 and clip4.z in [0, clip4.w].
    if (!(clip4.w > 0.0 && clip4.z >= 0.0 && clip4.z <= clip4.w))
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
}
