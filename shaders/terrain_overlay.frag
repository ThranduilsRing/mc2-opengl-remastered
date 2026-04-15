// terrain_overlay.frag
// Fragment shader for alpha cement perimeter / transition tiles.
// These tiles are definitively concrete so we can apply the full cloud FBM range
// (mix 0.70-1.0) that gos_terrain.frag uses — no more per-pixel range narrowing.
//
// MRT:
//   location=0  FragColor       — lit scene colour
//   location=1  GBuffer1.alpha  = 1.0  → shadow_screen.frag skips these pixels
//                                 (same flag as solid terrain, so deferred shadow pass
//                                  treats them identically to interior runway tiles)

#define PREC highp

#include <include/noise.hglsl>
#include <include/shadow.hglsl>

in PREC vec3  WorldPos;
in PREC vec2  Texcoord;
in PREC float FogValue;   // 1=clear, 0=fully fogged
in PREC vec4  Color;      // RGBA [0,1]

layout(location=0) out PREC vec4 FragColor;
#ifdef MRT_ENABLED
layout(location=1) out PREC vec4 GBuffer1;
#endif

uniform sampler2D tex1;
uniform PREC vec4 fog_color;
uniform PREC float time;
uniform vec4 terrainLightDir;

void main()
{
    PREC vec4 tex_color = texture(tex1, Texcoord);

    // Tone correction: operate on raw texture colour so per-vertex lighting from
    // adjacent non-cement tiles cannot taint the concrete hue.
    // Vertex luminance is extracted and re-applied after correction to preserve
    // terrain AO / lighting variation.
    float vertexLum  = dot(Color.rgb, vec3(0.299, 0.587, 0.114));
    float texLuma    = dot(tex_color.rgb, vec3(0.299, 0.587, 0.114));
    vec3  texGray    = vec3(texLuma);
    vec3  overlayNeutral = vec3(0.69, 0.69, 0.68);
    vec3  corrected  = mix(tex_color.rgb, texGray, 0.22);
    corrected        = mix(corrected, overlayNeutral, 0.15);
    corrected       *= 0.88;
    corrected       *= vertexLum;

    PREC vec4 c;
    c.rgb = corrected;
    c.a   = Color.a * tex_color.a;

    // Cloud shadows — full terrain range (0.70-1.0).
    // Uses WorldPos (stable world-space) so shadows don't drift with camera.
    {
        PREC vec2  cloudUV    = WorldPos.xy * 0.0006 + vec2(time * 0.012, time * 0.005);
        PREC float cloudNoise = fbm(cloudUV, 4) * 0.5 + 0.5;
        c.rgb *= mix(0.70, 1.0, smoothstep(0.3, 0.7, cloudNoise));
    }

    // Static + dynamic shadow maps.
    // Flat up-normal (z-up in MC2 world space) — cement tiles are horizontal.
    {
        vec3  lightDir3 = normalize(terrainLightDir.xyz);
        float shadow    = calcShadow(WorldPos, vec3(0.0, 0.0, 1.0), lightDir3, 8);
        shadow = min(shadow, calcDynamicShadow(WorldPos, vec3(0.0, 0.0, 1.0), lightDir3, 4));
        c.rgb *= shadow;
    }

    // Fog: FogValue=1 → clear, FogValue=0 → fully fogged.
    if (fog_color.x > 0.0 || fog_color.y > 0.0 || fog_color.z > 0.0 || fog_color.w > 0.0)
        c.rgb = mix(fog_color.rgb, c.rgb, FogValue);

    FragColor = c;

#ifdef MRT_ENABLED
    // Mark as terrain so deferred shadow_screen pass skips these pixels
    // (same convention as gos_terrain.frag: alpha=1 = terrain flag).
    GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
#endif
}
