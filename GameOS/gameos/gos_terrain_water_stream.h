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
uint32_t UploadAndBindThinRecords();

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

// Read-only access to the recipe array. Pointer is stable across the
// mission lifetime (vector capacity is fixed after Build).
const WaterRecipe* GetRecipes();
uint32_t GetRecipeCount();

// True after Build has succeeded; false after Reset or before first Build.
bool IsReady();

} // namespace WaterStream
