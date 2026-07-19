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
// TERRAIN-DECAL-LIGHTING-1a: share the terrain lighting stack so cement
// transition overlays no longer form a lighting seam against lit terrain.
// Same 6 uniforms + computeTerrainNormalFromHeight() as gos_terrain.frag.
// Default-OFF (all 3 terrain env gates unset) → uniforms force-zeroed by
// CPU helper → all branches short-circuit → byte-identical legacy output.
#include <include/terrain_height_normal.hglsl>

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
uniform int u_pathTint;  // MC2_SHADER_PATH_TINT: 1 = solid signature colour (debug); 0 = normal

void main()
{
    // MC2_SHADER_PATH_TINT: solid BLUE so this shader's surfaces are unmistakable.
    if (u_pathTint != 0) {
        FragColor = vec4(0.0, 0.0, 1.0, 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }
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

    // Cloud shadows moved to the fullscreen cloud pass (cloud.frag).
    if (surfaceDebugMode == 3) {
        FragColor = vec4(c.rgb, 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }

    vec3  lightDir3 = normalize(terrainLightDir.xyz);
    // TERRAIN-DECAL-LIGHTING-1a: use the height-derived normal for shadow
    // sampling when the same gate the terrain main path uses is active.
    // Matches gos_terrain.frag's shadowN convention: pass the flat up-normal
    // (NOT the height-derived one) to calcShadow so slope-scale bias stays
    // consistent pixel-to-pixel — this prevents the sprinkle/inverted-shadow
    // pattern that bumpy normals cause on shadow PCF (see the comment block
    // at gos_terrain.frag:732). Height-derived normal is only used by the
    // hemisphere additive below.
    const PREC vec3 shadowN = vec3(0.0, 0.0, 1.0);
    float staticShadow  = calcShadow(WorldPos, shadowN, lightDir3, 8);
    float dynamicShadow = calcDynamicShadow(WorldPos, shadowN, lightDir3, 4);
    float shadow        = staticShadow * dynamicShadow;
    // DEBUG-VIZ: 30 = dynamic-cast shadow only (isolates building dynamic shadow
    // on cement vs grass), 31 = combined. Grayscale, early-return.
    if (surfaceDebugMode == 30) {
        FragColor = vec4(vec3(dynamicShadow), 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }
    if (surfaceDebugMode == 31) {
        FragColor = vec4(vec3(shadow), 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }
    if (surfaceDebugMode == 2) {
        FragColor = vec4(vec3(shadow), 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }

    c.rgb *= shadow;

    // TERRAIN-DECAL-LIGHTING-1a: V1 hemisphere additive (+ V2 shadow-aware
    // floor). Same expression as gos_terrain.frag:780-846 but with
    // snowWeight=0 inline (cement doesn't snow). Default-OFF byte-equivalence:
    // terrainLightingV1Strength uploads 0.0 when the V1 env gate is unset
    // → entire branch skipped → output identical to pre-slice. When V1 is
    // ON but V2 is OFF, the CPU helper force-uploads V2 floor=1.0 so the
    // mix(floor, 1.0, shadow) collapses to 1.0 → V1 unmodulated. N is the
    // height-derived normal when the NFH gate is on, else flat up.
    if (terrainLightingV1Strength > 0.0) {
        PREC vec3 N = vec3(0.0, 0.0, 1.0);
        if (useTerrainNormalsFromHeight != 0 && terrainHeightParams.x > 0.5) {
            PREC vec3 hN = computeTerrainNormalFromHeight(WorldPos.xy);
            // Same additive-perturbation form as gos_terrain.frag. No detail
            // normal contribution on cement — start from flat up.
            N.xy += (hN.xy / max(hN.z, 0.05)) * terrainNormalsFromHeightStrength;
            N.xy = clamp(N.xy, -0.75, 0.75);
            N = normalize(N);
        }
        const PREC vec3 hemiSkyTint    = vec3(0.55, 0.62, 0.75);
        const PREC vec3 hemiGroundTint = vec3(0.32, 0.28, 0.22);
        PREC float skyFactor = N.z * 0.5 + 0.5;
        PREC vec3  hemiFill  = mix(hemiGroundTint, hemiSkyTint, skyFactor);
        PREC float hemiAmount = terrainLightingV1Strength;  // no snowWeight on cement
        PREC float hemiShadowMix = mix(terrainLightingV2ShadowFillFloor, 1.0, shadow);
        c.rgb += hemiFill * hemiAmount * 0.25 * hemiShadowMix;
    }

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
