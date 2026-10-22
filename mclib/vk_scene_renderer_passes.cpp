#include "vk_scene_renderer.h"
// mc2::VkSceneRenderer::renderLists phases 8-11 (dynamic shadow pass, solid terrain, GPU static props
// and mechs, terrain overlay / decal batches).

#include "vk_scene_renderer_internal.h"
#include "mc2fmt.h"
#include "../GameOS/gameos/static_caster_set.h"
#include "../GameOS/gameos/shadow_mover_feed.h"
#include <cstring>

extern bool g_useGpuObjects;
extern bool g_useGpuMechs;
extern bool     g_loadRaceTrace;
extern long     g_loadRaceTerrainFirstResidentFrame;
extern bool     g_loadRaceTerrainResidentNow;
extern uint32_t g_mc2FrameCounter;
extern long     g_loadRaceAtlasFirstFrame;
extern long     g_loadRaceArmFirstFrame;
extern bool g_useGpuStaticProps;
void mc2::VkSceneRenderer::renderListsDynamicShadowPass (RenderSubmissions& submissions)
{
#ifdef MC2_VULKAN_ISLAND

		// Publish this frame's poses before fitting or drawing the mover map.
		// The colour island consumes this same snapshot after terrain.
		GpuMechBatcher::instance().flush();
		if (gos_IsModernShadowPathActive() && (g_useGpuObjects || g_useGpuMechs)) {
			const glm::mat4 camera=eye->worldToClipGL();
			float rows[16];
			std::memcpy(rows,&camera,sizeof(rows)); // Camera::projectModernClipGL uses these four explicit rows
			float sun[3]; gos_GetTerrainLightDir(&sun[0],&sun[1],&sun[2]);
			static std::vector<GpuStaticPropInstance> movers;
			static std::vector<GpuStaticPropInstance> registryMovers;
			static uint64_t registryGeneration=~0ull, casterVersion=~0ull, casterDelta=~0ull;
			const auto& set=mc2shadow::staticCasterSet();
			const uint64_t generation=GpuStaticPropRegistry::getRegistryGeneration();
			if (generation!=registryGeneration || set.version()!=casterVersion || set.deltaVersion()!=casterDelta) {
				GpuStaticPropRegistry::getDynamicPropShadowInstancesExcluding(registryMovers,set.recipeMask());
				registryGeneration=generation; casterVersion=set.version(); casterDelta=set.deltaVersion();
			}
			movers=registryMovers;
			set.appendMoverLeaves(movers,g_mc2FrameCounter);
			mc2shadow::moverPropFeed().append(movers,g_mc2FrameCounter);
			render_contract::executorOwnBeginTopLevel(render_contract::PassIdentity::ShadowCaster,"renderLists_unionMovers");
			mc2_vk_shadow_island_run_movers(rows,sun,movers.data(),static_cast<unsigned int>(movers.size()));
			render_contract::executorOwnEndTopLevel(render_contract::PassIdentity::ShadowCaster,"renderLists_unionMovers");
		}
		mc2_vk_shadow_island_submit_frame();
		g_numShadowShapes.store(0,std::memory_order_relaxed);
		return;

#endif
}

void mc2::VkSceneRenderer::renderListsTerrainSolid (RenderSubmissions& submissions)
{
	// No special depth state for DRAWSOLID terrain

	{
		ZoneScopedN("Render.TerrainSolid");

		// MC2_LOAD_RACE_TRACE: terrain GPU-residency probe. The degraded
		// mission-load state renders terrain flat/untextured; this latches the
		// first frame on which terrain is fully GPU-resident. Two independent
		// readiness sources, selected by which renderer is actually active
		// (mc2TerrainLodChunkEnabled(), cached static bool -- cheap read, no walk):
		//   - Default chunk/GPU-indirect path: submissions.masterVertexNodes[] is NEVER
		//     populated with terrain nodes here (the legacy per-quad walk below
		//     is suppressed on this path), so the old terrTotal>0 walk always
		//     false-negatived (terrain_resident=N on every run, even healthy
		//     ones). Reuse the SAME cached latches gos_terrain_indirect.cpp
		//     already sets for its own "[LOADRACE] atlas first-resident" /
		//     "[LOADRACE] gpu terrain first-arm" one-shot lines -- two long
		//     compares, no new walk.
		//   - Legacy per-quad path (MC2_TERRAIN_LOD_CHUNK=0): unchanged
		//     submissions.masterVertexNodes walk (non-mutating isTextureNodeResident, so it
		//     never forces an upload).
		// Also refreshes g_loadRaceTerrainResidentNow each frame for the verdict.
		{






			if (g_loadRaceTrace)
			{
				bool fully;
				long terrTotal = 0;
				const bool chunkPath = mc2TerrainLodChunkEnabled();
				if (chunkPath)
				{
					fully = (g_loadRaceAtlasFirstFrame >= 0) &&
						(g_loadRaceArmFirstFrame >= 0);
				}
				else
				{
					long terrResident = 0;
					for (long ti = 0; ti < submissions.nextAvailableVertexNode; ++ti)
					{
						const DWORD tf = submissions.masterVertexNodes[ti].flags;
						if ((tf & MC2_DRAWSOLID) && (tf & MC2_ISTERRAIN) &&
							submissions.masterVertexNodes[ti].vertices)
						{
							++terrTotal;
							if (mc2::textures::isDecoded(submissions.masterVertexNodes[ti].textureIndex))
								++terrResident;
						}
					}
					fully = (terrTotal > 0) && (terrResident == terrTotal);
				}
				g_loadRaceTerrainResidentNow = fully;
				if (fully && g_loadRaceTerrainFirstResidentFrame < 0)
				{
					g_loadRaceTerrainFirstResidentFrame = static_cast<long>(g_mc2FrameCounter);
					if (chunkPath)
						mc2::fmt::print_out("[LOADRACE] terrain first fully-resident frame={:d} (chunk path: atlas_frame={:d} arm_frame={:d})\n", mc2::fmt::as_i32(g_loadRaceTerrainFirstResidentFrame), mc2::fmt::as_i32(g_loadRaceAtlasFirstFrame), mc2::fmt::as_i32(g_loadRaceArmFirstFrame));
					else
						mc2::fmt::print_out("[LOADRACE] terrain first fully-resident frame={:d} (terrainNodes={:d})\n", mc2::fmt::as_i32(g_loadRaceTerrainFirstResidentFrame), mc2::fmt::as_i32(terrTotal));
					std::fflush(stdout);
				}
			}
		}

		// Modern path. flush() returns true on success and false on overflow
		// / not-ready / not-killswitched. On false we fall through to the
		// legacy loop for the WHOLE FRAME — never partial-frame. The legacy
		// ring data has been kept in sync by addVertices
		// running unconditionally in quad.cpp.
		// Bucket-census instrumentation (env-gated MC2_BUCKET_CENSUS=1).
		// Count legacy-eligible nodes BEFORE flush() runs (both the legacy
		// draw branch at ~1369 and the alternate reset branch at ~1382
		// zero currentVertex per node, so end-of-zone undercounts). The
		// filter mirrors the legacy DRAWSOLID|ISTERRAIN draw-emission
		// predicate so it is apples-to-apples with PatchStream's modern
		// scope.
		bool modernHandled = false;

		if (gos_terrain_indirect::IsFrameSolidArmed()) {
			// Indirect SOLID owns this frame. The SOLID gate-off in setupTextures()
			// already fired, so TerrainPatchStream has no SOLID records — do NOT fall
			// back to flush() when DrawIndirect returns false (plan v2 advisor
			// stop-the-line #1). A false return is a hard failure: logged, arming
			// disabled process-wide; operator advice in event=hard_failure line.
			// Phase 7.5 LOD chunk coexistence: when MC2_TERRAIN_LOD_CHUNK=1 the chunk
			// path owns terrain rendering (flushDrawCommands in gamecam.cpp). Suppress
			// DrawIndirect to prevent depth/color conflicts — both paths write the same
			// world-space surface and the indirect path draws first (land->render),
			// causing the coarser LOD chunk mesh to fail the depth test.
			if (mc2TerrainLodChunkEnabled()) {
				modernHandled = true;   // chunks handle it (existing)
			} else {
				// INDIRECT-BRIDGE-RETIRE-1: indirect-bridge SOLID terrain draw retired (deprecated branch,
				// terrain_path.indirect proven 0 in default/smoke/capture/editor). LOD-chunk owns terrain.
				// Do NOT DrawIndirect(); fire the telemetry counter as a regression tripwire. modernHandled
				// MUST be true here: on armed frames setupTextures already fired SOLID gate-off so PatchStream
				// has no SOLID records — a false value would fall through to the (now terrain-skipping) shared
				// loop and could mis-fire the LegacyMLR tripwire. NOTE: DrawIndirect() itself is RETAINED
				// (RenderWaterReflectionPass calls it internally, gos_terrain_indirect.cpp); only this
				// txmmgr SOLID-draw caller is retired.
				RenderCore::framegraph::noteTerrainPath(RenderCore::framegraph::TerrainPath::IndirectBridge);
				modernHandled = true;
			}
		}

		// RENDER-FRAME-PLAN-SCAFFOLD-1: tattle WHICH terrain-solid branch drew this
		// frame. This is the rake-prevention trace — capture takes the legacy MLR
		// branch (modern bridge not armed), which bound TerrainSolid 0x and confused
		// the routing recon. Observe only; no behavior change.
		if (render_frame_plan::traceEnabled()) {
			using namespace render_frame_plan;
			const bool armed = gos_terrain_indirect::IsFrameSolidArmed();
			if (modernHandled && armed && !mc2TerrainLodChunkEnabled())
				trace(Phase::TerrainOpaque, "TerrainSolid", PathKind::ApplyPipeline, -1, "TerrainSolid");      // modern indirect bridge (routed)
			else if (modernHandled && armed)
				trace(Phase::TerrainOpaque, "TerrainSolidLODChunk", PathKind::ApplyPipeline, -1, "TerrainSolid");  // LOD-chunk, routed (ROUTING-1)
			else if (modernHandled)
				trace(Phase::TerrainOpaque, "TerrainSolidThin", PathKind::RawGL, -1, "None");                 // un-armed patch-stream flush
			else
				trace(Phase::TerrainOpaque, "TerrainLegacyMLR", PathKind::MLR, -1, "None");                   // legacy master-node fall-through
		}


		bool bSkip_DRAWSOLID = false;
		for (long i=0;i<submissions.nextAvailableVertexNode && !bSkip_DRAWSOLID;i++)
		{
				if ((submissions.masterVertexNodes[i].flags & MC2_DRAWSOLID) &&
					(submissions.masterVertexNodes[i].vertices))
				{
					// LEGACY-MLR-DELETE-1: legacy masterVertexNode terrain draw retired. Terrain solid is
					// owned by LOD-chunk (gamecam.cpp) / indirect bridge / patch-stream. A terrain node
					// must NOT draw via this shared MC2_DRAWSOLID loop. Always skip terrain here. If a terrain
					// node arrives with modernHandled==false, no modern path drew terrain this frame — a real
					// regression (terrain would be invisible). Fire the LegacyMLR telemetry counter as a loud
					// tripwire so it surfaces in terrain_path.legacy_mlr instead of silently vanishing.
					if (submissions.masterVertexNodes[i].flags & MC2_ISTERRAIN) {
						if (!modernHandled)
							RenderCore::framegraph::noteTerrainPath(RenderCore::framegraph::TerrainPath::LegacyMLR);
						submissions.masterVertexNodes[i].currentVertex = submissions.masterVertexNodes[i].vertices;
						continue;
					}

					gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
					gos_SetRenderState( gos_State_Terrain, 0 );

#ifdef MC2_VULKAN_ISLAND
                    vkLegacySubmitNode(submissions, i, MC2_LEGACY_PASS_SOLID, 0);
#endif
					submissions.masterVertexNodes[i].currentVertex = submissions.masterVertexNodes[i].vertices;
				}
			}
#ifdef MC2_VULKAN_ISLAND
		vkLegacyRun(submissions);   // VK-PARITY-F1: queued legacy SOLID nodes -> shared GBuffer
#endif
		// Emit one [BUCKET_CENSUS v1] line per frame (env-gated). Runs
		// after flush() has populated the modern-side stats and after
		// either branch has reset the legacy ring; legacy_eligible was
		// captured pre-flush above. emitCensus() is a no-op when the
		// env var is unset.

	}   // end Render.TerrainSolid zone
}

void mc2::VkSceneRenderer::renderListsGpuStaticProps (RenderSubmissions& submissions)
{
	// Task 10 flush() — moved here from after Render.3DObjects because
	// terrain renders AFTER 3D objects in this codebase; placing our
	// flush earlier meant terrain overwrote our pixels. Running after
	// terrain but before overlays gives buildings the right layering:
	// on top of terrain, below decals/roads.
	{
		ZoneScopedN("Render.GpuStaticProps");


		if (g_useGpuStaticProps || g_useGpuObjects) {
			// [RENDERLISTS_COST v1] prep+cull-dispatch only; the two flush units
			// below carry their own spans (no double-count vs total).
			// Stage 3.C registry flush MOVED earlier (before flushShadow, see
			// STATIC-PROP SHADOW-ORDER FIX 2026-05-29) so terrain-object instances
			// are in s_bucketsByType before the shadow-time SSBO upload locks the
			// slot. It injects static-registry instances into batcher buckets AND
			// appends Cat_StaticProp substrate records for the cull shader; both are
			// now done above, before this block. Do NOT re-add a flush() call here
			// (double-flush). compute_dispatch() below still runs after it.

			// Step 4.6 (global-pool slice 1): compute per-cmd baseInstance prefix-sum
			// and advance the coalesce ring slot BEFORE compute_dispatch() so the
			// patch shader can read baseInstanceByCmd[] in the same dispatch.
			// The GpuSP.PrepBaseInstance Tracy zone (override branch).
			{
				ZoneScopedN("GpuSP.PrepBaseInstance");
				batcher_prepareBaseInstanceTable();
			}

			// C1b GPU authority flip: compute_dispatch() is now called HERE
			// (moved from mission.cpp) so it processes BOTH dynamic actor records
			// (from substrate_flushUpload in objmgr::update) AND the static prop
			// records appended by GpuStaticPropRegistry::flush() above.
			// The patch shader then writes GPU-computed instanceCounts into the
			// indirect command buffer. GpuStaticPropBatcher::flush() below uses
			// glMultiDrawElementsIndirect which reads those GPU-authoritative counts.
			// M1 FROZEN-STATIC-CULL-RECORDS: build + install the frozen static
			// cull-record prefix now that baseInstanceForType is valid (set by
			// batcher_prepareBaseInstanceTable above) and before compute_dispatch
			// consumes the records. No-op on clean frames / unless the gate is set.
			{ GpuStaticPropRegistry::buildStaticPrefixGolden(); }

			if (gpu_cull::compute_isEnabled()) {
				ZoneScopedN("GpuSP.CullDispatch");
				// VK-GL-LOST-DRAWS: nullptr tag = unmeasured. compute_dispatch() is compute-only (no raster); a
				// GL_SAMPLES_PASSED segment holding only compute + barriers returns a garbage
				// ~1e12 on this driver (begin snapshot never taken) and mis-attributes the frame's
				// real draws to it. Suspend the query for it.
				gpu_cull::compute_dispatch();
			}

		}

		// MEASURED-REORDER-SPMECH-1 (tier-C reorder EXPERIMENT, default-OFF):
		// The two opaque flush UNITS below — StaticProp BatcherFlush (+Pass_SpColor
		// timer) and the Mech block (+Pass_Mechs timer) — are the one legal adjacent
		// swap the legal-reorder oracle proved. StaticProp PREP + compute_dispatch
		// (above, inside Render.GpuStaticProps) must stay BEFORE both flushes in
		// EITHER path; only the two flush units swap order. The OpaqueObject
		// begin(executorOwnBeginTopLevel @ Render.3DObjects)/end wrapper brackets
		// both regardless. Gate OFF -> today's order verbatim (byte-identical).
		// Gate ON -> mech flush, then static-prop flush.
		static const bool s_reorderSpMech = mc2::gate::get<"MC2_FRAMEGRAPH_REORDER_SPMECH">();

		// Unit A: static-prop batcher flush, scoped by Pass_SpColor. Captured pre-
		// flush eligibility lives in the extern flags read in Render.GpuStaticProps;
		// re-read here (same flags) so the flush still respects the GPU-path gate.
		auto runSpFlushUnit = []() {


			if (g_useGpuStaticProps || g_useGpuObjects) {
				ZoneScopedN("GpuSP.BatcherFlush");
				GpuStaticPropBatcher::instance().flush(getLastRenderSnapshot());
			}
		};

		// Unit B: GPU mech batcher Slice A flush — runs adjacent to the static-prop
		// flush, inside renderLists(submissions) so terrain has already been emitted by the
		// patch stream and the depth state is set up. Independent of
		// g_useGpuStaticProps; gated on its own MC2_GPU_MECHS env var inside the
		// flush itself. Pass_Mechs is a disjoint GL_TIME_ELAPSED scope.
		auto runMechFlushUnit = []() {
			ZoneScopedN("Render.GpuMechs");
			// [RENDER_PASS v1] advisory telemetry (env-gated, rate-limited).
			// MECHOPAQUE-NOTE-RELOCATE-1: note fires at the real mech GPU draw
			// site (after shadow, after static-prop flush) — NOT at the preamble.
			// FBO/viewport facts here are draw-time (scene FBO + SceneGEqual depth
			// state established by terrain/static-prop passes above).
			render_contract::noteRenderPass(render_contract::PassIdentity::OpaqueObject,
			                               "GpuMechBatcher_flush(submit)");
			// APPLY-STATE-MECHOPAQUE-1: when MC2_FRAMEGRAPH_EXECUTOR is ON and a
			// kTopLevelStateDesc row exists, the executor records the MechOpaque pass here
			// (at the real flush call site, NOT the begin seam which wraps non-mech draws too).
			if (render_contract::isTopLevelExecutorEnabled() &&
			    RenderCore::framegraph::findTopLevelStateDesc(RenderCore::RenderPassId::MechOpaque) != nullptr) {
				GpuMechBatcher::instance().executorApplyMechOpaqueState();
			}
			// VK-P14 MECH TIER-1: flush() BUILDS the instance/bone/run feeds (CPU only); the island
			// then co-writes the skinned mechs IN-PLACE into the shared GBuffer. Fail-soft (island 0)
			// = mechs dropped this frame, no corruption.

#ifdef MC2_VULKAN_ISLAND
			if (mc2_vk_mech_island_enabled()) {
				ZoneScopedN("Render.GpuMechs.VkTier1");
				if (gosPostProcess* ppVkMech = getGosPostProcess()) {
					(void)mc2_vk_mech_island_run(ppVkMech->getWidth(), ppVkMech->getHeight());
				}
			}
#endif
		};

		if (s_reorderSpMech) {
			// EXPERIMENT (gate-ON): swap the two opaque flush units.
			runMechFlushUnit();
			runSpFlushUnit();
		} else {
			// Default path: today's order verbatim — static-prop flush, then mechs.
			runSpFlushUnit();
			runMechFlushUnit();
		}
		render_contract::executorOwnEndTopLevel(render_contract::PassIdentity::OpaqueObject,
		                                        "renderLists_GpuMechBatcher_flush");
	}
}

void mc2::VkSceneRenderer::renderListsOverlayBatches (RenderSubmissions& submissions)
{
	// DRAWSOLID done

	// ── New world-space overlay batches ──────────────────────────────────────
	// These draw calls flush batches accumulated during land->render() and
	// craterManager->render().  They set their own GL state and restore it.
	// Render order matches design: terrain → cement overlays → decals → (old overlays).
	{
		ZoneScopedN("Render.TerrainOverlays");
		gos_DrawTerrainOverlays();
	}
	// Slice A — cement-overlay static-bake draw. Mirrors the Render.Terrain
	// Mines hook below EXACTLY: gated on IsFrameOverlayArmed() (default OFF
	// unless MC2_TERRAIN_INDIRECT_OVERLAY=1). When armed, the per-quad M2d
	// gos_PushTerrainOverlay producer is skipped (quad.cpp gate-off) and
	// gos_DrawTerrainOverlays above flushes an empty batch (early-return);
	// DrawDecalStatic draws the persistent static bake instead. Placed right
	// after Render.TerrainOverlays so the static cement composites in the
	// same slot the per-frame batch used (before mines/decals/old overlays).
	{
		ZoneScopedN("Render.TerrainOverlaysStatic");
		if (gos_terrain_indirect::IsFrameOverlayArmed()) {
			gos_terrain_indirect::DrawDecalStatic();
		}
	}
	// PR2c Stage 2c — mine static-bake draw. Hooks between TerrainOverlays
	// and Decals so mines composite ABOVE cement/road overlays and BENEATH
	// crater decals (state=2 blown-mine sprites coexist with crater decals).
	// Default-off: IsFrameMineArmed() returns false unless
	// MC2_TERRAIN_INDIRECT_MINE=1 AND the texture-array has been built.
	{
		ZoneScopedN("Render.TerrainMines");
		if (gos_terrain_indirect::IsFrameMineArmed()) {
			gos_terrain_indirect::DrawMineStatic();
		}
	}
	{
		ZoneScopedN("Render.Decals");
		// MC2_DYNAMIC_DECALS: push dynamic impact decals into the batch before flushing.
		// Gate-off: gatherToDecalBatch() is a no-op; gate-on: pushes faded quads.
		// frameLength = duration of last frame in seconds (timing.h).
		DynDecal::gatherToDecalBatch(frameLength);
		gos_DrawDecals();
	}
#ifdef MC2_VULKAN_ISLAND
	// VK-PARITY-OVERLAY-1: the static cement/road overlay bake and the decal batch were queued
	// (not GL-drawn) when the terrain island is armed; draw them into the shared GBuffer now,
	// in the same slot (after terrain/objects, before the old overlays).
	if (mc2_vk_overlay_enabled())
	{
		if (gosPostProcess* ppVkOv = getGosPostProcess())
			(void)mc2_vk_overlay_run(ppVkOv->getWidth(), ppVkOv->getHeight());
	}
#endif
	// ── End new world-space overlay batches ───────────────────────────────────
}
