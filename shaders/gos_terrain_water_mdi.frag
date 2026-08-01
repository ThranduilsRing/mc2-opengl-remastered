//#version 430 (provided by makeProgram prefix)
//
// MDI fragment shader for Task 1.5 (Stage 1 GPU-driven water).
// Based on gos_tex_vertex.frag with these changes:
//   - uniform int isWater removed; replaced by flat in int o_isWater from VS
//   - uniform sampler2D tex2 added (unit 1 = detail texture)
//   - tex_color selection: base layer uses tex1, detail layer uses tex2
//   - all isWater references in wave-animation block replaced with o_isWater
//
// Everything else (fog blend, GBuffer1, ALPHA_TEST ifdef, PREC, etc.) is
// identical to gos_tex_vertex.frag.

#define PREC highp

#include <include/render_contract.hglsl>
#include <include/edge_haze.hglsl>   // WATER-EDGE-FEATHER-1: edgeHazeAmount + EDGE_HAZE_SKY

in PREC vec4 Color;
in PREC vec2 Texcoord;
in PREC float FogValue;
flat in int o_isWater;
in PREC float WaterThickness;     // water-v1: world-unit column from VS
in PREC vec3  WorldPos;           // water-v1: surface pos (Fresnel view vector)

layout (location=0) out PREC vec4 FragColor;
layout (location=1) out PREC vec4 GBuffer1;

// WATER-EDGE-FEATHER-1: dissolve the hard map-edge WATER rim into the fog'd
// horizon (companion to terrain TERRAIN-EDGE-FEATHER-1). Water is alpha-blended,
// so the feather BOTH tints col toward EDGE_HAZE_SKY AND lifts alpha toward 0 at
// the rim — a tint-only fade would leave a hazed-but-opaque line. Default OFF
// (u_waterEdgeFeather==0 -> skipped -> byte-identical). u_halfMap = WORLD-unit
// map half-extent (matches WorldPos.xy raw MC2 world units).
uniform int   u_waterEdgeFeather;
uniform float u_waterEdgeFeatherStrength;
uniform float u_halfMap;

uniform sampler2D tex1;
uniform sampler2D tex2;
uniform sampler2D u_waterReflRT;      // WATER-REFLECTION-SAMPLE-1: 1/4-res terrain
                                      //   reflection RT (unit 2), filled by Phase C1
                                      //   (RenderWaterReflectionPass). Replaces the old
                                      //   dead terrain-colormap atlas sampler.
uniform float u_waterRtStrength;      //   0 = no RT blend -> fall back to SH sky.
uniform vec2  u_waterScreenSize;      //   full-res FBO dims for the screen-space RT UV.
// WATER-ASPECT-CORRECT-2: the frustum aspect the C++ side actually built this
// frame (mclib/camera.cpp) -- native/viewport aspect by default (UI-ASPECT-
// ANCHOR-1), or the legacy 4/3 only when that fix is disabled
// (MC2_CAMERA_ASPECT_NATIVE=0). Replaces the old hardcoded 3.0/4.0 assumption
// below, which over-corrected dFdx(nz) once the frustum stopped being 4:3-
// locked by default (root cause of "water HDR clipped at odd resolutions").
uniform float u_waterProjAspect;
uniform PREC vec4 fog_color;
uniform PREC float time;          // seconds — used for water animation
uniform PREC vec4 cameraPos;      // water-v1: MC2 world-space camera (Fresnel)
uniform PREC float alphaDepth;    // MapData::alphaDepth (world-units); shore smoothstep range
// WATER-SHORE-MASK-1 (batch-6 item 3): OPTIONAL smoothed-contour distance
// field (the cooked shoreline_mask sidecar, R = signed distance to waterline,
// 0.5 = edge, +landward, BAND_NORM_WU=128 -> wu = (R-0.5)*256). When present,
// the shore alpha term is additionally feathered along this SMOOTH contour,
// dissolving the tile-quantized teal staircase ring at water edges (the
// WaterThickness-driven band follows raw tile geometry). u_hasShoreMask==0
// (no sidecar / killswitch MC2_WATER_SHORE_MASK=0) -> byte-identical.
// SHORELINE-GEOMETRY-1 (MC2_WATER_SHORE_MEANDER): organic-coastline silhouette.
// The shore discard contour is the iso-line of the bilinearly interpolated
// WaterThickness. On the stock TERRACED heightfield that iso-line degenerates
// onto the tile lattice (one straight segment per boundary tile -> the
// axis-aligned / 45-degree staircase). Perturbing the FIELD by a static,
// zero-mean, world-space fBm bends the contour through tile interiors, which
// is what actually changes the silhouette — widening the alpha band (the
// earlier MC2_WATER_SHORE_FEATHER) only blurs the risers without moving them.
// Static in time (a swimming coastline would be far worse than a stepped one)
// and keyed on continuous WorldPos.xy, so it is seam-free and camera-
// independent. u_shoreMeanderWu == 0 -> term skipped -> byte-identical.
uniform float u_shoreMeanderWu;     // amplitude, world units
uniform float u_shoreMeanderFreq;   // cycles per world unit
uniform sampler2D  u_shoreMask;
uniform int        u_hasShoreMask;
uniform PREC vec4  u_shoreMaskBounds;   // topLeftX, topLeftY(=maxY), sizeX, sizeY (wu)
// WATER-SHORE-EDGE-1 (MC2_WATER_SHORE_MODE): per-fragment signed-height shore
// edge. Both the legacy ramp (per-vertex coarse WaterThickness, iso-line
// degenerates to the 128wu tile lattice) and the mode-1 mask feather (cooked
// contour steps ~8wu) quantize the waterline. Mode 2 instead samples the
// hi-res 4x visual heightfield (R32F GL_LINEAR -> continuous bilinear zero-
// crossing) and feathers on shoreDist = waterElevation - h(WorldPos.xy):
//   shore = smoothstep(-K, K*0.5, shoreDist), K = u_shoreFeatherWu (wu),
// min()'d with the depth term (can only soften, never extend water onto land).
// u_shoreMode: 0 = legacy ramp only, 1 = mask feather (shipped
// WATER-SHORE-MASK-1), 2 = per-fragment height (default). Missing height
// sidecar -> u_hasShoreHeight==0 -> mode-1 behaviour; missing mask too ->
// legacy (unchanged pixels).
// u_shoreHeightBounds: topLeftX, topLeftY(=maxY), spanX, spanY (wu) where span
// = (V-1) grid intervals; the half-texel remap below puts texel centres
// exactly on fine-grid vertices.
uniform int        u_shoreMode;
uniform sampler2D  u_shoreHeightTex;
uniform int        u_hasShoreHeight;
uniform PREC vec4  u_shoreHeightBounds;
uniform PREC float u_shoreFeatherWu;
uniform float      waterElevation;      // Terrain::waterElevation (same value as VS)
uniform int u_waterDebugMode;     // WATER-DEBUG-VIEWS-1: fragment/material-space debug.
                                  // 0=Final 1=Tint 2=Alpha 3=Normal 4=Depth 5=Shore 6=Lighting
                                  // 7=SHReflect 8=RTsample 9=ReflectBlend 10=Aniso(aspect probe).
                                  // Distinct from VS geometry-space debugMode (MC2_RENDER_WATER_FASTPATH_DEBUG).
uniform float u_waterSkyTintStrength;  // WATER-VISUAL-FIRST-SLICE: 0 = exact no-op (default).
uniform vec3  u_waterSkyTintColor;     // camera-INDEPENDENT sky/horizon tint target (NOT fresnel)
uniform float u_waterReflStrength;     // WATER-SKY-REFLECTION-1: 0 = exact no-op (gate OFF default).
// WATER-SUN-SPEC-2 (repairs WATER-SUN-SPEC-1, c50e99a6). BOTH of these were
// uploaded EVERY FRAME by gameos_graphics.cpp since c50e99a6 and NEITHER was
// ever declared here, in gos_terrain_water_fast_mdi.vert, or in ANY shader in
// this repo's history (`git log -S"u_waterSpecIntensity" --all -- 'shaders/*'`
// returns no commits). glGetUniformLocation returned -1 and the upload lambda
// skips loc < 0 silently, so MC2_WATER_SHINE was arithmetically incapable of
// changing one pixel at any value, on any mission -- and the water surface had
// NO sun direction at all, which is why "the sun does not glint off the water"
// could not be fixed by any lighting tuning. Declaring them is the repair.
uniform PREC vec4 terrainLightDir;     // RAW MC2 Z-up sun dir (.z = sin(elevation)); NOT the Stuff frame.
uniform float u_waterSpecIntensity;    // MC2_WATER_SHINE; 0 = exact no-op (default OFF).
// WATER-HDRI-REFL-1: direct equirect HDRI sample for specular sun glint.
// u_waterHdriLod < 0 => HDRI unavailable, fall back to SH-L2 only.
uniform sampler2D u_hdri;        // HDRI equirect (unit 3, with mipmaps)
uniform float     u_skyYaw;      // azimuth rotation (rad) matching renderHdriSkyboxInvVP
uniform float     u_waterHdriLod; // sample LOD; C++ default 2.0 (WATER-HDRI-REFL-SHARPEN-1), MC2_WATER_HDRI_LOD env overrides

// water-v1 style params. WATER-TUNING-UI-1: the user-tunable subset is promoted
// from compile-time const to uniform (live ImGui control in Graphics Options >
// Water). The C++ upload (gameos_graphics.cpp MDI bind block) seeds each with
// the EXACT former-const default, so default rendering is byte-identical.
uniform vec3  SHALLOW_COLOR;       // default vec3(0.22, 0.45, 0.38) — user-approved teal
uniform vec3  DEEP_COLOR;          // default vec3(0.03, 0.13, 0.20) — dark blue, NOT black
uniform float ABSORPTION_DENSITY;  // default 0.022 — 1/world-units (Beer-Lambert k; ~45u e-fold over 0..150)
uniform float WATER_MAX_ALPHA;     // default 0.87 — mild transparency; 1.0 = opaque slab, lower = more see-through. f(depth) only
const float SKY_AMBIENT        = 0.18;   // brightness floor (camera-independent)
// --- camera-INDEPENDENT procedural water detail (BAR-style: 2 fBm layers,
//     OPPOSITE scroll dirs -> organic churn, no grid). f(WorldPos,time) only. ---
const float WAVE_FREQ   = 0.030;   // 1/world-u; lower = bigger waves, visible at zoom-out
const float WAVE_SPEED  = 6.0;     // world-u/sec domain scroll
uniform float RIPPLE_GAIN; // default 0.22 — crest BRIGHTEN amount (mild, low color variance)
const vec3  GLINT_TINT   = vec3(0.82, 0.88, 0.94);  // near-WHITE wave-cap (slightly cool)
uniform float GLINT_GAIN;  // default 0.30 — additive camera-INDEPENDENT white crest shimmer
const float GLINT_THRESH = 0.36;   // a bit more crest area shows white caps
const float WAVE_FADE_NEAR = 9000.0;  // full detail well out (visible at zoom-out now)
const float WAVE_FADE_FAR  = 40000.0; // only the very furthest extreme calms (no flat-at-zoom)

// WATER-SKY-REFLECTION-1: reflection control consts. The OLD S3 path reflected
// the terrain ground colormap (REFL_STEPS/STEP_LEN ray-march) and was shelved
// 2026-05-17 because the ground-color source gave a compass hue-swing under
// orbit ("ugly as shit"). That was a SOURCE problem, not a camera-dependence
// principle (rule superseded 2026-05-18). The reflect-vector + Fresnel + mix
// math is reused below; only the SOURCE changed: ground colormap -> SH-L2 sky.
const float REFL_F0         = 0.02;   // Schlick base reflectance (water ~0.02)
const float REFL_MAX        = 0.55;   // WATER-REFL-DEFAULT-ON: raised from 0.30 for visible shiny
const float REFL_WAVE_SLOPE = 0.05;   // fBm-gradient -> wave-normal slope (0.02-0.10)
// WATER-GRAZE-REFL-1 (master-FAIL #9): at grazing angles the strong Fresnel mix
// (reflMix -> REFL_MAX) mirrors the HDRI/SH sky sample — but that sample points
// at a dark clear-sky band that mismatches the rendered dusk/overcast, so
// grazing water reads flat/dull and "does not reflect the bright sky." Pull the
// reflected color toward the AUTHORED sky/horizon tint (u_waterSkyTintColor,
// always uploaded) at grazing angles so the reflection carries the sky the
// player expects. GRAZE_SKY_PULL is the max pull at full grazing; the pow()
// exponent keeps top-down water on the true HDRI/SH sample (byte-identical).
const float GRAZE_SKY_PULL  = 0.70;   // 0 -> exact old reflection color
const float GRAZE_SKY_EXP   = 4.0;    // grazing sharpness (higher = only near-horizon)

// WATER-SUN-SPEC-2: Blinn-Phong sun-glint constants. The lobe is deliberately
// TIGHT -- this is the specular highlight of a ~0.5-degree solar disk on a
// near-flat dielectric, not a broad sheen. A wide lobe would just re-add the
// camera-independent GLINT_GAIN shimmer that already exists above.
const float SUN_SPEC_POWER  = 96.0;   // Blinn-Phong exponent (tight sun lobe)
const vec3  SUN_SPEC_TINT   = vec3(1.00, 0.97, 0.90);  // slightly warm sun white

// Precision-safe (fract-early) hash -> stable for large MC2 world coords;
// value-noise + 3-octave fBm. No texture, no seam (continuous WorldPos).
PREC float h21(PREC vec2 ip){
    PREC vec3 q = fract(vec3(ip.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}
PREC float vnoise(PREC vec2 uv){
    PREC vec2 i = floor(uv), f = fract(uv);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(h21(i),               h21(i + vec2(1.0, 0.0)), f.x),
               mix(h21(i + vec2(0.0,1.0)), h21(i + vec2(1.0, 1.0)), f.x), f.y);
}
PREC float fbm3(PREC vec2 uv){
    PREC float s = 0.0, a = 0.5;
    for (int k = 0; k < 3; ++k){ s += a * vnoise(uv); uv = uv * 2.03 + 11.7; a *= 0.5; }
    return s;   // ~0 .. 0.875, mean ~0.4375
}

// WATER-SKY-REFLECTION-1: SH-L2 sky irradiance, inlined from the canonical
// "default" set in RenderCore/IblShCoeffs.h (projected from the HDRI
// data/hdr/DaySkyHDRI063B_4K.exr). Shader-first: no C++ coeff upload, no RT.
// SOURCE OF TRUTH = IblShCoeffs.h; re-sync these if that file regenerates.
// Order: [0]=L00 [1]=L1-1 [2]=L10 [3]=L11 [4]=L2-2 [5]=L2-1 [6]=L20 [7]=L21 [8]=L22.
// Axis: Y-up (pole on .y), same basis as static_prop.vert::evalShL2.
//PORT-BEGIN:waterSkyShCoeffs
const vec3 kWaterSkySh[9] = vec3[9](
    vec3( 1.713250528,   1.773145394,   1.652499519  ),
    vec3( 0.5869951003,  0.5782034038,  0.5179309118 ),
    vec3( 0.01179062044, 0.001345766309,-0.008740967483),
    vec3( 1.966354845,   1.856721003,   1.505876116  ),
    vec3( 1.334658751,   1.287703421,   1.077505997  ),
    vec3( 0.01688171049, 0.01641851255, 0.01176750778),
    vec3(-1.016652552,  -0.9684202893, -0.7816944278 ),
    vec3( 0.05064675817, 0.04545421310, 0.03281146747),
    vec3( 1.962057200,   1.850144657,   1.499676879  )
);
//PORT-END:waterSkyShCoeffs

// WATER-HDRI-REFL-1: equirect sample of the live HDRI texture using the same
// Z-up (MC2 frame) azimuth/elevation formula as renderHdriSkyboxInvVP (frameFix==2).
// rdir is in MC2 Z-up world space (east=x, north=y, up=z).
// Apply the cached skyYaw rotation about +Z (up) so the reflected sun matches the
// visible HDRI sun. LOD is caller-supplied (MC2_WATER_HDRI_LOD, default 2.5).
//PORT-BEGIN:waterEvalHdri
PREC vec3 waterEvalHdri(PREC vec3 rdir) {
    float s = sin(u_skyYaw), c = cos(u_skyYaw);
    PREC vec3 d = vec3(rdir.x * c - rdir.y * s,
                       rdir.x * s + rdir.y * c,
                       rdir.z);
    PREC vec2 uv = vec2(atan(d.y, d.x) / (2.0 * 3.14159265) + 0.5,
                        asin(clamp(d.z, -1.0, 1.0)) / 3.14159265 + 0.5);
    PREC vec3 raw = textureLod(u_hdri, uv, u_waterHdriLod).rgb;
    // Reinhard per-channel: physical HDR values [0, inf) -> [0, 1).
    // Keeps blue > red for clear sky (hue preserved), prevents sun/bright-cloud
    // from collapsing to featureless white in the reflection mix.
    return raw / (raw + 1.0);
}
//PORT-END:waterEvalHdri

// Mirror of static_prop.vert::evalShL2 (Ramamoorthi-Hanrahan 2001). Returns
// diffuse sky IRRADIANCE by direction -> smooth, low-frequency, orbit-stable
// broad sky color (the Phase-A reflection shape). Input n is Y-up.
//PORT-BEGIN:waterEvalSkySh
PREC vec3 waterEvalSkySh(PREC vec3 n){
    const float c1 = 0.429043, c2 = 0.511664, c3 = 0.743125, c4 = 0.886227, c5 = 0.247708;
    return (  c1 * kWaterSkySh[8] * (n.x*n.x - n.z*n.z)
            + c3 * kWaterSkySh[6] * (n.y*n.y)
            + c4 * kWaterSkySh[0]
            - c5 * kWaterSkySh[6]
            + 2.0*c1 * kWaterSkySh[4] * (n.x*n.y)
            + 2.0*c1 * kWaterSkySh[5] * (n.y*n.z)
            + 2.0*c1 * kWaterSkySh[7] * (n.x*n.z)
            + 2.0*c2 * kWaterSkySh[3] *  n.x
            + 2.0*c2 * kWaterSkySh[1] *  n.y
            + 2.0*c2 * kWaterSkySh[2] *  n.z );
}
//PORT-END:waterEvalSkySh

void main(void)
{
    if (o_isWater == 1) {
        // ---- water-v1 stylized base layer ----
        // viewVec used ONLY for distance (waveLOD anti-alias) - NOT angle.
        PREC vec3  viewVec = cameraPos.xyz - WorldPos;
        //PORT-BEGIN:waterBaseColor
        PREC float waveLOD = 1.0 - smoothstep(WAVE_FADE_NEAR, WAVE_FADE_FAR, length(viewVec));

        // BAR-style: TWO fBm layers scrolling in OPPOSITE directions -> organic
        // churn, no grid/lattice. Continuous WorldPos = seam-free; fract-early
        // hash = precision-safe at large MC2 coords. Camera-INDEPENDENT.
        PREC float sc = time * WAVE_SPEED * WAVE_FREQ;
        PREC vec2  q0 = WorldPos.xy * WAVE_FREQ        + vec2( 1.00,  0.60) * sc;
        PREC vec2  q1 = WorldPos.xy * WAVE_FREQ * 1.70 + vec2(-0.80, -1.10) * sc;
        PREC float nz = (fbm3(q0) + fbm3(q1)) - 0.875;   // ~zero-mean organic detail

        PREC float trans    = clamp(exp(-WaterThickness * ABSORPTION_DENSITY), 0.0, 1.0);  // clamp handles negative WaterThickness (above-water shore tiles)
        PREC vec3  waterCol = mix(DEEP_COLOR, SHALLOW_COLOR, trans);

        // alphaDepth is MapData::alphaDepth (world-units); guard against 0.
        // Shore-extension fix: extend blend into above-water tiles (negative
        // WaterThickness) so the water tint creeps onto the beach, matching
        // the original CPU alpha-band behavior (alphaEdge on above-water verts).
        PREC float shoreBlend = max(alphaDepth, 1.0);
        // SHORELINE-GEOMETRY-1: bend the waterline off the tile lattice.
        // shoreWT drives ONLY the shore/alpha term and the discard (i.e. the
        // silhouette); the depth tint above still uses the true
        // WaterThickness, so shallow/deep colour stays physically keyed to
        // the real terrain and only the coastline SHAPE changes.
        PREC float shoreWT = WaterThickness;
        if (u_shoreMeanderWu > 0.0) {
            // Two decorrelated octaves of the existing world-space value-fBm.
            // fbm3 has mean ~0.4375; subtracting it makes each term zero-mean
            // so the contour wanders equally seaward and landward about its
            // stock position (no net water gain/loss).
            PREC vec2  mp = WorldPos.xy * u_shoreMeanderFreq;
            PREC float mn = (fbm3(mp) - 0.4375) * 1.60
                          + (fbm3(mp * 2.70 + vec2(37.2, 17.9)) - 0.4375) * 0.80;
            shoreWT += u_shoreMeanderWu * mn;
        }
        PREC float shore = smoothstep(-shoreBlend * 0.5, shoreBlend, shoreWT);
        // WATER-SHORE-MASK-1: feather the shore band along the smoothed
        // contour distance (see uniform block comment). min() with the
        // depth-driven term: deep water keeps full alpha, and the mask can
        // only SOFTEN the edge, never extend water onto dry land beyond it.
        // WATER-SHORE-EDGE-1: resolve the EFFECTIVE mode with data-driven
        // fallback (2 -> 1 when the height sidecar is absent; 1 -> 0 when the
        // mask is absent), so missing data always degrades to shipped pixels.
        int shoreModeEff = u_shoreMode;
        if (shoreModeEff == 2 && u_hasShoreHeight == 0) shoreModeEff = 1;
        if (shoreModeEff == 1 && u_hasShoreMask   == 0) shoreModeEff = 0;
        if (shoreModeEff == 2) {
            // Per-fragment signed height vs the hi-res bilinear heightfield.
            PREC vec2 hn;
            hn.x = (WorldPos.x - u_shoreHeightBounds.x) / max(u_shoreHeightBounds.z, 1e-5);
            hn.y = (u_shoreHeightBounds.y - WorldPos.y) / max(u_shoreHeightBounds.w, 1e-5);
            if (hn.x >= 0.0 && hn.x <= 1.0 && hn.y >= 0.0 && hn.y <= 1.0) {
                // Half-texel remap: hn spans (V-1) grid intervals; texel
                // centres must land on grid vertices.
                PREC vec2 ts = vec2(textureSize(u_shoreHeightTex, 0));
                PREC vec2 huv = (hn * (ts - 1.0) + 0.5) / ts;
                PREC float hTerr = texture(u_shoreHeightTex, huv).r;
                PREC float shoreDist = waterElevation - hTerr;   // + = underwater
                PREC float K = max(u_shoreFeatherWu, 0.5);
                PREC float shoreH = smoothstep(-K, K * 0.5, shoreDist);
                shore = min(shore, shoreH);
            }
        } else if (shoreModeEff == 1) {
            PREC vec2 slUV;
            slUV.x = (WorldPos.x - u_shoreMaskBounds.x) / max(u_shoreMaskBounds.z, 1e-5);
            slUV.y = (u_shoreMaskBounds.y - WorldPos.y) / max(u_shoreMaskBounds.w, 1e-5);
            if (slUV.x >= 0.0 && slUV.x <= 1.0 && slUV.y >= 0.0 && slUV.y <= 1.0) {
                PREC vec4 sl = texture(u_shoreMask, slUV);
                if (sl.a > 0.5) {
                    PREC float maskDistWu = (sl.r - 0.5) * 256.0;   // +land / -water
                    PREC float shoreM = 1.0 - smoothstep(-12.0, 6.0, maskDistWu);
                    shore = min(shore, shoreM);
                }
            }
        }
        // shoreModeEff == 0: legacy per-vertex ramp only (no extra term).
        if (shore <= 0.0) discard;            // kill tiles too far above waterline

        PREC vec3  vertexLightRGB = Color.bgra.rgb;  // VS packs .bgra; un-swizzle (camera-indep)
        PREC vec3  col = waterCol * max(vertexLightRGB, vec3(SKY_AMBIENT))
                       + waterCol * SKY_AMBIENT * 0.5;
        // Camera-INDEPENDENT granular wave detail (dual counter-scroll fBm).
        // BRIGHTEN-only (no darkening) + sharpened reflective crest shimmer ->
        // reads as light catching the ripples. STALE NOTE (superseded, kept for
        // history): this comment used to say "no Fresnel/specular... camera-
        // dependent term deferred to S3." That is no longer true — camera-
        // dependent SH-sky + HDRI + terrain-RT reflection (Fresnel-mixed) is
        // implemented and live below (WATER-SKY-REFLECTION-1 / WATER-HDRI-REFL-1 /
        // WATER-REFLECTION-SAMPLE-1). Don't trust this block's wording over the
        // code that follows it.
        PREC float crest = max(nz, 0.0);                       // troughs stay at base
        col *= 1.0 + RIPPLE_GAIN * waveLOD * crest;            // brighten only
        PREC float glint = smoothstep(GLINT_THRESH, 0.80, nz); // sharp crest sparkle
        col += glint * GLINT_GAIN * waveLOD * GLINT_TINT;      // camera-INDEPENDENT shimmer
        //PORT-END:waterBaseColor
        // WATER-VISUAL-FIRST-SLICE: gated camera-INDEPENDENT sky/horizon tint.
        // f(uniform color, strength) only — no view angle, no reflection. At
        // strength 0 (default) the mix returns col unchanged -> byte-identical.
        // Applied before fog so atmospheric fog still attenuates by distance.
        col = mix(col, u_waterSkyTintColor, clamp(u_waterSkyTintStrength, 0.0, 1.0));

        // ---- WATER-SKY-REFLECTION-1: gated camera-DEPENDENT sky reflection ----
        // Replaces the shelved terrain-colormap S3 (whose ground-color source
        // gave a compass hue-swing under orbit). Source is now the SH-L2 SKY
        // (kWaterSkySh) -> orbit-stable broad shape. Camera-dependence is
        // intentional and permitted (the 2026-05-17 "camera-independent" rule
        // was superseded 2026-05-18: rejection was QUALITY, not principle).
        // u_waterReflStrength 0 (gate OFF default) AND debug!=7 -> block skipped
        // -> byte-identical. (debug==7 evaluates the term for inspection but
        // reflMix stays 0 at strength 0, so the rendered Final is unchanged.)
        PREC vec3  skyReflCol = col;   // SH sky term (debug 7) / fallback
        PREC vec4  rtSample   = vec4(0.0);   // terrain RT sample (debug 8)
        PREC vec3  reflectCol = col;   // final blended reflection (debug 9)
        PREC float reflMix    = 0.0;
        if (u_waterReflStrength > 0.0 || u_waterRtStrength > 0.0 ||
            u_waterDebugMode == 7 || u_waterDebugMode == 8 || u_waterDebugMode == 9) {
            // Wave normal from the in-scope fBm gradient (reused S3 scaffold).
            //PORT-BEGIN:waterReflSetup
            // WATER-ASPECT-CORRECT-1/2: a projection frustum locked to a
            // DIFFERENT aspect than the viewport compresses dFdx vs dFdy by
            // (vpAspect/projAspect); scale X back up. u_waterProjAspect is the
            // frustum aspect C++ actually built this frame -- by default
            // (UI-ASPECT-ANCHOR-1) that equals vpAspect, so _ac is a 1.0
            // no-op; it only deviates under the legacy 4:3-locked frustum
            // (MC2_CAMERA_ASPECT_NATIVE=0), which is when this correction is
            // actually needed.
            float vpAspect = u_waterScreenSize.x / max(u_waterScreenSize.y, 1.0);
            float _ac = vpAspect / max(u_waterProjAspect, 0.0001);
            // WATER-GRAZE-REFL-1: fade the wave-normal PERTURBATION by waveLOD so
            // distant water flattens toward a clean +Z mirror. Beyond ~9000wu
            // (the grazing horizon) the wave detail was still perturbing the
            // reflect vector at full strength, scattering the horizon sky sample
            // into shimmer/alias with no reflection to anchor it. A flat mirror
            // at distance is both physically right (calm far water) and what
            // makes the grazing sky reflection read cleanly. Near water
            // (waveLOD==1) is byte-identical.
            PREC vec2  nzGrad     = clamp(vec2(dFdx(nz) * _ac, dFdy(nz)), -2.0, 2.0) * waveLOD;
            PREC vec3  waveNormal = normalize(vec3(nzGrad * REFL_WAVE_SLOPE, 1.0));
            PREC vec3  vdir       = normalize(cameraPos.xyz - WorldPos);  // MC2 Z-up
            PREC vec3  rdir       = reflect(-vdir, waveNormal);           // MC2 Z-up
            // AXIS SWAP (load-bearing): rdir is MC2 world (Z-up, .x=east .y=north);
            // the SH coeffs live in the Stuff Y-up frame evalShL2 consumes (pole
            // on .y). MC2.z(up) -> SH.y. The two HORIZONTAL axes (SH .x/.z) were
            // empirically 180-deg off in azimuth (user: sun read SW instead of
            // NE), so both horizontals are negated -> azimuth flipped 180 to put
            // the baked HDRI sun on the correct side. (Verified visually against
            // ground truth rather than derived, since the projector basis sign on
            // the non-up axes was unconfirmed.)
            PREC vec3  skyDir     = vec3(rdir.x, rdir.z, -rdir.y);
            // WATER-HDRI-REFL-1: use live equirect HDRI when available (captures sun
            // disk + sky gradient, LOD-filtered to suppress wave-normal flicker).
            // Fall back to SH-L2 when HDRI is absent (u_waterHdriLod < 0).
            if (u_waterHdriLod >= 0.0) {
                skyReflCol = waterEvalHdri(rdir);   // rdir in MC2 Z-up, skyYaw applied inside
            } else {
                // evalShL2 returns diffuse IRRADIANCE; /PI -> sky-radiance-like.
                skyReflCol = clamp(waterEvalSkySh(skyDir) * (1.0 / 3.14159265), 0.0, 2.0);
            }
            // WATER-GRAZE-REFL-1: bias the reflected color toward the authored
            // sky/horizon tint at grazing angles (see const block). vdir.z is the
            // view elevation cosine on flat +Z water; small at grazing -> grazeW
            // -> 1. Top-down (vdir.z->1) -> grazeW -> 0 -> skyReflCol unchanged.
            // Lives inside the reflection gate: MC2_WATER_REFLECTION=0 skips it.
            PREC float grazeW = pow(1.0 - max(vdir.z, 0.0), GRAZE_SKY_EXP);
            skyReflCol = mix(skyReflCol, u_waterSkyTintColor, GRAZE_SKY_PULL * grazeW);
            reflectCol = skyReflCol;   // default reflection = sky (fallback/base)
            //PORT-END:waterReflSetup

            // WATER-REFLECTION-SAMPLE-1: blend the terrain reflection RT (Phase C1)
            // OVER the SH sky where the RT has valid terrain (alpha>0). The RT was
            // rendered with the same projection (mirror MVP) -> sample at this
            // fragment's screen UV, conservatively perturbed by the wave gradient.
            // rtSample.a 0 (off-frustum / steep camera ~0 coverage) OR
            // u_waterRtStrength 0 -> reflectCol stays = skyReflCol (graceful sky
            // fallback; steep-camera water never goes empty).
            if (u_waterRtStrength > 0.0 || u_waterDebugMode == 8 || u_waterDebugMode == 9) {
                PREC vec2 ruv = gl_FragCoord.xy / max(u_waterScreenSize, vec2(1.0));
                ruv += nzGrad * (REFL_WAVE_SLOPE * 0.05);   // conservative wave distortion
                rtSample   = texture(u_waterReflRT, clamp(ruv, 0.0, 1.0));
                reflectCol = mix(skyReflCol, rtSample.rgb,
                                 clamp(rtSample.a * u_waterRtStrength, 0.0, 1.0));
            }
            //PORT-BEGIN:waterReflMix
            // Schlick Fresnel; flat +Z water normal -> N.V = vdir.z; grazing boosts.
            PREC float fres = REFL_F0 + (1.0 - REFL_F0) * pow(1.0 - max(vdir.z, 0.0), 5.0);
            // WATER-GRAZE-REFL-1 (master-FAIL #9): the sky reflection is a SURFACE
            // property, not wave DETAIL — gating reflMix by waveLOD (0 beyond
            // ~9000wu) erased the reflection exactly at the grazing horizon, the
            // one place Fresnel (fres->1) makes it strongest, leaving grazing
            // water flat/skytint-only. Let grazing fragments keep full reflection
            // at distance via max(waveLOD, fres): near/top-down water (waveLOD==1)
            // stays byte-identical; only the far grazing horizon (waveLOD<1,
            // fres high) now reflects. MC2_WATER_REFLECTION=0 remains the full
            // killswitch (u_waterReflStrength==0 -> whole block skipped).
            reflMix = clamp(fres * u_waterReflStrength * max(waveLOD, fres), 0.0, REFL_MAX);
            col = mix(col, reflectCol, reflMix);   // reflMix==0 at SH strength 0 -> no-op
            //PORT-END:waterReflMix
        }

        // ---- WATER-SUN-SPEC-2: gated Blinn-Phong SUN glint (MC2_WATER_SHINE) ----
        //PORT-BEGIN:waterSunSpec
        // Placed AFTER the reflection mix and BEFORE fog on purpose: a specular
        // highlight is emitted light, so it must not be diluted by the sky-tint
        // or reflection mix()es, but it MUST be attenuated by distance fog.
        //
        // WHY THIS IS NOT THE EXISTING GLINT: GLINT_GAIN above is a
        // camera-INDEPENDENT crest shimmer keyed on nz alone -- it has no sun
        // and no view vector, so it cannot move when the camera orbits and
        // cannot vanish when the sun is behind you. This term is the real thing:
        // half-vector between the sun and the eye against the fBm wave normal.
        //
        // ARITHMETIC PRECONDITION, stated so the next reader does not repeat
        // entry 17: this is EXACTLY 0.0 whenever terrainLightDir.z <= 0, i.e. on
        // every mission whose [Mission] LightDirPitch puts the sun at or below
        // the horizon. On those maps no value of MC2_WATER_SHINE can do
        // anything, and that is correct behaviour, not a dead gate.
        //
        // Control flow here is UNIFORM (both tests read uniforms only), so the
        // dFdx/dFdy below are well-defined inside the branch.
        if (u_waterSpecIntensity > 0.0 && terrainLightDir.z > 0.0) {
            // Same aspect correction the reflection wave-normal uses
            // (WATER-ASPECT-CORRECT-1/2); 1.0 no-op under the default frustum.
            float _sac = (u_waterScreenSize.x / max(u_waterScreenSize.y, 1.0))
                       / max(u_waterProjAspect, 0.0001);
            PREC vec2  sGrad = clamp(vec2(dFdx(nz) * _sac, dFdy(nz)), -2.0, 2.0) * waveLOD;
            PREC vec3  sN    = normalize(vec3(sGrad * REFL_WAVE_SLOPE, 1.0));
            PREC vec3  sV    = normalize(viewVec);                    // MC2 Z-up eye vector
            PREC vec3  sL    = normalize(terrainLightDir.xyz);        // MC2 Z-up sun vector
            PREC vec3  sH    = normalize(sL + sV);
            PREC float spec  = pow(max(dot(sN, sH), 0.0), SUN_SPEC_POWER);
            // sL.z fades the lobe out as the sun approaches the horizon, so a
            // low sun cannot produce a hard-edged highlight out of nowhere.
            col += spec * u_waterSpecIntensity * sL.z * waveLOD * SUN_SPEC_TINT;
        }
        //PORT-END:waterSunSpec

        if (fog_color.x > 0.0 || fog_color.y > 0.0 || fog_color.z > 0.0 || fog_color.w > 0.0)
            col = mix(fog_color.rgb, col, FogValue);

        // WATER-DEBUG-VIEWS-1: fragment/material-space debug visualizations.
        // mode 0 (Final) leaves the path byte-identical; each non-zero mode is
        // backed by a real water-v1 term computed above, output opaque so it
        // reads clearly over terrain. Mode 7 = SH-L2 sky reflection term.
        if (u_waterDebugMode != 0) {
            PREC vec3 dbg;
            if      (u_waterDebugMode == 1) dbg = waterCol;                       // 1 Tint: DEEP<->SHALLOW mix, pre-ripple
            else if (u_waterDebugMode == 2) dbg = vec3(shore * WATER_MAX_ALPHA);  // 2 Alpha: final alpha as grayscale
            else if (u_waterDebugMode == 3) dbg = vec3(0.5, 0.5, 1.0);            // 3 Normal: flat-up only (no real surface normal)
            else if (u_waterDebugMode == 4) dbg = vec3(trans);                    // 4 Depth: Beer-Lambert transmittance (1=shallow,0=deep)
            else if (u_waterDebugMode == 5) dbg = vec3(shore);                    // 5 Shore: shoreline ramp mask
            else if (u_waterDebugMode == 6) dbg = vec3(RIPPLE_GAIN * waveLOD * crest)   // 6 Lighting: ripple brighten
                                                + glint * GLINT_GAIN * waveLOD * GLINT_TINT;  //            + crest glint
            else if (u_waterDebugMode == 7) dbg = skyReflCol;                     // 7 Reflection: SH-L2 sky term (pre-Fresnel/strength)
            else if (u_waterDebugMode == 8) dbg = mix(vec3(0.0), rtSample.rgb, rtSample.a); // 8 RT sample: terrain reflection RT (black where invalid/alpha=0)
            else if (u_waterDebugMode == 9) dbg = reflectCol;                     // 9 Reflection blend: SH sky <- terrain RT (final reflected color, pre-Fresnel)
            else if (u_waterDebugMode == 10) {                                    // 10 Aniso: screen-space derivative anisotropy probe
                // WATER-ASPECT-PROBE: world-units-per-pixel in screen X vs Y. With a
                // 4:3-locked projection rasterized into a non-16:9 viewport, X is
                // stretched -> dFdx(WorldPos) shrinks vs dFdy -> ratio departs 1.0.
                // Encoded so mid-gray(0.5)=isotropic; the FLATTER/uniform the tone,
                // the cleaner the read. ~0.5 everywhere = no mismatch; a uniform
                // shift away from 0.5 = anisotropic derivatives (root-cause confirm).
                PREC vec2 ddx   = vec2(dFdx(WorldPos.x), dFdx(WorldPos.y));
                PREC vec2 ddy   = vec2(dFdy(WorldPos.x), dFdy(WorldPos.y));
                PREC float aniso = length(ddx) / max(length(ddy), 1e-6);
                dbg = vec3(clamp(aniso * 0.5, 0.0, 1.0));
            }
            else                            dbg = vec3(1.0, 0.0, 1.0);            // unknown -> magenta sentinel
            FragColor = vec4(dbg, 1.0);
            GBuffer1  = rc_gbuffer1_screenShadowEligible(vec3(0.0, 0.0, 1.0));
            return;
        }

        PREC float waterA = shore * WATER_MAX_ALPHA;
        // WATER-EDGE-FEATHER-1: tint + dissolve the map-edge rim (see header).
        if (u_waterEdgeFeather != 0 && u_halfMap > 0.0) {
            float h = clamp(edgeHazeAmount(WorldPos.xy, u_halfMap) * u_waterEdgeFeatherStrength, 0.0, 1.0);
            col    = mix(col, EDGE_HAZE_SKY, h);   // tint toward horizon haze
            waterA *= (1.0 - h);                   // dissolve: alpha -> 0 at the rim
        }
        FragColor = vec4(col, waterA);  // shore ramp preserved; capped so deep water is mildly transparent
        GBuffer1  = rc_gbuffer1_screenShadowEligible(vec3(0.0, 0.0, 1.0));
        return;
    }

    // water-v1: suppress the legacy detail/spray layer (o_isWater==2). Its tiled
    // tex2 + UV-wrap "loop catch" looks bad against the new base; proper animated
    // detail is deferred to water-v2. Verbatim legacy path kept (dead) below.
    if (o_isWater == 2) discard;

    PREC vec4 c = Color.bgra;
    PREC vec4 tex_color = (o_isWater <= 1) ? texture(tex1, Texcoord) : texture(tex2, Texcoord);
    c *= tex_color;

#ifdef ALPHA_TEST
    if(tex_color.a < 0.5)
        discard;
#endif
    // Animated water
    if (o_isWater > 0) {
        PREC vec2 wuv = Texcoord * 6.2831853;  // 2*PI so sin tiles at integer UVs

        if (o_isWater == 1) {
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

	if(fog_color.x>0.0 || fog_color.y>0.0 || fog_color.z>0.0 || fog_color.w>0.0)
    	c.rgb = mix(fog_color.rgb, c.rgb, FogValue);
	FragColor = c;

	// F3 Option A: flat-up fallback (compatibility — no surface normal available).
	GBuffer1 = rc_gbuffer1_screenShadowEligible(vec3(0.0, 0.0, 1.0));
}
