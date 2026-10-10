// GVAppearance: shadow and body rendering (reads the vehicle-lit-path gate).
// Declared in the class headers; shared names in gvactor_internal.h.
#include "gvactor_internal.h"
#include "Gates/gate.h"


//-----------------------------------------------------------------------------
long GVAppearance::renderShadows (void)
{
	// Skip legacy blob shadows when shadow maps are active
	// ISSUE-34: keyed on the modern shadow path, not tessellation (see
	// mech3d.cpp renderShadows).
	if (gos_IsModernShadowPathActive())
		return NO_ERR;

	gvShape->SetTextureHandle(0,localTextureHandle);
	
	// C3: route renderShadows gate to GPU-lagged visibility when killswitch is enabled.
	const bool gvShadowVisible = s_gpuCullLifecycle
		? (gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_)) && visible)
		: (inView && visible);
	if (gvShadowVisible)
	{
		//---------------------------------------------
		// Call Multi-shape render stuff here.
		if (gvShadowShape)
			gvShadowShape->RenderShadows(true);
		else
			gvShape->RenderShadows(true);
	}
	return NO_ERR;
}

// VEHICLE-LIGHTING-PATH-1 killswitch (default ON). Gates the
// CacheGpuLightData() call in GVAppearance::render that finally populates the
// vehicle light row. MC2_VEHICLE_LIT_PATH=0 restores the shipped behavior --
// an all-zero row -> calc_light numLights==0 early-return -> vec3(1,1,1) ->
// raw albedo, i.e. byte-identical to the unlit vehicles users have been
// reporting. See the long root-cause note at the call site.
static const bool s_vehicleLitPath = mc2::gate::get<"MC2_VEHICLE_LIT_PATH">();

//-----------------------------------------------------------------------------
long GVAppearance::render (long depthFixup)
{
	gvShape->SetTextureHandle(0,localTextureHandle);

	// C3: render gate — GPU-lagged visibility when killswitch is enabled.
	// Preserve g_useGpuStaticProps fallback for static-prop path.
	const bool gvShouldRender = s_gpuCullLifecycle
		? (gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_)) || g_useGpuStaticProps)
		: (inView || g_useGpuStaticProps);
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
				gvShape->SetARGBHighLight(highLight);
			}
			else
			{
				gvShape->SetARGBHighLight(highlightColor);
			}

			Camera::HazeFactor = hazeFactor;

			if (drawFlash)
			{
				gvShape->SetARGBHighLight(flashColor);
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
			if (s_vehicleLitPath)
				gvShape->CacheGpuLightDataAllLeaves();

			gvShape->Render(true);

			if (selected & DRAW_TEXT)
			{
				gvShape->SetARGBHighLight(highLight);
			}
			else
			{
				gvShape->SetARGBHighLight(0x0);
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
		
			
			//selected = FALSE;
			
			//------------------------------------------
			// Render GOS FX if necessary
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
	
				Stuff::LinearMatrix4D 	shapeOrigin;
				Stuff::LinearMatrix4D	localToWorld;
				Stuff::Point3D			tPosition;
				
				tPosition.x = -position.x;
				tPosition.y = position.z;
				tPosition.z = position.y;
				
				shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
				shapeOrigin.BuildTranslation(tPosition);
				
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

					Stuff::LinearMatrix4D 	shapeOrigin;
					Stuff::LinearMatrix4D	localToWorld;
					Stuff::LinearMatrix4D	localResult;
							
					Stuff::Point3D wakePos;
					wakePos.x = -position.x;
					wakePos.y = Terrain::waterElevation;
					wakePos.z = position.y;
					
					shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
					shapeOrigin.BuildTranslation(wakePos);
							
					Stuff::UnitQuaternion effectRot;
					effectRot = Stuff::EulerAngles(90.0f * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f);
					localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
					localResult.Multiply(localToWorld,shapeOrigin);
		
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

					Stuff::LinearMatrix4D 	shapeOrigin;
					Stuff::LinearMatrix4D	localToWorld;
					Stuff::LinearMatrix4D	localResult;
							
					if (dustNodeIndex == -1)
						dustNodeIndex = gvShape->GetNodeNameId("dust_body");

					Stuff::Vector3D dustPos = getNodeIdPosition(dustNodeIndex);

					Stuff::Point3D wakePos;
					wakePos.x = -dustPos.x;
					wakePos.y = dustPos.z;
					wakePos.z = dustPos.y;
					
					shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
					shapeOrigin.BuildTranslation(wakePos);
							
					Stuff::UnitQuaternion effectRot;
					effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
					localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
					localResult.Multiply(localToWorld,shapeOrigin);
		
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

					Stuff::LinearMatrix4D 	shapeOrigin;
					Stuff::LinearMatrix4D	localToWorld;
					Stuff::LinearMatrix4D	localResult;
							
					if (activityNodeIndex == -1)
						activityNodeIndex = gvShape->GetNodeNameId("activity_node");

					Stuff::Vector3D dustPos = getNodeIdPosition(activityNodeIndex);
					Stuff::Point3D wakePos;
					wakePos.x = -dustPos.x;
					wakePos.y = dustPos.z;
					wakePos.z = dustPos.y;
					
					shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
					shapeOrigin.BuildTranslation(wakePos);
					
					/*		
					Stuff::UnitQuaternion effectRot;
					effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
					localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
					localResult.Multiply(localToWorld,shapeOrigin);
					*/
		
					drawInfo.m_parentToWorld = &shapeOrigin;
					if (!MLRVertexLimitReached)
						activity->Draw(&drawInfo);
				}

			}
		}
		
		if ((sensorLevel > 0) && (sensorLevel < 5))
		{
			//---------------------------------------
			// Draw Sensor Contact here.
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
