// TerrainQuad: the editor-only legacy per-quad texture setup (setupTextures) with the terrain recipe cache and the projectZ overlay emit.
// Declared in the class headers; shared names in quad_internal.h.
#include "quad_internal.h"
#include "texture_assets.h"
#include "mc2fmt.h"


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
// Per-CPU-water-frame reset of the nearest-vertex search is done once/frame in
// Terrain::renderWater (terrain.cpp) BEFORE the drawWater quad loop -- the
// single legacy-loop entry where CPU water is the live producer. It clears
// g_cpuWaterProbeAny / g_cpuWaterProbeBestD2 and bumps g_cpuWaterProbeStamp.

static const bool s_shapeCParityCheck = mc2::gate::get<"MC2_SHAPE_C_PARITY_CHECK">();
// Default ON post Slice-1 flip (2026-04-29). Tier1 parity validated:
// 19.7M field-equality checks across 5 missions, zero mismatches.
// A value starting with "0" still opts out for bisection; any other value (including unset) opts in.
static const bool s_shapeCEnabled = mc2::gate::get<"MC2_MODERN_TERRAIN_PATCHES">();

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
				   siteTag, tileR, tileC, static_cast<int>(inlineR.method), static_cast<int>(e.method())); \
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
			mc2::fmt::print_out("[SHAPE_C] MISMATCH uvData site={} tileR={:d} tileC={:d} inline=({:.5f},{:.5f},{:.5f},{:.5f}) cached=({:.5f},{:.5f},{:.5f},{:.5f})\n", mc2::fmt::safe(siteTag), mc2::fmt::as_i32(tileR), mc2::fmt::as_i32(tileC), inlineR.uvData.minU, inlineR.uvData.minV, inlineR.uvData.maxU, inlineR.uvData.maxV, e.uvData.minU, e.uvData.minV, e.uvData.maxU, e.uvData.maxV);
		ok = false; ++s_mismatches;
	}

	// Summary every 10000 checks and on first check of session.
	if (s_checks == 1 || s_checks % 10000 == 0)
		mc2::fmt::print_out("[SHAPE_C] parity checks={:d} mismatches={:d}\n", mc2::fmt::as_i32(s_checks), mc2::fmt::as_i32(s_mismatches));
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
				mc2::renderSubmissions().addTriangleBulk(r.terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, 2);
				EndLegacySolidCluster();
			}
		}
	}
	else if (r.isAlpha)
	{
		if (r.terrainHandle != 0) {
			if (BeginLegacySolidCluster()) {
				CostSplitSolidScope _csSolid;
				mc2::renderSubmissions().addTriangleBulk(r.terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, 2);
				EndLegacySolidCluster();
			}
		}
	}
	else // pure cement
	{
		if (r.terrainHandle != 0) {
			if (BeginLegacySolidCluster()) {
				CostSplitSolidScope _csSolid;
				mc2::renderSubmissions().addTriangleBulk(r.terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, 2);
				EndLegacySolidCluster();
			}
		}
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
		mineTextureHandle = mc2::textures::load(mineTextureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink, 0, 0x1);
	}

	if (blownTextureHandle == 0xffffffff)
	{
		FullPathFileName mineTextureName;
		mineTextureName.init(texturePath,"defaults" PATH_SEPARATOR "minescorch_00",".tga");
		blownTextureHandle = mc2::textures::load(mineTextureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink, 0, 0x1);
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
	const bool legacyTerrainNeeded =
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
					mc2::renderSubmissions().addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
					mc2::renderSubmissions().addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
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
								mc2::renderSubmissions().addTriangle(mineTextureHandle, MC2_DRAWALPHA);
								mc2::renderSubmissions().addTriangle(mineTextureHandle, MC2_DRAWALPHA);
								}
								mineResult.setMine(cellPos,localResult);
							}
							else if (localResult == 2)
							{
								tex_resolve(blownTextureHandle);
								{ CostSplitDetailOverlayScope _csDetail;
								mc2::renderSubmissions().addTriangle(blownTextureHandle, MC2_DRAWALPHA);
								mc2::renderSubmissions().addTriangle(blownTextureHandle, MC2_DRAWALPHA);
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
					mc2::renderSubmissions().addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
					mc2::renderSubmissions().addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
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
								mc2::renderSubmissions().addTriangle(mineTextureHandle,MC2_DRAWALPHA);
								mc2::renderSubmissions().addTriangle(mineTextureHandle,MC2_DRAWALPHA);
								}
								mineResult.setMine(cellPos,localResult);
							}
							else if (localResult == 2)
							{
								tex_resolve(blownTextureHandle);
								{ CostSplitDetailOverlayScope _csDetail;
								mc2::renderSubmissions().addTriangle(blownTextureHandle,MC2_DRAWALPHA);
								mc2::renderSubmissions().addTriangle(blownTextureHandle,MC2_DRAWALPHA);
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
			const MapData::WorldQuadTerrainCacheEntry* cachedEntry = nullptr;
			{
				CostSplitCacheResidentScope _csRes;
				cachedEntry = Terrain::mapData ? Terrain::mapData->getTerrainFaceCacheEntry(tileR, tileC) : nullptr;
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
		glm::vec3 vertex3D(vertices[0]->vx,vertices[0]->vy,Terrain::waterElevation);
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

				mc2::renderSubmissions().addTriangleBulk(waterHandle, MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATER, 2);
				mc2::renderSubmissions().addTriangleBulk(waterDetailHandle, MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATERDETAIL, 2);
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
