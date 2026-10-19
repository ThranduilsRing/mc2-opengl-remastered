#pragma once
// mclib/terrain_surface_bands.h
//
// [TERRAIN_SURFACE] PR-3 -- single source of truth for the distance-band LOD
// configuration (Fork LB = LB-precomputed, RULED). Plan PR-3 / design
// Section 3.2-3.5.
//
// LOCKSTEP DISCIPLINE (memory/cpp_glsl_ubo_struct_lockstep.md): the GLSL
// band-select compute (shaders/gpu_terrain_surface_band.comp) does NOT read
// this C++ header. Instead the host injects these exact values as a `#define`
// preamble at compile time (the proven single-source idiom used for
// gpu_cull's READBACK_SSBO_BINDING -- gpu_cull_compute.cpp:280). One source
// of truth here; the shader can never drift because it is fed from here.
//
// Fork LB realization (design 3.5): per-frame compute selects, per BLOCK, a
// vertex-decimation stride from the mission-static shared-vertex grid and
// emits the block's triangles into a per-frame output index buffer. NO
// tessellator (the existing gos_terrain.tesc scaffold stays dead). The
// per-edge crack-free rule (design 3.3) is enforced by emitting each block
// edge at the COARSER of the two adjacent blocks' strides
// (stride_e = 1 << min(myBand, neighborBand[e])) so the shared edge sample
// set is bit-identical from both sides -> no T-junction. Bands are discrete
// and enumerable so the seam space is the finite band-pair set (design 3.3).
//
// SSE/meshlet drop-in invariant (design 3.4): the mission-static VB / index
// topology / adjacency are LOD-source-agnostic; only this band-select compute
// is swapped for a later SSE/meshlet stage. These constants ARE the
// band-select stage's only tunables.

#include <algorithm>
#include <cstdint>

namespace mc2_terrain_surface_bands {

// ---------------------------------------------------------------------------
// Block = the band-aggregation unit: a kBlockCells x kBlockCells group of
// quad cells of the mission-static grid. A block is the granularity at which
// one band index is chosen; within a block the vertex stride is uniform, and
// the four block EDGES are independently strided to the per-edge neighbor-min
// (crack-free). kBlockCells MUST be a power of two so a stride of 1<<band
// divides the block exactly at every band (no partial cell at a seam).
// ---------------------------------------------------------------------------
constexpr int32_t kBlockCells = 8;   // 8x8 cells / block (power of two)

// ---------------------------------------------------------------------------
// Discrete LOD bands. band 0 = finest (stride 1 cell = the PR-1 static IB
// resolution). Each higher band doubles the vertex stride (halves the
// triangle density). kBandCount-1 is the COARSEST band == the C-1 unarmed
// fixed safe band (design C-1 item 2). The coarsest stride is clamped to
// kBlockCells so the block ALWAYS emits at least its 4 corner verts == 2
// triangles: the HARD `tessLevel(band) >= 1.0` floor (design 3.2,
// distant_buildings_render_at_lower_lod_never_distance_culled.md). NEVER 0,
// NEVER distance-cull -- the distance function only selects a coarser mesh,
// it can never remove a block.
// ---------------------------------------------------------------------------
constexpr int32_t kBandCount = 4;    // bands 0..3 ; band 3 == coarsest/unarmed

// Vertex stride (in grid cells) per band = 1 << band, clamped to kBlockCells.
//   band 0 -> stride 1  (full res, == PR-1 static IB)
//   band 1 -> stride 2
//   band 2 -> stride 4
//   band 3 -> stride 8  (== kBlockCells: 2 tris/block, the >=1 floor)
inline int32_t StrideForBand(int32_t band) {
    band = std::clamp(band, int32_t{0}, kBandCount - 1);
    int32_t s = 1 << band;
    return (s > kBlockCells) ? kBlockCells : s;
}

// ---------------------------------------------------------------------------
// Mission-tunable distance thresholds (world units). A block whose centroid
// distance to the camera is < kBandDist[i] gets band i; beyond the last
// threshold it gets the coarsest band (kBandCount-1). Monotonic increasing.
// These are deliberately generous (long-sightline design: distant terrain
// renders at LOWER LOD, never culled --
// distant_buildings_render_at_lower_lod_never_distance_culled.md). Tuned for
// the MC2 RTS camera range; a single source so Fork DE-style later tuning is
// a one-line change here, never a re-plumb (design 3.5 / §B Fork DE shape).
// ---------------------------------------------------------------------------
constexpr float kBandDist0 = 600.0f;   // < 600  -> band 0 (finest)
constexpr float kBandDist1 = 1400.0f;  // < 1400 -> band 1
constexpr float kBandDist2 = 3000.0f;  // < 3000 -> band 2 ; else band 3

// Pure, deterministic distance->band. Evaluated identically on CPU (fence /
// trace) and GPU (the compute). Determinism is load-bearing for crack-free
// seams: a neighbor block's band is recomputed from ITS centroid by the SAME
// function, so both sides of a shared edge agree on min(myBand,nbBand)
// without a second publish pass (design 3.3).
inline int32_t BandForDistance(float d) {
    if (d < kBandDist0) return 0;
    if (d < kBandDist1) return 1;
    if (d < kBandDist2) return 2;
    return kBandCount - 1;   // coarsest; never culled (>=1 floor)
}

} // namespace mc2_terrain_surface_bands
