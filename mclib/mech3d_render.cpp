#include "render_snap.h"
#include "terrain_runtime.h"
#include <algorithm>
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include <utility>
#include <atomic>
#include "utils/mc2log.h"
// Mech3DAppearance: bounds recalculation, render and renderShadows, with the MLR draw / light-row / FX-count counters they feed.
// Declared in the class headers; shared names in mech3d_internal.h.
#include "mech3d_internal.h"
#include "../RenderCore/axis_convert.h"
#include "mc2fmt.h"

// M2.5 (Q6 amendment 2): always-on MLR mech draw counter. Incremented
// at the legacy mechShape->Render(true) fallback site (~line 2623) so
// the M2.6 readiness decision rule has live data on Path-B incidence.
// NOT env-gated: M2.6 must consult this number regardless of
// MC2_OBJECT_ID_BUFFER state. Consumed per-mission by
// GpuMechBatcher::onMapUnload() via consumeAndResetMlrMechDraws().
static uint64_t s_mlrMechDrawsThisMission = 0;

// M2.5: getter for the per-mission MLR draw count, callable from
// GpuMechBatcher::onMapUnload() in a different TU. Declaration in
// gos_mech_batcher.cpp is at file scope per external-review C1.
extern "C" uint64_t consumeAndResetMlrMechDraws() {
    const uint64_t v = s_mlrMechDrawsThisMission;
    s_mlrMechDrawsThisMission = 0;
    return v;
}

// MC2_MECH_LOD_TRACE=1: per-actor LOD-swap boundary print.
static const bool s_mechLodTrace = false;

// MECH-LIGHTROW-FRESHNESS-1 counters. These are the UNCAPPED truth; the stderr
// lines below are capped/one-shot, so nobody should ever count log lines.
// s_mechStaleLightRowStale is the instrument that decides whether this gate
// is inert or load-bearing: zero in steady state means the mech submit path
// never actually holds a stale stamp, and the gate is inert rather than wrong.
// MECH-DECLINE-FIX-1: renamed from s_mechStaleLightRowDeclines. Nothing is
// declined any more (see the submit site) -- the counter records DETECTIONS.
static uint64_t s_mechStaleLightRowStale    = 0;
static uint64_t s_mechLightRowOutOfRange    = 0;

// MECH-DECLINE-FIX-1: per-mission consume accessor so the always-on
// [MECHBATCHER v1] onMapUnload summary can surface these totals. Mirrors the
// consumeAndResetMlrMechDraws() pattern directly above -- same TU-crossing
// shape, same per-mission reset semantics. Without this the counters were
// readable ONLY from their own once-per-mech-type stderr line, which means a
// count that starts or grows after that first line was unobservable, and the
// advice "check the counter on your first launch" was unexecutable.
extern "C" void consumeAndResetMechLightRowCounters(uint64_t* staleOut,
                                                    uint64_t* oobOut) {
    if (staleOut) *staleOut = s_mechStaleLightRowStale;
    if (oobOut)   *oobOut   = s_mechLightRowOutOfRange;
    s_mechStaleLightRowStale = 0;
    s_mechLightRowOutOfRange = 0;
}

// MC2_FX_COUNT_LOG=1: env-gated per-effect-site GOSFX draw counter for the
// Mech3DAppearance::render() effect sites. Default OFF, zero behavior change.
// Counts draw attempts (control reaching the site); emits a [FX_COUNT v1]
// summary line every 9000 ticks. Agent-checkable oracle for effect counts.
static const bool s_fxCountLog = false;
namespace {
	enum class FxSite {
		FX_RDUST=0, FX_LDUST, FX_JUMP, FX_LBOOM, FX_RBOOM,
		FX_CSMOKE, FX_SMOKE, FX_WAKE, FX_HELIDUST, FX_RARMSMOKE, FX_LARMSMOKE,
		FX_SITE_COUNT
	};
	using enum FxSite;
	static constexpr int FX_SITE_COUNT_I = std::to_underlying(FxSite::FX_SITE_COUNT);
	static unsigned long long s_fxDrawCounts[FX_SITE_COUNT_I] = {0};
	static unsigned long long s_fxTotalDraws = 0;
	static void fxCountTick(FxSite site) {
		++s_fxDrawCounts[std::to_underlying(site)];
		if ((++s_fxTotalDraws % 9000ULL) == 0ULL) {
			MC2_DIAGF(
				"[FX_COUNT v1] event=summary total=%llu rDust=%llu lDust=%llu jump=%llu "
				"lBoom=%llu rBoom=%llu critSmoke=%llu smoke=%llu wake=%llu heliDust=%llu "
				"rArmSmoke=%llu lArmSmoke=%llu\n",
				s_fxTotalDraws,
				s_fxDrawCounts[std::to_underlying(FX_RDUST)], s_fxDrawCounts[std::to_underlying(FX_LDUST)], s_fxDrawCounts[std::to_underlying(FX_JUMP)],
				s_fxDrawCounts[std::to_underlying(FX_LBOOM)], s_fxDrawCounts[std::to_underlying(FX_RBOOM)], s_fxDrawCounts[std::to_underlying(FX_CSMOKE)],
				s_fxDrawCounts[std::to_underlying(FX_SMOKE)], s_fxDrawCounts[std::to_underlying(FX_WAKE)], s_fxDrawCounts[std::to_underlying(FX_HELIDUST)],
				s_fxDrawCounts[std::to_underlying(FX_RARMSMOKE)], s_fxDrawCounts[std::to_underlying(FX_LARMSMOKE)]);
		}
	}
	// Shutdown emit via atexit() — mirrors the proven shutdownTexResolveTable
	// pattern (tex_resolve_table.cpp). A static with a CONSTRUCTOR side
	// effect runs reliably at dynamic-init; the registered handler fires at
	// clean process exit and always prints final per-site totals (even total=0
	// if a short/idle mission drew no effects — proves the chain end to end).
	static void fxCountEmit() {
		if (!s_fxCountLog) return;
		MC2_DIAGF(
			"[FX_COUNT v1] event=shutdown total=%llu rDust=%llu lDust=%llu jump=%llu "
			"lBoom=%llu rBoom=%llu critSmoke=%llu smoke=%llu wake=%llu heliDust=%llu "
			"rArmSmoke=%llu lArmSmoke=%llu\n",
			s_fxTotalDraws,
			s_fxDrawCounts[std::to_underlying(FX_RDUST)], s_fxDrawCounts[std::to_underlying(FX_LDUST)], s_fxDrawCounts[std::to_underlying(FX_JUMP)],
			s_fxDrawCounts[std::to_underlying(FX_LBOOM)], s_fxDrawCounts[std::to_underlying(FX_RBOOM)], s_fxDrawCounts[std::to_underlying(FX_CSMOKE)],
			s_fxDrawCounts[std::to_underlying(FX_SMOKE)], s_fxDrawCounts[std::to_underlying(FX_WAKE)], s_fxDrawCounts[std::to_underlying(FX_HELIDUST)],
			s_fxDrawCounts[std::to_underlying(FX_RARMSMOKE)], s_fxDrawCounts[std::to_underlying(FX_LARMSMOKE)]);
		std::fflush(stdout);
	}
	// atexit registration happens once from Mech3DAppearance::render() (a code
	// path guaranteed to run), not from a discardable anon-namespace static.
}

bool Mech3DAppearance::recalcBounds (void)
{
	// F3 CPU projection cost-baseline: aggregate per-actor scope into the
	// recalcBounds_perframe bucket. No-op when env OFF.
	::mc2_cpu_proj_cost::Scope _f3_recalcBounds_scope(
	    ::mc2_cpu_proj_cost::BUCKET_RECALCBOUNDS_PERFRAME);
	::mc2_cpu_proj_cost::add_workload_recalcbounds(1);

	// DEGRADE-DON'T-CRASH: no base shape (partial import) -> treat as not in
	// view; the mechShape->GetNodeNameId() / LOD-swap derefs below are unsafe.
	if (!mechShape)
	{
		setVisibilityGatesFromLegacy(false);
		return inView;
	}

	Stuff::Vector4D tempPos;
	bool wasInView = inView;
	setVisibilityGatesFromLegacy(false);
	float eyeDistance = 0.0f;

	if (eye)
	{
		//ALWAYS need to do this or select is YAYA
		// [PROJECTZ:ScreenXYOracle id=mech3d_screen_pos]
		eye->projectForScreenXY(position,screenPos);
		
		if (eye->usePerspective)
		{
			glm::vec3 Distance;
			glm::vec3 objPosition;
			glm::vec3 eyePosition(eye->getCameraOrigin());
			mc2ToRender(objPosition, position);
	
			Distance = objPosition - eyePosition;
			eyeDistance = glm::length(Distance);
			if (eyeDistance > Camera::MaxClipDistance)
			{
				hazeFactor = 1.0f;
				setVisibilityGatesFromLegacy(false);
			}
			else if (eyeDistance > Camera::MinHazeDistance)
			{
				Camera::HazeFactor = (eyeDistance - Camera::MinHazeDistance) * Camera::DistanceFactor;
				setVisibilityGatesFromLegacy(true);
			}
			else
			{
				Camera::HazeFactor = 0.0f;
				setVisibilityGatesFromLegacy(true);
			}
			
			if (inView)
			{
				glm::vec3 Distance;
				glm::vec3 objPosition;
				glm::vec3 eyePosition(eye->getCameraOrigin());
				mc2ToRender(objPosition, position);
		
				Distance = objPosition - eyePosition;
				Distance = glm::normalize(Distance);
				
				float cosine = glm::dot(Distance, eye->getLookVector());
 				if (cosine > eye->cosHalfFOV)
					setVisibilityGatesFromLegacy(true);
				else
					setVisibilityGatesFromLegacy(false);
			}
		}
		else
		{
			Camera::HazeFactor = 0.0f;
			setVisibilityGatesFromLegacy(true);
		}
		
		if (inView)
		{
			if (reloadBounds)
				mechType->reinit();

			
			// do a rough check if on screen.  If no where near, do NOT do the below.
			// Mighty mighty slow!!!!
			// Use the original check done before all this 3D madness.  Dig out sourceSafe tomorrow!
			tempPos = screenPos;
			upperLeft.x = tempPos.x;
			upperLeft.y = tempPos.y;
			
			lowerRight.x = tempPos.x;
			lowerRight.y = tempPos.y;
			
			upperLeft.x += (mechType->boundsUpperLeftX * eye->getScaleFactor());
			upperLeft.y += (mechType->boundsUpperLeftY * eye->getScaleFactor());
	
			lowerRight.x += (mechType->boundsLowerRightX * eye->getScaleFactor());
			lowerRight.y += (mechType->boundsLowerRightY * eye->getScaleFactor());

			if ((lowerRight.x >= 0) && (lowerRight.y >= 0) &&
				(upperLeft.x <= eye->getScreenResX()) &&
				(upperLeft.y <= eye->getScreenResY()))
			{
				glm::vec3 boxCoords[8];
				Stuff::Vector4D bcsp[8];
	
				if (rootNodeIndex == -1)
					rootNodeIndex = mechShape->GetNodeNameId("joint_root");

				//Gotta let the NodeIdPosition know that its matrix is valid or invalid so this actually does clip mech to screen!!
				// Leave the old one in place until we are inView again!
				// Should fix flickering on screen edge?  It does.  Must let jump flicker or he never comes down.
				setVisibilityGatesFromLegacy(wasInView);
				if (inView || (unitAnim().currentGestureId == 20))
					baseRootNodeDifference = (getNodeIdPosition(rootNodeIndex).z - position.z) - baseRootNodeHeight;

				if (inView && isHelicopter)
					baseRootNodeDifference -= HELICOPTER_FACTOR;

				setVisibilityGatesFromLegacy(true);

				if (InEditor)
				{
					if (isHelicopter)
						baseRootNodeDifference = 0.0f - HELICOPTER_FACTOR;
					else
						baseRootNodeDifference = 0.0f;
				}

				boxCoords[0].x = position.x + mechType->typeUpperLeft.x;
				boxCoords[0].y = position.y + mechType->typeUpperLeft.y;
				boxCoords[0].z = position.z + mechType->typeUpperLeft.z + baseRootNodeDifference;
	
				boxCoords[1].x = position.x + mechType->typeUpperLeft.x;
				boxCoords[1].y = position.y + mechType->typeLowerRight.y;
				boxCoords[1].z = position.z + mechType->typeUpperLeft.z + baseRootNodeDifference;
	
				boxCoords[2].x = position.x + mechType->typeLowerRight.x;
				boxCoords[2].y = position.y + mechType->typeUpperLeft.y;
				boxCoords[2].z = position.z + mechType->typeUpperLeft.z + baseRootNodeDifference;
	
				boxCoords[3].x = position.x + mechType->typeLowerRight.x;
				boxCoords[3].y = position.y + mechType->typeLowerRight.y;
				boxCoords[3].z = position.z + mechType->typeUpperLeft.z + baseRootNodeDifference;
	
				boxCoords[4].x = position.x + mechType->typeLowerRight.x;
				boxCoords[4].y = position.y + mechType->typeLowerRight.y;
				boxCoords[4].z = position.z + mechType->typeLowerRight.z + baseRootNodeDifference;
	
				boxCoords[5].x = position.x + mechType->typeLowerRight.x;
				boxCoords[5].y = position.y + mechType->typeUpperLeft.y;
				boxCoords[5].z = position.z + mechType->typeLowerRight.z + baseRootNodeDifference;
	
				boxCoords[6].x = position.x + mechType->typeUpperLeft.x;
				boxCoords[6].y = position.y + mechType->typeLowerRight.y;
				boxCoords[6].z = position.z + mechType->typeLowerRight.z + baseRootNodeDifference;
	
				boxCoords[7].x = position.x + mechType->typeUpperLeft.x;
				boxCoords[7].y = position.y + mechType->typeUpperLeft.y;
				boxCoords[7].z = position.z + mechType->typeLowerRight.z + baseRootNodeDifference;
	
				mc2snap::liftBoxZ(boxCoords, visualDelta_);   // UNIT-VISUAL-SNAP-2: selection box follows the drawn mesh

				float maxX = 0.0f, maxY = 0.0f;
				float minX = 0.0f, minY = 0.0f;

				for (long i=0;i<8;i++)
				{
					// [PROJECTZ:ScreenXYOracle id=mech3d_box_rect]
					eye->projectForScreenXY(boxCoords[i],bcsp[i]);
					if (!i)
					{
						maxX = minX = bcsp[i].x;
						maxY = minY = bcsp[i].y;
					}
					
					if (i)
					{
						maxX = std::max(maxX, bcsp[i].x);
						
						minX = std::min(minX, bcsp[i].x);
							
						maxY = std::max(maxY, bcsp[i].y);
						
						minY = std::min(minY, bcsp[i].y);
					}
				}
		
				upperLeft.x = minX;
				upperLeft.y = minY;
				lowerRight.x = maxX;
				lowerRight.y = maxY;
				
				// C3: when GPU cull is active, a GPU-visible actor must run LOD-swap
				// even if the CPU screen-rect test disagrees (N-1 lag can diverge).
				const bool cpuScreenRect = (lowerRight.x >= 0) && (lowerRight.y >= 0) &&
					(upperLeft.x <= eye->getScreenResX()) &&
					(upperLeft.y <= eye->getScreenResY());
				// Only consult GPU visibility when readback is actually enabled.
				// LIFECYCLE=1 without READBACK=1 makes readback_isActorVisibleLagged fail-open
				// (returns true for every actor), which would unconditionally widen the LOD-swap
				// block and cause a perf regression instead of a win.
				const bool rbVisible = (s_gpuCullLifecycle && gpu_cull::readback_isEnabled())
					? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_))
					: false;
				if (cpuScreenRect || rbVisible)
				{
					setVisibilityGatesFromLegacy(cpuScreenRect || rbVisible);
					
					if (status != OBJECT_STATUS_DESTROYED)
					{
						bool baseLOD = true;
						DWORD selectLOD = 0;
						if (useHighObjectDetail)
						{
							//Set LOD of Model here because we have the distance and we KNOW we can see it!
							for (long i=1;i<MAX_LODS;i++)
							{
								if (mechType->mechShape[i] && mechType->mechShape[i]->GetNumShapes() && (eyeDistance > mechType->lodDistance[i]))
								{
									baseLOD = false;
									selectLOD = i;
								}
							}
						}
						else
						{
							if (mechType->mechShape[1] && mechType->mechShape[1]->GetNumShapes())
							{
								baseLOD = false;
								selectLOD = 1;
							}
						}

						if (selectLOD != currentLOD)
						{
							if (s_mechLodTrace) {
								mc2::fmt::print_err("[MECHLOD v1] event=lod_swap actor={:016X} old_lod={:d} new_lod={:d}\n", mc2::fmt::as_ptr(static_cast<void*>(this)), mc2::fmt::as_u32(static_cast<unsigned long>(currentLOD)), mc2::fmt::as_u32(static_cast<unsigned long>(selectLOD)));
							}
							currentLOD = selectLOD;

							BYTE alphaValue = mechShape->GetAlphaValue();
							//mechShape->ClearAnimation();	//DO NOT do this with animating things!!
							mc2mechanim::UnregisterImportedActor(mechShape);	// IMPORTED-ACTOR-STABLE-KEY-1
							mechShapeOwner_.reset();
							mechShape = nullptr;

							mechShapeOwner_.reset(mechType->mechShape[currentLOD]->CreateFrom());
							mechShape = mechShapeOwner_.get();
							mechShape->SetAlphaValue(alphaValue);

							DWORD r, g, b;
							getPaintScheme(r, g, b);
							resetPaintScheme(r,g,b);

							if (unitAnim().rightArmOff)
								mechShape->StopUsing("joint_ruarm");

							if (unitAnim().leftArmOff)
								mechShape->StopUsing("joint_luarm");
						}

						if (currentLOD && baseLOD)
						{
							currentLOD = 0;

							BYTE alphaValue = mechShape->GetAlphaValue();
							mc2mechanim::UnregisterImportedActor(mechShape);	// IMPORTED-ACTOR-STABLE-KEY-1
							mechShapeOwner_.reset();
							mechShape = nullptr;

							mechShapeOwner_.reset(mechType->mechShape[currentLOD]->CreateFrom());
							mechShape = mechShapeOwner_.get();
							mechShape->SetAlphaValue(alphaValue);

							DWORD r, g, b;
							getPaintScheme(r, g, b);
							resetPaintScheme(r,g,b);

							if (unitAnim().rightArmOff)
								mechShape->StopUsing("joint_ruarm");

							if (unitAnim().leftArmOff)
								mechShape->StopUsing("joint_luarm");
						}

						mechType->setAnimation(mechShape,unitAnim().currentGestureId);
					}
				}
				else
				{
					setVisibilityGatesFromLegacy(false);		//Did alot of extra work checking this, but WHY draw and insult to injury?
				}
			}
			else
			{
				setVisibilityGatesFromLegacy(false);
			}
		}
	}

	return(inView);
}

long Mech3DAppearance::render (long depthFixup)
{
	if (!eye) return 0;
	// FX_COUNT one-time init: register the shutdown emit from a code path that
	// is guaranteed to run (a discardable anon-namespace static can be elided
	// by /OPT:REF). Unconditional init print proves render() runs + flag state.
	{
		static bool s_fxInitDone = false;
		if (!s_fxInitDone) {
			s_fxInitDone = true;
			MC2_DIAGF("[FX_COUNT v1] event=init flag=%d\n", s_fxCountLog ? 1 : 0);
			std::fflush(stdout);
			if (s_fxCountLog) std::atexit(&fxCountEmit);
		}
	}

	// DEGRADE-DON'T-CRASH: a partially-imported mech (no base shape) leaves
	// mechShape NULL (see init()). Render nothing rather than deref NULL.
	if (!mechShape)
	{
		// DIAG (GPU-MECH-EQUIV recon): capture the null-shape invisible cohort
		// (Cohort B — broken appearance type with mechShape==NULL early-outs here,
		// before both GPU submit and CPU fallback). Gated by MC2_MECH_RESTORE_TRACE.
		{
			static const bool s_mrtNull = false;
			static int s_nullEmits = 0;
			if (s_mrtNull && s_nullEmits < 20000) {
				++s_nullEmits;
				mc2::fmt::print_err("[MECHRESTORE v1] event=nullshape frame={:d} actor={:d} type={:016X} status={:d}\n", mc2::fmt::as_u32(static_cast<unsigned>(g_mc2FrameCounter.load(std::memory_order_relaxed))), mc2::fmt::as_u32(static_cast<unsigned long>(actorHandle_)), mc2::fmt::as_ptr(static_cast<void*>(mechType)), mc2::fmt::as_i32(static_cast<int>(status)));
			}
		}
		return NO_ERR;
	}

	// Force textures to reload due to unique instance.
	mechShape->SetTextureHandle(0,localTextureHandle);

	if (rightArm)
		rightArm->SetTextureHandle(0,localTextureHandle);

	if (leftArm)
		leftArm->SetTextureHandle(0,localTextureHandle);

	const bool mechShouldRender = s_gpuCullLifecycle
			? (gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_)))
			: inView;
	if (mechShouldRender)
	{
		if (visible)
		{
			uint32_t color = SD_BLUE;
			uint32_t highLight = 0x007f7f7f;
			if ((teamId > -1) && (teamId < 8)) {
				static uint32_t highLightTable[3] = {0x00007f00, 0x0000007f, 0x007f0000};
				static uint32_t colorTable[3] = {SB_GREEN | 0xff000000, SB_BLUE | 0xff000000, SB_RED | 0xff000000};
				color = colorTable[homeTeamRelationship];
				highLight = highLightTable[homeTeamRelationship];
			}
			if (selected & DRAW_COLORED && duration <= 0 )
			{
				mechShape->SetARGBHighLight(highLight);
			}
			else
			{
				mechShape->SetARGBHighLight(highlightColor);
			}

			Camera::HazeFactor = hazeFactor;

			if (drawFlash)
			{
				mechShape->SetARGBHighLight(flashColor);
			}
			// Call Multi-shape render stuff here.
			// GPU mech batcher Slice A: try GPU submit first; fall back to CPU
			// path if registration / capacity / shader-init says so. Counter
			// recorded BEFORE registration check so eligible-actor totals are
			// accurate even when the actor opts out for unrelated reasons.
			GpuMechBatcher::instance().recordEligibleActor();

			// Slice C1: render-only mech GPU cull. If
			// the GPU lagged-readback says this actor was invisible last
			// frame, skip the GPU mech submit. We DO NOT bypass mechShape->Render
			// (CPU fallback) because the actor still ran updateGeometry/AI/etc;
			// we just skip writing its geometry into the GPU mech bucket. update()
			// has already run by the time render() is called, so AI/lifecycle/
			// damage all proceed normally for offscreen actors. Independent of
			// MC2_GPU_CULL_LIFECYCLE which gates a separate concern.
			// Stale readback semantics: returns true (visible) for actors with
			// no readback record yet (newly spawned), so this is fail-open in
			// uncertainty. One frame of stale "invisible" → one frame of
			// missing render, recovered next frame at 60Hz.
			bool mechGpuCullSkip = false;
			{
				// CONSERVATIVE-OR (mirrors terrobj.cpp / static-prop fix):
				// the lagged GPU readback may CONFIRM invisibility but must
				// NOT DENY visibility. Only skip the GPU submit when BOTH the
				// lagged readback AND the CPU inView test agree the mech is
				// offscreen. The lagged readback is unstable frame-to-frame
				// (d65552ab made readback default-ON; slot rotation + camera
				// motion flip it even for a dead-center actor). When it flipped
				// to "invisible" we skipped the GPU submit and fell back to the
				// CPU mechShape->Render() path, which does not actually draw
				// while GPU mech batching owns rendering -> the mech vanished
				// for that frame -> center-screen flicker. Requiring CPU
				// inView agreement means an in-view mech is never skipped, so
				// it is always GPU-drawn; only genuinely-offscreen mechs (both
				// signals agree) still skip, preserving the cull perf win.
				const bool lagVis = gpu_cull::readback_isActorVisibleLagged(
				    static_cast<uint32_t>(actorHandle_));
				if (!lagVis && !inView) {
					mechGpuCullSkip = true;
				}
			}

			bool gpuMechSubmitted = false;
			if (!mechGpuCullSkip && g_drawMechs) {
				// Replicate the highlight selection from the CPU SetARGBHighLight
				// branches above so the GPU path sees the same color choice.
				uint32_t gpuHighlightARGB = highLight;
				if (selected & DRAW_COLORED && duration <= 0)
					gpuHighlightARGB = highLight;
				else
					gpuHighlightARGB = static_cast<uint32_t>(highlightColor);
				if (drawFlash)
					gpuHighlightARGB = static_cast<uint32_t>(flashColor);

				GpuMechSubmitDesc desc{};
				desc.mechShape      = mechShape;
				desc.mechType       = mechType;
				desc.currentLOD     = static_cast<int>(currentLOD);
				desc.slot0TexHandle = static_cast<uint32_t>(localTextureHandle);
				// AO-1: carry the AO slot (0 when no/failed AO). HAS_AO bit (4) added
				// below ONLY if aoValid, so the shader samples unit 6 iff a real AO loaded.
				desc.slot6TexHandle = aoValid ? static_cast<uint32_t>(aoTextureHandle) : 0u;
				// NORMALS-1: carry the normal-map slot. HAS_NORMAL bit (5) added below ONLY
				// if normalValid, so the shader samples unit 7 iff a real normal map loaded.
				desc.slot7TexHandle = normalValid ? static_cast<uint32_t>(normalTextureHandle) : 0u;
				// Slice B1: use the dedup-cache slot populated by
				// CacheGpuLightData() in update() (mech3d.cpp — the ONLY
				// writer that ever runs on a mech shape).
				// MECH-LIGHTROW-FRESHNESS-1 (default-ON; =0 reverts bit-for-bit)
				// THE DEFECT. cachedGpuLightIndex_ names a DYNAMIC light-table
				// slot, and dynamic slots are rebased EVERY FRAME:
				// legacy texture manager::resetLightData (mclib/txmmgr.cpp)
				// resets lightDataStructuresCount to the static high-water and
				// clears every dedup map, its own comment (txmmgr.cpp)
				// stating that prior-frame hash->slot entries "are invalid". An
				// index minted on frame N can therefore name a DIFFERENT actor's
				// row -- or no written row at all -- on frame N+1. cachedFrame_
				// is the companion stamp saying which frame it was minted on.
				// This site was the ONLY one of the four consumers of that field
				// that never read the stamp. The other three all do:
				//   msl.cpp          requires it, re-gathers
				//   gos_static_prop_registry.cpp  requires it, drops the draw
				//   gos_static_prop_batcher.cpp   requires it, re-gathers
				// shaders/mech.vert and index light[] with NO bounds
				// check, so a stale or never-written row shades the mech with
				// zeroes. The pre-slice fallback was `= 0` -- a REAL slot, not a
				// sentinel -- which is exactly what made the failure silent.
				// WHY THIS PATH NO LONGER DECLINES -- MECH-DECLINE-FIX-1.
				// db221dcc2 shipped a DECLINE here on the stated ground that
				// "declining leaves gpuMechSubmitted false and the CPU MLR path
				// draws the mech at mechShape->Render(true)". THAT IS FALSE IN
				// THE CONFIGURATION WHERE THE DECLINE CAN FIRE, and the proof is
				// the enclosing gate of this very block:
				//     if (!mechGpuCullSkip && g_drawMechs
				//         && !previewContext)                     <--
				// Both surviving terms are exactly the conditions under which
				// updateGeometry picks a FAST-TRANSFORM variant for mechShape
				//: previewContext forces the full TransformMultiShape,
				// So whenever we are inside this block, mechShape was transformed
				// by _HierarchyOnly (the default: MC2_GPU_MECHS is on and
				// the modern shadow path is active) or by _PositionsOnly.
				// _HierarchyOnly delegates to
				// TransformMultiShape_BuildRecipe (msl.cpp), which sets
				// s_buildRecipeOnly, and msl.cpp then does `continue` BEFORE
				// the per-leaf dispatch -- so TG_Shape::MultiTransformShape never
				// runs, `lastTurnTransformed` is never advanced (its only writers
				// are tgl.cpp, all inside the skipped per-leaf
				// dispatchers, plus Touch() which is called on building
				// and tree shapes only, never on mechShape), and TG_Shape::Render
				// returns at the staleness guard tgl.cpp without drawing.
				// The engine already documents this twice, in-tree:
				// "The GPU fast-transform variants (_PositionsOnly /
				//        _HierarchyOnly) leave listOfVertices stale, so
				//        TG_Shape::Render early-returns and the preview goes blank."
				// (MECH-CULL-CPU-FIX) "we skipped the GPU submit and
				//        fell back to the CPU mechShape->Render() path, WHICH DOES
				//        NOT ACTUALLY DRAW while GPU mech batching owns rendering
				//        -> the mech vanished for that frame -> center-screen
				//        flicker."
				// That second one is an OBSERVED runtime bug this file already
				// fixed once. Declining here reintroduced it.
				// AND THE DECLINE WAS WORST IN ITS OWN TARGET CASE. It cannot fire
				// in steady state: Mission::update() calls clearArrays() ->
				// resetLightData() (mission.cpp), then CacheGpuLightData()
				// stamps cachedFrame_ = g_mc2FrameCounter (ungated by cull),
				// then draw_screen() runs this check -- and g_mc2FrameCounter is
				// only incremented AFTER draw_screen (gameosmain.cpp). Reset,
				// stamp and check therefore all sit in one counter epoch and the
				// stamp always matches. The stamp can only go stale when
				// Mech3DAppearance::update() does not run at render cadence, i.e.
				// the Alt+D / Ctrl+R debug toggles the original rationale named --
				// but those skip Mission::update() itself, so resetLightData() does
				// not run either, the light table is NOT rebased, and the "stale"
				// index still names its own correct row. Pre-fix those frames
				// rendered CORRECTLY; post-fix they declined and the mech vanished.
				// SO: keep the detection, drop the diversion. The freshness verdict
				// is now RECORDED and reported (per-mission, always-on, via
				// consumeAndResetMechLightRowCounters) and the submit proceeds with
				// the same index the pre-slice read resolved. If the counter comes
				// back non-zero on a supervised run, the right repair is the one
				// the original slice itself named -- re-stamp the row at update()
				// time -- not a render-time decline onto a path that does not draw.
				// The range check SURVIVES, because mech.vert index light[]
				// with no bounds check and an out-of-range read is undefined. But it
				// now CLAMPS to slot 0 instead of declining: clamping is the
				// pre-slice substitute (a defined, in-range row -- the static/baked
				// prefix under LIGHTBAKE), and unlike declining it still draws. An
				// invisible mech is a worse outcome than a mistinted one for the
				// actors the player aims at and is shot by.
				const uint32_t cachedLI = mechShape->getCachedGpuLightIndex();
				{
					const uint32_t cachedFR = mechShape->getCachedFrame();
					const bool cacheSet   = (cachedLI != 0xFFFFFFFFu);
					const bool cacheFresh = (cachedFR == g_mc2FrameCounter.load(std::memory_order_relaxed));
					// Same index the pre-slice read resolved. The freshness
					// verdict below RECORDS; it never diverts the submit.
					uint32_t resolvedLI = cacheSet ? cachedLI : 0u;
					if (!cacheSet || !cacheFresh) {
						++s_mechStaleLightRowStale;
						// FAIL-LOUD: one line per distinct mech type, uncapped
						// counter inline. Silence is what let this ship.
						static std::set<const void*> s_loggedStaleMechTypes;
						if (s_loggedStaleMechTypes.insert(static_cast<const void*>(mechType)).second) {
							mc2::fmt::print_err("[LIGHTROW_FRESHNESS v1] event=mech_stale_row_detected mechType={:016X} lod={:d} staleIndex={:d} cachedFrame={:d} currentFrame={:d} stale_so_far={:d} (prior-frame light slot; submit PROCEEDS with it -- declining would not draw, see MECH-DECLINE-FIX-1)\n", mc2::fmt::as_ptr(static_cast<void*>(mechType)), mc2::fmt::as_i32(static_cast<int>(currentLOD)), mc2::fmt::as_u32(cachedLI), mc2::fmt::as_u32(cachedFR), mc2::fmt::as_u32(static_cast<unsigned>(g_mc2FrameCounter.load(std::memory_order_relaxed))), mc2::fmt::as_u64(static_cast<unsigned long long>(s_mechStaleLightRowStale)));
							fflush(stderr);
						}
					}
					// RANGE CHECK, now applied to the resolved index on EVERY
					// path (stale included -- a stale index is exactly the one
					// most likely to be out of range after a rebase). Clamps
					// rather than declines; see the block comment above.
					if (mc2::textures::ready() &&
					    resolvedLI >= mc2::vkLights::getLightStructCount()) {
						++s_mechLightRowOutOfRange;
						static bool s_loggedMechOob = false;
						if (!s_loggedMechOob) {
							s_loggedMechOob = true;
							mc2::fmt::print_err("[LIGHTROW_FRESHNESS v1] event=mech_row_out_of_range mechType={:016X} index={:d} liveRowCount={:d} currentFrame={:d} (INVARIANT BREAK -- shader would read light[] out of bounds; CLAMPED to slot 0)\n", mc2::fmt::as_ptr(static_cast<void*>(mechType)), mc2::fmt::as_u32(resolvedLI), mc2::fmt::as_u32(mc2::vkLights::getLightStructCount()), mc2::fmt::as_u32(static_cast<unsigned>(g_mc2FrameCounter.load(std::memory_order_relaxed))));
							fflush(stderr);
						}
						resolvedLI = 0u;
					}
					desc.lightDataIndex = resolvedLI;
				}
				// Slice B+ (2026-05-09): per-actor lightsOut from object
				// status. CPU path at mech3d.cpp sets
				// mechShape->SetLightsOut(true) for these states; GPU path
				// reads bit 1 of inst.renderFlags in mech.vert and skips
				// per-light contributions, leaving only the ambient floor.
				const bool lightsOut =
					(status == OBJECT_STATUS_DESTROYED) ||
					(status == OBJECT_STATUS_DISABLED) ||
					(status == OBJECT_STATUS_SHUTDOWN);
				desc.renderFlags    = lightsOut ? 0x2u : 0x0u;  // bit 1
				if (aoValid) desc.renderFlags |= 0x10u;          // AO-1: bit 4 = HAS_AO (handle loaded)
				if (normalValid) desc.renderFlags |= 0x20u;      // NORMALS-1: bit 5 = HAS_NORMAL (handle loaded)
				desc.highlightARGB  = gpuHighlightARGB;
				// Slice B2: pack actor hazeFactor [0,1] into the alpha
				// byte of fogARGB. mech.vert combines it with
				// g_scene.fogColor.rgb to drive per-actor fog mix in
				// mech.frag. RGB of fogARGB is unused (the engine fog
				// color is global; per-actor color tinting deferred).
				const float hazeClamped = (hazeFactor < 0.0f) ? 0.0f : (hazeFactor > 1.0f ? 1.0f : hazeFactor);
				const uint8_t hazeByte  = static_cast<uint8_t>((hazeClamped * 255.0f + 0.5f));
				desc.fogARGB        = static_cast<uint32_t>(hazeByte) << 24;

				// M2.5 (Q3 unconditional): forward the M2-stored RenderWorld handle
				// to the GPU. M2 stored the handle on Mech3DAppearance::mechRenderHandle
				// via GameAdapters::Mech::registerMech. M2.5 emits the bits to
				// attachment-2 via mech.frag under MC2_OBJECT_ID_BUFFER.
				// CPU fill is UNCONDITIONAL per spec Q3: env-OFF still pays the
				// load+store (< 10 ns per instance) so instance data shape stays
				// stable; the env gate lives at the GLSL macro level. Realistic
				// cost at mc2_24 (46 mechs): < 1 us per frame.
				// Handle::invalid().raw() == 0 by definition, so any pre-register
				// frame or actor that missed registration writes 0 -- correctly
				// classified as "background" by RenderWorld::lookupAtPixel.
				desc.objectIdRaw    = getRenderWorldHandle().raw();

				// GAMEADAPTERS-VISUAL-STATE-BRIDGE-1: forward the sanitized
				// per-mech visual state pushed by BattleMech::render(). Slice 1
				// only carries it to the SSBO/debug dump; no shader consumes it
				// yet (byte-identical). damage01/flags land in GpuMechInstance;
				// heat01 rides along for the debug dump (always 0, USEHEAT off).
				{
					const RenderCore::MechVisualState& vs = getVisualState();
					desc.heat01      = vs.heat01;
					desc.damage01    = vs.damage01;
					desc.visualFlags = vs.flags;
				}

				// MECH-DECLINE-FIX-1: the light-row freshness verdict no longer
				// gates this submit. db221dcc2 skipped it on a stale row and
				// recorded GpuMechFallbackReason::StaleLightRow, on the false
				// premise that the CPU MLR draw below would pick the mech up;
				// inside this block that draw provably does not draw (see the
				// block comment at the lightDataIndex resolve above). The
				// StaleLightRow reason is therefore no longer recorded -- it can
				// never be a *fallback*, because nothing falls back. The enum
				// member is retained so the reason numbering, kFallbackNames and
				// the kGpuMechFallbackReasonCount static_assert stay stable; the
				// stale-row signal now rides the always-on per-mission
				// [MECHBATCHER v1] event=stale_light_row_summary line instead.
				{
					gpuMechSubmitted = GpuMechBatcher::instance().submitActor(desc);
					// Only count as a fallback if the GPU path was nominally
					// enabled at this point. submitActor returns false on
					// (killswitch off || !finalized || shader fail || late
					// registration). Killswitch-off and not-yet-finalized are
					// not "fallbacks" — they're "GPU path not active." Without
					// this gate, every actor in MC2_GPU_MECHS=0 mode is logged
					// as ShaderInitFailure, making the [MECHBATCHER v1]
					// event=summary fallback counters useless.
					if (!gpuMechSubmitted) {
						GpuMechBatcher::instance().recordCpuFallback(
							GpuMechBatcher::instance().wasLastFailureLateRegistration()
								? GpuMechFallbackReason::UnregisteredType
								: GpuMechFallbackReason::ShaderInitFailure);
					}
				}
			}

			// MECH-CULL-CPU-FIX: mechGpuCullSkip gates GPU submit (correct), but must
			// NOT gate the CPU fallback. readback_isActorVisibleLagged() uses lagged
			// GPU cull results; on camera-move frames the lagged readback shows some
			// center-screen mechs as invisible (frustum shifted between readback frame
			// and now), causing them to skip BOTH GPU and CPU render → visual pop.
			// CPU fallback is always safe — it renders via the established CPU path and
			// the mech is properly depth-tested / frustum-clipped by the rasterizer.
			// The GPU cull DOES still gate GpuMechBatcher submission (line above) so
			// off-screen mechs are still excluded from the GPU batcher workload.
			if (!gpuMechSubmitted && g_drawMechs) {
				// M2.5 (Q6 amendment 2): count MLR/CPU-fallback draws so
				// the always-on per-mission mlr_mech_summary line reflects
				// Path-B incidence. M2.6 readiness decision consults this
				// value (spec §12 Q6 amendment 3).
				++s_mlrMechDrawsThisMission;  // the GPU batcher is the only mech draw path; this counts submit failures
			}

			// [MECHRESTORE v1] per-actor submit discriminator. Gated +
			// globally throttled (one load needs only the first frames;
			// per-actor-per-frame in this hot path would flood). The
			// invisible-from-savegame signature is gpuMechSubmitted=0
			// cullSkip=0 finalized=0 for every actor of one type.
			{
				static const bool s_mechRestoreTrace = false;
				static int s_mechRestoreEmits = 0;
				// DIAG (GPU-MECH-EQUIV recon): emit EVERY submitted=0 actor on a
				// separate generous cap (the mech-bay preview consumes the 2000
				// success cap before the mission roster renders). frame + inView
				// let us tell persistent invisibility (same actor failing across
				// many frames, in-view) from a 1-frame spawn transient, and
				// cullSkip vs lateReg tells offscreen-cull from registration-fail.
				static int s_mechRestoreFailEmits = 0;
				const bool emitFail = (!gpuMechSubmitted && s_mechRestoreFailEmits < 20000);
				if (s_mechRestoreTrace && (emitFail || s_mechRestoreEmits < 2000)) {
					if (emitFail) ++s_mechRestoreFailEmits; else ++s_mechRestoreEmits;
					mc2::fmt::print_err("[MECHRESTORE v1] event=submit frame={:d} actor={:d} type={:016X} lod={:d} submitted={:d} cullSkip={:d} lateReg={:d} finalized={:d} status={:d} inView={:d}\n", mc2::fmt::as_u32(static_cast<unsigned>(g_mc2FrameCounter.load(std::memory_order_relaxed))), mc2::fmt::as_u32(static_cast<unsigned long>(actorHandle_)), mc2::fmt::as_ptr(static_cast<void*>(mechType)), mc2::fmt::as_i32(static_cast<int>(currentLOD)), mc2::fmt::as_i32(gpuMechSubmitted ? 1 : 0), mc2::fmt::as_i32(mechGpuCullSkip ? 1 : 0), mc2::fmt::as_i32(GpuMechBatcher::instance().wasLastFailureLateRegistration() ? 1 : 0), mc2::fmt::as_i32(GpuMechBatcher::instance().isFinalized() ? 1 : 0), mc2::fmt::as_i32(static_cast<int>(status)), mc2::fmt::as_i32(static_cast<int>(inView)));
				}
			}

			if (selected & DRAW_BARS)
			{
				drawBars();
			}
			if ( selected & DRAW_BRACKETS )
			{
				drawSelectBrackets(color);
			}
			if ((objectNameId != -1) && (selected & DRAW_TEXT) && !sensorLevel )
			{
				drawBars();

				if ( strlen( mechName ) )
				{
					drawTextHelp( mechName, color );
				}
				else
				{
					char tmpString[255];
					cLoadString(objectNameId, tmpString, 254);
					drawTextHelp(tmpString, color);
				}
				if ( strlen( pilotName ) )
				{
					drawPilotName( pilotName, color );
				}
				else if ( pilotNameID != -1 ) // only draw your own
				{
					char tmpPilotName[255];
					cLoadString( pilotNameID, tmpPilotName, 254 );
					drawPilotName( tmpPilotName, color );

				}

			}

			if (!InEditor)
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
				
				if (!isWaking && useNonWeaponEffects)
				{
					for (long i=0;i<MAX_DUST_POOFS;i++)
					{
						if (rightFootPoofDraw[i] && rightDustPoofEffect[i] && rightDustPoofEffect[i]->IsExecuted())
						{
							glm::mat4 	shapeOrigin;
							glm::mat4	localToWorld;
							
							mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
							mc2::math::buildTranslation(shapeOrigin, rFootPosition[i]);
							
							drawInfo.m_parentToWorld = &shapeOrigin;
					 
							if (!MLRVertexLimitReached)
								rightDustPoofEffect[i]->Draw(&drawInfo); if (s_fxCountLog) fxCountTick(FX_RDUST);
						}
						
						if (leftFootPoofDraw[i] && leftDustPoofEffect[i] && leftDustPoofEffect[i]->IsExecuted())
						{
							glm::mat4 	shapeOrigin;
							glm::mat4	localToWorld;
							
							mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
							mc2::math::buildTranslation(shapeOrigin, lFootPosition[i]);
							
							drawInfo.m_parentToWorld = &shapeOrigin;
					 
							if (!MLRVertexLimitReached)
								leftDustPoofEffect[i]->Draw(&drawInfo); if (s_fxCountLog) fxCountTick(FX_LDUST);
						}
					}
				}
				
				if ((unitAnim().currentGestureId == GestureJump) && unitAnim().inJump && jumpJetEffect && jumpJetEffect->IsExecuted())
				{
					long jumpNodeId = mechType->numSmokeNodes + mechType->numWeaponNodes;
					glm::vec3 jumpNodePos = getNodePosition(jumpNodeId);
					
					glm::vec3			actualPosition;
					glm::mat4 	shapeOrigin;
					glm::mat4	localToWorld;
					glm::mat4	localResult;
					
					mc2ToRender(actualPosition, snapAnchor(jumpNodePos));
					
 					mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
					mc2::math::buildTranslation(shapeOrigin, actualPosition);
					
					glm::quat effectRot;
					effectRot = mc2::math::quatFromEuler(mc2::math::Euler(JUMP_PITCH * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f));
					localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
					localResult = localToWorld * shapeOrigin;
					
 					drawInfo.m_parentToWorld = &localResult;
					if (!MLRVertexLimitReached)
						jumpJetEffect->Draw(&drawInfo); if (s_fxCountLog) fxCountTick(FX_JUMP);
				}
						
				if (leftShoulderBoom)
				{
					glm::mat4 	shapeOrigin;
					glm::mat4	localToWorld;
					glm::mat4	localResult;
							
					if (leftArmNodeIndex == -1)
						leftArmNodeIndex = mechShape->GetNodeNameId("joint_luarm");

					glm::vec3 leftNodePos = getNodeIdPosition(leftArmNodeIndex);
					mc2ToRender(leftShoulderPos, snapAnchor(leftNodePos));
		
 					mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
					mc2::math::buildTranslation(shapeOrigin, leftShoulderPos);
							
					glm::quat effectRot;
					effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,rotation * DEGREES_TO_RADS,0.0f));
					localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
					localResult = localToWorld * shapeOrigin;
					
 					drawInfo.m_parentToWorld = &localResult;
					if (!MLRVertexLimitReached)
						leftShoulderBoom->Draw(&drawInfo); if (s_fxCountLog) fxCountTick(FX_LBOOM);
				}
				
				if (rightShoulderBoom)
				{
					glm::mat4 	shapeOrigin;
					glm::mat4	localToWorld;
					glm::mat4	localResult;
							
					if (rightArmNodeIndex == -1)
						rightArmNodeIndex = mechShape->GetNodeNameId("joint_ruarm");

					glm::vec3 rightNodePos = getNodeIdPosition(rightArmNodeIndex);
					mc2ToRender(rightShoulderPos, snapAnchor(rightNodePos));
					
 					mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
					mc2::math::buildTranslation(shapeOrigin, rightShoulderPos);
							
					glm::quat effectRot;
					effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,rotation * DEGREES_TO_RADS,0.0f));
					localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
					localResult = localToWorld * shapeOrigin;
					
 					drawInfo.m_parentToWorld = &localResult;
					if (!MLRVertexLimitReached)
						rightShoulderBoom->Draw(&drawInfo); if (s_fxCountLog) fxCountTick(FX_RBOOM);
				}
				
				if (isSmoking != -1)
				{
					glm::mat4 	shapeOrigin;
					glm::mat4	localToWorld;
							
					glm::vec3 smokeNodePos = getNodePosition(0);
					glm::vec3 smokePos;
					mc2ToRender(smokePos, snapAnchor(smokeNodePos));
					
					mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
					mc2::math::buildTranslation(shapeOrigin, smokePos);
							
					drawInfo.m_parentToWorld = &shapeOrigin;
					if (isSmoking > 0)
					{
						//Loops until told to stop.
						if (!MLRVertexLimitReached)
							criticalSmoke->Draw(&drawInfo); if (s_fxCountLog) fxCountTick(FX_CSMOKE);
					}
					else
					{
						if (!MLRVertexLimitReached)
							smokeEffect->Draw(&drawInfo); if (s_fxCountLog) fxCountTick(FX_SMOKE);
					}
				}
				
				if (waterWake && isWaking && (unitAnim().currentGestureId != 20))	//Do NOT draw wake if we are jumping.  keep calcing it, though!
				{
					glm::mat4 	shapeOrigin;
					glm::mat4	localToWorld;
					glm::mat4	localResult;
							
					glm::vec3 wakePos;
					wakePos.x = -position.x;
					wakePos.y = Terrain::waterElevation;
					wakePos.z = position.y;
					
					mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
					mc2::math::buildTranslation(shapeOrigin, wakePos);
							
					glm::quat effectRot;
					effectRot = mc2::math::quatFromEuler(mc2::math::Euler(90.0f * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f));

					if (!unitAnim().inReverse)
						localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
					else
						localToWorld = mc2::math::multiply(gosFX::Effect_Into_Motion, effectRot);

					localResult = localToWorld * shapeOrigin;
		
 					drawInfo.m_parentToWorld = &localResult;
					if (!MLRVertexLimitReached)
						waterWake->Draw(&drawInfo); if (s_fxCountLog) fxCountTick(FX_WAKE);
				}
				
				if (isDusting && helicopterDustCloud)
				{
					glm::mat4 	shapeOrigin;
					glm::mat4	localToWorld;
					glm::mat4	localResult;
							
					glm::vec3 wakePos;
					mc2ToRender(wakePos, position);
					
					mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
					mc2::math::buildTranslation(shapeOrigin, wakePos);
							
		
 					drawInfo.m_parentToWorld = &shapeOrigin;
					if (!MLRVertexLimitReached)
						helicopterDustCloud->Draw(&drawInfo); if (s_fxCountLog) fxCountTick(FX_HELIDUST);
				}
			}
				   
		}
		
		if ((sensorLevel > 0) && (sensorLevel < 5))
		{
			if (sensorLevel > 1)
			{
				sensorSquareShape->Render(eye->getViewUniforms());
			}
			else
			{
				sensorTriangleShape->Render(eye->getViewUniforms());
			}
		}
	}

	if (unitAnim().rightArmOff && rightArm && rightArmInView)
	{
		Camera::HazeFactor = rightArmHazeFactor;

		rightArm->Render(eye->getViewUniforms(),true);

		if (rightArmSmoke)
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
					
			mc2ToRender(rightShoulderPos, rightArmPos);
			
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, rightShoulderPos);
					
			glm::quat effectRot;
			effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,rotation * DEGREES_TO_RADS,0.0f));
			localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
			localResult = localToWorld * shapeOrigin;
			
			drawInfo.m_parentToWorld = &localResult;
			if (!MLRVertexLimitReached)
				rightArmSmoke->Draw(&drawInfo); if (s_fxCountLog) fxCountTick(FX_RARMSMOKE);
		}
	}
 	
 	if (unitAnim().leftArmOff && leftArm && leftArmInView)
	{
		Camera::HazeFactor = leftArmHazeFactor;

		leftArm->Render(eye->getViewUniforms(),true);
		
		if (leftArmSmoke)
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
					
			mc2ToRender(leftShoulderPos, leftArmPos);
			
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, leftShoulderPos);
					
			glm::quat effectRot;
			effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,rotation * DEGREES_TO_RADS,0.0f));
			localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
			localResult = localToWorld * shapeOrigin;
			
			drawInfo.m_parentToWorld = &localResult;
			if (!MLRVertexLimitReached)
				leftArmSmoke->Draw(&drawInfo); if (s_fxCountLog) fxCountTick(FX_LARMSMOKE);
		}
	}
	
 	return NO_ERR;
}

TG_MultiShape* Mech3DAppearance::previewShape (void)
{
	if (!mechShape)
		return nullptr;
	mechShape->SetTextureHandle(0,localTextureHandle);
	mechShape->SetARGBHighLight(highlightColor);
	return mechShape;
}

bool Mech3DAppearance::submitPreviewGpu (bool freshSession)
{
	if (!mechShape || !mechType)
		return false;
	auto& batcher = GpuMechBatcher::instance();
	// The lab has no map load. A new lab session drops the type records left by the last mission (onMapUnload keeps
	// them, and their types are freed with the mission) exactly as the next mission's onMapLoad would; then register
	// this type (idempotent) and build or extend the shared geometry.
	if (freshSession)
		batcher.onMapLoad();
	batcher.registerTypeLod(mechType, static_cast<int>(currentLOD));
	if (!batcher.isFinalized())
		batcher.finalizeGeometry();
	else
		batcher.finalizePending();
	if (!batcher.isFinalized())
		return false;
	mechShape->SetTextureHandle(0, localTextureHandle);
	GpuMechSubmitDesc desc{};
	desc.mechShape      = mechShape;
	desc.mechType       = mechType;
	desc.currentLOD     = static_cast<int>(currentLOD);
	desc.slot0TexHandle = static_cast<uint32_t>(localTextureHandle);
	desc.slot6TexHandle = aoValid ? static_cast<uint32_t>(aoTextureHandle) : 0u;
	desc.slot7TexHandle = normalValid ? static_cast<uint32_t>(normalTextureHandle) : 0u;
	desc.lightDataIndex = 0u;	// the lab posts its own single light row
	desc.renderFlags    = 0u;
	if (aoValid) desc.renderFlags |= 0x10u;
	if (normalValid) desc.renderFlags |= 0x20u;
	desc.highlightARGB  = static_cast<uint32_t>(highlightColor);
	desc.fogARGB        = 0u;
	desc.objectIdRaw    = 0u;
	return batcher.submitActor(desc);
}

long Mech3DAppearance::renderShadows (void)
{
	// The CPU blob shadows are gone: shadows come from the modern shadow path.
	return NO_ERR;
}
