//#version 420 (version provided by material prefix)

layout(triangles, equal_spacing, ccw) in;

in vec3 tcs_WorldPos[];
in vec3 tcs_WorldNorm[];
in vec2 tcs_Texcoord[];

uniform vec4 tessDisplace;      // x=phongAlpha (unused here), y=displaceScale
uniform mat4 lightSpaceMatrix;

// Textures for displacement sampling
uniform sampler2D tex1;         // colormap (for material classification)
uniform sampler2D matNormal2;   // dirt normal+disp (only dirt displaces)
uniform vec4 detailNormalTiling; // .x = base tiling multiplier

#include <include/terrain_common.hglsl>

void main()
{
    vec3 bary = gl_TessCoord;

    vec3 worldPos = bary.x * tcs_WorldPos[0]
                  + bary.y * tcs_WorldPos[1]
                  + bary.z * tcs_WorldPos[2];

    vec3 worldNorm = normalize(
        bary.x * tcs_WorldNorm[0]
      + bary.y * tcs_WorldNorm[1]
      + bary.z * tcs_WorldNorm[2]);

    vec2 texcoord = bary.x * tcs_Texcoord[0]
                  + bary.y * tcs_Texcoord[1]
                  + bary.z * tcs_Texcoord[2];

    // Texture-based displacement along normal (dirt only) — matches main TES
    float displaceScale = tessDisplace.y;
    if (displaceScale > 0.0) {
        vec3 colSample = texture(tex1, texcoord).rgb;
        vec4 matWeights = tc_getColorWeights(colSample);

        float dirtWeight = matWeights.z;
        if (dirtWeight > 0.01) {
            float baseTiling = detailNormalTiling.x;
            vec2 dispUV = texcoord * baseTiling * TC_MAT_TILING.z;
            float disp = 1.0 - texture(matNormal2, dispUV).a;
            worldPos += worldNorm * (disp - 0.5) * displaceScale * dirtWeight;
        }
    }

    // Simple orthographic projection into light space
    gl_Position = lightSpaceMatrix * vec4(worldPos, 1.0);
}
