//#version 400 (version provided by material prefix)

layout(triangles, equal_spacing, ccw) in;

in vec4 tcs_Color[];
in float tcs_FogValue[];
in vec2 tcs_Texcoord[];
in float tcs_TerrainType[];
in vec3 tcs_WorldPos[];
in vec3 tcs_WorldNorm[];

out vec4 Color;
out float FogValue;
out vec2 Texcoord;
out float TerrainType;
out vec3 WorldNorm;
out vec3 WorldPos;
out float UndisplacedDepth;

uniform vec4 tessDisplace;      // x=phongAlpha, y=displaceScale
uniform mat4 terrainMVP;        // axisSwap * worldToClip (clip-space transform)
uniform vec4 terrainViewport;   // (vmx, vmy, vax, vay) for perspective projection
uniform mat4 mvp;               // projection_ : screen pixels -> NDC

// Textures for displacement sampling (shared with fragment shader)
uniform sampler2D tex1;         // colormap (for material classification)
uniform sampler2D matNormal0;   // rock normal+disp
uniform sampler2D matNormal1;   // grass normal+disp
uniform sampler2D matNormal2;   // dirt normal+disp
uniform sampler2D matNormal3;   // concrete normal+disp
uniform vec4 detailNormalTiling; // .x = base tiling multiplier

#include <include/terrain_common.hglsl>

void main()
{
    vec3 bary = gl_TessCoord;

    // Barycentric interpolation of all attributes
    vec3 worldPos = bary.x * tcs_WorldPos[0]
                  + bary.y * tcs_WorldPos[1]
                  + bary.z * tcs_WorldPos[2];

    vec3 worldNorm = normalize(
        bary.x * tcs_WorldNorm[0]
      + bary.y * tcs_WorldNorm[1]
      + bary.z * tcs_WorldNorm[2]);

    Color = bary.x * tcs_Color[0]
          + bary.y * tcs_Color[1]
          + bary.z * tcs_Color[2];

    FogValue = bary.x * tcs_FogValue[0]
             + bary.y * tcs_FogValue[1]
             + bary.z * tcs_FogValue[2];

    Texcoord = bary.x * tcs_Texcoord[0]
             + bary.y * tcs_Texcoord[1]
             + bary.z * tcs_Texcoord[2];

    TerrainType = bary.x * tcs_TerrainType[0]
                + bary.y * tcs_TerrainType[1]
                + bary.z * tcs_TerrainType[2];

    // Match overlay depth against the same world-space terrain surface that TES displaces.
    vec3 undisplacedWorldPos = bary.x * tcs_WorldPos[0]
                             + bary.y * tcs_WorldPos[1]
                             + bary.z * tcs_WorldPos[2];
    vec4 uclip = terrainMVP * vec4(undisplacedWorldPos, 1.0);
    UndisplacedDepth = (uclip.z / uclip.w) * 0.5 + 0.5;

    // --- Phong tessellation smoothing ---
    float alpha = tessDisplace.x;  // phongAlpha
    if (alpha > 0.0) {
        vec3 proj0 = worldPos - dot(worldPos - tcs_WorldPos[0], tcs_WorldNorm[0]) * tcs_WorldNorm[0];
        vec3 proj1 = worldPos - dot(worldPos - tcs_WorldPos[1], tcs_WorldNorm[1]) * tcs_WorldNorm[1];
        vec3 proj2 = worldPos - dot(worldPos - tcs_WorldPos[2], tcs_WorldNorm[2]) * tcs_WorldNorm[2];
        vec3 phongPos = bary.x * proj0 + bary.y * proj1 + bary.z * proj2;
        worldPos = mix(worldPos, phongPos, alpha);
    }

    // --- Texture-based displacement along normal (dirt only) ---
    float displaceScale = tessDisplace.y;
    if (displaceScale > 0.0) {
        vec3 colSample = texture(tex1, Texcoord).rgb;
        vec4 matWeights = tc_getColorWeights(colSample);
        float dirtWeight = matWeights.z;
        if (dirtWeight > 0.01) {
            float baseTiling = detailNormalTiling.x;
            vec2 dispUV = Texcoord * baseTiling * TC_MAT_TILING.z;
            float disp = 1.0 - texture(matNormal2, dispUV).a;
            worldPos += worldNorm * (disp - 0.5) * displaceScale * dirtWeight;
        }
    }

    WorldNorm = worldNorm;
    WorldPos = worldPos;

    // --- Projection of DISPLACED position (visual rendering) ---
    vec4 clip = terrainMVP * vec4(worldPos, 1.0);
    float rhw = 1.0 / clip.w;
    vec3 screen;
    screen.x = clip.x * rhw * terrainViewport.x + terrainViewport.z;
    screen.y = clip.y * rhw * terrainViewport.y + terrainViewport.w;
    screen.z = clip.z * rhw;
    vec4 ndc = mvp * vec4(screen, 1.0);
    float absW = abs(clip.w);
    gl_Position = vec4(ndc.xyz * absW, absW);

    // Terrain seam expansion in clip space only — WorldPos is NOT modified.
    // Expanding worldPos.xy shifted the surface gradient at edge triangles, changing
    // interpolated normals and shadow coordinates, producing diamond outlines on flat
    // concrete. Operating purely on gl_Position means the rasterizer sees overlap but
    // the fragment shader gets pristine WorldPos: no shadow offset, no normal tilt.
    // No reliefGate needed — screen-space expansion creates no 3D geometry warping.
    {
        float edgeDist = min(min(bary.x, bary.y), bary.z);
        float edgeMask = 1.0 - smoothstep(0.0, 0.08, edgeDist);
        if (edgeMask > 0.001) {
            // Project patch centroid through the same chain as the vertex.
            vec3 patchCent3D = (tcs_WorldPos[0] + tcs_WorldPos[1] + tcs_WorldPos[2]) / 3.0;
            vec4 cClip = terrainMVP * vec4(patchCent3D, 1.0);
            float cRhw = 1.0 / cClip.w;
            vec3 cScreen;
            cScreen.x = cClip.x * cRhw * terrainViewport.x + terrainViewport.z;
            cScreen.y = cClip.y * cRhw * terrainViewport.y + terrainViewport.w;
            cScreen.z = cClip.z * cRhw;
            vec4 cNdc = mvp * vec4(cScreen, 1.0);
            // NDC direction from centroid to this vertex; expand ~1 px outward.
            // gl_Position is pre-divide clip; NDC = gl_Position.xy / gl_Position.w.
            vec2 vertNDC = gl_Position.xy / absW;
            vec2 centNDC = cNdc.xy / abs(cClip.w);
            vec2 seamDir2D = vertNDC - centNDC;
            float seamLen2D = length(seamDir2D);
            if (seamLen2D > 0.0001) {
                // 0.002 NDC ≈ 1 px at 1920 wide; scale back into clip space by absW.
                gl_Position.xy += (seamDir2D / seamLen2D) * 0.002 * edgeMask * absW;
            }
        }
    }
}
