// shaders/static_prop.vert — slice 2 stage 2.C.2 GPU-lighting wired
//
// Stage 2.C.2 flips this from "draw with CPU-baked stale colors via the
// per-frame Colors SSBO" to "compute lit ARGB per vertex via lighting.hglsl
// calc_light()". The Colors SSBO is still read for legacy debug modes
// (RAlt+9 mode 4) and to keep the slice 1 substrate intact, but the main
// path no longer depends on it.
//
// Note: the ENABLE_VERTEX_LIGHTING define from lighting.hglsl is used by
// gos_tex_vertex_lighted to select VS-side vs FS-side calc_light placement.
// static_prop.vert ALWAYS does VS-side here — the per-fragment branch in
// the legacy mover path doesn't apply to the slice 1 batcher's draw shape.
//
// Stage 2.D.2.1 (C1): MC2_STATIC_PROP_LIGHTING gates the .zyx swizzle in
// lighting.hglsl get_base_light(). This shader passes perVertexARGB with
// layout (.x=B, .y=G, .z=R, .w=A), so .zyx reorders to (R,G,B) for
// correct RGB-ordered input to calc_light. gos_tex_vertex_lighted.vert does
// NOT define this symbol and therefore takes the .xyz (legacy) path which
// preserves its pre-38ba240 behavior. See memory/mc2_argb_packing.md.
#define MC2_STATIC_PROP_LIGHTING
#include <include/lighting.hglsl>
#include <include/terrain_depth_bias.hglsl>

layout(location = 0) in vec3  a_position;
layout(location = 1) in vec3  a_normal;
layout(location = 2) in vec2  a_uv;
layout(location = 3) in uint  a_localVertexID;
// Slice 2 (object-offload) — Stage 2.C.2: per-vertex hot-color tag from
// TG_TypeVertex::aRGBLight, written by registerType() at VBO offset 36.
// Decoded by get_base_light() against the per-type magic colors below.
layout(location = 4) in uint  a_aRGBLight;

struct Instance {
    mat4  modelMatrix;
    uint  typeID;
    uint  firstColorOffset;
    uint  flags;
    // Slice 2 (object-offload) — Stage 2.A renamed _pad0 → lightDataIndex
    // on the C++ side at offset 76; mirror the rename here for clarity.
    // Stage 2.C.2 reads it to index the LightsData[32] UBO (binding 0)
    // for calc_light below.
    uint  lightDataIndex;
    vec4  aRGBHighlight;
    vec4  fogRGB;
};

// Slice 2 (object-offload) — Stage 2.C.2: per-type hot-color SSBO. Built
// once at finalizeGeometry() from TG_TypeShape::hotPinkRGB / hotYellowRGB /
// hotGreenRGB. Indexed by inst.typeID. 48 bytes per type.
struct PerTypeData {
    vec4 hotPinkRGB;
    vec4 hotYellowRGB;
    vec4 hotGreenRGB;
};

layout(std430, binding = 0) readonly buffer Instances { Instance     i[]; } instances_;
layout(std430, binding = 1) readonly buffer Colors    { uint         c[]; } colors_;
layout(std430, binding = 2) readonly buffer PerType   { PerTypeData  t[]; } perType_;
// Slice 2 (object-offload) — Stage 2.D.1: parity readback harness output.
// Bound only when MC2_OBJECT_PARITY_CHECK=1 from the C++ side; the
// u_parityWrite gate below short-circuits the write when unbound so this
// declaration is harmless on the default-off path. Index convention:
//   parityOut[gl_InstanceID * u_parityVertsPerType + (gl_VertexID - u_parityBaseVertex)]
// where u_parityVertsPerType is set per-type to numTypeTriangles*3 (expanded
// inside one glDrawElementsInstancedBaseVertex call). The bound range from
// glBindBufferRange is sized exactly instanceCount * vertsPerType uint32
// entries, so writes never overflow.
layout(std430, binding = 3) buffer ParityOut { uint parityOut[]; } parityOut_;

#ifdef MC2_USE_VIEW_UNIFORMS
#include <include/view_uniforms.hglsl>
#else
uniform mat4 u_worldToClipGL;  // world -> GL clip (kAxisSwapMC2toGL * worldToClip)
#endif
// STATIC-PROP-TERRAIN-SUN-DIFFUSE: raw MC2 terrain sun (x=east,y=north,z=up,
// toward-sun), same vector gos_GetTerrainLightDir returns and the terrain
// shader uses. Used as the directional term for static-prop diffuse because
// calc_light's frozen worldLights[0] sun was ~90deg off.
uniform vec3 u_terrainSunMC2;
// EDGE-LIGHT-EMISSION-1 (gate MC2_STATIC_PROP_NIGHT_TAGS): the eye's real
// night state, so get_base_light() can evaluate the stock hot-colour NIGHT
// branches. These were hardcoded `false, 0.0` on this path ("stubbed for
// now"), which meant every runway edge light, taxiway marker and beacon --
// all authored with the Hot Green "Building Base Lights -- ONLY at NIGHT"
// vertex tag, each carrying its own emission colour in its .ini `//Night
// Lights` block -- evaluated its DAYTIME arm and emitted nothing. The lights
// were off. The legacy CPU path (tgl.cpp:1901) never had this bug.
//
// u_nightEmissiveScale is applied to the EMISSION DELTA only (night base minus
// day base), so 1.0 is exact stock and 0.0 is exactly the old behaviour; it is
// a grading knob and the verify-by-exaggeration handle, not a fudge in the
// lighting maths. Gate OFF uploads isNight=0/factor=0 -> byte-identical.
uniform int   u_isNight;             // 0/1
uniform float u_nightFactor;         // 0..1 day->night blend
uniform float u_nightEmissiveScale;  // 1.0 = stock; 0.0 = pre-fix; >1 exaggerate
// BEACON-FIXTURE-ID-1: emission scale applied ONLY to types the CPU-side
// discriminator classified as light fixtures (runway/taxiway edge lights and
// beacons). The flag rides ptd.hotPinkRGB.w -- 1.0 = fixture, 0.0 = everything
// else. This is what lets the fixtures come on at night while every building
// stays byte-identical, which u_nightEmissiveScale (all-props) cannot do.
uniform float u_fixtureEmissiveScale;
// FIXTURE-GLOW-1 (gate MC2_STATIC_PROP_FIXTURE_GLOW): a BOUNDED,
// APPARENT-SIZE-ATTENUATED ADDITIVE glow for the same fixture types
// BEACON-FIXTURE-ID-1 identifies (ptd.hotPinkRGB.w == 1.0).
//
// WHY A SEPARATE TERM IS REQUIRED. The emissive path above cannot make a
// fixture read as a light, for TWO independent reasons, both measured:
//   1. get_base_light() clamps internally at 1.0, so MC2_STATIC_PROP_FIXTURE_
//      LIGHTS = 1, 4 and 8 produce BYTE-IDENTICAL frames. There is no headroom
//      left in base_light.
//   2. Worse, base_light is then MODULATED by the albedo:
//      static_prop.frag:436  `vec4 c = tex_color * vec4(litRgb, ...)`.
//      A fixture's lens texel is dark, so even a fully saturated emissive seed
//      comes out dim. This is the actual reason the lights "are not on at ALL":
//      a light source cannot be expressed as a multiplier on a dark texture.
// This term is added in the FRAGMENT shader AFTER the texture modulate and
// AFTER fog (static_prop.frag, next to `FragColor = c`), so it escapes both
// the 1.0 clamp and the albedo multiply. That is the whole mechanism.
//
// WHY IT CANNOT REPRODUCE THE RUNWAY-MARKER-LIGHTS-1 REGRESSION. That defect
// came from an UNBOUNDED minimum-screen-size clamp (k = max(1, kLbMinNdc/sil)),
// which held every marker at a constant screen size, so hundreds of evenly
// spaced fixtures became hard dashes at overview altitude. This term does the
// OPPOSITE at both ends:
//   * it never touches geometry, position or size -- the fixture keeps shrinking
//     with distance exactly as it does today;
//   * it is driven by the SAME apparent-size quantity that clamp used (sil), but
//     inverted: where that clamp grew the fixture as sil fell, this FADES the
//     glow as sil falls, through a smoothstep that is bounded to [0,1] and is
//     exactly 0.0 once the fixture is about a pixel across. See the calibration
//     block below the uniform.
// It is also multiplied by the night factor, so it is identically 0.0 in
// daylight.
uniform float u_fixtureGlowStrength;   // 0 = off (default-off is byte-identical)
// ATTENUATION METRIC -- read this before touching the constants.
//
// The driver is the fixture's APPARENT SILHOUETTE SIZE IN NDC, computed with
// exactly the same math the RUNWAY-MARKER-LIGHTS-1 billboard clamp uses a few
// hundred lines above:  sil = kLbRefRadius * ndcPerUnit.  Same quantity, used
// in the OPPOSITE direction: that clamp took max() to push size UP when sil got
// small (unbounded -> constant-size dashes at overview); this fades the glow
// OUT when sil gets small, via a smoothstep whose output is bounded to [0,1] by
// construction and is EXACTLY 0.0 below kFixtureGlowSilMin.
//
// ★ Do NOT drive this off view depth (clip.w). MC2's camera changes ALTITUDE by
// scaling the PROJECTION, not by moving the eye (Camera::AltitudeMaximumLo and
// the newScaleFactor = 1 - (alt-min)/testMax term in mclib/camera.cpp). Measured
// with debug view 50: median eye distance is ~700 units at alt 350 AND ~800 at
// alt 6000 -- essentially flat. A clip.w window therefore never engages and
// gives an UNBOUNDED glow at every altitude. This lane shipped that mistake
// first and the capture caught it; the metric below is the correction.
//
// Calibration, anchored to BEACON-FIXTURE-ID-1's measured pixel counts at 4K
// (1 screen pixel ~= 0.000926 NDC vertically at 2160 rows):
//   ground alt 350  -- fixture covered ~1676 px => radius ~23 px => sil ~0.021  -> FULL
//   mid    alt 2200 -- 24..64 px                => radius ~3-4.5 px => sil ~0.0035 -> ~full ("bright speck")
//   overview alt 6000 -- 69 px across ALL fixtures => each sub-pixel => sil < 0.0009 -> ZERO
const float kFixtureGlowRefRadius = 1.2;      // == kLbRefRadius (runwaylight.ase silhouette)
const float kFixtureGlowSilMin    = 0.0009;   // ~1 px radius: at/below this the glow is EXACTLY 0
const float kFixtureGlowSilFull   = 0.0035;   // ~3.8 px radius: full strength at/above this
const float kFixtureGlowWhite     = 0.5;      // 0 = pure fixture hue, 1 = white core
// FAR-BACKDROP-LIT (gate MC2_FAR_BACKDROP_LIT, default OFF): night-shade
// substitution for far/impostor static-prop LODs. Far/impostor LODs register
// via the positions-only hierarchy walk (bdactor.cpp TransformMultiShape_
// HierarchyOnly) and never get the per-vertex NIGHT light bake, so their
// base_light (a_aRGBLight) is a stale MISSION-LOAD (day) value -> fullbright
// impostors at night (and pure-black for dark-baked far geometry). When the
// gate is on, main() fades the stale seed toward the mission's own night
// object-light ambient across the LOD impostor distance (using clip.w as a
// view-depth proxy). u_farBackdropLit==0 (default) -> fade factor 0 ->
// seed==base_light -> byte-identical to the pre-change render.
uniform int   u_farBackdropLit;      // 0=off (default), 1=on
uniform float u_farBackdropNear;     // view-depth (clip.w) where the fade begins
uniform float u_farBackdropFar;      // view-depth where the fade completes
uniform float u_farBackdropStrength; // max substitution amount (default 1.0; >1 exaggerates)
uniform vec3  u_farBackdropTint;     // debug/exaggeration additive tint (default 0)
// Slice 2 (object-offload) — Stage 2.D.1: parity write gate.
// 0 (default) = no write to parityOut_; nonzero = write per-vertex lit ARGB.
// 'uniform uint' crashes this engine's shader compile (memory/uniform_uint_crash.md)
// so we use int + a >0 test in GLSL.
uniform int u_parityWrite;
// Per-draw vertex count for the type currently being drawn. Constant
// inside one glDrawElementsInstancedBaseVertex call. Equal to
// typeShape->numTypeTriangles * 3 (expanded/triangle-soup vertex count).
uniform int u_parityVertsPerType;
// VBO base vertex for the current type. gl_VertexID (with baseVertex) is
// absolute; subtract this to get the type-local [0, parityVerts) index.
// Set per-type (constant across all packets of the same type).
uniform int u_parityBaseVertex;
// Diagnostic Addition 3 (Approach A): when non-zero, the shader writes
// light[inst.lightDataIndex].numLights.x into parityOut_[0] from the
// (inst=0, vert=0) invocation only.  All other invocations still write
// their normal lit-ARGB if u_parityWrite>0.  C++ reads slot 0 back and
// prints [PARITY_DIAG v2] event=shader_observed gpu_numLights=N.
// Default 0 = disabled.  Set to 1 per-frame for typeId=474 draws when
// MC2_OBJECT_PARITY_TRACE=1.  'uniform uint' crashes engine compile
// (memory/uniform_uint_crash.md); use int.
uniform int u_parityNumLightsDebugMode;
// Diagnostic Addition 4 (Approach A): when non-zero, the shader writes
// get_base_light()'s RGB output (packed as B|G<<8|R<<16|0xFF<<24) into
// parityOut_[0] from the (inst=0, vert=0) invocation only.  C++ reads
// slot 0 back and prints [PARITY_DIAG v2] event=base_light_observed
// typeId=84 rgb=R,G,B (packed=0xPACKED).  Default 0 = disabled.
// Set to 1 per-frame for typeId=84 draws when MC2_OBJECT_PARITY_TRACE=1.
// 'uniform uint' crashes engine compile (memory/uniform_uint_crash.md).
uniform int u_parityBaseLightDebugMode;

// V-AMBIENT-STATIC-1: hemisphere ambient fill (StaticPropOpaque lane only).
// Strength 0.0 (default) == OFF and is mathematically a no-op (adds vec3(0)
// to lit). CPU uploads 1.0 when MC2_STATIC_PROP_AMBIENT_V1=1 (default OFF).
// Sky/ground are subtle neutrals; first improvement intentionally subtle.
// Axis: worldNormal is in STUFF/model space (a_normal * mat3(inst.modelMatrix)
// — see line ~226). inst.modelMatrix is Stuff-convention (.x=left, .y=elev,
// .z=fwd), and the Stuff->MC2 axis swap at line 156 is applied to POSITION
// ONLY, not to the normal. So Stuff "up" = worldNormal.y here, not .z.
// Window-flag nodes skip this term (they are magic-color only — matches
// existing window-skip of calc_light).
uniform float u_ambientV1Strength;

// PROP-BASE-LIGHT-DIR-1: strength of the directional (hemispherical-wrap)
// modulation applied to the baked per-vertex base_light seed. 0.0 (default,
// CPU uploads 0.0 when MC2_PROP_BASE_LIGHT_DIR is unset/0) makes the term a
// bitwise no-op -- the `> 0.0` guard below skips it entirely, so seed ==
// base_light and the render is byte-identical to pre-slice output.
// Range 0..1: the modulation factor is mix(1-strength, 1, wrap), so a face
// pointing AT the sun keeps factor 1.0 exactly and can never brighten; a
// face pointing directly AWAY is scaled by (1-strength).
uniform float u_propBaseLightDirStrength;

// V-IBL-STATIC-1: SH-L2 image-based ambient on StaticPropOpaque lane.
// Coefficient order (must match projector tools/ibl/project_sh.py +
// generated header RenderCore/IblShCoeffs.h):
//   [0]=L00, [1]=L1-1, [2]=L10, [3]=L11,
//   [4]=L2-2, [5]=L2-1, [6]=L20, [7]=L21, [8]=L22
// Coefficients are RAW projection integrals C_lm = integral(L * Y_lm dw).
// The Ramamoorthi-Hanrahan c1..c5 constants below absorb the per-band
// diffuse cosine-lobe kernel (pi, 2pi/3, pi/4) -- so this evaluator yields
// IRRADIANCE directly. Do NOT premultiply the kernel in the projector,
// and do NOT apply additional /pi at the caller -- c1..c5 are pre-convolved.
// Default u_iblShStrength = 0.0 (CPU uploads 0.0 when env unset/0).
// The `if (u_iblShStrength > 0.0)` guard below short-circuits evalShL2
// entirely -> byte-identical to pre-slice output.
uniform vec3  u_iblSh[9];
uniform float u_iblShStrength;

// V-MATERIAL-PBR-3: PBR uniforms (u_pbrV1Strength, u_pbrV1RoughnessOverride,
// u_pbrV1DiagSunFound) MOVED to static_prop.frag — the PBR math now runs
// per-fragment so the localized specular highlight is no longer averaged
// across the Gouraud-interpolated vertex output. The .vert side now only
// IDENTIFIES the sun (one INFINITE light per ObjectLights entry) and
// forwards (sunDir, sunColor, sunFound) as flat varyings to the fragment.
// CPU upload sites are unchanged: glGetUniformLocation/glUniform* operate
// per-program, stage-agnostic.

// SH-L2 evaluator (Ramamoorthi-Hanrahan 2001 named constants).
// Axis convention: Y-up world (same as V-AMBIENT-STATIC-1 hemi_t at
// static_prop.vert:281). worldNormal is Stuff-space Y-up (model normal *
// mat3(Stuff modelMatrix); the Stuff->MC2 axis swap is applied to position
// only). Projector tools/ibl/project_sh.py uses the same Y-up basis
// (n = (sin(theta)*cos(phi), cos(theta), sin(theta)*sin(phi)), theta from
// +Y) so L20 binds to (3*n.y*n.y - 1) and L22 to (n.x*n.x - n.z*n.z).
vec3 evalShL2(vec3 n) {
    // Named Ramamoorthi-Hanrahan c1..c5 (Eq. 13). These constants ARE the
    // diffuse-kernel-times-basis-normalization-times-polynomial-coefficient
    // products: c1=0.429043 (band L=2 quadratic terms), c2=0.511664 (band L=1
    // linear terms), c3=0.743125 (L20 quadratic weight), c4=0.886227 (L00
    // constant), c5=0.247708 (L20 constant offset). Together they encode
    // the cosine-lobe kernel (pi, 2pi/3, pi/4) baked into the evaluator,
    // so the input u_iblSh[i] are RAW projection coefficients and the output
    // is diffuse irradiance directly.
    const float kSH_c1 = 0.429043;
    const float kSH_c2 = 0.511664;
    const float kSH_c3 = 0.743125;
    const float kSH_c4 = 0.886227;
    const float kSH_c5 = 0.247708;
    // Y-up axis convention: pole on .y. L20 polynomial binds to (3y^2 - 1),
    // L22 polynomial to (x^2 - z^2). Matches projector basis in
    // tools/ibl/project_sh.py (n = (sin(theta)*cos(phi), cos(theta),
    // sin(theta)*sin(phi)) with theta the polar angle from +Y).
    return (
          kSH_c1 * u_iblSh[8] * (n.x * n.x - n.z * n.z)         // L22:  x^2 - z^2
        + kSH_c3 * u_iblSh[6] * (n.y * n.y)                     // L20 quadratic
        + kSH_c4 * u_iblSh[0]                                   // L00
        - kSH_c5 * u_iblSh[6]                                   // L20 offset
        + 2.0 * kSH_c1 * u_iblSh[4] * (n.x * n.y)               // L2-2: xy
        + 2.0 * kSH_c1 * u_iblSh[5] * (n.y * n.z)               // L2-1: yz
        + 2.0 * kSH_c1 * u_iblSh[7] * (n.x * n.z)               // L21:  xz
        + 2.0 * kSH_c2 * u_iblSh[3] * n.x                       // L11:  x
        + 2.0 * kSH_c2 * u_iblSh[1] * n.y                       // L1-1: y (Y-up pole)
        + 2.0 * kSH_c2 * u_iblSh[2] * n.z                       // L10:  z
    );
}

out vec3  v_normal;
out vec2  v_uv;
flat out uint v_flags;
out vec4  v_highlight;
out vec4  v_fog;
out vec4  v_argb;
flat out uint v_localVertexID;
flat out uint v_drawID;          // plan v3.8 Step 8.2: forwarded to FS as
                                 // uint(gl_DrawIDARB) under MC2_COALESCE,
                                 // else 0u (fragment shader's MC2_COALESCE
                                 // branch indexes perDraw_.entries[] by it).

// LIGHTING-DEBUG-VIEWS-1B-STATIC-PROPS: separated lighting channels for the
// unified MC2_LIGHTING_DEBUG_VIEW enum. v_argb already SUMS sun+ambient+ibl so
// the fragment cannot recover them — export the components here. Always written
// (window branch -> 0). Frag reads them only when u_lightingDebugView selects a
// channel, so the default (off) render is pixel-identical.
//   v_dbgSun       = sunColor * NdotL (directional term only)
//   v_dbgAmbient   = ambientTerm + hemisphere-fill + SH-IBL (all fill light)
//   v_dbgLightMeta = (x=lightDataIndex, y=numLights, z/w spare) for count/index views
out      vec3 v_dbgSun;
out      vec3 v_dbgAmbient;
flat out vec4 v_dbgLightMeta;
// FIXTURE-GLOW-1: additive post-modulate glow, already attenuated. vec3(0.0)
// for every non-fixture vertex and for every fixture vertex that does not carry
// an emissive hot-colour tag (the pole/base geometry), so the glow lands on the
// LENS only. Interpolated (not flat) so it falls off smoothly across the lens.
out      vec3 v_fixtureGlow;

// FOLIAGE-STATICPROP-DEPTH-PREPASS-1: guarantee gl_Position is computed
// bit-identically across the two program objects that link this same VS — the
// color program (static_prop.frag) and the depth-prepass program
// (static_prop_depth.frag). Without this, a cross-program compiler reordering
// of the position math could yield a 1-ULP depth difference and the color
// pass's GL_EQUAL test would reject every fragment (props vanish). Global
// redeclaration of the built-in is legal in GLSL and applies to the implicit
// gl_Position output.
invariant gl_Position;

#if defined(MC2_USE_VIEW_UNIFORMS)
// V-MATERIAL-PBR-3: varyings for per-fragment PBR specular.
//   v_worldPos       — world-space fragment position (Stuff-space here, same
//                      as worldPos in main(); used to form view vector via
//                      u_cameraWorldPos in the frag).
//   v_pbrV1SunDir    — sun light direction as stored in ObjectLights
//                      (surface->sun direction NEGATED, per calc_light
//                      convention `dot(N, -ld.light_dir.xyz)`). Frag negates
//                      again to obtain surface->sun L. Flat: constant per
//                      ObjectLights row; no interpolation needed.
//   v_pbrV1SunColor  — sun light color. Flat for same reason.
//   v_pbrV1SunFound  — 0/1 sentinel: did we find a TG_LIGHT_INFINITE entry
//                      in ObjectLights[lightDataIndex]? Flat int.
out      vec3 v_worldPos;
flat out vec3 v_pbrV1SunDir;
flat out vec3 v_pbrV1SunColor;
flat out int  v_pbrV1SunFound;
#endif

void main() {
    // Plan v3.8 Step 8.1 — coalesce variant indexes by
    // (gl_BaseInstanceARB + gl_InstanceID): the multi-draw issues N
    // instances starting at gl_BaseInstanceARB, and gl_InstanceID
    // restarts at 0 each draw call. Legacy single-draw uses bare
    // gl_InstanceID.  ARB-suffixed builtins are mandatory under
    // #version 430 + GL_ARB_shader_draw_parameters; the unsuffixed
    // gl_BaseInstance / gl_DrawID are the GL 4.6 core promotion names
    // and are NOT defined under the extension.
    // Plan v3.8 Step 8.2 — v_drawID forwarding to fragment.
#ifdef MC2_COALESCE
    Instance inst = instances_.i[gl_BaseInstanceARB + gl_InstanceID];
    v_drawID      = uint(gl_DrawIDARB);
#else
    Instance inst = instances_.i[gl_InstanceID];
    v_drawID      = 0u;
#endif
    // modelMatrix from SSBO std430 default col-major: GLSL sees the same
    // matrix as memory. For Stuff row-vec convention (translation in row 3),
    // use `v * M` to apply translation.
    vec4 world_stuff = vec4(a_position, 1.0) * inst.modelMatrix;
    // 2026-05-04 fix: inst.modelMatrix is shapeToWorld in Stuff/MLR camera
    // frame (.x=left, .y=elev, .z=forward). terrainMVP expects MC2 world
    // (x=east, y=north, z=elev). Apply the documented mapping
    // (memory/cpu_displacement_done.md):
    //     terrain.x = -camera.x, terrain.y = camera.z, terrain.z = camera.y
    // Without this swap, world.y (elev) gets read by terrainMVP as MC2
    // north and world.z (south distance) as MC2 elev → assets misplaced
    // along a wrong axis (visible 2026-05-04 as "trees in the sky" pattern).
    vec3 world_mc2 = vec3(-world_stuff.x, world_stuff.z, world_stuff.y);

    // LIGHT-BILLBOARD-GPU-1 (master-FAIL #8): rigid camera-facing billboard +
    // minimum screen size for beacon/runway light fixtures (flags bit 4,
    // emitted only for TG_Shape::isLightBillboard children under
    // MC2_LIGHT_BILLBOARD_FACING; bit 5 = debug exaggeration).
    //
    // WHAT THESE ACTUALLY ARE (measured from data/tgl/runwaylight.ase et al —
    // the predecessor lane's "single flat quad" description is WRONG): a
    // 6-vertex / 7-face truncated triangular prism, ~2.85 units tall. The
    // three side faces carry the dark post colour; the ONLY emissive element
    // is the small horizontal top-cap triangle (~0.79 unit radius, vertex
    // colour 1,1,0.435).
    //
    // THE REAL BUG, reproduced stock at a fixed camera POSITION with only the
    // pitch changed (mc2_10 night airbase, bookmarks 204753 vs 204907): at a
    // higher angle every runway edge light is a visible dot; at a grazing
    // angle the runways are bare. The cap is horizontal, so its projected area
    // falls off with the view angle, and since the whole fixture already
    // subtends only 1-2 pixels at RTS ranges the cap drops below one pixel and
    // is simply never rasterised. This is a sub-pixel geometry problem, not a
    // backface-culling one — which is why the predecessor lane's two-sided CPU
    // change measured near-null, and why a two-sided GPU draw bucket would
    // ALSO have failed to fix it.
    //
    // THE FIX, in two parts:
    //   1. Rigid spherical billboard. The whole fixture is rotated about its
    //      own origin so its local +Z (the cap normal / post axis) points at
    //      the eye, using the RIGID per-instance basis (R1,U1,toCam) applied
    //      to the LOCAL vertex coordinates. Using the per-VERTEX normal here
    //      instead would rotate each vertex by a different amount and tear the
    //      prism apart (observed: the lights vanished entirely). The local
    //      frame and (R1,U1,toCam) are both right-handed, so the mapping is a
    //      pure rotation: shape, scale and TRIANGLE WINDING are preserved and
    //      the cap can never be back-face culled either.
    //   2. Minimum screen size. Even facing the eye, the cap is sub-pixel at
    //      range, so the fixture is uniformly scaled about its origin until
    //      its silhouette spans at least kLbMinNdc in NDC. The clamp is
    //      max(1.0, ...) — inactive at close range, where the geometry is
    //      therefore untouched.
    //
    // Frame note: world_mc2 and u_cameraWorldPos are the SAME space (see
    // static_prop.frag's view-vector reconstruction, which subtracts
    // v_worldPos == world.xyz from u_cameraWorldPos). World up in MC2 is +Z.
#if defined(MC2_USE_VIEW_UNIFORMS)
    const uint kFlagIsLightBillboard  = (1u << 4);
    const uint kFlagLightBillboardBig = (1u << 5);
    if ((inst.flags & kFlagIsLightBillboard) != 0u) {
        vec4 origin_stuff = vec4(0.0, 0.0, 0.0, 1.0) * inst.modelMatrix;
        vec3 origin_mc2   = vec3(-origin_stuff.x, origin_stuff.z, origin_stuff.y);
        const vec3 kUpMc2 = vec3(0.0, 0.0, 1.0);
        vec3 toCamRaw     = u_cameraWorldPos.xyz - origin_mc2;
        vec3 r1raw        = cross(kUpMc2, toCamRaw);
        vec4 clipOrigin   = u_worldToClipGL * vec4(origin_mc2, 1.0);
        // Degenerate guards: eye directly overhead (r1raw ~ 0), coincident
        // eye, or the fixture behind the eye — leave the geometry stock
        // rather than divide by ~0.
        if (dot(r1raw, r1raw) > 1e-4 && dot(toCamRaw, toCamRaw) > 1e-4 &&
            clipOrigin.w > 1e-3) {
            vec3 toCam = normalize(toCamRaw);
            vec3 R1    = normalize(r1raw);
            vec3 U1    = cross(toCam, R1);   // (R1,U1,toCam) right-handed

            // RUNWAY-MARKER-LIGHTS-1 — two corrections to this clamp. Read
            // this before touching the constants; the user verdict on the
            // uncapped version was "atrociously hideous".
            //
            // (a) kLbRefRadius was the authored CAP radius (~0.79). But the
            //     rigid billboard below maps local Z (the 2.85-unit post axis)
            //     onto toCam, so the post foreshortens and what the viewer
            //     actually sees is the local XY cross-section — measured from
            //     runwaylight.ase, extents 2.015 x 2.326, i.e. a silhouette
            //     radius near 1.2, not 0.8. Feeding the clamp the cap radius
            //     made it over-inflate every fixture by ~1.5x beyond intent.
            //
            // (b) THE STREAK BUG. The clamp was max(1.0, ...) with NO upper
            //     bound. sil falls off as 1/distance, so k rose without limit
            //     and pinned every fixture to a CONSTANT ~4-7px on screen at
            //     EVERY distance. At map-overview altitude, where a runway
            //     light should be sub-pixel or gone, all several hundred of
            //     them were still full-size, full-brightness, hard-edged
            //     triangles — and being regularly spaced along the runway and
            //     taxiway edges they read as bright white dashed slashes
            //     across the dark valley. Capping k restores the normal
            //     1/distance shrink beyond the cap: the fixture is guaranteed
            //     to survive rasterisation at playable RTS ranges (the point
            //     of the original fix) and then fades back out on its own at
            //     overview altitude instead of screaming.
            const float kLbMinNdc     = 0.0012;
            const float kLbRefRadius  = 1.2;
            const float kLbMaxScale   = 2.5;
            // NDC-per-world-unit across the screen at this depth: push the
            // billboard's own right axis through the clip matrix as a
            // DIRECTION (w=0) and divide by the origin's clip w.
            vec4 clipR      = u_worldToClipGL * vec4(R1, 0.0);
            float ndcPerUnit = length(clipR.xy) / clipOrigin.w;
            float k = 1.0;
            float sil = kLbRefRadius * ndcPerUnit;
            if (sil > 1e-9) k = clamp(kLbMinNdc / sil, 1.0, kLbMaxScale);
            // DEBUG ONLY (MC2_LIGHT_BILLBOARD_FACING=2): oversize the fixtures
            // so the affected population is unmistakable in a capture.
            if ((inst.flags & kFlagLightBillboardBig) != 0u) k *= 4.0;

            // Rigid: LOCAL coordinates re-emitted in the camera-facing basis.
            // Recover the instance's uniform scale from the model matrix so a
            // scaled placement keeps its size (Stuff row-vector convention:
            // local +X maps to world through matrix ROW 0, i.e. the .x of each
            // column). Rotation is discarded on purpose — that is the point.
            float mScale = length(vec3(inst.modelMatrix[0].x,
                                       inst.modelMatrix[1].x,
                                       inst.modelMatrix[2].x));
            vec3 p    = a_position * (k * mScale);
            world_mc2 = origin_mc2 + R1 * p.x + U1 * p.y + toCam * p.z;
        }
    }
#endif

    vec4 world = vec4(world_mc2, 1.0);
    // Match terrain_overlay.vert exactly: terrainMVP is the CPU-composed
    // axisSwap * worldToClip matrix uploaded GL_FALSE.
    // Apply the same reverse-Z surface bias as terrain so props at coplanar
    // or near-coplanar mountain geometry win the GL_GEQUAL depth test.
    // Shadow pass uses shadow_static_prop.vert (separate) — no guard needed.
    // `invariant gl_Position` guarantees depth prepass + color pass are
    // bit-identical after the bias, so the depth-prepass GL_EQUAL gate holds.
    vec4 clip = u_worldToClipGL * world;
    // TERRAIN-DEPTH-BIAS-OWNERSHIP-1: terrain now writes TRUE depth (0). Props
    // take OBJECT_DEPTH_BIAS (> OVERLAY) so they sit ON TOP of cement/road
    // transition tiles (no sinking) and win the GEQUAL tie over coplanar ground,
    // while the bias is tiny enough that the reverse-Z world band is sub-visible
    // (no show-through behind distant terrain). invariant gl_Position keeps the
    // depth-prepass GL_EQUAL gate exact (single VS).
    clip.z += OBJECT_DEPTH_BIAS * clip.w;
    gl_Position = clip;

    // Slice 2 (object-offload) — Stage 2.C.2: GPU vertex lighting.
    //
    // 1. Decode the per-vertex aRGBLight tag (a_aRGBLight) into a vec4 in
    //    B,G,R,A order to match get_base_light()'s expected swizzle. On
    //    little-endian x86 the C++ DWORD memcpy at registerType() lands as
    //    B,G,R,A bytes in memory; GL_UNSIGNED_INT pulls all 4 bytes as a
    //    single uint, then we extract per-byte and pack into vec4 with
    //    .xyzw = (B,G,R,A) so get_base_light()'s b|g<<8|r<<16|a<<24 decode
    //    matches mclib/tgl.cpp's per-vertex hot-color magic comparisons.
    vec4 perVertexARGB;
    perVertexARGB.x = float((a_aRGBLight >>  0) & 0xFFu) / 255.0;  // b
    perVertexARGB.y = float((a_aRGBLight >>  8) & 0xFFu) / 255.0;  // g
    perVertexARGB.z = float((a_aRGBLight >> 16) & 0xFFu) / 255.0;  // r
    perVertexARGB.w = float((a_aRGBLight >> 24) & 0xFFu) / 255.0;  // a

    // 2. Look up per-type hot-color magic. inst.typeID indexes the per-type
    //    SSBO populated at finalizeGeometry().
    PerTypeData ptd = perType_.t[inst.typeID];

    // 3. Decode base lighting (resolves the hot-color magic tags).
    //    isNight/nightFactor are stubbed to false/0 for now — the legacy
    //    gos_tex_vertex_lighted shader at line 75 does the same pending the
    //    eye-state UBO wiring.
    //
    // Stage 2.C.4 N1.5 (revised): pass lightsOut INTO get_base_light() via
    // its existing 5th parameter rather than short-circuiting at the call
    // site. The prior short-circuit (if (kFlagIsLightsOut) base_light=vec3(0))
    // was over-broad: it suppressed ALL aRGBLight values including magic-tag
    // branches (hot-pink/yellow/green) that CPU handles unconditionally
    // BEFORE consulting lightsOut. CPU's `if (!lightsOut)` gate at
    // tgl.cpp:1880 is ONLY inside the `else if (startVLight & 0x00ffffff)`
    // non-magic colored-seed arm. get_base_light()'s existing gate at
    // lighting.hglsl:116 is already in the right place — passing the real
    // lightsOut bit lets the function-internal gate fire for only that arm,
    // exactly mirroring CPU behavior.
    //
    // Example: hot-pink + lightsOut=1 + isWindow=1:
    //   CPU:  magic-tag arm wins (tgl.cpp:1804-1828, sets 0x2F2F2F); lightsOut
    //         NOT consulted; window-skip suppresses lighting loop.
    //   GPU pre-fix: N1.5 short-circuit fires → base_light=vec3(0) → lit=0 → 0x000000 WRONG.
    //   GPU post-fix: get_base_light runs hot-pink branch → vec3(0x2F/255); window-skip
    //         passes lit=base_light → 0x2F2F2F. MATCHES CPU.
    const uint kFlagIsLightsOut = (1u << 0);
    bool lightsOut = (inst.flags & kFlagIsLightsOut) != 0u;
    // EDGE-LIGHT-EMISSION-1: evaluate get_base_light TWICE -- once with the
    // night state forced off (exactly the pre-fix "stubbed false/0" result)
    // and once with the eye's real night state. The difference is the stock
    // NIGHT EMISSION and nothing else, so scaling only that delta is provably
    // a no-op at scale 0 and exact stock at scale 1. The tagged vertices are
    // self-illuminated by construction: this term is ADDITIVE and computed
    // before ambient/sun, so night ambient never darkens it.
    vec3 base_day = get_base_light(
        perVertexARGB,
        false, 0.0, false, lightsOut,
        ptd.hotPinkRGB.rgb,
        ptd.hotYellowRGB.rgb,
        ptd.hotGreenRGB.rgb);
    vec3 base_light = base_day;
    // BEACON-FIXTURE-ID-1: pick the emission scale by fixture identity. A
    // fixture takes max(all-props scale, fixture scale) so the two gates
    // compose instead of fighting; everything else keeps the all-props scale,
    // which is 0.0 by default -> base_light == base_day -> byte-identical.
    // NOTE: this scales the emission on the fixture's REAL geometry only. It
    // deliberately introduces no screen-size clamp and no billboard -- the
    // unbounded min-screen-size clamp is exactly what produced the
    // "atrociously hideous" overview-altitude streaks in RUNWAY-MARKER-LIGHTS-1
    // (and why MC2_LIGHT_BILLBOARD_FACING was defaulted OFF). The emission
    // therefore shrinks and fades with distance on its own.
    bool  isFixture     = (ptd.hotPinkRGB.w > 0.5);
    float emissiveScale = isFixture
                        ? max(u_nightEmissiveScale, u_fixtureEmissiveScale)
                        : u_nightEmissiveScale;
    // FIXTURE-GLOW-1: night weight, 0.0 in full daylight. Used both to gate the
    // emissive block (unchanged) and to fade the additive glow, so there is no
    // dusk pop and no daylight glow.
    float nightWeight = clamp(max(float(u_isNight), u_nightFactor), 0.0, 1.0);
    bool  glowWanted  = isFixture && u_fixtureGlowStrength > 0.0;
    v_fixtureGlow = vec3(0.0);
    if ((emissiveScale > 0.0 || glowWanted) && nightWeight > 0.0) {
        vec3 base_night = get_base_light(
            perVertexARGB,
            u_isNight != 0, u_nightFactor, false, lightsOut,
            ptd.hotPinkRGB.rgb,
            ptd.hotYellowRGB.rgb,
            ptd.hotGreenRGB.rgb);
        // Byte-identity guard: the emissive assignment stays behind its ORIGINAL
        // condition, so turning the glow on cannot perturb base_light by so much
        // as an ulp, and turning the glow off restores the exact prior program.
        if (emissiveScale > 0.0) {
            base_light = clamp(base_day + (base_night - base_day) * emissiveScale,
                               vec3(0.0), vec3(1.0));
        }
        if (glowWanted) {
            // The night EMISSION DELTA is the glow tint. Using the delta -- not a
            // per-type constant -- is what confines the glow to the emissive lens
            // vertices: the fixture's post/base carries no magic tag, so its delta
            // is exactly vec3(0.0) and it stays unlit. It also gives each fixture
            // its own colour for free (white runwaylight, red/green/blue beacons)
            // with no extra plumbing.
            vec3 tint = clamp(base_night - base_day, vec3(0.0), vec3(1.0));
            // White-hot core: push toward the tint's own peak channel rather than
            // toward vec3(1.0), so vec3(0.0) maps to vec3(0.0) and the "lens only"
            // property above survives the whitening.
            float peak = max(tint.r, max(tint.g, tint.b));
            vec3  core = mix(tint, vec3(peak), kFixtureGlowWhite);
            // BOUNDED attenuation on APPARENT SIZE. ndcPerUnit = NDC covered by
            // one world unit at this depth; push two world axes through the clip
            // matrix as DIRECTIONS (w=0) and divide by the vertex's clip w. The
            // max() over two axes avoids the degenerate case where one axis
            // points nearly along the view direction.
            vec4  clipAxX    = u_worldToClipGL * vec4(1.0, 0.0, 0.0, 0.0);
            vec4  clipAxY    = u_worldToClipGL * vec4(0.0, 1.0, 0.0, 0.0);
            float ndcPerUnit = max(length(clipAxX.xy), length(clipAxY.xy))
                             / max(clip.w, 1e-3);
            float sil        = kFixtureGlowRefRadius * ndcPerUnit;
            // smoothstep saturates at both ends, so atten is in [0,1] and is
            // EXACTLY 0.0 for any sil <= kFixtureGlowSilMin. Nothing here can
            // grow without bound and nothing here touches geometry or size.
            float atten = smoothstep(kFixtureGlowSilMin, kFixtureGlowSilFull, sil);
            v_fixtureGlow = core * (u_fixtureGlowStrength * atten * nightWeight);
        }
    }

    // 4. Compute world-space normal and position for calc_light's distance
    //    math. modelMatrix is `v * M` form (Stuff convention, SSBO std430),
    //    so `vec4(p,1) * inst.modelMatrix` = world-space position. Normals
    //    must use the same row-vector convention: `a_normal * mat3(M)`.
    //    Using `mat3(M) * a_normal` (column-vector) applies the INVERSE
    //    rotation and produces wrong lighting for any non-identity rotation.
    //    Proved by Stage 2.D.2 parity: ~94% mismatch before fix, 0% after.
    //
    //    Stage 2.D.2.1 (m5) NOTE: ASSUMES no non-uniform scale on static-prop
    //    shapeToWorld. If non-uniform scale is ever introduced, the correct
    //    transform for normals is the inverse-transpose of the 3x3 submatrix,
    //    not `a_normal * mat3(M)` — that fix becomes WRONG under non-uniform
    //    scale and would silently produce incorrect lighting.
    vec3 worldNormal = a_normal * mat3(inst.modelMatrix);
    vec3 worldPos    = world.xyz;

    // STATIC-PROP-NORMAL-FRAME-FIX: match mech's Stuff->MC2 normal swap (was
    // Stuff-frame -> 90deg off vs terrain/mech sun). worldPos above is already
    // MC2-frame (world.xyz = the Stuff->MC2 swap at line 251); the normal was
    // left in Stuff frame, so calc_light's NdotL mixed frames -> 90deg-rotated
    // sun on static props. Mirror mech.vert:162
    // normalize(vec3(-normalStuff.x, normalStuff.z, normalStuff.y)) so the
    // normal and position fed to calc_light share the MC2 frame, exactly as
    // mech passes (worldMC2 normal + worldMC2 position).
    vec3 worldNormalMC2 = normalize(vec3(-worldNormal.x, worldNormal.z, worldNormal.y));

    // 5. Per-vertex full 6-type lighting via lighting.hglsl calc_light.
    //    inst.lightDataIndex addresses one ObjectLights entry in the
    //    LightsData[32] UBO populated per-actor by GatherGpuObjectLightDataOnly().
    //
    //    isWindow (inst.flags bit 1): mirrors CPU tgl.cpp:1929 `!isWindow` guard.
    //    Window nodes (LitWin_* node names) skip ambient + directional lighting
    //    so their hot-color magic (dark-grey daytime, glowing at night) is
    //    unaffected by the sun direction. GPU must match: skip calc_light when
    //    isWindow is set, returning only get_base_light() output.
    //    Stage 2.D.2 parity proved this is the cause of ~86% mismatch on window
    //    building types — GPU was adding lighting where CPU skips it.
    //
    //    [T3.2] Bit-2 (kFlagIsSpotlight) read deleted. Post-T3.1 the C++
    //    submit path skips spotlight children unconditionally (T1.3 ->
    //    unconditional `continue` in gos_static_prop_batcher.cpp
    //    submitMultiShape). The CPP-side bit-2 emission is dead, so the
    //    shader-side bit-2 read is removed in lockstep per
    //    memory/cpp_glsl_ubo_struct_lockstep.md. Spotlights now contribute
    //    via real TG_Light registrations (BldgAppearance::update), not via
    //    cone-billboard packets.
    const uint kFlagIsWindow    = (1u << 1);
    // STATIC-PROP-FOLIAGE-NdotL: two-sided lighting for alpha-test props.
    // Cross-quad tree geometry has face normals; orbiting camera reveals
    // different faces with different NdotL → dark face appears to move with
    // camera. abs(NdotL) lights both sides consistently.
    const uint kFlagIsAlphaTest = (1u << 2u);
    // LIGHTING-DEBUG-VIEWS-1B: defaults. lightDataIndex needs no SSBO read;
    // numLights (.y) is filled inside the non-window branch from ld_sun.
    v_dbgSun       = vec3(0.0);
    v_dbgAmbient   = vec3(0.0);
    v_dbgLightMeta = vec4(float(inst.lightDataIndex), 0.0, 0.0, 0.0);
    vec3 lit;
    if ((inst.flags & kFlagIsWindow) != 0u) {
        // Window node: hot-color magic only, no sun/ambient lighting.
        lit = base_light;
    } else {
        // STATIC-PROP-TERRAIN-SUN-DIFFUSE: use terrain sun (ground-shadow sun)
        // for the directional term; calc_light's frozen worldLights[0] sun was
        // ~90deg off. Keeps sun color + ambient.
        //
        // Replicate calc_light's structure (return = base_light + ambientSum +
        // sum_over_dir_lights(NdotL * lcolor)) but substitute u_terrainSunMC2
        // for the broken sun light_dir. We scan the same ObjectLights row:
        //   - AMBIENT (type 0) light_color accumulates into ambientTerm
        //     (identical to calc_light's `ambient += lcolor`).
        //   - The first INFINITE/INFINITEWITHFALLOFF (type 1/2) entry is the
        //     sun; we take its light_color as sunColor but IGNORE its broken
        //     light_dir, using u_terrainSunMC2 (raw-MC2 terrain sun) instead.
        //   - Point/spot lights are omitted (negligible for buildings); noted.
        ObjectLights ld_sun = light[int(inst.lightDataIndex)];
        vec3 ambientTerm = vec3(0.0);
        vec3 sunColor    = vec3(0.0);
        bool sunFound    = false;
        int n_sun = min(ld_sun.numLights.x, MAX_LIGHTS_IN_WORLD);
        v_dbgLightMeta.y = float(ld_sun.numLights.x);  // LIGHTING-DEBUG-VIEWS-1B: light-count view
        for (int i = 0; i < n_sun; ++i) {
            int lt = int(ld_sun.light_dir[i].w);
            if (lt == TG_LIGHT_AMBIENT) {
                ambientTerm += ld_sun.light_color[i].xyz;
            } else if (!sunFound &&
                       (lt == TG_LIGHT_INFINITE || lt == TG_LIGHT_INFINITEWITHFALLOFF)) {
                sunColor = ld_sun.light_color[i].xyz;
                sunFound = true;
            }
        }
        float ndl;
        if ((inst.flags & kFlagIsAlphaTest) != 0u) {
            ndl = abs(dot(normalize(worldNormalMC2), normalize(u_terrainSunMC2)));
        } else {
            ndl = max(dot(normalize(worldNormalMC2), normalize(u_terrainSunMC2)), 0.0);
        }
        // base_light = hot-color magic seed (same as calc_light's starting
        // `final = base_light`); ambientTerm + sunColor*NdotL = the lit term.
        //
        // FAR-BACKDROP-LIT: fade the stale per-vertex seed toward the mission's
        // night object-light ambient (ambientTerm, which derives from the
        // per-actor terrain light color set at bdactor.cpp:6403-6407 via the
        // gathered ObjectLights row) as the fragment's view depth crosses the
        // LOD impostor distance. Far/impostor LODs carry a frozen day/black
        // base_light because they skip the per-vertex night bake; near LODs
        // (small clip.w) keep their correct baked seed (fbT==0). u_farBackdropLit
        // ==0 -> fbT==0 -> seed==base_light -> byte-identical pre-change output.
        // PROP-BASE-LIGHT-DIR-1 ---------------------------------------------
        // THE USER'S DEFECT: "this building face should be in shadow as the
        // light is from behind." It is not a shadow bug. base_light is a
        // baked per-vertex constant with NO ORIENTATION TERM, so every face
        // of a building carries essentially the same value regardless of
        // which way it points. A face with the light behind it already has
        // ndl == 0, so its sun term is already zero and there is nothing for
        // any shadow term to attenuate -- which is exactly why
        // MC2_PROP_SHADOW_RECEIVE measured <=0.05% of pixels and 0.000% at
        // night (see .claude/SHADOW-DECISIONS-1-STATUS.md §2). The face is
        // bright because of base_light. So we modulate base_light itself.
        //
        // Form: multiplicative hemispherical wrap against the SAME mission
        // sun the diffuse term uses (u_terrainSunMC2, direction-TO-sun).
        //   wrap   = 0.5 + 0.5*dot(N, L)   -- 1.0 facing sun, 0.0 facing away
        //   factor = mix(1 - strength, 1, wrap)
        //
        // Three properties, each answering a stated constraint:
        //  1. NEVER BRIGHTENS. factor <= 1.0 with equality only at wrap==1.
        //     This redistributes, it does not add -- so it also helps rather
        //     than hurts the separate "buildings are over-bright at night"
        //     complaint, and composes safely with the sibling lane's
        //     MC2_STATIC_PROP_IBL_NIGHT_SCALE.
        //  2. PRESERVES AUTHORED VARIATION. It is a MULTIPLY on the existing
        //     per-vertex seed, not a replacement or a tone remap, so the
        //     asset's authored per-vertex differences survive scaled. This
        //     is deliberately not the tone-flattening that was rejected
        //     elsewhere ("after is WAY worse").
        //  3. NO DOUBLE-COUNT WITH THE IBL SH PROBE. The SH probe (V-IBL-
        //     STATIC-1, u_iblShStrength) is an ADDITIVE ambient irradiance
        //     term added to `lit` further down. This is a MULTIPLICATIVE
        //     modulation of a different summand (the baked seed). Neither
        //     term feeds the other, and we never scale the IBL contribution.
        //
        // Applied BEFORE the FAR-BACKDROP-LIT mix on purpose: distance-faded
        // impostors then converge on the untouched ambient target, so the far
        // horizon is unchanged and no new LOD-pop boundary is introduced.
        //
        // ALPHA-TEST INSTANCES ARE EXCLUDED. Foliage/fence cards are two-sided
        // (the ndl branch above uses abs() for exactly this reason) and their
        // normals do not describe a solid surface, so a signed wrap would
        // darken whichever side happened to be authored facing away -- an
        // arbitrary result. Buildings, the subject of the complaint, are not
        // alpha-test.
        //
        // WHY THE SHADER AND NOT THE CPU BAKE: a bake change would not reach
        // far/impostor LODs at all, because those register via
        // TransformMultiShape_HierarchyOnly and never run the per-vertex light
        // bake. It would also be a per-mission-light-direction rebake. Doing
        // it here reaches every LOD uniformly for one dot + one mix per
        // vertex, on a path that already normalizes both operands for ndl.
        vec3 baseLightDir = base_light;
        if (u_propBaseLightDirStrength > 0.0 &&
            (inst.flags & kFlagIsAlphaTest) == 0u) {
            float wrap = 0.5 + 0.5 * dot(normalize(worldNormalMC2),
                                         normalize(u_terrainSunMC2));
            baseLightDir = base_light
                         * mix(1.0 - u_propBaseLightDirStrength, 1.0, wrap);
        }
        // --------------------------------------------------------------------

        vec3 seed = baseLightDir;
        if (u_farBackdropLit != 0) {
            float fbDepth = clip.w;  // perspective w == view-space distance proxy
            float fbT = clamp((fbDepth - u_farBackdropNear)
                              / max(u_farBackdropFar - u_farBackdropNear, 1.0),
                              0.0, 1.0) * u_farBackdropStrength;
            // Target night shade = object-light ambient (+ optional debug tint
            // for verify-by-exaggeration). mix() lifts black-seeded far geometry
            // UP to the night ambient and pulls white/day-seeded impostors DOWN
            // to it — a single operation fixing both symptoms with no hard LOD-
            // pop boundary (smooth distance fade).
            seed = mix(baseLightDir, ambientTerm + u_farBackdropTint, fbT);
        }
        lit = seed + ambientTerm + sunColor * ndl;

        // V-AMBIENT-STATIC-1: hemisphere fill. Strength=0.0 (default) is a
        // bitwise no-op (add vec3(0)). Skip for window branch to preserve
        // hot-color magic behavior exactly. Axis: worldNormal is STUFF-space
        // (model normal * mat3(Stuff modelMatrix); the Stuff->MC2 swap at
        // line 156 transforms POSITION only). In Stuff convention .y is
        // elevation, so +y = up. Using .z here would gradient along the
        // Stuff fwd axis ("north/depth"), making north-facing walls
        // brighten instead of rooftops — see reviewer finding pre-fix.
        const vec3 kAmbientV1Sky    = vec3(0.20, 0.22, 0.28);
        const vec3 kAmbientV1Ground = vec3(0.10, 0.09, 0.07);
        float hemi_t = 0.5 + 0.5 * worldNormal.y;
        vec3 ambient_v1 = mix(kAmbientV1Ground, kAmbientV1Sky, hemi_t)
                          * u_ambientV1Strength;
        lit += ambient_v1;

        // V-IBL-STATIC-1: SH-L2 image-based ambient. Strength gate via env
        // (s_iblShEnabled in batcher); when OFF the CPU uploads 0.0 and the
        // guard short-circuits evalShL2 entirely -> byte-identical pixel.
        // worldNormal is Stuff-space Y-up, matching the projector basis
        // (see tools/ibl/project_sh.py axis-convention block).
        if (u_iblShStrength > 0.0) {
            vec3 ibl = evalShL2(normalize(worldNormal));
            lit += ibl * u_iblShStrength;
        }

        // LIGHTING-DEBUG-VIEWS-1B: split the summed lighting into channels.
        // At this point lit = base_light + ambientTerm + sunColor*ndl +
        // ambient_v1 + ibl. Sun term is the directional component; everything
        // else above base_light (hot-color seed) is fill/ambient.
        v_dbgSun     = sunColor * ndl;
        // PROP-BASE-LIGHT-DIR-1: decompose against baseLightDir (the seed that
        // actually entered `lit`), not the raw base_light -- otherwise the
        // directional modulation would be misattributed to the ambient channel.
        v_dbgAmbient = lit - baseLightDir - sunColor * ndl;

        // V-MATERIAL-PBR-3: per-vertex PBR math REMOVED — moved to
        // static_prop.frag so Schlick-Fresnel + power-lobe specular runs
        // per-fragment and produces a tight localized highlight instead of
        // a Gouraud-averaged uniform per-prop brightening. The .vert side
        // continues to IDENTIFY the sun light (one TG_LIGHT_INFINITE entry
        // per ObjectLights row, by convention) and forwards (sunDir,
        // sunColor, sunFound) via flat varyings — sun state is constant
        // per ObjectLights entry, so flat-shading is correct (and cheaper
        // than per-fragment SSBO iteration). View vector is reconstructed
        // in the frag from u_cameraWorldPos minus v_worldPos.
    }
#if defined(MC2_USE_VIEW_UNIFORMS)
    // V-MATERIAL-PBR-3: populate sun-light varyings for the frag PBR block.
    // Runs unconditionally (does NOT depend on u_pbrV1Strength) because
    // flat varyings must be written by the provoking vertex even when the
    // frag-side gate is off. Cost is one ObjectLights SSBO read + a short
    // scan that early-exits at the first INFINITE light.
    //
    // Sun direction convention: lighting.hglsl calc_light's INFINITE branch
    // uses `dot(normal, -ld.light_dir.xyz)`, so light_dir.xyz is the
    // surface->sun direction NEGATED. Frag re-negates to recover L.
    // Stock MC2 missions use TG_LIGHT_INFINITEWITHFALLOFF (type 2) for the
    // sun, not TG_LIGHT_INFINITE (type 1); both are directional so the same
    // direction/color extraction applies. Accept either.
    {
        ObjectLights ld_pbr = light[int(inst.lightDataIndex)];
        v_pbrV1SunDir   = vec3(0.0);
        v_pbrV1SunColor = vec3(0.0);
        v_pbrV1SunFound = 0;
        int n_pbr = min(ld_pbr.numLights.x, MAX_LIGHTS_IN_WORLD);
        for (int i = 0; i < n_pbr; ++i) {
            int lt = int(ld_pbr.light_dir[i].w);
            if (lt == TG_LIGHT_INFINITE || lt == TG_LIGHT_INFINITEWITHFALLOFF) {
                v_pbrV1SunDir   = ld_pbr.light_dir[i].xyz;
                v_pbrV1SunColor = ld_pbr.light_color[i].xyz;
                v_pbrV1SunFound = 1;
                break;
            }
        }
        v_worldPos = worldPos;
    }
#endif

    // 6. aRGBHighlight additive contribution — mirrors CPU tgl.cpp:2313-2335.
    //    CPU adds R/G/B channels of aRGBHighlight (objective/selection tint)
    //    to the lit argb, clamping each channel to [0, 255]. Must apply
    //    BEFORE the parity pack below so the readback includes the highlight.
    //    inst.aRGBHighlight is stored as (R,G,B,A) float[4] by submit() at
    //    gos_static_prop_batcher.cpp:745-748 (decode from packed ARGB uint).
    //    When no highlight is active the CPU passes aRGBHighlight=0 → r/g/b=0
    //    → no-op add, matching tgl.cpp:2313 `if (aRGBHighlight)` gate.
    //    Stage 2.D.2.1 (M3): closes the gap vs CPU for highlighted actors.
    lit = clamp(lit + inst.aRGBHighlight.rgb, 0.0, 1.0);

    // 6b. Output. Alpha is always 1.0 — CPU tgl.cpp:2225 hardcodes
    //    `(0xFF << 24) | r | g | b`; the raw aRGBLight alpha byte is
    //    NEVER propagated into the per-vertex lit argb. GPU must match.
    //    (The alpha in some aRGBLight tags like 0x02FFFFFF or 0xFAxxxxxx
    //    is MC2 data that the CPU simply ignores for vertex lighting.)
    v_argb = vec4(lit, 1.0);

    // 7. Slice 2 (object-offload) — Stage 2.D.1: parity readback harness.
    //    Pack the per-vertex lit ARGB into a single uint matching the
    //    in-memory B,G,R,A byte order convention used everywhere in this
    //    engine (see memory/mc2_argb_packing.md and the
    //    a_aRGBLight decode at lines 107-110 above). The CPU side that
    //    Stage 2.D.2/2.D.3 will compare against (listOfTriangles[].aRGBLight)
    //    is also a DWORD-as-BGRA, so this packing keeps the bytewise
    //    compare arithmetic-free.
    //
    //    The bound parity buffer range is sized exactly
    //    instanceCount * u_parityVertsPerType uint entries on the C++ side,
    //    so this index is always in-bounds when u_parityWrite > 0.
    if (u_parityWrite > 0) {
        uint b8 = uint(clamp(lit.b * 255.0, 0.0, 255.0));
        uint g8 = uint(clamp(lit.g * 255.0, 0.0, 255.0));
        uint r8 = uint(clamp(lit.r * 255.0, 0.0, 255.0));
        // Alpha is always 0xFF to match CPU tgl.cpp:2225 which hardcodes
        // (0xFF << 24) and ignores the raw aRGBLight alpha byte.
        uint a8 = 255u;
        uint bits = b8 | (g8 << 8) | (r8 << 16) | (a8 << 24);
        // gl_VertexID = IBO[i] + baseVertex (from glDrawElementsInstancedBaseVertex).
        // Subtract u_parityBaseVertex (the type's VBO baseVertex) to get the
        // type-local index in [0, u_parityVertsPerType). This keeps the write
        // in-bounds for types whose VBO region doesn't start at 0.
        int localVert = gl_VertexID - u_parityBaseVertex;
        int idx = gl_InstanceID * u_parityVertsPerType + localVert;
        parityOut_.parityOut[idx] = bits;

        // Addition 3 (Approach A): diagnostic numLights capture.
        // When u_parityNumLightsDebugMode==1 and this is the (inst=0, vert=0)
        // invocation, write the raw numLights.x value into slot 0 of the parity
        // buffer so C++ can read it back.  This overwrites the lit-ARGB at that
        // slot; the C++ side skips the normal parity compare for typeId=474 on
        // this frame and instead prints the numLights value.
        if (u_parityNumLightsDebugMode > 0 &&
            gl_InstanceID == 0 && localVert == 0) {
            ObjectLights ld = light[int(inst.lightDataIndex)];
            parityOut_.parityOut[0] = uint(ld.numLights.x);
        }
        // Addition 4 (Approach A): base_light diagnostic capture for typeId=84.
        // When u_parityBaseLightDebugMode==1 and this is the (inst=0, vert=0)
        // invocation, pack get_base_light()'s output (base_light, computed
        // BEFORE calc_light) as B|G<<8|R<<16|0xFF<<24 into parityOut_[0].
        // C++ reads slot 0 back and prints event=base_light_observed.
        // This runs AFTER Addition 3 so typeId=84 never triggers Addition 3.
        if (u_parityBaseLightDebugMode > 0 &&
            gl_InstanceID == 0 && localVert == 0) {
            uint bl_b = uint(clamp(base_light.b * 255.0, 0.0, 255.0));
            uint bl_g = uint(clamp(base_light.g * 255.0, 0.0, 255.0));
            uint bl_r = uint(clamp(base_light.r * 255.0, 0.0, 255.0));
            parityOut_.parityOut[0] = bl_b | (bl_g << 8u) | (bl_r << 16u) | (0xFFu << 24u);
        }
    }

    v_normal     = worldNormal;
    v_uv         = a_uv;
    v_flags      = inst.flags;
    v_highlight  = inst.aRGBHighlight;
    v_fog        = inst.fogRGB;
    v_localVertexID = a_localVertexID;
}
