// gos_terrain_lod_chunk: eager tuning caches (kTglc*) the draw path and the light-pool uploads read, and the GlStateGuard kill-switch.
// Declared in the class headers; shared names in gos_terrain_lod_chunk_internal.h.
#include "gos_terrain_lod_chunk_internal.h"


// GlStateGuard slice 2 kill-switch. Default-ON: when set, the terrain chunk
// draw owns its depth/blend/cull/mask/func via RAII guards (mc2gl::GlScoped*).
// MC2_GLSTATEGUARD_TERRAIN=0 reverts to the legacy hand-rolled save/restore
// (kept verbatim, nothing deleted) — the A/B used to prove the guards are
// pixel-neutral. Sampled once at process start.
bool glStateGuardTerrainEnabled() {
    static const bool on = mc2::gate::get<"MC2_GLSTATEGUARD_TERRAIN">();
    return on;
}

// REDUNDANT-PASS-HUNT-1 (re-compute class): the terrain-chunk per-frame uniform
// setup in gos_TerrainLodChunk_SubmitDrawCommands used to call getenv() ~11 times
// EVERY frame to resolve mission-constant debug/tuning gates (FORCE_COLOR, DIAG,
// CEMENT_DIAG_CONNECT, SLOPE_BIAS[_STRENGTH], CLIFF_TRIPLANAR[_STRENGTH],
// MACRO_VARIATION[_STRENGTH], EDGE_FEATHER[_STRENGTH]). getenv() on Windows takes
// a CRT lock and linear-scans the process environment block; none of these vars
// can change mid-process, so resolving them per frame is pure redundant recompute.
// These resolvers cache the SAME values once at first use — byte-identical result,
// matching the established `static const ... = [](){...}()` pattern already used
// throughout this file. The per-frame block reads these
// instead of hitting getenv. Uniform VALUES uploaded are unchanged.
// Resolved-once cached mirrors of the per-frame terrain-uniform env gates.
extern const bool kTglcForceColor = mc2::gate::get<"MC2_TERRAIN_LOD_CHUNK_FORCE_COLOR">();
// SHORELINE-GLOW-1: the two shoreline accessors are deliberately NOT in this
// anonymous namespace -- mclib/terrain.cpp calls gos_TerrainShorelineGateOn()
// too, so it needs external linkage. Defined at global scope further down,
// next to gos_TerrainLodChunk_IsShorelineMaskActive(); declared in the header.
extern const int kTglcDiag = mc2::gate::get<"MC2_TERRAIN_LOD_CHUNK_DIAG">();
extern const bool  kTglcCementDiagConnect = mc2::gate::get<"MC2_TERRAIN_CEMENT_DIAG_CONNECT">();
// CEMENT-ATLAS-SEAM-1: default ON (killswitch MC2_CEMENT_ATLAS_SEAM_FIX=0).
extern const bool kTglcCementAtlasSeamFix = mc2::gate::get<"MC2_CEMENT_ATLAS_SEAM_FIX">();
// NIGHT-LIGHT-POOLS-2: soft falloff tail (default ON; =0 exact legacy
// GetFalloff linear ramp) + mover-light dim factor (default 0.5; 1.0 = off).
extern const bool kTglcLightPoolSoft = mc2::gate::get<"MC2_LIGHT_POOL_SOFT">();
// NIGHT-LIGHT-POOLS-4: default dropped 0.5 -> 0.25 (5-mech group still read as
// one saturated egg at 0.5; compression can't help five overlapping full discs).
extern const float kTglcLightPoolMoverDim = mc2::gate::get<"MC2_LIGHT_POOL_MOVER_DIM">();
// NIGHT-LIGHT-POOLS-4: mover pool radius scale (shrinks close/far distances of
// mover-attached lights only; 1.0 = legacy radii = killswitch).
extern const float kTglcLightPoolMoverRadius = mc2::gate::get<"MC2_LIGHT_POOL_MOVER_RADIUS">();
// NIGHT-LIGHT-POOLS-4: mover pools combine by per-channel MAX (no additive
// stacking across an overlapping lance); =0 restores the additive sum.
extern const bool kTglcLightPoolMoverMax = mc2::gate::get<"MC2_LIGHT_POOL_MOVER_MAXBLEND">();
// NIGHT-LIGHT-POOLS-3: tone-compress the summed pool term (default ON;
// =0 restores the legacy min(pool,1) hard clamp -> stacked-pool blowout).
extern const bool kTglcLightPoolCompress = mc2::gate::get<"MC2_LIGHT_POOL_COMPRESS">();
// MOVER-CONTACT-SHADOW-1 (MASTER-FAIL #7): strength in [0,1] of the under-mech
// contact shadow that carves a mover's own bright light-pool center + darkens
// the ground beneath the body. Default 0.0 => uniform 0 => byte-identical
// (mix() collapses, no darkening). Recommended ~0.7. MC2_MOVER_CONTACT_SHADOW.
extern const float kTglcMoverContactShadow = mc2::gate::get<"MC2_MOVER_CONTACT_SHADOW">();
extern const bool  kTglcSlopeBias         = mc2::gate::get<"MC2_TERRAIN_SLOPE_BIAS">();
extern const float kTglcSlopeBiasStr      = mc2::gate::get<"MC2_TERRAIN_SLOPE_BIAS_STRENGTH">();   // row: default 1, > 0
extern const bool  kTglcCliffTriplanar    = mc2::gate::get<"MC2_TERRAIN_CLIFF_TRIPLANAR">();
extern const float kTglcCliffTriplanarStr = mc2::gate::get<"MC2_TERRAIN_CLIFF_TRIPLANAR_STRENGTH">();
// TERRAIN-CLIFF-HEIGHT-NORMAL-1: derive the cliff SHADING normal from the height
// gradient of the cooked cliff displacement (mat5 layer-5 alpha) so the rich rock
// relief actually catches light (the smooth marble rgb normal left it flat).
// Trace knob MC2_TERRAIN_CLIFF_HEIGHT_NORMAL_STRENGTH, default 2.0; only consumed
// inside the useTriplanarCliff frag block. Values <= 0 are rejected (the row is > 0) and use the default.
extern const float kTglcCliffHeightNormalStr = mc2::gate::get<"MC2_TERRAIN_CLIFF_HEIGHT_NORMAL_STRENGTH">();
// TERRAIN-CLIFF-POM-1: triplanar Parallax Occlusion Mapping on cliff faces.
// Gate MC2_TERRAIN_CLIFF_POM (default OFF -> u_cliffPom.x=0 -> frag march skipped,
// the triplanar block behaves exactly as TRIPLANAR-1). Depth (world-unit height
// scale, default 12) via MC2_TERRAIN_CLIFF_POM_DEPTH; max march steps (clamp
// 8..32, default 24) via MC2_TERRAIN_CLIFF_POM_STEPS.
extern const bool  kTglcCliffPom          = mc2::gate::get<"MC2_TERRAIN_CLIFF_POM">();
extern const float kTglcCliffPomDepth     = mc2::gate::get<"MC2_TERRAIN_CLIFF_POM_DEPTH">();
extern const float kTglcCliffPomSteps     = []() {
    float s = mc2::gate::get<"MC2_TERRAIN_CLIFF_POM_STEPS">();
    if (s < 8.0f) s = 8.0f; if (s > 32.0f) s = 32.0f;
    return s;
}();
// TERRAIN-CLIFF-DEBUG: bounded debug-viz gate (default 0/off -> u_cliffDebug=0 ->
// frag block skipped -> byte-identical). 1..4 select the debug output (see frag).
extern const int kTglcCliffDebug = mc2::gate::get<"MC2_TERRAIN_CLIFF_DEBUG">();
extern const float kTglcMacroVariation    = []() {
    if (!mc2::gate::get<"MC2_TERRAIN_MACRO_VARIATION">()) return 0.0f;
    return mc2::gate::get<"MC2_TERRAIN_MACRO_VARIATION_STRENGTH">();
}();
// WATER-FAR-BAND-ATTRIB-1 (batch3 item C): default flipped ON to match
// WATER-EDGE-FEATHER (54a70e2d). With water feathered ON and terrain feather
// OFF, the water dissolves at the map edge but EXPOSES the un-feathered dark
// submerged-terrain rim -> the residual hard navy far-band line at grazing
// angles (mc2_01 saved_205644; MC2_WATER_DEBUG_MODE=2 shows water alpha
// already 0 across the band -> the navy pixels are terrain, not water).
// Killswitch: MC2_TERRAIN_EDGE_FEATHER=0.
extern const bool kTglcEdgeFeather = mc2::gate::get<"MC2_TERRAIN_EDGE_FEATHER">();
extern const float kTglcEdgeFeatherStr    = mc2::gate::get<"MC2_TERRAIN_EDGE_FEATHER_STRENGTH">();
// TERRAIN-MATERIAL-TEXTURES-1: per-layer PBR albedo array gate (default OFF ->
// u_useMatAlbedo uploads 0 -> frag composition verbatim -> byte-identical).
extern const bool  kTglcMatAlbedo         = mc2::gate::get<"MC2_TERRAIN_MATERIAL_TEXTURES">();
