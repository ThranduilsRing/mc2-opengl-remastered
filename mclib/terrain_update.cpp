// Terrain: Terrain::update with the LOD chunk selection helpers.
// Declared in the class headers; shared names in terrain_internal.h.
#include <algorithm>
#include "terrain_internal.h"
#include "terrain_lod_rules.h"
#include "mc2fmt.h"
#include "terrain_field_store.h"
#include "terrain_surface_cook.h"


// Phase 7.5 diagnostic: streak counter for frames where LOD chunk is enabled
// but zero draw commands are emitted (signals frustum/cull bug).
static int s_lodZeroCmdFrames = 0;

// ---------------------------------------------------------------------------
// Terrain LOD chunk: per-block screen-space-error LOD selection (terrain-sse-lod-2).
// LOD_STEPS[i] is the vertex stride (coarse cells) of level i; each block is
// 20 quads * 128 wu/quad = 2560 wu per side. The chooser and its error/morph
// rules live in terrain_lod_rules.h (doctest: tests/unit/test_terrain_lod_rules.cpp).
// ---------------------------------------------------------------------------
static const int (&LOD_STEPS)[mc2lod::kLodStepCount] = mc2lod::kLodSteps;

// Camera-isolation diagnostic: MC2_TERRAIN_LOD_CHUNK_FORCE_LOD=k forces every
// block to LOD k (0=finest). With FORCE_LOD=0 + NO_CULL + NO_SKIRTS, rotation
// in place MUST be stable — if it still breaks, the defect is in the
// shader/worldToClip/camera convention, NOT LOD/cull/skirt policy.
static int lodForceLevel()
{
    static const int s_forceLod = mc2::gate::get<"MC2_TERRAIN_LOD_CHUNK_FORCE_LOD">();
    return s_forceLod;
}

// Per-block level errors, against the 4x visual bake when one is loaded (that is
// what LOD0 draws), else against the coarse height field. Rebuilt when either
// height version changes. Render-only: heights and activity admission are never
// modified here.
struct BlockErrors { mc2lod::LevelErrors level; mc2lod::EdgeErrors edge; };
static const BlockErrors* blockLevelErrors(const TerrainBlockMeta& bm)
{
    static uint64_t cachedVersion = 0, cachedVisualVersion = 0;
    static int cachedSide = 0;
    static bool cachedBake = false;
    static float cachedWater = 0.0f;
    static std::vector<BlockErrors> errors;
    const auto* store = gos_TerrainFieldStore();
    if (!store || !store->heightVersion()) return nullptr;
    const int side = store->mapSide();
    const bool bake = !store->visualHeight().empty() && store->visualSide() > 1;
    const int blockSide = (side - 1 + 19) / 20;
    const float water = Terrain::waterElevation;   // levels that move the coastline never pass (mclib/terrain_lod_rules.h)
    if (cachedVersion != store->heightVersion() || cachedSide != side || cachedBake != bake || cachedWater != water
        || (bake && cachedVisualVersion != store->visualHeightVersion())) {
        errors.assign(static_cast<size_t>(blockSide) * blockSide, BlockErrors{});
        const float* ref = bake ? store->visualHeight().data() : store->height().data();
        const int refSide = bake ? store->visualSide() : side;
        for (int y = 0; y < blockSide; ++y) for (int x = 0; x < blockSide; ++x) {
            const int qx = std::min(20, side - 1 - x * 20);
            const int qy = std::min(20, side - 1 - y * 20);
            errors[x + y * blockSide] = {mc2lod::levelErrors(ref, refSide, bake ? 4 : 1, x * 20, y * 20, qx, qy, water),
                                         mc2lod::edgeErrors(ref, refSide, bake ? 4 : 1, x * 20, y * 20, qx, qy, water)};
        }
        cachedWater = water;
        cachedVersion = store->heightVersion();
        cachedVisualVersion = store->visualHeightVersion();
        cachedSide = side;
        cachedBake = bake;
    }
    const size_t i = static_cast<size_t>(bm.originX / 20) + static_cast<size_t>(bm.originY / 20) * static_cast<size_t>(blockSide);
    return i < errors.size() ? &errors[i] : nullptr;
}

// Sets bm.lodLevel and bm.morph from the block's projected geometric error.
static void chooseProjectedLod(TerrainBlockMeta& bm, float halfMap, Camera* camera)
{
    bm.morph = 0;
    bm.sseNumerator = 0.0f;   // 0 = no projection this frame (forced / unknown): the stitch pass leaves it alone
    if (lodForceLevel() >= 0) { bm.lodLevel = static_cast<uint8_t>(std::min(lodForceLevel(), 5)); return; }
    const BlockErrors* errors = blockLevelErrors(bm);
    if (!errors) { bm.lodLevel = 0; return; }
    // Projection derivatives use the camera's own transform and pixel convention
    // (live FOV and viewport). The absolute numerator is affine over the box; its
    // extrema are at corners.
    Stuff::Vector4D zero, up;
    zero.MultiplyPoint(glm::vec3(0, 0, 0), camera->getWorldToClip());
    up.MultiplyPoint(glm::vec3(0, 1, 0), camera->getWorldToClip());
    const float ax = up.x - zero.x, ay = up.y - zero.y, aw = up.w - zero.w;
    float minW = 1e30f, maxW = -1e30f, nx = 0, ny = 0;
    for (int corner = 0; corner < 8; ++corner) {
        const float x = (bm.originX + ((corner & 1) ? bm.quadCountX : 0)) * 128.0f - halfMap;
        const float y = halfMap - (bm.originY + ((corner & 2) ? bm.quadCountY : 0)) * 128.0f;
        const float z = (corner & 4) ? bm.cullZMax : bm.cullZMin;
        Stuff::Vector4D clip;
        clip.MultiplyPoint(glm::vec3(-x, z, y), camera->getWorldToClip());
        minW = std::min(minW, clip.w);
        maxW = std::max(maxW, clip.w);
        nx = std::max(nx, std::abs(ax * clip.w - clip.x * aw));
        ny = std::max(ny, std::abs(ay * clip.w - clip.y * aw));
    }
    if (minW <= 0 && maxW >= 0) { bm.lodLevel = 0; return; }   // camera plane crosses the block
    const float w = std::min(std::abs(minW), std::abs(maxW));
    const float numerator = std::max(nx * camera->fgetScreenResX(), ny * camera->fgetScreenResY());
    const float tol = bm.hasConcrete ? mc2lod::kSseConcreteTolerancePx : mc2lod::kSseTolerancePx;
    const mc2lod::LodPick pick = mc2lod::chooseSseLevel(errors->level, bm.lodLevel, tol, numerator, w, std::abs(aw));
    bm.sseNumerator = numerator;
    bm.sseW = w;
    bm.sseAbsAw = std::abs(aw);
    bm.lodLevel = static_cast<uint8_t>(pick.level);
    bm.morph = static_cast<uint8_t>(pick.morph * 255.0f + 0.5f);
}

// Stitch error is in the bound: true when the shared edge (bm's edge e, order N,S,W,E) of a pair at different levels,
// drawn at the stitch stride, projects above either block's tolerance.
static bool stitchOverTolerance(const TerrainBlockMeta& bm, const TerrainBlockMeta& nbm, int e)
{
    const std::optional<int> k = mc2lod::stitchErrorLevel(bm.lodLevel, nbm.lodLevel, (e < 2) ? bm.quadCountX : bm.quadCountY);
    const BlockErrors* errors = k ? blockLevelErrors(bm) : nullptr;
    if (!errors) return false;
    const float edgeError = errors->edge[e][*k];
    for (const TerrainBlockMeta* b : {&bm, &nbm}) {
        const float tol = b->hasConcrete ? mc2lod::kSseConcreteTolerancePx : mc2lod::kSseTolerancePx;
        if (b->sseNumerator > 0.0f && mc2lod::projectedErrorPx(edgeError, b->sseNumerator, b->sseW, b->sseAbsAw) > tol)
            return true;
    }
    return false;
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
		for (auto cell : cells)
		{
			glm::vec3 pos;
			getCellPos(cell[0], cell[1], pos);   // fills xy + z=legacy elevation
			if (!Terrain::IsValidTerrainPosition(pos))
				continue;
			++samples;
			const float legGame = getTerrainElevation(pos);
			const int   legMat  = static_cast<int>(getTerrainType(pos));
			if (TerrainRuntime::sampleGameplayHeight(pos) != legGame)  ++mismGame;
			if (TerrainRuntime::sampleVisualHeight(pos)   != legGame)  ++mismVis;
			if (TerrainRuntime::sampleMaterialId(pos)     != legMat)   ++mismMat;
			if (TerrainRuntime::sampleWaterLevel(pos)     != legacyWater) ++mismWater;
		}
		mc2::fmt::print_out("[TERRAIN_RUNTIME v1] parity self-test: side={:d} samples={:d} mismatch{{gameplay={:d} visual={:d} material={:d} water={:d}}}\n", mc2::fmt::as_i32(realVerticesMapSide), mc2::fmt::as_i32(samples), mc2::fmt::as_i32(mismGame), mc2::fmt::as_i32(mismVis), mc2::fmt::as_i32(mismMat), mc2::fmt::as_i32(mismWater));
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
	// code/gamecam.cpp after shadow pass, before mc2::VkSceneRenderer::renderLists(mc2::renderSubmissions()).
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
			const size_t need = static_cast<size_t>(s_terrainChunkSide) * static_cast<size_t>(s_terrainChunkSide);
			if (s_skirtEdgeMaskVec.size() < need)
				s_skirtEdgeMaskVec.resize(need, 0);
			if (s_stitchStepVec.size() < need)
				s_stitchStepVec.resize(need, 0);
			if (s_morphVec.size() < need)
				s_morphVec.resize(need, 0);
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
		glm::vec3 camOriginLod = eye->getCameraOrigin();
		const float eyeX = -camOriginLod.x;   // east = -west
		const float eyeY =  camOriginLod.z;   // north = Stuff/MLR .z

		// Diagnostic: one-shot dump of planes + first superchunk AABB + camera pos.
		// MC2_TERRAIN_LOD_CHUNK_NO_CULL=1 bypasses frustum so GPU path can be tested.
		static const bool s_noCull = false;
		// [LOW-CAMERA-TERRAIN-CULL-1 / FIX-2] When set, drop the near plane from the
		// terrain AABB frustum test so near terrain isn't culled at a low/grazing
		// pitch (near plane at NearPlaneDistance=-400). Other 5 planes intact;
		// default OFF -> byte-identical to today.
		// Default ON for this low-camera build; set MC2_LOWCAM_TERRAIN_NEAR=0 to disable.
		static const bool s_lowCamNear = true;
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
			mc2::fmt::print_out("[TerrainLOD v1] DIAG noCull={:d} superchunks={:d} blocks={:d}\n", mc2::fmt::as_i32(static_cast<int>(s_noCull)), mc2::fmt::as_i32(s_superchunkSide * s_superchunkSide), mc2::fmt::as_i32(s_terrainChunkSide * s_terrainChunkSide));
			fflush(stdout);

			// --- 4-point vertex parity: legacy tileColToWorldCoord vs chunk formula ---
			// Confirms the two formulas agree on world position for 4 corner samples.
			// Points: (0,0), (side-1,0), (0,side-1), (side/2,side/2).
			if (mapData && mapData->getBlocks() && tileColToWorldCoord && tileRowToWorldCoord) {
				int side = static_cast<int>(realVerticesMapSide);
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
					float chkX = static_cast<float>(col) * 128.0f - halfMap;
					float chkY = halfMap - static_cast<float>(row) * 128.0f;
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
			glm::vec3 cbMn, cbMx;
			cbMn.x = static_cast<float>(cbm.originX) * 128.0f - cbHalfMap;
			cbMx.x = static_cast<float>(cbm.originX + cbm.quadCountX) * 128.0f - cbHalfMap;
			cbMx.y = cbHalfMap - static_cast<float>(cbm.originY) * 128.0f;
			cbMn.y = cbHalfMap - static_cast<float>(cbm.originY + cbm.quadCountY) * 128.0f;
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

		// Cull boxes carry the real z range (gameplay + baked heights): refresh them when the bake lands or a block was edited.
		{
			static uint64_t s_cullVisVer = ~0ull;
			const auto* fieldStore = gos_TerrainFieldStore();
			const uint64_t visVer = fieldStore ? fieldStore->visualHeightVersion() : 0;
			bool refreshed = (visVer != s_cullVisVer);
			const int blockCount = s_terrainChunkSide * s_terrainChunkSide;
			for (int bi = 0; bi < blockCount; ++bi) {
				TerrainBlockMeta& rb = s_blockMeta[bi];
				if (refreshed || rb.dirtyAabb) {
					recomputeBlockAabb(rb);
					refreshed = true;
				}
			}
			if (refreshed) {
				for (int scY = 0; scY < s_superchunkSide; ++scY)
					for (int scX = 0; scX < s_superchunkSide; ++scX)
						recomputeSuperchunkAabb(scX, scY);
			}
			s_cullVisVer = visVer;
		}

		// --- Phase 5 Pass 1: superchunk cull + per-block frustum + LOD selection ---
		for (int scY = 0; scY < s_superchunkSide; ++scY)
		{
			for (int scX = 0; scX < s_superchunkSide; ++scX)
			{
				SuperchunkMeta& sc = s_superchunkMeta[scX + scY * s_superchunkSide];

				glm::vec3 scMn, scMx;
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
						glm::vec3 bmMn, bmMx;
						bmMn.x =  static_cast<float>(bm.originX)                  * 128.0f - halfMap;
						bmMx.x =  static_cast<float>(bm.originX + bm.quadCountX)  * 128.0f - halfMap;
						bmMx.y =  halfMap - static_cast<float>(bm.originY)                  * 128.0f;
						bmMn.y =  halfMap - static_cast<float>(bm.originY + bm.quadCountY)  * 128.0f;
						// Real per-block z range (gameplay + baked 4x heights + skirt/decal margin).
						bmMn.z = bm.cullZMin;
						bmMx.z = bm.cullZMax;

						// CRIT-1: write inFrustum BEFORE any continue.
						// [LOW-CAMERA-TERRAIN-CULL-1 / FIX-2] skip-near variant when gated.
						bool passedCull = s_noCull ? true
							: (s_lowCamNear ? eye->quadAabbInFrustumSkipNear(planes, bmMn, bmMx)
							                : eye->quadAabbInFrustum(planes, bmMn, bmMx));
						bm.inFrustum = passedCull;


						chooseProjectedLod(bm, halfMap, eye);
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
			static const bool s_noApron = false;
			if (!s_noApron)
			{
				const int side = s_terrainChunkSide;
				static std::vector<uint8_t> s_visSnap;
				s_visSnap.assign(static_cast<size_t>(side) * side, 0);
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

					chooseProjectedLod(bm, halfMap, eye);
					bm.inFrustum = true;   // render this apron block
				}
			}
		}

		// Cement identity is resolved per fragment; the same geometric error bound
		// protects its depth/contact surface without a material-driven density clamp.

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
						const int kEdgeOfNeighbour[4] = { 3, 2, 1, 0 };   // E, W, S, N -> stitch edge index (N,S,W,E)
						for (int n = 0; n < 4; ++n)
						{
							int nx = bx + nbDx[n], ny = by + nbDy[n];
							if (nx < 0 || nx >= s_terrainChunkSide) continue;
							if (ny < 0 || ny >= s_terrainChunkSide) continue;
							TerrainBlockMeta& nbm = s_blockMeta[nx + ny * s_terrainChunkSide];
							if (!nbm.inFrustum) continue;
							int delta = static_cast<int>(bm.lodLevel) - static_cast<int>(nbm.lodLevel);
							if (delta > 1) {
								bm.lodLevel = nbm.lodLevel + 1;
								bm.morph = 255;    // forced finer than its error asks: draw the next coarser surface
								lodChanged = true;
							} else if (delta < -1) {
								nbm.lodLevel = bm.lodLevel + 1;
								nbm.morph = 255;
								lodChanged = true;
							} else if (delta != 0 && stitchOverTolerance(bm, nbm, kEdgeOfNeighbour[n])) {
								// The stitched edge (lcm stride, e.g. 4|5 -> 20) is coarser than the tolerance allows:
								// refine the coarser block to the finer level, still drawing its own surface (morph 255).
								TerrainBlockMeta& coarse = (delta > 0) ? bm : nbm;
								coarse.lodLevel = static_cast<unsigned char>(std::min(bm.lodLevel, nbm.lodLevel));
								coarse.morph = 255;
								lodChanged = true;
							}
						}
					}
				}
			}
		}

		// --- Phase 5 Pass 3: emit draw commands using lodLevel -> lodStep ---
		// --- Phase 6: also compute per-block skirt depth (parallel array) ---
		for (int by = 0; by < s_terrainChunkSide; ++by)
		{
			for (int bx = 0; bx < s_terrainChunkSide; ++bx)
			{
				const TerrainBlockMeta& bm = s_blockMeta[bx + by * s_terrainChunkSide];
				if (!bm.inFrustum) continue;

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
					static const bool s_noSkirts = false;
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
					static const bool s_noStitch = false;
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
								edgeMask |= static_cast<uint8_t>((1u << e));  // skirt backstop
							if (!s_noStitch && nbm.inFrustum &&
								nbm.lodLevel != bm.lodLevel)     // visible neighbour at another stride: both sides snap to lcm
								stitch |= (static_cast<uint32_t>((mc2lod::stitchStride(LOD_STEPS[bm.lodLevel], LOD_STEPS[nbm.lodLevel],
											(e < 2) ? bm.quadCountX : bm.quadCountY) & 0xFF)))
										  << (e * 8);            // edge order N,S,W,E -> byte 0..3
						}
					}
					s_skirtEdgeMaskVec[s_cmdCount] = edgeMask;
					s_stitchStepVec[s_cmdCount]    = stitch;
					// Geomorph toward the next coarser level: per-corner weights shared by every block at the corner
					// (mc2lod::cornerMorph), so same-level edges agree and stitched/skirted edges stay unmorphed.
					{
						uint32_t morphWord = 0;
						if (bm.lodLevel + 1 < mc2lod::kLodStepCount)
						{
							float corner[4];
							const int cdx[4] = { 0, 1, 0, 1 };   // NW, NE, SW, SE
							const int cdy[4] = { 0, 0, 1, 1 };
							for (int c = 0; c < 4; ++c)
							{
								mc2lod::CornerBlock tb[4];
								for (int t = 0; t < 4; ++t)
								{
									const int tx = bx + cdx[c] - 1 + (t & 1), ty = by + cdy[c] - 1 + (t >> 1);
									const bool onMap = tx >= 0 && ty >= 0 && tx < s_terrainChunkSide && ty < s_terrainChunkSide;
									const TerrainBlockMeta* tm = onMap ? &s_blockMeta[tx + ty * s_terrainChunkSide] : nullptr;
									tb[t] = { onMap, tm && tm->inFrustum, tm ? tm->lodLevel : 0, tm ? tm->morph / 255.0f : 0.0f };
								}
								corner[c] = mc2lod::cornerMorph(tb, bm.lodLevel);
							}
							if (corner[0] > 0.0f || corner[1] > 0.0f || corner[2] > 0.0f || corner[3] > 0.0f)
								morphWord = mc2lod::packMorph(corner, LOD_STEPS[bm.lodLevel + 1]);
						}
						s_morphVec[s_cmdCount] = morphWord;
					}
					if (edgeMask)
					{
						// Skirt depth must cover the LOD CRACK (deviation of the fine
						// edge from the coarse neighbour's interpolated edge), which is
						// only a fraction of the block's full min->max elevation range.
						// Using the full range produced giant cliff walls on hilly
						// terrain (mountains: elevRange in the thousands). Scale down
						// and CAP. MC2_TERRAIN_LOD_CHUNK_SKIRT_MAX overrides the cap.
						static const float s_skirtMax = 256.0f;
						float elevRange = bm.maxElev - bm.minElev;
						float depth     = elevRange * 0.5f + 32.0f;
						depth = std::min(depth, s_skirtMax);
						depth = std::max(depth, 32.0f);
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

		// Block-set-change diagnostic: log which (bx,by) blocks are visible whenever
		// the visible set changes. Fires on change only — no noise when camera is still.
		// Track every chunk; larger maps exceed a single 32-bit mask.
		{
			static std::vector<uint8_t> s_prevBlockSet;
			const auto blockCount = static_cast<size_t>(s_terrainChunkSide) * s_terrainChunkSide;
			bool blockSetChanged = s_prevBlockSet.size() != blockCount;
			s_prevBlockSet.resize(blockCount, 2); // force first print, including an empty set
			for (int by2 = 0; by2 < s_terrainChunkSide; ++by2)
				for (int bx2 = 0; bx2 < s_terrainChunkSide; ++bx2)
				{
					const auto index = static_cast<size_t>(by2) * s_terrainChunkSide + bx2;
					const uint8_t visible = s_blockMeta[index].inFrustum ? 1 : 0;
					blockSetChanged |= s_prevBlockSet[index] != visible;
					s_prevBlockSet[index] = visible;
				}
			if (blockSetChanged) {
				// Build compact "(bx,by)" list of passing blocks.
				char buf[256]; int pos = 0;
				for (int by2 = 0; by2 < s_terrainChunkSide && pos < 200; ++by2)
					for (int bx2 = 0; bx2 < s_terrainChunkSide && pos < 200; ++bx2)
						if (s_blockMeta[bx2 + by2 * s_terrainChunkSide].inFrustum)
							pos += mc2::fmt::to_buf_n(buf + pos, sizeof(buf) - pos, "({:d},{:d})", mc2::fmt::as_i32(bx2), mc2::fmt::as_i32(by2));
				buf[pos] = '\0';
				// Camera east/north already computed above as eyeX/eyeY.
				mc2::fmt::print_out("[BlockSet] frame={:d} cmds={:d} cam=({:.0f},{:.0f}) blocks={}\n",
					mc2::fmt::as_u32(static_cast<unsigned long>(gCurrentFrame)), mc2::fmt::as_i32(s_cmdCount), eyeX, eyeY, mc2::fmt::safe(buf));
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
			mc2::fmt::print_out("[TerrainLOD v1] frame={:d} cmds={:d} LOD0={:d} LOD1={:d} LOD2={:d} LOD3={:d} LOD4={:d} LOD5={:d}\n", mc2::fmt::as_u32(static_cast<unsigned long>(gCurrentFrame)), mc2::fmt::as_i32(s_cmdCount), mc2::fmt::as_i32(lodCounts[0]), mc2::fmt::as_i32(lodCounts[1]), mc2::fmt::as_i32(lodCounts[2]), mc2::fmt::as_i32(lodCounts[3]), mc2::fmt::as_i32(lodCounts[4]), mc2::fmt::as_i32(lodCounts[5]));
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
				mc2::fmt::print_out("[TerrainLOD v1] FIRST_ZERO frame={:d} cam=({:.1f},{:.1f},{:.1f})\n", mc2::fmt::as_u32(static_cast<unsigned long>(gCurrentFrame)), camOriginLod.x, camOriginLod.y, camOriginLod.z);
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
					mc2::fmt::print_out("[TerrainLOD v1] ERROR: enabled but zero draw commands (frame={:d} streak={:d})\n", mc2::fmt::as_u32(static_cast<unsigned long>(gCurrentFrame)), mc2::fmt::as_i32(s_lodZeroCmdFrames));
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
		glm::vec3 camOrigin = eye->getCameraOrigin();
		gos_SetTerrainCameraPos(camOrigin.x, camOrigin.y, camOrigin.z);


	}

	return TRUE;
}
