// VK-PARITY-OVERLAY-1 TERRAIN OVERLAY / DECAL TIER-1 -- shared VS.
// =============================================================================
// Port of shaders/terrain_overlay.vert (the VS GL uses for BOTH the cement/road
// terrain-overlay batch and the crater/footprint decal batch). WorldOverlayVert in
// (28B: wx,wy,wz, u,v, fog, argb BGRA), clip = worldToClipGL * world, then the same
// reverse-Z depth bias (distance-proportional OVERLAY_DEPTH_BIAS_REL, or the legacy
// constant NDC offset behind biasLegacy) and the same behind-camera / past-far reject.
// worldToClipGL comes from the terrain island's dynamic UBO slot 0 (set 0 binding 0 --
// only the leading mat4 is declared here; the block is a prefix of TerrainFrameUBO).
#include "../../include/terrain_depth_bias.hglsl"

layout(location = 0) in vec3  a_worldPos;
layout(location = 1) in vec2  a_uv;
layout(location = 2) in float a_fog;
layout(location = 3) in vec4  a_color;      // R8G8B8A8_UNORM of the BGRA uint: x=B y=G z=R w=A

layout(set = 0, binding = 0, std140) uniform TerrainFrameUBO {
    mat4 worldToClipGL;
} uT;

layout(push_constant) uniform OverlayPC {
    vec4  fogColor;        // off 0:  gosRenderer fog_color_ (decal fog mix)
    float artFlatExempt;   // off 16: always 0 (stock flat-pad multiply); slot kept for the layout
    float mapHalfExtent;   // off 20
    int   biasLegacy;      // off 24: MC2_OVERLAY_BIAS_LEGACY
    int   cementAdopt;     // off 28: concrete adopt, 1 unless options.cfg StockOverlayConcrete
    int   seamFix;         // off 32: MC2_CEMENT_ATLAS_SEAM_FIX (default 1)
    int   _pad0;           // off 36
    int   _pad1;           // off 40
    int   _pad2;           // off 44
} pc;

layout(location = 0) out vec3  WorldPos;
layout(location = 1) out vec2  Texcoord;
layout(location = 2) out float FogValue;
layout(location = 3) out vec4  Color;

void main() {
    WorldPos = a_worldPos;
    Texcoord = a_uv;
    FogValue = a_fog;
    Color    = a_color.bgra;                 // Color = colorIn.bgra (VBO bytes are B,G,R,A)

    vec4 clip4 = uT.worldToClipGL * vec4(a_worldPos, 1.0);
    if (pc._pad0 == 2) { gl_Position = clip4; return; } // Mines have true terrain depth.
    clip4.z += OVERLAY_DEPTH_BIAS_REL * clip4.z;
    gl_Position = clip4;
    if (!(clip4.w > 0.0 && clip4.z >= 0.0 && clip4.z <= clip4.w))
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
}
