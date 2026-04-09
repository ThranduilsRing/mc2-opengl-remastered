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

    // --- Phong tessellation smoothing ---
    float alpha = tessDisplace.x;  // phongAlpha
    if (alpha > 0.0) {
        // Project interpolated position onto each corner's tangent plane
        vec3 proj0 = worldPos - dot(worldPos - tcs_WorldPos[0], tcs_WorldNorm[0]) * tcs_WorldNorm[0];
        vec3 proj1 = worldPos - dot(worldPos - tcs_WorldPos[1], tcs_WorldNorm[1]) * tcs_WorldNorm[1];
        vec3 proj2 = worldPos - dot(worldPos - tcs_WorldPos[2], tcs_WorldNorm[2]) * tcs_WorldNorm[2];
        vec3 phongPos = bary.x * proj0 + bary.y * proj1 + bary.z * proj2;
        worldPos = mix(worldPos, phongPos, alpha);
    }

    // --- Texture-based displacement along normal ---
    float displaceScale = tessDisplace.y;
    if (displaceScale > 0.0) {
        // Classify material from colormap (same HSV logic as fragment shader)
        vec3 colSample = texture(tex1, Texcoord).rgb;
        vec4 matWeights = tc_getColorWeights(colSample);

        // Compute blended tiling from per-material rates
        float baseTiling = detailNormalTiling.x;
        float blendedTiling = dot(TC_MAT_TILING, matWeights);
        vec2 dispUV = Texcoord * baseTiling * blendedTiling;

        // Sample weighted displacement from material normal alphas
        float disp = tc_sampleDisplacement(dispUV, matWeights,
            matNormal0, matNormal1, matNormal2, matNormal3);

        // Center displacement around 0.5 so it pushes both up and down
        worldPos += worldNorm * (disp - 0.5) * displaceScale;
    }

    WorldNorm = worldNorm;

    // --- Projection: replicate CPU projectZ pipeline on GPU ---
    // Step 1: world -> clip coords (axisSwap * worldToClip)
    vec4 clip = terrainMVP * vec4(worldPos, 1.0);

    // Step 2: perspective divide -> screen coords
    // MC2 can produce negative clip.w; CPU uses fabs(rhw), we must match
    float rhw = 1.0 / clip.w;  // may be negative, like CPU
    vec3 screen;
    screen.x = clip.x * rhw * terrainViewport.x + terrainViewport.z;
    screen.y = clip.y * rhw * terrainViewport.y + terrainViewport.w;
    screen.z = clip.z * rhw;

    // Step 3: screen -> NDC via projection_ (same as VS: mvp * pos / pos.w)
    vec4 ndc = mvp * vec4(screen, 1.0);

    // Step 4: scale by abs(clip.w) so GPU perspective divide recovers NDC
    // Must use abs() because GPU clips negative w
    float absW = abs(clip.w);
    gl_Position = vec4(ndc.xyz * absW, absW);
}
