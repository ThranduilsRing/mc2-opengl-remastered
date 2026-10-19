// MC_TextureManager::renderLists phases 8-11 (dynamic shadow pass, solid terrain, GPU static props
// and mechs, terrain overlay / decal batches).

#include "txmmgr_render_internal.h"
#include "mc2fmt.h"
void MC_TextureManager::renderListsDynamicShadowPass (void)
{
	// GPU-driven dynamic sun shadow (Phase 1): frustum-fit + flushShadow.
	// Runs BEFORE gpu_cull::compute_dispatch so the static-prop shadow uses the
	// full camera-visible per-type ranges (not the cull-narrowed indirect).
	// Casters = the camera-visible (inView) batched set (Phase 1 scope; the
	// off-screen-caster low-sun shadow is the documented Phase-2 gap).
	{
		ZoneScopedN("RenderLists.DynamicShadowPass");
		extern bool g_useGpuObjects;
		extern bool g_useGpuMechs;
		// ISSUE-34: modern-shadow-path predicate (was tess-keyed) — dynamic
		// shadow pass stays on when tessellation is off.
		if (gos_IsModernShadowPathActive() && (g_useGpuObjects || g_useGpuMechs)) {
			// Unproject 8 GL-NDC corners through the inverse of worldToClipGL().
			// SHADOW-DYNAMIC-PROJECTION-FIX-1: worldToClipGL() maps MC2 world (Z-up)
			//   = kAxisSwapMC2toGL * worldToCam * cameraToClipGL
			// -> GL clip (NDC xy[-1,1], z[0,1], w>0 in front), with the MC2->GL axis
			// swap (x'=-x, y'=z, z'=y) ALREADY BAKED IN. So inverting it maps a GL-NDC
			// corner straight back to MC2 world: NO manual (-x,z,y) swizzle and NO w<0
			// negate (this matrix emits positive-w-in-front).
			// PRIOR BUG: the code inverted getWorldToClip() (D3D pixel-homogeneous,
			// Y-down, w<0 in front) but fed it the GL-NDC cube below. GL-NDC and that
			// native clip differ by kPixelHomogToGLNDC (Y-flip + x offset + w sign-flip,
			// camera.cpp), never applied -> the unprojected box was mirrored/
			// offset, only ~correct when the frustum was axis-aligned/centered. That is
			// why trees shadowed only at certain camera angles.
			static const float ndc[8][3] = {
				{-1.0f,-1.0f, 0.0f},{ 1.0f,-1.0f, 0.0f},
				{-1.0f, 1.0f, 0.0f},{ 1.0f, 1.0f, 0.0f},
				{-1.0f,-1.0f, 1.0f},{ 1.0f,-1.0f, 1.0f},
				{-1.0f, 1.0f, 1.0f},{ 1.0f, 1.0f, 1.0f}
			};
			float cornersMC2[8][3];
			// Invert(src) stores the inverse of src into *this (matrix.hpp).
			glm::mat4 clipToWorldGL;
			clipToWorldGL = glm::inverse((eye->worldToClipGL()));
			for (int c = 0; c < 8; ++c) {
				Stuff::Vector4D in, out;
				in.x = ndc[c][0]; in.y = ndc[c][1]; in.z = ndc[c][2]; in.w = 1.0f;
				out.Multiply(in, clipToWorldGL);      // row-vector * matrix; result is MC2-world homogeneous
				float inv = (fabsf(out.w) > 1e-6f) ? (1.0f / out.w) : 0.0f;
				cornersMC2[c][0] = out.x * inv;       // already MC2 (axis swap baked into worldToClipGL)
				cornersMC2[c][1] = out.y * inv;
				cornersMC2[c][2] = out.z * inv;
			}
			float lx, ly, lz;
			gos_GetTerrainLightDir(&lx, &ly, &lz);   // same accessor used by old shim
			// Focus source = the camera ORBIT TARGET (getPosition), not a
			// screen-center terrain raycast. The orbit target is yaw-invariant
			// MC2-world east/north (.x=east WU, .y=north WU; yaw is applied only
			// to the eye direction, never to position) and moves linearly with
			// panning, so the cascade no longer skews with the horizon or with
			// facing. Z = cameraShiftZ (terrain elevation at the target, already
			// clamped up to waterElevation), giving the correct water-clamped
			// look-at height in the same MC2 world space as cornersMC2
			// (x=east,y=north,z=elev). Always valid -- no raycast fallback needed.
			glm::vec3 camTgt = eye->getPosition();        // MC2-world orbit target (east,north)
			float shadowCenterXYZ[3] = { camTgt.x, camTgt.y, eye->getCameraShiftZ() };
			bool scOk = true;                                   // camera target always valid
			gos_BuildDynamicLightMatrix(-lx, -ly, -lz, cornersMC2,
			                            shadowCenterXYZ, scOk);  // sign matches old shim
			// SAME-ORDER-EXECUTOR-SLICE-2: validate-only ownership of the Shadow top-level
			// pass (dynamic shadow only — static is once/mission, not wrapped here).
			// Body sets its own state (FBO bind, depth, lightSpaceMatrix); we just validate.
			render_contract::executorOwnBeginTopLevel(render_contract::PassIdentity::ShadowCaster,
			                                          "renderLists_gos_BeginDynamicShadowPass");
			// Item 1 P1: caster set used by the CSM cascade replay (set in the
			// prop-caster path below).
			const std::vector<GpuStaticPropInstance>* csmCasterSet = nullptr;
			// The caster set is the whole registry of non-building props (visibility-independent), so every tree and prop in the light box casts;
			// buildings cast via the world-fixed static map. Only the VK shadow island draws it (the GL caster draws are gone).
			{
				static std::vector<GpuStaticPropInstance> s_dynPropInsts; // reused; accessor clears
					// SHADOW-DYNAMIC-PROP-DIRTY-ONLY-1 (gate, DEFAULT OFF; set env to enable):
					// The caster set (registry-indexed instances: modelMatrix + typeID) is
					// REGISTRY-STATIC — it changes only when a prop spawns, despawns, or has
					// an immutable-field write, all of which bump s_registryGeneration. The
					// shadow pass uses only modelMatrix+typeID from the instances (no lighting
					// data, no per-frame color offset), so the vector is safe to cache across
					// frames as long as the generation is unchanged.
					// Cache policy: rebuild when the registry generation changes. On a clean scene
					// getDynamicPropShadowInstances would walk the full s_recipeRanges vector (~14K recipes);
					// skipping it saves ~120-150 us of cache-cold memory traffic per frame.
					// DEFAULT-ON since 2026-06-04 (smoke-clean mc2_24; proven
					// s_registryGeneration dirty-only pattern, shadow pass reads only
					// modelMatrix+typeID): a value starting with "0" opts out.
					static const bool s_dynPropDirtyOnly = mc2::gate::get<"MC2_SHADOW_DYNAMIC_PROP_DIRTY_ONLY">();
					static uint64_t  s_dynPropInstsGeneration = UINT64_MAX; // sentinel: force first build
					if (!s_dynPropDirtyOnly ||
					    GpuStaticPropRegistry::getRegistryGeneration() != s_dynPropInstsGeneration) {
						GpuStaticPropRegistry::getDynamicPropShadowInstances(s_dynPropInsts);
						s_dynPropInstsGeneration = GpuStaticPropRegistry::getRegistryGeneration();
					}

					// SHADOW-CASTER-LIGHTBOX-CULL-1 (gate MC2_SHADOW_CASTER_LIGHTBOX_CULL,
					// DEFAULT OFF). The caster set above is the WHOLE registry (~14K props,
					// visibility-independent). The dynamic shadow map is camera-fit (small,
					// esp. under ShadowBoundedNearFit), so thousands of off-map props
					// draw shadows that never land in the map = wasted GPU fill. When ON,
					// filter the casters to only those whose origin projects inside the
					// dynamic shadow frustum (+margin), using the EXACT same matrix the
					// shadow VS uses (getDynamicLightSpaceMatrix(), column-major, GL_FALSE).
					// World-position frame: shaders/shadow_static_prop.vert computes
					//   worldStuff = vec4(a_position,1) * modelMatrix   (row-vector * M)
					//   worldMC2   = vec3(-worldStuff.x, worldStuff.z, worldStuff.y)
					//   gl_Position = lightSpaceMatrix * vec4(worldMC2,1)
					// For the local origin a_position=(0,0,0): worldStuff = (M[3],M[7],M[11])
					// in the column-major float[16] (v*M picks element c*4+3), so
					//   worldMC2 = (-M[3], M[11], M[7])
					// This is EXACTLY the registry's actorWorldCenter extraction
					// (gos_static_prop_registry.cpp) and the frame the light matrix
					// consumes — verified against both the VS and the SHADOWZRANGE probe.
					// DEFAULT ON since 2026-06-16 (render-hygiene-s1). Off-map props
					// never land their shadows on the playfield — filtering them saves
					// ~0.2-0.4ms GPU fill on prop-heavy missions (mc2_24). Kill: =0.
					static const bool s_casterLightboxCull = mc2::gate::get<"MC2_SHADOW_CASTER_LIGHTBOX_CULL">();
					const std::vector<GpuStaticPropInstance>* dynShadowSet = &s_dynPropInsts;
					if (s_casterLightboxCull) {
						ZoneScopedN("Shadow.CasterCull");
						static std::vector<GpuStaticPropInstance> s_culledDynPropInsts; // reused, no churn
						// SHADOW-CASTER-CULL-CACHE-1 (TXMMGR-PERF-EASYWINS-1, gate
						// MC2_SHADOW_CASTER_CULL_CACHE, DEFAULT OFF): the cull result is a
						// pure function of (caster set, light-space matrix, margin). The
						// caster set is generation-keyed (rebuilt above only on
						// s_registryGeneration change) and the matrix is camera-fit — on a
						// stationary camera its 16 floats are bit-identical across frames.
						// Reuse the previous culled vector when BOTH are unchanged; any
						// camera motion or registry mutation recomputes exactly as before.
						// margin is session-static (env-init'd) — no key part.
						static const bool s_cullCacheEnabled = mc2::gate::get<"MC2_SHADOW_CASTER_CULL_CACHE">();
						static uint64_t s_cullCacheGen = UINT64_MAX;   // sentinel: no cache
						static float    s_cullCacheM[16] = {};
						static const float s_cullMargin = mc2::gate::get<"MC2_SHADOW_CASTER_CULL_MARGIN">();
						static const bool s_cullDebug = mc2::gate::get<"MC2_SHADOW_CULL_DEBUG">();
						gosPostProcess* ppCull = getGosPostProcess();
						const float* M = ppCull ? ppCull->getDynamicLightSpaceMatrix() : nullptr;
						if (!M) {
							// No matrix yet (early frames) -> keep everything (safe).
							dynShadowSet = &s_dynPropInsts;
						} else if (s_cullCacheEnabled &&
						           s_cullCacheGen == GpuStaticPropRegistry::getRegistryGeneration() &&
						           0 == memcmp(s_cullCacheM, M, sizeof(s_cullCacheM))) {
							// Cache HIT: same caster generation + bit-identical light matrix
							// -> the cull result is unchanged; reuse the retained vector.
							dynShadowSet = &s_culledDynPropInsts;
						} else {
							s_culledDynPropInsts.clear();
							s_culledDynPropInsts.reserve(s_dynPropInsts.size());
							const float lim = 1.0f + s_cullMargin;
							int dbgN = 0;
							for (const GpuStaticPropInstance& inst : s_dynPropInsts) {
								const float* mm = inst.modelMatrix;
								// world (MC2 frame the light matrix consumes)
								const float wx = -mm[3];
								const float wy =  mm[11];
								const float wz =  mm[7];
								// clip = M * (wx,wy,wz,1), M column-major: (row r,col c)=M[c*4+r]
								const float cx = M[0*4+0]*wx + M[1*4+0]*wy + M[2*4+0]*wz + M[3*4+0];
								const float cy = M[0*4+1]*wx + M[1*4+1]*wy + M[2*4+1]*wz + M[3*4+1];
								const float cw = M[0*4+3]*wx + M[1*4+3]*wy + M[2*4+3]*wz + M[3*4+3];
								bool keep = true;
								if (cw > 0.0f) {
									const float ndcx = cx / cw;
									const float ndcy = cy / cw;
									keep = (ndcx >= -lim && ndcx <= lim &&
									        ndcy >= -lim && ndcy <= lim);
								} else {
									// behind the light near plane: cannot land in the map.
									keep = false;
								}
								if (s_cullDebug && dbgN < 8) {
									++dbgN;
									const float ndcx = (cw != 0.0f) ? cx / cw : 0.0f;
									const float ndcy = (cw != 0.0f) ? cy / cw : 0.0f;
									mc2::fmt::print_err("[SHADOW_CULL_DBG] world=({:.1f},{:.1f},{:.1f}) clipW={:.3f} ndc=({:.3f},{:.3f}) keep={:d}\n", wx, wy, wz, cw, ndcx, ndcy, mc2::fmt::as_i32(keep ? 1 : 0));
									fflush(stderr);
								}
								if (keep) s_culledDynPropInsts.push_back(inst);
							}
							dynShadowSet = &s_culledDynPropInsts;
							if (s_cullCacheEnabled) {
								s_cullCacheGen = GpuStaticPropRegistry::getRegistryGeneration();
								memcpy(s_cullCacheM, M, sizeof(s_cullCacheM));
							}

							static bool s_cullLogged = false;
							if (!s_cullLogged) {
								s_cullLogged = true;
								const size_t total = s_dynPropInsts.size();
								const size_t kept  = s_culledDynPropInsts.size();
								const float pct = total ? (100.0f * (float)(total - kept) / (float)total) : 0.0f;
								mc2::fmt::print_err("[SHADOW_CULL] casters {:d} -> {:d} ({:.0f}% culled, margin={:.2f})\n", mc2::fmt::as_size(total), mc2::fmt::as_size(kept), pct, s_cullMargin);
								fflush(stderr);
							}
						}
					}
#ifdef MC2_VULKAN_ISLAND
				// The VK shadow island re-renders the dynShadowSet caster instances into its own D32 shadow map (the consumers bind it directly).
				if (mc2_vk_shadow_island_enabled()) {
					ZoneScopedN("RenderLists.VkShadowInterop");
					gosPostProcess* ppVk = getGosPostProcess();
					if (ppVk) {
						mc2_vk_shadow_island_run(
							ppVk->getDynamicShadowMapSize(),
							ppVk->getDynamicLightSpaceMatrix(),
							dynShadowSet->data(),
							(unsigned int)dynShadowSet->size(),
							ppVk->getShadowBiasFactor(),
							ppVk->getShadowBiasUnits());
					}
				}
#endif
				csmCasterSet = dynShadowSet;   // Item 1 P1: remember for the cascade replay
			}
			render_contract::executorOwnEndTopLevel(render_contract::PassIdentity::ShadowCaster,
			                                        "renderLists_gos_EndDynamicShadowPass");

			// Item 1 P1: the VK shadow island records each CSM layer from the same caster set (the GL per-layer replay is gone).
			// Skipped entirely when the ShadowCSM option is off.
			if (mc2ShadowCsmEnabled()) {
				gosPostProcess* ppCsm = getGosPostProcess();
				if (ppCsm && ppCsm->csmBuilt()) {
					ZoneScopedN("RenderLists.DynShadowCSM");
					const int csmN = ppCsm->getDynamicShadowCascadeCount();
					for (int ci = 0; ci < csmN; ++ci) {
#ifdef MC2_VULKAN_ISLAND
						if (ci < csmN - 1 && csmCasterSet && mc2_vk_shadow_island_enabled()) {
							const float* mats = ppCsm->getDynamicCascadeMatrices();
							const float* layerMat = mats ? (mats + (size_t)ci * 16u) : nullptr;
							mc2_vk_shadow_island_run_csm_array_layer(
								ppCsm->getDynamicShadowMapSize(),
								csmN - 1,
								ci,
								layerMat,
								csmCasterSet->data(),
								(unsigned int)csmCasterSet->size(),
								ppCsm->getShadowBiasFactor(),
								ppCsm->getShadowBiasUnits());
						}
						if (ci == csmN - 1 && csmCasterSet && mc2_vk_shadow_island_enabled()) {
							const float* mats = ppCsm->getDynamicCascadeMatrices();
							const float* lastMat = mats ? (mats + (size_t)ci * 16u) : nullptr;
							mc2_vk_shadow_island_run_csm_fullmap(
								ppCsm->getDynamicFullMapSize(),
								lastMat,
								csmCasterSet->data(),
								(unsigned int)csmCasterSet->size(),
								ppCsm->getShadowBiasFactor(),
								ppCsm->getShadowBiasUnits());
						}
#endif
					}
					// P5 headless log: layers + prop caster-set size fed to each
					// cascade (per-cascade containment is decided GPU-side in the
					// shader; this proves the array pass ran with casters).
					static bool s_csmLogged = false;
					if (!s_csmLogged) {
						s_csmLogged = true;
						const size_t nCasters = csmCasterSet ? csmCasterSet->size() : 0;
						mc2::fmt::print_err("[CSM] layers={:d} casters_per_layer={:d}\n", mc2::fmt::as_i32(csmN), mc2::fmt::as_size(nCasters));
						fflush(stderr);
					}
#ifdef MC2_VULKAN_ISLAND
					// MC2_VK_SHADOW_MATRIX_DIAG (Trace, default OFF = one cached-bool test).
					if (mc2_vk_shadow_matrix_diag_enabled()) {
						mc2_vk_shadow_matrix_diag_tick(
							ppCsm->getDynamicLightSpaceMatrix(), ppCsm->getDynamicShadowMapSize(),
							ppCsm->getDynamicCascadeMatrices(), csmN, ppCsm->getDynamicShadowMapSize(),
							ppCsm->getDynamicFullMapSize(), ppCsm->getDynamicFullMapTexelWorld());
					}
#endif
				}
			}
		}
#ifdef MC2_VULKAN_ISLAND
		// The VK shadow passes of the frame were recorded into one command buffer; submit it before any consumer (terrain, the screen shadow) reads.
		if (mc2_vk_shadow_island_enabled()) mc2_vk_shadow_island_submit_frame();
#endif
	}
	g_numShadowShapes.store(0, std::memory_order_relaxed);
}

void MC_TextureManager::renderListsTerrainSolid (void)
{
	// No special depth state for DRAWSOLID terrain

	{
		ZoneScopedN("Render.TerrainSolid");
		TracyGpuZone("Render.TerrainSolid");

		// MC2_LOAD_RACE_TRACE: terrain GPU-residency probe. The degraded
		// mission-load state renders terrain flat/untextured; this latches the
		// first frame on which terrain is fully GPU-resident. Two independent
		// readiness sources, selected by which renderer is actually active
		// (mc2TerrainLodChunkEnabled(), cached static bool -- cheap read, no walk):
		//   - Default chunk/GPU-indirect path: masterVertexNodes[] is NEVER
		//     populated with terrain nodes here (the legacy per-quad walk below
		//     is suppressed on this path), so the old terrTotal>0 walk always
		//     false-negatived (terrain_resident=N on every run, even healthy
		//     ones). Reuse the SAME cached latches gos_terrain_indirect.cpp
		//     already sets for its own "[LOADRACE] atlas first-resident" /
		//     "[LOADRACE] gpu terrain first-arm" one-shot lines -- two long
		//     compares, no new walk.
		//   - Legacy per-quad path (MC2_TERRAIN_LOD_CHUNK=0): unchanged
		//     masterVertexNodes walk (non-mutating isTextureNodeResident, so it
		//     never forces an upload).
		// Also refreshes g_loadRaceTerrainResidentNow each frame for the verdict.
		{
			extern bool     g_loadRaceTrace;
			extern long     g_loadRaceTerrainFirstResidentFrame;
			extern bool     g_loadRaceTerrainResidentNow;
			extern uint32_t g_mc2FrameCounter;
			extern long     g_loadRaceAtlasFirstFrame;   // gos_terrain_indirect.cpp latch
			extern long     g_loadRaceArmFirstFrame;     // gos_terrain_indirect.cpp latch
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
					for (long ti = 0; ti < nextAvailableVertexNode; ++ti)
					{
						const DWORD tf = masterVertexNodes[ti].flags;
						if ((tf & MC2_DRAWSOLID) && (tf & MC2_ISTERRAIN) &&
							masterVertexNodes[ti].vertices)
						{
							++terrTotal;
							if (mc2::textures::isDecoded(masterVertexNodes[ti].textureIndex))
								++terrResident;
						}
					}
					fully = (terrTotal > 0) && (terrResident == terrTotal);
				}
				g_loadRaceTerrainResidentNow = fully;
				if (fully && g_loadRaceTerrainFirstResidentFrame < 0)
				{
					g_loadRaceTerrainFirstResidentFrame = (long)g_mc2FrameCounter;
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

		// TERRAIN-CULL-STATE-PROBE-1 (MC2_TERRAIN_CULL_PROBE, default OFF): the
		// terrain-solid dispatch CHOKEPOINT — runs every frame terrain renders,
		// before the indirect/patch-stream/legacy branch. None of those paths set
		// cull, so the ambient cull/frontFace read HERE is what terrain renders
		// under. Read-only; first few frames then stops.
		{
			static const bool s_cullProbe = mc2::gate::get<"MC2_TERRAIN_CULL_PROBE">();
			static int s_cullProbeFrames = 0;
			if (s_cullProbe && s_cullProbeFrames < 8) {
				++s_cullProbeFrames;
				GLboolean cullOn = glIsEnabled(GL_CULL_FACE);
				GLint cullMode = 0, frontFace = 0;
				glGetIntegerv(GL_CULL_FACE_MODE, &cullMode);
				glGetIntegerv(GL_FRONT_FACE, &frontFace);
				mc2::fmt::print_err("[TERRAIN_CULL_PROBE] path=dispatch frame={:d} cull={} cullMode={} frontFace={}\n", mc2::fmt::as_i32(s_cullProbeFrames), mc2::fmt::safe(cullOn ? "ENABLED" : "disabled"), mc2::fmt::safe(cullMode == GL_BACK ? "BACK" : cullMode == GL_FRONT ? "FRONT" :
						cullMode == GL_FRONT_AND_BACK ? "FRONT_AND_BACK" : "?"), mc2::fmt::safe(frontFace == GL_CCW ? "CCW" : frontFace == GL_CW ? "CW" : "?"));
				std::fflush(stderr);
			}
		}

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
		for (long i=0;i<nextAvailableVertexNode && !bSkip_DRAWSOLID;i++)
		{
				if ((masterVertexNodes[i].flags & MC2_DRAWSOLID) &&
					(masterVertexNodes[i].vertices))
				{
					// LEGACY-MLR-DELETE-1: legacy masterVertexNode terrain draw retired. Terrain solid is
					// owned by LOD-chunk (gamecam.cpp) / indirect bridge / patch-stream. A terrain node
					// must NOT draw via this shared MC2_DRAWSOLID loop. Always skip terrain here. If a terrain
					// node arrives with modernHandled==false, no modern path drew terrain this frame — a real
					// regression (terrain would be invisible). Fire the LegacyMLR telemetry counter as a loud
					// tripwire so it surfaces in terrain_path.legacy_mlr instead of silently vanishing.
					if (masterVertexNodes[i].flags & MC2_ISTERRAIN) {
						if (!modernHandled)
							RenderCore::framegraph::noteTerrainPath(RenderCore::framegraph::TerrainPath::LegacyMLR);
						masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
						continue;
					}

					gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
					gos_SetRenderState( gos_State_Terrain, 0 );

#ifdef MC2_VULKAN_ISLAND
                    vkLegacySubmitNode(i, MC2_LEGACY_PASS_SOLID, 0);
#endif
					masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
				}
			}
#ifdef MC2_VULKAN_ISLAND
		vkLegacyRun();   // VK-PARITY-F1: queued legacy SOLID nodes -> shared GBuffer
#endif
		// Emit one [BUCKET_CENSUS v1] line per frame (env-gated). Runs
		// after flush() has populated the modern-side stats and after
		// either branch has reset the legacy ring; legacy_eligible was
		// captured pre-flush above. emitCensus() is a no-op when the
		// env var is unset.

	}   // end Render.TerrainSolid zone
}

void MC_TextureManager::renderListsGpuStaticProps (void)
{
	// Task 10 flush() — moved here from after Render.3DObjects because
	// terrain renders AFTER 3D objects in this codebase; placing our
	// flush earlier meant terrain overwrote our pixels. Running after
	// terrain but before overlays gives buildings the right layering:
	// on top of terrain, below decals/roads.
	{
		ZoneScopedN("Render.GpuStaticProps");
		TracyGpuZone("Render.GpuStaticProps");
		extern bool g_useGpuStaticProps;
		extern bool g_useGpuObjects;
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
				TracyGpuZone("GpuSP.CullDispatch");
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
			extern bool g_useGpuStaticProps;
			extern bool g_useGpuObjects;
			if (g_useGpuStaticProps || g_useGpuObjects) {
				ZoneScopedN("GpuSP.BatcherFlush");
				TracyGpuZone("GpuSP.BatcherFlush");
				GpuStaticPropBatcher::instance().flush(getLastRenderSnapshot());
			}
		};

		// Unit B: GPU mech batcher Slice A flush — runs adjacent to the static-prop
		// flush, inside renderLists() so terrain has already been emitted by the
		// patch stream and the depth state is set up. Independent of
		// g_useGpuStaticProps; gated on its own MC2_GPU_MECHS env var inside the
		// flush itself. Pass_Mechs is a disjoint GL_TIME_ELAPSED scope.
		auto runMechFlushUnit = []() {
			ZoneScopedN("Render.GpuMechs");
			TracyGpuZone("Render.GpuMechs");
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
			GpuMechBatcher::instance().flush();
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

void MC_TextureManager::renderListsOverlayBatches (void)
{
	// DRAWSOLID done

	// ── New world-space overlay batches ──────────────────────────────────────
	// These draw calls flush batches accumulated during land->render() and
	// craterManager->render().  They set their own GL state and restore it.
	// Render order matches design: terrain → cement overlays → decals → (old overlays).
	{
		ZoneScopedN("Render.TerrainOverlays");
		TracyGpuZone("Render.TerrainOverlays");
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
		TracyGpuZone("Render.TerrainOverlaysStatic");
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
		TracyGpuZone("Render.TerrainMines");
		if (gos_terrain_indirect::IsFrameMineArmed()) {
			gos_terrain_indirect::DrawMineStatic();
		}
	}
	{
		ZoneScopedN("Render.Decals");
		TracyGpuZone("Render.Decals");
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
