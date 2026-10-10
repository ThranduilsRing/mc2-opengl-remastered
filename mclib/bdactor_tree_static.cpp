// BldgAppearance / TreeAppearance: TreeAppearance static registry: touch*, registerStatic, prewarm.
// Declared in the class headers; shared names in bdactor_internal.h.
#include "bdactor_internal.h"


void TreeAppearance::touch()
{
	// FRAME-JOBS-2F: if touchSerialCommit() already ran this frame (split path active),
	// skip redundant Path B work. See BldgAppearance::touch for full contract.
	if (mc2FrameJobsTouchDiagEnabled()) {
		touchEntryDiagRollFrame(g_mc2FrameCounter);
		if (touchSerialCommitFrame == g_mc2FrameCounter) {
			++s_touchEntryDiag.legacy_skipped;
			return;
		}
		++s_touchEntryDiag.legacy_ran_nosplit;
	} else {
		if (touchSerialCommitFrame == g_mc2FrameCounter)
			return;
	}

	selectActiveLOD();  // re-evaluate LOD every frame on the skip path
	// Stage 3.C: called by the outer-skip gate instead of update() when this
	// tree is registered and stable. Re-submits the cached lightData_ (set
	// during the last update() call) to get a fresh UBO slot index for this
	// frame — no s_listOfLights dependency, no terrain lookup needed.
	// Touch() advances lastTurnTransformed so TG_Shape::Render()'s staleness
	// guard doesn't suppress the legacy fallback path.
	if (treeShape) {
		// STABLE-LIGHT-SKIP-BROADEN-1: broaden the LBSS resubmit-skip that
		// TreeAppearance::touchSerialCommit() takes (bdactor.cpp ~6709) to this
		// legacy touch() path (the live path in stock config — FRAME-JOBS touch
		// split default-OFF). Eligibility mirrors the serial-commit tree criteria
		// exactly: registered + valid recipeIndex + valid lightDataIndex +
		// !needsFullBakeNextFrame (trees don't track lastLightEnvGen — mission
		// lighting is effectively static for trees). When eligible the cached
		// lightDataIndex is still valid on the GPU (same baked-prefix persistence
		// invariant), so the resubmit below is redundant. Still call treeShape->
		// Touch() (advance lastTurnTransformed) — skip only the expensive resubmit.
		// Gate: mc2StableLightSkipTouchEnabled() (default OFF => resubmit still runs).
		{
			const bool treeRegistered = (staticReg[activeLOD].registered && staticReg[activeLOD].recipeIndex >= 0);
			const bool treeIdxValid   = (staticReg[activeLOD].lightDataIndex != 0xFFFFFFFFu);
			const bool treeEligible   = treeRegistered && treeIdxValid && !needsFullBakeNextFrame;
			if (mc2StableLightSkipTouchEnabled() && mc2LightbridgeStableSkipEnabled() && treeEligible) {
				lbssRecordTree(/*skipTaken=*/true, /*blk_idx=*/false);
				treeShape->Touch();
				return;
			}
		}
		// [LIGHTBRIDGE v1] C6 retirement: repoint to the primed 38d8720 slot
		// (zero FNV/memcmp; cachedFrame_ stamped). MISS keeps the legacy
		// resubmit (NOT CacheGpuLightData -- terrain-color-staleness,
		// msl.cpp:1874-1887). MC2_LIGHTBAKE=0 -> legacy path bit-for-bit.
		extern bool mc2LightBakeEnabled();
		extern bool mc2IsBakedStaticLightPresent(int32_t);
		// [LIGHTBRIDGE-BAKED-PROBE-1] probe only — EmitBakedGpuLightData discards baked
		if (mc2LightBakeEnabled()
		    && staticReg[activeLOD].registered && staticReg[activeLOD].recipeIndex >= 0
		    && mc2IsBakedStaticLightPresent(staticReg[activeLOD].recipeIndex)) {
			TG_HWLightsData baked{};
			treeShape->EmitBakedGpuLightData(staticReg[activeLOD].recipeIndex, baked);
		} else {
			treeShape->ResubmitCachedGpuLightData();
		}
		// 2026-05-11 per-instance capture: see BldgAppearance::touch.
		staticReg[activeLOD].lightDataIndex = treeShape->getCachedGpuLightIndex();
		treeShape->Touch();
	}
}

// FRAME-JOBS-2D: lock-free per-instance prep; runs on worker threads.
// selectActiveLOD() reads per-instance state only (no shared mutation).
// treeShape->Touch() is lock-free (verified: sets lastTurnTransformed only, tgl.cpp:4073).
void TreeAppearance::touchWorkerPrepass()
{
	selectActiveLOD();
	if (treeShape)
		treeShape->Touch();
}

// FRAME-JOBS-2D: light-data resubmit; runs serially on main thread after worker join.
void TreeAppearance::touchSerialCommit()
{
	// FRAME-JOBS-2F: stamp this frame unconditionally so touch() (Path B, terrain object
	// loop) returns immediately. Set BEFORE any early returns including stable-skip.
	touchSerialCommitFrame = g_mc2FrameCounter;
	if (mc2FrameJobsTouchDiagEnabled()) {
		touchEntryDiagRollFrame(g_mc2FrameCounter);
		++s_touchEntryDiag.serial_commit_hits;
	}

	if (!treeShape) {
		if (s_proxyReconEnabled) {
			g_spr_phase2Calls.fetch_add(1, std::memory_order_relaxed);
			g_spr_treeCalls.fetch_add(1, std::memory_order_relaxed);
			g_spr_rejNoShape.fetch_add(1, std::memory_order_relaxed);
			g_spr_callCounter.fetch_add(1, std::memory_order_relaxed);
		}
		return;
	}

	// FRAME-JOBS-2F Step 5: needsFullBakeNextFrame guard — mirrors BldgAppearance's
	// stableLightSkipEligible which includes !needsFullBakeNextFrame. In practice,
	// objects with needsFullBakeNextFrame=true do not reach touchSerialCommit via the
	// split path (IsStaticNow() returns false), but add explicit guard for safety.
	// When true, fall through to full commit (do not apply stable-skip).
	const bool forceFullCommit = needsFullBakeNextFrame;

	// STATIC-SCENE-PROXY-RECON-1: classify tree as proxy candidate or not.
	// Must run BEFORE the LBSS early return below (most trees skip via LBSS).
	// Trees don't track lastLightEnvGen (mission lighting is static for trees),
	// so the criteria are: registered + valid recipeIndex + valid lightDataIndex
	// + !needsFullBakeNextFrame. Mirrors the tree stable-skip check exactly.
	if (s_proxyReconEnabled) {
		g_spr_phase2Calls.fetch_add(1, std::memory_order_relaxed);
		g_spr_treeCalls.fetch_add(1, std::memory_order_relaxed);

		const bool treeRegistered = (staticReg[activeLOD].registered && staticReg[activeLOD].recipeIndex >= 0);
		const bool treeIdxValid   = (staticReg[activeLOD].lightDataIndex != 0xFFFFFFFFu);
		const bool isProxy = treeRegistered && treeIdxValid && !forceFullCommit;
		if (!isProxy) {
			if (!treeRegistered)
				g_spr_rejNoStaticReg.fetch_add(1, std::memory_order_relaxed);
			else if (!treeIdxValid)
				g_spr_rejBadLightIdx.fetch_add(1, std::memory_order_relaxed);
			else if (forceFullCommit)
				g_spr_rejNeedsFullBake.fetch_add(1, std::memory_order_relaxed);
		}
		if (isProxy) g_spr_proxyCandidate.fetch_add(1, std::memory_order_relaxed);
		// Tree path also bumps the shared call counter so the print trigger fires
		// on tree-heavy missions where bldg count alone wouldn't reach the modulo.
		g_spr_callCounter.fetch_add(1, std::memory_order_relaxed);
	}

	// [LIGHTBRIDGE-STABLE-SKIP-WIRE-1] Tree path: if MC2_LIGHTBRIDGE_STABLE_SKIP=1 and
	// the active LOD's baked light slot is valid, skip EmitBakedGpuLightData entirely.
	// INVARIANT: same as BldgAppearance — lightData_[recipeIndex] persists frame-to-frame;
	// staticReg[activeLOD].lightDataIndex == recipeIndex from prior frame's emit is valid.
	// Trees don't track lastLightEnvGen (mission lighting is effectively static for trees).
	if (mc2LightbridgeStableSkipEnabled() && !forceFullCommit) {
		const bool indexValid = (staticReg[activeLOD].lightDataIndex != 0xFFFFFFFFu);
		const bool registered = (staticReg[activeLOD].registered && staticReg[activeLOD].recipeIndex >= 0);
		const bool skipTaken  = (registered && indexValid);
		lbssRecordTree(skipTaken, registered && !indexValid);
		if (skipTaken)
			return;
	}

	extern bool mc2LightBakeEnabled();
	extern bool mc2IsBakedStaticLightPresent(int32_t);
	// [LIGHTBRIDGE-BAKED-PROBE-1] probe only — EmitBakedGpuLightData discards baked
	if (mc2LightBakeEnabled()
	    && staticReg[activeLOD].registered && staticReg[activeLOD].recipeIndex >= 0
	    && mc2IsBakedStaticLightPresent(staticReg[activeLOD].recipeIndex)) {
		TG_HWLightsData baked{};
		treeShape->EmitBakedGpuLightData(staticReg[activeLOD].recipeIndex, baked);
	} else {
		treeShape->ResubmitCachedGpuLightData();
	}
	staticReg[activeLOD].lightDataIndex = treeShape->getCachedGpuLightIndex();
	// NOTE: treeShape->Touch() is in touchWorkerPrepass() (verified lock-free)
}

void TreeAppearance::invalidateStaticRegistration()
{
	// TREE-OVERRIDE-LOD-MVP Task 2: tear down every registered LOD recipe.
	for (long lod = 0; lod < MAX_LODS; ++lod) {
		if (staticReg[lod].registered && staticReg[lod].recipeIndex >= 0)
			GameAdapters::StaticProp::destroyStaticPropByIndex(staticReg[lod].recipeIndex);
		staticReg[lod] = StaticRegistration{};
	}
}

// Task 5 (Track B): mission-load bulk static-prop registration (mirror of
// BldgAppearance::registerStatic). HC-1: writes directly to typed staticReg.
void TreeAppearance::registerStatic() {
	const bool seamProbe = (mc2::gate::get<"MC2_MODOVERRIDE_TRACE">())
		&& appearType && appearType->treeRenderShape[0];
	if (seamProbe)
		fprintf(stderr, "[SEAMPROBE] tree registerStatic ENTER name=%s registered=%d hasShape=%d enabled=%d renderShape=%p\n",
			appearType->name, (int)staticReg[0].registered, (int)(treeShape!=NULL),
			(int)GpuStaticPropRegistry::isEnabled(), (void*)appearType->treeRenderShape[0]), fflush(stderr);
	// TREE-OVERRIDE-LOD-MVP Task 2: registered-state is now per-LOD. LOD0 is
	// the canonical "already done" gate (it always exists — override or stock).
	if (staticReg[0].registered) return;
	if (!treeShape)              return;
	if (!GpuStaticPropRegistry::isEnabled()) return;

	// MODEL-OVERRIDE: register render-shape geometry before finalizeGeometry
	// (see BldgAppearance::registerStatic for full rationale — late init() reg
	// otherwise drops ~1k override trees to the CPU first-render path).
	// TREE-OVERRIDE-LOD-MVP M-C: for override types iterate ONLY populated LODs
	// (each distinct override LOD shape uploaded once); stock types keep the
	// full MAX_LODS loop (distinct stock treeShape[i] per LOD).
	const bool _treeRegIsOverride = (appearType->treeRenderShapeLodCount > 0);
	const int  _treeRegLods = _treeRegIsOverride
		? (int)appearType->treeRenderShapeLodCount : MAX_LODS;
	for (int i = 0; i < _treeRegLods; ++i)
		GpuStaticPropBatcher::instance().registerMultiShape(appearType->getTreeRenderShape(i), _treeRegIsOverride);

	if (mc2::gate::get<"MC2_MODOVERRIDE_TRACE">() && appearType->treeRenderShape[0]) {
		TG_TypeMultiShape* rs = appearType->treeRenderShape[0];
		fprintf(stderr, "[MODOVERRIDE_TRACE] tree registerStatic name=%s renderShape=%p numShapes=%ld leaf0=%p leaf1=%p lodCount=%ld\n",
		        appearType->name, (void*)rs, rs->GetNumShapes(),
		        rs->GetNumShapes() > 0 ? (void*)rs->GetTypeNode(0) : nullptr,
		        rs->GetNumShapes() > 1 ? (void*)rs->GetTypeNode(1) : nullptr,
		        appearType->treeRenderShapeLodCount);
		fflush(stderr);
	}

	// Compute transform — same coordinate convention as TreeAppearance::update().
	// yaw includes the per-instance yaw offset (matches first-render path exactly).
	float yawAngle = (rotation * DEGREES_TO_RADS) + (yaw * DEGREES_TO_RADS);
	float pitchAngle = (pitch * DEGREES_TO_RADS);
	Stuff::UnitQuaternion rot;
	rot = Stuff::EulerAngles(pitchAngle, yawAngle, 0.0f);
	Stuff::Point3D xlatPosition;
	xlatPosition.x = -position.x;
	xlatPosition.y = TerrainRuntime::groundElevation(position);
	xlatPosition.z = position.y;

	// TREE-OVERRIDE-LOD-MVP M-A (load-bearing): build the recipe AND capture the
	// light slot per LOD FROM getTreeRenderShape(lod)'s OWN geometry. If we only
	// swapped recipeIndex while the recipe geometry stayed LOD0, per-LOD
	// selection (Task 5) would draw the SAME mesh → no perf win, and the
	// IsStaticNow shape-key would break. So each populated LOD gets its own
	// transformed render-instance, its own buildRecipeFromShape pass, its own
	// syncStaticProp recipe index, and (via the post-register bake re-arm) its
	// own permanent baked light slot.
	//
	// LOD0 reuses the existing per-instance `treeShape` (which is
	// CreateFrom(getTreeRenderShape(0))) so the LOD0 recipe + the IsStaticNow
	// `staticReg[0].shape == treeShape` key are byte-identical to pre-LOD
	// behavior. Higher LODs build from a throwaway CreateFrom(getTreeRenderShape
	// (lod)) instance that is freed before return — its geometry already lives in
	// the immutable per-type VBO (registered above), and only its recipe/light
	// slot are persisted in staticReg[lod]. treeShape stays collision/bounds +
	// LOD0 render; collision reads stock treeShape[] (untouched).
	//
	// M4: a buildRecipe MISS for a given LOD marks ONLY that LOD unavailable and
	// CONTINUES — never aborts the whole instance or half-populates. LOD0 MISS
	// still leaves staticReg[0].registered=false (first-render fallback covers
	// it, exactly as the single-LOD abort did before).
	bool anyRegistered = false;
	const int lodCount = _treeRegIsOverride ? (int)appearType->treeRenderShapeLodCount : 1;
	for (int lod = 0; lod < lodCount && lod < MAX_LODS; ++lod) {
		// Render-instance for THIS LOD: reuse treeShape for LOD0; temp for >0.
		TG_MultiShape* lodShape = (lod == 0)
			? treeShape
			: appearType->getTreeRenderShape(lod)->CreateFrom();
		if (!lodShape) { staticReg[lod].registered = false; continue; }

		lodShape->TransformMultiShape_BuildRecipe(&xlatPosition, &rot);

		std::vector<GpuStaticPropInstance> batch;
		const int numShapes = static_cast<int>(lodShape->GetNumShapes());
		batch.reserve(numShapes);
		int t_diag_total=0,t_diag_skip_pm=0,t_diag_skip_h=0,t_diag_skip_unreg=0,t_diag_added=0;
		bool lodMiss = false;
		for (int i = 0; i < numShapes; ++i) {
			++t_diag_total;
			const TG_ShapeRec* rec = lodShape->GetShapeRec(i);
			if (!rec || !rec->processMe || !rec->node) { ++t_diag_skip_pm; continue; }
			TG_Shape* child = rec->node;
			// 2026-05-10 fix: skip non-SHAPE_NODE helpers (mirror of submitMultiShape's
			// filter) — see BldgAppearance::registerStatic for full rationale.
			if (!child->IsShapeNode()) { ++t_diag_skip_h; continue; }
			// GHOST-CONE-RECIPE-SKIP-1: see BldgAppearance::registerStatic —
			// same skip for the tree/LOD recipe loop (lockstep).
			{
				static const bool s_recipeSpotSkip2 = mc2::gate::get<"MC2_STATICPROP_RECIPE_SPOTLIGHT_SKIP">();
				if (s_recipeSpotSkip2 && child->GetIsSpotlight()) { ++t_diag_skip_h; continue; }
			}
			uint32_t flags = 0;
			if (child->GetLightsOut())   flags |= (1u << 0);
			if (child->GetIsWindow())    flags |= (1u << 1);
			if (propFoliageAlphaFlag() && child->GetIsAlphaTest()) flags |= (1u << 2);
			Stuff::Matrix4D xform(rec->shapeToWorld);
			GpuStaticPropInstance inst;
			if (!GpuStaticPropBatcher::instance().buildRecipeFromShape(
					child, xform,
					static_cast<uint32_t>(child->GetARGBHighlight()),
					static_cast<uint32_t>(child->GetFogRGB()),
					flags, &inst)) {
				++t_diag_skip_unreg;
				if (seamProbe)
					fprintf(stderr, "[SEAMPROBE] tree buildRecipe MISS name=%s lod=%d leaf=%d child=%p node=%s -> LOD unavailable\n",
						appearType->name, lod, i, (void*)child, child->getNodeName()), fflush(stderr);
				// M4: mark THIS LOD unavailable and stop building it; do NOT
				// abort the whole instance — other LODs still register.
				lodMiss = true;
				break;
			}
			if (seamProbe)
				fprintf(stderr, "[SEAMPROBE] tree buildRecipe HIT name=%s lod=%d leaf=%d child=%p node=%s typeID=%u\n",
					appearType->name, lod, i, (void*)child, child->getNodeName(), inst.typeID), fflush(stderr);
			// EDITOR-STATIC-RECIPE-FROM-TYPE-1 (mirror of BldgAppearance::registerStatic):
			// re-point the recipe at the canonical primed TYPE leaf
			// (getTreeRenderShape(lod), which carries finalized VBO geometry) instead of
			// the CreateFrom instance copy's leaf, which late-registers after the editor
			// finalize latch -> typeID with no geometry -> invisible. Editor-only no-op
			// in-game. See the building site for full rationale.
			{
				extern bool InEditor;   // mech3d.cpp
				if (InEditor && appearType) {
					TG_TypeMultiShape* typeRS = appearType->getTreeRenderShape(lod);
					if (typeRS && i < typeRS->GetNumShapes()) {
						TG_TypeNodePtr typeLeaf = typeRS->GetTypeNode(i);
						uint32_t canonId = 0, idxc = 0, fi = 0, ic = 0; int32_t bv = 0;
						if (typeLeaf
						    && batcher_typeIdForTypeShape((const void*)typeLeaf, &canonId)
						    && batcher_getTypeDrawInfo(canonId, &idxc, &fi, &bv, &ic)
						    && idxc > 0) {
							inst.typeID = canonId;
						}
					}
				}
			}
			batch.push_back(inst);
			++t_diag_added;
		}
		{
			static const bool s_trace = mc2::gate::get<"MC2_TREE_REG_TRACE">();
			static int s_treeRegLogged = 0;
			if (s_trace && s_treeRegLogged < 80) {
				++s_treeRegLogged;
				fprintf(stderr,
					"[TREE_REG_DIAG v1] sample=first80 appearType=%s lod=%d numShapes=%d total=%d pm_skip=%d "
					"h_skip=%d unreg_skip=%d added=%d\n",
					(appearType ? appearType->name : "<null>"), lod,
					numShapes, t_diag_total, t_diag_skip_pm, t_diag_skip_h,
					t_diag_skip_unreg, t_diag_added);
				fflush(stderr);
			}
			(void)t_diag_total; (void)t_diag_skip_pm; (void)t_diag_skip_h;
			(void)t_diag_skip_unreg; (void)t_diag_added;
		}

		int32_t regIdx = -1;
		if (!lodMiss && !batch.empty()) {
			// TREE-OVERRIDE-LOD-MVP forced-LOD FIX (2026-06-03): pass treeShape
			// (the ALWAYS-LIVE LOD0 per-instance shape) as the recipe `multi`, NOT
			// the throwaway lodShape — lodShape is deleted before the next frame, so
			// rng.multi would dangle and flush()'s stale-frame gate
			// (rng.multi->getCachedFrame() != currentFrame) would fire every frame →
			// recipe skipped → LOD1 instances drawn=0 → trees invisible. The DRAWN
			// geometry stays LOD1: it is carried by the batch entries' typeIDs (built
			// from lodShape's leaves via buildRecipeFromShape above, pointing at LOD1's
			// VBO region), not by `multi`. `multi` is used only for the frame-stamp
			// gate, the per-frame lightDataIndex fallback (shared LOD0 slot — correct),
			// and texture pins (LOD1 shares LOD0's texture set). treeShape's frame
			// stamp is refreshed every frame by touch()/update(), keeping the LOD1
			// recipe live exactly like LOD0.
			(void)GameAdapters::StaticProp::syncStaticProp(
				treeShape, batch.data(), batch.size(), &regIdx);
			if (seamProbe)
				fprintf(stderr, "[SEAMPROBE] tree syncStaticProp name=%s lod=%d batchSize=%zu regIdx=%d firstTypeID=%u\n",
					appearType->name, lod, batch.size(), (int)regIdx,
					batch.empty()?0xFFFFFFFFu:batch[0].typeID), fflush(stderr);
		}
		if (regIdx >= 0) {
			staticReg[lod].registered  = true;
			// shape key: LOD0 keys on treeShape (the live render-instance);
			// the IsStaticNow check is LOD0-only (see IsStaticNow). Higher LODs
			// have no live per-instance shape, so we record the type's render
			// shape for diagnostics only — never compared at draw time.
			// TREE-OVERRIDE-LOD-MVP K×M/forced-LOD FIX (2026-06-03): store nullptr for
			// lod>=1, NOT the throwaway lodShape (freed below). A dangling .shape made
			// IsStaticNow()'s shape-key fail for activeLOD>=1 → recovery path re-registered
			// treeShape (LOD0) into staticReg[lod] → forced LOD drew LOD0 geometry (STOP).
			// Higher-LOD .shape is never compared at draw time (IsStaticNow bypasses the
			// key when activeLOD!=0; recovery check is activeLOD==0-only).
			staticReg[lod].shape       = (lod == 0) ? treeShape : nullptr;
			staticReg[lod].recipeIndex = regIdx;
			anyRegistered = true;

			// COUNTER-HONESTY-1: tag the recipe Tree. Until this lane, ONLY the
			// two BldgAppearance sites called setRecipePopulation(), so every tree
			// recipe kept the 0xFF "unset" default and the registry's population
			// breakdown was structurally building-only — an instrument that could
			// only ever report the one value it was wired for.
			// Behaviour-neutral for both existing consumers: getBuildingShadowInstances
			// keeps `population != Building` (Tree and unset are both excluded), and
			// getDynamicPropShadowInstances keeps `population == Building` (Tree and
			// unset are both admitted). The tag makes the census honest; it does not
			// move a single shadow caster.
			GpuStaticPropRegistry::setRecipePopulation(
				regIdx, GpuStaticPropPopulation::Tree);

			// TREE-OVERRIDE-LOD-MVP Task 3 (K×M GATE): bake the higher-LOD light
			// slot HERE, at registration, from this LOD's OWN transformed render-
			// instance. LOD0's light is baked by the next update() (activeLOD=0,
			// byte-identical to pre-LOD), but a higher LOD never becomes active
			// while activeLOD is pinned 0 (Task 5 adds distance selection), so its
			// light would otherwise never gather — and the K×M gate could not see
			// whether LOD1's per-instance bake creates a SECOND lightData_ slot or
			// dedups to LOD0's. mc2CacheOrBakeStaticGpuLight walks the hierarchy +
			// GatherGpuObjectLightDataOnly (position-keyed) and routes the content
			// through addLightDataStructure's FNV+memcmp dedup. Same world position
			// as LOD0 ⇒ byte-identical TG_HWLightsData ⇒ dedup HIT ⇒ shares LOD0's
			// lightData_ slot (the architectural PASS basis). It DOES consume a
			// distinct recipe-keyed baked slot (B grows with recipes, bounded by
			// types×LODs — expected, not instance growth). LOD0 skipped here to
			// keep the LOD0-only baseline bit-identical.
			// TREE-OVERRIDE-LOD-MVP K×M FIX (2026-06-03): do NOT bake/gather a separate
			// light slot per LOD. Higher LODs SHARE the LOD0 actor light slot -- same
			// world position => identical terrain lighting, and the gather is position-
			// keyed not geometry-keyed, so a per-LOD bake here only risked a fresh
			// per-instance slot (the K×M STOP) for zero lighting gain. update() resolves
			// staticReg[0].lightDataIndex each frame and shares it to the active LOD;
			// seed the share here so a higher LOD that becomes active before the next
			// update() still points at LOD0's slot.
			if (lod != 0) {
				staticReg[lod].lightDataIndex = staticReg[0].lightDataIndex;
				// K×M GATE (now PASS by construction): record the SHARED LOD0 slot, not a
				// per-LOD bake. The real proof is recordInstance()'s U counter staying flat
				// across LOD count.
				mc2_lightslot_trace::recordLodBakeSlot(staticReg[0].lightDataIndex);
				// Far/impostor LODs are excluded from the dynamic-prop shadow replay. The original
				// rationale (the shadow pass has no alpha discard) no longer holds, but whether
				// letting them cast is worth the larger caster set is undemonstrated; see
				// .claude/SHADOW-DECISIONS-1-STATUS.md.
				GpuStaticPropRegistry::setRecipeNoShadow(regIdx, true);
			}
		} else {
			// M4: LOD unavailable — leave registered=false, continue.
			staticReg[lod].registered  = false;
		}

		// Free the throwaway higher-LOD render-instance (LOD0 == treeShape, kept).
		if (lod != 0 && lodShape) {
			lodShape->ClearAnimation();
			delete lodShape;
		}
	}

	if (anyRegistered) {
		// H4 fix (2026-05-06): see BldgAppearance::registerStatic for full
		// rationale. registerStatic only ran TransformMultiShape_BuildRecipe
		// (positions only); leaf TG_Shape::lightData_ is still default/zero.
		// needsFullBakeNextFrame = true forces the first post-registration
		// frame through full update() (populating lightData_) so every
		// registered LOD recipe gets its permanent baked light slot before the
		// first static replay; subsequent frames proceed via UPDATE_SKIP /
		// static-replay with valid cached data.
		// Spec: docs/superpowers/specs/2026-05-06-update-skip-touch-residual-debug-strategy.md
		needsFullBakeNextFrame = true;
		// [G1-STATIC-EAGER-LIGHT v1] Eager fallback: mirror of BldgAppearance::registerStatic.
		// See that site for full rationale. Gate default-OFF; unset = byte-identical.
		{
			extern void mc2SetBakedStaticLight(int32_t, const TG_HWLightsData&);
			extern void mc2WriteStaticLightSlot(int32_t, const TG_HWLightsData&);
			static const bool s_eagerLightBake = mc2::gate::get<"MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE">();
			if (s_eagerLightBake) {
				TG_HWLightsData fallback;
				fallback.numLights_ = 1;
				fallback.lightColor[0][0] = 0.3f;
				fallback.lightColor[0][1] = 0.3f;
				fallback.lightColor[0][2] = 0.3f;
				fallback.lightColor[0][3] = 1.0f;
				// MERGE FIX (cook tree-LOD x gpucull G1): gpucull used a single regIdx; the cook
				// branch rewrote this fn for per-LOD recipes (staticReg[lod]). Eager-bake all.
				for (int _lod = 0; _lod < MAX_LODS; ++_lod) {
					if (staticReg[_lod].registered && staticReg[_lod].recipeIndex >= 0) {
						mc2SetBakedStaticLight(staticReg[_lod].recipeIndex, fallback);
						mc2WriteStaticLightSlot(staticReg[_lod].recipeIndex, fallback);
					}
				}
			}
		}
	}
}

// TREE-OVERRIDE-LOD-MVP Task 2: report the active LOD's registration (pinned 0).
bool TreeAppearance::isStaticRegistered() const { return staticReg[activeLOD].registered; }

int32_t TreeAppearance::getStaticRecipeIndex() const {
    return staticReg[activeLOD].registered ? staticReg[activeLOD].recipeIndex : -1;
}

// STATIC-REG-PREWARM-QUEUE-1: mission-load off-screen light bake for trees.
// Mirrors BldgAppearance::prewarmStaticLightBake — see that function for rationale.
// Trees bake LOD0 only (activeLOD is pinned 0 at mission load; Task 5 adds distance
// selection later). LOD0 is the canonical light key; other LODs share the slot.
bool TreeAppearance::prewarmStaticLightBake(Camera* cam)
{
	const int lod = 0;  // activeLOD is always 0 at mission-load prewarm time
	if (!staticReg[lod].registered || staticReg[lod].recipeIndex < 0)
		return false;
	if (!needsFullBakeNextFrame)
		return false;
	if (!treeShape)
		return false;
	if (!cam || cam->getNumLights() == 0)
		return false;

	treeShape->SetLightList(cam->getWorldLights(), cam->getNumLights());

	// xlatPosition/rot: same transform as TreeAppearance::update (line ~6129-6132).
	// Trees use pitch + yaw combined; position.y is world-Y but terrain height
	// is overridden in touchSerialCommit (registerStatic path uses position directly).
	Stuff::Point3D xlatPosition;
	xlatPosition.x = -position.x;
	xlatPosition.y =  position.z;
	xlatPosition.z =  position.y;

	float yawAngle   = (rotation * DEGREES_TO_RADS) + (yaw * DEGREES_TO_RADS);
	float pitchAngle = (pitch    * DEGREES_TO_RADS);
	Stuff::UnitQuaternion rot;
	rot = Stuff::EulerAngles(pitchAngle, yawAngle, 0.0f);

	treeShape->TransformMultiShape_HierarchyOnly(&xlatPosition, &rot);

	// Bake into permanent SSBO slot for LOD0.
	mc2CacheOrBakeStaticGpuLight(treeShape, staticReg[lod].registered, staticReg[lod].recipeIndex);
	const uint32_t lightIdx = treeShape->getCachedGpuLightIndex();
	// Share light slot across all LODs (matches update() K×M fix).
	for (int l = 0; l < MAX_LODS; ++l)
		staticReg[l].lightDataIndex = lightIdx;

	if (lightIdx != 0xFFFFFFFFu) {
		needsFullBakeNextFrame = false;
		return true;
	}
	return false;
}
