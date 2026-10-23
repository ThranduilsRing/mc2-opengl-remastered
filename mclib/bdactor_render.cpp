// BldgAppearance / TreeAppearance: BldgAppearance and TreeAppearance render and renderShadows.
// Declared in the class headers; shared names in bdactor_internal.h.
#include "bdactor_internal.h"
#include "../RenderCore/axis_convert.h"
#include "mc2fmt.h"
#include "Gates/gate.h"

#include <cstdlib>
#include <string>
#include "../GameOS/gameos/static_caster_set.h"   // shadow-bake caster events

namespace {
// Parity row 28 oracle: a fixed highlight on every building so a frozen-sim capture can compare the GL and VK highlight
// terms (in play only a damage flash lights a building, which a capture cannot time). Capture-only: read solely while the
// capture replay is armed; a malformed value is rejected with a log line and the hook stays off.
uint32_t captureForcedBuildingHighlight()
{
	static const uint32_t s_forced = []() -> uint32_t {
		if (!mc2::gate::isSet<"MC2_VISUAL_BOOKMARK_CAPTURE">()) return 0u;
		const std::string v = mc2::gate::str<"MC2_CAPTURE_FORCE_BUILDING_HIGHLIGHT">();
		if (v.empty()) return 0u;
		// Exactly eight hex digits: no sign, space, 0x prefix, short or overlong value.
		uint32_t argb = 0u;
		bool valid = v.size() == 8u;
		for (size_t i = 0; valid && i < v.size(); ++i) {
			const char c = v[i];
			const int digit = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
			if (digit < 0) valid = false;
			else argb = (argb << 4) | static_cast<uint32_t>(digit);
		}
		if (!valid) {
			mc2::fmt::print_err("[CAPTURE_HIGHLIGHT v1] rejected MC2_CAPTURE_FORCE_BUILDING_HIGHLIGHT=\"{}\" (want exactly 8 hex digits, ARGB)\n", mc2::fmt::safe(v.c_str()));
			return 0u;
		}
		mc2::fmt::print_out("[CAPTURE_HIGHLIGHT v1] forcing building highlight 0x{:08x}\n", mc2::fmt::as_u32(argb));
		return argb;
	}();
	return s_forced;
}
}  // namespace

long BldgAppearance::render (long depthFixup)
{
	if (gos_IsModernShadowPathActive() &&
		(mc2shadow::staticCasterSet().isMoverOwner(static_cast<const Appearance*>(this)) || !isStaticEligible()))
		GpuStaticPropBatcher::instance().captureMoverShadowShape(static_cast<const Appearance*>(this),bldgShape);
	// GPU-batcher path bypasses inView here — the whole point of C2 is
	// letting the GPU clipper decide visibility. The legacy angular-cull
	// recalcBounds has a ~87% false-negative rate at wolfman zoom; under
	// the GPU path we render every actor and trust the GPU.
	// PREVIEW-FIX: the SimpleCamera component/weapon preview (MechLab loadout)
	// owns visibility via the UI; force the render gate on in that context.
	if (inView || g_useGpuStaticProps || g_mechPreviewRenderDepth > 0)
	{
		uint32_t color = SD_BLUE;
		uint32_t highLight = 0x007f7f7f;
		if ((teamId > -1) && (teamId < 8)) {
			static unsigned long highLightTable[3] = {0x00007f00, 0x0000007f, 0x007f0000};
			static uint32_t colorTable[3] = {SB_GREEN | 0xff000000, SB_BLUE | 0xff000000, SB_RED| 0xff000000};
			color = colorTable[homeTeamRelationship];
			highLight = highLightTable[homeTeamRelationship];
		}

		if (selected & DRAW_COLORED)
		{
			bldgShape->SetARGBHighLight(highLight);
		}
		else
		{
			bldgShape->SetARGBHighLight(highlightColor);
		}
		
		if (drawFlash)
		{
			bldgShape->SetARGBHighLight(flashColor);
		}
		if (const uint32_t forced = captureForcedBuildingHighlight())
			bldgShape->SetARGBHighLight(forced);

		// Call Multi-shape render stuff here.
		// Slice 1 path (g_useGpuObjects). No cull bypass; submitMultiShape
		// is per-child Layer-B by construction. Returns false only when
		// EVERY child is ineligible.
		// Caller-side accounting: recordEligibleActor() fires unconditionally
		// when slice 1 reaches this site (so a null shape or skipped submit
		// still counts toward eligible_actors). recordCpuFallback() fires
		// when no submit succeeded.
		bool submittedToGpu = false;
		// PREVIEW-FIX: in the SimpleCamera component/weapon preview, skip the GPU
		// static-prop submit (the batcher flushes with the world snapshot/terrain
		// MVP, not the UI camera) so submittedToGpu stays false and the legacy CPU
		// bldgShape->Render() below runs, honoring this SimpleCamera.
		if (g_useGpuObjects && g_mechPreviewRenderDepth == 0)
		{
			GpuStaticPropBatcher::instance().recordEligibleActor(
				GpuStaticPropPopulation::Building);

			// Stage 3.D: static registry fast path (mirror of TreeAppearance
			// at bdactor.cpp). Set MC2_FORCE_DYNAMIC_BUILDINGS=1 to force
			// fallback to dynamic submitMultiShape for boundary diagnosis.
			// 2026-05-10 diag: per-frame counters to localise buildings-don't-
			// render bug (substrate=ON misses buildings; killswitch shows them).
			static uint64_t s_diag_render_calls = 0;
			static uint64_t s_diag_static_now_true = 0;
			static uint64_t s_diag_lightidx_uintmax = 0;
			static uint64_t s_diag_markVisible = 0;
			static uint64_t s_diag_static_now_false_reg = 0;
			static uint64_t s_diag_static_now_false_eligible = 0;
			static uint64_t s_diag_static_now_false_other = 0;
			static uint64_t s_diag_dyn_submit = 0;
			++s_diag_render_calls;
			const bool isnow = IsStaticNow();
			// [SEAMPROBE] stage 10: per-render admission gate for the override
			// building (hangar). Logs IsStaticNow() components so we can see why
			// markVisible() never fires for the prop while it fires for trees.
			{
				static const bool s_seamRender = mc2::gate::get<"MC2_MODOVERRIDE_TRACE">();
				static int s_seamRenderLogged = 0;
				if (s_seamRender && appearType && appearType->bldgRenderShape
				        && s_seamRenderLogged < 12) {
					++s_seamRenderLogged;
					mc2::fmt::print_err("[SEAMPROBE] sample=first12 bldg render name={} isnow={:d} reg={:d} shapeMatch={:d} needBake={:d} elig={:d} recipeIdx={:d} | spin={:d} drawFlash={:d} destructFX={:d} activity={:d} activity1={:d} animState={:d}\n", mc2::fmt::safe(appearType->name), mc2::fmt::as_i32(static_cast<int>(isnow)), mc2::fmt::as_i32(static_cast<int>(staticReg.registered)), mc2::fmt::as_i32(static_cast<int>((staticReg.shape == bldgShape))), mc2::fmt::as_i32(static_cast<int>(needsFullBakeNextFrame)), mc2::fmt::as_i32(static_cast<int>(isStaticEligible())), mc2::fmt::as_i32(staticReg.recipeIndex), mc2::fmt::as_i32((appearType?appearType->spinMe:-1)), mc2::fmt::as_i32(static_cast<int>((drawFlash!=0))), mc2::fmt::as_i32(static_cast<int>((destructFX!=nullptr))), mc2::fmt::as_i32(static_cast<int>((activity!=nullptr))), mc2::fmt::as_i32(static_cast<int>((activity1!=nullptr))), mc2::fmt::as_i32(static_cast<int>(bdAnimationState)));
					fflush(stderr);
				}
			}
			if (isnow) ++s_diag_static_now_true;
			else {
				if (!staticReg.registered) ++s_diag_static_now_false_reg;
				else if (!isStaticEligible()) ++s_diag_static_now_false_eligible;
				else ++s_diag_static_now_false_other;
			}
			if (isnow) {
				// ── [GUARD_FORWARD v1] ZERO-LIGHT-ROW-1 divergence census ──────
				// Measure, WITHOUT changing behaviour, how often the variable
				// this guard validates (the shared per-multi cache) disagrees
				// with the variable it forwards (the per-actor capture).
				// Default-ON and cheap: two loads + a switch per static building
				// render, printed once every 600 renders. The two mixed states
				// are the whole question -- if both stay 0 across a real run the
				// guard/forward mismatch is LATENT, not live, and no behaviour
				// change is warranted. See mclib/static_light_row_guard.h.
				if (bldgShape) {
					static uint64_t s_gf_total = 0, s_gf_agree = 0, s_gf_differ = 0;
					static uint64_t s_gf_fwdSentinel = 0, s_gf_valSentinel = 0, s_gf_bothSentinel = 0;
					static bool     s_gf_reported = false;
					const static_light_row::Divergence gfd =
						static_light_row::classifyGuardForward(
							bldgShape->getCachedGpuLightIndex(),   // validated
							staticReg.lightDataIndex);            // forwarded
					++s_gf_total;
					switch (gfd) {
						case static_light_row::kAgree:                 ++s_gf_agree;        break;
						case static_light_row::kDifferBothValid:       ++s_gf_differ;       break;
						case static_light_row::kForwardedSentinelOnly: ++s_gf_fwdSentinel;  break;
						case static_light_row::kValidatedSentinelOnly: ++s_gf_valSentinel;  break;
						case static_light_row::kBothSentinel:          ++s_gf_bothSentinel; break;
					}
					// CADENCE: static props render every frame in the thousands, so
					// a small modulus here would emit hundreds of lines a second
					// and drown the log this census exists to be read from.
					// Emit on the FIRST mixed state -- the finding, reported the
					// instant it exists -- and then only rarely.
					const bool gfFirstMixed =
						static_light_row::guardCorrectionWouldChangeBehaviour(gfd)
						&& !s_gf_reported;
					if (gfFirstMixed) s_gf_reported = true;
					if (gfFirstMixed || ((s_gf_total % 200000ull) == 0ull && s_gf_agree != s_gf_total)) {  // 2026-10-05: census only when a divergence exists
						mc2::fmt::print_err("[GUARD_FORWARD v1] event={} class=bldg total={:d} agree={:d} differ_both_valid={:d} forwarded_sentinel_only={:d} validated_sentinel_only={:d} both_sentinel={:d} would_change={:d}\n",
							mc2::fmt::safe(gfFirstMixed ? "first_divergence" : "census"),
							mc2::fmt::as_u64(static_cast<unsigned long long>(s_gf_total)),
							mc2::fmt::as_u64(static_cast<unsigned long long>(s_gf_agree)),
							mc2::fmt::as_u64(static_cast<unsigned long long>(s_gf_differ)),
							mc2::fmt::as_u64(static_cast<unsigned long long>(s_gf_fwdSentinel)),
							mc2::fmt::as_u64(static_cast<unsigned long long>(s_gf_valSentinel)),
							mc2::fmt::as_u64(static_cast<unsigned long long>(s_gf_bothSentinel)),
							mc2::fmt::as_u64(static_cast<unsigned long long>((s_gf_fwdSentinel + s_gf_valSentinel))));
						fflush(stderr);
					}
				}
				static const bool s_forceDynamicBldgs =
				    bdForceDynamicDefault<"MC2_FORCE_DYNAMIC_BUILDINGS">();
				if (s_forceDynamicBldgs) {
					invalidateStaticRegistration();
				} else if (bldgShape && bldgShape->getCachedGpuLightIndex() == UINT32_MAX) {
					// Light gather failed this frame — invalidate so dynamic
					// path re-runs and re-registers next frame.
					// ZERO-LIGHT-ROW-1: NOTE the variable. This tests the shared
					// per-multi cache; the value forwarded ~15 lines below is
					// staticReg.lightDataIndex, the PER-ACTOR capture. The census
					// immediately above measures how often that distinction bites.
					++s_diag_lightidx_uintmax;
					invalidateStaticRegistration();
				} else {
					// 2026-05-11: pass per-actor captured lightDataIndex so
					// flush() can read it (when MC2_STATIC_PER_INSTANCE_LIGHT=1)
					// instead of multi->getCachedGpuLightIndex() — the per-multi
					// scratch slot is last-writer-wins across sibling instances.
					// STATIC-PROP REGISTRATION CONTRACT v1 (see TreeAppearance::render):
					// only suppress legacy when the registry accepted a LIVE recipe; on a
					// dead/tombstoned handle, invalidate so the full-bake path re-registers
					// + draws this frame instead of the prop vanishing.
					const GpuStaticPropRegistry::StaticSubmitResult bldgRes =
						GpuStaticPropRegistry::markVisibleChecked(
							staticReg.recipeIndex,
							staticReg.lightDataIndex,
							bldgShape ? bldgShape->GetExtentRadius() : 0.0f);
					if (bldgRes == GpuStaticPropRegistry::StaticSubmitResult::Submitted) {
						++s_diag_markVisible;
						submittedToGpu = true;
					} else {
						bdLogStaticRegInvalid("bldg", static_cast<int>(bldgRes), staticReg.recipeIndex,
							appearType ? appearType->name : nullptr);
						invalidateStaticRegistration();  // dead handle -> full-bake re-registers below
					}
				}
			}
			static const bool s_bldgDiagTrace = mc2::gate::get<"MC2_BLDG_DIAG_TRACE">();
			if (s_bldgDiagTrace && (s_diag_render_calls % 600) == 0) {
				mc2::fmt::print_err("[BLDG_DIAG v1] event=summary calls={:d} staticNow={:d} notreg={:d} notelig={:d} other={:d} lightidxUM={:d} markVis={:d} dynSubmit={:d}\n", mc2::fmt::as_u64(static_cast<unsigned long long>(s_diag_render_calls)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_diag_static_now_true)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_diag_static_now_false_reg)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_diag_static_now_false_eligible)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_diag_static_now_false_other)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_diag_lightidx_uintmax)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_diag_markVisible)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_diag_dyn_submit)));
				fflush(stderr);
			}
			(void)s_diag_dyn_submit;  // updated below if we go to the dyn path

			if (!submittedToGpu && bldgShape)
			{
				// Stage 3.D: shape-swap invalidation. IsStaticNow's
				// staticReg.shape==bldgShape check routed us here when
				// bldgShape was reassigned (LOD swap, damage→bldgDmgShape),
				// but staticReg.registered=true still blocks the registration
				// block below. Invalidate the stale entry first.
				// PERF DIAGNOSTIC 2026-05-07: see TreeAppearance::render for the
				// per-frame churn analysis. Buildings have damage-state swaps
				// (intact → dmg shape) but typically no LOD swap; this rate
				// should be much lower than the tree counterpart.
				if (staticReg.registered && staticReg.shape != bldgShape) {
					invalidateStaticRegistration();
				}

				// Slice 2 (object-offload) — Stage 2.C+: pass appearType->name
				// as callerName so [OBJBATCHER v1] event=late_register can
				// identify which actor class owns the unregistered type.
				const char* callerName = (appearType ? appearType->name : nullptr);
				submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(
					bldgShape, GpuStaticPropPopulation::Building, callerName);
				if (submittedToGpu) ++s_diag_dyn_submit;
				// Slice 2 (object-offload) — Stage 2.B: late-registration
				// recovery flag. When submitMultiShape failed because a leaf
				// type was unregistered, mark the actor for full-bake on the
				// NEXT update — defensive hygiene that ensures positions-only
				// is never run on this actor before its type registers.
				if (!submittedToGpu &&
				    GpuStaticPropBatcher::instance().wasLastFailureLateRegistration())
				{
					needsFullBakeNextFrame = true;
					invalidateStaticRegistration();  // clear any stale registration
				}

				// Stage 3.D: registration block. On the first successful full-bake
				// submission with no late-reg flag AND with this instance currently
				// static-eligible, snapshot the leaf batch into the registry.
				// Subsequent frames use the static path above.
				if (submittedToGpu && !staticReg.registered
				        && !needsFullBakeNextFrame
				        && isStaticEligible()) {
					const auto& batch =
						GpuStaticPropBatcher::instance().getLastBuiltBatch();
					int32_t legacyIdx = -1;
					(void)GameAdapters::StaticProp::syncStaticProp(
						bldgShape, batch.data(), batch.size(), &legacyIdx);
					staticReg.recipeIndex = legacyIdx;
					mc2shadow::staticCasterSet().ownerRegistered(static_cast<const Appearance*>(this), legacyIdx);
					staticReg.registered  = (staticReg.recipeIndex >= 0);
					staticReg.shape       = bldgShape;
					if (staticReg.registered) {
						GpuStaticPropRegistry::setRecipePopulation(
							staticReg.recipeIndex, GpuStaticPropPopulation::Building);
						// H4 follow-up (2026-05-07): per-frame re-registration
						// after damage/shape swap has the same lightData_ gap as
						// mission-load registerStatic(). Force one full update()
						// so touch() cannot resubmit default-zero lightData_.
						// Spec: docs/superpowers/specs/2026-05-07-lod-swap-static-registry-churn.md
						needsFullBakeNextFrame = true;
					}
				}
			}
			if (!submittedToGpu)
			{
				GpuStaticPropBatcher::instance().recordCpuFallback(
					GpuStaticPropPopulation::Building);
			}
		}
		// Legacy bypass-cull path (g_useGpuStaticProps). Mutually exclusive
		// with slice 1 — gated on !g_useGpuObjects so the two paths cannot
		// coexist. Tagged Legacy so Gate F's fallback-rate is computed only
		// over slice-1 populations. See spec R1.
		if (!submittedToGpu && !g_useGpuObjects && g_useGpuStaticProps && bldgShape)
		{
			submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(
				bldgShape, GpuStaticPropPopulation::Legacy);
		}
		if (!submittedToGpu)
		{
			if (appearType->spinMe)
				bldgShape->Render(false,0.00001f);
			else if (!depthFixup)
				bldgShape->Render();
			else if (depthFixup > 0)
				bldgShape->Render(false,0.9999999f);
			else if (depthFixup < 0)
				bldgShape->Render(false,0.00001f);
		}

		if (selected & DRAW_BARS)
		{
			if (!appearType->spinMe)
			{
				drawBars();

			}
		}

		if ( selected & DRAW_TEXT )
		{
			if (objectNameId != -1)
			{
				char tmpString[255];
				cLoadString(objectNameId, tmpString, 254);

				drawTextHelp(tmpString, color);
			}
		}
		
		if (destructFX && destructFX->IsExecuted())
		{
			gosFX::Effect::DrawInfo drawInfo;
			drawInfo.m_clipper = theClipper;
			
			MidLevelRenderer::MLRState mlrState;
			mlrState.SetDitherOn();
			mlrState.SetTextureCorrectionOn();
			mlrState.SetZBufferCompareOn();
			mlrState.SetZBufferWriteOn();
	
			drawInfo.m_state = mlrState;
			drawInfo.m_clippingFlags = 0x0;

 			glm::mat4 	shapeOrigin;
			glm::mat4	localToWorld;
			glm::vec3			tPosition;
			


			glm::vec3 actualPosition = position;

			mc2ToRender(tPosition, actualPosition);

			float yaw = (180.0f + rotation) * DEGREES_TO_RADS;
			glm::quat rot;
			rot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));
 			mc2::math::buildRotation(shapeOrigin, rot);
			mc2::math::buildTranslation(shapeOrigin, tPosition);
			
			drawInfo.m_parentToWorld = &shapeOrigin;
			
			if (!MLRVertexLimitReached)
				destructFX->Draw(&drawInfo);
		}
		
		if (isActivitying)
		{
			gosFX::Effect::DrawInfo drawInfo;
			drawInfo.m_clipper = theClipper;

			MidLevelRenderer::MLRState mlrState;
			mlrState.SetDitherOn();
			mlrState.SetTextureCorrectionOn();
			mlrState.SetZBufferCompareOn();
			mlrState.SetZBufferWriteOn();
	
			drawInfo.m_state = mlrState;
			drawInfo.m_clippingFlags = 0x0;

			glm::mat4 	shapeOrigin;
			glm::mat4	localToWorld;
			glm::mat4	localResult;

			if (activityNodeId == -1)
				activityNodeId = bldgShape->GetNodeNameId("activity_node");
			glm::vec3 dustPos = getNodeIdPosition(activityNodeId);

			if (rotationalNodeId == -1)
			{
				if (S_stricmp(appearType->rotationalNodeId,"NONE") != 0)
	   				rotationalNodeId = bldgShape->GetNodeNameId(appearType->rotationalNodeId);
				else
					rotationalNodeId = -2;
			}

			if (rotationalNodeId >= 0)
				dustPos = getNodeIdPosition(rotationalNodeId);
				
			glm::vec3 wakePos;
			mc2ToRender(wakePos, dustPos);
			
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, wakePos);
					

			drawInfo.m_parentToWorld = &shapeOrigin;
			if (!MLRVertexLimitReached && activity)
				activity->Draw(&drawInfo);
			
			if (activity1)
			{
				if (activityNodeId == -1)
					activityNodeId = bldgShape->GetNodeNameId("activity_node");
				glm::vec3 dustPos = getNodeIdPosition(activityNodeId);
	
				if (rotationalNodeId == -1)
				{
					if (S_stricmp(appearType->rotationalNodeId,"NONE") != 0)
		   				rotationalNodeId = bldgShape->GetNodeNameId(appearType->rotationalNodeId);
					else
						rotationalNodeId = -2;
				}
	
				if (rotationalNodeId >= 0)
					dustPos = getNodeIdPosition(rotationalNodeId);
					
				glm::vec3 wakePos;
				mc2ToRender(wakePos, dustPos);
				
				mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
				mc2::math::buildTranslation(shapeOrigin, wakePos);
						
	
				drawInfo.m_parentToWorld = &shapeOrigin;
				if (!MLRVertexLimitReached)
					activity1->Draw(&drawInfo);
			}
		}

	}
	return NO_ERR;
}

long BldgAppearance::renderShadows (void)
{
	return NO_ERR;
}

long TreeAppearance::render (long depthFixup)
{
	if (gos_IsModernShadowPathActive() && shadowIsMoving()) GpuStaticPropBatcher::instance().captureMoverShadowShape(static_cast<const Appearance*>(this),treeShape);
	// Mirror BldgAppearance::render: bypass inView under GPU path — the
	// GPU clipper decides visibility, and the legacy angular cull has a
	// ~87% false-negative rate at wolfman zoom.
	if (inView || g_useGpuStaticProps)
	{
		long color = SD_BLUE;
		if ((teamId > -1) && (teamId < 8)) {
			static uint32_t colorTable[3] = {SB_GREEN | 0xff000000, SB_BLUE | 0xff000000, SB_RED | 0xff000000};
			color = colorTable[homeTeamRelationship];
		}
		bool submittedToGpu = false;
		if (g_useGpuObjects)
		{
			GpuStaticPropBatcher::instance().recordEligibleActor(
				GpuStaticPropPopulation::Tree);

			// Stage 3.C: static registry fast path. If this tree's instance was
			// previously registered and position/shape are stable, inject it into
			// the batcher via markVisible() (processed at registry flush) instead of
			// running the full submitMultiShape() compute path.
			// CacheGpuLightData() is called here (not in touch()) so the light-index
			// refresh is co-located with the render-side emission that needs it.
			// The UINT32_MAX guard handles the degenerate case where no light data
			// is available: invalidate and fall through to the dynamic path.
			// Does NOT return early — selection visualization (drawBars/drawBrackets)
			// at lines 4141-4161 must still run if selected is non-zero.
			if (IsStaticNow()) {
				// Diagnostic 2026-05-05 (advisor-recommended boundary test): set
				// MC2_FORCE_DYNAMIC_TREES=1 to force the static path to fall back
				// to dynamic submitMultiShape. If "black billboard square" trees
				// disappear with the env var set, the static replay path is the
				// failing boundary. If they remain, shared draw/material/global
				// state is guilty. Revert by unsetting the env var (no rebuild).
				// ── [GUARD_FORWARD v1] ZERO-LIGHT-ROW-1 divergence census ──────
				// Same measurement as BldgAppearance::render. Trees are the
				// WEAKER of the two sites: TreeAppearance::touch/touchSerialCommit
				// assign staticReg[activeLOD].lightDataIndex with
				// no sentinel test at all, where the building touch paths at least
				// record one into hasValidStaticLight. Counting only.
				{
					static uint64_t s_gf_total = 0, s_gf_agree = 0, s_gf_differ = 0;
					static uint64_t s_gf_fwdSentinel = 0, s_gf_valSentinel = 0, s_gf_bothSentinel = 0;
					static bool     s_gf_reported = false;
					const static_light_row::Divergence gfd =
						static_light_row::classifyGuardForward(
							treeShape->getCachedGpuLightIndex(),          // validated
							staticReg[activeLOD].lightDataIndex);         // forwarded
					++s_gf_total;
					switch (gfd) {
						case static_light_row::kAgree:                 ++s_gf_agree;        break;
						case static_light_row::kDifferBothValid:       ++s_gf_differ;       break;
						case static_light_row::kForwardedSentinelOnly: ++s_gf_fwdSentinel;  break;
						case static_light_row::kValidatedSentinelOnly: ++s_gf_valSentinel;  break;
						case static_light_row::kBothSentinel:          ++s_gf_bothSentinel; break;
					}
					// CADENCE: see the BldgAppearance::render census. Trees are the
					// worst case -- thousands per frame -- so first-hit + a rare
					// periodic is the only sane emit policy for a default-ON probe.
					const bool gfFirstMixed =
						static_light_row::guardCorrectionWouldChangeBehaviour(gfd)
						&& !s_gf_reported;
					if (gfFirstMixed) s_gf_reported = true;
					if (gfFirstMixed || ((s_gf_total % 200000ull) == 0ull && s_gf_agree != s_gf_total)) {  // 2026-10-05: census only when a divergence exists
						mc2::fmt::print_err("[GUARD_FORWARD v1] event={} class=tree total={:d} agree={:d} differ_both_valid={:d} forwarded_sentinel_only={:d} validated_sentinel_only={:d} both_sentinel={:d} would_change={:d}\n",
							mc2::fmt::safe(gfFirstMixed ? "first_divergence" : "census"),
							mc2::fmt::as_u64(static_cast<unsigned long long>(s_gf_total)),
							mc2::fmt::as_u64(static_cast<unsigned long long>(s_gf_agree)),
							mc2::fmt::as_u64(static_cast<unsigned long long>(s_gf_differ)),
							mc2::fmt::as_u64(static_cast<unsigned long long>(s_gf_fwdSentinel)),
							mc2::fmt::as_u64(static_cast<unsigned long long>(s_gf_valSentinel)),
							mc2::fmt::as_u64(static_cast<unsigned long long>(s_gf_bothSentinel)),
							mc2::fmt::as_u64(static_cast<unsigned long long>((s_gf_fwdSentinel + s_gf_valSentinel))));
						fflush(stderr);
					}
				}
				static const bool s_forceDynamicTrees =
				    bdForceDynamicDefault<"MC2_FORCE_DYNAMIC_TREES">();
				if (s_forceDynamicTrees) {
					invalidateStaticRegistration();
				} else if (treeShape->getCachedGpuLightIndex() == UINT32_MAX) {
					// Light-data gather failed this frame — invalidate so the dynamic
					// path re-runs and re-registers next frame with correct lights.
					invalidateStaticRegistration();
				} else {
					// 2026-05-11: see BldgAppearance::render markVisible site.
					// TREE-OVERRIDE-LOD-MVP Task 2: replay the active LOD's recipe +
					// light slot (activeLOD pinned 0 → identical to single-LOD today).
					// STATIC-PROP REGISTRATION CONTRACT v1: markVisibleChecked() reports
					// whether the registry ACCEPTED a live recipe. Only suppress the legacy
					// fallback (submittedToGpu=true) when it did. On a dead/tombstoned handle
					// (the "trees vanish on replay" bug) invalidate so the !submittedToGpu
					// full-bake path below re-registers + draws this frame — GPU cache miss
					// becomes a legacy re-bake, NOT an invisible prop.
					const GpuStaticPropRegistry::StaticSubmitResult treeRes =
						GpuStaticPropRegistry::markVisibleChecked(
							staticReg[activeLOD].recipeIndex,
							staticReg[activeLOD].lightDataIndex,
							treeShape ? treeShape->GetExtentRadius() : 0.0f);
					if (treeRes == GpuStaticPropRegistry::StaticSubmitResult::Submitted) {
						// [LIGHTSLOT v1] Task 0: registered static-replay trees record
						// their per-frame slot here (the steady-state path) so the
						// distinct-slot set captures the whole forest, not just frame-1
						// full-bake instances.
						mc2_lightslot_trace::recordInstance(
							appearType,
							appearType && appearType->treeRenderShapeLodCount > 0,
							staticReg[activeLOD].lightDataIndex);
						submittedToGpu = true;
					} else {
						bdLogStaticRegInvalid("tree", static_cast<int>(treeRes),
							staticReg[activeLOD].recipeIndex,
							appearType ? appearType->name : nullptr);
						invalidateStaticRegistration();  // drop dead handle -> full-bake re-registers below
					}
				}
			}
			if (!submittedToGpu && treeShape)
			{
				// Stage 3.C / M1: shape-swap invalidation. IsStaticNow()'s
				// staticReg.shape==treeShape check routes us here when treeShape was
				// reassigned (LOD swap at bdactor.cpp, damage at ~3596/3626), but
				// staticReg.registered==true still blocks the registration block below.
				// Invalidate the stale entry first so the new shape gets registered.
				// PERF DIAGNOSTIC 2026-05-07: count this branch — per capture 3
				// analysis, LOD-swap-driven invalidate+re-register on trees was
				// running tens of times per frame, leaking recipe slots and
				// pin-count churn. Tracy zone makes the rate visible per-frame.
				// TREE-OVERRIDE-LOD-MVP Task 2: the dynamic recovery path only ever
				// deals with the live LOD0 render-instance (treeShape); key on the
				// active LOD tuple (activeLOD pinned 0).
				// forced-LOD FIX: only the live LOD0 instance can suffer treeShape reassignment;
				// guard to activeLOD==0 so a higher LOD's nullptr .shape never triggers invalidation.
				if (activeLOD == 0 && staticReg[0].registered && staticReg[0].shape != treeShape) {
					invalidateStaticRegistration();
				}

				// Stage 2.C+: see BldgAppearance::render for callerName intent.
				const char* callerName = (appearType ? appearType->name : nullptr);
				submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(
					treeShape, GpuStaticPropPopulation::Tree, callerName);
				// Slice 2 (object-offload) — Stage 2.B: see BldgAppearance::render
				// for full rationale on the late-reg recovery flag.
				if (!submittedToGpu &&
				    GpuStaticPropBatcher::instance().wasLastFailureLateRegistration())
				{
					needsFullBakeNextFrame = true;
					invalidateStaticRegistration();  // clear stale registration if any
				}
				// Stage 3.C: registration block. On the first successful full-bake
				// submission with no late-reg flag, snapshot the leaf batch into the
				// registry. Subsequent frames use the static path above.
				// Pass treeShape as multi so flush() can patch lightDataIndex each frame.
				if (submittedToGpu && !shadowIsMoving() && !staticReg[activeLOD].registered
				        && !needsFullBakeNextFrame) {
					const auto& batch =
						GpuStaticPropBatcher::instance().getLastBuiltBatch();
					int32_t legacyIdx = -1;
					(void)GameAdapters::StaticProp::syncStaticProp(
						treeShape, batch.data(), batch.size(), &legacyIdx);
					staticReg[activeLOD].recipeIndex = legacyIdx;
					staticReg[activeLOD].registered  = (staticReg[activeLOD].recipeIndex >= 0);
					staticReg[activeLOD].shape        = treeShape;
					if (staticReg[activeLOD].registered) {
						// COUNTER-HONESTY-1: tag Tree on the dynamic re-register
						// path too (mirror of the Building tag). See
						// TreeAppearance::registerStatic for the full rationale —
						// behaviour-neutral, census-honest.
						GpuStaticPropRegistry::setRecipePopulation(
							staticReg[activeLOD].recipeIndex,
							GpuStaticPropPopulation::Tree);
						// H4 follow-up (2026-05-07): per-frame re-registration
						// after LOD/shape swap has the same lightData_ gap as
						// mission-load registerStatic(). Force one full update()
						// so touch() cannot resubmit default-zero lightData_.
						// Spec: docs/superpowers/specs/2026-05-07-lod-swap-static-registry-churn.md
						needsFullBakeNextFrame = true;
					}
				}
			}
			if (!submittedToGpu)
			{
				GpuStaticPropBatcher::instance().recordCpuFallback(
					GpuStaticPropPopulation::Tree);
			}
		}
		// Legacy bypass-cull path. Mutually exclusive with slice 1 — gated on
		// !g_useGpuObjects. Tagged Legacy so Gate F's fallback-rate is computed
		// only over slice-1 populations. See spec R1.
		if (!submittedToGpu && !g_useGpuObjects && g_useGpuStaticProps && treeShape)
		{
			submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(
				treeShape, GpuStaticPropPopulation::Legacy);
		}
		if (!submittedToGpu)
			treeShape->Render();

		if (selected & DRAW_BARS)
		{
			drawBars();
		}

		if ( selected & DRAW_BRACKETS )
		{
			drawSelectBrackets(color);
		}

		if ( selected & DRAW_TEXT )
		{

			if (objectNameId != -1)
			{
				char tmpString[255];
				cLoadString(objectNameId, tmpString, 254);

				drawTextHelp(tmpString, color);
			}
		}

		// I don't want my selection reset each time I draw HKG
//		selected = FALSE;

	}
	return NO_ERR;
}

long TreeAppearance::renderShadows (void)
{
	return NO_ERR;
}
