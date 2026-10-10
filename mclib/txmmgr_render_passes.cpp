// MC_TextureManager::renderLists phases 8-11 (dynamic shadow pass, solid terrain, GPU static props
// and mechs, terrain overlay / decal batches).

#include "txmmgr_render_internal.h"

void MC_TextureManager::renderListsDynamicShadowPass (void)
{
	// GPU-driven dynamic sun shadow (Phase 1): frustum-fit + flushShadow.
	// Runs BEFORE gpu_cull::compute_dispatch so the static-prop shadow uses the
	// full camera-visible per-type ranges (not the cull-narrowed indirect).
	// Casters = the camera-visible (inView) batched set (Phase 1 scope; the
	// off-screen-caster low-sun shadow is the documented Phase-2 gap).
	{
		ZoneScopedN("RenderLists.DynamicShadowPass");
		rlcost::Span _rl(rlcost::kDynShadow);
		gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_ShadowDyn);
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
			Stuff::Matrix4D clipToWorldGL;
			clipToWorldGL.Invert(eye->worldToClipGL());
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
			Stuff::Vector3D camTgt = eye->getPosition();        // MC2-world orbit target (east,north)
			float shadowCenterXYZ[3] = { camTgt.x, camTgt.y, eye->getCameraShiftZ() };
			bool scOk = true;                                   // camera target always valid
			gos_BuildDynamicLightMatrix(-lx, -ly, -lz, cornersMC2,
			                            shadowCenterXYZ, scOk);  // sign matches old shim
			// SAME-ORDER-EXECUTOR-SLICE-2: validate-only ownership of the Shadow top-level
			// pass (dynamic shadow only — static is once/mission, not wrapped here).
			// Body sets its own state (FBO bind, depth, lightSpaceMatrix); we just validate.
			render_contract::executorOwnBeginTopLevel(render_contract::PassIdentity::ShadowCaster,
			                                          "renderLists_gos_BeginDynamicShadowPass");
			gos_BeginDynamicShadowPass();             // no-op if shadowsEnabled_ false
#ifdef MC2_VULKAN_ISLAND
			// VK-P8a SHADOW-INTEROP: clear any stale VK-produced shadow tex at the top of
			// the bracket so a frame where the island declines/isn't reached (e.g. the
			// non-prop-caster flushShadow branch) never leaves a stale VK tex bound for the
			// forward consumers. The producer below re-publishes it on success. No-op when
			// the gate is OFF (the tex is already 0 and stays 0).
			if (mc2_vk_shadow_island_enabled()) {
				if (gosPostProcess* ppClr = getGosPostProcess())
					ppClr->setDynamicShadowInteropTexture(0);
			}
#endif
			// Item 1 P1: caster set used by the CSM cascade replay (set in the
			// prop-caster path below; nullptr => CSM replay uses flushShadow).
			const std::vector<GpuStaticPropInstance>* csmCasterSet = nullptr;
			// SHADOW-STATIC-BUILDINGS-2: when buildings cast via the world-fixed
			// static map, skip them in the dynamic pass to avoid a redundant fuzzy
			// double-shadow on buildings (trees/mechs still cast dynamically).
			static const bool s_skipBldgInDynamic = !mc2::gate::is<"MC2_STATIC_PROP_BUILDING_SHADOW", "0">();
			// SHADOW-DYNAMIC-PROP-CASTERS-1 (gate, DEFAULT ON; =0 kill-switch): the camera-visible
			// flushShadow feed (s_typeRanges) only admits props the camera frustum
			// cull marked visible this frame, so most trees never cast into the (now
			// correctly camera-fit) dynamic map -- only the few nearest the camera.
			// When enabled, replay ALL registered NON-building props from the
			// registry (visibility-independent) into the dynamic map instead, so
			// every tree/prop in the light box casts. Buildings keep casting via the
			// world-fixed static map, so this pairs with MC2_STATIC_PROP_BUILDING_SHADOW
			// (with that OFF, buildings get no dynamic shadow -- intended for the
			// combined config). flushShadow is skipped entirely in this mode (it would
			// only double-draw the camera-visible subset). Mechs still cast via their
			// own batcher. Gate OFF -> byte-identical to prior behavior.
			static const bool s_dynPropCasters =
				!mc2::gate::is<"MC2_SHADOW_DYNAMIC_PROP_CASTERS", "0">();  // DEFAULT ON; =0 kill-switch
			if (s_dynPropCasters) {
				static std::vector<GpuStaticPropInstance> s_dynPropInsts; // reused; accessor clears
				// Include buildings in the dynamic feed ONLY when the static building
					// map is NOT active (else buildings would double-shadow). This keeps
					// buildings casting under bare MC2_SHADOW_ENABLE (no regression).
					const bool includeBldg = !s_skipBldgInDynamic;
					// SHADOW-DYNAMIC-PROP-DIRTY-ONLY-1 (gate, DEFAULT OFF; set env to enable):
					// The caster set (registry-indexed instances: modelMatrix + typeID) is
					// REGISTRY-STATIC — it changes only when a prop spawns, despawns, or has
					// an immutable-field write, all of which bump s_registryGeneration. The
					// shadow pass uses only modelMatrix+typeID from the instances (no lighting
					// data, no per-frame color offset), so the vector is safe to cache across
					// frames as long as the generation is unchanged.
					// Cache policy: rebuild when (a) generation changes, or (b) includeBldg
					// toggles (env-gated at startup — in practice stable per session, but
					// compare it anyway). On a clean scene getDynamicPropShadowInstances walks
					// the full s_recipeRanges vector (~14K recipes); skipping it saves ~120-
					// 150µs of cache-cold memory traffic per frame.
					// NOTE: includeBldg is captured from a static at the outer scope, so it
					// is also effectively constant per session; the extra compare is free.
					// DEFAULT-ON since 2026-06-04 (smoke-clean mc2_24; proven
					// s_registryGeneration dirty-only pattern, shadow pass reads only
					// modelMatrix+typeID): a value starting with "0" opts out.
					static const bool s_dynPropDirtyOnly = mc2::gate::get<"MC2_SHADOW_DYNAMIC_PROP_DIRTY_ONLY">();
					static uint64_t  s_dynPropInstsGeneration = UINT64_MAX; // sentinel: force first build
					static bool      s_dynPropInstsIncludeBldg = false;
					if (!s_dynPropDirtyOnly ||
					    GpuStaticPropRegistry::getRegistryGeneration() != s_dynPropInstsGeneration ||
					    includeBldg != s_dynPropInstsIncludeBldg) {
						GpuStaticPropRegistry::getDynamicPropShadowInstances(s_dynPropInsts, includeBldg);
						s_dynPropInstsGeneration  = GpuStaticPropRegistry::getRegistryGeneration();
						s_dynPropInstsIncludeBldg = includeBldg;
					}

					// SHADOW-CASTER-LIGHTBOX-CULL-1 (gate MC2_SHADOW_CASTER_LIGHTBOX_CULL,
					// DEFAULT OFF). The caster set above is the WHOLE registry (~14K props,
					// visibility-independent). The dynamic shadow map is camera-fit (small,
					// esp. under MC2_SHADOW_BOUNDED_NEAR_FIT), so thousands of off-map props
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
						// margin + includeBldg are session-static (env-init'd) — no key part.
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
									fprintf(stderr,
										"[SHADOW_CULL_DBG] world=(%.1f,%.1f,%.1f) "
										"clipW=%.3f ndc=(%.3f,%.3f) keep=%d\n",
										wx, wy, wz, cw, ndcx, ndcy, keep ? 1 : 0);
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
								fprintf(stderr,
									"[SHADOW_CULL] casters %zu -> %zu (%.0f%% culled, margin=%.2f)\n",
									total, kept, pct, s_cullMargin);
								fflush(stderr);
							}
						}
					}
				{
				// LANE-D measure-first: give the dynamic prop shadow CASTER draw its
				// own GPU timestamp so its cost is deconflated from GpuSP.BatcherFlush
				// (which previously absorbed it as first-GPU-zone self-time).
				ZoneScopedN("RenderLists.DynShadowDraw");
				TracyGpuZone("GpuSP.DynShadowDraw");
				GpuStaticPropBatcher::instance().drawDynamicPropShadows(*dynShadowSet);
			}
#ifdef MC2_VULKAN_ISLAND
				// VK-P8a SHADOW-INTEROP (gate MC2_VK_SHADOW_INTEROP, default-OFF): the SECOND
				// VK geometry region and the FIRST VK-PRODUCES-DEPTH region. VK re-renders the
				// SAME dynShadowSet caster instances into an exportable D32 shadow map on the
				// on-demand shared engine VulkanContext, signals vk2gl, and hands GL the imported
				// GL-texture view of that D32. GL then binds it in place of getDynamicShadowTexture()
				// so the four forward-shading consumers sample the VK-produced map via the UNCHANGED
				// hardware sampler2DShadow calcShadow() path. Ordering: VK produce -> VK signal vk2gl
				// -> GL wait (inside the island) -> here we publish the tex -> GL consumes at forward
				// shading. The GL caster draw above still ran (it fills the engine-owned depth tex);
				// its output is simply unused this frame when VK succeeds. NO GL->VK acquire is issued
				// (VK never reads a GL-written depth) -- that direction poisons the AMD interop
				// timeline (scene blackout). Gate OFF -> enabled()==0 -> interop tex stays 0 ->
				// getDynamicShadowTexture() returns the GL tex -> byte-identical.
				if (mc2_vk_shadow_island_enabled()) {
					ZoneScopedN("RenderLists.VkShadowInterop");
					gosPostProcess* ppVk = getGosPostProcess();
					if (ppVk) {
						const unsigned int vkTex = mc2_vk_shadow_island_run(
							ppVk->getDynamicShadowMapSize(),
							ppVk->getDynamicLightSpaceMatrix(),
							dynShadowSet->data(),
							(unsigned int)dynShadowSet->size(),
							ppVk->getShadowBiasFactor(),
							ppVk->getShadowBiasUnits());
						// >0 => VK produced+signalled this frame; publish the shared D32's GL view so
						// forward consumers sample it. 0 => island declined (fail-soft) -> leave the
						// interop tex cleared so consumers use the GL-produced map (no regression).
						ppVk->setDynamicShadowInteropTexture((GLuint)vkTex);
					}
				}
#endif
				csmCasterSet = dynShadowSet;   // Item 1 P1: remember for the cascade replay
			} else {
				GpuStaticPropBatcher::instance().flushShadow(s_skipBldgInDynamic);
			}
			GpuMechBatcher::instance().flushShadow();
			gos_EndDynamicShadowPass();
			render_contract::executorOwnEndTopLevel(render_contract::PassIdentity::ShadowCaster,
			                                        "renderLists_gos_EndDynamicShadowPass");

			// Item 1 P1: replay casters into each CSM array layer. The legacy
			// single-map pass above is fully complete (props+mech+EndPass restored
			// state). Each beginDynamicShadowCascade binds the array layer + sets
			// the active cascade matrix (via getDynamicLightSpaceMatrix resolving to
			// the active cascade) and clears forward-Z. Skipped entirely (no GL) and
			// byte-identical when MC2_SHADOW_CSM is OFF. csmCasterSet is non-null in
			// the prop-caster path; otherwise we fall back to flushShadow per layer.
			if (mc2ShadowCsmEnabled()) {
				gosPostProcess* ppCsm = getGosPostProcess();
				if (ppCsm && ppCsm->getDynamicShadowArrayTexture()) {
					ZoneScopedN("RenderLists.DynShadowCSM");
					const int csmN = ppCsm->getDynamicShadowCascadeCount();
					for (int ci = 0; ci < csmN; ++ci) {
						if (!ppCsm->beginDynamicShadowCascade(ci)) continue;
						if (csmCasterSet)
							GpuStaticPropBatcher::instance().drawDynamicPropShadows(*csmCasterSet);
						else
							GpuStaticPropBatcher::instance().flushShadow(s_skipBldgInDynamic);
						GpuMechBatcher::instance().flushShadow();
						ppCsm->endDynamicShadowCascadePass();
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
						fprintf(stderr, "[CSM] layers=%d casters_per_layer=%zu\n",
						        csmN, nCasters);
						fflush(stderr);
					}
#ifdef MC2_VULKAN_ISLAND
					// MC2_VK_SHADOW_MATRIX_DIAG (Trace, default OFF = one cached-bool test).
					if (mc2_vk_shadow_matrix_diag_enabled()) {
						mc2_vk_shadow_matrix_diag_tick(
							ppCsm->getDynamicLightSpaceMatrix(), ppCsm->getDynamicShadowMapSize(),
							ppCsm->getDynamicShadowTextureRaw(),
							ppCsm->getDynamicCascadeMatrices(), csmN, ppCsm->getDynamicShadowMapSize(),
							ppCsm->getDynamicShadowArrayTexture(), ppCsm->getDynamicFullMapTexture(),
							ppCsm->getDynamicFullMapSize(), ppCsm->getDynamicFullMapTexelWorld());
					}
#endif
				}
			}
		}
		gos_render_pass_timer::End(gos_render_pass_timer::Pass_ShadowDyn);
	}
	g_numShadowShapes.store(0, std::memory_order_relaxed);
}

void MC_TextureManager::renderListsTerrainSolid ([[maybe_unused]] const RenderListsFrame& st)
{
#ifdef MC2_VULKAN_ISLAND
	const bool vkLegacyOn = st.vkLegacyOn;
#endif
	// No special depth state for DRAWSOLID terrain

	{
		ZoneScopedN("Render.TerrainSolid");
	rlcost::Span _rl(rlcost::kTerrainSolid);
		TracyGpuZone("Render.TerrainSolid");
		gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_TerrainSolid);

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
							if (isTextureNodeResident(masterVertexNodes[ti].textureIndex))
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
						std::printf("[LOADRACE] terrain first fully-resident frame=%ld "
							"(chunk path: atlas_frame=%ld arm_frame=%ld)\n",
							g_loadRaceTerrainFirstResidentFrame,
							g_loadRaceAtlasFirstFrame, g_loadRaceArmFirstFrame);
					else
						std::printf("[LOADRACE] terrain first fully-resident frame=%ld "
							"(terrainNodes=%ld)\n",
							g_loadRaceTerrainFirstResidentFrame, terrTotal);
					std::fflush(stdout);
				}
			}
		}

		// Modern path. flush() returns true on success and false on overflow
		// / not-ready / not-killswitched. On false we fall through to the
		// legacy loop for the WHOLE FRAME — never partial-frame. The legacy
		// ring data has been kept in sync by addVertices/fillTerrainExtra
		// running unconditionally in quad.cpp.
		// Bucket-census instrumentation (env-gated MC2_BUCKET_CENSUS=1).
		// Count legacy-eligible nodes BEFORE flush() runs (both the legacy
		// draw branch at ~1369 and the alternate reset branch at ~1382
		// zero currentVertex per node, so end-of-zone undercounts). The
		// filter mirrors the legacy DRAWSOLID|ISTERRAIN draw-emission
		// predicate so it is apples-to-apples with PatchStream's modern
		// scope.
		static const bool s_bucketCensusOn = mc2::gate::get<"MC2_BUCKET_CENSUS">();
		uint32_t legacyEligible = 0;
		if (s_bucketCensusOn) {
			for (long ci = 0; ci < nextAvailableVertexNode; ++ci) {
				const DWORD cf = masterVertexNodes[ci].flags;
				if ((cf & MC2_DRAWSOLID) && (cf & MC2_ISTERRAIN) &&
				    masterVertexNodes[ci].vertices &&
				    masterVertexNodes[ci].currentVertex !=
				        masterVertexNodes[ci].vertices)
				{
					++legacyEligible;
				}
			}
		}

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
				std::fprintf(stderr,
					"[TERRAIN_CULL_PROBE] path=dispatch frame=%d cull=%s cullMode=%s frontFace=%s\n",
					s_cullProbeFrames,
					cullOn ? "ENABLED" : "disabled",
					cullMode == GL_BACK ? "BACK" : cullMode == GL_FRONT ? "FRONT" :
						cullMode == GL_FRONT_AND_BACK ? "FRONT_AND_BACK" : "?",
					frontFace == GL_CCW ? "CCW" : frontFace == GL_CW ? "CW" : "?");
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
		} else if (TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
			// PATCHSTREAM-THIN-RETIRE-1: patch-stream-thin terrain draw retired (deprecated branch,
			// terrain_path.patch_stream proven 0 in default/smoke/capture/editor). LOD-chunk owns
			// terrain. Do NOT flush()-draw; fire the telemetry counter as a regression tripwire so
			// patch_stream>0 surfaces if a reachable config ever needed it. modernHandled stays false
			// (no gate-off fired for un-armed frames) — matching the prior flush()-returns semantics
			// when it drew nothing.
			// NOTE: flush() side-effects audit — flush() does: (a) draw calls, (b) glFenceSync on
			// current slot, (c) markTerrainDrawn() on post-process, (d) noteTerrainPath(PatchStreamThin).
			// The ring slot advance is in beginFrame() NOT flush(), so skipping flush() does not break
			// the ring. The getLastFlush* snapshot getters read statics reset by beginFrame() each frame
			// — they will correctly read 0 when this branch is not flushing. markTerrainDrawn() is NOT
			// called here because no terrain was drawn. NO load-bearing non-draw side effects are lost.
			RenderCore::framegraph::noteTerrainPath(RenderCore::framegraph::TerrainPath::PatchStreamThin);
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

		// [TERRAIN_SURFACE] PR-2 (Wave 1, ADDITIVE / DEFAULT-OFF / DELETES
		// NOTHING). Screen-agnostic continuous-surface VALIDATION draw: runs
		// on EVERY frame regardless of arming (design Convergence C-1 --
		// surface existence is decoupled from IsFrameSolidArmed). A no-op
		// unless MC2_TERRAIN_SURFACE is set (gos_terrain_surface::IsEnabled,
		// checked inside the bridge), so the default path is byte-for-byte
		// behaviour-neutral. When ON, the surface draws ON TOP of the still-
		// running legacy/indirect terrain above for visual validation of the
		// V-ssbo VS + Fork D clip-space pre-divide reverse-Z bias. NO legacy
		// kill site lands here -- the substitutive draw-kill is PR-4.
		gos_terrain_surface_bridge_draw();

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

					DWORD totalVertices = masterVertexNodes[i].numVertices;
					if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
					{
						totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
					}

#ifdef MC2_VULKAN_ISLAND
					if (vkLegacyOn)
					{
						// VK-PARITY-F1: hand this node's CPU-lit gos_VERTEX triangle list to the
						// VK legacy-shape island (drawn after the loop) instead of GL.
						vkLegacySubmitNode(i, MC2_LEGACY_PASS_SOLID, 0);
						continue;
					}
#endif
					// LEGACY-MLR-DELETE-1: dead (terrain skipped above); non-terrain nodes never have terrain extras.
					gos_SetTerrainBatchExtras(nullptr, 0);

					if (totalVertices && (totalVertices < MAX_SENDDOWN))
					{
						gos_SetRenderState( gos_State_Texture, tex_resolve(masterVertexNodes[i].textureIndex));
						gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
					}
					else if (totalVertices > MAX_SENDDOWN)
					{
						gos_SetRenderState( gos_State_Texture, tex_resolve(masterVertexNodes[i].textureIndex));

						//Must divide up vertices into batches of 10,000 each to send down.
						// Somewhere around 20000 to 30000 it really gets screwy!!!
						long currentVertices = 0;
						while (currentVertices < totalVertices)
						{
							gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
							long tVertices = totalVertices - currentVertices;
							if (tVertices > MAX_SENDDOWN)
								tVertices = MAX_SENDDOWN;

							gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );

							currentVertices += tVertices;
						}
					}
					//Reset the list to zero length to avoid drawing more then once!
					//Also comes in handy if gameLogic is not called.
					masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
				}
			}
#ifdef MC2_VULKAN_ISLAND
		if (vkLegacyOn)
			vkLegacyRun();   // VK-PARITY-F1: queued legacy SOLID nodes -> shared GBuffer
#endif
		// Emit one [BUCKET_CENSUS v1] line per frame (env-gated). Runs
		// after flush() has populated the modern-side stats and after
		// either branch has reset the legacy ring; legacy_eligible was
		// captured pre-flush above. emitCensus() is a no-op when the
		// env var is unset.
		if (s_bucketCensusOn) {
			TerrainPatchStream::emitCensus(legacyEligible);
		}
		gos_render_pass_timer::End(gos_render_pass_timer::Pass_TerrainSolid);
	}   // end Render.TerrainSolid zone
	drainGLErrors("terrain");
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
		gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_SpColor);
		extern bool g_useGpuStaticProps;
		extern bool g_useGpuObjects;
		if (g_useGpuStaticProps || g_useGpuObjects) {
			// [RENDERLISTS_COST v1] prep+cull-dispatch only; the two flush units
			// below carry their own spans (no double-count vs total).
			rlcost::Span _rl(rlcost::kGpuSpPrep);
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
			// [SPFLUSH_COST_SPLIT v1] baseinstance_upload span (nifty) wrapped in
			// the GpuSP.PrepBaseInstance Tracy zone (override branch).
			{
				ZoneScopedN("GpuSP.PrepBaseInstance"); GosGlLostDrawsScope _lost_("gpusp.prep_base_instance");
				const unsigned long long _t_bi0 = s_spflushTxmEnabled ? __rdtsc() : 0ULL;
				batcher_prepareBaseInstanceTable();
				if (s_spflushTxmEnabled) s_spflush_baseinstance_upload_cyc += __rdtsc() - _t_bi0;
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
			{ GosGlLostDrawsScope _lost_("gpusp.static_prefix"); GpuStaticPropRegistry::buildStaticPrefixGolden(); }

			if (gpu_cull::compute_isEnabled()) {
				ZoneScopedN("GpuSP.CullDispatch"); GosGlLostDrawsScope _lost_(nullptr);
				// VK-GL-LOST-DRAWS: nullptr tag = unmeasured. compute_dispatch() is compute-only (no raster); a
				// GL_SAMPLES_PASSED segment holding only compute + barriers returns a garbage
				// ~1e12 on this driver (begin snapshot never taken) and mis-attributes the frame's
				// real draws to it. Suspend the query for it.
				TracyGpuZone("GpuSP.CullDispatch");
				gpu_cull::compute_dispatch();
			}

			// CAMERA-MOVE-DIAG: log substrate record count + cull output before/after
			// dispatch to distinguish compute early-return vs frustum-all-fail.
			// Gate: MC2_CAMERA_MOVE_DIAG=1. Causes GL stalls — diagnostic only.
			{
				static const bool s_diagEnabled = mc2::gate::get<"MC2_CAMERA_MOVE_DIAG">();
				if (s_diagEnabled) {
					static float s_lastMvp[16] = {};
					static uint32_t s_frameN = 0;
					extern const float* gos_GetTerrainMVPMat4();
					const float* mvp = gos_GetTerrainMVPMat4();
					bool mvpChanged = mvp && (memcmp(mvp, s_lastMvp, 16 * sizeof(float)) != 0);
					if (mvp && mvpChanged) memcpy(s_lastMvp, mvp, 16 * sizeof(float));
					++s_frameN;
					// Log every camera-move frame; stationary every 120 frames
					bool doLog = mvpChanged || (s_frameN % 120 == 0);
					if (doLog) {
						const uint32_t subRec = gpu_cull::substrate_getCurrentRecordCount();
						const GLintptr  subOff = gpu_cull::substrate_getCurrentSlotOffset();
						glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
						const GLuint bktBuf = gpu_cull::compute_getBucketCountsBuf();
						const GLuint cmdBuf = gpu_cull::compute_getIndirectCmdBuf();
						uint32_t bkt[4] = {};
						if (bktBuf) glGetNamedBufferSubData(bktBuf, 0, sizeof(bkt), bkt);
						uint32_t inst0 = 0;
						if (cmdBuf) glGetNamedBufferSubData(cmdBuf, 4, sizeof(inst0), &inst0); // struct[0].instanceCount
						// Sum ALL bucket counts across all 221+ buckets.
						uint32_t bktTotal = 0;
						const uint32_t nBkts = gpu_cull::compute_getBucketCount();
						if (bktBuf && nBkts > 0) {
							std::vector<uint32_t> allBkt(nBkts + 1u);
							glGetNamedBufferSubData(bktBuf, 0, (nBkts + 1u) * sizeof(uint32_t), allBkt.data());
							for (uint32_t b = 0; b < nBkts; ++b) bktTotal += allBkt[b];
						}
						printf("[DIAG] %s subRec=%u | bkt0=%u bkt1=%u bkt2=%u bkt3=%u bktTOTAL=%u | inst0=%u | mvp[0..3]=[%.3f %.3f %.3f %.3f]\n",
							mvpChanged ? "MOVE  " : "STATIC",
							subRec,
							bkt[0], bkt[1], bkt[2], bkt[3], bktTotal, inst0,
							mvp ? mvp[0] : 0.f, mvp ? mvp[1] : 0.f,
							mvp ? mvp[2] : 0.f, mvp ? mvp[3] : 0.f);
						fflush(stdout);
					}
				}
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
				rlcost::Span _rl(rlcost::kSpBatcherFlush);
#ifdef MC2_VULKAN_ISLAND
				// VK-P11 STATIC-PROP COLOR Tier-1 (gate MC2_VK_PROP_COLOR_TIER1):
				// render visible static-prop color instances directly into the shared
				// scene GBuffer in Vulkan instead of the GL flush() color draws. The
				// island pulls its per-frame instance feed from the batcher
				// (s_bucketsByType still populated here because we skip flush()). On
				// success skip the GL flush; on any fail-soft (0) fall through to GL so
				// props still draw. Gate OFF -> enabled()==0 -> GL flush() as before.
				bool vkPropColorHandled = false;
				if (mc2_vk_color_prop_island_enabled()) {
					ZoneScopedN("GpuSP.VkColorPropInterop");
					if (gosPostProcess* ppVkClr = getGosPostProcess()) {
						vkPropColorHandled = (1 == mc2_vk_color_prop_island_run(
							ppVkClr->getWidth(), ppVkClr->getHeight()));
					}
				}
				if (vkPropColorHandled) {
					// flush() (sole per-frame bucket clear) was skipped: clear here so
					// dynamic submits do not accumulate across island frames.
					mc2_color_prop_island_end_frame();
				} else
					GpuStaticPropBatcher::instance().flush(getLastRenderSnapshot());
#else
				GpuStaticPropBatcher::instance().flush(getLastRenderSnapshot());
#endif
			}
			// Pass_SpColor ends after the static-prop flush: GL_TIME_ELAPSED
			// scopes cannot nest, so mechs get their own disjoint scope.
			gos_render_pass_timer::End(gos_render_pass_timer::Pass_SpColor);
		};

		// Unit B: GPU mech batcher Slice A flush — runs adjacent to the static-prop
		// flush, inside renderLists() so terrain has already been emitted by the
		// patch stream and the depth state is set up. Independent of
		// g_useGpuStaticProps; gated on its own MC2_GPU_MECHS env var inside the
		// flush itself. Pass_Mechs is a disjoint GL_TIME_ELAPSED scope.
		auto runMechFlushUnit = []() {
			ZoneScopedN("Render.GpuMechs");
			TracyGpuZone("Render.GpuMechs");
			rlcost::Span _rl(rlcost::kMechFlush);
			gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_Mechs);
			// [RENDER_PASS v1] advisory telemetry (env-gated, rate-limited).
			// MECHOPAQUE-NOTE-RELOCATE-1: note fires at the real mech GPU draw
			// site (after shadow, after static-prop flush) — NOT at the preamble.
			// FBO/viewport facts here are draw-time (scene FBO + SceneGEqual depth
			// state established by terrain/static-prop passes above).
			render_contract::noteRenderPass(render_contract::PassIdentity::OpaqueObject,
			                               "GpuMechBatcher_flush(submit)");
			// APPLY-STATE-MECHOPAQUE-1: when MC2_FRAMEGRAPH_EXECUTOR is ON and a
			// kTopLevelStateDesc row exists, the executor pre-applies the MechOpaque
			// pipeline here (at the real flush call site, NOT the begin seam which wraps
			// non-mech draws too); flush()'s body then skips its own applyPipeline.
			// Gate OFF -> not called -> body applies -> byte-identical.
			if (render_contract::isTopLevelExecutorEnabled() &&
			    RenderCore::framegraph::findTopLevelStateDesc(RenderCore::RenderPassId::MechOpaque) != nullptr) {
				GpuMechBatcher::instance().executorApplyMechOpaqueState();
			}
			// VK-P14 MECH TIER-1 (gate MC2_VK_MECH_TIER1, requires MC2_VK_SHARED_SCENE_DEPTH,
			// default-OFF): flush() still BUILDS the instance/bone SSBOs + feed snapshot but
			// SKIPS its GL color draws when the gate is on; the island then co-writes the
			// skinned mechs IN-PLACE into the shared GBuffer. Fail-soft (island 0) = mechs
			// dropped this frame (GL draws already skipped), no corruption. Gate OFF ->
			// enabled()==0 -> flush() draws GL as before (byte-identical).
			GpuMechBatcher::instance().flush();
#ifdef MC2_VULKAN_ISLAND
			if (mc2_vk_mech_island_enabled()) {
				ZoneScopedN("Render.GpuMechs.VkTier1");
				if (gosPostProcess* ppVkMech = getGosPostProcess()) {
					(void)mc2_vk_mech_island_run(ppVkMech->getWidth(), ppVkMech->getHeight());
				}
			}
#endif
			gos_render_pass_timer::End(gos_render_pass_timer::Pass_Mechs);
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

	// B4 Slice Stage 1b — mask-SOLID dual-run dispatch.
	// Draws the same SOLID quads as the legacy drawPass (which is still active
	// in Stage 1b — both run; parity comparator validates the masks match).
	// Default-off: IsFrameMaskSolidArmed() returns false unless
	// MC2_TERRAIN_MASK_DISPATCH=1 AND MC2_TERRAIN_MASK_DISPATCH_SOLID != "0".
	// Pass_Overlays = one coarse scope spanning Render.TerrainMask.Solid
	// through Render.Decals (sub-zones stay Tracy-only).
	// [RENDERLISTS_COST v1] manual span: the overlay region is a run of sibling
	// blocks (mask-solid .. decals), not one lexical scope.
	const unsigned long long _rlOv0 = rlcost::s_enabled ? rlcost::nowNs() : 0ULL;
	gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_Overlays);
	{
		ZoneScopedN("Render.TerrainMask.Solid"); GosGlLostDrawsScope _lost_("overlays.mask_solid");
		TracyGpuZone("Render.TerrainMask.Solid");
		if (gos_terrain_mask_dispatch::IsMaskDispatchReady()
		 && gos_terrain_mask_dispatch::IsFrameMaskSolidArmed()) {
			gos_terrain_mask_dispatch::DrawMaskSolid();
		}
	}

	// ── New world-space overlay batches ──────────────────────────────────────
	// These draw calls flush batches accumulated during land->render() and
	// craterManager->render().  They set their own GL state and restore it.
	// Render order matches design: terrain → cement overlays → decals → (old overlays).
	{
		ZoneScopedN("Render.TerrainOverlays"); GosGlLostDrawsScope _lost_("overlays.terrain_overlays");
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
		ZoneScopedN("Render.TerrainOverlaysStatic"); GosGlLostDrawsScope _lost_("overlays.terrain_overlays_static");
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
		ZoneScopedN("Render.TerrainMines"); GosGlLostDrawsScope _lost_("overlays.terrain_mines");
		TracyGpuZone("Render.TerrainMines");
		if (gos_terrain_indirect::IsFrameMineArmed()) {
			gos_terrain_indirect::DrawMineStatic();
		}
	}
	{
		ZoneScopedN("Render.Decals"); GosGlLostDrawsScope _lost_("overlays.decals");
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
	gos_render_pass_timer::End(gos_render_pass_timer::Pass_Overlays);
	if (rlcost::s_enabled) rlcost::s_ns[rlcost::kOverlays] += rlcost::nowNs() - _rlOv0;
	// ── End new world-space overlay batches ───────────────────────────────────
}
