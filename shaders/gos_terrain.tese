//#version 400

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

uniform vec4 tessDisplace;  // x=phongAlpha, y=displaceScale
uniform mat4 terrainMVP;    // MC2 world coords -> OpenGL NDC (Task 7 wires this up)

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

    // --- Projection ---
    // Passthrough: interpolate the already-projected gl_Position from VS
    // Task 7 will replace this with: gl_Position = terrainMVP * vec4(worldPos, 1.0);
    gl_Position = bary.x * gl_in[0].gl_Position
                + bary.y * gl_in[1].gl_Position
                + bary.z * gl_in[2].gl_Position;
}
