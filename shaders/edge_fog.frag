// EDGE-FOG-4: cloud bank at the map boundary.
//
// XY boundary position uses ray-height-plane intersection — camera-distance-independent.
// This eliminates the "creeping in" artifact: the fog boundary stays fixed in world
// space as the camera scrolls, because we never read the depth for XY.
//
// Handles void pixels (rawDepth < 0.0001) by projecting the camera ray to the fog
// height plane. This extends the fog PAST the map boundary into the OOB void area.
//
// Height fade: pixels with actual world Z above the cloud bank top (u_fogHeight) get
// no fog. Geometry Z from depth; void pixels treated as being at u_fogHeight.
//
// Blend pattern: GL_SRC_ALPHA blend, outputs (fogColor, alpha). Only reads depthTex;
// sceneColorTex_ is never sampled — no read/write feedback loop (same as fog_oob).
//
// Gate:  MC2_EDGE_FOG      (default ON, "0" to disable)
// Tune:  MC2_EDGE_FOG_START   — world units inside boundary where fog begins (default 50)
//        MC2_EDGE_FOG_HEIGHT  — cloud bank top in world Z                   (default 50)
//        MC2_EDGE_FOG_MAX     — peak opacity 0..1                          (default 0.92)

in vec2 TexCoord;
layout(location = 0) out vec4 outFog;

uniform sampler2D depthTex;
uniform mat4      invViewProj;
uniform vec3      u_fogColor;
uniform float     u_halfExtent;
uniform float     u_fogStart;
uniform float     u_fogHeight;
uniform float     u_fogMax;
uniform float     u_waterElevation;  // sea-level world Z — skip fog at/below water surface

// SKYBOX-FOG-EXCLUDE-1/2 (gate MC2_SKYBOX_FOG_EXCLUDE, default OFF -> u_skyExcludeEnabled=0,
// byte-identical to legacy). Mirrors fog_oob.frag: true-sky pixels are tagged
// stencil=1 by the HDRI skybox's stencil-tag pass and FEATHER-excluded here
// (v2: modulate by the same worldDir.z fade band fog_oob uses instead of the
// v1 hard zero, so any tag/fog frame mismatch degrades to a smooth rolloff,
// not a hard seam).
// usampler2D: GL_DEPTH_STENCIL_TEXTURE_MODE=GL_STENCIL_INDEX views return raw
// unsigned stencil index values (0..255), not normalized floats.
uniform usampler2D stencilTex;
uniform int        u_skyExcludeEnabled;

// FOG-HORIZON-CLAMP-1: shared elevation profile (see fog_oob.frag). elevSin =
// -normalize(wFar-wNear).z is the sine of the view ray's elevation above the
// horizon. Full fog at/below the horizon, smooth fade to zero across
// [startSin,endSin], clear above -- keeps the cloud bank from bleeding upward
// into the sky. This narrows the surviving band toward the horizon on top of the
// pass's existing "dz >= -0.001 => no fog" upward guard. u_horizonClampEnabled==0
// restores the previous behaviour (no elevation clamp).
uniform int   u_horizonClampEnabled;
uniform float u_horizonFadeStartSin;
uniform float u_horizonFadeEndSin;

void main()
{
    float rawDepth = texture(depthTex, TexCoord).r;

    vec2 ndc = TexCoord * 2.0 - 1.0;

    // Camera ray in world space: from near plane (depth=1) to far plane (depth=0).
    // Computed unconditionally — used for ray-plane XY regardless of pixel type.
    vec4 pNear = invViewProj * vec4(ndc, 1.0, 1.0);
    vec4 pFar  = invViewProj * vec4(ndc, 0.0, 1.0);
    vec3 wNear = pNear.xyz / pNear.w;
    vec3 wFar  = pFar.xyz / pFar.w;

    // SKYBOX-FOG-EXCLUDE-2: feathered true-sky exclusion (stencil==1) when the
    // gate is on. Stencil is only ever tagged for depth-unwritten pixels (sky
    // never writes depth), so gate this on the same rawDepth<0.0001 population
    // the void-height-plane branch below already targets. Instead of the v1
    // hard zero, modulate the final fog factor by the same worldDir.z fade
    // band fog_oob.frag uses (normalize(wFar-wNear) here == fog_oob's
    // worldDir; z < -0.22 = deep sky in that frame), so exclusion rolls off
    // smoothly. Gate OFF -> skyExclude stays 1.0 -> byte-identical to legacy.
    float skyExclude = 1.0;
    if (u_skyExcludeEnabled != 0 && rawDepth < 0.0001) {
        uint stencilVal = texture(stencilTex, TexCoord).r;
        if (stencilVal != 0u) {
            float skyDirZ = normalize(wFar - wNear).z;
            skyExclude = smoothstep(-0.22, -0.01, skyDirZ);
        }
    }

    // Actual geometry world Z for height fade.
    // Void pixels (no geometry): treat as being at the fog height plane.
    float geoZ = u_fogHeight;
    if (rawDepth >= 0.0001) {
        vec4 wp = invViewProj * vec4(ndc, rawDepth, 1.0);
        geoZ = wp.z / wp.w;
    }

    // Water surface sits at sea level — don't let the cloud bank overwrite it.
    // A 2 WU margin covers wave displacement so the edge doesn't flicker.
    if (geoZ <= u_waterElevation + 2.0) { outFog = vec4(0.0); return; }

    // Suppress fog for pixels above the cloud bank top.
    // smoothstep(a, b, x) with a > b → 1 at x <= b, 0 at x >= a.
    float heightFade = smoothstep(u_fogHeight + 20.0, u_fogHeight, geoZ);
    if (heightFade <= 0.0) { outFog = vec4(0.0); return; }

    // Intersect camera ray with the fog height plane for XY.
    // Key property: this result is independent of rawDepth (no depth-fudge contamination),
    // so the fog boundary does not shift as the camera scrolls.
    float dz = wFar.z - wNear.z;
    if (dz >= -0.001) { outFog = vec4(0.0); return; }  // looking up/horizontal: no fog
    float t = (u_fogHeight - wNear.z) / dz;
    if (t < 0.0 || t > 1.0) { outFog = vec4(0.0); return; }
    vec2 planeXY = wNear.xy + t * (wFar.xy - wNear.xy);

    // Signed distance from map boundary (positive = inside, negative = outside).
    float distFromEdge = u_halfExtent - max(abs(planeXY.x), abs(planeXY.y));

    // Cloud bank fill:
    //   inside boundary → ramp from 0 (at u_fogStart inside) to 1 (at edge)
    //   outside boundary → solid 1 (inside cloud bank floor, no fade-out)
    float innerRamp   = smoothstep(u_fogStart, 0.0, distFromEdge);
    float outsideFill = step(0.0, -distFromEdge);

    // FOG-HORIZON-CLAMP-1: fold in the shared elevation profile. elevSin =
    // -viewDir.z; full at/below horizon, fade to zero across [startSin,endSin].
    // (The dz>=-0.001 guard above already killed strictly up/horizontal rays; this
    // shapes the small surviving downward-elevation band toward the horizon so the
    // cloud bank never bleeds upward.) clamp=0 -> factor 1.0 (previous behaviour).
    float horizonFactor = 1.0;
    if (u_horizonClampEnabled != 0) {
        float elevSin = -normalize(wFar - wNear).z;
        horizonFactor = 1.0 - smoothstep(u_horizonFadeStartSin, u_horizonFadeEndSin, elevSin);
    }

    float fogFactor = clamp(max(innerRamp, outsideFill) * heightFade * u_fogMax, 0.0, 1.0)
                      * skyExclude   // SKYBOX-FOG-EXCLUDE-2 feather (1.0 when gate OFF)
                      * horizonFactor;  // FOG-HORIZON-CLAMP-1 (1.0 when clamp OFF)
    outFog = vec4(u_fogColor, fogFactor);
}
