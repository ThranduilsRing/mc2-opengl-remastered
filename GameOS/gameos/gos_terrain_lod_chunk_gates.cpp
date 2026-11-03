// gos_terrain_lod_chunk: eager tuning caches (kTglc*) the draw path and the light-pool uploads read, and the GlStateGuard kill-switch.
// Declared in the class headers; shared names in gos_terrain_lod_chunk_internal.h.
#include "gos_terrain_lod_chunk_internal.h"

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

extern const int kTglcDiag = mc2::gate::get<"MC2_TERRAIN_LOD_CHUNK_DIAG">();
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

TerrainShorelineKnobs gos_TerrainShorelineKnobs() {
    TerrainShorelineKnobs k;
    k.on = gos_TerrainShorelineGateOn();
    // The water plane the bands are placed against: the same source as the water fast path (Terrain::waterElevation,
    // mirrored into gosPostProcess at mission load).
    if (gosPostProcess* pp = getGosPostProcess()) k.waterElevation = pp->getWaterElevation();
    k.timeSeconds  = gos_GetShaderClockSeconds();
    k.strength     = mc2::gate::get<"MC2_TERRAIN_SHORELINE_STRENGTH">();
    k.foamStrength = mc2::gate::get<"MC2_TERRAIN_SHORELINE_FOAM">();
    // Band widths are HORIZONTAL runs from the waterline (wet 16 wu, foam 5 wu by default); the legacy _HEIGHT names
    // are aliases read only while the _RUN name is unset.
    k.wetRun  = mc2::gate::isSet<"MC2_TERRAIN_SHORELINE_WET_RUN">()  ? mc2::gate::get<"MC2_TERRAIN_SHORELINE_WET_RUN">()
                                                                     : mc2::gate::get<"MC2_TERRAIN_SHORELINE_WET_HEIGHT">();
    k.foamRun = mc2::gate::isSet<"MC2_TERRAIN_SHORELINE_FOAM_RUN">() ? mc2::gate::get<"MC2_TERRAIN_SHORELINE_FOAM_RUN">()
                                                                     : mc2::gate::get<"MC2_TERRAIN_SHORELINE_FOAM_HEIGHT">();
    // Static world-XY jitter on the run so the lobes stop tracing the mesh waterline's straight segments; 0 = exact contour.
    k.edgeJitter = mc2::gate::get<"MC2_TERRAIN_SHORELINE_EDGE_JITTER">();
    return k;
}
