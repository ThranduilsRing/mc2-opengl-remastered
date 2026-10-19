// Terrain: Terrain::geometry with its GeoScope timing and ground-pick parity probe.
// Declared in the class headers; shared names in terrain_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "terrain_internal.h"


float cosineEyeHalfFOV = 0.0f;
constexpr float MAX_CAMERA_RADIUS = 250.0f;
constexpr float CLIP_THRESHOLD_DISTANCE = 768.0f;

//a full triangle.
constexpr float VERTEX_EXTENT_RADIUS = 384.0f;

extern bool InEditor;

// [SLIMSPLIT v1] RDTSC cost-split instrumentation (env MC2_SLIM_COST_SPLIT)
// deleted by 8Z-DEADCODE-SWEEP-1: the slimReduce per-vertex loop it bracketed was
// removed in 8z-A3, leaving SlimSplitOn()/SlimSplitRollAndMaybeEmit() and the g_ss*
// counters with zero live callers. This is the "separate cleanup pass" the geometry()
// comment named. The live [8Z_VESTIGIAL]/[8Z_RETIRED_ENV] opt-out warn stubs and the
// still-wired GeoScope/MC2_GEOM_PHASE_SPLIT instrumentation below are retained.
#include <stdlib.h>
#include <stdio.h>

// ---- MC2_GEOM_PHASE_SPLIT: wall-ns split of Terrain::geometry() phases -------
// Locate the 1K-map geometry() spike (avg ~2.25ms / max ~236ms) by phase.
// Default OFF, zero behavior change. Per-phase avg/max at exit + a per-spike
// (>5ms) line naming the top contributor. Coarse per-phase scopes only (NOT
// per-quad/vertex — respects the 100ns hot-loop rule).
#include <chrono>
namespace {
	enum GeoPhase { GP_TOTAL=0, GP_SLIM, GP_CHUNKPROD, GP_QUADSETUP,
	                GP_LIGHTPACK, GP_LIGHTCOPY, GP_INDIRECT, GP_COUNT };
	static const char* s_gpNames[GP_COUNT] = {
		"total","slim","chunkProd","quadSetup","lightingPack","lightingCopy","indirectCompute" };
	static const bool s_gpOn = mc2::gate::get<"MC2_GEOM_PHASE_SPLIT">();
	static unsigned long long s_gpSum[GP_COUNT]     = {0};
	static unsigned long long s_gpMax[GP_COUNT]     = {0};
	static unsigned long long s_gpFrameNs[GP_COUNT] = {0};
	static unsigned long long s_gpFrames = 0;
	static bool s_gpAtexit = false;
	static void gpEmit() {
		if (!s_gpOn) return;
		std::printf("[GEOM_PHASE_SPLIT v1] event=shutdown frames=%llu", s_gpFrames);
		for (int i = 0; i < GP_COUNT; i++)
			std::printf(" %s={avg_us:%.1f,max_us:%.1f}", s_gpNames[i],
				s_gpFrames ? (double)s_gpSum[i]/s_gpFrames/1000.0 : 0.0,
				(double)s_gpMax[i]/1000.0);
		std::printf("\n"); std::fflush(stdout);
	}
	struct GeoScope {
		int idx; std::chrono::steady_clock::time_point t0;
		explicit GeoScope(int i) : idx(i) {
			if (s_gpOn) t0 = std::chrono::steady_clock::now();
		}
		~GeoScope() {
			if (!s_gpOn) return;
			s_gpFrameNs[idx] += (unsigned long long)
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - t0).count();
		}
	};
	// Fold per-frame -> totals/max + spike log. Call once at geometry() exit.
	static void gpFrameEnd(unsigned long long totalNs, long numberQuads, long numObjBlocks) {
		if (!s_gpOn) return;
		if (!s_gpAtexit) { s_gpAtexit = true; std::atexit(gpEmit); }
		++s_gpFrames;
		s_gpFrameNs[GP_TOTAL] = totalNs;
		int culprit = GP_SLIM; unsigned long long cmax = 0;
		for (int i = 0; i < GP_COUNT; i++) {
			s_gpSum[i] += s_gpFrameNs[i];
			if (s_gpFrameNs[i] > s_gpMax[i]) s_gpMax[i] = s_gpFrameNs[i];
			if (i != GP_TOTAL && s_gpFrameNs[i] > cmax) { cmax = s_gpFrameNs[i]; culprit = i; }
		}
		if (totalNs > 5000000ULL) {  // >5ms spike
			std::printf("[GEOM_PHASE_SPIKE v1] frame=%llu total_us=%.1f culprit=%s "
				"culprit_us=%.1f numberQuads=%ld numObjBlocks=%ld\n",
				s_gpFrames, (double)totalNs/1000.0, s_gpNames[culprit],
				(double)cmax/1000.0, numberQuads, numObjBlocks);
			std::fflush(stdout);
		}
		for (int i = 0; i < GP_COUNT; i++) s_gpFrameNs[i] = 0;
	}
}

//---------------------------------------------------------------------------
void Terrain::geometry (void)
{
	ZoneScopedN("Terrain::geometry");
	const std::chrono::steady_clock::time_point _geoT0 =
		s_gpOn ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

	// Shape A (M0a) — per-frame texture-handle memoization. Initialized at
	// the EARLIEST terrain frame boundary because converted setup-time reads
	// in TerrainQuad::setupTextures, ensureTerrainFaceCacheEntryResident, and
	// terrtxm{,2}.h accessors fire during this function (mission-update phase),
	// before GameCamera::render. See
	// docs/superpowers/specs/2026-04-27-modern-terrain-tex-resolve-table-design.md.
	{
		static uint64_t s_texResolveFrameCounter = 0;
		beginFrameTexResolve(++s_texResolveFrameCounter);
	}

	//---------------------------------------------------------------------
	//-----------------------------------

	glm::vec3 cameraPos;
	cameraPos.x = -eye->getCameraOrigin().x;
	cameraPos.y = eye->getCameraOrigin().z;
	cameraPos.z = eye->getCameraOrigin().y;

	float vClipConstant = eye->verticalSphereClipConstant;
	float hClipConstant = eye->horizontalSphereClipConstant;

	// S2.13: MC2_TERRAIN_CULL_WIDE=1 forces the angular sphere cull to
	// admit-all (constants set to 1e9). Investigation revealed editor
	// terrain coverage at ~25% of map vertices passing the angular cull
	// (885/3474 verts on mc2_01 default camera). Whether this matches
	// game-side behavior is unverified; this gate exists as an opt-in
	// workaround so visual A/B can confirm or refute the cull as root cause
	// of the "terrain clips to small center region in editor" symptom.
	// Default OFF: bit-identical legacy behavior. See handoff S2.13.
	if (mc2::gate::get<"MC2_TERRAIN_CULL_WIDE">()) {
		vClipConstant = 1.0e9f;
		hClipConstant = 1.0e9f;
	}


	// VPL retirement: the MC2_VPL_CULL / MC2_VPL_REDUCE getenv reads are
	// KEPT solely to gate the one-shot event=retired lifecycle lines
	// emitted just before the slim reduce loop below (8c-part-2: the VPL
	// body those probes compared against is deleted, so any relocated
	// self-comparison would be tautological/false-alarm; demote-not-
	// silently-delete per the worktree Debug-instrumentation rule).
	static const bool s_vplCull = mc2::gate::get<"MC2_VPL_CULL">();
	static const bool s_vplReduce = mc2::gate::get<"MC2_VPL_REDUCE">();

	// F6 T1/T2: terrain admission Modern path. Replaces per-vertex
	// projectForTerrainAdmission with a world-space frustum-plane test:
	//   extractFrustumPlanes (one-shot per frame) + quadAabbInFrustum
	//   on a degenerate AABB (point, min==max). Default Legacy preserves
	//   the red-band fallback per spec §8.
	// F6 T2: mode flag moved to mc2_terrain_admission::isModern()
	// (terrain_admission_mode.h) shared with quad.cpp setupTextures sites.
	// Cache populated below; quad.cpp reads via eye->getCachedFrustumPlanes().
	// Reference: docs/observations/2026-05-22-terrain-admission-hotpath-recon.md
	const bool s_admissionModern = mc2_terrain_admission::isModern();
	static bool s_admissionModeLogged = false;
	if (!s_admissionModeLogged) {
		s_admissionModeLogged = true;
		MC2_DIAGFE(stderr, "[TERRAIN_ADMISSION v1] event=mode_select mode=%s\n",
			s_admissionModern ? "modern" : "legacy");
		fflush(stderr);
	}

	long i=0;

	// VPL retirement Step 6: self-contained slim per-vertex loop. Originally
	// re-homed the leastZ/mostZ/leastW/mostW/leastWY/mostWY reduction feeding
	// the inverse-projection consumer; that RED reduction was deleted in
	// Phase 4 (2026-05-19) once the consumer chain was retired in Phase 3
	// (6d61801). The per-vertex projectForTerrainAdmission call
	// SURVIVES because its return drives the clipInfo / objBlock cull cascade
	// AND the rv->px/py/pz/pw raster writes consumed by the legacy
	// TerrainQuad::draw() immediate path on un-armed frames.
	// VPL retirement Step 8c-part-2: the VertexProjectLoop body (fast +
	// legacy-twin) is DELETED here â the slim reduce loop below is the
	// proven sole producer of BOTH the cull cascade and the
	// leastZ/mostZ/... reduction (8c-part-1 static + camera-swept
	// superset_violations=0 + bit-identity proof). The legacy-reference
	// sources the MC2_VPL_CULL and MC2_VPL_REDUCE probes compared against
	// died with the body, so a relocated self-comparison would be a pure
	// tautology / false-alarm; both probes are RETIRED (demote-not-
	// silently-delete per the worktree Debug-instrumentation rule + plan
	// v3.3:467 + v3.5:530) to a one-shot lifecycle line, env-gated by the
	// surviving getenv so it only prints when someone had the probe on.
	// See docs/superpowers/reviews/2026-05-15-step8-vpl-body-deletion-
	// adversarial-review.md CRIT-1/Â§6 + the v3.5 plan amendment.
	if (s_vplCull)
	{
		static bool s_vplCullRetiredLogged = false;
		if (!s_vplCullRetiredLogged) {
			s_vplCullRetiredLogged = true;
			fprintf(stderr,
				"[VPL_CULL v1] event=retired reason=vpl_body_deleted_slim_is_sole_producer\n");
			fflush(stderr);
		}
	}
	if (s_vplReduce)
	{
		static bool s_vplReduceRetiredLogged = false;
		if (!s_vplReduceRetiredLogged) {
			s_vplReduceRetiredLogged = true;
			fprintf(stderr,
				"[VPL_REDUCE v1] event=retired reason=vpl_body_deleted_slim_is_sole_reduction_producer\n");
			fflush(stderr);
		}
	}
	// Approach A (lag-free): reset the per-frame camera-windowed solid
	// recipe-index window IMMEDIATELY before the slim loop that fills it.
	// The slim loop runs BEFORE gos_terrain_indirect::ComputeDispatch()
	// (called later in geometry()), so the window collected here is consumed
	// by THIS frame's dispatch — same-frame, no 1-frame lag.  The append
	// (inside the `if (rv->clipInfo)` block below) rides the existing slim
	// iteration; NO new per-frame walk is introduced.
	gos_terrain_indirect::BeginFrameSolidWindow();
	const bool s_solidNarrowOn = gos_terrain_indirect::SolidWindowEnabled();

	// F6 T1/T2: cache frustum planes once per frame (O(1)) into Camera member.
	// Both slimReduce below AND quad.cpp::setupTextures water-corner sites read
	// via eye->getCachedFrustumPlanes(). Single extraction shared across all sites.
	// MC2_BLOCK_FRUSTUM_FALLBACK also needs planes; cache unconditionally when
	// either feature is active so both share the single extraction.
	static const bool s_blockFrustumFallback = mc2::gate::get<"MC2_BLOCK_FRUSTUM_FALLBACK">();
	if (s_admissionModern || s_blockFrustumFallback) {
		eye->cacheFrustumPlanes();
	}

	// 8z-A3: slimReduce per-vertex loop deleted. Under chunk=ON, makeLists is
	// skipped (numberVertices==0) so this loop was a no-op. Production
	// objBlockInfo.active/objVertexActive/solid-window written by s_lodChunkProd
	// (Phase 8c). The dead SlimSplit RDTSC machinery was removed by
	// 8Z-DEADCODE-SWEEP-1; the live GeoScope/MC2_GEOM_PHASE_SPLIT helpers remain.

	// MC2_BLOCK_FRUSTUM_FALLBACK: post-slimReduce block-level frustum AABB pass.
	// Widens object-block admission for cameras where the per-vertex angular cull
	// incorrectly rejects in-frustum blocks (wolfman / low-angle view). Additive
	// only — never deactivates a block the legacy vertex pass already activated.
	// Sets both objBlockInfo[b].active AND objVertexActive[vertNum] so the
	// per-object gate in objmgr (objVertexActive[obj->getVertexNum()]) also passes.
	// CRIT-1 superset invariant preserved: new active set = old ∪ aabb_activated.
	if (s_blockFrustumFallback) {
		const float (*planes)[4] = eye->getCachedFrustumPlanes();
		const long ssNumActiveVerts = realVerticesMapSide * realVerticesMapSide;
		static constexpr float kBlockZMin = -200.0f;
		static constexpr float kBlockZMax = 2500.0f;
		for (long b = 0; b < numObjBlocks; ++b) {
			if (objBlockInfo[b].active) continue;
			const long bx = b % blocksMapSide;
			const long by = b / blocksMapSide;
			glm::vec3 mn(
				float(bx * verticesBlockSide - halfVerticesMapSide) * worldUnitsPerVertex,
				float(halfVerticesMapSide - (by + 1) * verticesBlockSide) * worldUnitsPerVertex,
				kBlockZMin);
			glm::vec3 mx(
				mn.x + worldUnitsBlockSide,
				mn.y + worldUnitsBlockSide,
				kBlockZMax);
			if (!eye->quadAabbInFrustum(planes, mn, mx)) continue;
			objBlockInfo[b].active = true;
			// Propagate to per-vertex active flags: objmgr checks
			// objVertexActive[obj->getVertexNum()] as a second per-object gate.
			// Assumes row-major vertex storage: vertexNum = row*realVMS + col.
			const long rowStart = by * verticesBlockSide;
			const long rowEnd   = rowStart + verticesBlockSide;
			const long colStart = bx * verticesBlockSide;
			const long colEnd   = colStart + verticesBlockSide;
			for (long row = rowStart; row < rowEnd && row < realVerticesMapSide; ++row) {
				for (long col = colStart; col < colEnd && col < realVerticesMapSide; ++col) {
					const long vn = row * realVerticesMapSide + col;
					if (vn < ssNumActiveVerts)
						objVertexActive[vn] = true;
				}
			}
		}
	}

	// === Phase 8c: PRODUCTION handoff — chunk producer writes the REAL
	// objBlockInfo.active / objVertexActive / solid window under the flag ===
	// Active when MC2_TERRAIN_LOD_CHUNK=1. Under the flag, makeLists is skipped
	// so slimReduce is a no-op and these production outputs are otherwise EMPTY
	// (terrain-object AI gated off at objmgr.cpp:2193, solid window falls back
	// to full-range). This re-homes them onto the O(blocks) chunk producer (the
	// SAME angular-cone + 1-ring dilation proven FN=0 by 8a/8b on tier1 and the
	// 1K map). Production arrays are pre-cleared every frame by
	// clearObjBlocksActive / clearObjVerticesActive (mission.cpp:565-566)
	// BEFORE geometry(), so we only set true. All writes happen INSIDE the
	// O(active-blocks x span) loop — never an O(nV) scan — so the retired O(n^2)
	// slimReduce stays retired (guardrail: do NOT revive makeLists here).
	// 8z-A1: s_activeABForce removed; chunk producer always runs under the flag.
	static const bool s_lodChunkProd = (mc2TerrainLodChunkEnabled());
	if (s_lodChunkProd && eye && objBlockInfo && objVertexActive && numObjBlocks > 0)
	{
		ZoneScopedN("Terrain::geometry chunkActiveProd");
		GeoScope _gsChunk(GP_CHUNKPROD);
		const long nB = numObjBlocks;
		const long nV = realVerticesMapSide * realVerticesMapSide;
		static std::vector<uint8_t> s_prodBlock;
		s_prodBlock.assign(nB, 0);

		const float hMapW      = float(halfVerticesMapSide) * worldUnitsPerVertex;
		// [LOW-CAMERA-TERRAIN-CULL-1 v2] The angular solid-window producer (this
		// loop) is the SECOND terrain visibility gate — independent of the frustum
		// inFrustum set we relaxed via MC2_LOWCAM_TERRAIN_NEAR. At grazing pitch,
		// near terrain spread SIDEWAYS of the look vector falls just past the
		// unconditional near-field bypass and outside the horizontal cone -> dropped.
		// Widen the near-field bypass radius (purely additive: only admits more
		// blocks; dilation/superset invariants preserved). Default 4x; =1 = stock.
		static const float s_lowCamSolidNear = mc2::gate::get<"MC2_LOWCAM_SOLID_NEAR">();
		const float kNearField = 768.0f * s_lowCamSolidNear;   // CLIP_THRESHOLD_DISTANCE
		const float kExtent    = 384.0f;   // VERTEX_EXTENT_RADIUS
		const float blockR     = (verticesBlockSide * 0.5f) * worldUnitsPerVertex * 1.5f;

		// Base-active: angular-cone replication of legacy onScreenR (identical to 8a).
		for (long b = 0; b < nB; ++b)
		{
			const long bx = b % blocksMapSide, by = b / blocksMapSide;
			glm::vec3 bc(
				(float(bx * verticesBlockSide) + verticesBlockSide * 0.5f) * worldUnitsPerVertex - hMapW,
				hMapW - (float(by * verticesBlockSide) + verticesBlockSide * 0.5f) * worldUnitsPerVertex,
				cameraPos.z);
			glm::vec3 oc; oc = bc - cameraPos;
			Camera::cameraFrame.trans_to_frame(oc);
			const float distEye = glm::length(oc);
			glm::vec3 cv = oc; cv.z = 0.0f;
			const float distClip = glm::length(cv);
			bool act = (distClip <= (kNearField + blockR));
			if (!act)
			{
				if (fabs(oc.y) > 1.0e-3f && distEye > 1.0e-3f)
				{
					const float clip_distance = fabsf(1.0f / oc.y);
					const float extent_angle  = kExtent / distEye;
					const float angR          = blockR * clip_distance;
					const float hAng          = fabsf(oc.x) * clip_distance;
					if (hAng <= (hClipConstant + extent_angle + angR)) act = true;
				}
				else act = true;
			}
			if (act) s_prodBlock[b] = 1;
		}

		// 1-block neighbor dilation (closes frustum-edge sliver FN; FP-safe).
		{
			const long bms = blocksMapSide;
			std::vector<uint8_t> dil(nB, 0);
			for (long b = 0; b < nB; ++b)
			{
				if (!s_prodBlock[b]) continue;
				const long bx = b % bms, by = b / bms;
				for (long dy = -1; dy <= 1; ++dy)
				for (long dx = -1; dx <= 1; ++dx)
				{
					const long nx = bx + dx, ny = by + dy;
					if (nx < 0 || ny < 0 || nx >= bms || ny >= bms) continue;
					dil[nx + ny * bms] = 1;
				}
			}
			s_prodBlock.swap(dil);
		}

		// SINK: write PRODUCTION inside the O(active-blocks x span) loop. The live
		// solid window was cleared this frame by BeginFrameSolidWindow() (in the
		// slimReduce block above, which still runs but iterates zero vertices).
		const bool solidOn = gos_terrain_indirect::SolidWindowEnabled();
		long pBlocks = 0, pVerts = 0, pWin = 0;
		for (long b = 0; b < nB; ++b)
		{
			if (!s_prodBlock[b]) continue;
			objBlockInfo[b].active = true;
			++pBlocks;
			const long bx = b % blocksMapSide, by = b / blocksMapSide;
			const long rowStart = by * verticesBlockSide, colStart = bx * verticesBlockSide;
			for (long row = rowStart; row < rowStart + verticesBlockSide && row < realVerticesMapSide; ++row)
			for (long col = colStart; col < colStart + verticesBlockSide && col < realVerticesMapSide; ++col)
			{
				const long vn = row * realVerticesMapSide + col;
				if (vn < 0 || vn >= nV) continue;
				objVertexActive[vn] = true;
				++pVerts;
				if (solidOn && gos_terrain_indirect::RecipeForVertexNum((int32_t)vn))
				{
					gos_terrain_indirect::AppendSolidWindowCandidate((int32_t)vn);
					++pWin;
				}
			}
		}

		// Telemetry — guards the silent "obj-active empty under flag" failure mode.
		static unsigned long s_prodFrame = 0;
		++s_prodFrame;
		if (s_prodFrame <= 3 || (s_prodFrame % 600) == 0)
		{
			printf("[TerrainLOD prod] frame=%lu objBlocks=%ld objVerts=%ld solidWindow=%ld slimVerts=0\n",
			       s_prodFrame, pBlocks, pVerts, pWin);
			if (pBlocks == 0)
				printf("[TerrainLOD prod] WARNING: zero active obj blocks — terrain-object AI would be gated off\n");
			fflush(stdout);
		}
	}

	// 8z-A1: Phase 8a/8b A/B diagnostic blocks (MC2_TERRAIN_ACTIVE_AB,
	// MC2_TERRAIN_SOLID_AB) deleted — 8a/8b/8c shipped, FN=0 validated.

	//-----------------------------------
	// setup terrain quad textures
	// Also sets up mine data.
	TerrainQuadPtr currentQuad = quadList;

	{
		ZoneScopedN("Terrain::geometry quadSetupTextures");
		// Stage 3: preflight arming — walks live quadList BEFORE the loop so
		// IsFrameSolidArmed() is stable for all setupTextures() calls.
		// On un-armed frames (recipe not ready, disabled, etc.) this returns
		// false with zero side-effects; setupTextures runs as normal.
		gos_terrain_indirect::ComputePreflight();
		// Phase 1: terrain lighting GPU compute — per-frame trio (design doc Q5).
		// BeginFrame advances ring slot; PackAndDispatch packs + dispatches;
		// CopyResultsToVertexPool (Stage 3): T1/T2/T3 non-blocking tryConsume
		// writes GPU lightRGB/fogRGB into vertices[i] BEFORE the setupTextures loop.
		gos_terrain_lighting::BeginFrame();
		{ GeoScope _gsLP(GP_LIGHTPACK); gos_terrain_lighting::PackAndDispatch(); }
		{ GeoScope _gsLC(GP_LIGHTCOPY); gos_terrain_lighting::CopyResultsToVertexPool(quadList, numberQuads); }
		// Phase C: SOLID compute dispatch. MUST be AFTER PackAndDispatch above
		// so Phase 1's post-dispatch barrier has published the lighting SSBO.
		{ GeoScope _gsIC(GP_INDIRECT); gos_terrain_indirect::CaptureFrameInputs(); }
		// S6 coarse cost A/B instrument: ONE QPC pair around the WHOLE
		// per-frame setupTextures loop (NOT per-quad - the per-quad
		// std::chrono COST_SPLIT scopes are observer-effect-poisoned and
		// disqualified; capped FPS is also useless). Env-gated, prints a
		// min/mean/max summary every 600 frames (MC2_TGL_POOL_TRACE idiom).
		// Used to A/B armed ((ii) skipped) vs MC2_GPU_DRIVEN_WATER=0
		// ((ii) runs) - the only setupTextures delta between those is (ii),
		// so this isolates (ii)'s real per-frame CPU contribution.
		static const bool s_s6CostOn = mc2::gate::get<"MC2_WATER_S6_COST">();
		static uint64_t s_s6QpcFreq = 0;
		uint64_t s_s6QpcStart = 0;
		if (s_s6CostOn)
		{
			if (s_s6QpcFreq == 0)
				QueryPerformanceFrequency((LARGE_INTEGER*)&s_s6QpcFreq);
			QueryPerformanceCounter((LARGE_INTEGER*)&s_s6QpcStart);
		}
		// QUADSETUP-ARMED-SKIP-WALK-1: when the GPU terrain path fully owns the
		// frame, the per-quad setupTextures() call is dead work — its consumers
		// (draw()/drawWater(), the recipe + mine-enqueue blocks, clipInfo) are
		// all either skipped or self-gated to no-op when armed, and decals/mines
		// come from the static bakes (DrawDecalStatic/DrawMineStatic, default-ON
		// since 2026-05-17). The native water producer reads mission recipes and
		// per-frame native scalars; it does not need setupTextures.
		// Picking is independent (Camera::inverseProject
		// forward-projects fresh from vx/vy/elevation — Task 0 verified: zero
		// wx/wy/wz/clipInfo dependency). Native water recipes are keyed by map
		// coordinates and do not depend on the camera-relative quadList window.
		// DEFAULT-ON: only literal "0" opts out, UNSET opts IN. (This sentence read
		// "Gate default-OFF; ... unset/\"0\" = OFF kill-switch" until ENV-DROP-CLOSE-1,
		// 2026-07-30 -- contradicting both the DEFAULT-ON line three rows below it
		// and the reader below that.) NativeInputSourceReady() is
		// in the predicate so we only skip when the native recipe producer is ready.
		// DEFAULT-ON since 2026-06-03 (clean Tracy mc2_01 1.01ms->372us, tier1 5/5,
		// water/mines/decals/picking user-verified): only literal "0" opts out
		// (bisection / revert escape hatch), any other value INCLUDING UNSET opts in.
		static const bool s_armedSkipOn = mc2::gate::get<"MC2_QUADSETUP_ARMED_SKIP">();
		const bool fullyArmed =
			gos_terrain_indirect::IsFrameSolidArmed() &&
			gos_terrain_indirect::IsFrameOverlayArmed() &&
			gos_terrain_indirect::IsFrameMineArmed() &&
			gos_terrain_indirect::WaterFastPathOwnsArmedDraw() &&
			(Terrain::terrainTextures2 != nullptr) &&
			!drawTerrainGrid &&
			WaterStream::NativeInputSourceReady();
		const bool skipSetup = s_armedSkipOn && fullyArmed;

		// [FASTPATH_DROP] transition log. Default OFF (MC2_FASTPATH_DROP_LOG=1 to enable).
		// OBSERVATIONAL ONLY — reads existing state, no rendering change.
		// Emits exactly one line per armed<->fallback transition; silent every other frame.
		{
			static const bool s_fastpathDropLog = mc2::gate::get<"MC2_FASTPATH_DROP_LOG">();
			if (s_fastpathDropLog)
			{
				static unsigned long s_dropFrame = 0;
				++s_dropFrame;
				// prevArmed: true = was armed last frame, false = was in fallback.
				// Initialise to armed so that the very first fallback (warmup) is
				// reported; a first-frame armed start is NOT reported (no transition).
				static bool s_prevArmed = true;
				const bool nowArmed = fullyArmed; // s_armedSkipOn is the outer gate; log the inner predicate only

				if (nowArmed != s_prevArmed)
				{
					// Determine which conjunct failed first (short-circuit order = T-table order).
					const char* reason = "UNKNOWN";
					if (!nowArmed)
					{
						// Falling from armed -> fallback: name the first failing gate.
						using namespace gos_terrain_indirect;
						if (!IsFrameSolidArmed() || !IsFrameOverlayArmed())
							reason = "T7_SOLID_OR_OVERLAY_NOT_ARMED";
						else if (!IsFrameMineArmed())
							reason = "T17_MINE_NOT_ARMED";
						else if (!WaterFastPathOwnsArmedDraw())
							reason = "T9_OR_T10_WATER_NOT_ARMED";
						else if (Terrain::terrainTextures2 == nullptr)
							reason = "T11_NO_TEXTURES2";
						else if (drawTerrainGrid)
							reason = "T13_DRAW_TERRAIN_GRID";
						else if (!WaterStream::NativeInputSourceReady())
							reason = "T14_NATIVE_WATER_INPUT_UNAVAILABLE";
						else
							reason = "T15_OR_OTHER_SKIP_OFF";
					}
					else
					{
						reason = "RECOVERY";
					}
					printf("[FASTPATH_DROP] frame=%lu transition=%s reason=%s chunk=%d water=%d solid=%d overlay=%d editor=%d\n",
						s_dropFrame,
						nowArmed ? "FALLBACK_TO_ARMED" : "ARMED_TO_FALLBACK",
						reason,
						(int)mc2TerrainLodChunkEnabled(),
						(int)gos_terrain_indirect::WaterFastPathOwnsArmedDraw(),
						(int)gos_terrain_indirect::IsFrameSolidArmed(),
						(int)gos_terrain_indirect::IsFrameOverlayArmed(),
						(int)drawTerrainGrid);
					fflush(stdout);
					s_prevArmed = nowArmed;
				}
			}
		}

		GeoScope _gsQuadSetup(GP_QUADSETUP);  // setup loop (GPU dispatches timed separately above)
		long quadsSkipped = 0;
		constexpr long waterCandidates = 0; // native GPU cull owns selection
		// The setupTextures body is a no-op in the default-ON steady state:
		// native water culling owns selection and Slice A skips setupTextures().
		// The loop would iterate ~40K quads doing nothing. Skip it
		// whole — retires the residual ~32µs bare-iteration cost. currentQuad is a
		// pure cursor with no post-loop reader, so it need not advance here.
		if (skipSetup)
		{
			quadsSkipped = numberQuads;
		}
		else
		for (i=0;i<numberQuads;i++)
		{
			if (skipSetup)
				++quadsSkipped;
			else
#ifdef MC2_IS_EDITOR
				currentQuad->setupTextures();
#else
				// 8z-B: setupTextures is compiled out of the game build. The GPU path
				// is armed from frame 1 (so !skipSetup is effectively never reached in
				// production); if it ever is, there is intentionally no legacy fallback
				// (the loud-fail T16/T19 path surfaces a real GL/init failure).
				++quadsSkipped;
#endif
			currentQuad++;
		}
		// QUADSETUP-ARMED-SKIP-WALK-1 bounded telemetry (every 600 frames, only
		// when the gate is enabled so production-with-feature-off stays silent).
		if (s_armedSkipOn) {
			static uint32_t s_qsSkipFrames = 0;
			if ((++s_qsSkipFrames % 600) == 0) {
				printf("[QUADSETUP_SKIP v1] fullyArmed=%d skip=%d quadsSkipped=%ld waterCandidates=%ld pickingCarveout=none\n",
				       fullyArmed ? 1 : 0, skipSetup ? 1 : 0,
				       quadsSkipped, waterCandidates);
				fflush(stdout);
			}
		}
		if (s_s6CostOn)
		{
			uint64_t s6End = 0;
			QueryPerformanceCounter((LARGE_INTEGER*)&s6End);
			double s6Ms = (double)(s6End - s_s6QpcStart) * 1000.0 / (double)s_s6QpcFreq;
			static uint32_t s_s6Frames = 0;
			static double   s_s6Sum = 0.0;
			static double   s_s6Min = 1e30;
			static double   s_s6Max = 0.0;
			s_s6Frames++;
			s_s6Sum += s6Ms;
			if (s6Ms < s_s6Min) s_s6Min = s6Ms;
			if (s6Ms > s_s6Max) s_s6Max = s6Ms;
			if ((s_s6Frames % 600) == 0)
			{
				printf("[WATER_S6COST v1] event=summary frames=%u quadSetupTextures_ms mean=%.4f min=%.4f max=%.4f (window of 600)\n",
				       s_s6Frames, s_s6Sum / 600.0, s_s6Min, s_s6Max);
				fflush(stdout);
				s_s6Sum = 0.0; s_s6Min = 1e30; s_s6Max = 0.0;
			}
		}
		// Stage 1 cost-split: roll per-frame nanosecond accumulators (no-op
		// when MC2_TERRAIN_COST_SPLIT unset). ParityFrameTick advances the
		// summary cadence; Stage 2 passes the actual quads-checked count.
		// Stage 2: terrain lighting parity check — AFTER the setupTextures loop
		// so CPU has written all lightRGB/fogRGB for this frame.
		// GetMappedOutputForParity() synchronously waits on current-frame fence
		// (parity mode only — production path skips this entirely).
		if (gos_terrain_lighting::IsParityCheckEnabled()) {
			const gos_terrain_lighting::GpuTerrainLightingOutput* mappedOut =
				gos_terrain_lighting::GetMappedOutputForParity();
			gos_terrain_lighting::Parity_CompareFrame(quadList, numberQuads, mappedOut);
		}
		gos_terrain_indirect::CostSplit_RollFrame();
		{
			int quadsChecked = 0;
			if (gos_terrain_indirect::IsParityCheckEnabled())
				quadsChecked = gos_terrain_indirect::ParityCompareRecipeFrame();
			gos_terrain_indirect::ParityFrameTick(quadsChecked);
		}
	}

	if (s_gpOn) {
		const unsigned long long _geoTotal = (unsigned long long)
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - _geoT0).count();
		gpFrameEnd(_geoTotal, numberQuads, numObjBlocks);
	}
}
