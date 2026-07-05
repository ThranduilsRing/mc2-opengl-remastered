// shaders/static_prop.frag
// GPU static prop renderer — main fragment shader.
// NOTE: no #version directive here — makeProgram() prepends "#version 430\n".
// All formerly-uint uniforms are declared `int` because this project's
// shader_builder crashes on `uniform uint` (see memory/uniform_uint_crash.md).

#define PREC highp

#include <include/render_contract.hglsl>

#if defined(MC2_USE_VIEW_UNIFORMS)
// V-MATERIAL-PBR-3: per-fragment PBR specular consumes u_cameraWorldPos to
// build the view vector. Same compile-guard contract as static_prop.vert —
// the include is suppressed when MC2_VIEW_UNIFORMS=0 (process-lifetime gate
// at GLSL prefix injection in gos_static_prop_batcher.cpp).
#include <include/view_uniforms.hglsl>
#endif

// [RENDER_CONTRACT]
//   Pass:           StaticProp
//   Color0:         RGBA, opaque (alpha-test for ALPHA_TEST_BIT materials)
//   GBuffer1:       rc_gbuffer1_screenShadowEligible (production path)
//                   rc_gbuffer1_legacyDebugSentinelScreenShadowEligible (debug)
//   ShadowContract: castsStatic=true, castsDynamic=true,
//                   skipsPostScreenShadow=false (post-shadow applies)
//   StateContract:  depthTest=true, depthWrite=true, blend=Opaque,
//                   requiresMRT=true

in vec3  v_normal;
in vec2  v_uv;
flat in uint v_flags;
in vec4  v_highlight;
in vec4  v_fog;
in vec4  v_argb;
flat in uint v_localVertexID;
flat in uint v_drawID;          // plan v3.8 Step 8.3: forwarded from VS as
                                // uint(gl_DrawIDARB) under MC2_COALESCE,
                                // else 0u (vertex shader sets per branch).

// LIGHTING-DEBUG-VIEWS-1B-STATIC-PROPS: separated lighting channels (match
// static_prop.vert outputs in name/type/qualifier). Only read when
// u_lightingDebugView selects a channel; default path ignores them.
in       vec3 v_dbgSun;
in       vec3 v_dbgAmbient;
flat in  vec4 v_dbgLightMeta;   // x=lightDataIndex, y=numLights

#if defined(MC2_USE_VIEW_UNIFORMS)
// V-MATERIAL-PBR-3: per-fragment PBR varyings (must match static_prop.vert
// outputs in name, type, and flat qualifier). v_worldPos is interpolated
// (per-pixel view vector); v_pbrV1Sun* are flat (constant per ObjectLights
// entry, written by provoking vertex).
in       vec3 v_worldPos;
flat in  vec3 v_pbrV1SunDir;
flat in  vec3 v_pbrV1SunColor;
flat in  int  v_pbrV1SunFound;

// V-MATERIAL-PBR-3: PBR uniforms moved from static_prop.vert. Same
// upload sites (gos_static_prop_batcher.cpp glGetUniformLocation +
// glUniform*); glUniform is per-program, stage-agnostic, so no CPU edit
// is required. Default behavior preserved:
//   u_pbrV1Strength             = 0.0  (PBR block short-circuits)
//   u_pbrV1RoughnessOverride    = -1.0 (sentinel; use material/literal)
//   u_pbrV1DiagSunFound         = 0    (DIAG off)
// Runtime safety interlock (s_viewUniformsDisabled flag in the batcher)
// force-uploads strength=0 when MC2_VIEW_UNIFORMS=0, complementing this
// compile-guard.
uniform float u_pbrV1Strength;
uniform float u_pbrV1RoughnessOverride;
uniform int   u_pbrV1DiagSunFound;
#endif

#ifdef MC2_COALESCE
// Plan v3.8 Step 8.3 — coalesce-only: per-draw indirection via gl_DrawIDARB.
// `int u_drawIDBase` per uniform_uint_crash.md (uniform uint crashes
// shader_builder); cast to uint at index time.
uniform int u_drawIDBase;
struct PerDrawEntry {
    int   packetID;
    int   materialFlags;
    int   maxLocalVertexID;
    int   texArrayLayer;
    float uvScaleX;
    float uvScaleY;
    int   objectIdRaw;   // M1.5: handle.raw() (read into uint at use site)
    uint  materialIdx;   // MaterialGpu-3: index into MaterialTable.materials[]
                         // (was _pad1; uint matches uint32_t at std430 offset 28)
};
layout(std430, binding = 4) readonly buffer PerDrawData {
    PerDrawEntry entries[];
} perDraw_;
uniform sampler2DArray u_texArr;

// MaterialGpu-3: material table at binding 5.
// RENDER CONTRACT: static_prop.frag coalesce declares MaterialTable at binding 5
//   after MaterialGpu-3. Binding 5 may be unbound when u_materialGpuSample=0;
//   shader MUST NOT access materialTable_.materials[] in that case (enforced by the uniform branch below).
// Always declared in the coalesce variant so the reflection surface is stable.
// Instance name materialTable_ follows the usage pattern documented in material_gpu.hglsl.
#include <include/material_gpu.hglsl>
layout(std430, binding = 5) readonly buffer MaterialTable {
    MaterialGpu materials[];
} materialTable_;
uniform int u_materialGpuSample;  // 0 = legacy texArrayLayer, 1 = material table
#ifdef MC2_STATICPROP_PBR_SLOTS
uniform sampler2DArray u_ormTexArr;     // R=AO G=roughness B=metallic (linear)
uniform int            u_ormSampleEnable; // int (uniform-uint crash trap); 0 = no ORM
#endif

#else
uniform sampler2D u_tex;
uniform int       u_materialFlags;   // bit 0: ALPHA_TEST
uniform int       u_maxLocalVertexID;
uniform int       u_packetID;
#ifdef MC2_OBJECT_ID_BUFFER
// M1.5 non-coalesce path: handle bits uploaded as int (uniform-uint
// crash trap, memory/uniform_uint_crash.md), cast to uint in body.
uniform int       u_objectIdRaw;
#endif
#endif

uniform float u_fogValue;        // 1.0 = clear, 0.0 = fully fogged
uniform int   u_debugAddrMode;   // 0 normal, 1 gradient, 2 hash, 3 white, 4 argb-only, 5 tex-only, 6 highlight-only, 7 tex+highlight

// V-MATERIAL-DEBUG-1: per-fragment material debug view (StaticPropOpaque lane).
// Default 0 = OFF; CPU uploads 0 unless MC2_STATIC_PROP_DEBUG_MATERIAL is set.
// Modes:
//   0 off          (no visual change — guarded by `if (mode != 0) { ...; return; }`)
//   1 albedo       (raw sampled albedo, no lighting/fog/highlight)
//   2 materialIdx  (hashed-palette color from materialIdx)
//   3 normal       (worldNormal mapped to [0,1])
//   4 texArrayLayer(hashed-palette color from texArrayLayer)
//   5 roughness    (V-MATERIAL-PBR-1: MaterialGpu.roughnessFactor as grayscale)
//   6 metallic     (V-MATERIAL-PBR-1: MaterialGpu.metallicFactor  as grayscale)
// Legacy (non-MC2_COALESCE) lane has no materialIdx / texArrayLayer locals; in
// those branches materialIdx/texArrayLayer are 0 (visualizes as a single hue).
// Modes 5/6 likewise collapse to 0.0 (black) in the legacy lane since the
// MaterialGpu table is only bound under the coalesce path.
uniform int   u_debugMaterialMode;

// LIGHTING-DEBUG-VIEWS-1B-STATIC-PROPS: unified lighting debug channel.
// 0 = OFF (default; CPU uploads 0 unless MC2_LIGHTING_DEBUG_VIEW selects a
// channel). 40 albedo, 41 normal, 42 sun, 43 ambient/SH, 44 no-shadow marker,
// 45 final (falls through), 46 overbright, 47 light-count, 48 light-index.
uniform int   u_lightingDebugView;

uniform int   u_pathTint;  // MC2_SHADER_PATH_TINT: 1 = solid signature colour (debug); 0 = normal

// TERRAIN-DECAL-FILL-1: ambient/fill floor for the cliff-wall mesh-decal's
// shadow side. The static-prop vertex lighting is max(N·L,0) with no ambient
// floor, so a face pointing away from the sun goes to ~0 (black void). This
// uniform is applied as a per-fragment floor ONLY to fragments whose instance
// carries kFlagDecalFill (bit 3, set solely on MarbleCliff decal instances in
// bdactor.cpp). Default 0.0 upload + bit-unset = byte-identical for every other
// static prop AND (when unset) for the decal itself. CPU default is 0.20.
uniform float u_terrainDecalFill;
const uint kFlagDecalFill = (1u << 3);

// TERRAIN-DECAL-COLORBLEND-1: RVT-style terrain-color match for the cliff mesh
// decal. Blends the decal's sampled albedo toward the TERRAIN COLORMAP sampled
// at the decal's world-XY position, so the cliff mesh melts into its
// surroundings instead of reading as raw light-marble. Uses the SAME colormap
// texture + world->UV transform as terrain_lod_chunk.frag (u_colormap /
// u_atlasTopLeftX/Y / u_atlasOneOverWorldUnits): the batcher binds the merged
// colormap atlas to kColormapTexUnit and uploads the atlas params.
//   - Applied ONLY to fragments carrying kFlagDecalFill (bit 3).
//   - u_terrainDecalColorBlend == 0.0 (default upload when MC2_TERRAIN_DECAL_
//     COLORBLEND unset) OR flag-unset -> the whole block is skipped -> every
//     other static prop (and the decal itself) is byte-identical.
//   - Requires v_worldPos (MC2/terrain world frame, same as the chunk shader),
//     which only exists under MC2_USE_VIEW_UNIFORMS; guarded accordingly.
uniform sampler2D u_terrainColormap;    // merged colormap atlas (kColormapTexUnit)
uniform float     u_decalAtlasTLX;      // = Terrain::mapTopLeft3d.x
uniform float     u_decalAtlasTLY;      // = Terrain::mapTopLeft3d.y
uniform float     u_decalAtlasOOW;      // = Terrain::oneOverWorldUnitsMapSide
uniform float     u_terrainDecalColorBlend;  // 0 = off (byte-identical)

layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec4 GBuffer1;
#ifdef MC2_OBJECT_ID_BUFFER
// M1.5: per-pixel object handle. Emitted to GL_COLOR_ATTACHMENT2
// (R32UI). Spec section 5; struct field renamed in Task 7.
layout(location = 2) out uint v_objectId;
#endif

const int ALPHA_TEST_BIT = 1;

uint hash_u(uint x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// V-MATERIAL-DEBUG-1: hash-based deterministic palette for index visualization.
// Maps an integer key to a distinguishable RGB color in [0,1]. Cheap (one
// hash_u + 3 byte extracts). Used by debug modes 2 (materialIdx) and 4
// (texArrayLayer). Only invoked when u_debugMaterialMode != 0 so the default
// path pays nothing.
vec3 debug_palette(uint key) {
    uint h = hash_u(key * 2654435761u + 1u);
    return vec3(
        float((h >>  0) & 0xFFu) / 255.0,
        float((h >>  8) & 0xFFu) / 255.0,
        float((h >> 16) & 0xFFu) / 255.0);
}

void main() {
    // MC2_SHADER_PATH_TINT: solid MAGENTA so this shader's surfaces are unmistakable.
    if (u_pathTint != 0) {
        FragColor = vec4(1.0, 0.0, 1.0, 1.0);
        GBuffer1  = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible();
        return;
    }
    // Plan v3.8 Step 8.5 — resolve the three per-draw values to locals so
    // the rest of main() reads them identically in legacy and coalesce
    // modes. Under MC2_COALESCE these come from PerDrawData.entries[] at
    // (v_drawID + u_drawIDBase); under legacy from per-packet/per-type
    // uniforms (uploaded by flush() at :1567/:1685/:1687).
#ifdef MC2_COALESCE
    int materialFlags    = perDraw_.entries[v_drawID + uint(u_drawIDBase)].materialFlags;
    int packetID         = perDraw_.entries[v_drawID + uint(u_drawIDBase)].packetID;
    int maxLocalVertexID = perDraw_.entries[v_drawID + uint(u_drawIDBase)].maxLocalVertexID;
    int texArrayLayer    = perDraw_.entries[v_drawID + uint(u_drawIDBase)].texArrayLayer;
    // v3.8 mixed-size handling: the texture array is allocated at the
    // group's max(W,H). Each layer's actual texture occupies the
    // upper-left sub-region (0..uvScaleX, 0..uvScaleY). uvScale=1.0 →
    // texture fills layer (no remap). uvScale<1 → fract(v_uv) restores
    // GL_REPEAT semantics on the original texture's [0,1] domain, then
    // multiply by uvScale crops to the sub-region. Sampler is set to
    // GL_CLAMP_TO_EDGE so accidental over-shoot doesn't bleed into
    // adjacent (zero-padded) area at the edge.
    float uvScaleX = perDraw_.entries[v_drawID + uint(u_drawIDBase)].uvScaleX;
    float uvScaleY   = perDraw_.entries[v_drawID + uint(u_drawIDBase)].uvScaleY;
    uint  materialIdx = perDraw_.entries[v_drawID + uint(u_drawIDBase)].materialIdx;
    vec2  uvSampled = fract(v_uv) * vec2(uvScaleX, uvScaleY);
    // MaterialGpu sampling switch (u_materialGpuSample uniform).
    // Default path (u_materialGpuSample=0): effectiveLayer = texArrayLayer (legacy fallback).
    // Sample path (u_materialGpuSample=1): effectiveLayer = MaterialGpu[materialIdx].albedoTex.
    // Invariant: both paths produce the same layer while albedoTex == texArrayLayer holds.
    int effectiveLayer = texArrayLayer;
    if (u_materialGpuSample != 0) {
        uint albedo = materialTable_.materials[materialIdx].albedoTex;
        if (albedo != kMatTexAbsent) {
            // kMatTexAbsent = 0xFFFFFFFFu (defined in material_gpu.hglsl).
            // v2 mismatches=0 strongly predicts this guard is never triggered
            // for well-formed static-prop packets. Retained defensively.
            effectiveLayer = int(albedo);
        }
    }
    vec4 tex_color = texture(u_texArr, vec3(uvSampled, float(effectiveLayer)));
#else
    int materialFlags    = u_materialFlags;
    int packetID         = u_packetID;
    int maxLocalVertexID = u_maxLocalVertexID;
    vec4 tex_color = texture(u_tex, v_uv);
#endif

    // LODBUG probe (debug-mode 8) bypasses alpha-test discard so it reports
    // "draw call landed here" for materials whose texture coverage is empty
    // (e.g. an unregistered LOD-1 typeShape that took a CPU-fallback path
    // emitting no geometry would simply produce zero fragments — mode 8 lets
    // the user verify that *no* fragment is being emitted, vs. "drawn but
    // transparent"). All other debug modes (1..7) retain the discard so
    // existing tree-card / alpha-test diagnostics are unchanged.
    if (u_debugAddrMode != 8 && (materialFlags & ALPHA_TEST_BIT) != 0 && tex_color.a < 0.5) {
        discard;
    }

    // V-MATERIAL-DEBUG-1: per-fragment material debug view.
    // PIXEL-INVARIANT PROOF: when u_debugMaterialMode == 0 (default; CPU
    // uploads 0 unless MC2_STATIC_PROP_DEBUG_MATERIAL is set non-zero), this
    // entire block is skipped via the `!= 0` guard, control flow returns to
    // the existing pipeline at the next line, and the output is byte-identical
    // to pre-slice f005f7ce. Alpha-test discard above already ran exactly as
    // before, so discard behavior is unchanged.
    if (u_debugMaterialMode != 0) {
        vec3 dbg;
#ifdef MC2_COALESCE
        uint dbgMaterialIdx   = materialIdx;
        uint dbgTexArrayLayer = uint(texArrayLayer);
        // V-MATERIAL-PBR-1: read PBR scalars from MaterialGpu (binding=5).
        // Guarded by u_materialGpuSample to honor the contract that the buffer
        // may be unbound when sampling is disabled (see binding=5 comment above).
        float dbgRoughness = 0.0;
        float dbgMetallic  = 0.0;
        if (u_materialGpuSample != 0) {
            dbgRoughness = materialTable_.materials[materialIdx].roughnessFactor;
            dbgMetallic  = materialTable_.materials[materialIdx].metallicFactor;
        }
#if defined(MC2_STATICPROP_PBR_SLOTS) && defined(MC2_COALESCE)
        if (u_ormSampleEnable != 0) {
            uint ormTex = materialTable_.materials[materialIdx].metallicRoughnessTex;
            if (ormTex != kMatTexAbsent) {
                vec3 orm = texture(u_ormTexArr, vec3(uvSampled, float(ormTex))).rgb;
                dbgRoughness *= orm.g;
                dbgMetallic  *= orm.b;
            }
        }
#endif
#else
        // Legacy non-coalesce lane has no per-draw materialIdx / texArrayLayer;
        // collapse to 0 so debug modes 2/4 still produce a deterministic color
        // (single hue), which is enough to confirm the lane is exercised.
        uint dbgMaterialIdx   = 0u;
        uint dbgTexArrayLayer = 0u;
        // V-MATERIAL-PBR-1: legacy lane has no MaterialGpu binding; report 0.0.
        float dbgRoughness = 0.0;
        float dbgMetallic  = 0.0;
#endif
        if      (u_debugMaterialMode == 1) dbg = tex_color.rgb;
        else if (u_debugMaterialMode == 2) dbg = debug_palette(dbgMaterialIdx);
        else if (u_debugMaterialMode == 3) dbg = normalize(v_normal) * 0.5 + 0.5;
        else if (u_debugMaterialMode == 4) dbg = debug_palette(dbgTexArrayLayer);
        else if (u_debugMaterialMode == 5) dbg = vec3(dbgRoughness);  // V-MATERIAL-PBR-1
        else if (u_debugMaterialMode == 6) dbg = vec3(dbgMetallic);   // V-MATERIAL-PBR-1
        else                               dbg = vec3(1.0, 0.0, 1.0);  // unknown mode -> hot pink
        FragColor = vec4(dbg, 1.0);
        GBuffer1  = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible();
#ifdef MC2_OBJECT_ID_BUFFER
        // Debug views also emit object-id (matches mode 8 / other debug modes
        // implicit behavior: object picking still works in debug views).
#ifdef MC2_COALESCE
        v_objectId = uint(perDraw_.entries[v_drawID + uint(u_drawIDBase)].objectIdRaw);
#else
        v_objectId = uint(u_objectIdRaw);
#endif
#endif
        return;
    }

    if (u_debugAddrMode == 1) {
        float t = float(v_localVertexID) / max(float(maxLocalVertexID), 1.0);
        FragColor = vec4(t, t, t, 1.0);
        GBuffer1  = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible();
        return;
    }
    if (u_debugAddrMode == 2) {
        uint h = hash_u(uint(packetID) * 2654435761u + v_localVertexID);
        FragColor = vec4(
            float((h >>  0) & 0xFFu) / 255.0,
            float((h >>  8) & 0xFFu) / 255.0,
            float((h >> 16) & 0xFFu) / 255.0,
            1.0);
        GBuffer1  = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible();
        return;
    }
    // Bisection modes (RAlt+9 cycles 0..7).
    if (u_debugAddrMode == 3) { FragColor = vec4(1.0); GBuffer1 = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible(); return; }
    if (u_debugAddrMode == 4) { FragColor = vec4(v_argb.rgb,    1.0); GBuffer1 = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible(); return; }
    if (u_debugAddrMode == 5) { FragColor = vec4(tex_color.rgb, 1.0); GBuffer1 = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible(); return; }
    if (u_debugAddrMode == 6) { FragColor = vec4(v_highlight.rgb * v_highlight.a, 1.0); GBuffer1 = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible(); return; }
    if (u_debugAddrMode == 7) {
        vec3 rgb = tex_color.rgb + v_highlight.rgb * v_highlight.a;
        FragColor = vec4(rgb, 1.0);
        GBuffer1 = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible();
        return;
    }
    // LODBUG probe (mode 8): solid magenta, alpha-test bypassed above. Every
    // fragment emitted by any draw call lands here as bright pink so the user
    // can answer "did this building get drawn at all post LOD swap?" without
    // ambiguity from texture sampling, lighting, fog, or highlight math.
    if (u_debugAddrMode == 8) {
        FragColor = vec4(1.0, 0.0, 1.0, 1.0);
        GBuffer1  = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible();
        return;
    }

    vec3 litRgb = v_argb.rgb;
    if ((materialFlags & ALPHA_TEST_BIT) != 0) {
        // Tree cards/leaves read too black on the light-facing falloff side.
        // Keep their lighting variation, but cap the darkest side at ~50%.
        litRgb = max(litRgb, vec3(0.5));
    }
    // TERRAIN-DECAL-FILL-1 (v2): raise the cliff-decal's shadow-side brightness.
    // NOTE: the old max(litRgb, fill) was a visual no-op -- the baked per-vertex
    // light v_argb.rgb is scaled to a bright band and never drops below the
    // slider's operative range on visible faces, so max() returned litRgb every
    // time. Fixed below (post-texture): an ADDITIVE ambient weighted by how
    // UNlit the fragment is, applied to the final color so the slider always
    // moves shadow-side pixels while the lit side (shadowW~0) is untouched.
    // Applied after the tex_color multiply -- see the c.rgb block below.

    // TERRAIN-DECAL-COLORBLEND-1: mix the cliff-decal albedo toward the terrain
    // colormap sampled at this fragment's world-XY (same atlas-UV reconstruction
    // as terrain_lod_chunk.frag). Flag-gated + blend>0 gated -> non-decal props
    // and the blend==0 default are byte-identical. v_worldPos is the MC2/terrain
    // world frame (static_prop.vert world_mc2 = (-x,z,y)), identical to the
    // frame the colormap UV transform expects.
#if defined(MC2_USE_VIEW_UNIFORMS)
    if ((v_flags & kFlagDecalFill) != 0u && u_terrainDecalColorBlend > 0.0) {
        vec2 cmUV;
        cmUV.x = (v_worldPos.x - u_decalAtlasTLX) * u_decalAtlasOOW;
        cmUV.y = (u_decalAtlasTLY - v_worldPos.y) * u_decalAtlasOOW;
        vec3 cmColor = texture(u_terrainColormap, cmUV).rgb;
        tex_color.rgb = mix(tex_color.rgb, cmColor, clamp(u_terrainDecalColorBlend, 0.0, 1.0));
    }
#endif

    vec4 c = tex_color * vec4(litRgb, v_argb.a);
    // TERRAIN-DECAL-FILL-1 (v2): additive shadow-side ambient (see note above).
    // shadowW ~ 1 where the baked light is dark, ~0 where bright -> only the
    // shadow side lifts. Scaled by tex_color so the fill stays rock-colored.
    // Flag-gated + fill>0 gated -> non-decal props / default stay byte-identical.
    if ((v_flags & kFlagDecalFill) != 0u && u_terrainDecalFill > 0.0) {
        float litMax = max(litRgb.r, max(litRgb.g, litRgb.b));
        float shadowW = 1.0 - clamp(litMax, 0.0, 1.0);
        c.rgb += u_terrainDecalFill * tex_color.rgb * shadowW;
    }
    c.rgb += v_highlight.rgb * v_highlight.a;

#if defined(MC2_USE_VIEW_UNIFORMS)
    // V-MATERIAL-PBR-3: per-fragment Schlick-Fresnel + power-lobe specular.
    // Math identical to former vert-side block; differences vs PBR-2:
    //   1. runs per-fragment so pow(NdotH, P) yields a tight localized
    //      highlight instead of Gouraud-averaged uniform brightening;
    //   2. albedo-tinted F0 for metallics uses real per-fragment tex_color
    //      (was constant kF0Dielectric in PBR-2 since albedo wasn't
    //      available at the vertex stage);
    //   3. roughness/metallic source from MaterialGpu (binding=5) when
    //      u_materialGpuSample!=0 — same access pattern as debug modes 5/6
    //      from V-MATERIAL-PBR-1, guarded so the buffer may be unbound.
    //
    // Default-OFF byte-identity: u_pbrV1Strength=0.0 short-circuits the
    // whole block (matches PBR-2 OFF behavior). Magenta DIAG branch
    // requires `u_pbrV1Strength > 0.0 && DIAG != 0`, so DIAG OFF state is
    // also unchanged.
    //
    // Added BEFORE fog mix so specular fogs out with the rest of the
    // surface; AFTER highlight add so the specular contribution doesn't
    // get clobbered by highlight clamp semantics (highlight already
    // applied straight, no further compensation needed).
    const uint kFlagIsWindow = (1u << 1);
    if (u_pbrV1Strength > 0.0 && (v_flags & kFlagIsWindow) == 0u) {
        if (v_pbrV1SunFound != 0) {
            if (u_pbrV1DiagSunFound != 0) {
                // DIAG cyan: sun found at this fragment's provoking vertex.
                c.rgb = vec3(0.0, 1.0, 1.0);
            } else {
                // Material scalars: prefer MaterialGpu when sampling is on;
                // fall back to the literal defaults otherwise (metallic=0,
                // roughness=1.0) so gate-ON behavior degrades gracefully when
                // binding=5 is unbound.
                // MATERIAL-M0: roughness default PINNED to 1.0 to match the
                // authoritative producer (MaterialGpu record default,
                // gos_static_prop_batcher.cpp m.roughnessFactor) and the asset
                // viewer fallback (LocalPbrMaterialBackend.cpp / MaterialRenderBackend.h).
                // All three sites must agree. See docs/material-m0-contract.md.
                float metallic  = 0.0;
                float roughness = 1.0;
#ifdef MC2_COALESCE
                if (u_materialGpuSample != 0) {
                    metallic  = materialTable_.materials[materialIdx].metallicFactor;
                    roughness = materialTable_.materials[materialIdx].roughnessFactor;
                }
#endif
#if defined(MC2_STATICPROP_PBR_SLOTS) && defined(MC2_COALESCE)
                if (u_ormSampleEnable != 0) {
                    uint ormTex = materialTable_.materials[materialIdx].metallicRoughnessTex;
                    if (ormTex != kMatTexAbsent) {
                        vec3 orm = texture(u_ormTexArr, vec3(uvSampled, float(ormTex))).rgb;
                        roughness *= orm.g;
                        metallic  *= orm.b;
                    }
                }
#endif
                // Runtime override (ImGui slider) — sentinel < 0 means use
                // material/literal; >= 0 replaces roughness post-material.
                if (u_pbrV1RoughnessOverride >= 0.0) {
                    roughness = u_pbrV1RoughnessOverride;
                }

                // STATIC-PROP-PBR-SPACE-FIX-1: convert N and L to GL world
                // space before computing view-dependent specular terms.
                // v_normal and v_pbrV1SunDir are Stuff world space (from
                // a_normal*mat3(shapeToWorld) and GatherLightsParameters
                // lightToWorld.GetLocalForwardInWorld respectively).
                // v_worldPos and u_cameraWorldPos are GL world space (the
                // Stuff->GL swap x'=-x,y'=z,z'=y is applied to positions
                // in the vert shader but NOT to normals/lightDir).
                // Without this fix H = normalize(L_stuff+V_eye_gl) crosses
                // spaces; NdotH and Fresnel are geometrically wrong.
                vec3 N_stuff = normalize(v_normal);
                vec3 N      = vec3(-N_stuff.x, N_stuff.z, N_stuff.y);  // Stuff->GL
                vec3 V_eye  = normalize(u_cameraWorldPos.xyz - v_worldPos);  // GL
                vec3 L_stuff = normalize(-v_pbrV1SunDir);  // surface->sun, Stuff space
                vec3 L      = vec3(-L_stuff.x, L_stuff.z, L_stuff.y);  // Stuff->GL
                vec3 H      = normalize(L + V_eye);

                // Albedo-tinted F0 for metallics — real per-fragment albedo
                // (deferred from PBR-2 because vertex stage had no access).
                vec3 albedoRgb = tex_color.rgb;
                const vec3 kF0Dielectric = vec3(0.04);
                vec3 F0 = mix(kF0Dielectric, albedoRgb, metallic);
                vec3 F  = F0 + (vec3(1.0) - F0)
                             * pow(max(0.0, 1.0 - dot(N, V_eye)), 5.0);

                float smoothness = 1.0 - roughness;
                float specPower  = mix(1.0, 512.0, smoothness * smoothness);
                float NdotH      = max(0.0, dot(N, H));
                float NdotL      = max(0.0, dot(N, L));
                vec3  specular   = v_pbrV1SunColor * F * pow(NdotH, specPower) * NdotL;

                c.rgb += specular * u_pbrV1Strength;
            }
        } else if (u_pbrV1DiagSunFound != 0) {
            // DIAG magenta: PBR gate ON, DIAG ON, but no INFINITE light
            // found in this fragment's ObjectLights entry.
            c.rgb = vec3(1.0, 0.0, 1.0);
        }
    }
#endif  // MC2_USE_VIEW_UNIFORMS

    // LIGHTING-DEBUG-VIEWS-1B-STATIC-PROPS: unified lighting debug channels.
    // u_lightingDebugView==0 -> skipped (pixel-invariant default). Static props
    // do NOT self-shadow in this shader (the post screen-shadow pass applies
    // building shadowing), so channel 44 is an explicit NO-SHADOW marker rather
    // than a faked shadow factor. Channel 45 (final) and unknown ids fall
    // through to the normal lit render below.
    if (u_lightingDebugView != 0) {
        vec3 dbg;
        bool handled = true;
        if      (u_lightingDebugView == 40) dbg = tex_color.rgb;                    // albedo
        else if (u_lightingDebugView == 41) dbg = normalize(v_normal) * 0.5 + 0.5;  // normal
        else if (u_lightingDebugView == 42) dbg = v_dbgSun;                         // sun/direct
        else if (u_lightingDebugView == 43) dbg = v_dbgAmbient;                     // ambient/SH/fill
        else if (u_lightingDebugView == 44) dbg = vec3(0.15, 0.15, 0.35);           // no-shadow marker
        else if (u_lightingDebugView == 46) {                                       // over/under-bright heatmap
            float luma = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));
            if      (luma > 1.0)  dbg = vec3(1.0, clamp(2.0 - luma, 0.0, 1.0) * 0.4, 0.0);
            else if (luma < 0.05) dbg = vec3(0.0, 0.0, 1.0);
            else                  dbg = vec3(luma);
        }
        else if (u_lightingDebugView == 47) {                                       // dynamic light-count heat
            float t = clamp(v_dbgLightMeta.y / 16.0, 0.0, 1.0);
            dbg = vec3(t, 1.0 - t, 0.0);
        }
        else if (u_lightingDebugView == 48) dbg = debug_palette(uint(v_dbgLightMeta.x)); // baked light index
        else handled = false;                                                       // 45/unknown -> fall through
        if (handled) {
            FragColor = vec4(dbg, 1.0);
            GBuffer1  = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible();
#ifdef MC2_OBJECT_ID_BUFFER
#ifdef MC2_COALESCE
            v_objectId = uint(perDraw_.entries[v_drawID + uint(u_drawIDBase)].objectIdRaw);
#else
            v_objectId = uint(u_objectIdRaw);
#endif
#endif
            return;
        }
    }

    c.rgb = mix(v_fog.rgb, c.rgb, u_fogValue);

    FragColor = c;
    // Static props receive the static shadow map but NOT the dynamic-cascade
    // self-shadow (CPU never self-shadowed buildings; hard terminator on flat
    // facets looks wrong). a=0.25 routes to the no-self-shadow object path.
    // PROP-SHADOW-RECEIVE-1: MC2_PROP_SHADOW_RECEIVE=1 opts the a=0.25 class
    // back INTO the dynamic CSM sample inside shadow_screen.frag (softened,
    // floor 0.4) — the mask class here is unchanged either way.
    GBuffer1  = rc_gbuffer1_screenShadowEligible_noSelfShadow(normalize(v_normal));
#ifdef MC2_OBJECT_ID_BUFFER
    // M1.5: emit handle.raw() to attachment-2. Alpha-tested fragments
    // that discard() above skip this write naturally. Coalesce path
    // sources from PerDrawData.entries[]; legacy path from u_objectIdRaw.
#ifdef MC2_COALESCE
    v_objectId = uint(perDraw_.entries[v_drawID + uint(u_drawIDBase)].objectIdRaw);
#else
    v_objectId = uint(u_objectIdRaw);
#endif
#endif
}
