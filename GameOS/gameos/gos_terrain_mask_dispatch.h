// GameOS/gameos/gos_terrain_mask_dispatch.h
//
// Slice B4: per-frame CPU-built visibility mask SSBO pair (SOLID + water).
// Phase B indirect-terrain modernisation.
#pragma once

#include <cstdint>

class TerrainQuad;
using TerrainQuadPtr = TerrainQuad*;

namespace gos_terrain_mask_dispatch {

// SSBO binding constants (grep-verified free as of 2026-05-11 — verify again
// before committing if bindless or GPU-cull slices have landed since).
constexpr uint32_t kMaskSolidSsboBinding   = 17u;
constexpr uint32_t kMaskWaterSsboBinding   = 18u;
constexpr uint32_t kMaskSolidRecipeBinding = 19u;

// ---------- Lifecycle -------------------------------------------------------

// Called once per mission (from primeMissionTerrainCache invalidation hook,
// same call site as gos_terrain_indirect::BuildDenseRecipe). Allocates the
// two mask SSBOs at worst-case size if not already allocated. No-op if already
// allocated. Returns false if allocation fails (caller should fall back to
// legacy drawPass).
bool Init(int32_t mapSide);

// Called on mission swap / teardown. Clears the CPU-side shadow vectors and
// resets IsMaskDispatchReady() to false. Does NOT free the GL SSBOs (they are
// reused across missions).
void Reset();

// ---------- Per-frame predicates -------------------------------------------

// True iff:
//   1. MC2_TERRAIN_MASK_DISPATCH env var is set and not "0"
//   2. gos_terrain_indirect::IsDenseRecipeReady()
//   3. Init() has been called successfully for the current mapSide
// Queried BEFORE the mask build to decide whether to enter the mask-build branch.
bool IsMaskDispatchEnabled();

// True iff IsMaskDispatchEnabled() AND BuildAndUploadMasksForFrame() completed
// successfully this frame (both SSBOs populated and GL_SHADER_STORAGE_BARRIER_BIT issued).
// Queried by the dispatch zones in txmmgr.cpp to decide whether to draw.
// Reset at the top of each frame by BeginFrame().
bool IsMaskDispatchReady();

// Reset per-frame "ready" flag. Call at the start of Terrain::render (before
// the mask-build branch).
void BeginFrame();

// Per-bucket armed predicates (IsMaskDispatchReady() AND per-bucket killswitch).
bool IsFrameMaskSolidArmed();
bool IsFrameMaskWaterArmed();

// ---------- Core per-frame work --------------------------------------------

// Builds both the SOLID and water visibility masks from quadList, uploads
// them to their SSBOs, and issues glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT).
// Sets IsMaskDispatchReady() = true on success.
// quadList: pointer to the first element of Terrain::quadList (camera-windowed).
// numQuads: Terrain::numberQuads for this frame.
void BuildAndUploadMasksForFrame(const TerrainQuadPtr quadList, long numQuads);

// ---------- Draw stubs (Stage 1a: no-op; real bodies land in Stage 1b/1c) --

// Executes the mask-SOLID MDI draw. Returns true if draw was issued.
// Stage 1a: returns false (stub).
bool DrawMaskSolid();

// Executes the mask-water MDI draw. Returns true if draw was issued.
// Stage 1a: returns false (stub).
bool DrawMaskWater();

// Returns the GL buffer name of the SOLID mask SSBO (0 if not allocated).
uint32_t GetSolidMaskSSBO();

// Returns the GL buffer name of the water mask SSBO (0 if not allocated).
uint32_t GetWaterMaskSSBO();

} // namespace gos_terrain_mask_dispatch
