// WeaponBolt: bolt rendering and the VK VFX immediate-draw gates it reads.
// Declared in the class headers; shared names in weaponbolt_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "weaponbolt_internal.h"
#include "../RenderCore/axis_convert.h"
#include "mc2fmt.h"
#include "cmponent.h"
#include "../GameOS/gameos/beam_lights.h"
#include "beam_clip.h"


// VK-FIELD-VFX-2 (2026-07-11): under the field VK umbrella the DEFAULT bolt path
// (drawOldWay==false -> mc2::renderSubmissions().addVertices) DEFERS the bolt geometry into
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

// G7b K3: the sim-written fields render reads (flags, positions, beam geometry, target hot spot, owner facing) are
// copied here at the render extract, while the sim is quiescent.
static glm::vec3 boltTargetHotSpot (const glm::vec3& raw, GameObjectWatchID targetWID, int32_t targetHotSpot)
{
	glm::vec3 hotSpot = raw;
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
	return hotSpot;
}

void WeaponBolt::extractFxPacket (mc2::render_view::RenderList& list)
{
	mc2::render_view::FxPacket& view = list.fx.emplace_back();
	view.kind = mc2::render_view::FxPacket::Kind::Bolt;
	view.position = position;
	view.watchId = getWatchID(false);
	const auto* type = static_cast<WeaponBoltTypePtr>(getObjectType());
	gosFX::Effect* const fx[5] = {gosEffect.get(), muzzleEffect.get(), hitEffect.get(), missEffect.get(), waterMissEffect.get()};
	for (int i = 0; i < 5; i++)
	{
		view.effects[i] = list.retain(fx[i]);
		if (fx[i] && fx[i]->IsExecuted())
			view.flags |= mc2::render_view::EffectLive0 << i;
	}
	if (getFlag(OBJECT_FLAG_JUSTCREATED) || isTargeted())	//can we be seen by the enemy?
		return;
	view.flags |= mc2::render_view::Visible;
	if (hitTarget)
		view.flags |= mc2::render_view::HitTarget;
	view.aux[0] = visualPosition;
	if (targetPosition)
	{
		view.aux[2] = *targetPosition;
		view.aux[1] = boltTargetHotSpot(*targetPosition, targetWID, targetHotSpot);
	}
	// Draw with the same flight heading as Execute, not GameObject's base facing.
	const bool missile = weaponShot.masterId >= 0 &&
		MasterComponent::masterList[weaponShot.masterId].getForm() == COMPONENT_FORM_WEAPON_MISSILE;
	view.value[0] = targetPosition ? -world_angle_between(visualPosition, missile ? view.aux[1] : *targetPosition) : rotation;
	GameObjectPtr myOwner = ObjectManager->getByWatchID(ownerWID);
	view.value[1] = myOwner ? myOwner->getRotation() : 0.0f;
	for (int i = 0; i < 4; i++)
	{
		view.bolt.laserVertices[i] = laserVertices[i];
		view.bolt.laserSide[i] = laserSide[i];
	}
	view.bolt.startUV = startUV;
	view.bolt.uvRepeat = type->uvRepeat;
	view.bolt.unitLength = type->unitLength;
	view.bolt.frontRGB = type->frontRGB;
	view.bolt.middleRGB = type->middleRGB;
	view.bolt.backRGB = type->backRGB;
	view.bolt.midEdgeRGB = type->midEdgeRGB;
	view.bolt.boltAlpha = type->boltAlpha;
	view.bolt.edgeAlpha = type->edgeAlpha;
	view.bolt.isBeam = type->isBeam;
	const int masterId = weaponShot.masterId;
	view.bolt.energyLight = type->isBeam ||
		(masterId >= 0 && masterId < MasterComponent::numComponents &&
		 MasterComponent::masterList[masterId].getForm() == COMPONENT_FORM_WEAPON_ENERGY);
	view.bolt.lightRemaining = hitTarget ? 0.0f :
		(type->isBeam ? timeLeft : mc2::beam_lights::kImpactDuration);
	// A laser reaches its endpoint immediately; PPC reaches it on arrival.
	view.bolt.impactLightRemaining = type->isBeam
		? mc2::beam_lights::kImpactDuration - (type->beamDuration - timeLeft)
		: (hitTarget ? mc2::beam_lights::kImpactDuration - (type->afterHitTime - hitLeft) : 0.0f);
	view.bolt.gosTextureHandle = gosTextureHandle;
	view.bolt.mcTextureHandle = mcTextureHandle;
	view.flags |= mc2::render_view::BoltGeometry;
}

void WeaponBolt::drawPacket (const mc2::render_view::FxPacket& view, const mc2::render_view::RenderList& list)
{
	auto* const gosEffect = list.effect(view.effects[0]);
	auto* const muzzleEffect = list.effect(view.effects[1]);
	auto* const hitEffect = list.effect(view.effects[2]);
	auto* const missEffect = list.effect(view.effects[3]);
	auto* const waterMissEffect = list.effect(view.effects[4]);
	if (view.has(mc2::render_view::Visible))
	{
		// Extract copies under the member names, so the draw body below reads the view.
		const glm::vec3 position = view.position;
		glm::vec3 laserPosition = view.aux[0];
		// Legacy projection takes mutable positions; never hand it the frozen packet.
		auto draw = view.bolt;
		auto& laserVertices = draw.laserVertices;
		auto& laserSide = draw.laserSide;
		const float startUV = draw.startUV;
		const auto* rvType = &draw;
		const auto gosTextureHandle = draw.gosTextureHandle;
		const auto mcTextureHandle = draw.mcTextureHandle;
		const bool hitTarget = view.has(mc2::render_view::HitTarget);

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
		// Draw with the same flight heading as Execute, not GameObject's base facing.
		const float effectHeading = view.value[0];

		if (!gosEffect && !hitTarget && !rvType->isBeam)
		{
			gos_VERTEX	lq1Vertices[3], lq2Vertices[3], lq3Vertices[3], lq4Vertices[3];
			Stuff::Vector4D screenPos;
			
			DWORD alphaMode = rvType->boltAlpha;
			alphaMode <<= 24;
			
			DWORD edgeMode = rvType->edgeAlpha;
			edgeMode <<= 24;
			
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_laser_tip]
			eye->projectForScreenXY(laserPosition,screenPos);
			lq1Vertices[0].x		= screenPos.x;
			lq1Vertices[0].y		= screenPos.y;
			lq1Vertices[0].z		= 0.1f;
			lq1Vertices[0].rhw		= screenPos.w;
			lq1Vertices[0].u		= 0.0f;
			lq1Vertices[0].v		= 0.0f;
			lq1Vertices[0].argb		= edgeMode + rvType->frontRGB;
	
			lq2Vertices[0].x		= screenPos.x;
			lq2Vertices[0].y		= screenPos.y;
			lq2Vertices[0].z		= 0.1f;
			lq2Vertices[0].rhw		= screenPos.w;
			lq2Vertices[0].u		= 0.0f;
			lq2Vertices[0].v		= 0.0f;
			lq2Vertices[0].argb		= edgeMode + rvType->frontRGB;
	
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_laser_v0]
			eye->projectForScreenXY(laserVertices[0],screenPos);
			lq1Vertices[1].x		= screenPos.x;
			lq1Vertices[1].y		= screenPos.y;
			lq1Vertices[1].z		= 0.1f;
			lq1Vertices[1].rhw		= screenPos.w;
			lq1Vertices[1].u		= 0.0f;
			lq1Vertices[1].v		= 0.0f;
			lq1Vertices[1].argb		= edgeMode + rvType->midEdgeRGB;
			
			lq3Vertices[1].x		= screenPos.x;
			lq3Vertices[1].y		= screenPos.y;
			lq3Vertices[1].z		= 0.1f;
			lq3Vertices[1].rhw		= screenPos.w;
			lq3Vertices[1].u		= 0.0f;
			lq3Vertices[1].v		= 0.0f;
			lq3Vertices[1].argb		= edgeMode + rvType->midEdgeRGB;
			
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_laser_v3]
			eye->projectForScreenXY(laserVertices[3],screenPos);
			lq1Vertices[2].x		= screenPos.x;
			lq1Vertices[2].y		= screenPos.y;
			lq1Vertices[2].z		= 0.1f;
			lq1Vertices[2].rhw		= screenPos.w;
			lq1Vertices[2].u		= 0.0f;
			lq1Vertices[2].v		= 0.0f;
			lq1Vertices[2].argb		= alphaMode + rvType->middleRGB;
			
			lq2Vertices[2].x		= screenPos.x;
			lq2Vertices[2].y		= screenPos.y;
			lq2Vertices[2].z		= 0.1f;
			lq2Vertices[2].rhw		= screenPos.w;
			lq2Vertices[2].u		= 0.0f;
			lq2Vertices[2].v		= 0.0f;
			lq2Vertices[2].argb		= alphaMode + rvType->middleRGB;
			
			lq3Vertices[0].x		= screenPos.x;
			lq3Vertices[0].y		= screenPos.y;
			lq3Vertices[0].z		= 0.1f;
			lq3Vertices[0].rhw		= screenPos.w;
			lq3Vertices[0].u		= 0.0f;
			lq3Vertices[0].v		= 0.0f;
			lq3Vertices[0].argb		= alphaMode + rvType->middleRGB;
			
			lq4Vertices[0].x		= screenPos.x;
			lq4Vertices[0].y		= screenPos.y;
			lq4Vertices[0].z		= 0.1f;
			lq4Vertices[0].rhw		= screenPos.w;
			lq4Vertices[0].u		= 0.0f;
			lq4Vertices[0].v		= 0.0f;
			lq4Vertices[0].argb		= alphaMode + rvType->middleRGB;
			
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_laser_v2]
			eye->projectForScreenXY(laserVertices[2],screenPos);
			lq3Vertices[2].x		= screenPos.x;
			lq3Vertices[2].y		= screenPos.y;
			lq3Vertices[2].z		= 0.1f;
			lq3Vertices[2].rhw		= screenPos.w;
			lq3Vertices[2].u		= 0.0f;
			lq3Vertices[2].v		= 0.0f;
			lq3Vertices[2].argb		= edgeMode + rvType->backRGB;
			
			lq4Vertices[2].x		= screenPos.x;
			lq4Vertices[2].y		= screenPos.y;
			lq4Vertices[2].z		= 0.1f;
			lq4Vertices[2].rhw		= screenPos.w;
			lq4Vertices[2].u		= 0.0f;
			lq4Vertices[2].v		= 0.0f;
			lq4Vertices[2].argb		= edgeMode + rvType->backRGB;
			
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_laser_v1]
			eye->projectForScreenXY(laserVertices[1],screenPos);
			lq2Vertices[1].x		= screenPos.x;
			lq2Vertices[1].y		= screenPos.y;
			lq2Vertices[1].z		= 0.1f;
			lq2Vertices[1].rhw		= screenPos.w;
			lq2Vertices[1].u		= 0.0f;
			lq2Vertices[1].v		= 0.0f;
			lq2Vertices[1].argb		= edgeMode + rvType->midEdgeRGB;
		
			lq4Vertices[1].x		= screenPos.x;
			lq4Vertices[1].y		= screenPos.y;
			lq4Vertices[1].z		= 0.1f;
			lq4Vertices[1].rhw		= screenPos.w;
			lq4Vertices[1].u		= 0.0f;
			lq4Vertices[1].v		= 0.0f;
			lq4Vertices[1].argb		= edgeMode + rvType->midEdgeRGB;

            const glm::vec3 worlds[5]={laserPosition,laserVertices[0],laserVertices[3],laserVertices[2],laserVertices[1]};
            std::array<glm::vec4,5> clips;
            bool needsClip=false;
            for(int i=0;i<5;++i) {
                glm::vec3 render; mc2ToRender(render,worlds[i]);
                Stuff::Vector4D p; p.MultiplyPoint(render,eye->getWorldToClip());
                clips[i]=mc2::beam::cameraClip({p.x,p.y,p.z,p.w},eye->getUsePerspective());
                needsClip |= clips[i].w-clips[i].z<0 || clips[i].w<=1e-4f;
            }
            if(needsClip) {
                gos_SetRenderState(gos_State_Texture,0);
                gos_SetRenderState(gos_State_AlphaMode,gos_Alpha_OneZero);
                const auto draw=[&](const gos_VERTEX* vertices,int a,int b,int c) {
                    std::array<mc2::beam::Vertex,3> triangle;
                    const int indices[3]={a,b,c};
                    for(int i=0;i<3;++i) {
                        const auto color=vertices[i].argb;
                        triangle[i]={clips[indices[i]],{vertices[i].u,vertices[i].v},
                            {static_cast<float>((color>>24)&255),static_cast<float>((color>>16)&255),static_cast<float>((color>>8)&255),static_cast<float>(color&255)}};
                    }
                    const auto polygon=mc2::beam::clipTriangle(triangle);
                    for(int j=1;j+1<polygon.count;++j) {
                        gos_VERTEX out[3]={vertices[0],vertices[1],vertices[2]};
                        const int fan[3]={0,j,j+1};
                        for(int i=0;i<3;++i) {
                            const auto& p=polygon.vertices[fan[i]];
                            const auto xy=mc2::beam::screenPosition(p.clip,eye->getUsePerspective(),
                                {eye->viewMulX,eye->viewMulY},{eye->viewAddX,eye->viewAddY});
                            out[i].x=xy.x; out[i].y=xy.y; out[i].rhw=1.0f/p.clip.w;
                            out[i].u=p.uv.x; out[i].v=p.uv.y;
                            out[i].argb=(static_cast<DWORD>(p.argb.x)<<24)|(static_cast<DWORD>(p.argb.y)<<16)|(static_cast<DWORD>(p.argb.z)<<8)|static_cast<DWORD>(p.argb.w);
                        }
                        gos_DrawTriangles(out,3);
                    }
                };
                draw(lq1Vertices,0,1,2); draw(lq2Vertices,0,4,2);
                draw(lq3Vertices,2,1,3); draw(lq4Vertices,2,4,3);
            } else {

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
					mc2::renderSubmissions().addVertices(0xffffffff,lq1Vertices,MC2_DRAWALPHA);
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
					mc2::renderSubmissions().addVertices(0xffffffff,lq2Vertices,MC2_DRAWALPHA);
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
					mc2::renderSubmissions().addVertices(0xffffffff,lq3Vertices,MC2_DRAWALPHA);
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
					mc2::renderSubmissions().addVertices(0xffffffff,lq4Vertices,MC2_DRAWALPHA);
				}
			}
            }
		}
		else if (!hitTarget && rvType->isBeam)
		{
            const float dist = glm::length(view.aux[1] - position);
            const float repeat = rvType->uvRepeat * dist / rvType->unitLength;
            const auto vertex = [&](const glm::vec3& world, float u, float v, DWORD color) {
                glm::vec3 render;
                mc2ToRender(render, world);
                Stuff::Vector4D clip;
                clip.MultiplyPoint(render, eye->getWorldToClip());
                return mc2::beam::Vertex{mc2::beam::cameraClip({clip.x,clip.y,clip.z,clip.w},eye->getUsePerspective()), {u, v},
                    {static_cast<float>((color >> 24) & 255), static_cast<float>((color >> 16) & 255),
                     static_cast<float>((color >> 8) & 255), static_cast<float>(color & 255)}};
            };
            const auto drawBeam = [&](const glm::vec3* ribbon) {
                const auto a = vertex(ribbon[0], startUV, 0, (rvType->edgeAlpha << 24) + rvType->midEdgeRGB);
                const auto b = vertex(ribbon[1], startUV, 0.999999f, (rvType->boltAlpha << 24) + rvType->midEdgeRGB);
                const auto c = vertex(ribbon[2], startUV + repeat, 0, (rvType->edgeAlpha << 24) + rvType->middleRGB);
                const auto d = vertex(ribbon[3], startUV + repeat, 0.999999f, (rvType->boltAlpha << 24) + rvType->middleRGB);
                for (const auto& triangle : {std::array<mc2::beam::Vertex, 3>{a, c, d}, {a, b, d}}) {
                    const auto polygon = mc2::beam::clipTriangle(triangle);
                    for (int i = 1; i + 1 < polygon.count; ++i) {
                        gos_VERTEX vertices[3]{};
                        const int indices[] = {0, i, i + 1};
                        for (int j = 0; j < 3; ++j) {
                            const auto& p = polygon.vertices[indices[j]];
                            const float rhw = 1.0f / p.clip.w;
                            auto& out = vertices[j];
                            const auto screen = mc2::beam::screenPosition(p.clip, eye->getUsePerspective(),
                                {eye->viewMulX, eye->viewMulY}, {eye->viewAddX, eye->viewAddY});
                            out.x = screen.x; out.y = screen.y;
                            out.z = p.clip.z * rhw;
                            out.rhw = eye->getUsePerspective() ? rhw : 0.000001f;
                            out.u = p.uv.x; out.v = p.uv.y;
                            out.argb = (static_cast<DWORD>(p.argb.x + 0.5f) << 24) | (static_cast<DWORD>(p.argb.y + 0.5f) << 16) |
                                       (static_cast<DWORD>(p.argb.z + 0.5f) << 8) | static_cast<DWORD>(p.argb.w + 0.5f);
                        }
                        if (useImmediate) gos_DrawTriangles(vertices, 3);
                        else mc2::renderSubmissions().addVertices(mcTextureHandle, vertices, MC2_ISEFFECTS | MC2_DRAWONEIN);
                    }
                }
            };

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

            }
            drawBeam(laserVertices);
            drawBeam(laserSide);


			if (gosEffect)
			{
				glm::vec3 actualPosition;
				glm::vec3 hotSpot = view.aux[1];
	
				mc2ToRender(actualPosition, hotSpot);	//Play the beam hitting effect at the hit location
				
				glm::mat4 	shapeOrigin;
				glm::mat4	localToWorld;
				glm::mat4	localResult;
				
				mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
				mc2::math::buildTranslation(shapeOrigin, actualPosition);
				
				glm::quat effectRot;
				effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,effectHeading * DEGREES_TO_RADS,0.0f));
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
			effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,effectHeading * DEGREES_TO_RADS,0.0f));
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
			
			glm::quat effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, view.value[1] * DEGREES_TO_RADS,0.0f));

			localToWorld = mc2::math::multiply(gosFX::Effect_Into_Motion, effectRot);
			localResult = localToWorld * shapeOrigin;
		
 			drawInfo.m_parentToWorld = &localResult;
	 
			if (!MLRVertexLimitReached)
				muzzleEffect->Draw(&drawInfo);
		}

		if (hitEffect && hitTarget && view.executed(2))
		{
			glm::vec3 hotSpot = view.aux[1];

			glm::vec3 tPosition;
			mc2ToRender(tPosition, hotSpot);
			
			glm::mat4 	shapeOrigin;
			glm::mat4	localToWorld;
			
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, tPosition);
			
			drawInfo.m_parentToWorld = &shapeOrigin;
	 
			if (!MLRVertexLimitReached)
				hitEffect->Draw(&drawInfo);
		}

		if (missEffect && hitTarget && view.executed(3))
		{
			glm::vec3 hotSpot = view.aux[2];
			
			glm::vec3 tPosition;
			mc2ToRender(tPosition, hotSpot);
			
			glm::mat4 	shapeOrigin;
			glm::mat4	localToWorld;
			
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, tPosition);
			
			drawInfo.m_parentToWorld = &shapeOrigin;
	 
			if (!MLRVertexLimitReached)
				missEffect->Draw(&drawInfo);
		}

		if (waterMissEffect && hitTarget && view.executed(4))
		{
			glm::vec3 hotSpot = view.aux[2];
			
			glm::vec3 tPosition;
			tPosition.x = -hotSpot.x;
			tPosition.y = Terrain::waterElevation; 
			tPosition.z = hotSpot.y;
			
			glm::mat4 	shapeOrigin;
			glm::mat4	localToWorld;
			
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, tPosition);
			
			drawInfo.m_parentToWorld = &shapeOrigin;
	 
			if (!MLRVertexLimitReached)
				waterMissEffect->Draw(&drawInfo);
		}
	}
}
