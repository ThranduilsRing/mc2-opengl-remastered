// Terrain: Terrain::update with the LOD chunk selection and morph factor helpers.
// Declared in the class headers; shared names in terrain_internal.h.
#include "terrain_internal.h"
#include "mc2fmt.h"


// Phase 7.5 diagnostic: streak counter for frames where LOD chunk is enabled
// but zero draw commands are emitted (signals frustum/cull bug).
static int s_lodZeroCmdFrames = 0;

// ---------------------------------------------------------------------------
// Terrain LOD chunk Phase 5 — per-block distance LOD selection.
// LOD_STEPS[i] is the vertex stride baked into each patch VBO.
// LOD_DIST_THRESH[k] is the upper distance (world units) for lodLevel k.
// Each block is 20 quads * 128 wu/quad = 2560 wu per side.
// ---------------------------------------------------------------------------
static const int   LOD_STEPS[6]      = {1, 2, 4, 5, 10, 20};
// Doubled 2026-06-09 (was 3K/7K/15K/30K/60K) so LOD transitions sit well outside
// normal RTS view distance -> no visible pop-in at gameplay zoom. Runtime-scale
// with MC2_TERRAIN_LOD_CHUNK_DIST_SCALE (default 1.0; e.g. 0.5 restores the old
// thresholds, 2.0 pushes them even farther).
static const float LOD_DIST_THRESH[5] = {
    6000.0f,    // lodLevel 0: lodStep=1  — within 6 K wu
    14000.0f,   // lodLevel 1: lodStep=2  — within 14 K wu
    30000.0f,   // lodLevel 2: lodStep=4  — within 30 K wu
    60000.0f,   // lodLevel 3: lodStep=5  — within 60 K wu
    120000.0f,  // lodLevel 4: lodStep=10 — within 120 K wu
                // lodLevel 5: lodStep=20 — beyond 120 K wu
};

// Camera-isolation diagnostic: MC2_TERRAIN_LOD_CHUNK_FORCE_LOD=k forces every
// block to LOD k (0=finest). With FORCE_LOD=0 + NO_CULL + NO_SKIRTS, rotation
// in place MUST be stable — if it still breaks, the defect is in the
// shader/worldToClip/camera convention, NOT LOD/cull/skirt policy.
// (Shared by chooseLodLevel and computeMorphFactor — a forced LOD also forces
// morphFactor=0 so FORCE_LOD A/B captures show the PURE band surface.)
static int lodForceLevel()
{
    static const int s_forceLod = mc2::gate::get<"MC2_TERRAIN_LOD_CHUNK_FORCE_LOD">();
    return s_forceLod;
}

// Runtime distance scale (default 1.0). >1 pushes LOD transitions farther.
// MC2_TERRAIN_LOD_CHUNK_DIST_SCALE — shared by band selection and geomorph
// factor so the morph window tracks the scaled thresholds exactly.
static float lodDistScale()
{
    static const float s_distScale = mc2::gate::get<"MC2_TERRAIN_LOD_CHUNK_DIST_SCALE">();
    return s_distScale;
}

// Choose LOD level (0-5) for a block given squared distance and previous level.
// Promotion (going finer) is immediate; demotion (going coarser) uses 10%
// hysteresis in linear distance (= 1.21x in distSq) to prevent flickering.
static uint8_t chooseLodLevel(float distSq, uint8_t prevLevel)
{
    const int forceLod = lodForceLevel();
    if (forceLod >= 0)
        return (uint8_t)(forceLod > 5 ? 5 : forceLod);

    const float s_distScale = lodDistScale();

    uint8_t desired = 5;
    for (int k = 0; k < 5; ++k) {
        float t = LOD_DIST_THRESH[k] * s_distScale;
        if (distSq < t * t) {
            desired = (uint8_t)k;
            break;
        }
    }
    if (desired > prevLevel) {
        // Demotion — only commit if clearly past the current fine threshold.
        float thresh = LOD_DIST_THRESH[desired - 1] * s_distScale;
        if (distSq < thresh * thresh * 1.21f)
            return prevLevel; // stay fine
    }
    return desired;
}

// TERRAIN-LOD-GEOMORPH-1 rung a: per-block geomorph factor m in [0,1].
// m=0 at band entry (pure own-band max-mip surface), ramping to 1 over the
// OUTER (1 - MORPH_START) fraction of the band so the block's interior verts
// slide onto the parent band's surface BEFORE the demotion threshold — the
// band switch then lands on geometry that already matches (no one-frame snap).
// Promotion mirrors it: a freshly promoted block enters at m~1 (== the surface
// it just left) and slides down as the camera approaches. Levels 0 (mode-1
// fine path) and 5 (no coarser parent) never morph. Under FORCE_LOD the factor
// is pinned to 0 so A/B captures isolate the pure band surface. Consumed by
// the vert only when mips are resident (u_geomorphMips) — no mips, no morph.
static float computeMorphFactor(float distSq, uint8_t level)
{
    if (level == 0 || level >= 5) return 0.0f;
    if (lodForceLevel() >= 0)     return 0.0f;
    static const float s_morphStart = mc2::gate::get<"MC2_TERRAIN_LOD_MORPH_START">();
    const float sc = lodDistScale();
    const float lo = LOD_DIST_THRESH[level - 1] * sc;
    const float hi = LOD_DIST_THRESH[level] * sc;
    if (hi <= lo) return 0.0f;
    const float d = sqrtf(distSq);
    float r = (d - lo) / (hi - lo);            // 0..1 across the band (may exceed under hysteresis/clamps)
    float m = (r - s_morphStart) / (1.0f - s_morphStart);
    if (m < 0.0f) m = 0.0f;
    if (m > 1.0f) m = 1.0f;
    return m;
}

extern float textureOffset;
//---------------------------------------------------------------------------
long Terrain::update (void)
{
	ZoneScopedN("Terrain::update");

	//-----------------------------------------------------------------
	// TERRAIN-RUNTIME-API-1 parity self-test (gate MC2_TERRAIN_RUNTIME_PARITY=1).
	// Read-only, runs once. Proves the TerrainRuntime compat seam links and
	// returns byte-identical values to the legacy accessors it forwards to.
	// In-scope here to the inline getCellPos cell helper.
	// One-shot: evaluate the gate exactly once (first frame with a live grid),
	// so the OFF path costs nothing per frame after that.
	static bool s_runtimeParityChecked = false;
	if (!s_runtimeParityChecked && realVerticesMapSide > 0)
	{
		s_runtimeParityChecked = true;
		if (TerrainRuntime::parityGateEnabled())
		{
		const int cells[][2] = { {3,3}, {30,30}, {90,90}, {150,150} };
		int samples = 0, mismGame = 0, mismVis = 0, mismMat = 0, mismWater = 0;
		const float legacyWater = getWaterElevation();
		for (int i = 0; i < 4; ++i)
		{
			Stuff::Vector3D pos;
			getCellPos(cells[i][0], cells[i][1], pos);   // fills xy + z=legacy elevation
			if (!Terrain::IsValidTerrainPosition(pos))
				continue;
			++samples;
			const float legGame = getTerrainElevation(pos);
			const int   legMat  = (int)getTerrainType(pos);
			if (TerrainRuntime::sampleGameplayHeight(pos) != legGame)  ++mismGame;
			if (TerrainRuntime::sampleVisualHeight(pos)   != legGame)  ++mismVis;
			if (TerrainRuntime::sampleMaterialId(pos)     != legMat)   ++mismMat;
			if (TerrainRuntime::sampleWaterLevel(pos)     != legacyWater) ++mismWater;
		}
		printf("[TERRAIN_RUNTIME v1] parity self-test: side=%ld samples=%d "
		       "mismatch{gameplay=%d visual=%d material=%d water=%d}\n",
		       realVerticesMapSide, samples, mismGame, mismVis, mismMat, mismWater);
		fflush(stdout);
		} // gate-enabled
	} // one-shot

	//-----------------------------------------------------------------
	// Startup the Terrain Color Map
	if ( terrainTextures2  && !(terrainTextures2->colorMapStarted))
	{
		ZoneScopedN("Terrain::update startColorMap");
		if (colorMapName)
			terrainTextures2->init(colorMapName);
		else
			terrainTextures2->init(terrainName);
	}

	//----------------------------------------------------------------
	// Nothing is ever visible.  We recalc every frame.  True LOS!
//	Terrain::VisibleBits->resetAll(0);
		
	if (godMode)	
	{
//		Terrain::VisibleBits->resetAll(0xff);
	}

	if (turn > terrainLineChanged+10)
	{
		ZoneScopedN("Terrain::update debugHotkeys");
		if (userInput->getKeyDown(KEY_UP) && userInput->ctrl() && userInput->alt() && !userInput->shift())
		{
			textureOffset += 0.1f;;
			terrainLineChanged = turn;
		}
		
		if (userInput->getKeyDown(KEY_DOWN) && userInput->ctrl() && userInput->alt() && !userInput->shift())
		{
			textureOffset -= 0.1f;;
			terrainLineChanged = turn;
		}
	}
	
 	//---------------------------------------------------------------------
	{
		ZoneScopedN("Terrain::update mapDataUpdate");
		Terrain::mapData->update();
	}
	// Terrain LOD chunk Phase 4: skip legacy vertex/quad list build when chunk
	// path owns rendering. makeLists populates vertexList/quadList used by the
	// per-quad draw() loop; that loop is suppressed under the same env gate.
	// 8z-A1: MC2_TERRAIN_ACTIVE_AB / MC2_TERRAIN_SOLID_AB retired (8a/8b/8c shipped,
	// FN=0 validated); retirement-warn reader removed by GATE-RETIRE-1.
	if (!mc2TerrainLodChunkEnabled())
	{
		ZoneScopedN("Terrain::update makeLists");
		Terrain::mapData->makeLists(vertexList,numberVertices,quadList,numberQuads);
	}

	// -------------------------------------------------------------------------
	// Terrain LOD chunk Phase 4 — two-level AABB frustum cull.
	// MC2_TERRAIN_LOD_CHUNK=1 gate (s_blockMeta is nullptr when env unset).
	// Builds s_drawCmds[]; submitted via Terrain::flushDrawCommands() from
	// code/gamecam.cpp after shadow pass, before mcTextureManager->renderLists().
	if (s_blockMeta && s_superchunkMeta && s_drawCmds && eye)
	{
		ZoneScopedN("Terrain::update lodChunkCull");

		// Increment per-frame counter.
		++gCurrentFrame;

		// Reset draw-command list.
		s_cmdCount = 0;
		// Phase 10.2b: ensure the per-command skirt edge-mask array can hold one
		// entry per block (Pass 3 writes s_skirtEdgeMaskVec[s_cmdCount]).
		{
			const size_t need = (size_t)s_terrainChunkSide * (size_t)s_terrainChunkSide;
			if (s_skirtEdgeMaskVec.size() < need)
				s_skirtEdgeMaskVec.resize(need, 0);
			if (s_stitchStepVec.size() < need)
				s_stitchStepVec.resize(need, 0);
			if (s_shadowTierVec.size() < need)  // Slice B
				s_shadowTierVec.resize(need, 0);
			if (s_morphFactorVec.size() < need) // TERRAIN-LOD-GEOMORPH-1
				s_morphFactorVec.resize(need, 0.0f);
		}

		// Cache frustum planes once for this frame (eye->cacheFrustumPlanes()
		// is idempotent per-frame).
		eye->cacheFrustumPlanes();
		const float (*planes)[4] = eye->getCachedFrustumPlanes();

		const float halfMap = worldUnitsMapSide * 0.5f;

		// Camera position in MC2 world space for LOD distance metric.
		// getCameraOrigin() returns Stuff/MLR coords (.x=west, .y=elev, .z=north).
		// Terrain rendering space: east = -west, north = .z.
		// Use east/north for ground-plane LOD distance to block center.
		Stuff::Vector3D camOriginLod = eye->getCameraOrigin();
		const float eyeX = -camOriginLod.x;   // east = -west
		const float eyeY =  camOriginLod.z;   // north = Stuff/MLR .z

		// Diagnostic: one-shot dump of planes + first superchunk AABB + camera pos.
		// MC2_TERRAIN_LOD_CHUNK_NO_CULL=1 bypasses frustum so GPU path can be tested.
		static const bool s_noCull = mc2::gate::get<"MC2_TERRAIN_LOD_CHUNK_NO_CULL">();
		// [LOW-CAMERA-TERRAIN-CULL-1 / FIX-2] When set, drop the near plane from the
		// terrain AABB frustum test so near terrain isn't culled at a low/grazing
		// pitch (near plane at NearPlaneDistance=-400). Other 5 planes intact;
		// default OFF -> byte-identical to today.
		// Default ON for this low-camera build; set MC2_LOWCAM_TERRAIN_NEAR=0 to disable.
		static const bool s_lowCamNear = mc2::gate::get<"MC2_LOWCAM_TERRAIN_NEAR">();
		static bool s_cullDiagDone = false;
		if (!s_cullDiagDone && gCurrentFrame == 2) {
			s_cullDiagDone = true;
			const SuperchunkMeta& sc0 = s_superchunkMeta[0];
			mc2::fmt::print_out("[TerrainLOD v1] DIAG halfMap={:.1f} worldUnitsMapSide={:.1f}\n", halfMap, worldUnitsMapSide);
			mc2::fmt::print_out("[TerrainLOD v1] DIAG cam=({:.1f},{:.1f},{:.1f})\n", camOriginLod.x, camOriginLod.y, camOriginLod.z);
			mc2::fmt::print_out("[TerrainLOD v1] DIAG sc[0] AABB x=[{:.1f},{:.1f}] y=[{:.1f},{:.1f}] z=[{:.1f},{:.1f}]\n", sc0.worldMinX, sc0.worldMaxX, sc0.worldMinY, sc0.worldMaxY, sc0.worldMinZ, sc0.worldMaxZ);
			// Print all 6 planes with pass/fail for sc[0].
			static const char* planeNames[6] = {"left","right","bottom","top","near","far"};
			for (int p = 0; p < 6; p++) {
				float a = planes[p][0], b = planes[p][1], c = planes[p][2], d = planes[p][3];
				float px = (a >= 0.0f) ? sc0.worldMaxX : sc0.worldMinX;
				float py = (b >= 0.0f) ? sc0.worldMaxY : sc0.worldMinY;
				float pz = (c >= 0.0f) ? sc0.worldMaxZ : sc0.worldMinZ;
				float dot = a*px + b*py + c*pz + d;
				mc2::fmt::print_out("[TerrainLOD v1] DIAG plane[{:d}]={} ({:.4f},{:.4f},{:.4f},{:.4f}) pv=({:.1f},{:.1f},{:.1f}) dot={:.4f} {}\n", mc2::fmt::as_i32(p), mc2::fmt::safe(planeNames[p]), a, b, c, d, px, py, pz, dot, mc2::fmt::safe(dot>=0.0f?"PASS":"FAIL"));
			}
			mc2::fmt::print_out("[TerrainLOD v1] DIAG noCull={:d} superchunks={:d} blocks={:d}\n", mc2::fmt::as_i32((int)s_noCull), mc2::fmt::as_i32(s_superchunkSide * s_superchunkSide), mc2::fmt::as_i32(s_terrainChunkSide * s_terrainChunkSide));
			fflush(stdout);

			// --- 4-point vertex parity: legacy tileColToWorldCoord vs chunk formula ---
			// Confirms the two formulas agree on world position for 4 corner samples.
			// Points: (0,0), (side-1,0), (0,side-1), (side/2,side/2).
			if (mapData && mapData->getBlocks() && tileColToWorldCoord && tileRowToWorldCoord) {
				int side = (int)realVerticesMapSide;
				int samples[4][2] = {
					{0, 0},
					{side - 1, 0},
					{0, side - 1},
					{side / 2, side / 2}
				};
				const auto* blks = mapData->getBlocks();
				for (int i = 0; i < 4; i++) {
					int col = samples[i][0], row = samples[i][1];
					// Legacy formula (tileColToWorldCoord/tileRowToWorldCoord lookups).
					float legX = tileColToWorldCoord[col];  // col*128 - halfMap
					float legY = tileRowToWorldCoord[row];  // halfMap - row*128
					float legZ = blks[col + row * side].elevation;
					// Chunk shader formula (what the GLSL vertex shader computes).
					float chkX = float(col) * 128.0f - halfMap;
					float chkY = halfMap - float(row) * 128.0f;
					float chkZ = blks[col + row * side].elevation;
					mc2::fmt::print_out("[TerrainLOD parity] sample[{:d}]=({:d},{:d}) legacy=({:.1f},{:.1f},{:.1f}) chunk=({:.1f},{:.1f},{:.1f}) {}\n", mc2::fmt::as_i32(i), mc2::fmt::as_i32(col), mc2::fmt::as_i32(row), legX, legY, legZ, chkX, chkY, chkZ, mc2::fmt::safe((legX==chkX && legY==chkY && legZ==chkZ) ? "MATCH" : "MISMATCH"));
				}
				fflush(stdout);
			}
		}

		// Late-frame DIAG: steady-state planes when camera is live.
		static bool s_cullDiag120Done = false;
		if (!s_cullDiag120Done && gCurrentFrame == 120) {
			s_cullDiag120Done = true;
			const SuperchunkMeta& sc0 = s_superchunkMeta[0];
			mc2::fmt::print_out("[TerrainLOD v1] DIAG120 cam=({:.1f},{:.1f},{:.1f})\n", camOriginLod.x, camOriginLod.y, camOriginLod.z);
			mc2::fmt::print_out("[TerrainLOD v1] DIAG120 sc[0] AABB x=[{:.1f},{:.1f}] y=[{:.1f},{:.1f}] z=[{:.1f},{:.1f}]\n", sc0.worldMinX, sc0.worldMaxX, sc0.worldMinY, sc0.worldMaxY, sc0.worldMinZ, sc0.worldMaxZ);
			for (int p = 0; p < 6; p++) {
				float a = planes[p][0], b = planes[p][1], c = planes[p][2], d = planes[p][3];
				float px2 = (a >= 0.0f) ? sc0.worldMaxX : sc0.worldMinX;
				float py2 = (b >= 0.0f) ? sc0.worldMaxY : sc0.worldMinY;
				float pz2 = (c >= 0.0f) ? sc0.worldMaxZ : sc0.worldMinZ;
				float dot2 = a*px2 + b*py2 + c*pz2 + d;
				static const char* pn[6] = {"left","right","bottom","top","near","far"};
				mc2::fmt::print_out("[TerrainLOD v1] DIAG120 plane[{:d}]={} ({:.4f},{:.4f},{:.4f},{:.4f}) dot={:.4f} {}\n", mc2::fmt::as_i32(p), mc2::fmt::safe(pn[p]), a, b, c, d, dot2, mc2::fmt::safe(dot2>=0.0f?"PASS":"FAIL"));
			}
			// Center block AABB + per-plane test. sc[0] is top-left and may be
			// legitimately off-screen. Center block is the better "should always pass"
			// reference; if it also fails, the plane equation or AABB formula is wrong.
			int cbx = s_terrainChunkSide / 2, cby = s_terrainChunkSide / 2;
			const TerrainBlockMeta& cbm = s_blockMeta[cbx + cby * s_terrainChunkSide];
			float cbHalfMap = worldUnitsMapSide * 0.5f;
			Stuff::Vector3D cbMn, cbMx;
			cbMn.x = float(cbm.originX) * 128.0f - cbHalfMap;
			cbMx.x = float(cbm.originX + cbm.quadCountX) * 128.0f - cbHalfMap;
			cbMx.y = cbHalfMap - float(cbm.originY) * 128.0f;
			cbMn.y = cbHalfMap - float(cbm.originY + cbm.quadCountY) * 128.0f;
			cbMn.z = -200.0f; cbMx.z = 2500.0f;
			mc2::fmt::print_out("[TerrainLOD v1] DIAG120 center block=({:d},{:d}) originXY=({:d},{:d}) AABB x=[{:.1f},{:.1f}] y=[{:.1f},{:.1f}]\n", mc2::fmt::as_i32(cbx), mc2::fmt::as_i32(cby), mc2::fmt::as_i32(cbm.originX), mc2::fmt::as_i32(cbm.originY), cbMn.x, cbMx.x, cbMn.y, cbMx.y);
			static const char* pn2[6] = {"left","right","bottom","top","near","far"};
			for (int p = 0; p < 6; p++) {
				float a = planes[p][0], b = planes[p][1], c = planes[p][2], d = planes[p][3];
				float pxc = (a >= 0.0f) ? cbMx.x : cbMn.x;
				float pyc = (b >= 0.0f) ? cbMx.y : cbMn.y;
				float pzc = (c >= 0.0f) ? cbMx.z : cbMn.z;
				float dotc = a*pxc + b*pyc + c*pzc + d;
				mc2::fmt::print_out("[TerrainLOD v1] DIAG120 center plane[{:d}]={} dot={:.4f} {}\n", mc2::fmt::as_i32(p), mc2::fmt::safe(pn2[p]), dotc, mc2::fmt::safe(dotc>=0.0f?"PASS":"FAIL"));
			}
			fflush(stdout);
		}

		// --- Phase 5 Pass 1: superchunk cull + per-block frustum + LOD selection ---
		for (int scY = 0; scY < s_superchunkSide; ++scY)
		{
			for (int scX = 0; scX < s_superchunkSide; ++scX)
			{
				SuperchunkMeta& sc = s_superchunkMeta[scX + scY * s_superchunkSide];

				Stuff::Vector3D scMn, scMx;
				scMn.x = sc.worldMinX; scMn.y = sc.worldMinY; scMn.z = sc.worldMinZ;
				scMx.x = sc.worldMaxX; scMx.y = sc.worldMaxY; scMx.z = sc.worldMaxZ;

				// [LOW-CAMERA-TERRAIN-CULL-1 / FIX-2] skip-near variant when gated.
				sc.inFrustum = s_noCull ? true
					: (s_lowCamNear ? eye->quadAabbInFrustumSkipNear(planes, scMn, scMx)
					                : eye->quadAabbInFrustum(planes, scMn, scMx));
				if (!sc.inFrustum)
				{
					// Cull all constituent blocks without testing them.
					for (int dy = 0; dy < 4; ++dy)
					{
						for (int dx = 0; dx < 4; ++dx)
						{
							int bx = scX * 4 + dx, by = scY * 4 + dy;
							if (bx >= s_terrainChunkSide || by >= s_terrainChunkSide) continue;
							s_blockMeta[bx + by * s_terrainChunkSide].inFrustum = false;
						}
					}
					continue;
				}

				// --- Level 2: block cull + LOD selection within visible superchunk ---
				for (int dy = 0; dy < 4; ++dy)
				{
					for (int dx = 0; dx < 4; ++dx)
					{
						int bx = scX * 4 + dx, by = scY * 4 + dy;
						if (bx >= s_terrainChunkSide || by >= s_terrainChunkSide) continue;

						TerrainBlockMeta& bm = s_blockMeta[bx + by * s_terrainChunkSide];

						// Recompute dirty AABB before testing.
						if (bm.dirtyAabb)
							recomputeBlockAabb(bm);

						// World-space AABB (same formula as recomputeSuperchunkAabb).
						Stuff::Vector3D bmMn, bmMx;
						bmMn.x =  float(bm.originX)                  * 128.0f - halfMap;
						bmMx.x =  float(bm.originX + bm.quadCountX)  * 128.0f - halfMap;
						bmMx.y =  halfMap - float(bm.originY)                  * 128.0f;
						bmMn.y =  halfMap - float(bm.originY + bm.quadCountY)  * 128.0f;
						// Use conservative z-range matching old MC2_BLOCK_FRUSTUM_FALLBACK path
						// (kBlockZMin=-200, kBlockZMax=2500) so tight actual elevations don't cause
						// spurious frustum failure. Actual elevations still used for skirt depth below.
						bmMn.z = -200.0f;
						bmMx.z = 2500.0f;

						// CRIT-1: write inFrustum BEFORE any continue.
						// [LOW-CAMERA-TERRAIN-CULL-1 / FIX-2] skip-near variant when gated.
						bool passedCull = s_noCull ? true
							: (s_lowCamNear ? eye->quadAabbInFrustumSkipNear(planes, bmMn, bmMx)
							                : eye->quadAabbInFrustum(planes, bmMn, bmMx));
						bm.inFrustum = passedCull;

						// Phase 5: compute block center in MC2 world space and choose LOD.
						// Block center vertex = (originX + quadCountX*0.5, originY + quadCountY*0.5).
						// MC2 world: worldX = mapX * 128 - halfMap; worldY = halfMap - mapY * 128.
						float cX = float(bm.originX) * 128.0f + float(bm.quadCountX) * 0.5f * 128.0f - halfMap;
						float cY = halfMap - (float(bm.originY) * 128.0f + float(bm.quadCountY) * 0.5f * 128.0f);
						float dx2 = cX - eyeX, dy2 = cY - eyeY;
						float distSq = dx2 * dx2 + dy2 * dy2;
						bm.lodLevel = chooseLodLevel(distSq, bm.lodLevel);
					}
				}
			}
		}

		// --- Phase 10.2e: 1-ring terrain draw APRON ---
		// Render one extra block beyond the frustum-visible set so the cull
		// boundary does not show as a cliff / missing-terrain edge. Apron blocks
		// were skipped for LOD selection if their superchunk was culled, so assign
		// a LOD here; Pass 2 below then delta-clamps the dilated set (fewer seams).
		// RENDER-ONLY: the Phase 8 object-active / solid-window producers use an
		// independent angular test and never read s_blockMeta.inFrustum.
		// MC2_TERRAIN_LOD_CHUNK_NO_APRON=1 disables it (A/B).
		{
			static const bool s_noApron = mc2::gate::get<"MC2_TERRAIN_LOD_CHUNK_NO_APRON">();
			if (!s_noApron)
			{
				const int side = s_terrainChunkSide;
				static std::vector<uint8_t> s_visSnap;
				s_visSnap.assign((size_t)side * side, 0);
				for (int i = 0; i < side * side; ++i)
					s_visSnap[i] = s_blockMeta[i].inFrustum ? 1 : 0;
				for (int by = 0; by < side; ++by)
				for (int bx = 0; bx < side; ++bx)
				{
					if (s_visSnap[bx + by * side]) continue;   // already visible
					bool nearVis = false;
					for (int dy = -1; dy <= 1 && !nearVis; ++dy)
					for (int dx = -1; dx <= 1 && !nearVis; ++dx)
					{
						int nx = bx + dx, ny = by + dy;
						if (nx < 0 || ny < 0 || nx >= side || ny >= side) continue;
						if (s_visSnap[nx + ny * side]) nearVis = true;
					}
					if (!nearVis) continue;
					TerrainBlockMeta& bm = s_blockMeta[bx + by * side];
					if (bm.dirtyAabb) recomputeBlockAabb(bm);
					float cX = float(bm.originX) * 128.0f + float(bm.quadCountX) * 0.5f * 128.0f - halfMap;
					float cY = halfMap - (float(bm.originY) * 128.0f + float(bm.quadCountY) * 0.5f * 128.0f);
					float dx2 = cX - eyeX, dy2 = cY - eyeY;
					bm.lodLevel  = chooseLodLevel(dx2 * dx2 + dy2 * dy2, bm.lodLevel);
					bm.inFrustum = true;   // render this apron block
				}
			}
		}

		// --- Step 5c: clamp CONCRETE blocks to fine LOD ---
		// The cement word is per-tile; at coarse LOD a triangle spans several
		// tiles and samples one tile's cement-atlas layer -> torn/garbled runways.
		// Force concrete blocks to LOD 0 (per-tile accurate). Done BEFORE the delta
		// clamp so neighbors ramp down gracefully into the cement area.
		// MC2_TERRAIN_LOD_CHUNK_CEMENT_MAXLOD=k relaxes the cap (default 0).
		{
			static const int s_cementMaxLod = mc2::gate::get<"MC2_TERRAIN_LOD_CHUNK_CEMENT_MAXLOD">();
			for (int i = 0; i < s_terrainChunkSide * s_terrainChunkSide; ++i)
			{
				TerrainBlockMeta& bm = s_blockMeta[i];
				if (bm.inFrustum && bm.hasConcrete && bm.lodLevel > (unsigned char)s_cementMaxLod)
					bm.lodLevel = (unsigned char)s_cementMaxLod;
			}
		}

		// --- Phase 5 Pass 2: neighbor LOD delta clamp (visible blocks only) ---
		// Iterates until stable so chains (e.g., LOD0 next to LOD5) propagate.
		// BOUNDED: the LOD field is distance-smooth (neighbors normally already
		// differ by <=1) and the clamp only needs as many passes as the max LOD
		// delta (~6). But each pass re-sweeps ALL s_terrainChunkSide^2 blocks, so on
		// a LARGE map a single frame where a fast pan/zoom flips many blocks' LOD at
		// once can cascade to O(side^3) -> a multi-frame "lodChunkCull" hitch. Cap the
		// passes: a truncated clamp leaves at most a one-frame LOD seam (cosmetic; the
		// per-edge skirts/stitch below already backstop LOD mismatches), never a
		// hitch. Stock (5x5=25 blocks) settles in <=3 passes and never hits the cap.
		{
			const int kMaxClampPasses = 8;
			bool lodChanged = true;
			for (int clampPass = 0; clampPass < kMaxClampPasses && lodChanged; ++clampPass)
			{
				lodChanged = false;
				for (int by = 0; by < s_terrainChunkSide; ++by)
				{
					for (int bx = 0; bx < s_terrainChunkSide; ++bx)
					{
						TerrainBlockMeta& bm = s_blockMeta[bx + by * s_terrainChunkSide];
						if (!bm.inFrustum) continue;

						// Check 4 axis-aligned neighbors: E, W, S, N
						const int nbDx[4] = { 1, -1,  0,  0};
						const int nbDy[4] = { 0,  0,  1, -1};
						for (int n = 0; n < 4; ++n)
						{
							int nx = bx + nbDx[n], ny = by + nbDy[n];
							if (nx < 0 || nx >= s_terrainChunkSide) continue;
							if (ny < 0 || ny >= s_terrainChunkSide) continue;
							TerrainBlockMeta& nbm = s_blockMeta[nx + ny * s_terrainChunkSide];
							if (!nbm.inFrustum) continue;
							int delta = (int)bm.lodLevel - (int)nbm.lodLevel;
							if (delta > 1) {
								bm.lodLevel = nbm.lodLevel + 1;
								lodChanged = true;
							} else if (delta < -1) {
								nbm.lodLevel = bm.lodLevel + 1;
								lodChanged = true;
							}
						}
					}
				}
			}
		}

		// --- Slice B: per-chunk SHADOW TIER classification accumulators ---
		// Counts + world-area per tier over the chunks emitted this frame. Cheap
		// (a few ints + one tier compute per visible block). Logged only when
		// MC2_SHADOW_TIER_STATS is set; classification+vector-fill always runs.
		int    tierChunks[4] = {0, 0, 0, 0};      // tier 0/1/2/3 chunk counts
		double tierArea[4]   = {0.0, 0.0, 0.0, 0.0};

		// --- Slice B: shadow-tier cascade center + near-cascade outer radius. ---
		// The shadow tier is a coarse per-chunk proxy for receiver importance,
		// thresholded at the CSM cascade geometry (NOT camera-altitude LOD).
		// Cascade center == the shadow focus == eye->getPosition() (the orbit
		// target, MC2-world east/north; .x=east, .y=north). This matches the
		// CSM center fed to gos_BuildDynamicLightMatrix in txmmgr.cpp:2318-2319.
		// NOTE: eyeX/eyeY (used above for LOD) is getCameraOrigin() = the HIGH-UP
		// eye, NOT the target -- so we deliberately use getPosition() here so the
		// tier tracks the cascade center, not the camera altitude.
		// mc2ShadowCsmR0/R1() live in gos_postprocess.cpp (no header); declare
		// them extern so the tier tracks the same env (MC2_SHADOW_CSM_R0/R1) the
		// cascade radii use. Defaults R0=512, R1=4096.
		extern float mc2ShadowCsmR0();
		extern float mc2ShadowCsmR1();
		const Stuff::Vector3D shadowTierCenter = eye->getPosition();  // cascade center
		const float shadowR1 = mc2ShadowCsmR1();   // near-cascade outer radius (~4096)
		// Mid/far split: chunks past the near cascades but within a few R1 still
		// receive the full-map cascade at usable density -> tier 1 (coarse
		// dynamic); only the far tail is a static-only candidate. The 2.5x factor
		// is a starting guess kept as a clearly-labeled local for tuning.
		const float shadowMidFarMult = 2.5f;

		// --- Phase 5 Pass 3: emit draw commands using lodLevel -> lodStep ---
		// --- Phase 6: also compute per-block skirt depth (parallel array) ---
		for (int by = 0; by < s_terrainChunkSide; ++by)
		{
			for (int bx = 0; bx < s_terrainChunkSide; ++bx)
			{
				const TerrainBlockMeta& bm = s_blockMeta[bx + by * s_terrainChunkSide];
				if (!bm.inFrustum) continue;

				// --- Slice B: classify this visible chunk into a shadow tier. ---
				// DISTANCE-from-cascade-center heuristic (replaces lodLevel-based:
				// at MC2 zoom the camera maxes ~6400 WU altitude so lodLevel stays
				// 0-1 map-wide -> the whole map classified tier 0). Distance to the
				// shadow focus, thresholded at the CSM cascade radii, properly
				// discriminates near (crisp cascades) vs far (full-map cascade).
				// Chunk center XY uses the SAME conversion as the LOD site above
				// (worldX = mapX*128 - halfMap; worldY = halfMap - mapY*128).
				const float chunkCenterX =
					float(bm.originX) * 128.0f + float(bm.quadCountX) * 0.5f * 128.0f - halfMap;
				const float chunkCenterY =
					halfMap - (float(bm.originY) * 128.0f + float(bm.quadCountY) * 0.5f * 128.0f);
				const float stDx = chunkCenterX - shadowTierCenter.x;
				const float stDy = chunkCenterY - shadowTierCenter.y;
				const float stDist = sqrtf(stDx * stDx + stDy * stDy);
				int shadowTier;
				if (!bm.inFrustum)                              shadowTier = 3; // culled (completeness)
				else if (stDist < shadowR1)                     shadowTier = 0; // near cascades -> crisp dynamic
				else if (stDist < shadowR1 * shadowMidFarMult)  shadowTier = 1; // full-map cascade mid -> coarse dynamic
				else                                            shadowTier = 2; // far -> static-only candidate
				s_shadowTierVec[s_cmdCount] = shadowTier;
				// TERRAIN-LOD-GEOMORPH-1: geomorph factor from the block's FINAL
				// lodLevel (post cement/neighbor clamps) + center distance to the
				// SAME eye the band selection used (eyeX/eyeY, camera origin).
				// A block clamped finer than distance suggests simply rides at
				// m=1 (already on its parent surface) — still seam-safe because
				// perimeter verts never morph.
				{
					const float mfDx = chunkCenterX - eyeX;
					const float mfDy = chunkCenterY - eyeY;
					s_morphFactorVec[s_cmdCount] =
						computeMorphFactor(mfDx * mfDx + mfDy * mfDy, bm.lodLevel);
				}
				tierChunks[shadowTier]++;
				// World-area of this chunk = quads * (128*128) world-units^2.
				tierArea[shadowTier] +=
					(double)bm.quadCountX * (double)bm.quadCountY * (128.0 * 128.0);
				s_drawCmds[s_cmdCount].blockOriginX     = bm.originX;
				s_drawCmds[s_cmdCount].blockOriginY     = bm.originY;
				s_drawCmds[s_cmdCount].lodStep          = LOD_STEPS[bm.lodLevel];
				s_drawCmds[s_cmdCount].quadCountsPacked = (bm.quadCountX & 0xFF) | ((bm.quadCountY & 0xFF) << 8);
				// Phase 10.2: production skirt rule. A skirt only seals a real seam —
				// an LOD-mismatch crack or a cull-boundary drop. The Phase 6 debug
				// behavior (skirt on every edge, full depth) makes interior same-LOD
				// seams AND the map boundary render as cliff walls / floating slabs.
				// Emit a skirt only if an IN-MAP neighbor edge needs sealing:
				//   neighbor off-map (map boundary)     -> no skirt
				//   neighbor visible & same LOD         -> no skirt (coplanar, crack-free)
				//   neighbor LOD differs                -> skirt (LOD crack)
				//   neighbor not in-frustum (cull edge) -> skirt (fallback until 10.2e apron)
				// MC2_TERRAIN_LOD_CHUNK_NO_SKIRTS=1 forces all skirts off (A/B).
				{
					static const bool s_noSkirts = mc2::gate::get<"MC2_TERRAIN_LOD_CHUNK_NO_SKIRTS">();
					// Phase 10.2b: PER-EDGE mask. Build order N,S,W,E (matches the
					// driver's skirtEdgeOffset slots). A skirt is drawn on an edge
					// only if its neighbour needs sealing — same-LOD edges draw no
					// skirt (the source of the ridge slivers), map-boundary edges
					// draw none, and the 1-ring apron covers most cull edges.
					// Phase 10.4: ALSO pack the per-edge STITCH stride. For each edge
					// bordering a COARSER visible neighbour, the vertex shader snaps the
					// fine intermediate verts onto the coarse edge line so the shared
					// edge is bit-identical from both sides -> no T-junction crack
					// (neighbour-min, cf c2d7eb46). Skirts are KEPT as a backstop (a
					// skirt below a now-collinear edge is hidden, so it is harmless).
					// MC2_TERRAIN_LOD_CHUNK_NO_STITCH=1 disables stitching (A/B).
					static const bool s_noStitch = mc2::gate::get<"MC2_TERRAIN_LOD_CHUNK_NO_STITCH">();
					uint8_t  edgeMask = 0;
					uint32_t stitch   = 0;
					{
						const int edx[4] = {  0,  0, -1,  1 };  // N, S, W, E
						const int edy[4] = { -1,  1,  0,  0 };
						for (int e = 0; e < 4; ++e)
						{
							int nx = bx + edx[e], ny = by + edy[e];
							if (nx < 0 || ny < 0 ||
								nx >= s_terrainChunkSide || ny >= s_terrainChunkSide)
								continue;  // map boundary -> no skirt / no stitch
							const TerrainBlockMeta& nbm =
								s_blockMeta[nx + ny * s_terrainChunkSide];
							if (!s_noSkirts &&
								(!nbm.inFrustum || nbm.lodLevel != bm.lodLevel))
								edgeMask |= (uint8_t)(1u << e);  // skirt backstop
							if (!s_noStitch && nbm.inFrustum &&
								nbm.lodLevel > bm.lodLevel)      // coarser visible neighbour
								stitch |= ((uint32_t)(LOD_STEPS[nbm.lodLevel] & 0xFF))
										  << (e * 8);            // edge order N,S,W,E -> byte 0..3
						}
					}
					s_skirtEdgeMaskVec[s_cmdCount] = edgeMask;
					s_stitchStepVec[s_cmdCount]    = stitch;
					if (edgeMask)
					{
						// Skirt depth must cover the LOD CRACK (deviation of the fine
						// edge from the coarse neighbour's interpolated edge), which is
						// only a fraction of the block's full min->max elevation range.
						// Using the full range produced giant cliff walls on hilly
						// terrain (mountains: elevRange in the thousands). Scale down
						// and CAP. MC2_TERRAIN_LOD_CHUNK_SKIRT_MAX overrides the cap.
						static const float s_skirtMax = mc2::gate::get<"MC2_TERRAIN_LOD_CHUNK_SKIRT_MAX">();
						float elevRange = bm.maxElev - bm.minElev;
						float depth     = elevRange * 0.5f + 32.0f;
						if (depth > s_skirtMax) depth = s_skirtMax;
						if (depth < 32.0f)      depth = 32.0f;
						s_skirtDepths[s_cmdCount] = depth;
					}
					else
					{
						s_skirtDepths[s_cmdCount] = 0.0f;
					}
				}
				++s_cmdCount;
			}
		}

		// --- Slice B: SHADOW TIER stats (env-gated, ~once per 600 frames). ---
		// Cheap: a handful of ints/doubles accumulated in Pass 3 above, printed
		// only when MC2_SHADOW_TIER_STATS is set. Resets implicitly each frame
		// (tier* are frame-local). area%% is of total emitted visible terrain area.
		{
			static const bool s_shadowTierStats = mc2::gate::get<"MC2_SHADOW_TIER_STATS">();
			if (s_shadowTierStats && (gCurrentFrame % 600) == 0)
			{
				double totalArea = tierArea[0] + tierArea[1] + tierArea[2] + tierArea[3];
				double inv = (totalArea > 0.0) ? (100.0 / totalArea) : 0.0;
				mc2::fmt::print_out("[ShadowTier] high={:d} low={:d} static={:d} none={:d} | area% h={:.0f} l={:.0f} s={:.0f} (of visible terrain area)\n", mc2::fmt::as_i32(tierChunks[0]), mc2::fmt::as_i32(tierChunks[1]), mc2::fmt::as_i32(tierChunks[2]), mc2::fmt::as_i32(tierChunks[3]), tierArea[0] * inv, tierArea[1] * inv, tierArea[2] * inv);
				fflush(stdout);
			}
		}

		// Block-set-change diagnostic: log which (bx,by) blocks are visible whenever
		// the visible set changes. Fires on change only — no noise when camera is still.
		// Bitmask: bit = by*terrainChunkSide + bx (25 blocks, fits uint32_t).
		{
			uint32_t blockMask = 0;
			for (int by2 = 0; by2 < s_terrainChunkSide; ++by2)
				for (int bx2 = 0; bx2 < s_terrainChunkSide; ++bx2)
					if (s_blockMeta[bx2 + by2 * s_terrainChunkSide].inFrustum)
						blockMask |= (1u << (by2 * s_terrainChunkSide + bx2));
			static uint32_t s_prevBlockMask = 0xFFFFFFFF;  // force first print
			if (blockMask != s_prevBlockMask) {
				s_prevBlockMask = blockMask;
				// Build compact "(bx,by)" list of passing blocks.
				char buf[256]; int pos = 0;
				for (int by2 = 0; by2 < s_terrainChunkSide && pos < 200; ++by2)
					for (int bx2 = 0; bx2 < s_terrainChunkSide && pos < 200; ++bx2)
						if (blockMask & (1u << (by2 * s_terrainChunkSide + bx2)))
							pos += mc2::fmt::to_buf_n(buf + pos, sizeof(buf) - pos, "({:d},{:d})", mc2::fmt::as_i32(bx2), mc2::fmt::as_i32(by2));
				buf[pos] = '\0';
				// Camera east/north already computed above as eyeX/eyeY.
				printf("[BlockSet] frame=%lu cmds=%d cam=(%.0f,%.0f) blocks=%s\n",
					(unsigned long)gCurrentFrame, s_cmdCount, eyeX, eyeY, buf);
				fflush(stdout);
			}
		}

		// Phase 5/7.5 LOD telemetry — every frame for first 60 frames, then every 180.
		if ((gCurrentFrame <= 60 || gCurrentFrame % 180 == 0) && s_cmdCount > 0)
		{
			int lodCounts[6] = {};
			for (int ci = 0; ci < s_cmdCount; ++ci)
			{
				const int step = s_drawCmds[ci].lodStep;
				int lvl = (step == 1)  ? 0
				        : (step == 2)  ? 1
				        : (step == 4)  ? 2
				        : (step == 5)  ? 3
				        : (step == 10) ? 4 : 5;
				lodCounts[lvl]++;
			}
			mc2::fmt::print_out("[TerrainLOD v1] frame={:d} cmds={:d} LOD0={:d} LOD1={:d} LOD2={:d} LOD3={:d} LOD4={:d} LOD5={:d}\n", mc2::fmt::as_u32((unsigned long)gCurrentFrame), mc2::fmt::as_i32(s_cmdCount), mc2::fmt::as_i32(lodCounts[0]), mc2::fmt::as_i32(lodCounts[1]), mc2::fmt::as_i32(lodCounts[2]), mc2::fmt::as_i32(lodCounts[3]), mc2::fmt::as_i32(lodCounts[4]), mc2::fmt::as_i32(lodCounts[5]));
			fflush(stdout);
		}

		// One-shot: fires the first time cmds transitions from >0 to 0.
		// Prints all 6 frustum planes + camera position at the failure frame.
		{
			static bool s_wasNonZero   = false;
			static bool s_firstZeroDone = false;
			if (s_cmdCount > 0) s_wasNonZero = true;
			if (s_cmdCount == 0 && s_wasNonZero && !s_firstZeroDone) {
				s_firstZeroDone = true;
				mc2::fmt::print_out("[TerrainLOD v1] FIRST_ZERO frame={:d} cam=({:.1f},{:.1f},{:.1f})\n", mc2::fmt::as_u32((unsigned long)gCurrentFrame), camOriginLod.x, camOriginLod.y, camOriginLod.z);
				static const char* pnz[6] = {"left","right","bottom","top","near","far"};
				for (int p = 0; p < 6; p++) {
					float a = planes[p][0], b = planes[p][1], c = planes[p][2], d = planes[p][3];
					const SuperchunkMeta& sc0 = s_superchunkMeta[0];
					float px = (a >= 0.0f) ? sc0.worldMaxX : sc0.worldMinX;
					float py = (b >= 0.0f) ? sc0.worldMaxY : sc0.worldMinY;
					float pz = (c >= 0.0f) ? sc0.worldMaxZ : sc0.worldMinZ;
					float dot = a*px + b*py + c*pz + d;
					mc2::fmt::print_out("[TerrainLOD v1] FIRST_ZERO plane[{:d}]={} ({:.4f},{:.4f},{:.4f},{:.4f}) dot={:.4f} {}\n", mc2::fmt::as_i32(p), mc2::fmt::safe(pnz[p]), a, b, c, d, dot, mc2::fmt::safe(dot >= 0.0f ? "PASS" : "FAIL"));
				}
				fflush(stdout);
			}
		}

		// Phase 7.5 diagnostic: zero-command error detection.
		// If the chunk path is enabled but emits nothing after the first 10 frames,
		// log an error at 60 consecutive failures and every 300 thereafter.
		if (mc2TerrainLodChunkEnabled()) {
			if (s_cmdCount == 0 && gCurrentFrame > 10) {
				++s_lodZeroCmdFrames;
				if (s_lodZeroCmdFrames == 60 || s_lodZeroCmdFrames % 300 == 0)
					mc2::fmt::print_out("[TerrainLOD v1] ERROR: enabled but zero draw commands (frame={:d} streak={:d})\n", mc2::fmt::as_u32((unsigned long)gCurrentFrame), mc2::fmt::as_i32(s_lodZeroCmdFrames));
			} else {
				s_lodZeroCmdFrames = 0;
			}
		}

		// s_drawCmds[] is submitted in Terrain::flushDrawCommands() from gamecam.cpp.
	}

	// Set terrain light direction for normal map shader
	if (eye)
	{
		ZoneScopedN("Terrain::update cameraParams");
		// Light direction now set from gamecam.cpp with proper MC2->GL swizzle
		// gos_SetTerrainLightDir(eye->lightDirection.x, eye->lightDirection.y, eye->lightDirection.z);

		// Pass camera world position in raw MC2 space (matching vs_WorldPos for TCS distance LOD)
		Stuff::Vector3D camOrigin = eye->getCameraOrigin();
		gos_SetTerrainCameraPos(camOrigin.x, camOrigin.y, camOrigin.z);

		// Pass camera look direction for POM (direction camera looks toward terrain)
		Stuff::Vector3D lookDir = eye->getLookVector();
		// Swizzle same as camera pos, then normalize
		float lx = -lookDir.x, ly = lookDir.z, lz = lookDir.y;
		float len = sqrtf(lx*lx + ly*ly + lz*lz);
		if (len > 0.001f) { lx /= len; ly /= len; lz /= len; }
		gos_SetTerrainViewDir(lx, ly, lz);
	}

	return TRUE;
}
