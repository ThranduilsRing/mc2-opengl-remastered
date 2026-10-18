// GameOS/gameos/gos_terrain_water_stream.h
//
// Water-stream parallel to gos_terrain_patch_stream — but for the renderWater
// architectural slice (CPU→GPU offload), where the population is the small
// subset of terrain quads with at least one vertex below `Terrain::waterElevation`
// (the alpha-band trigger). Mission-static; no live-game write paths exist
// (verified 2026-04-30 — see specs/2026-04-29-renderwater-fastpath-design.md
// "Verified mutation invariant").
//
// Stage 1 (this header): CPU-side recipe + load-time population. No GL bindings,
// no shader, no draw. Pure data structure that future stages upload to a GPU SSBO.
//
// Schema is intentionally NOT shared with TerrainQuadRecord — water and solid
// terrain populations are disjoint (water quads frequently have terrainHandle==0;
// terrain quads frequently have no vertices below waterElevation), and the
// shaders read different fields. Co-indexing forces padding-pad in both schemas.

#pragma once

#include <cstdint>
#include <cstddef>


namespace WaterStream {

// One record per water-bearing quad in a mission. Built once on the first
// Terrain::update after mapData->makeLists populates quadList; persists for
// mission lifetime.
//
// std430-aligned. 64 bytes/record. mc2_01 measured at 7,995 records = 512 KB.
//
// Schema completeness audit (Stage 1 refinement, 2026-04-30): every per-vertex
// field that drawWater() reads from `vertices[i]` or `vertices[i]->pVertex`
// AND is mission-stable lives in this record. Per-frame fields (lightRGB,
// fogRGB, projected wx/wy/wz/ww) live in the per-frame thin SSBO (Stage 2).
//
//   drawWater() reads:                        | source                           | in recipe?
//   ------------------------------------------|----------------------------------|-----------
//   vertices[i]->vx, vy                        | mission-stable (PostcompVertex)  | YES (v*x/v*y)
//   vertices[i]->pVertex->elevation            | mission-stable (PostcompVertex)  | YES (v*e)
//   vertices[i]->pVertex->terrainType          | mission-stable (PostcompVertex)  | YES (terrainTypes)
//   vertices[i]->pVertex->water (bits 0/6/7)   | mission-stable (calcWater)       | YES (waterBits)
//   vertices[i]->pVertex->selected             | runtime debug, ignored           | NO
//   vertices[i]->wx, wy, wz, ww                | per-frame projected              | shader recomputes
//   vertices[i]->lightRGB, fogRGB              | per-frame                        | per-frame SSBO (Stage 2)
//   q.uvMode                                   | mission-stable                   | YES (flags bit 0)
//   q.waterHandle, waterDetailHandle           | per-frame texture-slot animation | global uniform (Stage 2)
struct alignas(16) WaterRecipe {
    // Per-vertex raw world XY (matches vertices[i]->vx, ->vy). 32 B.
    float v0x, v0y;
    float v1x, v1y;
    float v2x, v2y;
    float v3x, v3y;
    // Per-vertex elevation (used for alpha-band classification in VS). 16 B.
    float v0e, v1e, v2e, v3e;
    // Quad index inside Terrain::quadList (debug + parity correlation). 4 B.
    uint32_t quadIdx;
    // bit 0: uvMode (0 = BOTTOMRIGHT, 1 = BOTTOMLEFT)
    // bit 1: hasDetail (terrainTextures2 has a valid water-detail texture pool)
    uint32_t flags;
    // Per-vertex terrainType (uchar each, packed v0..v3 into low to high byte).
    // Fed through terrainTypeToMaterial() in the VS to populate fogRGB low byte
    // — preserves legacy `(frgb & 0xFFFFFF00) | terrainTypeToMaterial(...)` arithmetic.
    uint32_t terrainTypes;
    // Per-vertex water bits (uchar each, packed v0..v3). bit 0 set → underwater
    // (this quad's classification reason); bits 6/7 modulate the per-vertex wave
    // displacement applied in setupTextures' water-projection block at
    // quad.cpp:689-700: when bit 7 set → -frameCosAlpha; bit 6 set → +frameCosAlpha;
    // neither → 0. The fast-path VS reproduces that displacement to keep wave-bob.
    uint32_t waterBits;
};
static_assert(sizeof(WaterRecipe) == 64,
              "WaterRecipe must be 64 bytes for std430 alignment");

constexpr uint32_t kFlagBitUvModeBottomLeft = 0x1u;
constexpr uint32_t kFlagBitHasDetail        = 0x2u;

// Per-frame thin record (mirrors M1d/M1g terrain thin-record pattern).
// Built each frame from the current quadList window. Each entry points to
// a stable WaterRecipe via recipeIdx and carries the per-frame mutable state.
//
// 48 B/record. flags layout (mirrors gos_terrain_thin.vert flags):
//   bit 0: pzTri1Valid (corners 0,1,2 — or 0,1,3 for BOTTOMLEFT — all pass `wz ∈ [0,1)`)
//   bit 1: pzTri2Valid (corners 0,2,3 — or 1,2,3 for BOTTOMLEFT — all pass `wz ∈ [0,1)`)
//
// The per-triangle pz gate is THE LOAD-BEARING CPU PRE-CULL per
// memory:terrain_tes_projection.md: "There is no homogeneous clip-space test
// the GPU clipper can do to distinguish 'valid visible vert' from
// 'behind-camera vert that should be rejected.'" wz comes from the per-frame
// water projection in setupTextures (quad.cpp:715-722) — exactly the same
// post-perspective-divide z the legacy water emit gate at quad.cpp:2812 uses.
struct alignas(16) WaterThinRecord {
    uint32_t recipeIdx;     // index into WaterRecipe array (stable, map-keyed)
    uint32_t flags;         // bit 0 = pzTri1Valid, bit 1 = pzTri2Valid
    uint32_t _pad0, _pad1;
    uint32_t lightRGB0, lightRGB1, lightRGB2, lightRGB3; // ARGB per corner
    uint32_t fogRGB0,   fogRGB1,   fogRGB2,   fogRGB3;   // fogRGB.w = FogValue
};
static_assert(sizeof(WaterThinRecord) == 48,
              "WaterThinRecord must be 48 bytes for std430 alignment");

constexpr uint32_t kWaterThinFlagPzTri1Valid = 0x1u;
constexpr uint32_t kWaterThinFlagPzTri2Valid = 0x2u;

// --- Narrow-walk fast path -------------------------------------------------
//
// Default-on (env-gate `MC2_WATER_UPLOAD_NARROW=0` to opt out and fall back
// to the legacy full-quadList walk). The narrow walk lets the caller append
// candidates during the same `setupTextures()` loop the engine already runs,
// reducing UploadAndBindThinRecords' iteration count from `numberQuads`
// (camera window, ~14K-40K) to only the water-bearing subset (typically
// hundreds). Eligibility test must match UploadThin's exactly.
//
// Contract:
//   1. BeginFrameNarrow() at the top of the per-frame setupTextures loop —
//      clears the candidate vector, reserves prior-frame max + 10% slack.
//   2. AppendNarrowCandidate(&q) immediately after `q.setupTextures()` for
//      every quad whose corners + waterHandle pass the same predicate
//      UploadAndBindThinRecords applies. O(1), no allocation in the hot
//      path after the BeginFrameNarrow reserve.
//   3. UploadAndBindThinRecords reads the candidate vector if narrow mode
//      is on; otherwise falls back to the full quadList walk.
bool NarrowEnabled();
void BeginFrameNarrow();
struct NarrowCandidate {
    const void* quadPtr; // TerrainQuadPtr — opaque to keep the header lean
};
void AppendNarrowCandidate(const void* quadPtr);

// WATER-GPU-FULL-RECIPE-CULL-1B: true when authoritative full-recipe GPU cull is
// enabled (MC2_WATER_GPU_FULL_RECIPE_AUTHORITATIVE, default-OFF). When true the CPU
// narrow walk is skipped and the GPU culls the whole world-indexed recipe set
// directly into the draw-feeding thin buffer.
bool IsFullRecipeAuthoritative();

// Build the recipe array from the live Terrain::quadList. Walks all
// numberQuads entries; flags water-bearing quads (≥1 vertex below
// waterElevation). Idempotent — calling twice rebuilds.
//
// MC2_WATER_STREAM_DEBUG=1 prints `[WATER_STREAM v1] event=build_done
// recipes=N candidates_walked=M waterElevation=F has_terrainTextures2=B`
// at completion.
void Build();

// Reset to empty state (mission unload).
void Reset();

// Read-only access to the recipe array. Pointer is stable across the
// mission lifetime (vector capacity is fixed after Build).
const WaterRecipe* GetRecipes();
uint32_t GetRecipeCount();

// True after Build has succeeded; false after Reset or before first Build.
bool IsReady();

// Returns the WaterRecipe for a quad whose corner-0 vertexNum is vn.
// Returns nullptr if vn has no water recipe (pure-land quad or vn < 0).
// Only valid after IsReady() returns true.
const WaterRecipe* RecipeForVertexNum(int32_t vn);

// SHORELINE-STAIRCASE-1 (MC2_WATER_SHORE_FEATHER): the water-edge silhouette
// snaps to the terrain tile grid because the shore alpha ramp (shoreBlend =
// max(alphaDepth,1) in the water FS) is only ~alphaDepth world-units wide —
// a 1-2 px transition at top-down altitudes that reads as a hard tile step.
// This gate sets a FLOOR (world units) on the effective alphaDepth used by
// BOTH the FS shore smoothstep (fed via the water bridge, no gameos_graphics
// edit) AND the beach-ring shoreExt geometry (which is exactly alphaDepth*0.5,
// the band's land-side extent — the two stay matched). A wider band dissolves
// the staircase into a soft gradient that HUGS the rendered waterline by
// construction (the FS band is driven by per-fragment WaterThickness =
// waterElevation - interpolated terrain z), so it auto-widens horizontally on
// flat shores (worst staircase) and stays tight on cliffs. Read once, cached.
//   ShoreFeatherFloorWu() : the gate value in wu (0 if unset/<=0 -> disabled).
//   EffAlphaDepth(base)   : max(base, floor) when the gate is on, else base
//                           (default OFF -> returns base -> byte-identical).
float ShoreFeatherFloorWu();
float EffAlphaDepth(float baseAlphaDepth);

// SHORELINE-GEOMETRY-1 (MC2_WATER_SHORE_MEANDER): the SILHOUETTE fix.
//
// Why the feather above was not enough: the water FS already discards on a
// BILINEARLY interpolated WaterThickness, so its iso-contour is sub-tile by
// construction. The staircase survives because the stock heightfield is
// TERRACED — whole runs of shore vertices share one elevation — so the
// waterline iso-contour of that field degenerates onto the tile lattice: each
// boundary tile contributes one straight segment, and those segments line up
// into axis-aligned / 45-degree risers ~1 tile long. Widening the alpha band
// (MC2_WATER_SHORE_FEATHER) blurs those risers but cannot bend them; the
// user rejected it for exactly that reason ("still a literal stair step").
//
// The cure is to perturb the FIELD, not the alpha: add a static, zero-mean,
// world-space fBm displacement (world units) to the thickness that drives the
// shore term, so the discard contour MEANDERS organically across tile
// interiors instead of tracking the lattice. Horizontal contour displacement
// is amplitude / |grad(thickness)|, so it is self-scaling: large on the flat
// terraces that produce the worst steps, small on cliffs. The noise is
// time-INVARIANT (the coastline must not swim) and keyed on continuous world
// XY (seam-free, camera-independent).
//
//   ShoreMeanderWu()   : amplitude in wu (0 if unset/<=0 -> disabled).
//   ShoreMeanderFreq() : spatial frequency in cycles/wu (MC2_WATER_SHORE_MEANDER_FREQ).
//   EffShoreExt(base)  : the SINGLE source of truth for the tile-admission
//                        shore extent, shared by all four predicate mirrors
//                        (this .cpp, both mclib/terrain.cpp narrow-append
//                        branches, and the u_shoreExt fed to
//                        gpu_driven_water.comp). It widens by the meander
//                        amplitude so geometry exists wherever the meander
//                        can push water landward — otherwise the contour
//                        would be clipped back to a tile edge and the step
//                        would reappear.
// All default OFF/0 -> EffShoreExt collapses to the stock expression and the
// FS perturbation term is skipped -> byte-identical.
float ShoreMeanderWu();
float ShoreMeanderFreq();
float EffShoreExt(float baseAlphaDepth);

// STEP 3.4P: the per-frame scalars the cull dispatch uses, captured at the same point so the native Vulkan cull/pack (vk_native_producers) reads
// identical values. Valid for the frame only when the full-recipe authoritative path ran its guards and the native producer is active.
struct VkNativeInputs {
    float    mvp[16];
    float    frameCos;
    float    waterElevation;
    float    shoreExt;
    uint32_t mapSide;
    int32_t  biasLegacy;
    uint32_t recipeCount;
};
bool GetVkNativeInputs(VkNativeInputs& out);

// Captures the per-frame native-producer scalars at the water pass (the GL compute dispatch, the CPU thin pack, the GL draw and the readback
// feed are gone). False = no water this frame.
bool CaptureVkNativeInputs(float frameCos);

} // namespace WaterStream
