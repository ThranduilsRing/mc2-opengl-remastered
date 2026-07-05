//---------------------------------------------------------------------------
//
// Quad.cpp -- File contains class code for the Terrain Quads
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include Files
#ifndef QUAD_H
#include"quad.h"
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

// Per-quad scratch for the four BoolAdmission per-vertex projectZ calls in
// TerrainQuad::setupTextures. After each `eye->projectZ()` call we copy
// g_pzLastPredicates into the matching slot, so pz_emit_terrain_tris (called
// later for the same quad) can hand both screen-state AND predicate-state to
// the overlay without ever recomputing predicates. Single-threaded terrain
// submission makes the file-static safe.
static ProjectZPredicates s_pzVertPreds[4] = {};
static inline void pz_capture_vert_preds(int slot) {
    if (g_pzTrace) s_pzVertPreds[slot] = g_pzLastPredicates;
}
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

// [DEPTH_TRANSITION v1] cross-TU CPU-water REAL screen-z sample (env-gated
// MC2_DEPTH_TRANSITION_PROBE; silent default). Written by TerrainQuad::drawWater
// for the water vertex nearest the fixed mid-map probe point P; READ by the
// transition dump in gos_terrain_indirect.cpp. g_cpuWaterProbeStamp is bumped
// once per CPU-water frame so the dump can detect a STALE value (CPU water and
// the GPU water fast path are mutually exclusive per frame -- on an armed
// transition frame this is the last un-armed steady sample, NOT this-frame).
float    g_cpuWaterProbeZ        = 0.0f;
double   g_cpuWaterProbeBestD2   = 0.0;
bool     g_cpuWaterProbeAny      = false;
unsigned long long g_cpuWaterProbeStamp = 0;
// Per-CPU-water-frame reset of the nearest-vertex search is done once/frame in
// Terrain::renderWater (terrain.cpp) BEFORE the drawWater quad loop -- the
// single legacy-loop entry where CPU water is the live producer. It clears
// g_cpuWaterProbeAny / g_cpuWaterProbeBestD2 and bumps g_cpuWaterProbeStamp.

static const bool s_shapeCParityCheck = (getenv("MC2_SHAPE_C_PARITY_CHECK") != nullptr);

// Step 1b-1 (cpu-pack-retirement plan §2 A1): the M2 thin-record emit scope is
// orphaned by Fix B (commit 005ebc7) — the GPU compute shader
// gpu_driven_terrain_solid.comp re-derives the thin record + the pzTri flag
// bits for every quad it admits and overwrites the CPU-written record. When
// gos_terrain_indirect::IsFrameSolidArmed() is true (default-armed under
// MC2_GPU_DRIVEN_TERRAIN_SOLID=1) the CPU thin-emit is dead work. Only the
// thin-emit is gated; pz/pzTri compute, the both-culled cull, and the M2d
// gos_PushTerrainOverlay decal producer stay UNCONDITIONAL (the live
// cement-transition/runway/road decal path — §2 WARNING, 9964d5a regression
// class). MC2_TERRAIN_INDIRECT_THINEMIT_TRACE=1 emits a one-shot lifecycle
// line so smoke artifacts capture the gate state.
static const bool s_thinEmitTrace =
    (getenv("MC2_TERRAIN_INDIRECT_THINEMIT_TRACE") != nullptr);

// MC2_M2D_PZ_PARITY probe (VPL Step 8b precursor validator). Re-authored per
// docs/superpowers/reviews/2026-05-15-overlay-pz-scoped-redesign-rereview.md
// Finding 5. Arming is a static file-scope getenv read (NOT a Renderer!=3
// arm-guard — that guard sat in the dead legacy arm; the modern fast path is
// the perspective GL renderer, Environment.Renderer != 3). When armed, the
// M2d pzc/pzTri block bit-compares pzc_new (the corrected reprojection +
// clipInfo==0 sentinel guard) against pzc_old (the literal vertices[c]->pz
// formula, legal probe-only because terrain.cpp still writes cv->pz pre-Step-8b
// and the read is confined to a probe-local). First-N mismatches are emitted
// unconditionally (NOT frame-modulo); a per-mission summary line asserts
// checked > 0 (the v1 failure mode was checked == 0 = probe never fired).
// Demote-don't-delete after green per the Debug-instrumentation rule.
static const bool s_m2dPzParity = (getenv("MC2_M2D_PZ_PARITY") != nullptr);
static int  s_m2dPzParityChecked = 0;       // quads the probe actually compared
static int  s_m2dPzParityMismatch = 0;      // quads with any pzc/pzTri divergence
static int  s_m2dPzParityEmitted = 0;       // first-N detail lines emitted so far
static bool s_m2dPzParityArmLogged = false; // one-shot arm/scope-assertion line
static const int s_m2dPzParityEmitCap = 32; // first-N detail-line cap
static const bool s_shapeCEnabled = ([] {
	// Default ON post Slice-1 flip (2026-04-29). Tier1 parity validated:
	// 19.7M field-equality checks across 5 missions, zero mismatches.
	// Explicit "0" still opts out for bisection; any other value (including unset) opts in.
	const char* env = getenv("MC2_MODERN_TERRAIN_PATCHES");
	return (env == nullptr) || (env[0] != '0');
})();

#define SELECTION_COLOR 0xffff7fff
#define HIGHLIGHT_COLOR	0xff00ff00

// Map MC2 terrain type enum (0-20) to PBR material index (0-3)
// 0=Rock, 1=Grass, 2=Dirt, 3=Concrete
static BYTE terrainTypeToMaterial(DWORD terrainType)
{
    switch (terrainType)
    {
        case 3:  // Moss
        case 8:  // ForestFloor
        case 9:  // Grass
        case 12: // Slimy
            return 1; // Grass
        case 2:  // Mud
        case 4:  // Dirt
            return 2; // Dirt
        case 10: // Concrete
        case 13: case 14: case 15: case 16: // Cement 2-5
        case 17: case 18: case 19:          // Cement 6-8
        case 20:                            // END_CEMENT_TYPE (terrtxm.h:44)
            return 3; // Concrete
        default:
            return 0; // Rock (Mountain, Cliff, Ash, Tundra, Water, None)
    }
}

//---------------------------------------------------------------------------
float TerrainQuad::rainLightLevel = 1.0f;
DWORD TerrainQuad::lighteningLevel = 0;
DWORD TerrainQuad::mineTextureHandle = 0xffffffff;
DWORD TerrainQuad::blownTextureHandle = 0xffffffff;

extern bool drawTerrainGrid;
extern bool drawLOSGrid;

// Fill terrain extra data for tessellation — routed to per-node storage via texture manager
static void fillTerrainExtra(DWORD texHandle, DWORD flags, VertexPtr v0, VertexPtr v1, VertexPtr v2) {
    gos_TERRAIN_EXTRA textra[3];
    textra[0].wx = v0->vx; textra[0].wy = v0->vy; textra[0].wz = v0->pVertex->elevation;
    textra[0].nx = v0->pVertex->vertexNormal.x; textra[0].ny = v0->pVertex->vertexNormal.y; textra[0].nz = v0->pVertex->vertexNormal.z;
    textra[1].wx = v1->vx; textra[1].wy = v1->vy; textra[1].wz = v1->pVertex->elevation;
    textra[1].nx = v1->pVertex->vertexNormal.x; textra[1].ny = v1->pVertex->vertexNormal.y; textra[1].nz = v1->pVertex->vertexNormal.z;
    textra[2].wx = v2->vx; textra[2].wy = v2->vy; textra[2].wz = v2->pVertex->elevation;
    textra[2].nx = v2->pVertex->vertexNormal.x; textra[2].ny = v2->pVertex->vertexNormal.y; textra[2].nz = v2->pVertex->vertexNormal.z;
    mcTextureManager->addTerrainExtra(texHandle, textra, flags);
}

// Builds the same 3-element gos_TERRAIN_EXTRA[3] that fillTerrainExtra
// would push to addTerrainExtra, but returns it by value so the modern
// patch stream can mirror-write the bytes without re-reading the verts.
// Keeping the math identical to fillTerrainExtra is the BR-byte-parity
// guarantee for shadow / grass behavior.
static inline void buildTerrainExtraTriple(VertexPtr v0, VertexPtr v1, VertexPtr v2,
                                           gos_TERRAIN_EXTRA out[3])
{
    VertexPtr vs[3] = { v0, v1, v2 };
    for (int k = 0; k < 3; ++k) {
        out[k].wx = vs[k]->vx;
        out[k].wy = vs[k]->vy;
        out[k].wz = vs[k]->pVertex->elevation;
        out[k].nx = vs[k]->pVertex->vertexNormal.x;
        out[k].ny = vs[k]->pVertex->vertexNormal.y;
        out[k].nz = vs[k]->pVertex->vertexNormal.z;
    }
}

// Per-triangle diagnostic hook for terrain addTriangle sites.
// Observation-only; must be called AFTER addTriangle so it cannot affect submission.
// Active only when g_pzTrace is true (any PROJECTZ env var set).
static void pz_emit_terrain_tris(
    VertexPtr*  verts,     // array of 4
    int         uvMode,
    const char* callsiteId,
    const char* file,
    int         line)
{
    if (!g_pzTrace) return;
    long rowCol = verts[0]->posTile;
    int tileR = rowCol >> 16;
    int tileC = rowCol & 0x0000ffff;
    ProjectZTriVert pzv[4];
    for (int i = 0; i < 4; i++) {
        pzv[i].legacyAccepted = (verts[i]->clipInfo != 0);
        pzv[i].wx = verts[i]->px;
        pzv[i].wy = verts[i]->py;
        pzv[i].wz = verts[i]->pz;
        pzv[i].vx = verts[i]->vx;
        pzv[i].vy = verts[i]->vy;
        pzv[i].vz = verts[i]->pVertex->elevation;
    }
    // `eye` is the global CameraPtr declared in camera.h — the same camera the
    // BoolAdmission projectZ calls in setupTextures used.
    float resX = eye ? eye->fgetScreenResX() : 1920.0f;
    float resY = eye ? eye->fgetScreenResY() : 1080.0f;
    if (uvMode == BOTTOMRIGHT) {
        static const int c012[] = {0,1,2};
        static const int c023[] = {0,2,3};
        projectz_emit_tri(callsiteId, tileR, tileC, c012, pzv, true, file, line);
        projectz_emit_tri(callsiteId, tileR, tileC, c023, pzv, true, file, line);
        projectz_overlay_record_tri(pzv, s_pzVertPreds, c012, resX, resY);
        projectz_overlay_record_tri(pzv, s_pzVertPreds, c023, resX, resY);
    } else {
        static const int c013[] = {0,1,3};
        static const int c123[] = {1,2,3};
        projectz_emit_tri(callsiteId, tileR, tileC, c013, pzv, true, file, line);
        projectz_emit_tri(callsiteId, tileR, tileC, c123, pzv, true, file, line);
        projectz_overlay_record_tri(pzv, s_pzVertPreds, c013, resX, resY);
        projectz_overlay_record_tri(pzv, s_pzVertPreds, c123, resX, resY);
    }
}

static bool isTerrainQuadVisible(const TerrainQuad& quad)
{
	if (quad.uvMode == BOTTOMRIGHT)
	{
		long clipped1 = quad.vertices[0]->clipInfo + quad.vertices[1]->clipInfo + quad.vertices[2]->clipInfo;
		long clipped2 = quad.vertices[0]->clipInfo + quad.vertices[2]->clipInfo + quad.vertices[3]->clipInfo;
		return (clipped1 || clipped2) != 0;
	}

	long clipped1 = quad.vertices[0]->clipInfo + quad.vertices[1]->clipInfo + quad.vertices[3]->clipInfo;
	long clipped2 = quad.vertices[1]->clipInfo + quad.vertices[2]->clipInfo + quad.vertices[3]->clipInfo;
	return (clipped1 || clipped2) != 0;
}

static void enqueueTerrainMineState(TerrainQuad& quad)
{
	// PR2c Stage 0c — bracket the entire function for the dedicated mine bucket.
	// Counter increments once per call (per-quad). User-attested 2026-05-08
	// baseline ~157 µs/frame across all per-frame calls; the cost-split timer
	// confirms.
	CostSplitMineEnqueueScope _csMineEnq;
	gos_terrain_indirect::Counters_AddLegacyMineEnqueueQuad();

	long rowCol = quad.vertices[0]->posTile;
	long tileR = rowCol>>16;
	long tileC = rowCol & 0x0000ffff;

	if (GameMap)
	{
		quad.mineResult.init();

		// Fast skip for the ~99% of quads with no mines/scorch in any cell.
		// GameMap maintains tileMineCount incrementally on setMine() and rebuilds
		// after packet readPacket(); pre-rebuild it returns true conservatively.
		if (!GameMap->tileHasMines(tileR, tileC))
			return;

		long cellPos = 0;
		for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
		{
			for (long cellC = 0; cellC < MAPCELL_DIM; cellC++,cellPos++)
			{
				long actualCellRow = tileR * MAPCELL_DIM + cellR;
				long actualCellCol = tileC * MAPCELL_DIM + cellC;
				
				DWORD localResult = 0;
				if (GameMap->inBounds(actualCellRow, actualCellCol))
					localResult = GameMap->getMine(actualCellRow, actualCellCol);
					
				if (localResult == 1)
				{
					tex_resolve(TerrainQuad::mineTextureHandle);
					{ CostSplitDetailOverlayScope _csDetail;
					mcTextureManager->addTriangleBulk(TerrainQuad::mineTextureHandle, MC2_DRAWALPHA, 2);
					}
					quad.mineResult.setMine(cellPos,localResult);
				}
				else if (localResult == 2)
				{
					tex_resolve(TerrainQuad::blownTextureHandle);
					{ CostSplitDetailOverlayScope _csDetail;
					mcTextureManager->addTriangleBulk(TerrainQuad::blownTextureHandle, MC2_DRAWALPHA, 2);
					}
					quad.mineResult.setMine(cellPos,localResult);
				}
			}
		}
	}
}

// Terrain LOD chunk Phase 7A — standalone mine enqueue from visible quadList.
// Called from Terrain::render() when MC2_TERRAIN_LOD_CHUNK=1 is active,
// replacing the mine enqueue that was embedded in setupTextures().
// Iterates the camera-relative quadList (same population as the minePass
// drawMine loop) and runs enqueueTerrainMineState on every quad.
// The setupTextures mine-enqueue call is suppressed by a parallel guard in
// setupTextures() when MC2_TERRAIN_LOD_CHUNK=1, so no double-enqueue occurs.
// Callers must gate on !IsFrameMineArmed() — the GPU static-bake path owns
// mine state when armed and this function must not run.
void TerrainQuad::enqueueMinesFromGrid (TerrainQuadPtr quadList, long count)
{
	TerrainQuadPtr q = quadList;
	for (long i = 0; i < count; ++i, ++q)
		enqueueTerrainMineState(*q);
}

//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
// Class TerrainQuad
long TerrainQuad::init (VertexPtr v0, VertexPtr v1, VertexPtr v2, VertexPtr v3)
{
	vertices[0] = v0;
	vertices[1] = v1;
	vertices[2] = v2;
	vertices[3] = v3;

	return(NO_ERR);
}

float twoFiveFive = 255.0;
#define HUD_DEPTH		0.9999f			//HUD Objects draw over everything else (reverse-Z: near=1, far=0).
extern float cosineEyeHalfFOV; 
// NS3 boundary: BaseVertexColor canonical def (was code/ leak; consumers extern it).
DWORD BaseVertexColor = 0x00000000;
extern bool useShadows;
extern bool useFog;
extern long sprayFrame;
bool useWaterInterestTexture = true;
bool useOverlayTexture = true;
long numTerrainFaces = 0;
extern float MaxMinUV;


//---------------------------------------------------------------------------
struct TerrainRecipe {
	DWORD terrainHandle       = 0xffffffff;
	DWORD terrainDetailHandle = 0xffffffff;
	DWORD overlayHandle       = 0xffffffff;
	TerrainUVData uvData      = {};
	bool isCement             = false;
	bool isAlpha              = false;
};

static void shapeC_checkParity(
	const TerrainRecipe& inlineR,
	const MapData::WorldQuadTerrainCacheEntry& e,
	long tileR, long tileC, long uvMode)
{
	static int s_mismatches = 0;
	static int s_checks     = 0;
	++s_checks;

	const char* siteTag =
		!inlineR.isCement ? (uvMode == BOTTOMRIGHT ? "BR_no_cement" : "TL_no_cement")
	  : inlineR.isAlpha   ? (uvMode == BOTTOMRIGHT ? "BR_alpha"     : "TL_alpha")
						  : (uvMode == BOTTOMRIGHT ? "BR_cement"    : "TL_cement");

	bool ok = true;

#define SHAPE_C_CHECK_DWORD(field) \
	if (inlineR.field != e.field) { \
		if (s_mismatches < 10) \
			printf("[SHAPE_C] MISMATCH " #field " site=%s tileR=%ld tileC=%ld" \
				   " inline=%u cached=%u\n", \
				   siteTag, tileR, tileC, inlineR.field, e.field); \
		ok = false; ++s_mismatches; }

#define SHAPE_C_CHECK_BOOL(method) \
	if (inlineR.method != e.method()) { \
		if (s_mismatches < 10) \
			printf("[SHAPE_C] MISMATCH " #method " site=%s tileR=%ld tileC=%ld" \
				   " inline=%d cached=%d\n", \
				   siteTag, tileR, tileC, (int)inlineR.method, (int)e.method()); \
		ok = false; ++s_mismatches; }

	SHAPE_C_CHECK_DWORD(terrainHandle)
	SHAPE_C_CHECK_DWORD(terrainDetailHandle)
	SHAPE_C_CHECK_DWORD(overlayHandle)
	SHAPE_C_CHECK_BOOL(isCement)
	SHAPE_C_CHECK_BOOL(isAlpha)

#undef SHAPE_C_CHECK_DWORD
#undef SHAPE_C_CHECK_BOOL

	if (fabsf(inlineR.uvData.minU - e.uvData.minU) > 1e-5f ||
		fabsf(inlineR.uvData.minV - e.uvData.minV) > 1e-5f ||
		fabsf(inlineR.uvData.maxU - e.uvData.maxU) > 1e-5f ||
		fabsf(inlineR.uvData.maxV - e.uvData.maxV) > 1e-5f)
	{
		if (s_mismatches < 10)
			printf("[SHAPE_C] MISMATCH uvData site=%s tileR=%ld tileC=%ld"
				   " inline=(%.5f,%.5f,%.5f,%.5f) cached=(%.5f,%.5f,%.5f,%.5f)\n",
				   siteTag, tileR, tileC,
				   inlineR.uvData.minU, inlineR.uvData.minV,
				   inlineR.uvData.maxU, inlineR.uvData.maxV,
				   e.uvData.minU, e.uvData.minV, e.uvData.maxU, e.uvData.maxV);
		ok = false; ++s_mismatches;
	}

	// Summary every 10000 checks and on first check of session.
	if (s_checks == 1 || s_checks % 10000 == 0)
		printf("[SHAPE_C] parity checks=%d mismatches=%d\n", s_checks, s_mismatches);
	(void)ok;
}

// Fills recipe from inline getTextureHandle() calls. Always correct; never stale.
static void buildTerrainRecipeInline(VertexPtr* vertices, long uvMode, TerrainRecipe& r)
{
	r.isCement = Terrain::terrainTextures->isCement(vertices[0]->pVertex->textureData & 0x0000ffff);
	r.isAlpha  = Terrain::terrainTextures->isAlpha (vertices[0]->pVertex->textureData & 0x0000ffff);

	if (!r.isCement)
	{
		VertexPtr vA = (uvMode == BOTTOMRIGHT) ? vertices[0] : vertices[1];
		VertexPtr vB = (uvMode == BOTTOMRIGHT) ? vertices[2] : vertices[3];
		r.terrainHandle       = Terrain::terrainTextures2->getTextureHandle(vA, vB, &r.uvData);
		r.terrainDetailHandle = Terrain::terrainTextures2->getDetailHandle();
		r.overlayHandle       = 0xffffffff;
	}
	else if (r.isAlpha)
	{
		r.overlayHandle       = Terrain::terrainTextures->getTextureHandle(vertices[0]->pVertex->textureData & 0x0000ffff);
		r.terrainHandle       = Terrain::terrainTextures2->getTextureHandle(vertices[1], vertices[3], &r.uvData);
		r.terrainDetailHandle = Terrain::terrainTextures2->getDetailHandle();
	}
	else // pure cement
	{
		r.terrainHandle       = Terrain::terrainTextures->getTextureHandle(vertices[0]->pVertex->textureData & 0x0000ffff);
		r.terrainDetailHandle = 0xffffffff;
		r.overlayHandle       = 0xffffffff;
	}
}

// Fills recipe from cache when s_shapeCEnabled and entry is valid.
// Returns false if cache unavailable; recipe is unmodified.
static bool tryGetCachedTerrainRecipe(
	const MapData::WorldQuadTerrainCacheEntry* entry, TerrainRecipe& r)
{
	if (!s_shapeCEnabled || !entry || !entry->isValid())
		return false;
	r.isCement            = entry->isCement();
	r.isAlpha             = entry->isAlpha();
	r.terrainHandle       = entry->terrainHandle;
	r.terrainDetailHandle = entry->terrainDetailHandle;
	r.overlayHandle       = entry->overlayHandle;
	r.uvData              = entry->uvData;
	return true;
}

// Reserves the recipe's terrain triangle pairs via addTriangleBulk — one slot-walk
// per (handle, flags) tuple instead of two consecutive addTriangle calls each. Output
// numVertices is mathematically identical to the prior `addTriangle ×2` pattern.
// Does NOT call pz_emit_terrain_tris -- that stays at the setupTextures() callsite
// to preserve projectZ callsite identity.
static void addTerrainTriangles(const TerrainRecipe& r)
{
	// Stage 1 cost-split brackets — this is the hot path under Shape C
	// default-on (commit aee39cc 2026-04-29). The plan v2 verification
	// appendix V8 cited the legacy manual-emit clusters at quad.cpp:507/586
	// but those only fire under MC2_MODERN_TERRAIN_PATCHES=0; under Shape C
	// the dispatch is here via TerrainPatchStream's recipe cache.
	if (!r.isCement)
	{
		if (r.terrainHandle != 0)
		{
			// Hot-path SOLID gate-off (Stage 3): skip addTriangleBulk(SOLID) when
			// indirect draw is armed — the indirect path owns SOLID this frame.
			if (BeginLegacySolidCluster()) {
				CostSplitSolidScope _csSolid;
				mcTextureManager->addTriangleBulk(r.terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, 2);
				EndLegacySolidCluster();
			}
		}
	}
	else if (r.isAlpha)
	{
		if (r.terrainHandle != 0) {
			if (BeginLegacySolidCluster()) {
				CostSplitSolidScope _csSolid;
				mcTextureManager->addTriangleBulk(r.terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, 2);
				EndLegacySolidCluster();
			}
		}
	}
	else // pure cement
	{
		if (r.terrainHandle != 0) {
			if (BeginLegacySolidCluster()) {
				CostSplitSolidScope _csSolid;
				mcTextureManager->addTriangleBulk(r.terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, 2);
				EndLegacySolidCluster();
			}
		}
	}
}

void TerrainQuad::initMineTextureHandles (void)
{
	// A5 / R7: ensure the mine + scorch texture handles are loaded at mission-prime
	// time so they are valid even when TerrainQuad::setupTextures never runs (Phase 8z
	// removes setupTextures; its inline lazy-load was the ONLY loader, so deleting it
	// would leave the handles at 0xffffffff -> BuildMineTextureArray bails -> mine
	// tiles render black forever). The 0xffffffff guards make this idempotent.
	if (mineTextureHandle == 0xffffffff)
	{
		FullPathFileName mineTextureName;
		mineTextureName.init(texturePath,"defaults" PATH_SEPARATOR "mine_00",".tga");
		mineTextureHandle = mcTextureManager->loadTexture(mineTextureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink, 0, 0x1);
	}

	if (blownTextureHandle == 0xffffffff)
	{
		FullPathFileName mineTextureName;
		mineTextureName.init(texturePath,"defaults" PATH_SEPARATOR "minescorch_00",".tga");
		blownTextureHandle = mcTextureManager->loadTexture(mineTextureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink, 0, 0x1);
	}
}

#ifdef MC2_IS_EDITOR
// 8z-B editor quarantine: the legacy per-quad texture setup is retained ONLY in the
// editor build (it backs the passability-grid drawLine path via clipInfo). The game
// build compiles it out -- the GPU-indirect/chunk terrain path is armed from frame 1
// (FASTPATH proof: 0 warmup drops), so there is no legacy fallback to draw, and a hard
// GL/init failure is surfaced loudly by ForceDisableArmingForProcess (T16/T19 loud-fail).
// Mine texture handles load independently via TerrainQuad::initMineTextureHandles (A5).
void TerrainQuad::setupTextures (void)
{
	CostSplitSetupTotalScope _csSetup; // 1A-alt Slice 0 follow-up — full-function bucket
	if (mineTextureHandle == 0xffffffff)
	{
		FullPathFileName mineTextureName;
		mineTextureName.init(texturePath,"defaults" PATH_SEPARATOR "mine_00",".tga");
		mineTextureHandle = mcTextureManager->loadTexture(mineTextureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink, 0, 0x1);
	}

	if (blownTextureHandle == 0xffffffff)
	{
		FullPathFileName mineTextureName;
		mineTextureName.init(texturePath,"defaults" PATH_SEPARATOR "minescorch_00",".tga");
		blownTextureHandle = mcTextureManager->loadTexture(mineTextureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink, 0, 0x1);
	}

	// quadList is allocated via Malloc+memset(0), not C++ new, so TerrainQuad's
	// constructor and init() are never called. Reset render handles to their
	// sentinel values (0xffffffff = "no texture") so draw() exits at its first
	// guard rather than emitting tris with handle=0 when the GPU path is armed.
	terrainHandle       = 0xffffffff;
	terrainDetailHandle = 0xffffffff;
	waterHandle         = 0xffffffff;
	waterDetailHandle   = 0xffffffff;
	overlayHandle       = 0xffffffff;
	isCement            = false;

	// S6: hoist the armed-water gate predicate ONCE per quad. The shared
	// WaterFastPathOwnsArmedDraw() is a non-inline cross-TU call that itself
	// fans into IsFrameSolidArmed()/WaterStream::IsReady()/GetRecipeCount();
	// calling it per-vertex (x5/quad) in this hot per-quad loop would tension
	// the 100ns hot-loop rule. It is frame-invariant within a frame, so
	// compute it once here. legacyWaterDraw == "GPU fast path does NOT own
	// the armed draw -> the legacy CPU (ii) draw-side must run".
	// BOUNDARY (CONCERN-1, do not break): the (ii) gate below wraps ONLY the
	// per-vertex wx/wy/wz/ww writes + the clipped-body handle-resolution +
	// addTriangleBulk. clipInfo=clipData, calcThisFrame|=2, the leastZ/mostZ/
	// leastW/mostW/leastWY/mostWY 6-tuple, and the 0xffffffff sentinel(s)
	// MUST stay UNCONDITIONAL (they are (i)/M2a - never move them inside the
	// if (legacyWaterDraw) braces).
	const bool legacyWaterDraw = !gos_terrain_indirect::WaterFastPathOwnsArmedDraw();
	// Terrain LOD chunk Phase 3: cache env gate once per quad (hot path; avoid
	// repeated getenv() in the per-vertex loop below).
	const bool useLodChunkWaterGuard = (mc2TerrainLodChunkEnabled());

	// PERF-SETUPTEXTURES-GPU-GATE-1: gate the recipe-cache lookup + member-field
	// assignments when both GPU SOLID and GPU OVERLAY own this frame. In that state
	// terrain.cpp skips draw() entirely, so terrainHandle/terrainDetailHandle/
	// overlayHandle/uvData/isCement have no live consumers. addTerrainTriangles is
	// already internally no-op via BeginLegacySolidCluster() when solid is armed;
	// pz_emit_terrain_tris is diagnostic-only (no-op unless g_pzTrace set). The
	// prior failed attempt (9964d5a) gated on !IsFrameSolidArmed() alone, which
	// broke when IsFrameSolidArmed=true but IsFrameOverlayArmed=false (draw() still
	// ran and needed overlayHandle). Correct guard requires BOTH armed.
	// Kill-switch: MC2_SETUPTEXTURES_LEGACY_FORCE=1 restores full legacy execution.
	static const bool s_legacyForce =
	    (getenv("MC2_SETUPTEXTURES_LEGACY_FORCE") != nullptr);
	const bool legacyTerrainNeeded =
	    s_legacyForce ||
	    !(gos_terrain_indirect::IsFrameSolidArmed() &&
	      gos_terrain_indirect::IsFrameOverlayArmed());

 	if (!Terrain::terrainTextures2)
	{
		if (uvMode == BOTTOMRIGHT)
		{
			long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[2]->clipInfo;
			long clipped2 = vertices[0]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;

			if (clipped1 || clipped2)
			{
				{
					terrainHandle = Terrain::terrainTextures->getTextureHandle((vertices[0]->pVertex->textureData & 0x0000ffff));
					DWORD terrainDetailData = Terrain::terrainTextures->setDetail(1,0);
					if (terrainDetailData != 0xfffffff)
						terrainDetailHandle = Terrain::terrainTextures->getTextureHandle(terrainDetailData);
					else
						terrainDetailHandle = 0xffffffff;
					overlayHandle = 0xffffffff;

					// Cold-path SOLID gate-off (Stage 3, cluster A): under
					// MC2_MODERN_TERRAIN_PATCHES=0 only.
					if (BeginLegacySolidCluster()) {
					CostSplitSolidScope _csSolid;
					mcTextureManager->addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
					mcTextureManager->addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
					EndLegacySolidCluster();
					}
					pz_emit_terrain_tris(vertices, uvMode, "terrain_quad_cluster_a", __FILE__, __LINE__);
				}

				//--------------------------------------------------------------------
				//Mine Information
				long rowCol = vertices[0]->posTile;
				long tileR = rowCol>>16;
				long tileC = rowCol & 0x0000ffff;
						
				if (GameMap)
				{
					mineResult.init();
					long cellPos = 0;
					for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
					{
						for (long cellC = 0; cellC < MAPCELL_DIM; cellC++,cellPos++) 
						{
							long actualCellRow = tileR * MAPCELL_DIM + cellR;
							long actualCellCol = tileC * MAPCELL_DIM + cellC;
							
							DWORD localResult = 0;
							if (GameMap->inBounds(actualCellRow, actualCellCol))
								localResult = GameMap->getMine(actualCellRow, actualCellCol);
								
							if (localResult == 1)
							{
								tex_resolve(mineTextureHandle);
								{ CostSplitDetailOverlayScope _csDetail;
								mcTextureManager->addTriangle(mineTextureHandle, MC2_DRAWALPHA);
								mcTextureManager->addTriangle(mineTextureHandle, MC2_DRAWALPHA);
								}
								mineResult.setMine(cellPos,localResult);
							}
							else if (localResult == 2)
							{
								tex_resolve(blownTextureHandle);
								{ CostSplitDetailOverlayScope _csDetail;
								mcTextureManager->addTriangle(blownTextureHandle, MC2_DRAWALPHA);
								mcTextureManager->addTriangle(blownTextureHandle, MC2_DRAWALPHA);
								}
								mineResult.setMine(cellPos,localResult);
							}
						}
					}
				}
			}
			else
			{
				terrainHandle = 0xffffffff; 
				waterHandle = 0xffffffff; 
				waterDetailHandle = 0xffffffff;
				terrainDetailHandle = 0xffffffff;
				overlayHandle = 0xffffffff;
			}
		}
		else
		{
			long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[3]->clipInfo;
			long clipped2 = vertices[1]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;

			if (clipped1 || clipped2)
			{
				{
					terrainHandle = Terrain::terrainTextures->getTextureHandle((vertices[0]->pVertex->textureData & 0x0000ffff));
					DWORD terrainDetailData = Terrain::terrainTextures->setDetail(1,0);
					if (terrainDetailData != 0xfffffff)
						terrainDetailHandle = Terrain::terrainTextures->getTextureHandle(terrainDetailData);
					else
						terrainDetailHandle = 0xffffffff;
					overlayHandle = 0xffffffff;

					// Cold-path SOLID gate-off (Stage 3, cluster C): under
					// MC2_MODERN_TERRAIN_PATCHES=0 only.
					if (BeginLegacySolidCluster()) {
					CostSplitSolidScope _csSolid;
					mcTextureManager->addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
					mcTextureManager->addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
					EndLegacySolidCluster();
					}
					pz_emit_terrain_tris(vertices, uvMode, "terrain_quad_cluster_c", __FILE__, __LINE__);
				}

				//--------------------------------------------------------------------
				//Mine Information
				long rowCol = vertices[0]->posTile;
				long tileR = rowCol>>16;
				long tileC = rowCol & 0x0000ffff;
						
				if (GameMap)
				{
					long cellPos = 0;
					mineResult.init();
					for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
					{
						for (long cellC = 0; cellC < MAPCELL_DIM; cellC++,cellPos++) 
						{
							long actualCellRow = tileR * MAPCELL_DIM + cellR;
							long actualCellCol = tileC * MAPCELL_DIM + cellC;
							
							DWORD localResult = 0;
							if (GameMap->inBounds(actualCellRow, actualCellCol))
								localResult = GameMap->getMine(actualCellRow, actualCellCol);
								
							if (localResult == 1)
							{
								tex_resolve(mineTextureHandle);
								{ CostSplitDetailOverlayScope _csDetail;
								mcTextureManager->addTriangle(mineTextureHandle,MC2_DRAWALPHA);
								mcTextureManager->addTriangle(mineTextureHandle,MC2_DRAWALPHA);
								}
								mineResult.setMine(cellPos,localResult);
							}
							else if (localResult == 2)
							{
								tex_resolve(blownTextureHandle);
								{ CostSplitDetailOverlayScope _csDetail;
								mcTextureManager->addTriangle(blownTextureHandle,MC2_DRAWALPHA);
								mcTextureManager->addTriangle(blownTextureHandle,MC2_DRAWALPHA);
								}
								mineResult.setMine(cellPos,localResult);
							}
						}
					}
				}
			}
			else
			{
				terrainHandle = 0xffffffff; 
				waterHandle = 0xffffffff; 
				waterDetailHandle = 0xffffffff;
				terrainDetailHandle = 0xffffffff;
				overlayHandle = 0xffffffff;
			}
		}
	}
	else		//New single bitmap on the terrain.
	{
		// 1A-alt Slice 0 follow-up #2 — measure isTerrainQuadVisible call cost.
		// At wolfman zoom counter inference suggests ~6K quads/frame take the
		// invisible branch — if the check is even ~500ns, that's 3ms/frame.
		bool quadVisible = isTerrainQuadVisible(*this);
		if (!quadVisible)
		{
			gos_terrain_indirect::Counters_AddShapeCInvisibleQuad();
			overlayHandle = 0xffffffff;
			terrainHandle = 0xffffffff;
			waterHandle = 0xffffffff;
			waterDetailHandle = 0xffffffff;
			terrainDetailHandle = 0xffffffff;
			return;
		}
		else
		{
			if (legacyTerrainNeeded) {
			// 1A-alt Slice 0 follow-up — bracket the cache-fetch + residency
			// check. Prime suspect for the missing ~8ms between Tracy outer
			// zone and sum of (recipe + water + lighting) buckets.
			long rowCol = vertices[0]->posTile;
			long tileR = rowCol >> 16;
			long tileC = rowCol & 0x0000ffff;
			const MapData::WorldQuadTerrainCacheEntry* cachedEntry = NULL;
			{
				CostSplitCacheResidentScope _csRes;
				cachedEntry = Terrain::mapData ? Terrain::mapData->getTerrainFaceCacheEntry(tileR, tileC) : NULL;
				if (cachedEntry && cachedEntry->isValid())
				{
					Terrain::mapData->ensureTerrainFaceCacheEntryResident(*cachedEntry, false);
				}
			}

			{
				CostSplitRecipeCacheScope _csRec; // 1A-alt Slice 0 — Shape-C recipe lookup + member assigns + addTerrainTriangles bucket
				TerrainRecipe recipe;
				TerrainRecipe inlineRecipe;

				// Always build inline recipe when parity check is on -- keeps comparison
				// meaningful even after the cache path is enabled in Task 5.
				if (s_shapeCParityCheck)
					buildTerrainRecipeInline(vertices, uvMode, inlineRecipe);

				if (!tryGetCachedTerrainRecipe(cachedEntry, recipe))
					buildTerrainRecipeInline(vertices, uvMode, recipe);

				// Compare inline vs cache entry (not recipe -- recipe may now be cache-sourced).
				if (s_shapeCParityCheck && cachedEntry && cachedEntry->isValid())
					shapeC_checkParity(inlineRecipe, *cachedEntry, tileR, tileC, uvMode);

				// Assign member vars inside the member function.
				isCement            = recipe.isCement;
				terrainHandle       = recipe.terrainHandle;
				terrainDetailHandle = recipe.terrainDetailHandle;
				overlayHandle       = recipe.overlayHandle;
				uvData              = recipe.uvData;

				// Register triangle batches (no pz_emit here).
				addTerrainTriangles(recipe);

				// pz_emit stays at this callsite to preserve projectZ callsite identity.
				if (!recipe.isCement && recipe.terrainHandle != 0)
					pz_emit_terrain_tris(vertices, uvMode,
						(uvMode == BOTTOMRIGHT) ? "terrain_quad_cluster_a" : "terrain_quad_cluster_c",
						__FILE__, __LINE__);
			}
			} // legacyTerrainNeeded

			{
				// PR2c Stage 2c — gate off legacy enqueueTerrainMineState when
				// the static-bake path is armed. Indirect path owns mine state
				// for this frame; legacy reservation is unnecessary work.
				//
				// Terrain LOD chunk Phase 7A: also suppress when
				// MC2_TERRAIN_LOD_CHUNK=1 — the new standalone mine enqueue
				// (TerrainQuad::enqueueMinesFromGrid, called from
				// Terrain::render()) owns mine state for the LOD path.
				// Both guards are necessary: IsFrameMineArmed() handles the
				// GPU static-bake retirement; the LOD flag handles the
				// setupTextures-bypass retirement. Neither subsumes the other.
				static const bool s_lodChunkActive =
					(mc2TerrainLodChunkEnabled());
				if (!gos_terrain_indirect::IsFrameMineArmed()
				    && !s_lodChunkActive) {
					enqueueTerrainMineState(*this);
				}
			}
		}
	}

	//-----------------------------------------
	// NEW(tm) water texture code here.
	// 1A-alt Slice 0 — bracket the entire water-elevation projection + water-handle
	// resolution + addTriangleBulk(water/waterDetail) chain. Times both the
	// if-path (per-vertex projectForTerrainAdmission ×4 + reductions) and the
	// else-path (handle reset). Closing brace at end of else clause below.
	{
	CostSplitWaterVertProjScope _csWvp;
	if ((vertices[0]->pVertex->water & 1) ||
		(vertices[1]->pVertex->water & 1) ||
		(vertices[2]->pVertex->water & 1) ||
		(vertices[3]->pVertex->water & 1))
	{
		Stuff::Vector3D vertex3D(vertices[0]->vx,vertices[0]->vy,Terrain::waterElevation);
		Stuff::Vector4D screenPos;

		long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[2]->clipInfo;
		long clipped2 = vertices[0]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;

		if (uvMode != BOTTOMRIGHT)
		{
			clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[3]->clipInfo;
			clipped2 = vertices[1]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
		}

 		float negCos = Terrain::frameCos * -1.0;
		float ourCos = Terrain::frameCos;
		// Phase 3 water guard: frame-stamp path (MC2_TERRAIN_LOD_CHUNK) or
		// bit-flag path (legacy). Old path 100% intact when env is unset.
		auto calcWaterVert0 = [&]() {
			if (clipped1 || clipped2)
			{
				if (vertices[0]->pVertex->water & 128)
				{
					ourCos = negCos;
				}
				else if (vertices[0]->pVertex->water & 64)
				{
					ourCos = Terrain::frameCos;
				}

				vertex3D.z = ourCos + Terrain::waterElevation;

				bool clipData = false;
				// [PROJECTZ:BoolAdmission id=terrain_quad_vert0_admit]
				PROJECTZ_SITE("terrain_quad_vert0_admit", "BoolAdmission");
				// F6 T2: Modern path uses cached frustum planes (shared with
				// slimReduce; populated once per frame in Terrain::geometry).
				// legacyWaterDraw=true (rare; fast path NOT armed): still call
				// projectForTerrainAdmission so wx/wy/wz/ww have valid values;
				// clipData stays from frustum test.
				if (mc2_terrain_admission::isModern()) {
					clipData = eye->quadAabbInFrustum(eye->getCachedFrustumPlanes(), vertex3D, vertex3D);
					if (legacyWaterDraw) {
						eye->projectForTerrainAdmission(vertex3D, screenPos);
					}
				} else {
					clipData = eye->projectForTerrainAdmission(vertex3D,screenPos);
				}
				pz_capture_vert_preds(0);
				bool isVisible = Terrain::IsGameSelectTerrainPosition(vertex3D) || drawTerrainGrid;
				if (!isVisible)
				{
					clipData = false;
				}

				if (eye->usePerspective)
					vertices[0]->clipInfo = clipData;
				else
					vertices[0]->clipInfo = clipData;

				if (legacyWaterDraw)
				{
					vertices[0]->wx = screenPos.x;
					vertices[0]->wy = screenPos.y;
					vertices[0]->wz = screenPos.z;
					vertices[0]->ww = screenPos.w;
				}
			}
		};
		if (useLodChunkWaterGuard) {
			if (vertices[0]->lastWaterFrame != Terrain::gCurrentFrame) {
				calcWaterVert0();
				vertices[0]->lastWaterFrame = Terrain::gCurrentFrame;
			}
		} else {
			if (!(vertices[0]->calcThisFrame & 2)) {
				calcWaterVert0();
				vertices[0]->calcThisFrame |= 2;
			}
		}

		auto calcWaterVert1 = [&]() {
			if (clipped1 || clipped2)
			{
				if (vertices[1]->pVertex->water & 128)
				{
					ourCos = negCos;
				}
				else if (vertices[1]->pVertex->water & 64)
				{
					ourCos = Terrain::frameCos;
				}

				vertex3D.z = ourCos + Terrain::waterElevation;
				vertex3D.x = vertices[1]->vx;
				vertex3D.y = vertices[1]->vy;

				bool clipData = false;
				// [PROJECTZ:BoolAdmission id=terrain_quad_vert1_admit]
				PROJECTZ_SITE("terrain_quad_vert1_admit", "BoolAdmission");
				// F6 T2: see vert0 comment above.
				if (mc2_terrain_admission::isModern()) {
					clipData = eye->quadAabbInFrustum(eye->getCachedFrustumPlanes(), vertex3D, vertex3D);
					if (legacyWaterDraw) {
						eye->projectForTerrainAdmission(vertex3D, screenPos);
					}
				} else {
					clipData = eye->projectForTerrainAdmission(vertex3D,screenPos);
				}
				pz_capture_vert_preds(1);
				bool isVisible = Terrain::IsGameSelectTerrainPosition(vertex3D) || drawTerrainGrid;
				if (!isVisible)
				{
					clipData = false;
				}

				if (eye->usePerspective)
					vertices[1]->clipInfo = clipData; //onScreen;
				else
					vertices[1]->clipInfo = clipData;

				if (legacyWaterDraw)
				{
					vertices[1]->wx = screenPos.x;
					vertices[1]->wy = screenPos.y;
					vertices[1]->wz = screenPos.z;
					vertices[1]->ww = screenPos.w;
				}
			}
		};
		if (useLodChunkWaterGuard) {
			if (vertices[1]->lastWaterFrame != Terrain::gCurrentFrame) {
				calcWaterVert1();
				vertices[1]->lastWaterFrame = Terrain::gCurrentFrame;
			}
		} else {
			if (!(vertices[1]->calcThisFrame & 2)) {
				calcWaterVert1();
				vertices[1]->calcThisFrame |= 2;
			}
		}

		auto calcWaterVert2 = [&]() {
			if (clipped1 || clipped2)
			{
				if (vertices[2]->pVertex->water & 128)
				{
					ourCos = negCos;
				}
				else if (vertices[2]->pVertex->water & 64)
				{
					ourCos = Terrain::frameCos;
				}

				vertex3D.z = ourCos + Terrain::waterElevation;
				vertex3D.x = vertices[2]->vx;
				vertex3D.y = vertices[2]->vy;

				bool clipData = false;
				// [PROJECTZ:BoolAdmission id=terrain_quad_vert2_admit]
				PROJECTZ_SITE("terrain_quad_vert2_admit", "BoolAdmission");
				// F6 T2: see vert0 comment above.
				if (mc2_terrain_admission::isModern()) {
					clipData = eye->quadAabbInFrustum(eye->getCachedFrustumPlanes(), vertex3D, vertex3D);
					if (legacyWaterDraw) {
						eye->projectForTerrainAdmission(vertex3D, screenPos);
					}
				} else {
					clipData = eye->projectForTerrainAdmission(vertex3D,screenPos);
				}
				pz_capture_vert_preds(2);
				bool isVisible = Terrain::IsGameSelectTerrainPosition(vertex3D) || drawTerrainGrid;
				if (!isVisible)
				{
					clipData = false;
				}

				if (eye->usePerspective)
					vertices[2]->clipInfo = clipData; //onScreen;
				else
					vertices[2]->clipInfo = clipData;

				if (legacyWaterDraw)
				{
					vertices[2]->wx = screenPos.x;
					vertices[2]->wy = screenPos.y;
					vertices[2]->wz = screenPos.z;
					vertices[2]->ww = screenPos.w;
				}
			}
		};
		if (useLodChunkWaterGuard) {
			if (vertices[2]->lastWaterFrame != Terrain::gCurrentFrame) {
				calcWaterVert2();
				vertices[2]->lastWaterFrame = Terrain::gCurrentFrame;
			}
		} else {
			if (!(vertices[2]->calcThisFrame & 2)) {
				calcWaterVert2();
				vertices[2]->calcThisFrame |= 2;
			}
		}

		auto calcWaterVert3 = [&]() {
			if (clipped1 || clipped2)
			{
				if (vertices[3]->pVertex->water & 128)
				{
					ourCos = negCos;
				}
				else if (vertices[3]->pVertex->water & 64)
				{
					ourCos = Terrain::frameCos;
				}

				vertex3D.z = ourCos + Terrain::waterElevation;
				vertex3D.x = vertices[3]->vx;
				vertex3D.y = vertices[3]->vy;

				bool clipData = false;
				// [PROJECTZ:BoolAdmission id=terrain_quad_vert3_admit]
				PROJECTZ_SITE("terrain_quad_vert3_admit", "BoolAdmission");
				// F6 T2: see vert0 comment above.
				if (mc2_terrain_admission::isModern()) {
					clipData = eye->quadAabbInFrustum(eye->getCachedFrustumPlanes(), vertex3D, vertex3D);
					if (legacyWaterDraw) {
						eye->projectForTerrainAdmission(vertex3D, screenPos);
					}
				} else {
					clipData = eye->projectForTerrainAdmission(vertex3D,screenPos);
				}
				pz_capture_vert_preds(3);
				bool isVisible = Terrain::IsGameSelectTerrainPosition(vertex3D) || drawTerrainGrid;
				if (!isVisible)
				{
					clipData = false;
				}

				if (eye->usePerspective)
					vertices[3]->clipInfo = clipData; //onScreen;
				else
					vertices[3]->clipInfo = clipData;

				if (legacyWaterDraw)
				{
					vertices[3]->wx = screenPos.x;
					vertices[3]->wy = screenPos.y;
					vertices[3]->wz = screenPos.z;
					vertices[3]->ww = screenPos.w;
				}
			}
		};
		if (useLodChunkWaterGuard) {
			if (vertices[3]->lastWaterFrame != Terrain::gCurrentFrame) {
				calcWaterVert3();
				vertices[3]->lastWaterFrame = Terrain::gCurrentFrame;
			}
		} else {
			if (!(vertices[3]->calcThisFrame & 2)) {
				calcWaterVert3();
				vertices[3]->calcThisFrame |= 2;
			}
		}

		if (clipped1 || clipped2)
		{
			if (legacyWaterDraw)
			{
				if (!Terrain::terrainTextures2)
				{
					DWORD waterDetailData = Terrain::terrainTextures->setDetail(0,sprayFrame);
					waterHandle = Terrain::terrainTextures->getTextureHandle(MapData::WaterTXMData & 0x0000ffff);
					waterDetailHandle = Terrain::terrainTextures->getDetailHandle(waterDetailData & 0x0000ffff);
				}
				else
				{
					waterHandle = Terrain::terrainTextures2->getWaterTextureHandle();
					waterDetailHandle = Terrain::terrainTextures2->getWaterDetailHandle(sprayFrame);
				}

				mcTextureManager->addTriangleBulk(waterHandle, MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATER, 2);
				mcTextureManager->addTriangleBulk(waterDetailHandle, MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATERDETAIL, 2);
			}
		}
		else
		{
			waterHandle = 0xffffffff;
			waterDetailHandle = 0xffffffff;
		}
	}
	else
	{
		waterHandle = 0xffffffff;
		waterDetailHandle = 0xffffffff;
	}
	} // close CostSplitWaterVertProjScope (1A-alt Slice 0)
}
#endif // MC2_IS_EDITOR (8z-B: setupTextures gated to editor build)

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
#include "terrain_depth_bias.h"
using mc2depth::TERRAIN_DEPTH_FUDGE;
using mc2depth::WATER_DEPTH_FUDGE;
#define OVERLAY_ELEV_OFFSET		0.15f

// GPU projection: pack MC2 world coords into overlay vertex instead of screen-space
static inline void setOverlayWorldCoords(gos_VERTEX& v, const VertexPtr src) {
	v.x = src->vx;
	v.y = src->vy;
	v.z = src->pVertex->elevation + OVERLAY_ELEV_OFFSET;
	v.rhw = 1.0f;
}

//---------------------------------------------------------------------------
void TerrainQuad::draw (void)
{
	if (terrainHandle != 0xffffffff)
	{
		numTerrainFaces++;

		// M2b: pure-water early-exit. Quads with no base terrain texture, no overlay,
		// and no water-interest detail have nothing to emit here — addVertices, appendQuad,
		// the M2 fast-path emit, and overlay/detail draws are all gated on these
		// conditions. Water surface is rendered separately by Terrain::renderWater().
		// On water-heavy maps (mc2_01 ~75% water) this skips ~28K wasted iterations/frame.
		// Most pure-water quads are loop-hoisted in Terrain::render (terrain.cpp); this
		// is the in-function fallback for when the global useOverlayTexture /
		// useWaterInterestTexture flags get toggled and the loop check no longer matches.
		if (terrainHandle == 0
		    && !(useOverlayTexture && overlayHandle != 0xffffffff)
		    && !(useWaterInterestTexture && terrainDetailHandle != 0xffffffff))
		{
		    return;
		}

		//---------------------------------------
		// GOS 3D draw Calls now!

		// M2b instrumentation: pre-branch setup zone. Wraps gVertex decl, minU/maxU
		// compute, optional uvData read, camPosition load, fastPathEligible 7-flag
		// computation (4 PatchStream method calls), and the path_mix counter. This
		// runs for every quad that didn't pure-water early-exit. Closes at end of
		// the outer-if scope, so it INCLUDES the fast-path and legacy bodies — but
		// FP.* and Quad.legacy zones are children, so Tracy will show preBranch self
		// time = (preBranch total) - (FP.* + Quad.legacy + Legacy.*).
		gos_VERTEX gVertex[3];

        // sebi half pixel, hope this is correct :-)
		float minU = 0.5f / TERRAIN_TXM_SIZE;
		float maxU = 1.0f - 0.5f / TERRAIN_TXM_SIZE;
		float minV = 0.5f / TERRAIN_TXM_SIZE;
		float maxV = 1.0f - 0.5f / TERRAIN_TXM_SIZE;
		float oldminU = 0.0078125f;
		float oldmaxU = 0.9921875f;
		float oldminV = 0.0078125f;
		float oldmaxV = 0.9921875f;

		// OVERLAY-TILE-HIRES-1 (gated, default OFF => constants above unchanged).
		// oldmin/oldmax feed ONLY the overlay tile emits in this function and are
		// the half-texel inset for a 64px tile (0.5/64). A resynced hi-res tile
		// sampled with the 64px inset crops its outer texel ring (2 texels at
		// 256) and breaks road continuity across tile seams, so derive the inset
		// from the actual overlay texture edge (physical = logical * uvScale;
		// disk-loaded 4x-upscale TGAs store logical = physical/4).
		if (MC2_OverlayTileHiresSize() > 0 && overlayHandle != 0xffffffff)
		{
			DWORD ovLogicalW = 0, ovLogicalH = 0;
			if (mcTextureManager->tryGetTextureLogicalSize(overlayHandle, ovLogicalW, ovLogicalH))
			{
				const DWORD ovEdge = ovLogicalW * mcTextureManager->getUVScale(overlayHandle);
				if (ovEdge > 0)
				{
					oldminU = oldminV = 0.5f / (float)ovEdge;
					oldmaxU = oldmaxV = 1.0f - 0.5f / (float)ovEdge;
				}
			}
		}

		if (Terrain::terrainTextures2 && !(overlayHandle == 0xffffffff && isCement))
		{
			minU = uvData.minU;
			minV = uvData.minV;
			maxU = uvData.maxU;
			maxV = uvData.maxV;
		}

		Stuff::Point3D camPosition;
		camPosition = *TG_Shape::s_cameraOrigin;

		// === M2 fast-path eligibility ===
		// Quads enter the fast path when:
		//   (a) terrainHandle != 0           → emit thin record + optional detail + optional overlay
		//   (b) terrainHandle == 0 + detail  → emit detail only (+ optional overlay), no thin record
		//   (c) terrainHandle == 0 + overlay → emit overlay only, no thin record, no detail
		// M2d landed: overlay-only quads are now fast-path eligible; the inline overlay
		// emit below mirrors the legacy gos_PushTerrainOverlay sites (base/detail blocks
		// in legacy continue to exist as the fallback when FASTPATH env is off).
		// Per-gate flags retained so the path_mix diagnostic counter below can
		// attribute fastPathEligible failures to the specific gate (s_failOverlay
		// should read 0 post-M2d — leaving it in serves as the validation signal).
		const bool g_active   = TerrainPatchStream::isFastPathActive();
		const bool g_thin     = TerrainPatchStream::isThinRecordsActive();
		const bool g_ready    = TerrainPatchStream::isReady();
		const bool g_notOver  = !TerrainPatchStream::isOverflowed();
		const bool g_handle   = (terrainHandle != 0);
		const bool g_noOverlay= !(useOverlayTexture && overlayHandle != 0xffffffff);
		const bool g_noWater  = !(useWaterInterestTexture && terrainDetailHandle != 0xffffffff);
		const bool fastPathEligible =
		    g_active && g_thin && g_ready && g_notOver
		    && (g_handle || !g_noWater || !g_noOverlay);

		// MC2_THIN_DEBUG=1: per-gate fail counts to pinpoint which condition is culling
		// most quads from the fast path. Demoted to silent-by-default per CLAUDE.md
		// debug-instrumentation rule. Holds off until ~frame 1200 to skip warmup load
		// and capture steady-state. Prints 5 frames then stops; per-quad cost is one
		// branch on s_framesPrinted after that (effectively zero).
		{
		    static const bool s_pathMixOn = (getenv("MC2_THIN_DEBUG") != nullptr);
		    static uint32_t s_fast = 0, s_legacy = 0;
		    static uint32_t s_failActive = 0, s_failThin = 0, s_failReady = 0, s_failOver = 0;
		    static uint32_t s_failHandle = 0, s_failOverlay = 0, s_failWater = 0;
		    static uint32_t s_framesPrinted = 0;
		    static uint32_t s_quadsThisFrame = 0;
		    static uint32_t s_frameCounter = 0;
		    constexpr uint32_t kWarmupHoldoffFrames = 1200;  // ~15s @ 80fps
		    if (s_pathMixOn && s_framesPrinted < 5) {
		        const bool warmedUp = (s_frameCounter >= kWarmupHoldoffFrames);
		        if (warmedUp) {
		            if (fastPathEligible) ++s_fast;
		            else {
		                ++s_legacy;
		                if      (!g_active)    ++s_failActive;
		                else if (!g_thin)      ++s_failThin;
		                else if (!g_ready)     ++s_failReady;
		                else if (!g_notOver)   ++s_failOver;
		                else if (!g_handle)    ++s_failHandle;
		                else if (!g_noOverlay) ++s_failOverlay;
		                else if (!g_noWater)   ++s_failWater;
		            }
		        }
		        if (++s_quadsThisFrame >= 14000) {
		            ++s_frameCounter;
		            if (warmedUp) {
		                fprintf(stderr,
		                    "[THIN_DEBUG v1] event=path_mix frame=%u (post-warmup) fast=%u legacy=%u "
		                    "fail_active=%u fail_thin=%u fail_ready=%u fail_over=%u "
		                    "fail_handle0=%u fail_overlay=%u fail_water=%u\n",
		                    s_framesPrinted, s_fast, s_legacy,
		                    s_failActive, s_failThin, s_failReady, s_failOver,
		                    s_failHandle, s_failOverlay, s_failWater);
		                fflush(stderr);
		                s_fast = s_legacy = 0;
		                s_failActive = s_failThin = s_failReady = s_failOver = 0;
		                s_failHandle = s_failOverlay = s_failWater = 0;
		                ++s_framesPrinted;
		            }
		            s_quadsThisFrame = 0;
		        }
		    }
		}

		if (fastPathEligible)
		{
		    // pz-validity gate. v2-scoping: only the M2d overlay (live-when-armed)
		    // and the not-armed thin-emit consume pzc/pzTri; gate the (re-)compute
		    // behind the union of their producer predicates so the cost is bounded
		    // to the sparse overlay-quad set (NOT every terrain quad — the v1
		    // 146->42 regression was unconditional re-projection here).
		    // v1-mechanism: re-project on-site from the SAME (vx,vy,elevation)
		    // triple VPL used, and reproduce VPL's pz=-0.5 off-screen sentinel via
		    // the clipInfo==0 short-circuit (v1 Finding 1, proven bit-exact). This
		    // makes the gate INDEPENDENT of vertices[c]->pz so Step 8b can delete
		    // the VPL cv->pz write (terrain.cpp:1601 / :1738).
		    // Source of truth: docs/superpowers/reviews/
		    //   2026-05-15-overlay-pz-scoped-redesign-rereview.md Finding 1
		    //   §Corrected edit.
		    bool pzc[4] = { false, false, false, false };
		    const bool pzNeeded =
		        (useOverlayTexture && overlayHandle != 0xffffffff)
		        || (terrainHandle != 0 && !gos_terrain_indirect::IsFrameSolidArmed());
		    if (pzNeeded)
		    {
		        for (int c = 0; c < 4; c++) {
		            // VPL writes pz=-0.5 (sentinel) iff clipInfo==0 (onScreen==false)
		            // for the perspective renderer (terrain.cpp:1577/1601/1603,
		            // :1749/1759). Reproduce that co-decision; do NOT re-project the
		            // sentinel corners (re-projection of an off-cone corner whose true
		            // projectZ z is still in [0,1) would flip pzc false->true — the
		            // divergence v1 Finding 1 closes).
		            if (vertices[c]->clipInfo == 0) { pzc[c] = false; continue; }
		            Stuff::Vector3D ov3D(vertices[c]->vx, vertices[c]->vy,
		                                 vertices[c]->pVertex->elevation);
		            Stuff::Vector4D osp;
		            eye->projectForTerrainAdmission(ov3D, osp);
		            float pz_adj = osp.z + TERRAIN_DEPTH_FUDGE;
		            pzc[c] = (pz_adj >= 0.0f) && (pz_adj < 1.0f);
		        }
		    }

		    bool pzTri1, pzTri2;
		    if (uvMode == BOTTOMLEFT) {
		        pzTri1 = pzc[0] && pzc[1] && pzc[3];
		        pzTri2 = pzc[1] && pzc[2] && pzc[3];
		    } else {
		        pzTri1 = pzc[0] && pzc[1] && pzc[2];
		        pzTri2 = pzc[0] && pzc[2] && pzc[3];
		    }

		    // MC2_M2D_PZ_PARITY probe (Step 8b precursor validator, demote-don't-
		    // delete). Bit-compares the corrected reprojection+sentinel result
		    // above against the literal vertices[c]->pz formula in a PROBE-ONLY
		    // local (legal because terrain.cpp still writes cv->pz pre-Step-8b).
		    // Per 2026-05-15-overlay-pz-scoped-redesign-rereview.md Finding 5:
		    // static getenv arm (NO Renderer!=3 arm-guard), one-shot Renderer!=3
		    // scope-assertion log, first-N unconditional mismatch emit, running
		    // summary with checked>0 (vacuous-probe guard). pzc_old does NOT feed
		    // the production path; it exists solely for this comparison.
		    if (s_m2dPzParity)
		    {
		        if (!s_m2dPzParityArmLogged) {
		            s_m2dPzParityArmLogged = true;
		            fprintf(stderr,
		                "[M2D_PZ_PARITY v1] event=armed mechanism=reproject+clipInfo_guard "
		                "scope_assert=Renderer!=3 Environment.Renderer=%d emit_cap=%d "
		                "note=probe_only_cv_pz_read_pre_step8b\n",
		                (int)Environment.Renderer, s_m2dPzParityEmitCap);
		            fflush(stderr);
		        }
		        bool pzc_old[4];
		        for (int c = 0; c < 4; c++) {
		            float pz_adj_old = vertices[c]->pz + TERRAIN_DEPTH_FUDGE;
		            pzc_old[c] = (pz_adj_old >= 0.0f) && (pz_adj_old < 1.0f);
		        }
		        bool pzTri1_old, pzTri2_old;
		        if (uvMode == BOTTOMLEFT) {
		            pzTri1_old = pzc_old[0] && pzc_old[1] && pzc_old[3];
		            pzTri2_old = pzc_old[1] && pzc_old[2] && pzc_old[3];
		        } else {
		            pzTri1_old = pzc_old[0] && pzc_old[1] && pzc_old[2];
		            pzTri2_old = pzc_old[0] && pzc_old[2] && pzc_old[3];
		        }
		        ++s_m2dPzParityChecked;
		        const bool mism =
		            (pzc[0] != pzc_old[0]) || (pzc[1] != pzc_old[1]) ||
		            (pzc[2] != pzc_old[2]) || (pzc[3] != pzc_old[3]) ||
		            (pzTri1 != pzTri1_old) || (pzTri2 != pzTri2_old);
		        if (mism) {
		            ++s_m2dPzParityMismatch;
		            if (s_m2dPzParityEmitted < s_m2dPzParityEmitCap) {
		                ++s_m2dPzParityEmitted;
		                fprintf(stderr,
		                    "[M2D_PZ_PARITY v1] event=mismatch n=%d "
		                    "pzc_new=%d%d%d%d pzc_old=%d%d%d%d "
		                    "pzTri_new=%d%d pzTri_old=%d%d pzNeeded=%d uvMode=%d\n",
		                    s_m2dPzParityEmitted,
		                    pzc[0], pzc[1], pzc[2], pzc[3],
		                    pzc_old[0], pzc_old[1], pzc_old[2], pzc_old[3],
		                    pzTri1, pzTri2, pzTri1_old, pzTri2_old,
		                    pzNeeded ? 1 : 0, (int)uvMode);
		                fflush(stderr);
		            }
		        }
		        if ((s_m2dPzParityChecked % 14000) == 0) {
		            fprintf(stderr,
		                "[M2D_PZ_PARITY v1] event=summary checked=%d mismatches=%d\n",
		                s_m2dPzParityChecked, s_m2dPzParityMismatch);
		            fflush(stderr);
		        }
		    }

		    if (!pzTri1 && !pzTri2) return;  // both culled — skip entirely

		    // === Thin-record emit path (terrainHandle != 0 only) ===
		    // Detail-only quads (terrainHandle == 0 + has detail) skip this entire block
		    // and go straight to the M2c detail emit below. The thin record is a base-
		    // texture submission; quads without a base texture have nothing to put in it.
		    //
		    // Step 1b-1 gate-split: skip the entire thin-emit scope (recipe
		    // peek/build via ensureRecipeForQuad + the TerrainQuadThinRecord
		    // build + appendThinRecordDirect) when GPU SOLID is armed. Post
		    // Fix-B the compute shader is the sole projection authority and
		    // re-derives this record (incl. the pzTri1/pzTri2 flag bits) for
		    // every admitted quad, so the CPU emit is dead work whose output
		    // the GPU overwrites. NOTE: pz/pzTri compute, the
		    // `if (!pzTri1 && !pzTri2) return;` cull, and the M2d
		    // gos_PushTerrainOverlay block below stay UNCONDITIONAL — they
		    // feed the live decal producer (§2 WARNING, 9964d5a
		    // conflated-overlay regression class). The `goto fp_detail_only`
		    // inside this block jumps to a label AFTER it, so gating the
		    // block leaves the SSBO-full detail fallthrough intact.
		    const bool thinEmitArmed = gos_terrain_indirect::IsFrameSolidArmed();
		    if (s_thinEmitTrace) {
		        static bool s_loggedThinEmit = false;
		        if (!s_loggedThinEmit) {
		            s_loggedThinEmit = true;
		            printf("[TERRAIN_INDIRECT v1] event=cpu_thinemit_demoted "
		                   "path=quad_m2 armed=%d gate=IsFrameSolidArmed "
		                   "note=overlay_producer_unconditional\n",
		                   thinEmitArmed ? 1 : 0);
		            fflush(stdout);
		        }
		    }
		    if (terrainHandle != 0 && !thinEmitArmed)
		    {
		        const bool isCement = Terrain::terrainTextures->isCement(vertices[0]->pVertex->textureData & 0x0000ffff);
		        const bool isAlpha  = Terrain::terrainTextures->isAlpha(vertices[0]->pVertex->textureData & 0x0000ffff);
		        const bool alphaOverride = Terrain::terrainTextures2 && (!isCement || isAlpha);

		        // effectiveLightRGB: alphaOverride first, then selected (matching legacy priority)
		        auto lightRGBc = [&](int c) -> uint32_t {
		            DWORD lc = vertices[c]->lightRGB;
		            if (alphaOverride) lc = 0xffffffffu;
		            if (vertices[c]->pVertex->selected) lc = static_cast<DWORD>(SELECTION_COLOR);
		            return static_cast<uint32_t>(lc);
		        };

		        // Lazy recipe build: peek the cache first using only corner-0 (wx0, wy0).
		        // ~99% of quads hit the cache after warmup; building the full recipe (~20
		        // cold reads through vertices[c]->pVertex->{elevation, vertexNormal,
		        // terrainType}, ~30 stack stores, 4× terrainTypeToMaterial, bit-pack +
		        // memcpy) on a hit is pure waste — ensureRecipeForQuad discards `recipe`
		        // on hit. Build it only on miss.
		        const float wx0 = vertices[0]->vx;
		        const float wy0 = vertices[0]->vy;
		        const uint64_t recipeKey = TerrainPatchStream::makeRecipeKey(wx0, wy0);
		        uint32_t recipeIdx = TerrainPatchStream::tryGetRecipeIdx(recipeKey);
		        if (recipeIdx == UINT32_MAX)
		        {
		            TerrainQuadRecipe recipe;
		            recipe.wx0=wx0;             recipe.wy0=wy0;             recipe.wz0=vertices[0]->pVertex->elevation; recipe._wp0=0.f;
		            recipe.wx1=vertices[1]->vx; recipe.wy1=vertices[1]->vy; recipe.wz1=vertices[1]->pVertex->elevation; recipe._wp1=0.f;
		            recipe.wx2=vertices[2]->vx; recipe.wy2=vertices[2]->vy; recipe.wz2=vertices[2]->pVertex->elevation; recipe._wp2=0.f;
		            recipe.wx3=vertices[3]->vx; recipe.wy3=vertices[3]->vy; recipe.wz3=vertices[3]->pVertex->elevation; recipe._wp3=0.f;
		            recipe.nx0=vertices[0]->pVertex->vertexNormal.x; recipe.ny0=vertices[0]->pVertex->vertexNormal.y; recipe.nz0=vertices[0]->pVertex->vertexNormal.z; recipe._np0=0.f;
		            recipe.nx1=vertices[1]->pVertex->vertexNormal.x; recipe.ny1=vertices[1]->pVertex->vertexNormal.y; recipe.nz1=vertices[1]->pVertex->vertexNormal.z; recipe._np1=0.f;
		            recipe.nx2=vertices[2]->pVertex->vertexNormal.x; recipe.ny2=vertices[2]->pVertex->vertexNormal.y; recipe.nz2=vertices[2]->pVertex->vertexNormal.z; recipe._np2=0.f;
		            recipe.nx3=vertices[3]->pVertex->vertexNormal.x; recipe.ny3=vertices[3]->pVertex->vertexNormal.y; recipe.nz3=vertices[3]->pVertex->vertexNormal.z; recipe._np3=0.f;
		            recipe.minU=minU; recipe.minV=minV; recipe.maxU=maxU; recipe.maxV=maxV;

		            // Pack 4 corner material types into _wp0 — bit-preserving write; shader reads via floatBitsToUint
		            {
		                uint32_t m0 = terrainTypeToMaterial(vertices[0]->pVertex->terrainType);
		                uint32_t m1 = terrainTypeToMaterial(vertices[1]->pVertex->terrainType);
		                uint32_t m2 = terrainTypeToMaterial(vertices[2]->pVertex->terrainType);
		                uint32_t m3 = terrainTypeToMaterial(vertices[3]->pVertex->terrainType);
		                uint32_t tpacked = m0 | (m1 << 8) | (m2 << 16) | (m3 << 24);
		                memcpy(&recipe._wp0, &tpacked, 4);
		            }

		            recipeIdx = TerrainPatchStream::ensureRecipeForQuad(recipeKey, recipe);
		            if (recipeIdx == UINT32_MAX) goto fp_detail_only;  // SSBO full — try detail still
		        }

		        {
		            TerrainQuadThinRecord tr;
		            tr.recipeIdx     = recipeIdx;
		            tr.terrainHandle = static_cast<uint32_t>(terrainHandle);
		            tr.flags         = (uvMode == BOTTOMLEFT ? 1u : 0u)
		                             | (pzTri1 ? 2u : 0u)
		                             | (pzTri2 ? 4u : 0u);
		            tr.cementWord    = 0u;  // Fix B rename (was _pad0).  Patch-stream
		                                    // path doesn't carry cement bits.
		            tr.lightRGB0     = lightRGBc(0);
		            tr.lightRGB1     = lightRGBc(1);
		            tr.lightRGB2     = lightRGBc(2);
		            tr.lightRGB3     = lightRGBc(3);
		            // Fix B 2026-05-14: clipPos zeroed (patch-stream uses its own VS,
		            // not the indirect thin VS that consumes clipPos).
		            for (int k = 0; k < 16; ++k) tr.clipPos[k] = 0.0f;
		            TerrainPatchStream::appendThinRecordDirect(tr);
		        }
		    }
		    fp_detail_only:;

		    // PR2a Stage 1a — M2c detail emit DELETED 2026-05-08.
		    //
		    // Was: an `if (useWaterInterestTexture && terrainDetailHandle != 0xffffffff)`
		    // block (~64 lines) that built per-corner gos_VERTEX arrays + per-tri
		    // clampUVs lambda + per-tri addVertices calls into the masterVertexNodes
		    // queue with flags `MC2_ISTERRAIN | MC2_DRAWALPHA`.
		    //
		    // Two reasons it was retired:
		    //   1. ZERO PIXEL OUTPUT since 521d83a (2026-04-16, "Fix terrain underlayer
		    //      artifact"). Render.Overlays at txmmgr.cpp:1818-1822 unconditionally
		    //      resets-and-skips any non-water alpha+terrain node — detail nodes
		    //      have flags MC2_ISTERRAIN|MC2_DRAWALPHA (no water flag) so they hit
		    //      the suppress predicate. The flat-plane detail layer was suppressed
		    //      because tessellated terrain elevation made it visible as a "dark
		    //      striped under-pattern."
		    //   2. ZERO CPU EXECUTION across tier1 (verified 2026-05-08 Stage 0a,
		    //      m2c_detail_emit_quads=0 on mc2_01/03/10/17). Code drift since the
		    //      M2 thin-record landing eliminated the runtime path even though
		    //      the brainstorm cited m2_thin_record_cpu_reduction_results.md's
		    //      "~5,800 quads/frame on mc2_01" — that perf number is stale.
		    //
		    // Future "tessellation-aware detail layer" feature is independent scope
		    // (see recon-6 "Open follow-ups" #4); brainstorm separately if/when it
		    // ships. Don't resurrect this block.

		    // M2d: per-quad base-texture overlay emit. Replaces the four legacy
		    // gos_PushTerrainOverlay sites further down in this function (TOPRIGHT tri1+tri2,
		    // BOTTOMLEFT tri1+tri2) — same output, but builds WorldOverlayVert straight from
		    // vertices[c] instead of memcpy'ing gVertex through oVertex and calling
		    // setOverlayWorldCoords. wz includes OVERLAY_ELEV_OFFSET (0.15f) to
		    // prevent z-fighting against the base terrain — matches setOverlayWorldCoords.
		    // ARGB and fog use raw vertices[c]->lightRGB/fogRGB — NO `selected` override and
		    // NO `terrainTextures2` whiteout (those overrides apply to base/detail textures
		    // only; legacy overlay block also uses raw lightRGB).
		    if (useOverlayTexture && overlayHandle != 0xffffffff)
		    {
		        // PR2b Stage 0b — bracket the M2d fast-path block + counter.
		        // Counter increments once per quad that enters M2d (whether or
		        // not the overlay actually emits — overlayTexId==0 short-circuits
		        // below, but the per-quad scaffolding cost is real). Drops to
		        // zero post Stage 3b legacy gate-off.
		        CostSplitOverlayScope _csOverlay;
		        gos_terrain_indirect::Counters_AddM2dOverlayEmitQuad();

		        // Slice A — substitutive gate-off, mirrors the PR2c mine gate
		        // (quad.cpp `if (!IsFrameMineArmed()) enqueueTerrainMineState`).
		        // When armed, the mission-static cement-overlay bake
		        // (gos_terrain_indirect::DrawDecalStatic, Render.TerrainOverlays
		        // Static) reproduces this exact emit from a persistent buffer,
		        // so the per-quad gos_PushTerrainOverlay producer is skipped.
		        // Default OFF (MC2_TERRAIN_INDIRECT_OVERLAY unset) => this is
		        // false => the legacy per-quad emit runs unchanged: ZERO
		        // behavior change. The legacy :2599+ sites are the FASTPATH-off
		        // fallback (post-`return` body) and are deliberately untouched.
		        const DWORD overlayTexId = tex_resolve(overlayHandle);
		        if (overlayTexId != 0 &&
		            !gos_terrain_indirect::IsFrameOverlayArmed())
		        {
		            WorldOverlayVert wov_corner[4];
		            for (int c = 0; c < 4; c++) {
		                wov_corner[c].wx   = vertices[c]->vx;
		                wov_corner[c].wy   = vertices[c]->vy;
		                wov_corner[c].wz   = vertices[c]->pVertex->elevation + OVERLAY_ELEV_OFFSET;
		                wov_corner[c].fog  = (float)((vertices[c]->fogRGB >> 24) & 0xFF) / 255.0f;
		                wov_corner[c].argb = (unsigned int)vertices[c]->lightRGB;
		                // u/v assigned per-tri below.
		            }

		            if (uvMode == BOTTOMLEFT) {
		                if (pzTri1) {
		                    WorldOverlayVert w[3] = { wov_corner[0], wov_corner[1], wov_corner[3] };
		                    w[0].u=oldminU; w[0].v=oldminV;
		                    w[1].u=oldmaxU; w[1].v=oldminV;
		                    w[2].u=oldminU; w[2].v=oldmaxV;
		                    gos_PushTerrainOverlay(w, overlayTexId);
		                }
		                if (pzTri2) {
		                    WorldOverlayVert w[3] = { wov_corner[1], wov_corner[2], wov_corner[3] };
		                    w[0].u=oldmaxU; w[0].v=oldminV;
		                    w[1].u=oldmaxU; w[1].v=oldmaxV;
		                    w[2].u=oldminU; w[2].v=oldmaxV;
		                    gos_PushTerrainOverlay(w, overlayTexId);
		                }
		            } else {
		                // BOTTOMRIGHT (= TOPRIGHT diagonal)
		                if (pzTri1) {
		                    WorldOverlayVert w[3] = { wov_corner[0], wov_corner[1], wov_corner[2] };
		                    w[0].u=oldminU; w[0].v=oldminV;
		                    w[1].u=oldmaxU; w[1].v=oldminV;
		                    w[2].u=oldmaxU; w[2].v=oldmaxV;
		                    gos_PushTerrainOverlay(w, overlayTexId);
		                }
		                if (pzTri2) {
		                    WorldOverlayVert w[3] = { wov_corner[0], wov_corner[2], wov_corner[3] };
		                    w[0].u=oldminU; w[0].v=oldminV;
		                    w[1].u=oldmaxU; w[1].v=oldmaxV;
		                    w[2].u=oldminU; w[2].v=oldmaxV;
		                    gos_PushTerrainOverlay(w, overlayTexId);
		                }
		            }
		        }
		    }

		    return;
		}
		// === end M2 branch — legacy path continues below ===

		if (uvMode == BOTTOMRIGHT)
		{
			//--------------------------
			// Top Triangle
			DWORD lightRGB0 = vertices[0]->lightRGB; 
			DWORD lightRGB1 = vertices[1]->lightRGB; 
			DWORD lightRGB2 = vertices[2]->lightRGB; 
			bool isCement = Terrain::terrainTextures->isCement(vertices[0]->pVertex->textureData & 0x0000ffff);
			bool isAlpha = Terrain::terrainTextures->isAlpha(vertices[0]->pVertex->textureData & 0x0000ffff);

			if (Terrain::terrainTextures2 && (!isCement || isAlpha))
				lightRGB0 = lightRGB1 = lightRGB2 = 0xffffffff;

			lightRGB0 = vertices[0]->pVertex->selected ? SELECTION_COLOR : lightRGB0;
			lightRGB1 = vertices[1]->pVertex->selected ? SELECTION_COLOR : lightRGB1;
			lightRGB2 = vertices[2]->pVertex->selected ? SELECTION_COLOR : lightRGB2;

			gVertex[0].x		= vertices[0]->px;
			gVertex[0].y		= vertices[0]->py;
			gVertex[0].z		= vertices[0]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[0].rhw		= vertices[0]->pw;
			// [DEPTHBIAS_CALIB] VPL-#10 distance-proportional grounding
			// probe (env MC2_DEPTHBIAS_CALIB, INERT log-only). pw == |1/clip.w|;
			// clip.w == 1/pw. One mc2_17 zoom-sweep (in/default/out) gives
			// W0 (default-zoom clip.w) + the real min/max clip.w to ground
			// the 6 CRITICAL assumptions in the distance-proportional spec.
			{
				static const bool s_dbc = (getenv("MC2_DEPTHBIAS_CALIB") != nullptr);
				if (s_dbc) {
					// Filter to VALID on-screen post-admission terrain verts
					// ONLY: pz in [0,1) (the [0,1] glClipControl NDC-z the
					// depth buffer actually sees -- excludes off-screen /
					// pre-gate / pz=-0.5 sentinel), pw finite & positive in a
					// sane camera range (excludes the pw=0.5 sentinel and the
					// pw=512 / pw=5.4e-5 behind-camera/degenerate outliers
					// that wrecked the first capture). clip.w == 1/pw.
					const float pz = vertices[0]->pz;
					const float pw = vertices[0]->pw;
					const bool  valid =
						(pz >= 0.0f && pz < 1.0f) &&
						(pw > 1.0e-5f && pw < 0.2f);   // clip.w in (5, 100000)
					if (valid) {
						const float cw = 1.0f / pw;
						// WINDOWED stats, RESET each summary so the user's
						// distinct zoom pauses (full-in / default / full-out)
						// read as distinct consecutive summary lines.
						static int   s_w = 0;
						static float s_cwMin = 1e30f, s_cwMax = -1e30f, s_cwSum = 0.0f;
						static int   s_first = 0;
						if (cw < s_cwMin) s_cwMin = cw;
						if (cw > s_cwMax) s_cwMax = cw;
						s_cwSum += cw;
						if (s_first < 12) {
							++s_first;
							fprintf(stderr,
								"[DEPTHBIAS_CALIB v2] event=sample pz=%.6f clipw=%.3f\n",
								pz, cw);
						}
						if ((++s_w % 4000) == 0) {
							fprintf(stderr,
								"[DEPTHBIAS_CALIB v2] event=window validN=%d "
								"clipwMin=%.2f clipwAvg=%.2f clipwMax=%.2f\n",
								s_w, s_cwMin, s_cwSum / (float)s_w, s_cwMax);
							s_cwMin = 1e30f; s_cwMax = -1e30f; s_cwSum = 0.0f; s_w = 0;
						}
					}
				}
			}
			gVertex[0].u		= minU;
			gVertex[0].v		= minV;
			gVertex[0].argb		= lightRGB0;
			gVertex[0].frgb		= vertices[0]->fogRGB;
			gVertex[0].frgb		= (gVertex[0].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[0]->pVertex->terrainType);

			gVertex[1].x		= vertices[1]->px;
			gVertex[1].y		= vertices[1]->py;
			gVertex[1].z		= vertices[1]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[1].rhw		= vertices[1]->pw;
			gVertex[1].u		= maxU;
			gVertex[1].v		= minV;
			gVertex[1].argb		= lightRGB1;
			gVertex[1].frgb		= vertices[1]->fogRGB;
			gVertex[1].frgb		= (gVertex[1].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[1]->pVertex->terrainType);

			gVertex[2].x		= vertices[2]->px;
			gVertex[2].y		= vertices[2]->py;
			gVertex[2].z		= vertices[2]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[2].rhw		= vertices[2]->pw;
			gVertex[2].u		= maxU;
			gVertex[2].v		= maxV;
			gVertex[2].argb		= lightRGB2;
			gVertex[2].frgb		= vertices[2]->fogRGB;
			gVertex[2].frgb		= (gVertex[2].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[2]->pVertex->terrainType);

			// Capture tri1 pz result and vertex data before the destructive
			// gVertex shuffle for tri2. appendQuad (below) uses both.
			const bool pzTri1 = ((gVertex[0].z >= 0.0f) && (gVertex[0].z < 1.0f) &&
			                     (gVertex[1].z >= 0.0f) && (gVertex[1].z < 1.0f) &&
			                     (gVertex[2].z >= 0.0f) && (gVertex[2].z < 1.0f));
			const gos_VERTEX gvTri1[3] = {gVertex[0], gVertex[1], gVertex[2]};
			if (pzTri1)
			{
				{
					// sebi: beware this will be drawn with alpha blending, so need to make sure that alpha is not zero, because this is a base terrain layer!
					if(terrainHandle!=0 && !TerrainPatchStream::isFastPathActive()) {
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[0], vertices[1], vertices[2]);
						// PatchStream append moved to appendQuad after both pz gates.
					}

					//--------------------------------------------------------------
					// Draw the Overlay Texture if it exists.
					if (useOverlayTexture && (overlayHandle != 0xffffffff))
					{
						//Uses EXACT same coords as the above normal texture.
						// Just replace the UVs and the texture handle!!
						gos_VERTEX oVertex[3];
						memcpy(oVertex,gVertex,sizeof(gos_VERTEX)*3);
						oVertex[0].u		= oldminU;
						oVertex[0].v		= oldminV;
						oVertex[1].u		= oldmaxU;
						oVertex[1].v		= oldminV;
						oVertex[2].u		= oldmaxU;
						oVertex[2].v		= oldmaxV;

						//Light the overlays!!
						oVertex[0].argb		= vertices[0]->lightRGB;
						oVertex[1].argb		= vertices[1]->lightRGB;
						oVertex[2].argb		= vertices[2]->lightRGB;

						// GPU projection: MC2 world coords into new typed batch
						setOverlayWorldCoords(oVertex[0], vertices[0]);
						setOverlayWorldCoords(oVertex[1], vertices[1]);
						setOverlayWorldCoords(oVertex[2], vertices[2]);

						{
							WorldOverlayVert wov[3];
							for (int _k = 0; _k < 3; ++_k) {
								wov[_k].wx   = oVertex[_k].x;
								wov[_k].wy   = oVertex[_k].y;
								wov[_k].wz   = oVertex[_k].z;
								wov[_k].u    = oVertex[_k].u;
								wov[_k].v    = oVertex[_k].v;
								wov[_k].fog  = (float)((oVertex[_k].frgb >> 24) & 0xFF) / 255.0f;
								wov[_k].argb = oVertex[_k].argb;
							}
							const DWORD overlayTexId = tex_resolve(overlayHandle);
							if (overlayTexId != 0)
								gos_PushTerrainOverlay(wov, overlayTexId);
						}
					}

					//----------------------------------------------------
					// Draw the detail Texture
					if (useWaterInterestTexture && (terrainDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);

						float tilingFactor = Terrain::terrainTextures->getDetailTilingFactor(1);
						if (Terrain::terrainTextures2)
							tilingFactor = Terrain::terrainTextures2->getDetailTilingFactor();

						float oneOverTf		= tilingFactor / Terrain::worldUnitsMapSide;
						
						sVertex[0].u		= (vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverTf;
						sVertex[0].v		= (Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverTf;
		
						sVertex[1].u		= (vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverTf;
						sVertex[1].v		= (Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverTf;
		
						sVertex[2].u		= (vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverTf;
						sVertex[2].v		= (Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverTf;
						
						if ((sVertex[0].u > MaxMinUV) || 
							(sVertex[0].v > MaxMinUV) ||
							(sVertex[1].u > MaxMinUV) || 
							(sVertex[1].v > MaxMinUV) ||
							(sVertex[2].u > MaxMinUV) || 
							(sVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(sVertex[0].u,fmax(sVertex[1].u,sVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(sVertex[0].v,fmax(sVertex[1].v,sVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							sVertex[0].u -= maxU;
							sVertex[1].u -= maxU;
							sVertex[2].u -= maxU;
							
							sVertex[0].v -= maxV;
							sVertex[1].v -= maxV;
							sVertex[2].v -= maxV;
						}

						//Light the Detail Texture
						if (Terrain::terrainTextures2)
						{
							sVertex[0].argb		= 
							sVertex[1].argb		= 
							sVertex[2].argb		= 0xffffffff;
						}

						// Slice A (A2) - DRAWALPHA detail dead-confirmation counter.
						// Counter-only; behaviour unchanged. Post-return legacy body,
						// unreachable on armed frames; expected ==0 (A2 gate).
						gos_terrain_indirect::Counters_AddLegacyDrawAlphaDetailQuad();
						mcTextureManager->addVertices(terrainDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);	
					}
				}
			}

			//--------------------------
			//Bottom Triangle
			//
			// gVertex[0] same as above gVertex[0].
			// gVertex[1] is same as above gVertex[2].
			// gVertex[2] is calced from vertex[3].
			DWORD lightRGB3 = vertices[3]->lightRGB;
			if (Terrain::terrainTextures2 && (!isCement || isAlpha))
				lightRGB3 = 0xffffffff;

			lightRGB3 = vertices[3]->pVertex->selected ? SELECTION_COLOR : lightRGB3;

			gVertex[1].x		= gVertex[2].x;	
			gVertex[1].y		= gVertex[2].y;	
			gVertex[1].z		= gVertex[2].z;	
			gVertex[1].rhw		= gVertex[2].rhw;
			gVertex[1].u		= gVertex[2].u;
			gVertex[1].v		= gVertex[2].v;	
			gVertex[1].argb		= gVertex[2].argb;
			gVertex[1].frgb		= gVertex[2].frgb;

			gVertex[2].x		= vertices[3]->px;
			gVertex[2].y		= vertices[3]->py;
			gVertex[2].z		= vertices[3]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[2].rhw		= vertices[3]->pw;
			gVertex[2].u		= minU;
			gVertex[2].v		= maxV;
			gVertex[2].argb		= lightRGB3;
			gVertex[2].frgb		= vertices[3]->fogRGB;
			gVertex[2].frgb		= (gVertex[2].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[3]->pVertex->terrainType);

			const bool pzTri2 = ((gVertex[0].z >= 0.0f) && (gVertex[0].z < 1.0f) &&
			                     (gVertex[1].z >= 0.0f) && (gVertex[1].z < 1.0f) &&
			                     (gVertex[2].z >= 0.0f) && (gVertex[2].z < 1.0f));
			if (pzTri2)
			{
				{
					if(terrainHandle!=0 && !TerrainPatchStream::isFastPathActive()) {
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[0], vertices[2], vertices[3]);
						// PatchStream append moved to appendQuad below.
					}

					//--------------------------------------------------------------
					// Draw the Overlay Texture if it exists.
					if (useOverlayTexture && (overlayHandle != 0xffffffff))
					{
						//Uses EXACT same coords as the above normal texture.
						// Just replace the UVs and the texture handle!!
						gos_VERTEX oVertex[3];
						memcpy(oVertex,gVertex,sizeof(gos_VERTEX)*3);
						oVertex[0].u		= oldminU;
						oVertex[0].v		= oldminV;
						oVertex[1].u		= oldmaxU;
						oVertex[1].v		= oldmaxV;
						oVertex[2].u		= oldminU;
						oVertex[2].v		= oldmaxV;

						//Light the overlays!!
						oVertex[0].argb		= vertices[0]->lightRGB;
						oVertex[1].argb		= vertices[2]->lightRGB;
						oVertex[2].argb		= vertices[3]->lightRGB;

						// GPU projection: MC2 world coords into new typed batch
						setOverlayWorldCoords(oVertex[0], vertices[0]);
						setOverlayWorldCoords(oVertex[1], vertices[2]);
						setOverlayWorldCoords(oVertex[2], vertices[3]);

						{
							WorldOverlayVert wov[3];
							for (int _k = 0; _k < 3; ++_k) {
								wov[_k].wx   = oVertex[_k].x;
								wov[_k].wy   = oVertex[_k].y;
								wov[_k].wz   = oVertex[_k].z;
								wov[_k].u    = oVertex[_k].u;
								wov[_k].v    = oVertex[_k].v;
								wov[_k].fog  = (float)((oVertex[_k].frgb >> 24) & 0xFF) / 255.0f;
								wov[_k].argb = oVertex[_k].argb;
							}
							const DWORD overlayTexId = tex_resolve(overlayHandle);
							if (overlayTexId != 0)
								gos_PushTerrainOverlay(wov, overlayTexId);
						}
					}

					//----------------------------------------------------
					// Draw the detail Texture
					if (useWaterInterestTexture && (terrainDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);

						float tilingFactor = Terrain::terrainTextures->getDetailTilingFactor(1);
						if (Terrain::terrainTextures2)
							tilingFactor = Terrain::terrainTextures2->getDetailTilingFactor();

 						float oneOverTF		= tilingFactor / Terrain::worldUnitsMapSide;
						
						sVertex[0].u		= (vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverTF;
						sVertex[0].v		= (Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverTF;
		
						sVertex[1].u		= (vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverTF;
						sVertex[1].v		= (Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverTF;
		
						sVertex[2].u		= (vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverTF;
						sVertex[2].v		= (Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverTF;
		
						if ((sVertex[0].u > MaxMinUV) || 
							(sVertex[0].v > MaxMinUV) ||
							(sVertex[1].u > MaxMinUV) || 
							(sVertex[1].v > MaxMinUV) ||
							(sVertex[2].u > MaxMinUV) || 
							(sVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(sVertex[0].u,fmax(sVertex[1].u,sVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(sVertex[0].v,fmax(sVertex[1].v,sVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							sVertex[0].u -= maxU;
							sVertex[1].u -= maxU;
							sVertex[2].u -= maxU;
							
							sVertex[0].v -= maxV;
							sVertex[1].v -= maxV;
							sVertex[2].v -= maxV;
						}

						//Light the Detail Texture
						if (Terrain::terrainTextures2)
						{
							sVertex[0].argb		= 
							sVertex[1].argb		= 
							sVertex[2].argb		= 0xffffffff;
						}

						// Slice A (A2) - DRAWALPHA detail dead-confirmation counter.
						// Counter-only; behaviour unchanged. Post-return legacy body,
						// unreachable on armed frames; expected ==0 (A2 gate).
						gos_terrain_indirect::Counters_AddLegacyDrawAlphaDetailQuad();
						mcTextureManager->addVertices(terrainDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
					}
				}
			}

			// PatchStream: one bucket lookup for both triangles of this quad.
			if (terrainHandle != 0 && TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
				gos_TERRAIN_EXTRA tx1[3] = {}, tx2[3] = {};
				if (!TerrainPatchStream::isThinRecordsActive()) {
					if (pzTri1) buildTerrainExtraTriple(vertices[0], vertices[1], vertices[2], tx1);
					if (pzTri2) buildTerrainExtraTriple(vertices[0], vertices[2], vertices[3], tx2);
				}
				TerrainPatchStream::appendQuad(terrainHandle, gvTri1, tx1, pzTri1, gVertex, tx2, pzTri2);
				// M1 compact record — TOPRIGHT diagonal.
				// gvTri1[0..2] = corners 0,1,2 (saved before shuffle).
				// gVertex[2]   = corner 3 (constructed by shuffle from vertices[3]).
				TerrainQuadRecord rec;
				rec.wx0=vertices[0]->vx; rec.wy0=vertices[0]->vy; rec.wz0=vertices[0]->pVertex->elevation; rec._wp0=0.f;
				rec.wx1=vertices[1]->vx; rec.wy1=vertices[1]->vy; rec.wz1=vertices[1]->pVertex->elevation; rec._wp1=0.f;
				rec.wx2=vertices[2]->vx; rec.wy2=vertices[2]->vy; rec.wz2=vertices[2]->pVertex->elevation; rec._wp2=0.f;
				rec.wx3=vertices[3]->vx; rec.wy3=vertices[3]->vy; rec.wz3=vertices[3]->pVertex->elevation; rec._wp3=0.f;
				rec.nx0=vertices[0]->pVertex->vertexNormal.x; rec.ny0=vertices[0]->pVertex->vertexNormal.y; rec.nz0=vertices[0]->pVertex->vertexNormal.z; rec._np0=0.f;
				rec.nx1=vertices[1]->pVertex->vertexNormal.x; rec.ny1=vertices[1]->pVertex->vertexNormal.y; rec.nz1=vertices[1]->pVertex->vertexNormal.z; rec._np1=0.f;
				rec.nx2=vertices[2]->pVertex->vertexNormal.x; rec.ny2=vertices[2]->pVertex->vertexNormal.y; rec.nz2=vertices[2]->pVertex->vertexNormal.z; rec._np2=0.f;
				rec.nx3=vertices[3]->pVertex->vertexNormal.x; rec.ny3=vertices[3]->pVertex->vertexNormal.y; rec.nz3=vertices[3]->pVertex->vertexNormal.z; rec._np3=0.f;
				rec.minU=minU; rec.minV=minV; rec.maxU=maxU; rec.maxV=maxV;
				rec.lightRGB0=gvTri1[0].argb; rec.lightRGB1=gvTri1[1].argb; rec.lightRGB2=gvTri1[2].argb; rec.lightRGB3=gVertex[2].argb;
				rec.fogRGB0  =gvTri1[0].frgb; rec.fogRGB1  =gvTri1[1].frgb; rec.fogRGB2  =gvTri1[2].frgb; rec.fogRGB3  =gVertex[2].frgb;
				rec.terrainHandle = (uint32_t)terrainHandle;
				rec.flags = 0u | (pzTri1 ? 2u : 0u) | (pzTri2 ? 4u : 0u); // bit0=0 → TOPRIGHT
				rec._ctrl2 = 0u; rec._ctrl3 = 0u;
				TerrainPatchStream::appendQuadRecord(rec);
				TerrainPatchStream::addRecordVertParity((pzTri1 ? 3u : 0u) + (pzTri2 ? 3u : 0u));
				{
					TerrainQuadRecipe recipe;
					recipe.wx0=vertices[0]->vx; recipe.wy0=vertices[0]->vy; recipe.wz0=vertices[0]->pVertex->elevation; recipe._wp0=0.f;
					recipe.wx1=vertices[1]->vx; recipe.wy1=vertices[1]->vy; recipe.wz1=vertices[1]->pVertex->elevation; recipe._wp1=0.f;
					recipe.wx2=vertices[2]->vx; recipe.wy2=vertices[2]->vy; recipe.wz2=vertices[2]->pVertex->elevation; recipe._wp2=0.f;
					recipe.wx3=vertices[3]->vx; recipe.wy3=vertices[3]->vy; recipe.wz3=vertices[3]->pVertex->elevation; recipe._wp3=0.f;
					recipe.nx0=vertices[0]->pVertex->vertexNormal.x; recipe.ny0=vertices[0]->pVertex->vertexNormal.y; recipe.nz0=vertices[0]->pVertex->vertexNormal.z; recipe._np0=0.f;
					recipe.nx1=vertices[1]->pVertex->vertexNormal.x; recipe.ny1=vertices[1]->pVertex->vertexNormal.y; recipe.nz1=vertices[1]->pVertex->vertexNormal.z; recipe._np1=0.f;
					recipe.nx2=vertices[2]->pVertex->vertexNormal.x; recipe.ny2=vertices[2]->pVertex->vertexNormal.y; recipe.nz2=vertices[2]->pVertex->vertexNormal.z; recipe._np2=0.f;
					recipe.nx3=vertices[3]->pVertex->vertexNormal.x; recipe.ny3=vertices[3]->pVertex->vertexNormal.y; recipe.nz3=vertices[3]->pVertex->vertexNormal.z; recipe._np3=0.f;
					recipe.minU=minU; recipe.minV=minV; recipe.maxU=maxU; recipe.maxV=maxV;
					const uint32_t tFlags = 0u | (pzTri1 ? 2u : 0u) | (pzTri2 ? 4u : 0u); // bit0=0 → TOPRIGHT
					{
						uint32_t m0 = terrainTypeToMaterial(vertices[0]->pVertex->terrainType);
						uint32_t m1 = terrainTypeToMaterial(vertices[1]->pVertex->terrainType);
						uint32_t m2 = terrainTypeToMaterial(vertices[2]->pVertex->terrainType);
						uint32_t m3 = terrainTypeToMaterial(vertices[3]->pVertex->terrainType);
						uint32_t tpacked = m0 | (m1 << 8) | (m2 << 16) | (m3 << 24);
						memcpy(&recipe._wp0, &tpacked, 4);
					}
					TerrainPatchStream::appendThinRecord(terrainHandle, recipe, tFlags,
						gvTri1[0].argb, gvTri1[1].argb, gvTri1[2].argb, gVertex[2].argb);
					TerrainPatchStream::addThinRecordVertParity((pzTri1 ? 3u : 0u) + (pzTri2 ? 3u : 0u));
				}
			}
		}
		else if (uvMode == BOTTOMLEFT)
		{
			//------------------------------
			// Top Triangle.
			DWORD lightRGB0 = vertices[0]->lightRGB;
			DWORD lightRGB1 = vertices[1]->lightRGB;
			DWORD lightRGB3 = vertices[3]->lightRGB;
			bool isCement = Terrain::terrainTextures->isCement(vertices[0]->pVertex->textureData & 0x0000ffff);
			bool isAlpha = Terrain::terrainTextures->isAlpha(vertices[0]->pVertex->textureData & 0x0000ffff);
			if (Terrain::terrainTextures2 && (!isCement || isAlpha))
				lightRGB0 = lightRGB1 = lightRGB3 = 0xffffffff;

			lightRGB0 = vertices[0]->pVertex->selected ? SELECTION_COLOR : lightRGB0;
			lightRGB1 = vertices[1]->pVertex->selected ? SELECTION_COLOR : lightRGB1;
			lightRGB3 = vertices[3]->pVertex->selected ? SELECTION_COLOR : lightRGB3;

			gVertex[0].x		= vertices[0]->px;
			gVertex[0].y		= vertices[0]->py;
			gVertex[0].z		= vertices[0]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[0].rhw		= vertices[0]->pw;
			gVertex[0].u		= minU;
			gVertex[0].v		= minV;
			gVertex[0].argb		= lightRGB0;
			gVertex[0].frgb		= vertices[0]->fogRGB;
			gVertex[0].frgb		= (gVertex[0].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[0]->pVertex->terrainType);

			gVertex[1].x		= vertices[1]->px;
			gVertex[1].y		= vertices[1]->py;
			gVertex[1].z		= vertices[1]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[1].rhw		= vertices[1]->pw;
			gVertex[1].u		= maxU;
			gVertex[1].v		= minV;
			gVertex[1].argb		= lightRGB1;
			gVertex[1].frgb		= vertices[1]->fogRGB;
			gVertex[1].frgb		= (gVertex[1].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[1]->pVertex->terrainType);

			gVertex[2].x		= vertices[3]->px;
			gVertex[2].y		= vertices[3]->py;
			gVertex[2].z		= vertices[3]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[2].rhw		= vertices[3]->pw;
			gVertex[2].u		= minU;
			gVertex[2].v		= maxV;
			gVertex[2].argb		= lightRGB3;
			gVertex[2].frgb		= vertices[3]->fogRGB;
			gVertex[2].frgb		= (gVertex[2].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[3]->pVertex->terrainType);

			// Capture tri1 pz result and vertex data before the destructive
			// gVertex shuffle for tri2. appendQuad (below) uses both.
			const bool pzTri1 = ((gVertex[0].z >= 0.0f) && (gVertex[0].z < 1.0f) &&
			                     (gVertex[1].z >= 0.0f) && (gVertex[1].z < 1.0f) &&
			                     (gVertex[2].z >= 0.0f) && (gVertex[2].z < 1.0f));
			const gos_VERTEX gvTri1[3] = {gVertex[0], gVertex[1], gVertex[2]};
			if (pzTri1)
			{
				{
					if(terrainHandle!=0 && !TerrainPatchStream::isFastPathActive()) {
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[0], vertices[1], vertices[3]);
						// PatchStream append moved to appendQuad after both pz gates.
					}

					//----------------------------------------------------
					// Draw the detail Texture
					if (useWaterInterestTexture && (terrainDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						float tilingFactor = Terrain::terrainTextures->getDetailTilingFactor(1);
						if (Terrain::terrainTextures2)
							tilingFactor = Terrain::terrainTextures2->getDetailTilingFactor();
							
 						float oneOverTF		= tilingFactor / Terrain::worldUnitsMapSide;
						
						sVertex[0].u		= (vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverTF;
						sVertex[0].v		= (Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverTF;
		
						sVertex[1].u		= (vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverTF;
						sVertex[1].v		= (Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverTF;
		
						sVertex[2].u		= (vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverTF;
						sVertex[2].v		= (Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverTF;
		
						if ((sVertex[0].u > MaxMinUV) || 
							(sVertex[0].v > MaxMinUV) ||
							(sVertex[1].u > MaxMinUV) || 
							(sVertex[1].v > MaxMinUV) ||
							(sVertex[2].u > MaxMinUV) || 
							(sVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(sVertex[0].u,fmax(sVertex[1].u,sVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(sVertex[0].v,fmax(sVertex[1].v,sVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							sVertex[0].u -= maxU;
							sVertex[1].u -= maxU;
							sVertex[2].u -= maxU;
							
							sVertex[0].v -= maxV;
							sVertex[1].v -= maxV;
							sVertex[2].v -= maxV;
						}
						
						//Light the Detail Texture
						if (Terrain::terrainTextures2)
						{
							sVertex[0].argb		= 
							sVertex[1].argb		= 
							sVertex[2].argb		= 0xffffffff;
						}
						// Slice A (A2) - DRAWALPHA detail dead-confirmation counter.
						// Counter-only; behaviour unchanged. Post-return legacy body,
						// unreachable on armed frames; expected ==0 (A2 gate).
						gos_terrain_indirect::Counters_AddLegacyDrawAlphaDetailQuad();
						mcTextureManager->addVertices(terrainDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
					}
					
					//--------------------------------------------------------------
					// Draw the Overlay Texture if it exists.
					if (useOverlayTexture && (overlayHandle != 0xffffffff))
					{
						//Uses EXACT same coords as the above normal texture.
						// Just replace the UVs and the texture handle!!
						gos_VERTEX oVertex[3];
						memcpy(oVertex,gVertex,sizeof(gos_VERTEX)*3);
						oVertex[0].u		= oldminU;
						oVertex[0].v		= oldminV;
						oVertex[1].u		= oldmaxU;
						oVertex[1].v		= oldminV;
						oVertex[2].u		= oldminU;
						oVertex[2].v		= oldmaxV;

						//Light the overlays!!
						oVertex[0].argb		= vertices[0]->lightRGB;
						oVertex[1].argb		= vertices[1]->lightRGB;
						oVertex[2].argb		= vertices[3]->lightRGB;

						// GPU projection: MC2 world coords into new typed batch
						setOverlayWorldCoords(oVertex[0], vertices[0]);
						setOverlayWorldCoords(oVertex[1], vertices[1]);
						setOverlayWorldCoords(oVertex[2], vertices[3]);

						{
							WorldOverlayVert wov[3];
							for (int _k = 0; _k < 3; ++_k) {
								wov[_k].wx   = oVertex[_k].x;
								wov[_k].wy   = oVertex[_k].y;
								wov[_k].wz   = oVertex[_k].z;
								wov[_k].u    = oVertex[_k].u;
								wov[_k].v    = oVertex[_k].v;
								wov[_k].fog  = (float)((oVertex[_k].frgb >> 24) & 0xFF) / 255.0f;
								wov[_k].argb = oVertex[_k].argb;
							}
							const DWORD overlayTexId = tex_resolve(overlayHandle);
							if (overlayTexId != 0)
								gos_PushTerrainOverlay(wov, overlayTexId);
						}
					}
				}
			}

			//---------------------------------------
			// Bottom Triangle.
			// gVertex[0] is same as above gVertex[1]
			// gVertex[1] is new and calced from vertex[2].
			// gVertex[2] is same as above.
			DWORD lightRGB2 = vertices[2]->lightRGB;
			if (Terrain::terrainTextures2 && (!isCement || isAlpha))
				lightRGB2 = 0xffffffff;

			lightRGB2 = vertices[2]->pVertex->selected ? SELECTION_COLOR : lightRGB2;

			gVertex[0].x		= gVertex[1].x;	
			gVertex[0].y		= gVertex[1].y;	
			gVertex[0].z		= gVertex[1].z;	
			gVertex[0].rhw		= gVertex[1].rhw;	
			gVertex[0].u		= gVertex[1].u;	
			gVertex[0].v		= gVertex[1].v;	
			gVertex[0].argb		= gVertex[1].argb;
			gVertex[0].frgb		= gVertex[1].frgb;

			gVertex[1].x		= vertices[2]->px;
			gVertex[1].y		= vertices[2]->py;
			gVertex[1].z		= vertices[2]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[1].rhw		= vertices[2]->pw;
			gVertex[1].u		= maxU;
			gVertex[1].v		= maxV;
			gVertex[1].argb		= lightRGB2;
			gVertex[1].frgb		= vertices[2]->fogRGB;
			gVertex[1].frgb		= (gVertex[1].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[2]->pVertex->terrainType);

			const bool pzTri2 = ((gVertex[0].z >= 0.0f) && (gVertex[0].z < 1.0f) &&
			                     (gVertex[1].z >= 0.0f) && (gVertex[1].z < 1.0f) &&
			                     (gVertex[2].z >= 0.0f) && (gVertex[2].z < 1.0f));
			if (pzTri2)
			{
				{
					if(terrainHandle!=0 && !TerrainPatchStream::isFastPathActive()) {
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[1], vertices[2], vertices[3]);
						// PatchStream append moved to appendQuad below.
					}

					//----------------------------------------------------
					// Draw the detail Texture
					if (useWaterInterestTexture && (terrainDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						float tilingFactor = Terrain::terrainTextures->getDetailTilingFactor(1);
						if (Terrain::terrainTextures2)
							tilingFactor = Terrain::terrainTextures2->getDetailTilingFactor();
							
 						float oneOverTf		= tilingFactor / Terrain::worldUnitsMapSide;
						
						sVertex[0].u		= (vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverTf;
						sVertex[0].v		= (Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverTf;
		
						sVertex[1].u		= (vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverTf;
						sVertex[1].v		= (Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverTf;
		
						sVertex[2].u		= (vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverTf ;
						sVertex[2].v		= (Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverTf ;
		
						if ((sVertex[0].u > MaxMinUV) || 
							(sVertex[0].v > MaxMinUV) ||
							(sVertex[1].u > MaxMinUV) || 
							(sVertex[1].v > MaxMinUV) ||
							(sVertex[2].u > MaxMinUV) || 
							(sVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(sVertex[0].u,fmax(sVertex[1].u,sVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(sVertex[0].v,fmax(sVertex[1].v,sVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							sVertex[0].u -= maxU;
							sVertex[1].u -= maxU;
							sVertex[2].u -= maxU;
							
							sVertex[0].v -= maxV;
							sVertex[1].v -= maxV;
							sVertex[2].v -= maxV;
						}
						
						//Light the Detail Texture
						if (Terrain::terrainTextures2)
						{
							sVertex[0].argb		= 
							sVertex[1].argb		= 
							sVertex[2].argb		= 0xffffffff;
						}
						// Slice A (A2) - DRAWALPHA detail dead-confirmation counter.
						// Counter-only; behaviour unchanged. Post-return legacy body,
						// unreachable on armed frames; expected ==0 (A2 gate).
						gos_terrain_indirect::Counters_AddLegacyDrawAlphaDetailQuad();
						mcTextureManager->addVertices(terrainDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
					}
					
					//--------------------------------------------------------------
					// Draw the Overlay Texture if it exists.
					if (useOverlayTexture && (overlayHandle != 0xffffffff))
					{
						//Uses EXACT same coords as the above normal texture.
						// Just replace the UVs and the texture handle!!
						gos_VERTEX oVertex[3];
						memcpy(oVertex,gVertex,sizeof(gos_VERTEX)*3);
						oVertex[0].u		= oldmaxU;
						oVertex[0].v		= oldminV;
						oVertex[1].u		= oldmaxU;
						oVertex[1].v		= oldmaxV;
						oVertex[2].u		= oldminU;
						oVertex[2].v		= oldmaxV;

						//Light the overlays!!
						oVertex[0].argb		= vertices[1]->lightRGB;
						oVertex[1].argb		= vertices[2]->lightRGB;
						oVertex[2].argb		= vertices[3]->lightRGB;

						// GPU projection: MC2 world coords into new typed batch
						setOverlayWorldCoords(oVertex[0], vertices[1]);
						setOverlayWorldCoords(oVertex[1], vertices[2]);
						setOverlayWorldCoords(oVertex[2], vertices[3]);

						{
							WorldOverlayVert wov[3];
							for (int _k = 0; _k < 3; ++_k) {
								wov[_k].wx   = oVertex[_k].x;
								wov[_k].wy   = oVertex[_k].y;
								wov[_k].wz   = oVertex[_k].z;
								wov[_k].u    = oVertex[_k].u;
								wov[_k].v    = oVertex[_k].v;
								wov[_k].fog  = (float)((oVertex[_k].frgb >> 24) & 0xFF) / 255.0f;
								wov[_k].argb = oVertex[_k].argb;
							}
							const DWORD overlayTexId = tex_resolve(overlayHandle);
							if (overlayTexId != 0)
								gos_PushTerrainOverlay(wov, overlayTexId);
						}
					}
				}
			}

			// PatchStream: one bucket lookup for both triangles of this quad.
			if (terrainHandle != 0 && TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
				gos_TERRAIN_EXTRA tx1[3] = {}, tx2[3] = {};
				if (!TerrainPatchStream::isThinRecordsActive()) {
					if (pzTri1) buildTerrainExtraTriple(vertices[0], vertices[1], vertices[3], tx1);
					if (pzTri2) buildTerrainExtraTriple(vertices[1], vertices[2], vertices[3], tx2);
				}
				TerrainPatchStream::appendQuad(terrainHandle, gvTri1, tx1, pzTri1, gVertex, tx2, pzTri2);
				// M1 compact record — BOTTOMLEFT diagonal.
				// gvTri1 = {corner0, corner1, corner3} (saved before shuffle).
				// After shuffle: gVertex[0]=corner1, gVertex[1]=corner2, gVertex[2]=corner3.
				// corner2 (vertices[2]) is only in gVertex[1] after the shuffle.
				TerrainQuadRecord rec;
				rec.wx0=vertices[0]->vx; rec.wy0=vertices[0]->vy; rec.wz0=vertices[0]->pVertex->elevation; rec._wp0=0.f;
				rec.wx1=vertices[1]->vx; rec.wy1=vertices[1]->vy; rec.wz1=vertices[1]->pVertex->elevation; rec._wp1=0.f;
				rec.wx2=vertices[2]->vx; rec.wy2=vertices[2]->vy; rec.wz2=vertices[2]->pVertex->elevation; rec._wp2=0.f;
				rec.wx3=vertices[3]->vx; rec.wy3=vertices[3]->vy; rec.wz3=vertices[3]->pVertex->elevation; rec._wp3=0.f;
				rec.nx0=vertices[0]->pVertex->vertexNormal.x; rec.ny0=vertices[0]->pVertex->vertexNormal.y; rec.nz0=vertices[0]->pVertex->vertexNormal.z; rec._np0=0.f;
				rec.nx1=vertices[1]->pVertex->vertexNormal.x; rec.ny1=vertices[1]->pVertex->vertexNormal.y; rec.nz1=vertices[1]->pVertex->vertexNormal.z; rec._np1=0.f;
				rec.nx2=vertices[2]->pVertex->vertexNormal.x; rec.ny2=vertices[2]->pVertex->vertexNormal.y; rec.nz2=vertices[2]->pVertex->vertexNormal.z; rec._np2=0.f;
				rec.nx3=vertices[3]->pVertex->vertexNormal.x; rec.ny3=vertices[3]->pVertex->vertexNormal.y; rec.nz3=vertices[3]->pVertex->vertexNormal.z; rec._np3=0.f;
				rec.minU=minU; rec.minV=minV; rec.maxU=maxU; rec.maxV=maxV;
				// BOTTOMLEFT: gvTri1[0]=corner0, gvTri1[1]=corner1, gvTri1[2]=corner3
				//             gVertex[1] after shuffle = corner2
				rec.lightRGB0=gvTri1[0].argb; rec.lightRGB1=gvTri1[1].argb; rec.lightRGB2=gVertex[1].argb; rec.lightRGB3=gvTri1[2].argb;
				rec.fogRGB0  =gvTri1[0].frgb; rec.fogRGB1  =gvTri1[1].frgb; rec.fogRGB2  =gVertex[1].frgb; rec.fogRGB3  =gvTri1[2].frgb;
				rec.terrainHandle = (uint32_t)terrainHandle;
				rec.flags = 1u | (pzTri1 ? 2u : 0u) | (pzTri2 ? 4u : 0u); // bit0=1 → BOTTOMLEFT
				rec._ctrl2 = 0u; rec._ctrl3 = 0u;
				TerrainPatchStream::appendQuadRecord(rec);
				TerrainPatchStream::addRecordVertParity((pzTri1 ? 3u : 0u) + (pzTri2 ? 3u : 0u));
				{
					TerrainQuadRecipe recipe;
					recipe.wx0=vertices[0]->vx; recipe.wy0=vertices[0]->vy; recipe.wz0=vertices[0]->pVertex->elevation; recipe._wp0=0.f;
					recipe.wx1=vertices[1]->vx; recipe.wy1=vertices[1]->vy; recipe.wz1=vertices[1]->pVertex->elevation; recipe._wp1=0.f;
					recipe.wx2=vertices[2]->vx; recipe.wy2=vertices[2]->vy; recipe.wz2=vertices[2]->pVertex->elevation; recipe._wp2=0.f;
					recipe.wx3=vertices[3]->vx; recipe.wy3=vertices[3]->vy; recipe.wz3=vertices[3]->pVertex->elevation; recipe._wp3=0.f;
					recipe.nx0=vertices[0]->pVertex->vertexNormal.x; recipe.ny0=vertices[0]->pVertex->vertexNormal.y; recipe.nz0=vertices[0]->pVertex->vertexNormal.z; recipe._np0=0.f;
					recipe.nx1=vertices[1]->pVertex->vertexNormal.x; recipe.ny1=vertices[1]->pVertex->vertexNormal.y; recipe.nz1=vertices[1]->pVertex->vertexNormal.z; recipe._np1=0.f;
					recipe.nx2=vertices[2]->pVertex->vertexNormal.x; recipe.ny2=vertices[2]->pVertex->vertexNormal.y; recipe.nz2=vertices[2]->pVertex->vertexNormal.z; recipe._np2=0.f;
					recipe.nx3=vertices[3]->pVertex->vertexNormal.x; recipe.ny3=vertices[3]->pVertex->vertexNormal.y; recipe.nz3=vertices[3]->pVertex->vertexNormal.z; recipe._np3=0.f;
					recipe.minU=minU; recipe.minV=minV; recipe.maxU=maxU; recipe.maxV=maxV;
					// BOTTOMLEFT: same corner order as fat record above.
					// gvTri1[0]=corner0, gvTri1[1]=corner1, gvTri1[2]=corner3; gVertex[1]=corner2
					const uint32_t tFlags = 1u | (pzTri1 ? 2u : 0u) | (pzTri2 ? 4u : 0u); // bit0=1 → BOTTOMLEFT
					{
						uint32_t m0 = terrainTypeToMaterial(vertices[0]->pVertex->terrainType);
						uint32_t m1 = terrainTypeToMaterial(vertices[1]->pVertex->terrainType);
						uint32_t m2 = terrainTypeToMaterial(vertices[2]->pVertex->terrainType);
						uint32_t m3 = terrainTypeToMaterial(vertices[3]->pVertex->terrainType);
						uint32_t tpacked = m0 | (m1 << 8) | (m2 << 16) | (m3 << 24);
						memcpy(&recipe._wp0, &tpacked, 4);
					}
					TerrainPatchStream::appendThinRecord(terrainHandle, recipe, tFlags,
						gvTri1[0].argb, gvTri1[1].argb, gVertex[1].argb, gvTri1[2].argb);
					TerrainPatchStream::addThinRecordVertParity((pzTri1 ? 3u : 0u) + (pzTri2 ? 3u : 0u));
				}
			}
		}
	}

#ifdef _DEBUG
	if (selected )
	{
		drawLine();
		selected = FALSE;		
	}
#endif

}

extern float elevationAnimation;
#define SKY_FUDGE			(1.0f / 60000.0f)
#define WATER_ALPHA			0x1fffffff
extern float cloudScrollX;
extern float cloudScrollY;

//---------------------------------------------------------------------------
void TerrainQuad::drawWater (void)
{
	float cloudOffsetX = cos(360.0f * DEGREES_TO_RADS * 32.0f * cloudScrollX) * 0.1f;
	float cloudOffsetY = sin(360.0f * DEGREES_TO_RADS * 32.0f * cloudScrollY) * 0.1f;
	
	float sprayOffsetX = cloudScrollX * 10.0f;
	float sprayOffsetY = cloudScrollY * 10.0f;

	//Gotta be able to run the untextured maps!!!
	float oneOverWaterTF = 1.0f / 64.0f;
	float oneOverTF = 1.0f / 64.0f;

	if (Terrain::terrainTextures2)
	{
		oneOverWaterTF = (Terrain::terrainTextures2->getWaterDetailTilingFactor() / Terrain::worldUnitsMapSide);
		oneOverTF = (Terrain::terrainTextures2->getWaterTextureTilingFactor() / Terrain::worldUnitsMapSide);
	}

	if (waterHandle != 0xffffffff)
	{
		numTerrainFaces++;

		// [DEPTH_TRANSITION v1] CPU-water REAL screen-z sample (env-gated,
		// silent default). The zoom-step depth-pop diagnosis has been wrong
		// 3x; sampling the ACTUAL produced value is strictly better evidence
		// than a CPU re-derivation of the Stuff eye->projectForTerrainAdmission
		// pipeline (which is NOT reachable from the gos_terrain_indirect.cpp
		// transition dump site). The drawn CPU-water screen-z is exactly
		// `vertices[k]->wz + WATER_DEPTH_FUDGE` (== WATER_DEPTH_FUDGE_RASTER
		// 0.0025; see the gVertex[*].z assignments at ~:3345 and the
		// terrain_depth_bias.h RASTER regime). We latch the value for the
		// water vertex nearest the FIXED mid-map probe point P so the dump's
		// dz is frame-comparable. world XY is vertices[k]->vx / ->vy (the
		// same pre-projection world coords drawWater emits). g_cpuWaterProbeZ
		// carries a frame stamp because CPU water and the GPU fast path are
		// MUTUALLY EXCLUSIVE per frame (terrain.cpp:1225-1229 early-returns
		// renderWater when WaterFastPathOwnsArmedDraw()): on an armed
		// transition frame this static is the LAST un-armed steady value, not
		// this-frame data. The dump emits g_cpuWaterProbeFrameValid so a
		// stale CPU sample is visibly flagged, never silently trusted.
		extern float    g_cpuWaterProbeZ;
		extern double   g_cpuWaterProbeBestD2;
		extern bool     g_cpuWaterProbeAny;
		static const bool s_depthTransProbe =
		    (getenv("MC2_DEPTH_TRANSITION_PROBE") != nullptr);
		if (s_depthTransProbe)
		{
			const float px = Terrain::mapTopLeft3d.x
			                 + Terrain::worldUnitsMapSide * 0.5f;
			const float py = Terrain::mapTopLeft3d.y
			                 - Terrain::worldUnitsMapSide * 0.5f;
			for (int k = 0; k < 4; ++k)
			{
				const float dx = vertices[k]->vx - px;
				const float dy = vertices[k]->vy - py;
				const double d2 = (double)dx * dx + (double)dy * dy;
				if (!g_cpuWaterProbeAny || d2 < g_cpuWaterProbeBestD2)
				{
					g_cpuWaterProbeBestD2 = d2;
					g_cpuWaterProbeZ = vertices[k]->wz + WATER_DEPTH_FUDGE;
					g_cpuWaterProbeAny = true;
				}
			}
		}

		//---------------------------------------
		// GOS 3D draw Calls now!

		float minU = 0.0f;
		float maxU = 0.9999999f;

		float minV = 0.0f;
		float maxV = 0.9999999f;

		//-----------------------------------------------------
		// FOG time.  Set Render state to FOG on!
		if (useFog)
		{
			DWORD fogColor = eye->fogColor;
			//gos_SetRenderState( gos_State_Fog, (int)&fogColor);
			gos_SetRenderState( gos_State_Fog, fogColor);
		}
		else
		{
			gos_SetRenderState( gos_State_Fog, 0);
		}

		gos_VERTEX gVertex[3];
		if (uvMode == BOTTOMRIGHT)
		{
			//--------------------------
			// Top Triangle
			gVertex[0].x		= vertices[0]->wx;
			gVertex[0].y		= vertices[0]->wy;
			gVertex[0].z		= vertices[0]->wz + WATER_DEPTH_FUDGE;
			gVertex[0].rhw		= vertices[0]->ww;
			gVertex[0].u		= minU + cloudOffsetX;
			gVertex[0].v		= minV + cloudOffsetY;
			gVertex[0].argb		= vertices[0]->pVertex->selected ? SELECTION_COLOR : vertices[0]->lightRGB;
			gVertex[0].frgb		= vertices[0]->fogRGB;
			gVertex[0].frgb		= (gVertex[0].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[0]->pVertex->terrainType);

			gVertex[1].x		= vertices[1]->wx;
			gVertex[1].y		= vertices[1]->wy;
			gVertex[1].z		= vertices[1]->wz + WATER_DEPTH_FUDGE;
			gVertex[1].rhw		= vertices[1]->ww;
			gVertex[1].u		= maxU + cloudOffsetX;
			gVertex[1].v		= minV + cloudOffsetY;
			gVertex[1].argb		= vertices[1]->pVertex->selected ? SELECTION_COLOR : vertices[1]->lightRGB;
			gVertex[1].frgb		= vertices[1]->fogRGB;
			gVertex[1].frgb		= (gVertex[1].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[1]->pVertex->terrainType);

			gVertex[2].x		= vertices[2]->wx;
			gVertex[2].y		= vertices[2]->wy;
			gVertex[2].z		= vertices[2]->wz + WATER_DEPTH_FUDGE;
			gVertex[2].rhw		= vertices[2]->ww;
			gVertex[2].u		= maxU + cloudOffsetX;
			gVertex[2].v		= maxV + cloudOffsetY;
			gVertex[2].argb		= vertices[2]->pVertex->selected ? SELECTION_COLOR : vertices[2]->lightRGB;
			gVertex[2].frgb		= vertices[2]->fogRGB;
			gVertex[2].frgb		= (gVertex[2].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[2]->pVertex->terrainType);

			gVertex[0].u = (vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX;
			gVertex[0].v = (Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverTF + cloudOffsetY; 
																   
			gVertex[1].u = (vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[1].v = (Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverTF + cloudOffsetY; 
																   
			gVertex[2].u = (vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[2].v = (Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverTF + cloudOffsetY; 

			if ((gVertex[0].z >= 0.0f) &&
				(gVertex[0].z < 1.0f) &&
				(gVertex[1].z >= 0.0f) &&  
				(gVertex[1].z < 1.0f) && 
				(gVertex[2].z >= 0.0f) &&  
				(gVertex[2].z < 1.0f))
			{
				{
					//-----------------------------------------------------------------------------
					// Reject Any triangle which has vertices off screeen in software for now.
					// Do real cliping in geometry layer for software and hardware that needs it!
					if (waterHandle != 0xffffffff)
					{
						DWORD alphaMode0 = Terrain::alphaMiddle;
						DWORD alphaMode1 = Terrain::alphaMiddle;
						DWORD alphaMode2 = Terrain::alphaMiddle;
		
						if (vertices[0]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode0 = Terrain::alphaEdge;
						}
		
						if (vertices[1]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode1 = Terrain::alphaEdge;
						}
		
						if (vertices[2]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode2 = Terrain::alphaEdge;
						}
		
						if (vertices[0]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode0 = Terrain::alphaDeep;
						}
		
						if (vertices[1]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode1 = Terrain::alphaDeep;
						}
		
						if (vertices[2]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode2 = Terrain::alphaDeep;
						}
		
						gVertex[0].argb = (vertices[0]->lightRGB & 0x00ffffff) + alphaMode0;
						gVertex[1].argb = (vertices[1]->lightRGB & 0x00ffffff) + alphaMode1;
						gVertex[2].argb = (vertices[2]->lightRGB & 0x00ffffff) + alphaMode2;
		
						if ((gVertex[0].u > MaxMinUV) || 
							(gVertex[0].v > MaxMinUV) ||
							(gVertex[1].u > MaxMinUV) || 
							(gVertex[1].v > MaxMinUV) ||
							(gVertex[2].u > MaxMinUV) || 
							(gVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(gVertex[0].u,fmax(gVertex[1].u,gVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(gVertex[0].v,fmax(gVertex[1].v,gVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							gVertex[0].u -= maxU;
							gVertex[1].u -= maxU;
							gVertex[2].u -= maxU;
							
							gVertex[0].v -= maxV;
							gVertex[1].v -= maxV;
							gVertex[2].v -= maxV;
						}
						
						if (alphaMode0 + alphaMode1 + alphaMode2)
						{
							mcTextureManager->addVertices(waterHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATER);
						}
					}
						
					//----------------------------------------------------
					// Draw the sky reflection on the water.
					if (useWaterInterestTexture && (waterDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						sVertex[0].u		= ((vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[0].v		= ((Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[1].u		= ((vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[1].v		= ((Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[2].u		= ((vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[2].v		= ((Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[0].argb = (sVertex[0].argb & 0xff000000) + 0xffffff;
						sVertex[1].argb = (sVertex[1].argb & 0xff000000) + 0xffffff; 
						sVertex[2].argb = (sVertex[2].argb & 0xff000000) + 0xffffff; 

						mcTextureManager->addVertices(waterDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATERDETAIL);
					}
				}
 			}
   
			//--------------------------
			//Bottom Triangle
			//
			// gVertex[0] same as above gVertex[0].
			// gVertex[1] is same as above gVertex[2].
			// gVertex[2] is calced from vertex[3].
			gVertex[1].x		= gVertex[2].x;	
			gVertex[1].y		= gVertex[2].y;	
			gVertex[1].z		= gVertex[2].z;	
			gVertex[1].rhw		= gVertex[2].rhw;
			gVertex[1].u		= gVertex[2].u;
			gVertex[1].v		= gVertex[2].v;	
			gVertex[1].argb		= gVertex[2].argb;
			gVertex[1].frgb		= gVertex[2].frgb;

			gVertex[2].x		= vertices[3]->wx;
			gVertex[2].y		= vertices[3]->wy;
			gVertex[2].z		= vertices[3]->wz + WATER_DEPTH_FUDGE;
			gVertex[2].rhw		= vertices[3]->ww;
			gVertex[2].u		= minU + cloudOffsetX;
			gVertex[2].v		= maxV + cloudOffsetY;
			gVertex[2].argb		= vertices[3]->pVertex->selected ? SELECTION_COLOR : vertices[3]->lightRGB;
			gVertex[2].frgb		= vertices[3]->fogRGB;
			gVertex[2].frgb		= (gVertex[2].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[3]->pVertex->terrainType);

			gVertex[0].u = (vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX;
			gVertex[0].v = (Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverTF + cloudOffsetY;

			gVertex[1].u = (vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX;
			gVertex[1].v = (Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverTF + cloudOffsetY;

			gVertex[2].u = (vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX;
			gVertex[2].v = (Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverTF + cloudOffsetY;

			if ((gVertex[0].z >= 0.0f) &&
				(gVertex[0].z < 1.0f) &&
				(gVertex[1].z >= 0.0f) &&  
				(gVertex[1].z < 1.0f) && 
				(gVertex[2].z >= 0.0f) &&  
				(gVertex[2].z < 1.0f))
			{
				{
					//-----------------------------------------------------------------------------
					// Reject Any triangle which has vertices off screeen in software for now.
					// Do real cliping in geometry layer for software and hardware that needs it!
					if (waterHandle != 0xffffffff)
					{
	
						DWORD alphaMode0 = Terrain::alphaMiddle;
						DWORD alphaMode1 = Terrain::alphaMiddle;
						DWORD alphaMode2 = Terrain::alphaMiddle;
		
						if (vertices[0]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode0 = Terrain::alphaEdge;
						}
		
						if (vertices[2]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode1 = Terrain::alphaEdge;
						}
		
						if (vertices[3]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode2 = Terrain::alphaEdge;
						}
		
						if (vertices[0]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode0 = Terrain::alphaDeep;
						}
		
						if (vertices[2]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode1 = Terrain::alphaDeep;
						}
		
						if (vertices[3]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode2 = Terrain::alphaDeep;
						}
		
						gVertex[0].argb = (vertices[0]->lightRGB & 0x00ffffff) + alphaMode0;
						gVertex[1].argb = (vertices[2]->lightRGB & 0x00ffffff) + alphaMode1;
						gVertex[2].argb = (vertices[3]->lightRGB & 0x00ffffff) + alphaMode2;
		
						if ((gVertex[0].u > MaxMinUV) || 
							(gVertex[0].v > MaxMinUV) ||
							(gVertex[1].u > MaxMinUV) || 
							(gVertex[1].v > MaxMinUV) ||
							(gVertex[2].u > MaxMinUV) || 
							(gVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(gVertex[0].u,fmax(gVertex[1].u,gVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(gVertex[0].v,fmax(gVertex[1].v,gVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							gVertex[0].u -= maxU;
							gVertex[1].u -= maxU;
							gVertex[2].u -= maxU;
							
							gVertex[0].v -= maxV;
							gVertex[1].v -= maxV;
							gVertex[2].v -= maxV;
						}
						
						if (alphaMode0 + alphaMode1 + alphaMode2)
						{
							mcTextureManager->addVertices(waterHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATER);
						}
					}
						
					//----------------------------------------------------
					// Draw the sky reflection on the water.
					if (useWaterInterestTexture && (waterDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						sVertex[0].u		= ((vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[0].v		= ((Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[1].u		= ((vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[1].v		= ((Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[2].u		= ((vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[2].v		= ((Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[0].argb = (sVertex[0].argb & 0xff000000) + 0xffffff;
						sVertex[1].argb = (sVertex[1].argb & 0xff000000) + 0xffffff; 
						sVertex[2].argb = (sVertex[2].argb & 0xff000000) + 0xffffff; 

						mcTextureManager->addVertices(waterDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATERDETAIL);
					}
				}
			}
		}
		else if (uvMode == BOTTOMLEFT)
		{
			//------------------------------
			// Top Triangle.
			gVertex[0].x		= vertices[0]->wx;
			gVertex[0].y		= vertices[0]->wy;
			gVertex[0].z		= vertices[0]->wz + WATER_DEPTH_FUDGE;
			gVertex[0].rhw		= vertices[0]->ww;
			gVertex[0].u		= minU + cloudOffsetX;;
			gVertex[0].v		= minV + cloudOffsetY;;
			gVertex[0].argb		= vertices[0]->pVertex->selected ? SELECTION_COLOR : vertices[0]->lightRGB;
			gVertex[0].frgb		= vertices[0]->fogRGB;
			gVertex[0].frgb		= (gVertex[0].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[0]->pVertex->terrainType);

			gVertex[1].x		= vertices[1]->wx;
			gVertex[1].y		= vertices[1]->wy;
			gVertex[1].z		= vertices[1]->wz + WATER_DEPTH_FUDGE;
			gVertex[1].rhw		= vertices[1]->ww;
			gVertex[1].u		= maxU + cloudOffsetX;;
			gVertex[1].v		= minV + cloudOffsetY;;
			gVertex[1].argb		= vertices[1]->pVertex->selected ? SELECTION_COLOR : vertices[1]->lightRGB;
			gVertex[1].frgb		= vertices[1]->fogRGB;
			gVertex[1].frgb		= (gVertex[1].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[1]->pVertex->terrainType);

			gVertex[2].x		= vertices[3]->wx;
			gVertex[2].y		= vertices[3]->wy;
			gVertex[2].z		= vertices[3]->wz + WATER_DEPTH_FUDGE;
			gVertex[2].rhw		= vertices[3]->ww;
			gVertex[2].u		= minU + cloudOffsetX;;
			gVertex[2].v		= maxV + cloudOffsetY;;
			gVertex[2].argb		= vertices[3]->pVertex->selected ? SELECTION_COLOR : vertices[3]->lightRGB;
			gVertex[2].frgb		= vertices[3]->fogRGB;
			gVertex[2].frgb		= (gVertex[2].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[3]->pVertex->terrainType);

			gVertex[0].u = (vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX;
			gVertex[0].v = (Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverTF + cloudOffsetY;
																   
			gVertex[1].u = (vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[1].v = (Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverTF + cloudOffsetY; 
																   
			gVertex[2].u = (vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[2].v = (Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverTF + cloudOffsetY; 

			if ((gVertex[0].z >= 0.0f) &&
				(gVertex[0].z < 1.0f) &&
				(gVertex[1].z >= 0.0f) &&  
				(gVertex[1].z < 1.0f) && 
				(gVertex[2].z >= 0.0f) &&  
				(gVertex[2].z < 1.0f))
			{
				{
					//-----------------------------------------------------------------------------
					// Reject Any triangle which has vertices off screeen in software for now.
					// Do real cliping in geometry layer for software and hardware that needs it!
					if (waterHandle != 0xffffffff)
					{

						DWORD alphaMode0 = Terrain::alphaMiddle;
						DWORD alphaMode1 = Terrain::alphaMiddle;
						DWORD alphaMode2 = Terrain::alphaMiddle;
		
						if (vertices[0]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode0 = Terrain::alphaEdge;
						}
		
						if (vertices[1]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode1 = Terrain::alphaEdge;
						}
		
						if (vertices[3]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode2 = Terrain::alphaEdge;
						}
		
						if (vertices[0]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode0 = Terrain::alphaDeep;
						}
		
						if (vertices[1]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode1 = Terrain::alphaDeep;
						}
		
						if (vertices[3]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode2 = Terrain::alphaDeep;
						}
		
						gVertex[0].argb = (vertices[0]->lightRGB & 0x00ffffff) + alphaMode0;
						gVertex[1].argb = (vertices[1]->lightRGB & 0x00ffffff) + alphaMode1;
						gVertex[2].argb = (vertices[3]->lightRGB & 0x00ffffff) + alphaMode2;
		
						if ((gVertex[0].u > MaxMinUV) || 
							(gVertex[0].v > MaxMinUV) ||
							(gVertex[1].u > MaxMinUV) || 
							(gVertex[1].v > MaxMinUV) ||
							(gVertex[2].u > MaxMinUV) || 
							(gVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(gVertex[0].u,fmax(gVertex[1].u,gVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(gVertex[0].v,fmax(gVertex[1].v,gVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							gVertex[0].u -= maxU;
							gVertex[1].u -= maxU;
							gVertex[2].u -= maxU;
							
							gVertex[0].v -= maxV;
							gVertex[1].v -= maxV;
							gVertex[2].v -= maxV;
						}
						
						if (alphaMode0 + alphaMode1 + alphaMode2)
						{
							mcTextureManager->addVertices(waterHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATER);
						}
					}
						
					//----------------------------------------------------
					// Draw the sky reflection on the water.
					if (useWaterInterestTexture && (waterDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						sVertex[0].u		= ((vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[0].v		= ((Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[1].u		= ((vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[1].v		= ((Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[2].u		= ((vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[2].v		= ((Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[0].argb = (sVertex[0].argb & 0xff000000) + 0xffffff;
						sVertex[1].argb = (sVertex[1].argb & 0xff000000) + 0xffffff; 
						sVertex[2].argb = (sVertex[2].argb & 0xff000000) + 0xffffff; 

						mcTextureManager->addVertices(waterDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATERDETAIL);
					}
				}
			}

			//---------------------------------------
			// Bottom Triangle.
			// gVertex[0] is same as above gVertex[1]
			// gVertex[1] is new and calced from vertex[2].
			// gVertex[2] is same as above.
			gVertex[0].x		= gVertex[1].x;	
			gVertex[0].y		= gVertex[1].y;	
			gVertex[0].z		= gVertex[1].z;	
			gVertex[0].rhw		= gVertex[1].rhw;	
			gVertex[0].u		= gVertex[1].u;	
			gVertex[0].v		= gVertex[1].v;	
			gVertex[0].argb		= gVertex[1].argb;
			gVertex[0].frgb		= gVertex[1].frgb;

			gVertex[1].x		= vertices[2]->wx;
			gVertex[1].y		= vertices[2]->wy;
			gVertex[1].z		= vertices[2]->wz + WATER_DEPTH_FUDGE;
			gVertex[1].rhw		= vertices[2]->ww;
			gVertex[1].u		= maxU + cloudOffsetX;;
			gVertex[1].v		= maxV + cloudOffsetY;;
			gVertex[1].argb		= vertices[2]->pVertex->selected ? SELECTION_COLOR : vertices[2]->lightRGB;
			gVertex[1].frgb		= vertices[2]->fogRGB;
			gVertex[1].frgb		= (gVertex[1].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[2]->pVertex->terrainType);

			gVertex[0].u = (vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX;
			gVertex[0].v = (Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverTF + cloudOffsetY;
																   
			gVertex[1].u = (vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[1].v = (Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverTF + cloudOffsetY; 
																   
			gVertex[2].u = (vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[2].v = (Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverTF + cloudOffsetY; 
																   
			if ((gVertex[0].z >= 0.0f) &&
				(gVertex[0].z < 1.0f) &&
				(gVertex[1].z >= 0.0f) &&  
				(gVertex[1].z < 1.0f) && 
				(gVertex[2].z >= 0.0f) &&  
				(gVertex[2].z < 1.0f))
			{
				{
					//-----------------------------------------------------------------------------
					// Reject Any triangle which has vertices off screeen in software for now.
					// Do real cliping in geometry layer for software and hardware that needs it!
					if (waterHandle != 0xffffffff)
					{
	
						DWORD alphaMode0 = Terrain::alphaMiddle;
						DWORD alphaMode1 = Terrain::alphaMiddle;
						DWORD alphaMode2 = Terrain::alphaMiddle;
						
						if (vertices[1]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode0 = Terrain::alphaEdge;
						}
		
						if (vertices[2]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{									 
							alphaMode1 = Terrain::alphaEdge;
						}
		
						if (vertices[3]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode2 = Terrain::alphaEdge;
						}
						
						if (vertices[1]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode0 = Terrain::alphaDeep;
						}
		
						if (vertices[2]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode1 = Terrain::alphaDeep;
						}
		
						if (vertices[3]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode2 = Terrain::alphaDeep;
						}
		
						gVertex[0].argb = (vertices[1]->lightRGB & 0x00ffffff) + alphaMode0;
						gVertex[1].argb = (vertices[2]->lightRGB & 0x00ffffff) + alphaMode1;
						gVertex[2].argb = (vertices[3]->lightRGB & 0x00ffffff) + alphaMode2;
		
						if ((gVertex[0].u > MaxMinUV) || 
							(gVertex[0].v > MaxMinUV) ||
							(gVertex[1].u > MaxMinUV) || 
							(gVertex[1].v > MaxMinUV) ||
							(gVertex[2].u > MaxMinUV) || 
							(gVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(gVertex[0].u,fmax(gVertex[1].u,gVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(gVertex[0].v,fmax(gVertex[1].v,gVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							gVertex[0].u -= maxU;
							gVertex[1].u -= maxU;
							gVertex[2].u -= maxU;
							
							gVertex[0].v -= maxV;
							gVertex[1].v -= maxV;
							gVertex[2].v -= maxV;
						}
		 
						if (alphaMode0 + alphaMode1 + alphaMode2)
						{
							mcTextureManager->addVertices(waterHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATER);
						}
					}
						
					//----------------------------------------------------
					// Draw the sky reflection on the water.
					if (useWaterInterestTexture && (waterDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						sVertex[0].u		= ((vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[0].v		= ((Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[1].u		= ((vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[1].v		= ((Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[2].u		= ((vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[2].v		= ((Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[0].argb = (sVertex[0].argb & 0xff000000) + 0xffffff;
						sVertex[1].argb = (sVertex[1].argb & 0xff000000) + 0xffffff; 
						sVertex[2].argb = (sVertex[2].argb & 0xff000000) + 0xffffff; 

						mcTextureManager->addVertices(waterDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATERDETAIL);
					}
				}
			}
		}
	}
}

//---------------------------------------------------------------------------
long DrawDebugCells = 0;

void TerrainQuad::drawLine (void)
{
	long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[2]->clipInfo;
	long clipped2 = vertices[0]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;

	if (uvMode == BOTTOMLEFT)
	{
		clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[3]->clipInfo;
		clipped2 = vertices[1]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
	}

	//------------------------------------------------------------
	// Draw the Tile block lines at depth just above base tiles.
	// Reverse-Z (Step 6b): post-divide pz is now reverse-Z [near->1,
	// far->0] with GEQUAL; "just above base tiles" (toward camera) =
	// LARGER z, so the hardcoded -0.002f bias flips to +0.002f. Scene
	// debug-overlay consumer (drawTerrainGrid / selected-quad), not a
	// named-constant site; flipped explicitly.
	long color = XP_WHITE;

	if (uvMode == BOTTOMRIGHT)
	{
		if (clipped1 != 0)
		{
			Stuff::Vector4D pos1(vertices[0]->px,vertices[0]->py,vertices[0]->pz+0.002f,1.0f / vertices[0]->pw);
			Stuff::Vector4D pos2(vertices[1]->px,vertices[1]->py,vertices[1]->pz+0.002f,1.0f / vertices[1]->pw);
			
			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
			
			pos1.x = vertices[1]->px;
			pos1.y = vertices[1]->py;

			pos2.x = vertices[2]->px;
			pos2.y = vertices[2]->py;

			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
			
			pos1.x = vertices[2]->px;
			pos1.y = vertices[2]->py;
			pos1.z = vertices[2]->pz + 0.002f;

			pos2.x = vertices[0]->px;
			pos2.y = vertices[0]->py;
			pos2.z = vertices[0]->pz + 0.002f;

			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
		}
		
		if (clipped2 != 0)
		{
			Stuff::Vector4D pos1(vertices[0]->px,vertices[0]->py,vertices[0]->pz+0.002f,1.0f / vertices[0]->pw);
			Stuff::Vector4D pos2(vertices[2]->px,vertices[2]->py,vertices[2]->pz+0.002f,1.0f / vertices[2]->pw);
			
			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
			
			pos1.x = vertices[2]->px;
			pos1.y = vertices[2]->py;
			pos1.z = vertices[2]->pz + 0.002f;

			pos2.x = vertices[3]->px;
			pos2.y = vertices[3]->py;
			pos2.z = vertices[3]->pz + 0.002f;

			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
			
			pos1.x = vertices[3]->px;
			pos1.y = vertices[3]->py;
			pos1.z = vertices[3]->pz + 0.002f;

			pos2.x = vertices[0]->px;
			pos2.y = vertices[0]->py;
			pos2.z = vertices[0]->pz + 0.002f;

			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
		}
	}
	else
	{
		if (clipped1 != 0)
		{
			Stuff::Vector4D pos1(vertices[0]->px,vertices[0]->py,vertices[0]->pz+0.002f,1.0f / vertices[0]->pw);
			Stuff::Vector4D pos2(vertices[1]->px,vertices[1]->py,vertices[1]->pz+0.002f,1.0f / vertices[1]->pw);
			
			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
			
			pos1.x = vertices[1]->px;
			pos1.y = vertices[1]->py;
			pos1.z = vertices[1]->pz + 0.002f;

			pos2.x = vertices[3]->px;
			pos2.y = vertices[3]->py;
			pos2.z = vertices[3]->pz + 0.002f;

			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
			
			pos1.x = vertices[3]->px;
			pos1.y = vertices[3]->py;
			pos1.z = vertices[3]->pz + 0.002f;

			pos2.x = vertices[0]->px;
			pos2.y = vertices[0]->py;
			pos2.z = vertices[0]->pz + 0.002f;

			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
		}
		
		if (clipped2 != 0)
		{
			Stuff::Vector4D pos1(vertices[1]->px,vertices[1]->py,vertices[1]->pz+0.002f,1.0f / vertices[1]->pw);
			Stuff::Vector4D pos2(vertices[2]->px,vertices[2]->py,vertices[2]->pz+0.002f,1.0f / vertices[2]->pw);
			
			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
			
			pos1.x = vertices[2]->px;
			pos1.y = vertices[2]->py;
			pos1.z = vertices[2]->pz + 0.002f;

			pos2.x = vertices[3]->px;
			pos2.y = vertices[3]->py;
			pos2.z = vertices[3]->pz + 0.002f;

			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
			
			pos1.x = vertices[3]->px;
			pos1.y = vertices[3]->py;
			pos1.z = vertices[3]->pz + 0.002f;

			pos2.x = vertices[1]->px;
			pos2.y = vertices[1]->py;
			pos2.z = vertices[1]->pz + 0.002f;

			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
		}
	}

	//------------------------------------------------------------
	// Draw Movement Map Grid.
	//		Once movement is split up, turn this back on for editor -fs
	// I need ALL cells drawn to check elevation Code
	if (clipped1 != 0)
	{
		//--------------------------------------------------------------------
		// Display the ScenarioMap cell grid, as well, displaying open\blocked
		// states...
		float cellWidth = Terrain::worldUnitsPerVertex / MAPCELL_DIM;
		//cellWidth -= 5.0;
		
		long rowCol = vertices[0]->posTile;
		long tileR = rowCol>>16;
		long tileC = rowCol & 0x0000ffff;
				
		if (GameMap)
		{
			for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
			{
				for (long cellC = 0; cellC < MAPCELL_DIM; cellC++) 
				{
					long actualCellRow = tileR * MAPCELL_DIM + cellR;
					long actualCellCol = tileC * MAPCELL_DIM + cellC;
					
					MapCellPtr curCell = NULL;
					if (GameMap->inBounds(actualCellRow, actualCellCol))
						curCell = GameMap->getCell(actualCellRow, actualCellCol);
					
 					if (!curCell || 
						curCell->getDebug() || 
						!curCell->getPassable() || 
						curCell->getPathlock(0) || 
						curCell->getDeepWater() || 
						curCell->getShallowWater() ||
						curCell->getForest())
					{
						Stuff::Vector4D pos1;
						Stuff::Vector4D pos2;
						Stuff::Vector4D pos3;
						Stuff::Vector4D pos4;
						
						Stuff::Vector3D thePoint(vertices[0]->vx,vertices[0]->vy,vertices[0]->pVertex->elevation);
						
						thePoint.x += (cellC) * cellWidth;
						thePoint.y -= (cellR) * cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_cell_passability_0]
						eye->projectForDebugOverlay(thePoint,pos4);

						thePoint.x += cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_cell_passability_1]
						eye->projectForDebugOverlay(thePoint,pos1);

						thePoint.y -= cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_cell_passability_2]
						eye->projectForDebugOverlay(thePoint,pos2);

						thePoint.x -= cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_cell_passability_3]
						eye->projectForDebugOverlay(thePoint,pos3);

						pos1.z += 0.002f;
						pos2.z += 0.002f;
						pos3.z += 0.002f;
						pos4.z += 0.002f;

						DWORD color = XP_RED;
						
						if (!curCell)
						{
							color = XP_GREEN;
						}
						else if (curCell->getDebug() && DrawDebugCells)
						{
							color = XP_YELLOW;
						}
						else if (curCell->getPathlock(0))
						{
							color = XP_YELLOW;
						}
						else if (curCell->getForest())
						{
							color = SB_ORANGE;
						}
						else if (!curCell->getPassable())
						{
							color = SB_RED;
						}
						else if (curCell->getDeepWater())
						{
							color = SB_ORANGE;
						}
						else if (curCell->getShallowWater())
						{
							color = XP_BLUE;
						}

						{
							LineElement newElement(pos1,pos2,color,NULL);
							newElement.draw();
						}	
						
						{
							LineElement newElement(pos2,pos3,color,NULL);
							newElement.draw();
						}
						
						{
							LineElement newElement(pos3,pos4,color,NULL);
							newElement.draw();
						}
						
						{
							LineElement newElement(pos1,pos4,color,NULL);
							newElement.draw();
						}
					}
				}
			}
		}
		else if (drawEditorPassability && gEditorNavFlags)
		{
			// EditorNav overlay — one colored outline per terrain tile.
			// No GameMap dependency; works on maps of any size.
			long rowCol = vertices[0]->posTile;
			long tileR  = rowCol >> 16;
			long tileC  = rowCol & 0x0000ffff;

			if (tileR >= 0 && tileR < gEditorNavCellSide &&
			    tileC >= 0 && tileC < gEditorNavCellSide)
			{
				uint8_t f = gEditorNavFlags[tileR * gEditorNavCellSide + tileC];
				DWORD color;
				if      (f & EDITOR_NAV_BLOCKED)          color = SB_RED;
				else if (f & EDITOR_NAV_DEEP_WATER)       color = SB_ORANGE;
				else if (f & EDITOR_NAV_SHALLOW_WATER)    color = XP_BLUE;
				else if (f & EDITOR_NAV_GROUND_PASSABLE)  color = XP_GREEN;
				else                                       color = XP_RED;

				Stuff::Vector4D p0, p1, p2, p3;
				p0.x = vertices[0]->px; p0.y = vertices[0]->py; p0.z = vertices[0]->pz + 0.002f; p0.w = 1.f;
				p1.x = vertices[1]->px; p1.y = vertices[1]->py; p1.z = vertices[1]->pz + 0.002f; p1.w = 1.f;
				p2.x = vertices[2]->px; p2.y = vertices[2]->py; p2.z = vertices[2]->pz + 0.002f; p2.w = 1.f;
				p3.x = vertices[3]->px; p3.y = vertices[3]->py; p3.z = vertices[3]->pz + 0.002f; p3.w = 1.f;

				{ LineElement e(p0, p1, color, nullptr); e.draw(); }
				{ LineElement e(p1, p3, color, nullptr); e.draw(); }
				{ LineElement e(p3, p2, color, nullptr); e.draw(); }
				{ LineElement e(p2, p0, color, nullptr); e.draw(); }
			}
		}
	}

	if (GlobalMoveMap[0]->badLoad)
		return;

	if (clipped1 != 0)
	{
		float cellWidth = Terrain::worldUnitsPerVertex / MAPCELL_DIM;
		cellWidth -= 5.0;
				
		long rowCol = vertices[0]->posTile;
		long tileR = rowCol>>16;
		long tileC = rowCol & 0x0000ffff;
		
		long cellR = tileR * MAPCELL_DIM;
		long cellC = tileC * MAPCELL_DIM;
		
		for (long currentDoor = 0;currentDoor < GlobalMoveMap[0]->numDoors;currentDoor++)
		{
			if ((GlobalMoveMap[0]->doors[currentDoor].row >= cellR) && 
				(GlobalMoveMap[0]->doors[currentDoor].row < (cellR + MAPCELL_DIM)) &&
				(GlobalMoveMap[0]->doors[currentDoor].col >= cellC) &&
				(GlobalMoveMap[0]->doors[currentDoor].col < (cellC + MAPCELL_DIM)))
			{
				Stuff::Vector4D pos1;
				Stuff::Vector4D pos2;
				Stuff::Vector4D pos3;
				Stuff::Vector4D pos4;
					
				long xLength = 1;
				long yLength = 1;
				
				if (GlobalMoveMap[0]->doors[currentDoor].direction[0] == 1)
				{
					yLength = GlobalMoveMap[0]->doors[currentDoor].length;
				}
				
				if (GlobalMoveMap[0]->doors[currentDoor].direction[0] == 2)
				{
					xLength = GlobalMoveMap[0]->doors[currentDoor].length;
				}

				Stuff::Vector3D thePoint(vertices[0]->vx,vertices[0]->vy,vertices[0]->pVertex->elevation);
				
				thePoint.x += (GlobalMoveMap[0]->doors[currentDoor].col - cellC) * cellWidth;
				thePoint.y -= (GlobalMoveMap[0]->doors[currentDoor].row - cellR) * cellWidth;

				thePoint.z = land->getTerrainElevation(thePoint);
				// [PROJECTZ:DebugOnly id=debug_door_outline_0]
				eye->projectForDebugOverlay(thePoint,pos4);

				thePoint.x += (xLength) * cellWidth;
				thePoint.z = land->getTerrainElevation(thePoint);
				// [PROJECTZ:DebugOnly id=debug_door_outline_1]
				eye->projectForDebugOverlay(thePoint,pos1);

				thePoint.y -= (yLength) * cellWidth;
				thePoint.z = land->getTerrainElevation(thePoint);
				// [PROJECTZ:DebugOnly id=debug_door_outline_2]
				eye->projectForDebugOverlay(thePoint,pos2);

				thePoint.x -= (xLength) * cellWidth;
				thePoint.z = land->getTerrainElevation(thePoint);
				// [PROJECTZ:DebugOnly id=debug_door_outline_3]
				eye->projectForDebugOverlay(thePoint,pos3);

				pos1.z += 0.002f;
				pos2.z += 0.002f;
				pos3.z += 0.002f;
				pos4.z += 0.002f;
				{
					LineElement newElement(pos1,pos2,XP_GREEN,NULL);
					newElement.draw();
				}	
				
				{
					LineElement newElement(pos2,pos3,XP_GREEN,NULL);
					newElement.draw();
				}
				
				{
					LineElement newElement(pos3,pos4,XP_GREEN,NULL);
					newElement.draw();
				}
				
				{
					LineElement newElement(pos1,pos4,XP_GREEN,NULL);
					newElement.draw();
				}
			}
		}
	}
}

//-----------------------------------------------------------------------------------------------
void TerrainQuad::drawLOSLine (void)
{
	long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[2]->clipInfo;
	long clipped2 = vertices[0]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;

	if (uvMode == BOTTOMLEFT)
	{
		clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[3]->clipInfo;
		clipped2 = vertices[1]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
	}

	//------------------------------------------------------------
	// Draw the Tile block lines at depth just above base tiles.

	//------------------------------------------------------------
	// Draw LOS Map Grid.
	// Draw a color for LOS cell height data based on height.  Draw NOTHING if cell height is ZERO!!
	if (clipped1 != 0)
	{
		//--------------------------------------------------------------------
		float cellWidth = Terrain::worldUnitsPerVertex / MAPCELL_DIM;
		long rowCol = vertices[0]->posTile;
		long tileR = rowCol>>16;
		long tileC = rowCol & 0x0000ffff;
				
		if (GameMap)
		{
			for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
			{
				for (long cellC = 0; cellC < MAPCELL_DIM; cellC++) 
				{
					long actualCellRow = tileR * MAPCELL_DIM + cellR;
					long actualCellCol = tileC * MAPCELL_DIM + cellC;
					
					MapCellPtr curCell = NULL;
					if (GameMap->inBounds(actualCellRow, actualCellCol))
						curCell = GameMap->getCell(actualCellRow, actualCellCol);
					
 					if (curCell && curCell->getLocalHeight())
					{
						Stuff::Vector4D pos1;
						Stuff::Vector4D pos2;
						Stuff::Vector4D pos3;
						Stuff::Vector4D pos4;

						Stuff::Vector3D thePoint(vertices[0]->vx,vertices[0]->vy,vertices[0]->pVertex->elevation);

						thePoint.x += (cellC) * cellWidth;
						thePoint.y -= (cellR) * cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_los_cell_height_0]
						eye->projectForDebugOverlay(thePoint,pos4);

						thePoint.x += cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_los_cell_height_1]
						eye->projectForDebugOverlay(thePoint,pos1);

						thePoint.y -= cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_los_cell_height_2]
						eye->projectForDebugOverlay(thePoint,pos2);

						thePoint.x -= cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_los_cell_height_3]
						eye->projectForDebugOverlay(thePoint,pos3);
						
						pos1.z += 0.002f;
						pos2.z += 0.002f;
						pos3.z += 0.002f;
						pos4.z += 0.002f;

						DWORD color = XP_BLACK;
						
						if (curCell->getLocalHeight() < 2)
						{
							color = XP_BLACK;
						}
						else if (curCell->getLocalHeight() < 4)
						{
							color = XP_GRAY;
						}
						else if (curCell->getLocalHeight() < 6)
						{
							color = XP_RED;
						}
						else if (curCell->getLocalHeight() < 8)
						{
							color = XP_ORANGE;
						}
						else if (curCell->getLocalHeight() < 10)
						{
							color = XP_YELLOW;
						}
						else if (curCell->getLocalHeight() < 12)
						{
							color = XP_GREEN;
						}
						else if (curCell->getLocalHeight() < 14)
						{
							color = XP_BLUE;
						}
						else if (curCell->getLocalHeight() <= 16)
						{
							color = XP_WHITE;
						}

						{
							LineElement newElement(pos1,pos2,color,NULL);
							newElement.draw();
						}	
						
						{
							LineElement newElement(pos2,pos3,color,NULL);
							newElement.draw();
						}
						
						{
							LineElement newElement(pos3,pos4,color,NULL);
							newElement.draw();
						}
						
						{
							LineElement newElement(pos1,pos4,color,NULL);
							newElement.draw();
						}
					}
				}
			}
		}
	}
}

//---------------------------------------------------------------------------
void TerrainQuad::drawDebugCellLine (void)
{
	long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[2]->clipInfo;
	long clipped2 = vertices[0]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;

	if (uvMode == BOTTOMLEFT)
	{
		clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[3]->clipInfo;
		clipped2 = vertices[1]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
	}

	//------------------------------------------------------------
	// Draw the Tile block lines at depth just above base tiles.

	if (uvMode == BOTTOMRIGHT)
	{
		if (clipped1 != 0)
		{
			Stuff::Vector4D pos1(vertices[0]->px,vertices[0]->py,HUD_DEPTH,1.0f / vertices[0]->pw);
			Stuff::Vector4D pos2(vertices[1]->px,vertices[1]->py,HUD_DEPTH,1.0f / vertices[1]->pw);
			
			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[1]->px;
			pos1.y = vertices[1]->py;

			pos2.x = vertices[2]->px;
			pos2.y = vertices[2]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[2]->px;
			pos1.y = vertices[2]->py;

			pos2.x = vertices[0]->px;
			pos2.y = vertices[0]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
		}
		
		if (clipped2 != 0)
		{
			Stuff::Vector4D pos1(vertices[0]->px,vertices[0]->py,HUD_DEPTH,1.0f / vertices[0]->pw);
			Stuff::Vector4D pos2(vertices[2]->px,vertices[2]->py,HUD_DEPTH,1.0f / vertices[2]->pw);
			
			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[2]->px;
			pos1.y = vertices[2]->py;

			pos2.x = vertices[3]->px;
			pos2.y = vertices[3]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[3]->px;
			pos1.y = vertices[3]->py;

			pos2.x = vertices[0]->px;
			pos2.y = vertices[0]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
		}
	}
	else
	{
		if (clipped1 != 0)
		{
			Stuff::Vector4D pos1(vertices[0]->px,vertices[0]->py,HUD_DEPTH,1.0f / vertices[0]->pw);
			Stuff::Vector4D pos2(vertices[1]->px,vertices[1]->py,HUD_DEPTH,1.0f / vertices[1]->pw);
			
			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[1]->px;
			pos1.y = vertices[1]->py;

			pos2.x = vertices[3]->px;
			pos2.y = vertices[3]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[3]->px;
			pos1.y = vertices[3]->py;

			pos2.x = vertices[0]->px;
			pos2.y = vertices[0]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
		}
		
		if (clipped2 != 0)
		{
			Stuff::Vector4D pos1(vertices[1]->px,vertices[1]->py,HUD_DEPTH,1.0f / vertices[1]->pw);
			Stuff::Vector4D pos2(vertices[2]->px,vertices[2]->py,HUD_DEPTH,1.0f / vertices[2]->pw);
			
			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[2]->px;
			pos1.y = vertices[2]->py;

			pos2.x = vertices[3]->px;
			pos2.y = vertices[3]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[3]->px;
			pos1.y = vertices[3]->py;

			pos2.x = vertices[1]->px;
			pos2.y = vertices[1]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
		}
	}

	//------------------------------------------------------------
	// Draw Movement Map Grid.
	//		Once movement is split up, turn this back on for editor -fs
	// I need ALL cells drawn to check elevation Code
	if (clipped1 != 0)
	{
		//--------------------------------------------------------------------
		// Display the ScenarioMap cell grid, as well, displaying open\blocked
		// states...
		float cellWidth = Terrain::worldUnitsPerVertex / MAPCELL_DIM;
		//cellWidth -= 5.0;
		
		long rowCol = vertices[0]->posTile;
		long tileR = rowCol>>16;
		long tileC = rowCol & 0x0000ffff;
				
		if (GameMap)
		{
			for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
			{
				for (long cellC = 0; cellC < MAPCELL_DIM; cellC++) 
				{
					long actualCellRow = tileR * MAPCELL_DIM + cellR;
					long actualCellCol = tileC * MAPCELL_DIM + cellC;
					
					MapCellPtr curCell = NULL;
					if (GameMap->inBounds(actualCellRow, actualCellCol))
						curCell = GameMap->getCell(actualCellRow, actualCellCol);
					
 					if (!curCell || curCell->getDebug())
					{
						Stuff::Vector4D pos1;
						Stuff::Vector4D pos2;
						Stuff::Vector4D pos3;
						Stuff::Vector4D pos4;

						Stuff::Vector3D thePoint(vertices[0]->vx,vertices[0]->vy,vertices[0]->pVertex->elevation);

						thePoint.x += (cellC) * cellWidth;
						thePoint.y -= (cellR) * cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_cell_state_0]
						eye->projectForDebugOverlay(thePoint,pos4);

						thePoint.x += cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_cell_state_1]
						eye->projectForDebugOverlay(thePoint,pos1);

						thePoint.y -= cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_cell_state_2]
						eye->projectForDebugOverlay(thePoint,pos2);

						thePoint.x -= cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_cell_state_3]
						eye->projectForDebugOverlay(thePoint,pos3);

						pos1.z = pos2.z = pos3.z = pos4.z = HUD_DEPTH;

						DWORD color = XP_RED;
						if (!curCell)
						{
							color = XP_GREEN;
						}
						else {
							static DWORD debugColors[4] = {0, XP_RED, XP_WHITE, XP_BLUE};
							DWORD cellDebugValue = curCell->getDebug();
							if (cellDebugValue)
								color = debugColors[cellDebugValue];
						}
/*						else if (curCell->getPathlock())
						{
								color = XP_YELLOW;
						}
						else if (!curCell->getLineOfSight())
						{
					 		color = XP_BLUE;
						}
						else if (!curCell->getPassable())
						{
							color = SB_RED;
						}
*/
						{
							LineElement newElement(pos1,pos2,color,NULL);
							newElement.draw();
						}	
						
						{
							LineElement newElement(pos2,pos3,color,NULL);
							newElement.draw();
						}
						
						{
							LineElement newElement(pos3,pos4,color,NULL);
							newElement.draw();
						}
						
						{
							LineElement newElement(pos1,pos4,color,NULL);
							newElement.draw();
						}
					}
				}
			}
		}
	}


}

//---------------------------------------------------------------------------
void TerrainQuad::drawMine (void)
{
	// PR2c Stage 0c — bracket entire function. Counter once per call (per-quad).
	CostSplitMineDrawScope _csMineDraw;
	gos_terrain_indirect::Counters_AddLegacyMineDrawQuad();

	long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[2]->clipInfo;
	long clipped2 = vertices[0]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;

	if (uvMode == BOTTOMLEFT)
	{
		clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[3]->clipInfo;
		clipped2 = vertices[1]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
	}

	//------------------------------------------------------------
	// Draw Mines.
	// All mines are visible all the time!
	if ((clipped1 != 0) || (clipped2 != 0))
	{
		long cellPos = 0;
		float cellWidth = Terrain::worldUnitsPerCell;
		for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
		{
			for (long cellC = 0; cellC < MAPCELL_DIM; cellC++,cellPos++) 
			{
				//--------------------------------------------------------------------
				bool drawMine = false;
				bool drawBlownMine = false;
				
				if (mineResult.getMine(cellPos) == 1)
					drawMine = true;
								
				if (mineResult.getMine(cellPos) == 2)
					drawBlownMine = true;
								
				if (drawMine || drawBlownMine)
				{
					Stuff::Vector4D pos1;
					Stuff::Vector4D pos2;
					Stuff::Vector4D pos3;
					Stuff::Vector4D pos4;

					//------------------------------------------------------------------------------------
					// Dig the actual Vertex information out of the projected vertices already done.
					// In this way, the draw requires only interpolation and not Giant Matrix multiplies.
					Stuff::Vector3D thePoint(vertices[0]->vx,vertices[0]->vy,vertices[0]->pVertex->elevation);

					thePoint.x += (cellC) * cellWidth;
					thePoint.y -= (cellR) * cellWidth;
					thePoint.z = land->getTerrainElevation(thePoint);
					// [PROJECTZ:ScreenXYOracle id=mine_cell_corner0]
					eye->projectForScreenXY(thePoint,pos4);

					thePoint.x += cellWidth;
					thePoint.z = land->getTerrainElevation(thePoint);
					// [PROJECTZ:ScreenXYOracle id=mine_cell_corner1]
					eye->projectForScreenXY(thePoint,pos1);
					
					thePoint.y -= cellWidth;
					thePoint.z = land->getTerrainElevation(thePoint);
					// [PROJECTZ:ScreenXYOracle id=mine_cell_corner2]
					eye->projectForScreenXY(thePoint,pos2);

					thePoint.x -= cellWidth;
					thePoint.z = land->getTerrainElevation(thePoint);
					// [PROJECTZ:ScreenXYOracle id=mine_cell_corner3]
					eye->projectForScreenXY(thePoint,pos3);

					//------------------------------------
					// Replace with New RIA code
					gos_VERTEX gVertex[3];
					gos_VERTEX sVertex[3];
		
					gVertex[0].x        = sVertex[0].x		= pos1.x;
					gVertex[0].y        = sVertex[0].y		= pos1.y;
					gVertex[0].z        = sVertex[0].z		= pos1.z;
					gVertex[0].rhw      = sVertex[0].rhw	= pos1.w;
					gVertex[0].u        = sVertex[0].u		= 0.0f;
					gVertex[0].v        = sVertex[0].v		= 0.0f;
					gVertex[0].argb     = sVertex[0].argb	= vertices[0]->lightRGB;
					gVertex[0].frgb     = sVertex[0].frgb	= vertices[0]->fogRGB;
					{
						BYTE matIdx = terrainTypeToMaterial(vertices[0]->pVertex->terrainType);
						gVertex[0].frgb = (gVertex[0].frgb & 0xFFFFFF00) | matIdx;
						sVertex[0].frgb = (sVertex[0].frgb & 0xFFFFFF00) | matIdx;
					}

					gVertex[1].x		= pos2.x;
					gVertex[1].y		= pos2.y;
					gVertex[1].z		= pos2.z;
					gVertex[1].rhw		= pos2.w;
					gVertex[1].u		= 0.999999999f;
					gVertex[1].v		= 0.0f;
					gVertex[1].argb		= vertices[1]->lightRGB;
					gVertex[1].frgb		= vertices[1]->fogRGB;
					gVertex[1].frgb		= (gVertex[1].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[1]->pVertex->terrainType);

					gVertex[2].x        = sVertex[1].x		= pos3.x;
					gVertex[2].y        = sVertex[1].y		= pos3.y;
					gVertex[2].z        = sVertex[1].z		= pos3.z;
					gVertex[2].rhw      = sVertex[1].rhw	= pos3.w;
					gVertex[2].u        = sVertex[1].u		= 0.999999999f;
					gVertex[2].v        = sVertex[1].v		= 0.999999999f;
					gVertex[2].argb     = sVertex[1].argb	= vertices[2]->lightRGB;
					gVertex[2].frgb     = sVertex[1].frgb	= vertices[2]->fogRGB;
					{
						BYTE matIdx = terrainTypeToMaterial(vertices[2]->pVertex->terrainType);
						gVertex[2].frgb = (gVertex[2].frgb & 0xFFFFFF00) | matIdx;
						sVertex[1].frgb = (sVertex[1].frgb & 0xFFFFFF00) | matIdx;
					}

					sVertex[2].x		= pos4.x;
					sVertex[2].y		= pos4.y;
					sVertex[2].z		= pos4.z;
					sVertex[2].rhw		= pos4.w;
					sVertex[2].u		= 0.0f;
					sVertex[2].v		= 0.999999999f;
					sVertex[2].argb		= vertices[3]->lightRGB;
					sVertex[2].frgb		= vertices[3]->fogRGB;
					sVertex[2].frgb		= (sVertex[2].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[3]->pVertex->terrainType);
		
					if ((gVertex[0].z >= 0.0f) &&
						(gVertex[0].z < 1.0f) &&
						(gVertex[1].z >= 0.0f) &&  
						(gVertex[1].z < 1.0f) && 
						(gVertex[2].z >= 0.0f) &&  
						(gVertex[2].z < 1.0f) &&
						(sVertex[0].z >= 0.0f) &&
						(sVertex[0].z < 1.0f) &&
						(sVertex[1].z >= 0.0f) &&  
						(sVertex[1].z < 1.0f) && 
						(sVertex[2].z >= 0.0f) &&  
						(sVertex[2].z < 1.0f))
					{
						if (drawBlownMine)
						{
							mcTextureManager->addVertices(blownTextureHandle,gVertex,MC2_DRAWALPHA);
							mcTextureManager->addVertices(blownTextureHandle,sVertex,MC2_DRAWALPHA);
						}
						else
						{
							mcTextureManager->addVertices(mineTextureHandle,gVertex,MC2_DRAWALPHA);
							mcTextureManager->addVertices(mineTextureHandle,sVertex,MC2_DRAWALPHA);
						}
					}
				}
			}
		}
	}
}

//---------------------------------------------------------------------------

