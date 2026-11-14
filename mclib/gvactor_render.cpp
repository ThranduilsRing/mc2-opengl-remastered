// GVAppearance: shadow and body rendering (reads the vehicle-lit-path gate).
// Declared in the class headers; shared names in gvactor_internal.h.
#include <atomic>
#include <array>
#include "gvactor_internal.h"
#include "Gates/gate.h"
#include "prop_light_contract.h"
#include "../RenderCore/axis_convert.h"
#include "../RenderCore/water_reactive.h"

extern std::atomic<uint32_t> g_mc2FrameCounter;


// Vehicle shadows are cast by the shadow cascades from the same per-node matrices the body draws from; the old
// projected blob shadow (gvShadowShape) is gone.
long GVAppearance::renderShadows (void)
{
	return NO_ERR;
}

// VEHICLE-LIGHTING-PATH-1 killswitch (default ON). Gates the
// CacheGpuLightData() call in GVAppearance::render that finally populates the
// vehicle light row. MC2_VEHICLE_LIT_PATH=0 restores the shipped behavior --
// an all-zero row -> calc_light numLights==0 early-return -> vec3(1,1,1) ->
// raw albedo, i.e. byte-identical to the unlit vehicles users have been
// reporting. See the long root-cause note at the call site.
static const bool s_vehicleLitPath = mc2::gate::get<"MC2_VEHICLE_LIT_PATH">();

long GVAppearance::render (long depthFixup)
{
	if (!eye) return 0;
	TG_MultiShape* drawShape = renderShape ? renderShape.get() : gvShape;
	const auto visibility = buildVisState();
	mc2_vis::trace("vehicle-shadow", static_cast<const void*>(this), visibility);
	// Sensor contacts keep visible=true while the body fades to alpha zero.
	// Retain offscreen casters, but never reveal a fog-hidden body through its shadow.
	if (visible && contactView.shadow && gos_IsModernShadowPathActive())
		GpuStaticPropBatcher::instance().captureMoverShadowShape(static_cast<const Appearance*>(this),
			// An override body is imported after the batcher finalized (late registerType), so it has no recipe;
			// the stock shape is registered at mission load and carries the same pose, so it casts the shadow.
			renderShape ? gvShape : drawShape);
	if (!renderShape) drawShape->SetTextureHandle(0,localTextureHandle);

	// C3: render gate — GPU-lagged visibility when killswitch is enabled.
	const bool gvShouldRender = s_gpuCullLifecycle
		? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_))
		: inView;
	// The render gate (GPU-lagged when the cull lifecycle is on), not the CPU inView, says whether the body is drawn.
	mc2::waterreactive::buffer().submit(reinterpret_cast<uintptr_t>(this), scenarioTime,
		position.x, position.y, getRadius() * 0.35f, presentedAnimation().wakeRequested,
		visible && gvShouldRender && mc2_vis::meshDrawn(visibility), true);
	if (gvShouldRender)
	{
		uint32_t color = SD_BLUE;
		uint32_t highLight = 0x007f7f7f;
		if ((teamId > -1) && (teamId < 8)) {
			static std::array<uint32_t, 3> highLightTable = {0x00007f00, 0x0000007f, 0x007f0000};
			static std::array<uint32_t, 3> colorTable = {SB_GREEN | 0xff000000, SB_BLUE| 0xff000000, SB_RED | 0xff000000};
			color = colorTable[homeTeamRelationship];
			highLight = highLightTable[homeTeamRelationship];
		}

		if (visible && contactView.model)
		{
			if (selected & DRAW_COLORED && duration <= 0 )
			{
				drawShape->SetARGBHighLight(highLight);
			}
			else
			{
				drawShape->SetARGBHighLight(highlightColor);
			}

			Camera::HazeFactor = hazeFactor;

			if (drawFlash)
			{
				drawShape->SetARGBHighLight(flashColor);
			}

			//---------------------------------------------
			// Call Multi-shape render stuff here.
			// Force textures to reload due to unique instance.

			// VEHICLE-LIGHTING-PATH-1 (default ON, MC2_VEHICLE_LIT_PATH=0 to
			// revert): populate this actor's light row before the draw.
			//
			// ROOT CAUSE. Every other object class populates lightData_ via
			// CacheGpuLightData() before drawing -- mech3d.cpp,
			// bdactor.cpp/:3181/:4821 (bldg), :6566 (tree),
			// genactor.cpp (generic prop). GVAppearance called NEITHER
			// CacheGpuLightData() NOR GpuStaticPropBatcher::submitMultiShape(),
			// yet renders unconditionally through the legacy TG_Shape::Render
			// path, which consumes lightData_ at tgl.cpp with no gather
			// and no eligibility guard. (Buildings reach that same legacy
			// Render only as an `if (!submittedToGpu)` fallback --
			// bdactor.cpp -- which they normally never take.)
			//
			// Consequence chain, all verified from source:
			//   1. TG_HWLightsData() zero-inits numLights_        (tgl.h)
			//   2. addLightDataStructure is pure dedup/append with NO
			//      back-fill                              (txmmgr.cpp)
			//   3. calc_light takes its numLights.x==0 early-return; because
			//      MC2_STATIC_PROP_LIGHTING is NOT defined for this shader
			//      (only mech.vert:15 / static_prop.vert:20) it returns
			//      vec3(1,1,1), discarding even the vertex color
			//                              (lighting.hglsl:232-236)
			//   4. ENABLE_VERTEX_LIGHTING==1 globally    (lighting.hglsl:8)
			//   5. gos_tex_vertex_lighted.frag: c.xyz = c.xyz * lighting
			//  => vehicles rendered at RAW ALBEDO x 1.0, fully unlit.
			//
			// Mechs and buildings get ambient + sun*ndl, which exceeds 1.0 in
			// daylight. THAT is why a vehicle texture measured at the 49th
			// percentile of the corpus (trucksprop.tga 85.9 vs median 86.2)
			// looks dark parked next to a mech. The albedo theory was
			// correctly retracted in fc43338a; this reconciles the
			// measurement with the observation. Do not reopen it.
			//
			// This call routes vehicles into the SAME full 6-type calc_light
			// (ambient/infinite/falloff/point/spot) that mech.vert:219 uses --
			// a strict superset of the building path, which deliberately omits
			// point/spot (static_prop.vert:713).
			// ALL leaves, not just the first: TG_MultiShape::Render loops every
			// leaf and each TG_Shape::Render submits its OWN lightData_
			// (tgl.cpp). CacheGpuLightData() fills only the first leaf
			// (correct for the batcher, which broadcasts one index) and left
			// every other leaf at numLights_==0 -> raw albedo. Verified: the
			// first-leaf-only form produced ZERO pixel change at deterministic
			// A/B poses even though the CPU gather was demonstrably running
			// (MC2_LIGHT_ROW_CENSUS fired ON, silent OFF).
			bool submitted = false;
			if (renderShape) {
				if (s_vehicleLitPath)
					drawShape->CacheGpuLightDataAllLeaves(eye->lightContext());
				renderShape->SetTeamPaint(psRed, psGreen, psBlue);
				static int s_paintLogs = 0;
				if (s_paintLogs < 6 && ++s_paintLogs) mc2::fmt::print_err("[VEHICLE_PAINT] {} lod={} team={} paint={:08x}/{:08x}/{:08x}\n", appearType->name, renderLOD, teamId, psRed, psGreen, psBlue);
				submitted = GpuStaticPropBatcher::instance().submitMultiShape(
					drawShape, GpuStaticPropPopulation::Generic, eye->lightContext(), appearType->name,
					mc2::prop_light::packWord(land && eye ? land->getTerrainLight(position) : 1.0f, 0));
			}
			else if (gpuPose_) {
				// Stock body: node matrices, the light row, paint texture and haze go to the mech batcher.
				uint32_t gpuHighlight = (selected & DRAW_COLORED && duration <= 0) ? highLight : static_cast<uint32_t>(highlightColor);
				if (drawFlash)
					gpuHighlight = static_cast<uint32_t>(flashColor);
				submitted = submitToMechBatcher(gpuHighlight);
			}
			if (!submitted) {
				// Declined (fading alpha, a type no batcher holds): CPU fallback from the update-time hierarchy, lit by this
				// actor's own terrain light colour.
				eye->setLightColor(0, poseLightRGB_);
				eye->setLightIntensity(0, 1.0);
				const Mc2LegacyVehicleScope inventory(drawShape->GetAlphaValue() != 0xff ? MC2_LEGACY_VEHICLE_FADING :
					!GpuMechBatcher::instance().isVehicleTypeRegistered(drawShape->GetMultiType()) ? MC2_LEGACY_VEHICLE_UNREGISTERED : MC2_LEGACY_VEHICLE_DECLINED);
				drawShape->Render(eye->getViewUniforms(),true);
			}
			else if (gpuPose_ && !renderShape && eye->isNight) {
				// Searchlight cones: no batcher draws them (cone glow adds the beam light, not this geometry).
				drawShape->Render(eye->getViewUniforms(),true, -1.0f, true);
			}

			if (selected & DRAW_TEXT)
			{
				drawShape->SetARGBHighLight(highLight);
			}
			else
			{
				drawShape->SetARGBHighLight(0x0);
			}

				
			if (contactView.bar && (selected & DRAW_BARS))
			{
				drawBars();
			}

			if (contactView.model && (selected & DRAW_BRACKETS))
			{
				drawSelectBrackets(color);
			}

			if (contactView.tag && (selected & DRAW_TEXT))
			{
 
				if (objectNameId != -1)
				{
					char tmpString[255];
					cLoadString(objectNameId, tmpString, 254);					
					drawTextHelp(tmpString, color);
					
				}
				if ( strlen( pilotName ) )
				{
					drawPilotName( pilotName, color );
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
				
				mc2ToRender(tPosition, position);
				
				mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
				mc2::math::buildTranslation(shapeOrigin, tPosition);
				
				drawInfo.m_parentToWorld = &shapeOrigin;
				
				if (!MLRVertexLimitReached)
					destructFX->Draw(&drawInfo);
			}
			
			if (!sensorLevel)
			{
				if (!presentedAnimation().wakeRequested && dustCloud)
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
							
					if (dustNodeIndex == -1)
						dustNodeIndex = gvShape->GetNodeNameId("dust_body");

					glm::vec3 dustPos = getNodeIdPosition(dustNodeIndex);

					glm::vec3 wakePos;
					mc2ToRender(wakePos, snapAnchor(dustPos));
					
					mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
					mc2::math::buildTranslation(shapeOrigin, wakePos);
							
					glm::quat effectRot;
					effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,rotation * DEGREES_TO_RADS,0.0f));
					localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
					localResult = localToWorld * shapeOrigin;
		
					drawInfo.m_parentToWorld = &localResult;
					if (!MLRVertexLimitReached)
						dustCloud->Draw(&drawInfo);
				}
				
				if (activity && isActivitying)
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
							
					if (activityNodeIndex == -1)
						activityNodeIndex = gvShape->GetNodeNameId("activity_node");

					glm::vec3 dustPos = getNodeIdPosition(activityNodeIndex);
					glm::vec3 wakePos;
					mc2ToRender(wakePos, snapAnchor(dustPos));
					
					mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
					mc2::math::buildTranslation(shapeOrigin, wakePos);
					
		
					drawInfo.m_parentToWorld = &shapeOrigin;
					if (!MLRVertexLimitReached)
						activity->Draw(&drawInfo);
				}

			}
		}
		
		if (contactView.blip && !contactView.model)
		{
			transformSensorShapes();
			MC2_LEGACY_SITE("vehicle_sensor_blip");
			if (sensorLevel > 1)
			{
				sensorCircleShape->Render(eye->getViewUniforms());
			}
			else
			{
				sensorTriangleShape->Render(eye->getViewUniforms());
			}
		}
	}
	return NO_ERR;
}

// Hand the stock body to the mech batcher: it reads the node matrices only; the paint texture, light row and
// haze ride the instance. False declines (no paint texture, light row missing or out of range): the caller
// draws on the CPU.
bool GVAppearance::submitToMechBatcher(uint32_t highlightARGB)
{
	// Decline counters by reason; one summary line per mission (the frame counter restarts per mission).
	static uint32_t s_ok = 0, s_paint = 0, s_row = 0, s_range = 0, s_batcher = 0, s_lastFrame = 0;
	static bool s_logged = false;
	if (g_mc2FrameCounter.load(std::memory_order_relaxed) < s_lastFrame) { s_ok = s_paint = s_row = s_range = s_batcher = 0; s_logged = false; }
	s_lastFrame = g_mc2FrameCounter.load(std::memory_order_relaxed);
	if (!s_logged && g_mc2FrameCounter.load(std::memory_order_relaxed) >= 600)
	{
		s_logged = true;
		mc2::fmt::print_err("[GV_SUBMIT] frame={:d} ok={:d} decline_paint={:d} decline_row={:d} decline_range={:d} decline_batcher={:d}\n", mc2::fmt::as_u32(g_mc2FrameCounter.load(std::memory_order_relaxed)), mc2::fmt::as_u32(s_ok), mc2::fmt::as_u32(s_paint), mc2::fmt::as_u32(s_row), mc2::fmt::as_u32(s_range), mc2::fmt::as_u32(s_batcher));
	}
	if (localTextureHandle == 0xffffffff) { ++s_paint; return false; }
	GpuMechSubmitDesc desc{};
	desc.mechShape = gvShape;
	desc.mechType = nullptr;
	desc.renderFlags = kMechRenderFlagLegacyVehicle;	// legacy vehicle lighting: sun and ambient, no point or spot lights
	desc.slot0TexHandle = static_cast<uint32_t>(localTextureHandle);
	if (stockPbr.orm) {
		desc.slot6TexHandle = stockPbr.orm.get();
		desc.renderFlags |= kMechRenderFlagOrm | 0x10u;
	}
	if (stockPbr.normal) {
		desc.slot7TexHandle = stockPbr.normal.get();
		desc.renderFlags |= 0x20u;
	}
	const uint32_t lightRow = gvShape->getCachedGpuLightIndex();
	if (lightRow == 0xFFFFFFFFu) { ++s_row; return false; }
	if (mc2::textures::ready() && lightRow >= mc2::vkLights::getLightStructCount()) { ++s_range; return false; }
	// The row was gathered at update time; update and render can land on different frames, so stamp it fresh here.
	gvShape->setCachedFrame(g_mc2FrameCounter.load(std::memory_order_relaxed));
	desc.lightDataIndex = lightRow;
	desc.highlightARGB = highlightARGB;
	const float haze = std::clamp(std::max(hazeFactor, poseFog_), 0.0f, 1.0f);
	desc.fogARGB = static_cast<uint32_t>(haze * 255.0f + 0.5f) << 24;
	const bool ok = GpuMechBatcher::instance().submitActor(desc);
	++(ok ? s_ok : s_batcher);
	return ok;
}
