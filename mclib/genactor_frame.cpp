// GenericAppearance: render, renderShadows, update and the appearance teardown.
// Declared in the class headers; shared names in genactor_internal.h.
#include "genactor_internal.h"
#include "sim_overlap.h"


//-----------------------------------------------------------------------------
long GenericAppearance::render (long depthFixup)
{
	if (inView)
	{
		uint32_t color = SD_BLUE;
		unsigned long highLight = 0x007f7f7f;
		if ((teamId > -1) && (teamId < 8)) {
			static uint32_t highLightTable[3] = {0x00007f00, 0x0000007f, 0x007f0000};
			static uint32_t colorTable[3] = {SB_GREEN | 0xff000000, SB_BLUE | 0xff000000, SB_RED | 0xff000000};
			color = colorTable[homeTeamRelationship];
			highLight = highLightTable[homeTeamRelationship];
		}
		if (selected & DRAW_COLORED)
		{
			genShape->SetARGBHighLight(highLight);
		}
		else
		{
			genShape->SetARGBHighLight(highlightColor);
		}

		//---------------------------------------------
		// Call Multi-shape render stuff here.
		if (visible)
		{
			// Slice 1 path (g_useGpuObjects). Same shape as BldgAppearance::render.
			bool submittedToGpu = false;
			{
				GpuStaticPropBatcher::instance().recordEligibleActor(
					GpuStaticPropPopulation::Generic);
				if (genShape)
				{
					// Stage 2.C+: see BldgAppearance::render for callerName intent.
					const char* callerName = (appearType ? appearType->name : nullptr);
					submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(
						genShape, GpuStaticPropPopulation::Generic, callerName);
					// Slice 2 (object-offload) — Stage 2.B: see BldgAppearance::render
					// for full rationale on the late-reg recovery flag.
					if (!submittedToGpu &&
					    GpuStaticPropBatcher::instance().wasLastFailureLateRegistration())
					{
						needsFullBakeNextFrame = true;
					}
				}
				if (!submittedToGpu)
				{
					GpuStaticPropBatcher::instance().recordCpuFallback(
						GpuStaticPropPopulation::Generic);
				}
			}
			if (!submittedToGpu)
			{
				if (depthFixup)
					genShape->Render(false,0.99999f);	//Sky or something like it.  Push to back!
				else
					genShape->Render();
			}
		}
		else
		{
			gos_VERTEX planetPoint;

			planetPoint.x = screenPos.x;
			planetPoint.y = screenPos.y;
			planetPoint.z = screenPos.z;
			planetPoint.rhw = screenPos.w;
			planetPoint.argb = appearType->dotRGB;
			planetPoint.frgb = 0xff000000;
			planetPoint.u = planetPoint.v = 0.0;
	
			gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
			gos_SetRenderState( gos_State_ShadeMode, gos_ShadeFlat);
			gos_SetRenderState( gos_State_MonoEnable, 0);
			gos_SetRenderState( gos_State_Perspective, 1);
			gos_SetRenderState( gos_State_Clipping, 2);
			gos_SetRenderState( gos_State_AlphaTest, 0);
			gos_SetRenderState( gos_State_Specular, 0);
			gos_SetRenderState( gos_State_Dither, 0);
			gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendDecal);
			gos_SetRenderState( gos_State_Filter, gos_FilterNone);
			gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
			gos_SetRenderState( gos_State_ZCompare, 1);
			gos_SetRenderState(	gos_State_ZWrite, 0);
			gos_SetRenderState(gos_State_Texture,0);
	
			gos_DrawPoints(&planetPoint,1);
		}

		if (selected & DRAW_BARS)
		{
			drawBars();
		}

		if ( selected & DRAW_BRACKETS )
			drawSelectBrackets(color);
			
		if (selected & DRAW_TEXT && objectNameId != -1)
		{	
			char tmpString[255];
			cLoadString(objectNameId, tmpString, 254);

			drawTextHelp(tmpString, color);
	
		}
		
//		selected = FALSE;
//#define DRAW_BOX
#ifdef DRAW_BOX
		//---------------------------------------------------------
		// Render the Bounding Box to see if it is OK.
		glm::vec3 nodeCenter = bldgShape->GetRootNodeCenter();
		glm::vec3 boxStart;
		glm::vec3 boxEnd;
		boxStart.x = -(bldgShape->GetMinBox().x + nodeCenter.x);
		boxStart.z = bldgShape->GetMinBox().y + nodeCenter.y;
		boxStart.y = bldgShape->GetMinBox().z + nodeCenter.z;
		
		boxEnd.x = -(bldgShape->GetMaxBox().x + nodeCenter.x);
		boxEnd.z = bldgShape->GetMaxBox().y + nodeCenter.y;
		boxEnd.y = bldgShape->GetMaxBox().z + nodeCenter.z;
		
 		glm::vec3 boxCoords[8];
		glm::vec3 addCoords;
		
		addCoords.x = boxStart.x;
		addCoords.y = boxStart.y;
		addCoords.z = boxEnd.z;
		if (rotation != 0.0f)
			Rotate(addCoords,-rotation);
 		
		boxCoords[0].Add(position,addCoords);

		addCoords.x = boxStart.x;
		addCoords.y = boxEnd.y;  
		addCoords.z = boxEnd.z;  		
		if (rotation != 0.0f)
			Rotate(addCoords,-rotation);
 		
		boxCoords[1].Add(position,addCoords);

 		addCoords.x = boxEnd.x; 
		addCoords.y = boxEnd.y; 
		addCoords.z = boxEnd.z; 		
		if (rotation != 0.0f)
			Rotate(addCoords,-rotation);
 		
		boxCoords[2].Add(position,addCoords);
		
 		addCoords.x = boxEnd.x;   
		addCoords.y = boxStart.y; 
		addCoords.z = boxEnd.z;   		
		if (rotation != 0.0f)
			Rotate(addCoords,-rotation);
 		
		boxCoords[3].Add(position,addCoords);
		
 		addCoords.x = boxStart.x;
		addCoords.y = boxStart.y; 
		addCoords.z = boxStart.z; 		
		if (rotation != 0.0f)
			Rotate(addCoords,-rotation);
 		
		boxCoords[4].Add(position,addCoords);
 					  
 		addCoords.x = boxEnd.x;   
		addCoords.y = boxStart.y;   
		addCoords.z = boxStart.z; 
		if (rotation != 0.0f)
			Rotate(addCoords,-rotation);
 		
		boxCoords[5].Add(position,addCoords);
		
 		addCoords.x = boxEnd.x;   
		addCoords.y = boxEnd.y;   
		addCoords.z = boxStart.z; 
		if (rotation != 0.0f)
			Rotate(addCoords,-rotation);
 		
		boxCoords[6].Add(position,addCoords);
		
 		addCoords.x = boxStart.x; 
		addCoords.y = boxEnd.y;   
		addCoords.z = boxStart.z; 
		if (rotation != 0.0f)
			Rotate(addCoords,-rotation);
 		
		boxCoords[7].Add(position,addCoords);

		Stuff::Vector4D screenPos[8];
		for (long i=0;i<8;i++)
		{
			// [PROJECTZ:ScreenXYOracle id=genactor_box_wire]
			eye->projectForScreenXY(boxCoords[i],screenPos[i]);
		}

		{
			LineElement newElement(screenPos[0],screenPos[1],XP_WHITE,nullptr,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[0],screenPos[4],XP_WHITE,nullptr,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[0],screenPos[3],XP_WHITE,nullptr,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[5],screenPos[4],XP_WHITE,nullptr,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[5],screenPos[6],XP_WHITE,nullptr,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[5],screenPos[3],XP_WHITE,nullptr,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[2],screenPos[3],XP_WHITE,nullptr,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[2],screenPos[6],XP_WHITE,nullptr,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[2],screenPos[1],XP_WHITE,nullptr,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[7],screenPos[1],XP_WHITE,nullptr,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[7],screenPos[6],XP_WHITE,nullptr,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[7],screenPos[4],XP_WHITE,nullptr,-1);
			newElement.draw();
		}
#endif
#undef DRAW_BOX
	}
	return NO_ERR;
}

//-----------------------------------------------------------------------------
long GenericAppearance::renderShadows (void)
{
	return NO_ERR;	// the CPU shadow geometry is gone; shadows come from the modern shadow path
}

//-----------------------------------------------------------------------------
long GenericAppearance::update (bool animate)
{
	const bool simOnly = !simPresentationPass && (simulationOwned() || mc2::overlap::onSimWorker());
	::mc2_object_recon::Scope _recon_generic_(
		&::mc2_object_recon::g_per_frame.generic_update_ns,
		&::mc2_object_recon::g_per_frame.generic_update_calls);
	glm::vec3 xlatPosition;
	glm::quat rot;
	glm::quat yawAngle;
	glm::quat pitchAngle;

	if (inView && !simOnly)
	{
		if (rotation > 180)
			rotation -= 360;
	
		if (rotation < -180)
			rotation += 360;
	
		//-------------------------------------------
		// Does math necessary to draw Tree
		float yaw = rotation * DEGREES_TO_RADS;
		float p = pitch * DEGREES_TO_RADS;
		pitchAngle = mc2::math::quatFromEuler(mc2::math::Euler(p, 0.0f, 0.0f));
		yawAngle = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,yaw,0.0f)); 
		rot = pitchAngle;
		rot = mc2::math::composeQuat(pitchAngle, yawAngle);
	
		unsigned char lightr,lightg,lightb;
		float lightIntensity = 1.0f;
		if (land)
			land->getTerrainLight(position);
							
		lightr = eye->getLightRed(lightIntensity);
		lightg = eye->getLightGreen(lightIntensity);
		lightb = eye->getLightBlue(lightIntensity);
	
		DWORD lightRGB = (lightr<<16) + (lightg<<8) + lightb;
		
		eye->setLightColor(0,lightRGB);
		eye->setLightIntensity(0,1.0);
	
		DWORD fogRGB = 0xff<<24;
		float fogStart = eye->fogStart;
		float fogFull = eye->fogFull;
	
		mc2ToRender(xlatPosition, position);
	
		if (xlatPosition.y < fogStart)
		{
			float fogFactor = fogStart - xlatPosition.y;
			if (fogFactor < 0.0)
				fogRGB = 0xff<<24;
			else
			{
				fogFactor /= (fogStart - fogFull);
				if (fogFactor <= 1.0)
				{
					fogFactor *= fogFactor;
					fogFactor = 1.0 - fogFactor;
					fogFactor *= 256.0;
				}
				else
				{
					fogFactor = 256.0;
				}
	
				unsigned char fogResult = float2long(fogFactor);
				fogRGB = fogResult << 24;
			}
		}
		else
		{
			fogRGB = 0xff<<24;
		}
	
		if (useFog)
			genShape->SetFogRGB(fogRGB);
		else
			genShape->SetFogRGB(0xffffffff);

		// LIGHT-REALISM-1 (beacon emission): standalone beacon / runway-light
		// props (runwaylight.tgl and friends) are GenericAppearance objects,
		// NOT BldgAppearance children — a [BEACON_DIAG v1] sweep of every
		// building shape at mc2_10 found billboards=0 across the board. Queue
		// the soft colored ground disc here too; the classification, gating
		// (MC2_LIGHT_REALISM, default ON since CONE-SPILL-POLICY-1; =0
		// reverts) and night check all live inside
		// QueueBeaconGlow, so this is a no-op for every non-beacon prop.
		if (eye && eye->isNight)
			mc2_cone_glow::QueueBeaconGlow(genShape, eye->getNightFactor());
	}

	if ((!simPresentationPass || !simulationOwned()) && animate && genFrameRate != 0.0f)
	{
		//--------------------------------------------------------
		// Make sure animation runs no faster than bdFrameRate fps.
		float frameInc = genFrameRate * frameLength;
		
		//---------------------------------------
		// Increment Frames -- Everything else!
		if (frameInc != 0.0f)
		{
			if (!setFirstFrame)		//DO NOT ANIMATE ON FIRST FRAME!  Wait a bit!
			{
				if (isReversed)
					currentFrame -= frameInc;
				else
					currentFrame += frameInc;
			}
			else
			{
				setFirstFrame = false;
			}
	
			//--------------------------------------
			//Check Positive overflow of Animation
			if (currentFrame >= appearType->getNumFrames(genAnimationState))
			{
				if (isLooping)
					currentFrame -= appearType->getNumFrames(genAnimationState);
				else
					currentFrame = appearType->getNumFrames(genAnimationState) - 1;
					
				canTransition = true;		//Whenever we have completed one cycle or at last frame, OK to move on!
			}
			
	
			//--------------------------------------
			//Check negative overflow of gesture
			if (currentFrame < 0)
			{
				if (isLooping)
					currentFrame += appearType->getNumFrames(genAnimationState); 
				else
					currentFrame = 0.0f; 
					
				canTransition = true;		//Whenever we have completed one cycle or at last frame, OK to move on!
			}
		}
		
	}
	if (simOnly) return TRUE;
	mc2::overlap::assertMainThread("GenericAppearance::update presentation");
	genShape->SetFrameNum(currentFrame);

	// Under the GPU static-prop path we transform regardless of inView so
	// listOfVertices is fresh when submitMultiShape runs.
	if (inView)
	{
		genShape->SetIsClamped(true);
		genShape->SetLightList(nullptr,0);
		// Slice 2 (object-offload) — Stage 2.B: eligibility hoist.
		// See BldgAppearance::update for the full rationale; same shape.
		// Branch lives INSIDE the existing inView cull
		// gate to preserve slice 1's R1 invariant.
		if (!needsFullBakeNextFrame &&
		    GpuStaticPropBatcher::instance().isMultiShapeEligibleForGpuObjects(genShape))
		{
			// Stage 2.D.2.1 (M1): cache GPU light data now, while
			// worldLights[0]->aRGB is the per-actor terrain-scaled value.
			// Mirrors bdactor.cpp:2247 and :4398. Without this call,
			// submitMultiShape() falls back to GatherGpuObjectLightDataOnly()
			// during renderLists(), which reads the last-written
			// worldLights[0]->aRGB (the final actor's value) instead of
			// this actor's value. The resulting safe-by-accident invariant
			// (GenericAppearance::SetLightList(NULL, 0) zaps s_listOfLights)
			// is brittle — explicit caching removes the fragility.
			genShape->CacheGpuLightData();
			genShape->TransformMultiShape_HierarchyOnly (&xlatPosition,&rot);
		}
		else
		{
			genShape->TransformMultiShape_HierarchyOnly (&xlatPosition,&rot);
			if (!GpuStaticPropBatcher::instance().isMultiShapeEligibleForGpuObjects(genShape))
				genShape->ReserveCpuDraw();	// no batcher takes it: the CPU fallback draws it
			needsFullBakeNextFrame = false;
		}
	}

	return TRUE;
}

//-----------------------------------------------------------------------------
void GenericAppearance::destroy (void)
{
	if ( genShape )
	{
		delete genShape;
		genShape = nullptr;
	}

	appearanceTypeList->removeAppearance(appearType);
}

inline constexpr float HEIGHT_THRESHOLD = 10.0f;
//-----------------------------------------------------------------------------
void GenericAppearance::markTerrain (_ScenarioMapCellInfo* pInfo, int type, int counter)
{
}

//-----------------------------------------------------------------------------
void GenericAppearance::markMoveMap (bool passable)
{
}
