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

#include <GL/glew.h>  // GLuint — required for GetIndirectCmdBuffer() return type

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

// SSBO binding points for the water fast path (chosen to not collide with
// patch_stream's 0/1/2: fat record / recipe / thin record).
constexpr uint32_t kWaterRecipeSsboBinding   = 5;
constexpr uint32_t kWaterThinSsboBinding     = 6;

// GPU upload of the static recipe SSBO. Idempotent — safe to call repeatedly;
// only re-uploads when the recipe count changes (i.e., on a new mission).
// Internally manages a persistent GL buffer, lazily allocated.
// Returns the GL buffer name (0 if not ready).
unsigned int EnsureRecipeBufferUploaded();

// Per-frame thin-record upload. Walks the live quadList window, finds water-
// bearing quads, hashes each via top-left vertexNum to look up its stable
// recipeIdx, packs lightRGB / fogRGB / pzValid into a triple-buffered ring
// SSBO, binds it at kWaterThinSsboBinding. Returns the count of thin
// records emitted (i.e., the per-frame draw instance count).
//
// forceFullWalk (VK-P9a): ignore the narrow-walk fast path and walk the full
// quadList, so g_thinStaging is populated even when the armed full-recipe-
// authoritative GPU path (default-on) has skipped the CPU narrow walk. The VK
// water island passes true to get a CPU-staged thin feed. Default false =
// byte-identical existing behavior for all legacy callers.
uint32_t UploadAndBindThinRecords(bool forceFullWalk = false);

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

// --- Phase C Stage 1: compute dispatch ------------------------------------
//
// ComputeDispatchAndBindThinRecords() replaces UploadAndBindThinRecords() on
// the GPU-driven path. Dispatches two compute shaders:
//   1. gpu_driven_water.comp  — cull/pack (water quads → WaterThinRecord[])
//   2. gpu_driven_cmd_patch.comp — writes DrawArraysIndirectCommand.count
// Followed by GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT.
//
// Returns true on success (g_waterGpuDrivenArmed set true).
// Returns false if killswitch is off, recipe not ready, or window is empty.
// The MDI bridge (Task 1.5) calls IsGpuDrivenArmed() to decide the draw path.
bool ComputeDispatchAndBindThinRecords(float frameCos);

// True if ComputeDispatchAndBindThinRecords() succeeded this frame.
bool IsGpuDrivenArmed();

// GL buffer name for the indirect draw commands (2 × DrawArraysIndirectCommand).
// Valid only when IsGpuDrivenArmed() is true.
GLuint GetIndirectCmdBuffer();

// WATER-THINRING-FENCE-1: fence the thin-record ring slot drawn this frame.
// Call once per frame from the bridge AFTER the water draw is submitted (mirrors
// the solid thin-ring post-draw fence). Closes the cross-frame CPU/GPU slot-reuse
// race the barrier-only sync did not cover. Trace: MC2_WATER_THINRING_TRACE=1.
void EndThinRingFrameFence();

// When MC2_GPU_DRIVEN_PARITY=1: runs both CPU and GPU thin-record paths,
// reads both back from SSBOs, and byte-compares by recipeIdx. Emits
// [GPU_DRIVEN_WATER_PARITY v1] summary every 600 frames.
void ComputeDispatchParity_Check();

// VK-P9a WATER-INTEROP: GPU thin-record READBACK feed for the Vulkan water island.
// Reads back the thin records the armed GPU compute actually packed this frame (count
// from the bucket header, records from the packed ring slot) into an internal CPU copy
// and returns the count with *outPtr pointing at it (48B WaterThinRecord each, valid
// until the next call). Call AFTER ComputeDispatchAndBindThinRecords (the compute's
// final barrier makes the data visible). Returns 0 if the GPU path didn't run this
// frame. This is the reliable feed on the armed full-recipe-authoritative path, where
// the CPU pack (UploadAndBindThinRecords) yields 0 records.
uint32_t ReadbackGpuThinForVk(const void** outPtr);

// Tear down GL buffers (mission unload, app shutdown).
void ReleaseGlResources();

// --- Stage 3 parity check ---------------------------------------------------
//
// MC2_RENDER_WATER_PARITY_CHECK=1 enables a per-frame CPU-side byte-comparison
// between the legacy `TerrainQuad::drawWater()` `addVertices` argument streams
// and the fast-path's recipe + thin-record + per-vertex VS-equivalent CPU
// computation, for every in-window water-bearing quad this frame.
//
// Silent on success (matches the M2 thin-record parity convention in
// `m2_thin_record_cpu_reduction_results.md`). On any field-level mismatch:
//
//   [WATER_PARITY v1] event=mismatch frame=N quad=Q layer=base|detail
//                     tri=T vert=V field=<name> legacy=0xHEX fast=0xHEX
//
// Mismatch prints are throttled to first 16 per frame to keep logs bounded
// when an early-frame uniform delta cascades. A monotonic summary line is
// emitted every 600 frames AND on `ReleaseGlResources()`:
//
//   [WATER_PARITY v1] event=summary frames=N quads_checked=Q
//                     total_mismatches=K
//
// Comparison granularity:
// - Recipe-input fields (per-corner vx/vy/elev/terrainType/waterBits, uvMode,
//   hasDetail) — compared against q.vertices[i]/blocks state.
// - Thin-record fields (per-corner lightRGB/fogRGB, per-tri pzValid bit) —
//   compared against the same q.vertices[i] reads + CPU-pz check.
// - Derived gos_VERTEX bytes (u, v, argb, frgb-high-byte) — synthesized on
//   both sides per legacy `drawWater` formulas vs fast-path VS formulas, then
//   byte-compared. x/y/z/rhw NOT compared: legacy reads pre-projected CPU
//   `wx/wy/wz/ww`; fast path projects on GPU. Per-spec line 122: comparing
//   GPU-rendered output drifts below 1 ULP and produces fake mismatches.
//
// Per `feedback_offload_scope_stock_only.md`, parity is gated against stock
// content only (tier1 missions). Mod content is out of scope.
struct ParityFrameUniforms {
    float    waterElevation;
    float    alphaDepth;
    uint32_t alphaEdgeDword;     // Terrain::alphaEdge   (full DWORD, not byte)
    uint32_t alphaMiddleDword;   // Terrain::alphaMiddle
    uint32_t alphaDeepDword;     // Terrain::alphaDeep
    float    mapTopLeftX;
    float    mapTopLeftY;
    float    frameCos;
    float    frameCosAlpha;
    float    oneOverTF;
    float    oneOverWaterTF;
    float    cloudOffsetX;
    float    cloudOffsetY;
    float    sprayOffsetX;
    float    sprayOffsetY;
    float    maxMinUV;
    bool     useWaterInterestTexture;
    uint32_t waterDetailHandleSentinel;  // 0xffffffff if no detail bound this frame
    bool     terrainTextures2Present;
};

// Run the parity check for the current frame. Reads g_thinStaging (built by
// the most recent UploadAndBindThinRecords call, still populated). No-op
// unless MC2_RENDER_WATER_PARITY_CHECK is set in the environment.
//
// Cost in dev mode: ~12 byte-comparisons per in-window water-bearing quad
// per frame (~1.3K quads on mc2_01 → ~15K compare ops, sub-millisecond CPU).
void CheckParityFrame(const ParityFrameUniforms& u);

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

// Returns the water parity mask for this frame (indexed by corner-0 vertexNum,
// same format as gos_terrain_mask_dispatch's s_waterMask). Built during
// UploadAndBindThinRecords — only valid after that call. outWords = 450.
const uint32_t* GetWaterParityMask(int* outWords);

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

} // namespace WaterStream
