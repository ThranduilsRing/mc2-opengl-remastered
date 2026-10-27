// GVAppearance: shadow and body rendering (reads the vehicle-lit-path gate).
// Declared in the class headers; shared names in gvactor_internal.h.
#include "gvactor_internal.h"
#include "Gates/gate.h"
#include "../RenderCore/axis_convert.h"


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
	TG_MultiShape* drawShape = renderShape ? renderShape.get() : gvShape;
	const auto visibility = buildVisState();
	mc2_vis::trace("vehicle-shadow", static_cast<const void*>(this), visibility);
	// Sensor contacts keep visible=true while the body fades to alpha zero.
	// Retain offscreen casters, but never reveal a fog-hidden body through its shadow.
	if (visible && mc2_vis::meshDrawn(visibility) && gos_IsModernShadowPathActive())
		GpuStaticPropBatcher::instance().captureMoverShadowShape(static_cast<const Appearance*>(this),
			// An override body is imported after the batcher finalized (late registerType), so it has no recipe;
			// the stock shape is registered at mission load and carries the same pose, so it casts the shadow.
			renderShape ? gvShape : drawShape);
	if (!renderShape) drawShape->SetTextureHandle(0,localTextureHandle);

	// C3: render gate — GPU-lagged visibility when killswitch is enabled.
	const bool gvShouldRender = s_gpuCullLifecycle
		? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_))
		: inView;
	if (gvShouldRender)
	{
		uint32_t color = SD_BLUE;
		uint32_t highLight = 0x007f7f7f;
		if ((teamId > -1) && (teamId < 8)) {
			static uint32_t highLightTable[3] = {0x00007f00, 0x0000007f, 0x007f0000};
			static uint32_t colorTable[3] = {SB_GREEN | 0xff000000, SB_BLUE| 0xff000000, SB_RED | 0xff000000};
			color = colorTable[homeTeamRelationship];
			highLight = highLightTable[homeTeamRelationship];
		}

		if (visible)
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
					drawShape->CacheGpuLightDataAllLeaves();
				renderShape->SetTeamPaint(psRed, psGreen, psBlue);
				static int s_paintLogs = 0;
				if (s_paintLogs < 6 && ++s_paintLogs) mc2::fmt::print_err("[VEHICLE_PAINT] {} lod={} team={} paint={:08x}/{:08x}/{:08x}\n", appearType->name, renderLOD, teamId, psRed, psGreen, psBlue);
				submitted = GpuStaticPropBatcher::instance().submitMultiShape(
					drawShape, GpuStaticPropPopulation::Generic, appearType->name);
			}
			else if (gpuPose_) {
				// Stock body: node matrices, the light row, paint texture and haze go to the mech batcher.
				if (s_vehicleLitPath)
					drawShape->CacheGpuLightData();
				uint32_t gpuHighlight = (selected & DRAW_COLORED && duration <= 0) ? highLight : static_cast<uint32_t>(highlightColor);
				if (drawFlash)
					gpuHighlight = static_cast<uint32_t>(flashColor);
				submitted = submitToMechBatcher(gpuHighlight);
			}
			if (!submitted) {
				// Declined (or never on the GPU path): the legacy draw needs the CPU-transformed vertices.
				if (gpuPose_) drawShape->TransformMultiShape(&poseXlat_, &poseRot_);
				if (s_vehicleLitPath)
					drawShape->CacheGpuLightDataAllLeaves();
				drawShape->Render(true);
			}

			if (selected & DRAW_TEXT)
			{
				drawShape->SetARGBHighLight(highLight);
			}
			else
			{
				drawShape->SetARGBHighLight(0x0);
			}

				
			if (selected & DRAW_BARS)
			{
				drawBars();
			}

			if ( selected & DRAW_BRACKETS )
			{
				drawSelectBrackets(color);
			}

			if ( selected & DRAW_TEXT && !sensorLevel )
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
				if (waterWake && isWaking)
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
							
					glm::vec3 wakePos;
					wakePos.x = -position.x;
					wakePos.y = Terrain::waterElevation;
					wakePos.z = position.y;
					
					mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
					mc2::math::buildTranslation(shapeOrigin, wakePos);
							
					glm::quat effectRot;
					effectRot = mc2::math::quatFromEuler(mc2::math::Euler(90.0f * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f));
					localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
					localResult = localToWorld * shapeOrigin;
		
					drawInfo.m_parentToWorld = &localResult;
					if (!MLRVertexLimitReached)
						waterWake->Draw(&drawInfo);
				}
				else if (dustCloud)
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
					mc2ToRender(wakePos, dustPos);
					
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
					mc2ToRender(wakePos, dustPos);
					
					mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
					mc2::math::buildTranslation(shapeOrigin, wakePos);
					
		
					drawInfo.m_parentToWorld = &shapeOrigin;
					if (!MLRVertexLimitReached)
						activity->Draw(&drawInfo);
				}

			}
		}
		
		if ((sensorLevel > 0) && (sensorLevel < 5))
		{
			transformSensorShapes();
			if (sensorLevel > 1)
			{
				sensorCircleShape->Render();
			}
			else
			{
				sensorTriangleShape->Render();
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
	if (localTextureHandle == 0xffffffff) return false;
	GpuMechSubmitDesc desc{};
	desc.mechShape = gvShape;
	desc.mechType = nullptr;
	desc.slot0TexHandle = static_cast<uint32_t>(localTextureHandle);
	const uint32_t lightRow = gvShape->getCachedGpuLightIndex();
	if (lightRow == 0xFFFFFFFFu || (mc2::textures::ready() && lightRow >= mc2::vkLights::getLightStructCount())) return false;
	desc.lightDataIndex = lightRow;
	desc.highlightARGB = highlightARGB;
	const float haze = std::clamp(std::max(hazeFactor, poseFog_), 0.0f, 1.0f);
	desc.fogARGB = static_cast<uint32_t>(haze * 255.0f + 0.5f) << 24;
	return GpuMechBatcher::instance().submitActor(desc);
}
