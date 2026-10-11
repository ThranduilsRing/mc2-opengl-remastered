// Shared surface of the quad.cpp translation units (quad.cpp, quad_{setup,draw,water,lines}.cpp).
//
// quad.cpp keeps the mine-grid enqueue, init and the mine texture handles; the rest of TerrainQuad is split by responsibility
// (see DOC in each piece). The cost-split scope structs and terrainTypeToMaterial live here because several pieces use them.
#ifndef QUAD_INTERNAL_H
#define QUAD_INTERNAL_H

#ifndef QUAD_H
#include"quad.h"
#include"terrain_material.h"
#endif

#ifndef VERTEX_H
#include"vertex.h"
#endif

#ifndef TERRAIN_H
#include"terrain.h"
#endif

#ifndef CAMERA_H
#include"camera.h"
#endif
#include"terrain_admission_mode.h"  // F6 T2: shared isModern() flag

#ifndef DBASEGUI_H
#include"dbasegui.h"
#endif

#ifndef CELINE_H
#include"celine.h"
#endif

#ifndef MOVE_H
#include"move.h"
#endif

#include"../GameOS/gameos/gos_profiler.h"
#include"../GameOS/gameos/gos_terrain_patch_stream.h"
#include"../GameOS/gameos/gos_terrain_lod_chunk.h"  // Terrain LOD chunk Phase 3: lastWaterFrame guard
#include"../GameOS/gameos/gos_terrain_indirect.h"
#include"../GameOS/gameos/gos_terrain_lighting.h"
#include"projectz_trace.h"
#include"projectz_overlay.h"
#include"tex_resolve_table.h"

#include <chrono>
#include "Gates/gate.h"
#include "terrain_depth_bias.h"

// Stage 1 cost-split RAII timers — bracket SOLID and DRAWALPHA admit clusters
// inside TerrainQuad::setupTextures so we can split the per-frame
// "Terrain::geometry quadSetupTextures" cost between the SOLID branch (target
// of the indirect-terrain SOLID slice) and the detail/overlay branch (out of
// scope here). When MC2_TERRAIN_COST_SPLIT is unset, the active bool is read
// once via a single branch-predicted cached env load and the destructor
// early-outs without calling steady_clock::now() — production runs pay zero.
namespace {
struct CostSplitSolidScope {
    std::chrono::steady_clock::time_point t0;
    bool active;
    CostSplitSolidScope()
        : active(gos_terrain_indirect::IsCostSplitEnabled()) {
        if (active) t0 = std::chrono::steady_clock::now();
    }
    ~CostSplitSolidScope() {
        if (!active) return;
        const auto dt = std::chrono::steady_clock::now() - t0;
        gos_terrain_indirect::CostSplit_AddSolidNanos(
            std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count());
    }
};
struct CostSplitDetailOverlayScope {
    std::chrono::steady_clock::time_point t0;
    bool active;
    CostSplitDetailOverlayScope()
        : active(gos_terrain_indirect::IsCostSplitEnabled()) {
        if (active) t0 = std::chrono::steady_clock::now();
    }
    ~CostSplitDetailOverlayScope() {
        if (!active) return;
        const auto dt = std::chrono::steady_clock::now() - t0;
        gos_terrain_indirect::CostSplit_AddDetailOverlayNanos(
            std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count());
    }
};
// PR2c Stage 0c — mine-specific RAII timers. Independent of detail/overlay
// bucket so the ~157µs/frame baseline (user-attested 2026-05-08) shows up
// cleanly in the summary line. Pre-existing CostSplitDetailOverlayScope
// brackets at lines 282/290 stay (preserved semantics) — both buckets
// will see mine-cell addTriangleBulk work, but the new buckets give
// PR2c the dedicated number it needs.
struct CostSplitMineEnqueueScope {
    std::chrono::steady_clock::time_point t0;
    bool active;
    CostSplitMineEnqueueScope()
        : active(gos_terrain_indirect::IsCostSplitEnabled()) {
        if (active) t0 = std::chrono::steady_clock::now();
    }
    ~CostSplitMineEnqueueScope() {
        if (!active) return;
        const auto dt = std::chrono::steady_clock::now() - t0;
        gos_terrain_indirect::CostSplit_AddMineEnqueueNanos(
            std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count());
    }
};
struct CostSplitMineDrawScope {
    std::chrono::steady_clock::time_point t0;
    bool active;
    CostSplitMineDrawScope()
        : active(gos_terrain_indirect::IsCostSplitEnabled()) {
        if (active) t0 = std::chrono::steady_clock::now();
    }
    ~CostSplitMineDrawScope() {
        if (!active) return;
        const auto dt = std::chrono::steady_clock::now() - t0;
        gos_terrain_indirect::CostSplit_AddMineDrawNanos(
            std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count());
    }
};
// PR2b Stage 0b — overlay-specific RAII timer. Brackets the M2d fast-path
// block at quad.cpp:2035-2083 (the
// `if (useOverlayTexture && overlayHandle != 0xffffffff)`). Pre-existing
// CostSplitDetailOverlayScope brackets only the legacy Shape-C fallback
// at :485-503 which fires ~0 in tier1 steady state, so this timer is the
// load-bearing measurement for PR2b's retirement target.
struct CostSplitOverlayScope {
    std::chrono::steady_clock::time_point t0;
    bool active;
    CostSplitOverlayScope()
        : active(gos_terrain_indirect::IsCostSplitEnabled()) {
        if (active) t0 = std::chrono::steady_clock::now();
    }
    ~CostSplitOverlayScope() {
        if (!active) return;
        const auto dt = std::chrono::steady_clock::now() - t0;
        gos_terrain_indirect::CostSplit_AddOverlayNanos(
            std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count());
    }
};
// 1A-alt Slice 0 — recon RAII timers for the unmeasured sub-tasks inside
// TerrainQuad::setupTextures. Same MC2_TERRAIN_COST_SPLIT gating; zero-cost
// when disabled. See gos_terrain_indirect.h CostSplit_Add{WaterVertProj,
// Lighting,RecipeCache}Nanos for the bucket contract.
struct CostSplitWaterVertProjScope {
    std::chrono::steady_clock::time_point t0;
    bool active;
    CostSplitWaterVertProjScope()
        : active(gos_terrain_indirect::IsCostSplitEnabled()) {
        if (active) t0 = std::chrono::steady_clock::now();
    }
    ~CostSplitWaterVertProjScope() {
        if (!active) return;
        const auto dt = std::chrono::steady_clock::now() - t0;
        gos_terrain_indirect::CostSplit_AddWaterVertProjNanos(
            std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count());
    }
};
struct CostSplitRecipeCacheScope {
    std::chrono::steady_clock::time_point t0;
    bool active;
    CostSplitRecipeCacheScope()
        : active(gos_terrain_indirect::IsCostSplitEnabled()) {
        if (active) t0 = std::chrono::steady_clock::now();
    }
    ~CostSplitRecipeCacheScope() {
        if (!active) return;
        const auto dt = std::chrono::steady_clock::now() - t0;
        gos_terrain_indirect::CostSplit_AddRecipeCacheNanos(
            std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count());
    }
};
// 1A-alt Slice 0 follow-up — outer + cache-resident buckets to close the
// "missing 8ms" gap discovered when Tracy outer-zone (~11ms mean) exceeded
// sum of named buckets (~3.5ms).
struct CostSplitSetupTotalScope {
    std::chrono::steady_clock::time_point t0;
    bool active;
    CostSplitSetupTotalScope()
        : active(gos_terrain_indirect::IsCostSplitEnabled()) {
        if (active) t0 = std::chrono::steady_clock::now();
    }
    ~CostSplitSetupTotalScope() {
        if (!active) return;
        const auto dt = std::chrono::steady_clock::now() - t0;
        gos_terrain_indirect::CostSplit_AddSetupTotalNanos(
            std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count());
    }
};
struct CostSplitCacheResidentScope {
    std::chrono::steady_clock::time_point t0;
    bool active;
    CostSplitCacheResidentScope()
        : active(gos_terrain_indirect::IsCostSplitEnabled()) {
        if (active) t0 = std::chrono::steady_clock::now();
    }
    ~CostSplitCacheResidentScope() {
        if (!active) return;
        const auto dt = std::chrono::steady_clock::now() - t0;
        gos_terrain_indirect::CostSplit_AddCacheResidentNanos(
            std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count());
    }
};

// Stage 3 SOLID gate-off helpers.
// When indirect SOLID is armed for this frame, BeginLegacySolidCluster()
// returns false and the caller skips the addTriangleBulk(MC2_DRAWSOLID) body.
// EndLegacySolidCluster() bumps the legacy counter (un-armed path only).
//
// IN-MISSION: IsFrameSolidArmed() is always true (ComputePreflight sets it).
// These SOLID blocks are therefore ZERO-COST in-mission. They exist solely for
// the pre-mission deployment / unit-select screens where ComputePreflight() is
// never called and the legacy raster path renders terrain. Do NOT delete them
// until those screens are GPU-armed. (The hot-path cost Tracy sees in
// quadSetupTextures is the DRAWALPHA addTriangleBulk calls below, not SOLID.)
//
// Counter unit: per-quad (per-cluster), NOT per addTriangle call.
static inline bool BeginLegacySolidCluster() {
    return !gos_terrain_indirect::IsFrameSolidArmed();
}
static inline void EndLegacySolidCluster() {
    gos_terrain_indirect::Counters_AddLegacySolidSetupQuad();
}
}  // namespace

inline constexpr unsigned int SELECTION_COLOR = 0xffff7fff;

// Globals defined in quad.cpp (or elsewhere in the engine) that the split pieces share.
extern float g_cpuWaterProbeZ;
extern double g_cpuWaterProbeBestD2;
extern bool g_cpuWaterProbeAny;
extern unsigned long long g_cpuWaterProbeStamp;
extern bool drawTerrainGrid;
extern bool drawLOSGrid;
extern float twoFiveFive;
extern float cosineEyeHalfFOV;
extern DWORD BaseVertexColor;
extern bool useShadows;
extern bool useFog;
extern long sprayFrame;
extern bool useWaterInterestTexture;
extern bool useOverlayTexture;
extern long numTerrainFaces;
extern float MaxMinUV;

void enqueueTerrainMineState(TerrainQuad& quad);   // defined in quad.cpp; the mine grid enqueue, setup and draw call it

// Terrain type -> PBR material index (0=Rock,1=Grass,2=Dirt,3=Concrete):
// single-sourced in terrain_material.h.
inline BYTE terrainTypeToMaterial(DWORD terrainType)
{
    return mc2terrain::terrainTypeToMaterial(static_cast<uint32_t>(terrainType));
}

// Terrain/water NDC depth bias: SINGLE SOURCE OF TRUTH is now
// mclib/terrain_depth_bias.h (+ its lockstep GLSL sibling). These legacy
// CPU raster consumers are the RASTER water regime: WATER_DEPTH_FUDGE
// resolves (via the header's back-compat alias) to WATER_DEPTH_FUDGE_RASTER
// = terrain + 0.0005 = 0.0025. Water has TWO LEGITIMATE REGIMES (RASTER
// here / CPU raster + mask-water; FAST = the GPU water VS at 0.0025) -- they
// are deliberately NOT unified to one value; a single constant regressed
// the map edges (TES tiles through water). Do NOT "collapse the deltas":
// see the two-regime rationale + git 89d7c4f-vs-6ff6c5c reconciliation in
// terrain_depth_bias.h. Consumers below use TERRAIN_DEPTH_FUDGE /
// WATER_DEPTH_FUDGE (== RASTER) unqualified.
using mc2depth::TERRAIN_DEPTH_FUDGE;
using mc2depth::WATER_DEPTH_FUDGE;
inline constexpr float OVERLAY_ELEV_OFFSET = 0.15f;
#define SKY_FUDGE			(1.0f / 60000.0f)
inline constexpr int WATER_ALPHA = 0x1fffffff;
extern float cloudScrollX;
extern float cloudScrollY;

#endif // QUAD_INTERNAL_H
