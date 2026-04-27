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
#include <include/render_contract.hglsl>

// [RENDER_CONTRACT]
//   Pass:           TerrainOverlay
//   Color0:         RGBA, alpha-blended (binary alpha; transparent pixels discarded)
//   GBuffer1:       rc_gbuffer1_shadowHandled_flatUp
//   ShadowContract: castsStatic=false, castsDynamic=false,
//                   skipsPostScreenShadow=true (overlay handles shadow inline)
//   StateContract:  depthTest=true, depthWrite=false, blend=AlphaBlend,
//                   requiresMRT=true

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
uniform PREC vec4 cameraPos;
uniform vec4 terrainLightDir;
uniform int surfaceDebugMode;
uniform PREC float mapHalfExtent;  // half side length of playable map (0 = disabled)

void main()
{
    PREC vec4 tex_color = texture(tex1, Texcoord);
    // Vertex argb is forced to 0xffffffff in quad.cpp so no vertex-lighting needed.
    // Solid interior cement has a warm ochre cast; transition atlas tiles are cooler/brighter.
    // Intentional border darkening: ~18% under interior cement so the edge reads as
    // deliberate weathered concrete rather than a failed near-match.
    // Warm tilt (B pulled lower than R/G) matches the ochre cast of the solid interior.
    PREC vec4 c;
    c.rgb = tex_color.rgb * vec3(0.82, 0.80, 0.76);
    c.a   = tex_color.a;

    // Discard transparent pixels — cement transitions are binary-alpha tiles.
    // This keeps depth writes and GBuffer1 writes on the cement-visible region only,
    // letting the terrain underneath show through unchanged on the transparent region.
    if (c.a < 0.5) discard;

    if (surfaceDebugMode == 6) {
        FragColor = vec4(tex_color.rgb, 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }

    // Shared cloud mask — exact same expression and time epoch as gos_terrain.frag.
    PREC vec2  cloudUV    = WorldPos.xy * 0.0006 + vec2(time * 0.012, time * 0.005);
    PREC float cloudNoise = fbm(cloudUV, 4) * 0.5 + 0.5;
    PREC float cloudMask  = smoothstep(0.3, 0.7, cloudNoise); // 0=shadow, 1=clear
    if (surfaceDebugMode == 1) {
        FragColor = vec4(vec3(mix(0.92, 1.0, cloudMask)), 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }

    if (surfaceDebugMode == 3) {
        FragColor = vec4(c.rgb, 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }

    vec3  lightDir3 = normalize(terrainLightDir.xyz);
    float staticShadow  = calcShadow(WorldPos, vec3(0.0, 0.0, 1.0), lightDir3, 8);
    float dynamicShadow = calcDynamicShadow(WorldPos, vec3(0.0, 0.0, 1.0), lightDir3, 4);
    float shadow        = staticShadow * dynamicShadow;
    if (surfaceDebugMode == 2) {
        FragColor = vec4(vec3(shadow), 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }

    c.rgb *= mix(0.85, 1.0, cloudMask);
    c.rgb *= shadow;
    PREC float camDist2D = distance(WorldPos.xy, cameraPos.xy);
    PREC float terrainHeight = WorldPos.z;
    PREC float fogDensity = 0.00006;
    PREC float heightScale = exp(-max(terrainHeight, 0.0) * 0.002);
    PREC float fogAmount = 1.0 - exp(-camDist2D * fogDensity * heightScale);
    fogAmount = clamp(fogAmount, 0.0, 0.70);
    if (surfaceDebugMode == 4) {
        FragColor = vec4(vec3(1.0 - fogAmount), 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }
    PREC vec3 fogCol = vec3(0.58, 0.65, 0.75);
    c.rgb = mix(c.rgb, fogCol, fogAmount);

    // Map-edge haze: same ramp as gos_terrain.frag. Alpha cement overlay tiles
    // are emitted well past the playable boundary on some missions and sample
    // magenta "no-data" colormap pixels. Fade them to sky across the last
    // ~one-tile band so they match the main terrain's edge behaviour.
    if (mapHalfExtent > 0.0) {
        PREC vec3 edgeSkyCol = vec3(0.58, 0.65, 0.75);
        PREC float chebDist  = max(abs(WorldPos.x), abs(WorldPos.y));
        PREC float edgeStart = mapHalfExtent - 256.0;
        PREC float edgeEnd   = mapHalfExtent - 32.0;
        PREC float edgeHaze  = smoothstep(edgeStart, edgeEnd, chebDist);
        c.rgb = mix(c.rgb, edgeSkyCol, edgeHaze);
    }

    FragColor = c;

#ifdef MRT_ENABLED
    // Overlay handles its own shadow inline (cloud + static + dynamic above);
    // opt out of post-shadow to avoid double-shadowing.
    GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
}
