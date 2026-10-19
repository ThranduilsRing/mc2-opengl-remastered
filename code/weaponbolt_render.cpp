// WeaponBolt: bolt rendering and the VK VFX immediate-draw gates it reads.
// Declared in the class headers; shared names in weaponbolt_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "weaponbolt_internal.h"
#include "../RenderCore/axis_convert.h"
#include "mc2fmt.h"


// VK-FIELD-VFX-2 (2026-07-11): under the field VK umbrella the DEFAULT bolt path
// (drawOldWay==false -> mcTextureManager->addVertices) DEFERS the bolt geometry into
// the texture-manager render list, which renderLists() flushes into the (discarded)
// GL scene backbuffer under VK-only present -> weapon fire VANISHES. The round-1
// objmgr IsHUD wrap is INERT for this path because addVertices only memcpys verts
// (no IsHUD capture) and the flush runs OUTSIDE the IsHUD scope. Fix: when the VK VFX
// route is armed, take the IMMEDIATE gos_DrawTriangles path instead (the exact
// mechanism the selection brackets / health bars use, PROVEN visible) so the bolt
// verts are issued while objmgr has gos_State_IsHUD=1 -> composited into the VK HUD
// tail.
static bool vkVfxTraceOn()
{
	static const bool s_on = mc2vkgate::vfxTraceOn();
	return s_on;
}

void WeaponBolt::render (void)
{
	if (!getFlag(OBJECT_FLAG_JUSTCREATED) && !isTargeted())	//can we be seen by the enemy?
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

		// VK-FIELD-VFX-2: force the immediate gos_DrawTriangles path under the VK VFX
		// umbrella route so the bolt lands in the composited HUD tail (see file-top note).
		const bool useImmediate = true;
		const bool vfxTrace = vkVfxTraceOn();

		if (!gosEffect && !hitTarget && !(static_cast<WeaponBoltTypePtr>(getObjectType()))->isBeam)
		{
			gos_VERTEX	lq1Vertices[3], lq2Vertices[3], lq3Vertices[3], lq4Vertices[3];
			Stuff::Vector4D screenPos;
			
			DWORD alphaMode = (static_cast<WeaponBoltTypePtr>(getObjectType()))->boltAlpha;
			alphaMode <<= 24;
			
			DWORD edgeMode = (static_cast<WeaponBoltTypePtr>(getObjectType()))->edgeAlpha;
			edgeMode <<= 24;
			
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_laser_tip]
			eye->projectForScreenXY(laserPosition,screenPos);
			lq1Vertices[0].x		= screenPos.x;
			lq1Vertices[0].y		= screenPos.y;
			lq1Vertices[0].z		= 0.1f;
			lq1Vertices[0].rhw		= screenPos.w;
			lq1Vertices[0].u		= 0.0f;
			lq1Vertices[0].v		= 0.0f;
			lq1Vertices[0].argb		= edgeMode + (static_cast<WeaponBoltTypePtr>(getObjectType()))->frontRGB;
	
			lq2Vertices[0].x		= screenPos.x;
			lq2Vertices[0].y		= screenPos.y;
			lq2Vertices[0].z		= 0.1f;
			lq2Vertices[0].rhw		= screenPos.w;
			lq2Vertices[0].u		= 0.0f;
			lq2Vertices[0].v		= 0.0f;
			lq2Vertices[0].argb		= edgeMode + (static_cast<WeaponBoltTypePtr>(getObjectType()))->frontRGB;
	
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_laser_v0]
			eye->projectForScreenXY(laserVertices[0],screenPos);
			lq1Vertices[1].x		= screenPos.x;
			lq1Vertices[1].y		= screenPos.y;
			lq1Vertices[1].z		= 0.1f;
			lq1Vertices[1].rhw		= screenPos.w;
			lq1Vertices[1].u		= 0.0f;
			lq1Vertices[1].v		= 0.0f;
			lq1Vertices[1].argb		= edgeMode + (static_cast<WeaponBoltTypePtr>(getObjectType()))->midEdgeRGB;
			
			lq3Vertices[1].x		= screenPos.x;
			lq3Vertices[1].y		= screenPos.y;
			lq3Vertices[1].z		= 0.1f;
			lq3Vertices[1].rhw		= screenPos.w;
			lq3Vertices[1].u		= 0.0f;
			lq3Vertices[1].v		= 0.0f;
			lq3Vertices[1].argb		= edgeMode + (static_cast<WeaponBoltTypePtr>(getObjectType()))->midEdgeRGB;
			
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_laser_v3]
			eye->projectForScreenXY(laserVertices[3],screenPos);
			lq1Vertices[2].x		= screenPos.x;
			lq1Vertices[2].y		= screenPos.y;
			lq1Vertices[2].z		= 0.1f;
			lq1Vertices[2].rhw		= screenPos.w;
			lq1Vertices[2].u		= 0.0f;
			lq1Vertices[2].v		= 0.0f;
			lq1Vertices[2].argb		= alphaMode + (static_cast<WeaponBoltTypePtr>(getObjectType()))->middleRGB;
			
			lq2Vertices[2].x		= screenPos.x;
			lq2Vertices[2].y		= screenPos.y;
			lq2Vertices[2].z		= 0.1f;
			lq2Vertices[2].rhw		= screenPos.w;
			lq2Vertices[2].u		= 0.0f;
			lq2Vertices[2].v		= 0.0f;
			lq2Vertices[2].argb		= alphaMode + (static_cast<WeaponBoltTypePtr>(getObjectType()))->middleRGB;
			
			lq3Vertices[0].x		= screenPos.x;
			lq3Vertices[0].y		= screenPos.y;
			lq3Vertices[0].z		= 0.1f;
			lq3Vertices[0].rhw		= screenPos.w;
			lq3Vertices[0].u		= 0.0f;
			lq3Vertices[0].v		= 0.0f;
			lq3Vertices[0].argb		= alphaMode + (static_cast<WeaponBoltTypePtr>(getObjectType()))->middleRGB;
			
			lq4Vertices[0].x		= screenPos.x;
			lq4Vertices[0].y		= screenPos.y;
			lq4Vertices[0].z		= 0.1f;
			lq4Vertices[0].rhw		= screenPos.w;
			lq4Vertices[0].u		= 0.0f;
			lq4Vertices[0].v		= 0.0f;
			lq4Vertices[0].argb		= alphaMode + (static_cast<WeaponBoltTypePtr>(getObjectType()))->middleRGB;
			
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_laser_v2]
			eye->projectForScreenXY(laserVertices[2],screenPos);
			lq3Vertices[2].x		= screenPos.x;
			lq3Vertices[2].y		= screenPos.y;
			lq3Vertices[2].z		= 0.1f;
			lq3Vertices[2].rhw		= screenPos.w;
			lq3Vertices[2].u		= 0.0f;
			lq3Vertices[2].v		= 0.0f;
			lq3Vertices[2].argb		= edgeMode + (static_cast<WeaponBoltTypePtr>(getObjectType()))->backRGB;
			
			lq4Vertices[2].x		= screenPos.x;
			lq4Vertices[2].y		= screenPos.y;
			lq4Vertices[2].z		= 0.1f;
			lq4Vertices[2].rhw		= screenPos.w;
			lq4Vertices[2].u		= 0.0f;
			lq4Vertices[2].v		= 0.0f;
			lq4Vertices[2].argb		= edgeMode + (static_cast<WeaponBoltTypePtr>(getObjectType()))->backRGB;
			
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_laser_v1]
			eye->projectForScreenXY(laserVertices[1],screenPos);
			lq2Vertices[1].x		= screenPos.x;
			lq2Vertices[1].y		= screenPos.y;
			lq2Vertices[1].z		= 0.1f;
			lq2Vertices[1].rhw		= screenPos.w;
			lq2Vertices[1].u		= 0.0f;
			lq2Vertices[1].v		= 0.0f;
			lq2Vertices[1].argb		= edgeMode + (static_cast<WeaponBoltTypePtr>(getObjectType()))->midEdgeRGB;
		
			lq4Vertices[1].x		= screenPos.x;
			lq4Vertices[1].y		= screenPos.y;
			lq4Vertices[1].z		= 0.1f;
			lq4Vertices[1].rhw		= screenPos.w;
			lq4Vertices[1].u		= 0.0f;
			lq4Vertices[1].v		= 0.0f;
			lq4Vertices[1].argb		= edgeMode + (static_cast<WeaponBoltTypePtr>(getObjectType()))->midEdgeRGB;

			if (vfxTrace) {
				const bool culled =
					(lq1Vertices[0].x > eye->viewMulX) || (lq1Vertices[0].y > eye->viewMulY) ||
					(lq1Vertices[0].x < eye->viewAddX) || (lq1Vertices[0].y < eye->viewAddY) ||
					(lq1Vertices[1].x > eye->viewMulX) || (lq1Vertices[1].y > eye->viewMulY) ||
					(lq1Vertices[1].x < eye->viewAddX) || (lq1Vertices[1].y < eye->viewAddY) ||
					(lq1Vertices[2].x > eye->viewMulX) || (lq1Vertices[2].y > eye->viewMulY) ||
					(lq1Vertices[2].x < eye->viewAddX) || (lq1Vertices[2].y < eye->viewAddY);
				mc2::fmt::print_out("[VK_VFX_TRACE] bolt(laser) path={} culled={:d} view=[{:.0f}..{:.0f},{:.0f}..{:.0f}] v0=({:.0f},{:.0f}) v1=({:.0f},{:.0f}) v2=({:.0f},{:.0f}) argb0={:08X}\n", mc2::fmt::safe(useImmediate ? "IMMEDIATE" : "addVertices"), mc2::fmt::as_i32(static_cast<int>(culled)), eye->viewAddX, eye->viewMulX, eye->viewAddY, eye->viewMulY, lq1Vertices[0].x, lq1Vertices[0].y, lq1Vertices[1].x, lq1Vertices[1].y, lq1Vertices[2].x, lq1Vertices[2].y, mc2::fmt::as_u32(static_cast<unsigned>(lq1Vertices[0].argb)));
				fflush(stdout);
			}

			if (((lq1Vertices[0].x > eye->viewMulX) ||
				(lq1Vertices[0].y > eye->viewMulY) ||
				(lq1Vertices[0].x < eye->viewAddX) || 
				(lq1Vertices[0].y < eye->viewAddY) || 
				(lq1Vertices[1].x > eye->viewMulX) ||
				(lq1Vertices[1].y > eye->viewMulY) ||
				(lq1Vertices[1].x < eye->viewAddX) ||
				(lq1Vertices[1].y < eye->viewAddY) ||
				(lq1Vertices[2].x > eye->viewMulX) ||
				(lq1Vertices[2].y > eye->viewMulY) ||
				(lq1Vertices[2].x < eye->viewAddX) ||
				(lq1Vertices[2].y < eye->viewAddY)))
			{
			}
			else
			{
				if (useImmediate)
				{
					gos_SetRenderState( gos_State_Texture, 0 );
			
					//--------------------------------
					//Set States for Software Renderer
					if (Environment.Renderer == 3)
					{
						gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
						gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
						gos_SetRenderState( gos_State_MonoEnable, 1);
						gos_SetRenderState( gos_State_Perspective, 0);
						gos_SetRenderState( gos_State_Clipping, 0);
						gos_SetRenderState( gos_State_AlphaTest, 0);
						gos_SetRenderState( gos_State_Specular, 0);
						gos_SetRenderState( gos_State_Dither, 0);
						gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulateAlpha);
						gos_SetRenderState( gos_State_Filter, gos_FilterNone);
						gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
					}
					else
					{
						gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
						gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
						gos_SetRenderState( gos_State_MonoEnable, 0);
						gos_SetRenderState( gos_State_Perspective, 1);
						gos_SetRenderState( gos_State_Clipping, 2);
						gos_SetRenderState( gos_State_AlphaTest, 0);
						gos_SetRenderState( gos_State_Specular, 0);
						gos_SetRenderState( gos_State_Dither, 1);
						gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulateAlpha);
						gos_SetRenderState( gos_State_Filter, gos_FilterBiLinear);
						gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
					}

					gos_DrawTriangles(lq1Vertices,3);
				}
				else
				{
					mcTextureManager->addVertices(0xffffffff,lq1Vertices,MC2_DRAWALPHA);
				}
			}
	
			if (((lq2Vertices[0].x > eye->viewMulX) ||
				(lq2Vertices[0].y > eye->viewMulY) || 
				(lq2Vertices[0].x < eye->viewAddX) || 
				(lq2Vertices[0].y < eye->viewAddY) || 
				(lq2Vertices[1].x > eye->viewMulX) ||
				(lq2Vertices[1].y > eye->viewMulY) ||
				(lq2Vertices[1].x < eye->viewAddX) ||
				(lq2Vertices[1].y < eye->viewAddY) ||
				(lq2Vertices[2].x > eye->viewMulX) ||
				(lq2Vertices[2].y > eye->viewMulY) ||
				(lq2Vertices[2].x < eye->viewAddX) ||
				(lq2Vertices[2].y < eye->viewAddY)))
			{
			}
			else
			{
				if (useImmediate)
				{
					gos_DrawTriangles(lq2Vertices,3);
				}
				else
				{
					mcTextureManager->addVertices(0xffffffff,lq2Vertices,MC2_DRAWALPHA);
				}
			}
				
			if (((lq3Vertices[0].x > eye->viewMulX) ||
				(lq3Vertices[0].y > eye->viewMulY) || 
				(lq3Vertices[0].x < eye->viewAddX) || 
				(lq3Vertices[0].y < eye->viewAddY) || 
				(lq3Vertices[1].x > eye->viewMulX) ||
				(lq3Vertices[1].y > eye->viewMulY) ||
				(lq3Vertices[1].x < eye->viewAddX) ||
				(lq3Vertices[1].y < eye->viewAddY) ||
				(lq3Vertices[2].x > eye->viewMulX) ||
				(lq3Vertices[2].y > eye->viewMulY) ||
				(lq3Vertices[2].x < eye->viewAddX) ||
				(lq3Vertices[2].y < eye->viewAddY)))
			{
			}
			else
			{
				if (useImmediate)
				{
					gos_DrawTriangles(lq3Vertices,3);
				}
				else
				{
					mcTextureManager->addVertices(0xffffffff,lq3Vertices,MC2_DRAWALPHA);
				}
			}
	
			if (((lq4Vertices[0].x > eye->viewMulX) ||
				(lq4Vertices[0].y > eye->viewMulY) || 
				(lq4Vertices[0].x < eye->viewAddX) || 
				(lq4Vertices[0].y < eye->viewAddY) || 
				(lq4Vertices[1].x > eye->viewMulX) ||
				(lq4Vertices[1].y > eye->viewMulY) ||
				(lq4Vertices[1].x < eye->viewAddX) ||
				(lq4Vertices[1].y < eye->viewAddY) ||
				(lq4Vertices[2].x > eye->viewMulX) ||
				(lq4Vertices[2].y > eye->viewMulY) ||
				(lq4Vertices[2].x < eye->viewAddX) ||
				(lq4Vertices[2].y < eye->viewAddY)))
			{
			}
			else
			{
				if (useImmediate)
				{
					gos_DrawTriangles(lq4Vertices,3);
				}
				else
				{
					mcTextureManager->addVertices(0xffffffff,lq4Vertices,MC2_DRAWALPHA);
				}
			}
		}
		else if (!hitTarget && (static_cast<WeaponBoltTypePtr>(getObjectType()))->isBeam)
		{
			gos_VERTEX	lq1Vertices[3], lq2Vertices[3], lq3Vertices[3], lq4Vertices[3];
			Stuff::Vector4D screenPos;
			
			DWORD alphaMode = (static_cast<WeaponBoltTypePtr>(getObjectType()))->boltAlpha;
			alphaMode <<= 24;
			
			DWORD edgeMode = (static_cast<WeaponBoltTypePtr>(getObjectType()))->edgeAlpha;
			edgeMode <<= 24;
			
			glm::vec3 actualPosition;
			glm::vec3 hotSpot = *targetPosition;
			GameObjectPtr target = ObjectManager->getByWatchID(targetWID);
			if (target)
			{
				if (!target->isMech())
				{
					hotSpot = target->getPositionFromHS(targetHotSpot);
				}
				else
				{
					if (targetHotSpot)
						hotSpot = target->appearance->getHitNodeLeft();
					else
						hotSpot = target->appearance->getHitNodeRight(); 
				}
			}
				
			glm::vec3 length;
			length = hotSpot - position;
			float dist = glm::length(length);
			
			float realUVRepeat = (static_cast<WeaponBoltTypePtr>(getObjectType()))->uvRepeat * dist / (static_cast<WeaponBoltTypePtr>(getObjectType()))->unitLength;
			
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_beam_v0]
			eye->projectForScreenXY(laserVertices[0],screenPos);
			lq1Vertices[0].x		= screenPos.x;
			lq1Vertices[0].y		= screenPos.y;
			lq1Vertices[0].z		= screenPos.z;
			lq1Vertices[0].rhw		= screenPos.w;
			lq1Vertices[0].u		= startUV;
			lq1Vertices[0].v		= 0.0f;
			lq1Vertices[0].argb		= edgeMode + (static_cast<WeaponBoltTypePtr>(getObjectType()))->midEdgeRGB;
	
			lq2Vertices[0].x		= screenPos.x;
			lq2Vertices[0].y		= screenPos.y;
			lq2Vertices[0].z		= screenPos.z;
			lq2Vertices[0].rhw		= screenPos.w;
			lq2Vertices[0].u		= startUV;
			lq2Vertices[0].v		= 0.0f;
			lq2Vertices[0].argb		= edgeMode + (static_cast<WeaponBoltTypePtr>(getObjectType()))->midEdgeRGB;
			
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_beam_v1]
 			eye->projectForScreenXY(laserVertices[1],screenPos);
			lq2Vertices[1].x		= screenPos.x;
			lq2Vertices[1].y		= screenPos.y;
			lq2Vertices[1].z		= screenPos.z;
			lq2Vertices[1].rhw		= screenPos.w;
			lq2Vertices[1].u		= startUV;
			lq2Vertices[1].v		= 0.999999f;
			lq2Vertices[1].argb		= alphaMode + (static_cast<WeaponBoltTypePtr>(getObjectType()))->midEdgeRGB;
	
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_beam_v2]
			eye->projectForScreenXY(laserVertices[2],screenPos);
			lq1Vertices[1].x		= screenPos.x;
			lq1Vertices[1].y		= screenPos.y;
			lq1Vertices[1].z		= screenPos.z;
			lq1Vertices[1].rhw		= screenPos.w;
			lq1Vertices[1].u		= startUV + realUVRepeat;
			lq1Vertices[1].v		= 0.0f;
			lq1Vertices[1].argb		= edgeMode + (static_cast<WeaponBoltTypePtr>(getObjectType()))->middleRGB;
			
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_beam_v3]
			eye->projectForScreenXY(laserVertices[3],screenPos);
			lq1Vertices[2].x		= screenPos.x;
			lq1Vertices[2].y		= screenPos.y;
			lq1Vertices[2].z		= screenPos.z;
			lq1Vertices[2].rhw		= screenPos.w;
			lq1Vertices[2].u		= startUV + realUVRepeat; 
			lq1Vertices[2].v		= 0.999999f;
			lq1Vertices[2].argb		= alphaMode + (static_cast<WeaponBoltTypePtr>(getObjectType()))->middleRGB;
			
			lq2Vertices[2].x		= screenPos.x;
			lq2Vertices[2].y		= screenPos.y;
			lq2Vertices[2].z		= screenPos.z;
			lq2Vertices[2].rhw		= screenPos.w;
			lq2Vertices[2].u		= startUV + realUVRepeat; 
			lq2Vertices[2].v		= 0.999999f;
			lq2Vertices[2].argb		= alphaMode + (static_cast<WeaponBoltTypePtr>(getObjectType()))->middleRGB;
			
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_beam_side0]
			eye->projectForScreenXY(laserSide[0],screenPos);
			lq3Vertices[0].x		= screenPos.x;
			lq3Vertices[0].y		= screenPos.y;
			lq3Vertices[0].z		= screenPos.z;
			lq3Vertices[0].rhw		= screenPos.w;
			lq3Vertices[0].u		= startUV;
			lq3Vertices[0].v		= 0.0f;
			lq3Vertices[0].argb		= edgeMode + (static_cast<WeaponBoltTypePtr>(getObjectType()))->midEdgeRGB;
	
			lq4Vertices[0].x		= screenPos.x;
			lq4Vertices[0].y		= screenPos.y;
			lq4Vertices[0].z		= screenPos.z;
			lq4Vertices[0].rhw		= screenPos.w;
			lq4Vertices[0].u		= startUV;
			lq4Vertices[0].v		= 0.0f;
			lq4Vertices[0].argb		= edgeMode + (static_cast<WeaponBoltTypePtr>(getObjectType()))->midEdgeRGB;
			
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_beam_side1]
 			eye->projectForScreenXY(laserSide[1],screenPos);
			lq4Vertices[1].x		= screenPos.x;
			lq4Vertices[1].y		= screenPos.y;
			lq4Vertices[1].z		= screenPos.z;
			lq4Vertices[1].rhw		= screenPos.w;
			lq4Vertices[1].u		= startUV;
			lq4Vertices[1].v		= 0.999999f;
			lq4Vertices[1].argb		= alphaMode + (static_cast<WeaponBoltTypePtr>(getObjectType()))->midEdgeRGB;
	
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_beam_side2]
			eye->projectForScreenXY(laserSide[2],screenPos);
			lq3Vertices[1].x		= screenPos.x;
			lq3Vertices[1].y		= screenPos.y;
			lq3Vertices[1].z		= screenPos.z;
			lq3Vertices[1].rhw		= screenPos.w;
			lq3Vertices[1].u		= startUV + realUVRepeat;
			lq3Vertices[1].v		= 0.0f;
			lq3Vertices[1].argb		= edgeMode + (static_cast<WeaponBoltTypePtr>(getObjectType()))->middleRGB;
			
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_beam_side3]
			eye->projectForScreenXY(laserSide[3],screenPos);
			lq3Vertices[2].x		= screenPos.x;
			lq3Vertices[2].y		= screenPos.y;
			lq3Vertices[2].z		= screenPos.z;
			lq3Vertices[2].rhw		= screenPos.w;
			lq3Vertices[2].u		= startUV + realUVRepeat; 
			lq3Vertices[2].v		= 0.999999f;
			lq3Vertices[2].argb		= alphaMode + (static_cast<WeaponBoltTypePtr>(getObjectType()))->middleRGB;
			
			lq4Vertices[2].x		= screenPos.x;
			lq4Vertices[2].y		= screenPos.y;
			lq4Vertices[2].z		= screenPos.z;
			lq4Vertices[2].rhw		= screenPos.w;
			lq4Vertices[2].u		= startUV + realUVRepeat; 
			lq4Vertices[2].v		= 0.999999f;
			lq4Vertices[2].argb		= alphaMode + (static_cast<WeaponBoltTypePtr>(getObjectType()))->middleRGB;

			if (vfxTrace) {
				mc2::fmt::print_out("[VK_VFX_TRACE] beam path={} view=[{:.0f}..{:.0f},{:.0f}..{:.0f}] v0=({:.0f},{:.0f},z{:.3f}) v1=({:.0f},{:.0f},z{:.3f}) v2=({:.0f},{:.0f},z{:.3f}) argb0={:08X} tex={:d}\n", mc2::fmt::safe(useImmediate ? "IMMEDIATE" : "addVertices"), eye->viewAddX, eye->viewMulX, eye->viewAddY, eye->viewMulY, lq1Vertices[0].x, lq1Vertices[0].y, lq1Vertices[0].z, lq1Vertices[1].x, lq1Vertices[1].y, lq1Vertices[1].z, lq1Vertices[2].x, lq1Vertices[2].y, lq1Vertices[2].z, mc2::fmt::as_u32(static_cast<unsigned>(lq1Vertices[0].argb)), mc2::fmt::as_u32(static_cast<unsigned>(gosTextureHandle)));
				fflush(stdout);
			}

			if (useImmediate)
			{
				gos_SetRenderState( gos_State_Texture, 0 );
		
				//--------------------------------
				//Set States for Software Renderer
				if (Environment.Renderer == 3)
				{
					gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneInvAlpha);
					gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
					gos_SetRenderState( gos_State_MonoEnable, 1);
					gos_SetRenderState( gos_State_Perspective, 0);
					gos_SetRenderState( gos_State_Clipping, 1);
					gos_SetRenderState( gos_State_AlphaTest, 0);
					gos_SetRenderState( gos_State_Specular, 0);
					gos_SetRenderState( gos_State_Dither, 0);
					gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulateAlpha);
					gos_SetRenderState( gos_State_Filter, gos_FilterNone);
					gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
					gos_SetRenderState( gos_State_ZCompare, 1);
					gos_SetRenderState(	gos_State_ZWrite, 0);
				}
				else
				{
					// VK-FIELD-VFX-3 (look-restore): the VK HUD tail now composites the
					// native effect blend AlphaMode OneInvAlpha(4) as a textured tri-batch
					// (vkHudDrawRepresentable accepts 4 + effect sprites carry an rgba mirror),
					// so the beam renders with its real additive-style blend just like GL.
					// Round-2's AlphaInvAlpha(3) remap is retired — it was the "weird laser".
					gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneInvAlpha);
					gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
					gos_SetRenderState( gos_State_MonoEnable, 0);
					gos_SetRenderState( gos_State_Perspective, 1);
					gos_SetRenderState( gos_State_Clipping, 1);
					gos_SetRenderState( gos_State_AlphaTest, 0);
					gos_SetRenderState( gos_State_Specular, 0);
					gos_SetRenderState( gos_State_Dither, 1);
					gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulateAlpha);
					gos_SetRenderState( gos_State_Filter, gos_FilterBiLinear);
					gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
					gos_SetRenderState( gos_State_ZCompare, 1);
					gos_SetRenderState(	gos_State_ZWrite, 0);
				}
			
				// The beam renders TEXTURED under the VK route too: the beam sprite is a
				// BC7 .ktx2 sidecar, whose RGBA8 mirror flushHUDBatch decodes from the
				// texture's blocks on first use (gosTexture::ensureRgbaMirror), and
				// if that ever fails the tail drops an AlphaMode-4 tri-batch to
				// untextured vertex-coloured (round-3's proven look) instead of
				// declining — the beam can no longer vanish. GL / umbrella-off
				// behaviour unchanged (this is the stock textured state-set).
				if (gosTextureHandle != 0xffffffff)
					gos_SetRenderState( gos_State_Texture,gosTextureHandle);
				else
					gos_SetRenderState( gos_State_Texture,0);

				gos_DrawTriangles(lq1Vertices,3);
				gos_DrawTriangles(lq2Vertices,3);
				gos_DrawTriangles(lq3Vertices,3);
				gos_DrawTriangles(lq4Vertices,3);
				// VK-FIELD-VFX-3 (look-restore): the round-2 beam-axis LINE fallback
				// (3 opaque horizontal lines) is retired — it was a visibility hack that
				// produced the flat "weird laser". The textured ribbon quads above are the
				// real beam and match GL. (If a far-zoom sub-pixel ribbon coverage delta
				// resurfaces it will be handled by the ribbon geometry, not a line proxy.)
			}
			else
			{
				if ((lq1Vertices[0].z >= 0.0f) &&
					(lq1Vertices[0].z < 1.0f) &&
					(lq1Vertices[1].z >= 0.0f) &&  
					(lq1Vertices[1].z < 1.0f) && 
					(lq1Vertices[2].z >= 0.0f) &&  
					(lq1Vertices[2].z < 1.0f))
					{
						mcTextureManager->addVertices(mcTextureHandle,lq1Vertices,MC2_ISEFFECTS|MC2_DRAWONEIN);
					}
					
				if ((lq2Vertices[0].z >= 0.0f) &&
					(lq2Vertices[0].z < 1.0f) &&
					(lq2Vertices[1].z >= 0.0f) &&  
					(lq2Vertices[1].z < 1.0f) && 
					(lq2Vertices[2].z >= 0.0f) &&  
					(lq2Vertices[2].z < 1.0f))
					{
						mcTextureManager->addVertices(mcTextureHandle,lq2Vertices,MC2_ISEFFECTS|MC2_DRAWONEIN);
					}
					
				if ((lq3Vertices[0].z >= 0.0f) &&
					(lq3Vertices[0].z < 1.0f) &&
					(lq3Vertices[1].z >= 0.0f) &&  
					(lq3Vertices[1].z < 1.0f) && 
					(lq3Vertices[2].z >= 0.0f) &&  
					(lq3Vertices[2].z < 1.0f))
					{
						mcTextureManager->addVertices(mcTextureHandle,lq3Vertices,MC2_ISEFFECTS|MC2_DRAWONEIN);
					}
					
				if ((lq4Vertices[0].z >= 0.0f) &&
					(lq4Vertices[0].z < 1.0f) &&
					(lq4Vertices[1].z >= 0.0f) &&  
					(lq4Vertices[1].z < 1.0f) && 
					(lq4Vertices[2].z >= 0.0f) &&  
					(lq4Vertices[2].z < 1.0f))
					{
						mcTextureManager->addVertices(mcTextureHandle,lq4Vertices,MC2_ISEFFECTS|MC2_DRAWONEIN);
					}
			}
				
			if (gosEffect)
			{
				glm::vec3 actualPosition;
				glm::vec3 hotSpot = *targetPosition;
				GameObjectPtr target = ObjectManager->getByWatchID(targetWID);
				if (target)
				{
					if (!target->isMech())
					{
						hotSpot = target->getPositionFromHS(targetHotSpot);
					}
					else
					{
						if (targetHotSpot)
							hotSpot = target->appearance->getHitNodeLeft();
						else
							hotSpot = target->appearance->getHitNodeRight(); 
					}
				}
	
				mc2ToRender(actualPosition, hotSpot);	//Play the beam hitting effect at the hit location
				
				glm::mat4 	shapeOrigin;
				glm::mat4	localToWorld;
				glm::mat4	localResult;
				
				mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
				mc2::math::buildTranslation(shapeOrigin, actualPosition);
				
				glm::quat effectRot;
				effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,rotation * DEGREES_TO_RADS,0.0f));
				localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
				localResult = localToWorld * shapeOrigin;
				
				drawInfo.m_parentToWorld = &localResult;
		 
				if (!MLRVertexLimitReached)
					gosEffect->Draw(&drawInfo);
			}
		}
		else if (gosEffect)
		{
			glm::vec3 actualPosition;
			mc2ToRender(actualPosition, laserPosition);
			
			glm::mat4 	shapeOrigin;
			glm::mat4	localToWorld;
			glm::mat4	localResult;
			
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, actualPosition);
			
			glm::quat effectRot;
			effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,rotation * DEGREES_TO_RADS,0.0f));
			localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
			localResult = localToWorld * shapeOrigin;
			
 			drawInfo.m_parentToWorld = &localResult;
	 
			if (!MLRVertexLimitReached)
				gosEffect->Draw(&drawInfo);
		}
		
		if (muzzleEffect)
		{
			glm::vec3 mPosition;
			mc2ToRender(mPosition, position);
			
			glm::mat4 	shapeOrigin;
			glm::mat4	localToWorld;
			glm::mat4	localResult;
			
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, mPosition);
			
			GameObjectPtr myOwner = ObjectManager->getByWatchID(ownerWID);
			glm::quat effectRot;
		
			if (myOwner)
				effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, myOwner->getRotation() * DEGREES_TO_RADS,0.0f));
			else
				effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,0.0f,0.0f)); 

			localToWorld = mc2::math::multiply(gosFX::Effect_Into_Motion, effectRot);
			localResult = localToWorld * shapeOrigin;
		
 			drawInfo.m_parentToWorld = &localResult;
	 
			// VFX-WEAPON-FX-RESTORE-OPUS-1: suppress muzzleEffect Draw when oracle ON + hitTarget.
			// muzzleEffect renders at bolt position (= target after impact); with oracle ON
			// it creates persistent bright cards at the impact site for the full afterHitTime.
			if (!MLRVertexLimitReached && !(hitTarget && mc2::particles::Batcher::is_oracle_render_enabled()))
				muzzleEffect->Draw(&drawInfo);
		}

		if (hitEffect && hitTarget && hitEffect->IsExecuted())
		{
			glm::vec3 hotSpot = *targetPosition;
			GameObjectPtr target = ObjectManager->getByWatchID(targetWID);
			if (target)
			{
				if (!target->isMech())
				{
					hotSpot = target->getPositionFromHS(targetHotSpot);
				}
				else
				{
					if (targetHotSpot)
						hotSpot = target->appearance->getHitNodeLeft();
					else
						hotSpot = target->appearance->getHitNodeRight(); 
				}
			}

			glm::vec3 tPosition;
			mc2ToRender(tPosition, hotSpot);
			
			glm::mat4 	shapeOrigin;
			glm::mat4	localToWorld;
			
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, tPosition);
			
			drawInfo.m_parentToWorld = &shapeOrigin;
	 
			// VFX-WEAPON-FX-RESTORE-OPUS-1: suppress hitEffect Draw when oracle is ON.
			// hitEffect was always invisible in original MC2 (MLR-gated). Oracle renders
			// it as oversized bright cards. The game's createExplosion() provides impact
			// visual. Re-enable when hitEffect visuals are tuned for GPU billboard path.
			if (!MLRVertexLimitReached && !mc2::particles::Batcher::is_oracle_render_enabled())
				hitEffect->Draw(&drawInfo);
		}

		if (missEffect && hitTarget && missEffect->IsExecuted())
		{
			glm::vec3 hotSpot = *targetPosition;
			
			glm::vec3 tPosition;
			mc2ToRender(tPosition, hotSpot);
			
			glm::mat4 	shapeOrigin;
			glm::mat4	localToWorld;
			
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, tPosition);
			
			drawInfo.m_parentToWorld = &shapeOrigin;
	 
			// VFX-WEAPON-FX-RESTORE-OPUS-1: suppress missEffect Draw when oracle ON.
			// Was invisible pre-MLR-restore (gated); now renders as persistent card.
			// Re-enable when tuned for GPU billboard path.
			if (!MLRVertexLimitReached && !mc2::particles::Batcher::is_oracle_render_enabled())
				missEffect->Draw(&drawInfo);
		}

		if (waterMissEffect && hitTarget && waterMissEffect->IsExecuted())
		{
			glm::vec3 hotSpot = *targetPosition;
			
			glm::vec3 tPosition;
			tPosition.x = -hotSpot.x;
			tPosition.y = Terrain::waterElevation; 
			tPosition.z = hotSpot.y;
			
			glm::mat4 	shapeOrigin;
			glm::mat4	localToWorld;
			
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, tPosition);
			
			drawInfo.m_parentToWorld = &shapeOrigin;
	 
			// VFX-WEAPON-FX-RESTORE-OPUS-1: suppress waterMissEffect Draw when oracle ON.
			if (!MLRVertexLimitReached && !mc2::particles::Batcher::is_oracle_render_enabled())
				waterMissEffect->Draw(&drawInfo);
		}
	}
}
