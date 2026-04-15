//#version 300 es

#define PREC highp

#include <include/noise.hglsl>
#include <include/shadow.hglsl>
// IS_OVERLAY path: inline calcShadow + cloud shadow using MC2WorldPos world-space varying.
// The stencil/GBuffer1 post-process route (shadow_screen.frag) is NOT used for overlays —
// it requires clearOverlayAlpha to zero GBuffer1.alpha via stencil, but that mechanism is
// unreliable (FBO may not be bound during SplitDraw).  Inline sampling is simpler and correct.

in PREC vec4 Color;
in PREC vec2 Texcoord;
in PREC float FogValue;
#ifdef IS_OVERLAY
in PREC vec3 MC2WorldPos;
in PREC float OverlayUsesWorldPos;
#endif

layout (location=0) out PREC vec4 FragColor;

uniform sampler2D tex1;
uniform PREC vec4 fog_color;
uniform PREC float time;          // seconds — used for water (non-overlay) and cloud shadows (overlay)
uniform vec4 terrainLightDir;     // world-space sun direction (set by gos_SetupObjectShadows)

#ifndef IS_OVERLAY
uniform int isWater;
#endif

void main(void)
{
    PREC vec4 c = Color.bgra;
    PREC vec4 tex_color = texture(tex1, Texcoord);
    c *= tex_color;

#ifdef IS_OVERLAY
    if (OverlayUsesWorldPos > 0.5) {
        // Terrain overlays (road/cement edge tiles) drawn through the basic textured
        // overlay path.  Shadows applied inline below (calcShadow + cloud FBM) using MC2WorldPos.
        // Match them more closely to the runway/apron interior by pulling the overlay
        // texture farther away from the bright decal look and toward a flatter,
        // slightly darker neutral concrete tone before shadow/fog are applied.
        //
        // Tone correction is applied to the RAW TEXTURE colour (not c = Color*tex)
        // so that per-vertex lighting from non-cement neighbours (grass/dirt on the
        // outer perimeter) cannot taint the concrete hue.  Vertex luminance is then
        // re-applied separately to preserve terrain shading without colour contamination.
        float vertexLum = dot(Color.bgra.rgb, vec3(0.299, 0.587, 0.114));
        float texLuma = dot(tex_color.rgb, vec3(0.299, 0.587, 0.114));
        vec3 texGray = vec3(texLuma);
        vec3 overlayNeutral = vec3(0.69, 0.69, 0.68);
        vec3 overlayConcrete = mix(tex_color.rgb, texGray, 0.22);
        overlayConcrete = mix(overlayConcrete, overlayNeutral, 0.15);
        overlayConcrete *= 0.88;
        // Re-apply vertex luminance: keeps terrain lighting/AO without cross-type hue bleed.
        overlayConcrete *= vertexLum;
        c.rgb = overlayConcrete;
        c.a = Color.bgra.a * tex_color.a;

        // Cloud shadows — same FBM formula as gos_terrain.frag, uses MC2WorldPos (stable
        // world-space position, not depth-reconstructed) so shadows don't drift with camera.
        // Range is narrowed vs. terrain (0.85-1.0 vs. 0.70-1.0): overlay base luminance is
        // lower than interior concrete (no pureConcrete normalLight boost), so a full-range
        // cloud shadow over-darkens the perimeter relative to the interior.
        {
            PREC vec2 cloudUV = MC2WorldPos.xy * 0.0006 + vec2(time * 0.012, time * 0.005);
            PREC float cloudNoise = fbm(cloudUV, 4) * 0.5 + 0.5;
            c.rgb *= mix(0.925, 1.0, smoothstep(0.3, 0.7, cloudNoise));
        }

        // Map shadows (static buildings/fences + dynamic mechs).
        // terrainLightDir and shadow map uniforms are injected by gos_SetupObjectShadows()
        // after mat->apply() in Overlay.SplitDraw (gameos_graphics.cpp).
        // Use a flat up-normal (z-up in MC2 world space) — overlay tiles are horizontal.
        {
            vec3 lightDir3 = normalize(terrainLightDir.xyz);
            float shadow = calcShadow(MC2WorldPos, vec3(0.0, 0.0, 1.0), lightDir3, 8);
            shadow = min(shadow, calcDynamicShadow(MC2WorldPos, vec3(0.0, 0.0, 1.0), lightDir3, 4));
            c.rgb *= shadow;
        }

        // Same fog convention as the non-overlay path: FogValue=1 → clear, FogValue=0 → fully fogged.
        // Old: mix(c, fog_color, FogValue) was reversed — FogValue=1 (nearby, no fog) → fog_color → invisible.
        if(fog_color.x>0.0 || fog_color.y>0.0 || fog_color.z>0.0 || fog_color.w>0.0)
            c.rgb = mix(fog_color.rgb, c.rgb, FogValue);
        FragColor = c;
        return;
    }
#else
#ifdef ALPHA_TEST
    if(tex_color.a < 0.5)
        discard;
#endif
    // Animated water (non-overlay path only)
    if (isWater > 0) {
        PREC vec2 wuv = Texcoord * 6.2831853;  // 2*PI so sin tiles at integer UVs

        if (isWater == 1) {
            // Base water — moderate speed, slightly more visible
            PREC float wave1 = sin(wuv.x * 3.0 + wuv.y * 2.0 + time * 0.4)
                             + sin(wuv.x * 2.0 - wuv.y * 3.0 + time * 0.3);
            PREC float wave2 = sin(wuv.x * 5.0 + wuv.y * 4.0 - time * 0.5)
                             + sin(wuv.x * 4.0 - wuv.y * 5.0 - time * 0.2);
            wave1 *= 0.5;
            wave2 *= 0.5;

            PREC float waveBrightness = 1.0 + wave1 * 0.025 + wave2 * 0.015;
            c.rgb *= waveBrightness;
        } else {
            // Detail/spray layer — very slow, gentle undulation
            PREC float wave1 = sin(wuv.x * 3.0 + wuv.y * 2.0 + time * 0.12)
                             + sin(wuv.x * 2.0 - wuv.y * 3.0 + time * 0.08);
            PREC float wave2 = sin(wuv.x * 5.0 + wuv.y * 4.0 - time * 0.15)
                             + sin(wuv.x * 4.0 - wuv.y * 5.0 - time * 0.06);
            wave1 *= 0.5;
            wave2 *= 0.5;

            PREC float waveBrightness = 1.0 + wave1 * 0.015 + wave2 * 0.01;
            c.rgb *= waveBrightness;

            // Faint specular glint on detail layer
            PREC vec3 waveNormal = normalize(vec3(wave1 * 0.06, wave2 * 0.06, 1.0));
            PREC vec3 lightDir = normalize(vec3(0.3, 0.2, 1.0));
            PREC float spec = pow(max(dot(reflect(-lightDir, waveNormal), vec3(0.0, 0.0, 1.0)), 0.0), 96.0);
            c.rgb += vec3(spec * 0.08);
        }
    }
#endif

	if(fog_color.x>0.0 || fog_color.y>0.0 || fog_color.z>0.0 || fog_color.w>0.0)
    	c.rgb = mix(fog_color.rgb, c.rgb, FogValue);
	FragColor = c;
}
