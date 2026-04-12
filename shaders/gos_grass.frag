//#version 420 (version provided by material prefix)

#define PREC highp

#include <include/shadow.hglsl>

in PREC vec2 GrassUV;
in PREC vec3 GrassWorldPos;
in PREC vec3 GrassBaseColor;
in PREC float GrassAlpha;

layout(location=0) out PREC vec4 FragColor;
layout(location=1) out PREC vec4 GBuffer1;

uniform PREC vec4 terrainLightDir;

void main()
{
    // --- Procedural blade mask ---
    // GrassUV.x: 0=left edge, 1=right edge (centered at 0.5)
    // GrassUV.y: 0=base, 1=tip
    float t = GrassUV.y;  // height along blade (0=base, 1=tip)

    // Narrow triangle tapering to tip: full width at base, zero at tip
    float halfWidth = 0.5 * (1.0 - t);
    float distFromCenter = abs(GrassUV.x - 0.5);
    float bladeMask = 1.0 - smoothstep(halfWidth * 0.6, halfWidth, distFromCenter);

    // Discard thin/transparent pixels
    if (bladeMask < 0.3) discard;
    if (GrassAlpha < 0.01) discard;

    // --- Color ---
    // Mix terrain base color with a grass green tint
    const PREC vec3 grassGreen = vec3(0.28, 0.45, 0.18);
    PREC vec3 color = mix(GrassBaseColor, grassGreen, 0.55);

    // Height gradient: darker at base, lighter at tips
    color *= mix(0.6, 1.1, t);

    // --- Simple diffuse lighting (no normal map needed for simple blades) ---
    // Blade normal faces toward viewer; approximate as up-tilted slightly toward light
    PREC vec3 bladeNormal = normalize(vec3(terrainLightDir.x * 0.3, terrainLightDir.y * 0.3, 1.0));
    PREC float NdotL = clamp(dot(bladeNormal, terrainLightDir.xyz), 0.1, 1.0);
    PREC float diffuse = mix(0.5, 1.0, NdotL);
    color *= diffuse;

    // --- Shadow sampling ---
    float staticShadow  = calcShadow(GrassWorldPos, bladeNormal, terrainLightDir.xyz, 8);
    float dynShadow     = calcDynamicShadow(GrassWorldPos, bladeNormal, terrainLightDir.xyz, 4);
    float shadow = staticShadow * dynShadow;
    color *= shadow;

    // Final alpha: blade shape mask * grass weight * distance fade
    float alpha = bladeMask * GrassAlpha;

    FragColor = vec4(color, alpha);

    // GBuffer1: write a grass-terrain flag (alpha=1.0 like terrain so shadows skip it)
    GBuffer1 = vec4(bladeNormal * 0.5 + 0.5, 1.0);
}
