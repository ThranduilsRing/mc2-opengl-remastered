// Terrain: Terrain::render, draw-command flush and the water render paths.
// Declared in the class headers; shared names in terrain_internal.h.
#include "terrain_internal.h"
#include "mc2fmt.h"


//---------------------------------------------------------------------------
void Terrain::render (void)
{
	//-----------------------------------
	// render the cloud layer
	//-----------------------------------
	// Draw resulting terrain quads. The drawPass zone wraps the WHOLE loop
	// (one zone per frame, not per-quad). Per-quad zones were stripped on
	// 2026-05-07 because zone overhead dominated; a single zone wrapping the
	// ~14-40K iteration loop attributes the 2.02 ms drawPass cost without
	// re-introducing the per-call overhead. PatchStream sub-zones (Flush,
	// MemoryBarrier, BucketSort, etc.) live inside this and break out the
	// ~290 us PatchStream slice; the residual ~1.7 ms is non-PatchStream
	// per-quad CPU work that stays attributed to drawPass at coarse level.
	DWORD fogColor = eye->currentLighting().fogColor;

	if (drawTerrainTiles)
	{
		ZoneScopedN("Terrain::render drawPass");
		// Terrain LOD chunk Phase 4 kill switch: when MC2_TERRAIN_LOD_CHUNK=1 the
		// chunk path owns terrain rendering; suppress the per-quad draw() loop
		// entirely to avoid double-draw. Mine pass is NOT suppressed (Phase 7).
		// drawPass-retirement Slice B (mirrors the proven minePass gate at
		// the sibling loop below). The per-quad draw() loop is retired only
		// when BOTH producers it bundles are GPU-covered:
		//   - SOLID base terrain  -> GPU indirect path (IsFrameSolidArmed)
		//   - cement/road decals  -> Slice-A static bake (IsFrameOverlayArmed)
		// DRAWALPHA detail is unconditionally dead (pixel-suppressed since
		// 521d83a; A2-confirmed via legacy_drawalpha_detail_quads counter).
		// MC2_TERRAIN_INDIRECT_OVERLAY is DEFAULT-ON since the 60f2ef8
		// Stage-6 flip (IsOverlayEnabled(): only literal "0" opts out).
		// So on the stock/default path BOTH IsFrameSolidArmed() and
		// IsFrameOverlayArmed() are true -> the conjunction is true ->
		// the per-quad draw() loop below is SKIPPED (the else branch is
		// the live default branch; the drawPass zone is ~empty). The
		// conjunction is still load-bearing, but for the opposite reason
		// the old comment claimed: it ensures the MC2_TERRAIN_INDIRECT_
		// OVERLAY=0 revert (overlay disabled, the code-proof fallback)
		// STILL runs draw() so decals fall back to the M2d per-quad emit
		// and do not vanish = the 9964d5a-regression guard. Gating on
		// IsFrameSolidArmed() alone would kill decals on the =0 revert.
		if (!mc2TerrainLodChunkEnabled())
		{
			// Phase 7.5 diagnostic: confirm the old draw path is active (flag not set).
			// Fires once so we know which path owns terrain on this launch.
			static bool s_oldPathLoggedOnce = false;
			if (!s_oldPathLoggedOnce) {
				mc2::fmt::print_out("[TerrainLOD v1] OLD draw path active (flag not set)\n");
				fflush(stdout);
				s_oldPathLoggedOnce = true;
			}

			if (!(gos_terrain_indirect::IsFrameSolidArmed()
			      && gos_terrain_indirect::IsFrameOverlayArmed()))
			{
				TerrainQuadPtr currentQuad = quadList;
				for (long i = 0; i < numberQuads; i++)
				{
					// M2b loop-level pure-water hoist: skip the function call entirely for
					// quads with no base terrain, no overlay, and no detail handle. ~28K
					// quads/frame on water-heavy maps. Mirror of the in-draw() early-exit;
					// the in-function check is the fallback if useOverlayTexture /
					// useWaterInterestTexture globals get toggled at runtime.
					if (currentQuad->terrainHandle == 0
					    && currentQuad->overlayHandle == 0xffffffff
					    && currentQuad->terrainDetailHandle == 0xffffffff)
					{
						currentQuad++;
						continue;
					}
					currentQuad->draw();
					currentQuad++;
				}
			}
			else
			{
				// [SUBSYS] lifecycle line (env-gated, one-shot) per the
				// debug-instrumentation rule. Mirrors the mine retirement
				// trace. drawPass zone is now ~empty on armed frames -> the
				// Tracy total-frame delta is the substitutive proof.
				static bool s_drawPassRetiredLogged = false;
				if (!s_drawPassRetiredLogged
				    && gos_terrain_indirect::IsTraceEnabled())
				{
					s_drawPassRetiredLogged = true;
					mc2::fmt::print_out("[TERRAIN_DRAWPASS v1] event=retired reason=solid+overlay_armed (SOLID->gpu_indirect, decals->static_bake, detail->dead)\n");
					fflush(stdout);
				}
			}
		}
	}

	// MC2_TERRAIN_MINE_AB — A/B diagnostic. Pure observation: proves the legacy
	// per-quad mine enqueue (quad.cpp enqueueTerrainMineState, walked by the
	// setupTextures path) and the LOD-chunk grid enqueue (enqueueMinesFromGrid)
	// select the SAME set of mine-bearing cells over the visible quadList. Both
	// SET_LEGACY and SET_GRID mirror the exact cell loop + getMine call from
	// quad.cpp:422-476 (replicated inline; we do not touch quad.cpp state). Does
	// NOT enqueue or draw anything. Behavior is identical when the var is unset.
	{
		static bool s_mineAB = false;
		if (s_mineAB && GameMap && quadList && numberQuads > 0)
		{
			static unsigned long s_mineABFrame = 0;
			++s_mineABFrame;

			std::vector<long> setLegacy;
			std::vector<long> setGrid;

			// SET_LEGACY — walk quadList as the legacy per-quad path does.
			{
				TerrainQuadPtr q = quadList;
				for (long i = 0; i < numberQuads; i++, q++)
				{
					long rowCol = q->vertices[0]->posTile;
					long tileR = rowCol >> 16;
					long tileC = rowCol & 0x0000ffff;
					if (!GameMap->tileHasMines(tileR, tileC))
						continue;
					for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
					{
						for (long cellC = 0; cellC < MAPCELL_DIM; cellC++)
						{
							long actualCellRow = tileR * MAPCELL_DIM + cellR;
							long actualCellCol = tileC * MAPCELL_DIM + cellC;
							if (GameMap->inBounds(actualCellRow, actualCellCol)
							    && GameMap->getMine(actualCellRow, actualCellCol) != 0)
								setLegacy.push_back(static_cast<long>(actualCellRow) * 100000 + actualCellCol);
						}
					}
				}
			}

			// SET_GRID — mirror enqueueMinesFromGrid: same quadList, same cell
			// loop + getMine selection (enqueueMinesFromGrid -> enqueueTerrainMineState).
			{
				TerrainQuadPtr q = quadList;
				for (long i = 0; i < numberQuads; i++, q++)
				{
					long rowCol = q->vertices[0]->posTile;
					long tileR = rowCol >> 16;
					long tileC = rowCol & 0x0000ffff;
					if (!GameMap->tileHasMines(tileR, tileC))
						continue;
					for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
					{
						for (long cellC = 0; cellC < MAPCELL_DIM; cellC++)
						{
							long actualCellRow = tileR * MAPCELL_DIM + cellR;
							long actualCellCol = tileC * MAPCELL_DIM + cellC;
							if (GameMap->inBounds(actualCellRow, actualCellCol)
							    && GameMap->getMine(actualCellRow, actualCellCol) != 0)
								setGrid.push_back(static_cast<long>(actualCellRow) * 100000 + actualCellCol);
						}
					}
				}
			}

			std::sort(setLegacy.begin(), setLegacy.end());
			setLegacy.erase(std::unique(setLegacy.begin(), setLegacy.end()), setLegacy.end());
			std::sort(setGrid.begin(), setGrid.end());
			setGrid.erase(std::unique(setGrid.begin(), setGrid.end()), setGrid.end());

			std::vector<long> onlyLegacy;
			std::vector<long> onlyGrid;
			std::set_difference(setLegacy.begin(), setLegacy.end(),
			                    setGrid.begin(), setGrid.end(),
			                    std::back_inserter(onlyLegacy));
			std::set_difference(setGrid.begin(), setGrid.end(),
			                    setLegacy.begin(), setLegacy.end(),
			                    std::back_inserter(onlyGrid));

			if (setLegacy.size() != setGrid.size() || (s_mineABFrame % 300 == 0))
			{
				mc2::fmt::print_out("[MINE_AB] frame={:d} legacy={:d} grid={:d} onlyLegacy={:d} onlyGrid={:d}\n", mc2::fmt::as_u32(s_mineABFrame), mc2::fmt::as_i32(static_cast<int>(setLegacy.size())), mc2::fmt::as_i32(static_cast<int>(setGrid.size())), mc2::fmt::as_i32(static_cast<int>(onlyLegacy.size())), mc2::fmt::as_i32(static_cast<int>(onlyGrid.size())));
				fflush(stdout);
			}
		}
	}

	// Terrain LOD chunk Phase 7A — mine enqueue from visible tile grid.
	// When MC2_TERRAIN_LOD_CHUNK=1, the per-quad mine enqueue that was embedded
	// in setupTextures() is suppressed (see quad.cpp setupTextures s_lodChunkActive
	// guard). This block replaces it: enqueue runs over the full quadList once per
	// frame, identical population to the minePass drawMine loop below.
	// Must precede the minePass drawMine loop (draw reads mineResult set here).
	// Not gated on drawTerrainTiles — mine enqueue is a booking step that must
	// fire regardless of whether the LOD mesh draw is active this frame.
	if (mc2TerrainLodChunkEnabled()
	    && !gos_terrain_indirect::IsFrameMineArmed()
	    && quadList
	    && numberQuads > 0)
	{
		ZoneScopedN("Terrain::render lodChunkMineEnqueue");
		TerrainQuad::enqueueMinesFromGrid(quadList, numberQuads);
	}

	if (drawTerrainTiles)
	{
		ZoneScopedN("Terrain::render minePass");
		// PR2c Stage 2c — when armed, the indirect path owns mine drawing for
		// this frame (Render.TerrainMines zone in txmmgr.cpp). Skip the entire
		// per-quad drawMine loop here. This is the bulk of the ~1.83ms minePass
		// retirement (the loop fires drawMine on ALL ~14K visible quads, not
		// just the few mine-bearing ones, due to drawMine's early-return
		// pattern at quad.cpp:4242-4243).
		if (!gos_terrain_indirect::IsFrameMineArmed()) {
			TerrainQuadPtr currentQuad = quadList;
			for (long i = 0; i < numberQuads; i++)
			{
				currentQuad->drawMine();
				currentQuad++;
			}
		}
	}

	if (drawTerrainGrid || DrawDebugCells || drawLOSGrid)
	{
		ZoneScopedN("Terrain::render debugOverlays");
		TerrainQuadPtr currentQuad = quadList;
		for (long i = 0; i < numberQuads; i++)
		{
			if (drawTerrainGrid || drawEditorPassability)
			{
				if (useFog) gos_SetRenderState(gos_State_Fog, 0);
				currentQuad->drawLine();
				if (useFog) gos_SetRenderState(gos_State_Fog, fogColor);
			}
			else if (DrawDebugCells)
			{
				if (useFog) gos_SetRenderState(gos_State_Fog, 0);
				currentQuad->drawDebugCellLine();
				if (useFog) gos_SetRenderState(gos_State_Fog, fogColor);
			}
			else if (drawLOSGrid)
			{
				if (useFog) gos_SetRenderState(gos_State_Fog, 0);
				currentQuad->drawLOSLine();
				if (useFog) gos_SetRenderState(gos_State_Fog, fogColor);
			}
			currentQuad++;
		}
	}
}

//---------------------------------------------------------------------------
// Terrain LOD chunk Phase 4 — flush pending draw commands to the GPU.
// Called from code/gamecam.cpp after the shadow pass and before
// mc2::VkSceneRenderer::renderLists(mc2::renderSubmissions()). No-op when MC2_TERRAIN_LOD_CHUNK is unset
// (s_blockMeta is nullptr, s_cmdCount is 0). No GL calls in mclib/.
void Terrain::flushDrawCommands (void)
{
	// Phase 7.5 flush-cardinality probe: log every 60 frames so we know
	// flush runs once-per-frame unconditionally.
	static int s_flushCallCount = 0;
	++s_flushCallCount;
	if (s_flushCallCount % 600 == 0) {
		mc2::fmt::print_out("[TerrainLOD flush] count={:d} cmds={:d}\n", mc2::fmt::as_i32(s_flushCallCount), mc2::fmt::as_i32(s_cmdCount));
		fflush(stdout);
	}

	if (s_blockMeta && s_cmdCount > 0) {
		// [FRAME_PASS_STATS v1] visible terrain chunks = draw-command count
		// (one cmd per visible block); pulled from the existing aggregate, no
		// per-block counting. OFF=zero cost (Set* early-returns).
		gos_frame_pass_stats::SetPassCounts(
			gos_render_pass_timer::Pass_TerrainChunk, static_cast<uint32_t>(s_cmdCount), 0u);
		gos_frame_pass_stats::SetVisibleTerrainChunks(static_cast<uint32_t>(s_cmdCount));
		gos_TerrainLodChunk_SubmitDrawCommands(s_drawCmds, s_skirtDepths.get(),
			s_skirtEdgeMaskVec.empty() ? nullptr : s_skirtEdgeMaskVec.data(),
			s_stitchStepVec.empty() ? nullptr : s_stitchStepVec.data(),
			s_morphVec.empty() ? nullptr : s_morphVec.data(),
			s_cmdCount);
	}
	gos_TerrainLodChunk_EnsureSceneClear();   // a frame without a terrain pass still starts from a cleared shared scene
}
//---------------------------------------------------------------------------
// Single-source predicate: all conditions that allow renderWater() to skip the
// legacy loop and quad.cpp::setupTextures() to skip the armed-frame (ii) writes.
// Definition lives here (terrain.cpp) because this is the only TU that sees
// WaterStream + gpu_driven. Declared in gos_terrain_indirect.h (already
// included by both terrain.cpp and quad.cpp; zero new includes in either).
bool gos_terrain_indirect::WaterFastPathOwnsArmedDraw()
{
	static const bool s_fastPath =
	    (mc2::gate::get<"MC2_RENDER_WATER_FASTPATH">()) ||
	    gpu_driven::IsWaterEnabled();
	// PERF-MISSION-INTRO-ARMED-RENDER-1: water fast path now arms independently
	// of terrain solid. Water resources (WaterStream recipes, terrainTextures2)
	// are built during primeMissionTerrainCache() — fully ready on frame 1.
	// Removing the IsFrameSolidArmed() dependency lets water render on intro pans
	// where solid may not yet be armed (e.g. first few frames before LUT resolves).
	// MVP fallback: gameos_graphics.cpp uses gos_GetTerrainMVPMat4() when
	// !IsFrameSolidArmed(), already handled (fix #3, water_fastpath_interim_fixes).
	const bool g1 = s_fastPath;
	// g2: the solid-arm dependency is gone.
	const bool g2 = true;
	const bool g3 = WaterStream::IsReady();
	const bool g4 = (WaterStream::GetRecipeCount() > 0);
	const bool g5 = (Terrain::terrainTextures2 != nullptr);
	// S2.15 gate diag: editor fails to arm FAST water path - print once
	// per (mission/state-change) what gate fails. Throttled by signature.
	static const bool s_gateDiag = false;
	if (s_gateDiag)
	{
		const bool solidArmed = gos_terrain_indirect::IsFrameSolidArmed();
		uint32_t sig = (g1?1:0) | (solidArmed?2:0) | (g3?4:0) | (g4?8:0) | (g5?16:0);
		static uint32_t s_lastSig = 0xffffffff;
		if (sig != s_lastSig)
		{
			mc2::fmt::print_out("[WATER_GATE] fastPath={:d} armed={:d} streamReady={:d} recipes={:d} tex2={:d} (sig=0x{:x})\n", mc2::fmt::as_i32(g1?1:0), mc2::fmt::as_i32(solidArmed?1:0), mc2::fmt::as_i32(g3?1:0), mc2::fmt::as_u32(static_cast<unsigned>(WaterStream::GetRecipeCount())), mc2::fmt::as_i32(g5?1:0), mc2::fmt::as_u32(sig));
			fflush(stdout);
			s_lastSig = sig;
		}
	}
	return g1 && g2 && g3 && g4 && g5;
}

//---------------------------------------------------------------------------
void Terrain::renderWater (void)
{
	ZoneScopedN("Terrain::renderWater");

	// MC2_WATER_DEBUG=1: post-warmup population recon for the renderWater slice.
	// Reports per-frame how many quads are pure-skip (waterHandle == 0xffffffff,
	// out-of-frustum or non-water by map data) vs handle-valid (the upper bound
	// on the actually-emitting subset). Mirrors the MC2_THIN_DEBUG pattern in
	// quad.cpp: prints 5 frames after a 1200-frame warmup hold-off, then dormant.
	static const bool s_waterDebugOn = false;
	static uint32_t s_total = 0;
	static uint32_t s_handleValid = 0;
	static uint32_t s_detailEligibleByHandle = 0;
	static uint32_t s_framesPrinted = 0;
	static uint32_t s_frameCounter = 0;
	static uint64_t s_qpcFreq = 0;
	static uint64_t s_qpcStart = 0;
	constexpr uint32_t kWaterWarmupHoldoffFrames = 1200;
	if (s_waterDebugOn && s_qpcFreq == 0)
		QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&s_qpcFreq));
	if (s_waterDebugOn)
		QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&s_qpcStart));

	// Predicate is now single-sourced in gos_terrain_indirect::WaterFastPathOwnsArmedDraw().
	// renderWater() is once-per-frame, so this is the correct (non-hot) site
	// for the S6 armed-skip probe - it observes the EXACT predicate the
	// quad.cpp setupTextures (ii) gate uses, so armedSkip=1 here == "(ii)
	// legacy draw-side skipped this frame, GPU fast path owns it".
	const bool s6FastPathOwns = gos_terrain_indirect::WaterFastPathOwnsArmedDraw();
	{
		static const bool s_waterS6Trace = false;
		if (s_waterS6Trace)
		{
			static int s_lastS6 = -1;
			int s6 = s6FastPathOwns ? 1 : 0;
			if (s6 != s_lastS6)
			{
				mc2::fmt::print_out("[WATER_S6 v1] event=state armedSkip={:d} (1=GPU fast path owns armed draw; legacy (ii) draw-side skipped this frame)\n", mc2::fmt::as_i32(s6));
				fflush(stdout);
				s_lastS6 = s6;
			}
		}
	}
	if (s6FastPathOwns)
	{
		// Skip legacy loop entirely; renderWaterFastPath() does the work.
		return;
	}

	// [DEPTH_TRANSITION v1] reset the CPU-water REAL screen-z nearest-vertex
	// search once per CPU-water frame (env-gated; silent default). Reached
	// ONLY when the legacy loop runs (s6FastPathOwns early-returned above),
	// i.e. exactly the frames CPU water is the live producer. The stamp bump
	// lets the transition dump in gos_terrain_indirect.cpp detect a STALE
	// CPU sample on armed frames (CPU water and the GPU fast path are
	// mutually exclusive per frame). Pure writes, zero behavior change.
	{
		static const bool s_depthTransProbe = false;
		if (s_depthTransProbe)
		{
			extern float              g_cpuWaterProbeZ;
			extern double             g_cpuWaterProbeBestD2;
			extern bool               g_cpuWaterProbeAny;
			extern unsigned long long g_cpuWaterProbeStamp;
			(void)g_cpuWaterProbeZ;
			g_cpuWaterProbeAny = false;
			g_cpuWaterProbeBestD2 = 0.0;
			++g_cpuWaterProbeStamp;
		}
	}

	//-----------------------------------
	// Draw resulting terrain quads
	TerrainQuadPtr currentQuad = quadList;

	const bool collect = s_waterDebugOn && (s_framesPrinted < 5)
	                     && (s_frameCounter >= kWaterWarmupHoldoffFrames);
	uint32_t traceTotal = 0;
	uint32_t traceHandleValid = 0;
	uint32_t traceDetailEligible = 0;

	for (long i=0;i<numberQuads;i++)
	{
		++traceTotal;
		if (currentQuad->waterHandle != 0xffffffff)
		{
			++traceHandleValid;
			if (currentQuad->waterDetailHandle != 0xffffffff)
				++traceDetailEligible;
		}
		if (collect)
		{
			++s_total;
			if (currentQuad->waterHandle != 0xffffffff)
			{
				++s_handleValid;
				if (currentQuad->waterDetailHandle != 0xffffffff)
					++s_detailEligibleByHandle;
			}
		}

		if (drawTerrainTiles)
			currentQuad->drawWater();

		currentQuad++;
	}

	{
		static bool s_haveLast = false;
		static uint32_t s_lastHandleValid = 0;
		const bool disappeared = (s_haveLast && s_lastHandleValid > 0 && traceHandleValid == 0);
		const bool recovered = (s_haveLast && s_lastHandleValid == 0 && traceHandleValid > 0);
		if (disappeared || recovered || !s_haveLast) {
			mc2::fmt::print_err("[WATER_LEGACY v1] event=population total={:d} handle_valid={:d} detail_eligible={:d} state={} prev_handle_valid={:d}\n", mc2::fmt::as_u32(traceTotal), mc2::fmt::as_u32(traceHandleValid), mc2::fmt::as_u32(traceDetailEligible), mc2::fmt::safe(disappeared ? "disappeared" : (recovered ? "recovered" : "initial")), mc2::fmt::as_u32(s_lastHandleValid));
			fflush(stderr);
		}
		s_haveLast = true;
		s_lastHandleValid = traceHandleValid;
	}

	if (s_waterDebugOn)
	{
		uint64_t qpcEnd = 0;
		QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&qpcEnd));
		const uint64_t elapsedTicks = qpcEnd - s_qpcStart;
		const double elapsedUs = (s_qpcFreq > 0)
		    ? (1000000.0 * static_cast<double>(elapsedTicks) / static_cast<double>(s_qpcFreq))
		    : 0.0;
		if (s_framesPrinted < 5)
		{
			++s_frameCounter;
			if (s_frameCounter >= kWaterWarmupHoldoffFrames)
			{
				mc2::fmt::print_err("[WATER_DEBUG v1] event=population frame={:d} (post-warmup) total={:d} handle_valid={:d} detail_eligible={:d} elapsed_us={:.1f}\n", mc2::fmt::as_u32(s_framesPrinted), mc2::fmt::as_u32(s_total), mc2::fmt::as_u32(s_handleValid), mc2::fmt::as_u32(s_detailEligibleByHandle), elapsedUs);
				fflush(stderr);
				s_total = s_handleValid = s_detailEligibleByHandle = 0;
				++s_framesPrinted;
			}
		}
	}
}

//---------------------------------------------------------------------------
// Stage 2 of renderWater architectural slice. Called from gamecam.cpp AFTER
// mc2::VkSceneRenderer::renderLists(mc2::renderSubmissions()) so terrain has been flushed and
// depth-written. This is required for the alpha-blend-on-top semantics:
// running fast-path INSIDE renderWater() (before renderLists) means terrain
// hasn't drawn yet and overwrites our water.
void Terrain::renderWaterFastPath (void)
{
	// MC2_RENDER_WATER_FASTPATH: legacy fast-path gate. MC2_GPU_DRIVEN_WATER
	// also enables the fast-path entry (the MDI branch inside the bridge).
	static const bool s_fastPath =
	    (mc2::gate::get<"MC2_RENDER_WATER_FASTPATH">()) ||
	    gpu_driven::IsWaterEnabled();
	if (!s_fastPath) return;
	if (!WaterStream::IsReady()) return;
	if (WaterStream::GetRecipeCount() == 0) return;
	if (!Terrain::terrainTextures2) return;

	ZoneScopedN("Terrain::renderWaterFastPath");

	// MC2_WATER_DEBUG=1 parallel timer (matches the legacy renderWater printer
	// style at terrain.cpp:1004-1077, post-warmup window of 5 frames). Lets
	// gate B Tracy/perf comparison run side-by-side with one env var.
	static const bool s_waterDebugOn = false;
	static uint64_t s_qpcFreq2 = 0;
	static uint64_t s_qpcStart2 = 0;
	static uint32_t s_framesPrinted2 = 0;
	static uint32_t s_frameCounter2  = 0;
	constexpr uint32_t kFastWarmupHoldoffFrames = 1200;
	if (s_waterDebugOn && s_qpcFreq2 == 0)
		QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&s_qpcFreq2));
	if (s_waterDebugOn)
		QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&s_qpcStart2));

	// getWater*Handle() returns mc2::textures::ready()'s textureIndex (master node
	// id), NOT the engine's gosTextureHandle. tex_resolve() chases the lazy
	// first-touch indirection — same pattern as M2d overlay at quad.cpp:2084.
	const DWORD waterTexIdx =
	    Terrain::terrainTextures2->getWaterTextureHandle();
	const DWORD waterDetailTexIdx =
	    Terrain::terrainTextures2->getWaterDetailHandle(sprayFrame);
	const DWORD waterTexHandle =
	    (waterTexIdx != 0xffffffff) ? tex_resolve(waterTexIdx) : 0u;
	const DWORD waterDetailTexHandle =
	    (waterDetailTexIdx != 0xffffffff) ? tex_resolve(waterDetailTexIdx) : 0xffffffffu;

	const float oneOverWaterTF =
	    Terrain::terrainTextures2->getWaterDetailTilingFactor()
	    / Terrain::worldUnitsMapSide;
	const float oneOverTF =
	    Terrain::terrainTextures2->getWaterTextureTilingFactor()
	    / Terrain::worldUnitsMapSide;

	const float cloudOffsetX =
	    cosf(360.0f * DEGREES_TO_RADS * 32.0f * cloudScrollX) * 0.1f;
	const float cloudOffsetY =
	    sinf(360.0f * DEGREES_TO_RADS * 32.0f * cloudScrollY) * 0.1f;
	const float sprayOffsetX = cloudScrollX * 10.0f;
	const float sprayOffsetY = cloudScrollY * 10.0f;

	{
		static bool s_dumped = false;
		if (!s_dumped && false) {
			s_dumped = true;
			mc2::fmt::print_err("[WATER_FAST v1] event=alpha_uniforms waterElevation={:.3f} alphaDepth={:.3f} alphaEdgeByte={:d} alphaMiddleByte={:d} alphaDeepByte={:d}\n", static_cast<double>(Terrain::waterElevation), static_cast<double>(MapData::alphaDepth), mc2::fmt::as_u32(static_cast<unsigned>(((Terrain::alphaEdge   >> 24) & 0xFFu))), mc2::fmt::as_u32(static_cast<unsigned>(((Terrain::alphaMiddle >> 24) & 0xFFu))), mc2::fmt::as_u32(static_cast<unsigned>(((Terrain::alphaDeep   >> 24) & 0xFFu))));
			fflush(stderr);
		}
	}
	gos_terrain_bridge_renderWaterFast(
	    WaterStream::GetRecipeCount(),
	    static_cast<unsigned int>(waterTexHandle),
	    static_cast<unsigned int>(waterDetailTexHandle),
	    Terrain::waterElevation,
	    WaterStream::EffAlphaDepth(MapData::alphaDepth),   // SHORELINE-STAIRCASE-1: widened shore band (default OFF -> base)
	    static_cast<unsigned int>(((Terrain::alphaEdge   >> 24) & 0xFFu)),
	    static_cast<unsigned int>(((Terrain::alphaMiddle >> 24) & 0xFFu)),
	    static_cast<unsigned int>(((Terrain::alphaDeep   >> 24) & 0xFFu)),
	    Terrain::mapTopLeft3d.x,
	    Terrain::mapTopLeft3d.y,
	    Terrain::frameCos,
	    Terrain::frameCosAlpha,
	    oneOverTF,
	    oneOverWaterTF,
	    cloudOffsetX,
	    cloudOffsetY,
	    sprayOffsetX,
	    sprayOffsetY,
	    MaxMinUV);

	if (s_waterDebugOn)
	{
		uint64_t qpcEnd = 0;
		QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&qpcEnd));
		const uint64_t elapsedTicks = qpcEnd - s_qpcStart2;
		const double elapsedUs = (s_qpcFreq2 > 0)
		    ? (1000000.0 * static_cast<double>(elapsedTicks) / static_cast<double>(s_qpcFreq2)) : 0.0;
		++s_frameCounter2;
		if (s_frameCounter2 >= kFastWarmupHoldoffFrames && s_framesPrinted2 < 5)
		{
			mc2::fmt::print_err("[WATER_FAST v1] event=elapsed frame={:d} (post-warmup) recipeCount={:d} elapsed_us={:.1f}\n", mc2::fmt::as_u32(s_framesPrinted2), mc2::fmt::as_u32(static_cast<unsigned>(WaterStream::GetRecipeCount())), elapsedUs);
			fflush(stderr);
			++s_framesPrinted2;
		}
	}
}
