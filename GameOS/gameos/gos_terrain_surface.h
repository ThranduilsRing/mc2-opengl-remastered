// GameOS/gameos/gos_terrain_surface.h
#pragma once
//
// [TERRAIN_SURFACE] continuous-surface producer -- mission-load generation.
//
// Plan : docs/superpowers/plans/2026-05-18-terrain-continuous-surface-producer-plan.md
//        (PR-1, Wave 1). Design Section 1.3 / C-1 contract item 1 / M-4.
//
// PR-1 builds the four mission-static resources (surface vertex array,
// mission-static index buffer, per-tile material table, per-edge adjacency)
// at mission load, behind the MC2_TERRAIN_SURFACE path-select kill-switch
// (plan Section F, default-OFF). PR-1 is ADDITIVE / DEFAULT-OFF and DELETES
// NOTHING: the surface is generated but NOT drawn or consumed yet (the VS /
// LOD stage / draw repoint are PR-2..PR-4). Its DONE-claim is strictly
// "resources generated + the stock-only fence is green", never a perf /
// substitutive claim (plan PR-1 "Atomic boundary").
//
// Generation source = the existing dense TerrainQuadRecipe corners
// (gos_terrain_indirect.cpp). M-4 stock-only fence: generation consumes ONLY
// stock-derivable map/tile data at mission load, with NO arming /
// ComputePreflight / per-frame-compute precondition and NO savegame field
// (memory/stock_install_must_remain_playable.md). The generation entry point
// is Terrain::primeMissionTerrainCache (the mission-load recipe-build
// chokepoint), NOT ComputePreflight.

#include <cstdint>

namespace gos_terrain_surface {

// ---------------------------------------------------------------------------
// Env kill-switch (plan Section F).
//
//   MC2_TERRAIN_SURFACE -- path-select gate, read ONCE at first query and
//   cached (mirrors gos_terrain_indirect::IsEnabled / the existing
//   indirect-terrain / overlay gates). DEFAULT-OFF for Waves 0-3: only a
//   literal value turns generation on. Separate from the PR-0 trace gate
//   MC2_TERRAIN_SURFACE_TRACE (terrain_surface_trace.h) which is trace-only
//   and never selects a code path.
//
// PR-1 semantics: gate-OFF  -> generation is skipped entirely (zero cost,
//                              behaviour-neutral; the surface never exists).
//                 gate-ON   -> resources are generated at mission load and the
//                              stock-only fence runs; still NO draw (PR-1 lands
//                              generation only).
// ---------------------------------------------------------------------------
bool IsEnabled();   // MC2_TERRAIN_SURFACE (default-OFF)

// ---------------------------------------------------------------------------
// Mission lifecycle (called from mclib/terrain.cpp).
//
//   GenerateForMission() -- called from Terrain::primeMissionTerrainCache
//       AFTER gos_terrain_indirect::BuildDenseRecipe() (the dense recipe is
//       the generation source; it must be populated first). No-op when the
//       kill-switch is OFF. Builds all four resources from the dense recipe +
//       MapData (stock-derivable), runs the stock-only fence, emits the
//       [TERRAIN_SURFACE v1] mission-load lifecycle + fence prints.
//
//   ResetForMission()    -- called from Terrain::destroy (mission teardown)
//       and at the head of GenerateForMission (idempotent rebuild). CPU-clears
//       the surface state; emits the teardown lifecycle print. Always safe to
//       call (no-op when nothing was generated).
//
//   RegenerateForMission() -- called from the InvalidateAllRecipes chokepoint
//       (mission restart / shadow recalc / normal recompute -- the same events
//       that rebuild the dense recipe). Rebuilds from the freshly-rebuilt
//       recipe. No-op when the kill-switch is OFF or nothing was generated.
// ---------------------------------------------------------------------------
void GenerateForMission();
void ResetForMission();
void RegenerateForMission();

// ---------------------------------------------------------------------------
// Read accessors (PR-2+ consumers; PR-1 ships them so the lockstep / fence
// can be inspected and the [TERRAIN_SURFACE v1] prints can report counts).
// All return 0 / empty when the surface has not been generated.
// ---------------------------------------------------------------------------
bool     IsGenerated();        // true iff the four resources are populated
uint32_t GetVertexCount();     // surface vertices (== mapSide^2 nodes)
uint32_t GetIndexCount();      // surface indices (== cells * 6)
uint32_t GetTileCount();       // per-tile material table entries
uint32_t GetAdjacencyCount();  // per-cell adjacency entries (== cells)
int32_t  GetMapSide();         // realVerticesMapSide at generation time

// ---------------------------------------------------------------------------
// PR-2 GPU-upload accessors. PR-1 owns only the CPU std::vector<> generation;
// PR-2's draw bridge (gameos_graphics.cpp) needs the raw bytes to upload the
// surface vertex + index SSBOs once per generation. The pointers are valid
// until the next ResetForMission()/RegenerateForMission(); a generation
// epoch counter lets the bridge detect a rebuild and re-upload. These are
// READ-only views into the mission-static CPU buffers (32 B/vertex,
// 4 B/index); never mutated by the bridge.
// ---------------------------------------------------------------------------
const void* GetVertexData();      // TerrainSurfaceVertex[GetVertexCount()]
const void* GetIndexData();       // uint32_t[GetIndexCount()]
// META-FIX: per-tile material table (TerrainSurfaceTile[GetTileCount()], 32 B
// each) the surface VS reads to forward REAL Texcoord / TerrainType varyings
// (replacing the PR-2 placeholders). Row-major-keyed (mx + my*cells), the same
// key RecipeForVertexNum() populated -- the VS recovers it from the cell's
// top-left vertexNum so it is independent of the ADJUST-1 block-clustered
// index emission order. NULL when not generated.
const void* GetTileData();        // TerrainSurfaceTile[GetTileCount()]
uint32_t    GetGenerationEpoch(); // increments on every (re)generation

// ---------------------------------------------------------------------------
// PR-3 distance-band LOD (Fork LB = LB-precomputed). The band-select compute
// (shaders/gpu_terrain_surface_band.comp) is dispatched per BLOCK -- a
// kBlockCells x kBlockCells group of mission-static quad cells
// (mclib/terrain_surface_bands.h). These accessors expose the block grid
// dimensions + a worst-case output-index upper bound the bridge needs to
// size the per-frame regenerated-index SSBO. They depend ONLY on the
// mission-static map dimensions (NO per-frame / arming state) -- a partial
// far block is clamped, never dropped (>=1 floor; design 3.2).
// ---------------------------------------------------------------------------
uint32_t GetBlocksPerSide();        // ceil((mapSide-1)/kBlockCells); 0 if not gen
uint32_t GetMaxOutputIndexCount();  // worst-case (band 0, all blocks) index cap

} // namespace gos_terrain_surface
