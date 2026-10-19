#include "Gates/preferences_legacy.h"
#include "../GameOS/gameos/ui_texture.h"
/*************************************************************************************************\
gameTacMap.cpp			: Implementation of the gameTacMap component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"gametacmap.h"
#include"team.h"
#include"comndr.h"
#include"objmgr.h"
#include"cellip.h"
#include"gamecam.h"
#include"objective.h"
#include"mission.h"
#include"platform_windows.h"
#include "../resource.h"

// ---- IN-MISSION-HUD-IMGUI-SCOPE-1 slice E (MC2_HUD_IMGUI_TACMAP, default OFF) ----
// PLAN (recon of this file):
//  * Map texture: gos handle from gos_NewTextureFromMemory (init()); bridged to
//    ImGui via HudImGui::gosTextureGLName. Same UV window as the legacy corners
//    (u,v 2/bmpWidth .. 128/bmpWidth|Height — v0 uses bmpWidth in legacy, kept).
//  * Panel rect: authored HUD rect (left/top/right/bottom from rectInfos[0] via
//    setPos) through hudShrinkPoint (bottom band — see the update() hazard
//    comment below re getMouseHudX/Y) then beginPanel (canvas remap).
//  * Overlays reuse the SAME transforms: worldToTacMap for blips/rings/markers
//    (HUD-space out), then a pure fractional hud->panel map (no re-derived
//    world math). Blip/sensor color+visibility logic factored VERBATIM into
//    collectBlips(), shared by legacy render() and the ImGui path.
//  * Objective markers: parity with RenderMarkers (non-NAV = shadowed white
//    m_markerText at worldToTacMap pos; NAV draws nothing on the tac map),
//    including the flash windowing driven by animate().
//  * Viewport/frustum wedge: NOT ported — the legacy trapezoid was carved out
//    2026-05-19 (broken inverseProjectForPicking chain, see comment in
//    render()); parity = absent. Sticky-render-state block still runs on the
//    ImGui path (downstream renderers inherit it — see that comment).
//  * Interactions: ImGui-native (InvisibleButton over the panel). Click maps
//    panel-fraction -> authored rect -> tacMapToWorld (the exact inverse the
//    legacy update() does through getMouseHudX/Y), then dispatches the SAME
//    calls: airstrike doMove / eye->setPosition + setTarget(0) / right-click
//    doMove under useLeftRightMouseProfile. Left-drag pans continuously.
//    update() early-outs when the gate is on so clicks aren't double-handled.
#include"hudimguipanel.h"
#include "imgui.h"
#include <stdint.h>   // intptr_t (ImTextureID cast)
#include "Gates/gate.h"
extern unsigned char godMode;

constexpr int SQUARE_BLIP = 0;
constexpr int DOT_BLIP = 1;
constexpr int TRIANGLE_BLIP = 2;

const float GameTacMap::s_blinkLength = .5f;
float		GameTacMap::s_lastBlinkTime = 0.f;

extern bool ShowMovers;

GameTacMap::GameTacMap()
{
	top = 0;
	left = 0;
	right = 0;
	bottom = 0;

	buildingPoints = nullptr;
	buildingCount = 0;

	navMarkerCount = 0;
	curNavMarker = -1;

	objectiveAnimationId = -1;
	objectiveFlashTime = 0.0f;
	objectiveNumFlashes = 0;
}

void GameTacMap::init( unsigned char* bitmapData, int dataSize )
{

	EllipseElement::init();
	TGAFileHeader* pHeader = (TGAFileHeader*)bitmapData;

	bmpWidth = pHeader->width;
	bmpHeight = pHeader->height;

	textureHandle = gos_NewTextureFromMemory(gos_Texture_Solid,".tga",bitmapData,dataSize,0);
	char path[256];
	strcpy(  path, artPath );
	strcat( path, "viewingrect.tga" );
	viewRectHandle = mcTextureManager->loadTexture(path, gos_Texture_Alpha, 0 );

	strcpy( path, artPath );
	strcat( path, "blip.tga" );
	blipHandle = mcTextureManager->loadTexture( path, gos_Texture_Alpha, 0 );
}

bool GameTacMap::usingImGui() const
{
	return mc2::gate::get<"MC2_HUD_IMGUI_TACMAP">() && !HudImGui::pinnedToLegacy();
}

void GameTacMap::update()
{
	// slice E: ImGui panel owns hover/click (ImGui-native hit-testing inside
	// renderImGui) — skip the legacy HUD-space mouse path entirely.
	if ( usingImGui() )
		return;

	Stuff::Vector2DOf<long> screen;
	// The tac map is drawn through the bottom-band HUD batch and is shrunk by
	// s_hud_scale (flushHUDBatch, single bottom-center anchor) during a mission,
	// exactly like the command bar and force-group bar. Its hit-rect
	// (left/top/right/bottom, from rectInfos[0].rect) and the tacMapToWorld
	// size math are authored at 100% coords, so the click must be mapped back
	// through the HUD-shrink inverse -- mirror the force-group bar
	// (forcegroupbar.cpp) which uses getMouseHudX/Y. Using raw getMouseX/Y here
	// made the authored rect bleed upward over the bottom-left buttons (click =
	// camera jump to top of map) and offset/mis-scaled genuine minimap clicks.
	screen.x = userInput->getMouseHudX();
	screen.y = userInput->getMouseHudY();

	float width = right - left;
	float height = bottom - top;


	if ( !inRegion(screen.x, screen.y) )
		return;

	ControlGui::instance->setRolloverHelpText( IDS_TACMAP_HELP );
	
	if ( userInput->isLeftClick() )	
	{
		screen.x -= left;
		screen.y -= top;
		
		glm::vec3 world;

		tacMapToWorld( screen, width, height, world );
		if (MissionInterfaceManager::instance()->getControlGui()->isAddingAirstrike() &&  
			MissionInterfaceManager::instance()->getControlGui()->isButtonPressed( ControlGui::SENSOR_PROBE ))
			MissionInterfaceManager::instance()->doMove(world);
		else
		{
			eye->setPosition( world, false );
			((GameCamera*)(eye))->setTarget( nullptr );
		}
	}
	else if ( userInput->isRightClick() && useLeftRightMouseProfile )
	{
		screen.x -= left;
		screen.y -= top;
		
		glm::vec3 world;

		tacMapToWorld( screen, width, height, world );

		MissionInterfaceManager::instance()->doMove(world);
	}

}

bool GameTacMap::animate (long objectiveId, long nFlashes)
{
	if (objectiveAnimationId == -1)
	{
		objectiveAnimationId = objectiveId - 1;
		objectiveFlashTime = 0.0f;
		objectiveNumFlashes = nFlashes;

		return true;
	}

	return false;
}

void GameTacMap::render()
{
	if (turn < 2)		//Terrain not setup yet.  Left,Right,Top,Bottom are poopy!
		return;

	if ( usingImGui() && renderImGui() )
	{
		// The legacy path leaves the GPU in a known-good sticky state (the
		// preserved 13-call block below); downstream renderers inherit gos
		// render state, so the ImGui path must leave the SAME state behind.
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_MonoEnable, 0);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Clipping, 2);
		gos_SetRenderState( gos_State_AlphaTest, 0);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 1);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterBiLinear);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
		gos_SetRenderState( gos_State_ZCompare, 0);
		gos_SetRenderState( gos_State_ZWrite, 0);
		return;
	}

	gos_VERTEX corners[5];

	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero );

	gos_SetRenderState( gos_State_Specular,FALSE );
	gos_SetRenderState( gos_State_AlphaTest, FALSE );
			
	gos_SetRenderState( gos_State_Filter, gos_FilterNone );
	gos_SetRenderState( gos_State_ZWrite, 0 );
	gos_SetRenderState( gos_State_ZCompare, 0 );
	gos_SetRenderState( gos_State_Texture, textureHandle );


	for ( int i = 0; i < 4; ++i )
	{
		corners[i].rhw = 1.0f;
		corners[i].argb = 0xffffffff;
		corners[i].frgb = 0;
		corners[i].z = 0.0f;
	}


	corners[0].x = corners[2].x = left;
	corners[1].x = corners[3].x = right;
	corners[3].y = corners[2].y = bottom;
	corners[0].y = corners[1].y = top;

	corners[0].u = 2.f/(float)bmpWidth;
	corners[3].u = 128.f/(float)bmpWidth;
	corners[2].u = 2.f/(float)bmpWidth;
	corners[1].u = 128.f/(float)bmpWidth;
	corners[0].v = 2.f/(float)bmpWidth;
	corners[3].v = 128.f/(float)bmpHeight;
	corners[2].v = 128.f/(float)bmpHeight;
	corners[1].v = 2.f/(float)bmpWidth;
	
	gos_DrawTriangles( corners, 3 );
	gos_DrawTriangles( &corners[1], 3 );


	//-----------------------------------------------------------
	// Render the objective markers
	long count = 0;
	for ( CObjective* pObjective : Team::home->objectives )
	{
		//We are there.  Start flashing.
		if ((objectiveAnimationId == count) && objectiveNumFlashes)
		{
			objectiveFlashTime += frameLength;
			if ( objectiveFlashTime > .5f )
			{
				objectiveFlashTime = 0.0f;
				objectiveNumFlashes--;
			}
			else if ( objectiveFlashTime > 0.25f)
			{
				pObjective->RenderMarkers(this, 0);
			}

			if (objectiveNumFlashes == 0)
			{
				//Flashing is done.  We now return you to your regularly scheduled program.
				objectiveAnimationId = 0;
			}
		}
		else
		{
			pObjective->RenderMarkers(this, 0);
		}

		count++;
	}

	// Surgical Phase-1 carve-out 2026-05-19: the broken viewport-rect
	// overlay block that used to live here (4x inverseProjectForPicking +
	// worldToTacMap + gos_DrawQuads trapezoid) is killed (the camera
	// vertices are broken; the chain inverseProjectForPicking ->
	// inverseProjectZ -> setInverseProject is being retired in Phases 2-5).
	// HOWEVER these 13 gos_SetRenderState calls are PRESERVED: they were
	// inside the deleted block but their effect on GPU state is inherited
	// by every renderer that runs after GameTacMap::render() (gos render
	// state is sticky across calls; see memory/blend_state_inheritance_in_
	// post_process.md). Deleting them caused mech-invisible + terrain-
	// texture-tiling regressions because next-frame 3D rendering inherited
	// AlphaMode=OneZero / Filter=None / unset TextureAddress instead of
	// AlphaInvAlpha / BiLinear / Wrap. The state setup STAYS even though
	// it no longer prefaces a draw within this function.
	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
	gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
	gos_SetRenderState( gos_State_MonoEnable, 0);
	gos_SetRenderState( gos_State_Perspective, 0);
	gos_SetRenderState( gos_State_Clipping, 2);
	gos_SetRenderState( gos_State_AlphaTest, 0);
	gos_SetRenderState( gos_State_Specular, 0);
	gos_SetRenderState( gos_State_Dither, 1);
	gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
	gos_SetRenderState( gos_State_Filter, gos_FilterBiLinear);
	gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
	gos_SetRenderState( gos_State_ZCompare, 0);
	gos_SetRenderState( gos_State_ZWrite, 0);

	unsigned long colors[MAX_MOVERS];
	unsigned long ringColors[MAX_MOVERS];
	glm::vec3 positions[MAX_MOVERS];
	unsigned long ranges[MAX_MOVERS];
	bool		  selected[MAX_MOVERS];

	count = collectBlips( colors, ringColors, positions, ranges, selected );

	for (int i = 0; i < count; i++ )
	{
		drawSensor( positions[i], ranges[i], ringColors[i] );
	}
	bool bSel = 0; // draw unselected first
	for ( int j = 0; j < 2; j++ )
	{
		for ( int i = 0; i < count; i++ )
		{
			if ( selected[i] == bSel )
			{
				drawBlip( positions[i], colors[i], DOT_BLIP );
			}
		}
		bSel = 1;
	}



}

// factored VERBATIM from the legacy render() (slice E) — shared by the legacy
// draw above and the ImGui overlay path. Arrays are caller-sized MAX_MOVERS.
int GameTacMap::collectBlips( unsigned long* colors, unsigned long* ringColors,
							  glm::vec3* positions, unsigned long* ranges,
							  bool* selected )
{
	int count = 0;

	//------------------------------------------------------------
	// draw non-movers, must do separate check for vehicles, I'm not sure they
	// have sensors
	for (int i = 0; i < MAX_TEAMS; ++i )
	{
		TeamSensorSystem* pSys = SensorManager->getTeamSensor( i );
		
		if ( pSys )
		{
			for ( int j = 0; j < pSys->numSensors; ++j )
			{
				SensorSystem* pSensor = pSys->sensors[j];

				if ( !pSensor )
					continue;

				if ( pSensor->owner->isDestroyed() || pSensor->owner->isDisabled() || pSensor->owner->getStatus() == OBJECT_STATUS_SHUTDOWN )
					continue;

				if ( pSensor->getRange() < 1.1 || pSensor->broken)
					continue;

				if (!pSensor->owner->getTeam())
					continue;

				ObjectClass objClass = pSensor->owner->getObjectClass();

				unsigned long colorBlip = pSensor->owner->getSelected() ? 0xff4bff4b : 0xff00cc00;
				unsigned long colorRing = 0xff00cc00;

			
				if ( pSensor->owner->getTeam()->isNeutral( Team::home ) )
				{
					colorBlip = pSensor->owner->getSelected() ? 0xff4c4cff : 0xff0000ff;
					colorRing = 0xff0000ff;
				}
				else if ( pSensor->owner->getTeam()->isEnemy( Team::home ) ) // enemy
				{
					
					{
						colorBlip = pSensor->owner->getSelected() ? 0xffff3f3f : 0xffff0000;
						colorRing = 0xffff0000;
					}
				}

				if ( objClass != BATTLEMECH && objClass != GROUNDVEHICLE)
				{
					if ( objClass == ARTILLERY )
					{
						// blink
						s_lastBlinkTime += frameLength;
						if ( s_lastBlinkTime > s_blinkLength )
						{
							colorBlip = 0;
							colorRing = 0;
							s_lastBlinkTime = 0.f;
						}

					}

					colors[count] = colorBlip;
					ringColors[count] = colorRing;
					ranges[count] = pSensor->getRange();
					selected[count] = 0;
					positions[count] = pSensor->owner->getPosition();
					count++;

				}
			}
		}
	}

	unsigned long colorBlip, colorRing;

	//-----------------------------------------------------	
	// draw the movers
	for (int i=0;i<(ObjectManager->numMovers);i++)
	{
		MoverPtr mover = ObjectManager->getMover(i);
		if (mover && mover->getExists() && !(mover->isDestroyed() || mover->isDisabled()))
		{
			SensorSystem* pSensor = mover->getSensorSystem();
			float range = pSensor ? pSensor->getRange() : 0;
			long contactStatus = mover->getContactStatus(Team::home->getId(), true);
			if (mover->getTeamId() == Team::home->id)
			{
				if (mover->getCommanderId() == Commander::home->getId())
				{
					if (mover->isOnGUI())
					{
						colorBlip = mover->getSelected() ? 0xff4bff4b : 0xff00cc00;
						mover->getStatus() == OBJECT_STATUS_SHUTDOWN ? colorRing = 0 : colorRing =  0xff00cc00;
					}
					else
						continue;
				}
				else
				{
					if (mover->isOnGUI() && land->IsGameSelectTerrainPosition(mover->getPosition()) && mover->pathLocks)
					{
						colorBlip = mover->getSelected() ? 0xff4b4bff : 0xff0000cc;
						mover->getStatus() == OBJECT_STATUS_SHUTDOWN ? colorRing = 0 : colorRing =  0xff0000cc;
					}
					else
						continue;
				}
			}
			else if (ShowMovers || (MPlayer && MPlayer->allUnitsDestroyed[MPlayer->commanderID]) || ((mover->getTeamId() != Team::home->id)
				&&  ( contactStatus != CONTACT_NONE )
				&&  (mover->getStatus() != OBJECT_STATUS_SHUTDOWN) 
				&&  (!mover->hasNullSignature())
				&&	(mover->getEcmRange() <= 0.0f) ) )	//Do not draw ECM mechs!!)
			{
				//Not on our side.  Draw by contact status
				colorBlip = mover->getSelected() ? 0xffff3f3f : 0xffff0000;
				colorRing = 0xffff0000;
				
			}
			else
				continue;

			colors[count] = colorBlip;
			ringColors[count] = colorRing;
			ranges[count] = range;
			selected[count] = mover->getSelected();
			positions[count] = mover->getPosition();
			count++;

		}
	}

	return count;
}

// ---- slice E: ImGui tac-map panel ----

// legacy ARGB (gos) -> ImGui packed color
static ImU32 argbToImU32( unsigned long argb )
{
	return IM_COL32( (int)( ( argb >> 16 ) & 0xff ), (int)( ( argb >> 8 ) & 0xff ),
					 (int)( argb & 0xff ), (int)( ( argb >> 24 ) & 0xff ) );
}

bool GameTacMap::renderImGui()
{
	// panel over the authored HUD rect, through the bottom-band shrink (the
	// tac area is drawn shrunk by s_hud_scale — see the update() hazard
	// comment) then the canvas remap inside beginPanel.
	float pl = (float)left, pt = (float)top;
	float pr = (float)right, pb = (float)bottom;
	const float cyBand = 0.5f * ( pt + pb );
	HudImGui::hudShrinkPoint( pl, pt, cyBand );
	HudImGui::hudShrinkPoint( pr, pb, cyBand );

	if ( !HudImGui::beginPanel( "##hud_tacmap", pl, pt, pr, pb ) )
		return false;

	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 p0 = ImGui::GetWindowPos();
	const ImVec2 sz = ImGui::GetWindowSize();
	const ImVec2 p1( p0.x + sz.x, p0.y + sz.y );
	const float hudW = (float)( right - left );
	const float hudH = (float)( bottom - top );
	if ( hudW <= 0.f || hudH <= 0.f )
	{
		HudImGui::endPanel();
		return false;
	}
	// authored HUD point (worldToTacMap output space) -> panel pixels.
	// Pure fractional map: the panel rect IS the authored rect after
	// shrink+canvas, so fractions are preserved exactly.
	const float kx = sz.x / hudW, ky = sz.y / hudH;
	struct HudToPanel {
		ImVec2 o; float kx, ky; float l, t;
		ImVec2 operator()( float hx, float hy ) const
		{ return ImVec2( o.x + ( hx - l ) * kx, o.y + ( hy - t ) * ky ); }
	};
	const HudToPanel toPanel = { p0, kx, ky, (float)left, (float)top };

	// keep every overlay inside the map (legacy rings clip to the map rect)
	dl->PushClipRect( p0, p1, true );

	// ---- 1) map texture: same source + UV window as the legacy corner quads
	//      (v0 uses bmpWidth exactly as legacy does)
	const uint64_t mapTex=gos_GetUiTextureId(textureHandle);
	if ( mapTex )
	{
		const ImVec2 uv0( 2.f / (float)bmpWidth, 2.f / (float)bmpWidth );
		const ImVec2 uv1( 128.f / (float)bmpWidth, 128.f / (float)bmpHeight );
		dl->AddImage( (ImTextureID)(intptr_t)mapTex, p0, p1, uv0, uv1 );
	}

	// ---- 2) objective markers: parity with the legacy loop (RenderMarkers)
	//      incl. animate() flash windowing. Non-NAV = shadowed marker text at
	//      worldToTacMap pos; NAV draws nothing on the tac map.
	long objCount = 0;
	for ( CObjective* o : Team::home->objectives )
	{
		bool draw = true;
		if ( ( objectiveAnimationId == objCount ) && objectiveNumFlashes )
		{
			objectiveFlashTime += frameLength;
			draw = false;
			if ( objectiveFlashTime > .5f )
			{
				objectiveFlashTime = 0.0f;
				objectiveNumFlashes--;
			}
			else if ( objectiveFlashTime > 0.25f )
				draw = true;
			if ( objectiveNumFlashes == 0 )
				objectiveAnimationId = 0;
		}
		if ( draw )
		{
			if ( !o->IsResolved() && o->DisplayMarker() && o->IsActive()
				 && o->DisplayMarker() != NAV )
			{
				glm::vec3 objectivePos;
				objectivePos.x = o->MarkerX();
				objectivePos.y = o->MarkerY();
				objectivePos.z = 0.f;
				gos_VERTEX pos;
				worldToTacMap( objectivePos, pos );
				const char* txt = o->MarkerText();
				const ImVec2 ts = ImGui::CalcTextSize( txt );
				ImVec2 c = toPanel( pos.x, pos.y );
				c.x -= ts.x * 0.5f; c.y -= ts.y * 0.5f;
				// drop shadow (legacy drawShadowText offset -2,2) then text
				dl->AddText( ImVec2( c.x - 2.f, c.y + 2.f ), argbToImU32( 0xff000000 ), txt );
				dl->AddText( c, argbToImU32( 0xffffffff ), txt );
			}
		}
		objCount++;
	}

	// ---- 3) sensor rings + blips from the SHARED collection (same colors,
	//      same order, unselected-then-selected passes)
	unsigned long colors[MAX_MOVERS];
	unsigned long ringColors[MAX_MOVERS];
	glm::vec3 positions[MAX_MOVERS];
	unsigned long ranges[MAX_MOVERS];
	bool		  selected[MAX_MOVERS];
	const int count = collectBlips( colors, ringColors, positions, ranges, selected );

	for ( int i = 0; i < count; i++ )
	{
		if ( ringColors[i] == 0 )
			continue;
		float radius = (float)ranges[i];
		if ( radius > 1 )
			radius += land->metersPerCell * 6.f;   // same fudge as drawSensor
		radius *= hudW / ( land->metersPerCell * land->realVerticesMapSide * 3.f );
		gos_VERTEX v;
		worldToTacMap( positions[i], v );
		dl->AddCircle( toPanel( v.x, v.y + 1.f ), radius * kx,
					   argbToImU32( ringColors[i] ), 0, 1.5f );
	}

	const unsigned long blipGos = mcTextureManager->get_gosTextureHandle( blipHandle );
	const uint64_t blipTex=gos_GetUiTextureId(blipGos);
	bool bSel = 0;   // unselected first, selected on top (legacy order)
	for ( int j = 0; j < 2; j++ )
	{
		for ( int i = 0; i < count; i++ )
		{
			if ( selected[i] != bSel || colors[i] == 0 )
				continue;
			gos_VERTEX v;
			worldToTacMap( positions[i], v );
			// legacy blip quad: 4.1 HUD px, texture window 0..0.250001
			const ImVec2 a = toPanel( v.x - 2.f, v.y - 2.f );
			const ImVec2 b = toPanel( v.x + 2.1f, v.y + 2.1f );
			if ( blipTex )
				dl->AddImage( (ImTextureID)(intptr_t)blipTex, a, b,
							  ImVec2( 0.f, 0.f ), ImVec2( .250001f, .250001f ),
							  argbToImU32( colors[i] ) );
			else
				dl->AddRectFilled( a, b, argbToImU32( colors[i] ) );
		}
		bSel = 1;
	}

	dl->PopClipRect();

	// ---- 4) interactions: ImGui-native hit-testing, same dispatch calls as
	//      the legacy update() (which early-outs while this gate is on)
	ImGui::SetCursorScreenPos( p0 );
	ImGui::InvisibleButton( "##hud_tacmap_hit", sz,
		ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight );

	if ( ImGui::IsItemHovered() && ControlGui::instance )
		ControlGui::instance->setRolloverHelpText( IDS_TACMAP_HELP );

	const bool leftGo  = ImGui::IsItemActive() && ImGui::IsMouseDown( ImGuiMouseButton_Left );
	const bool leftClick  = ImGui::IsItemClicked( ImGuiMouseButton_Left );
	const bool rightClick = ImGui::IsItemClicked( ImGuiMouseButton_Right );
	if ( leftGo || rightClick )
	{
		// panel px -> authored rect-local HUD px (exact inverse of toPanel),
		// clamped to the map, then the SAME tacMapToWorld the legacy path uses
		const ImVec2 m = ImGui::GetMousePos();
		Stuff::Vector2DOf<long> screen;
		screen.x = (long)( ( m.x - p0.x ) / kx );
		screen.y = (long)( ( m.y - p0.y ) / ky );
		if ( screen.x < 0 ) screen.x = 0;
		if ( screen.y < 0 ) screen.y = 0;
		if ( screen.x > (long)hudW ) screen.x = (long)hudW;
		if ( screen.y > (long)hudH ) screen.y = (long)hudH;

		glm::vec3 world;
		tacMapToWorld( screen, (int)hudW, (int)hudH, world );

		if ( leftClick || rightClick )
		{
			// witness line (slice discipline, cf. [HUD-TABS v1]): proves the
			// ImGui hit-test -> tacMapToWorld -> dispatch chain fired
			const glm::vec3 camBefore = eye ? eye->getPosition() : glm::vec3( 0.f, 0.f, 0.f );
			fprintf( stderr, "[HUD-TACMAP v1] %s-click panel=(%ld,%ld) -> world=(%.1f,%.1f) cam_before=(%.1f,%.1f)\n",
					 leftClick ? "left" : "right", screen.x, screen.y,
					 world.x, world.y, camBefore.x, camBefore.y );
		}

		const bool airstrike =
			MissionInterfaceManager::instance()->getControlGui()->isAddingAirstrike()
			&& MissionInterfaceManager::instance()->getControlGui()->isButtonPressed( ControlGui::SENSOR_PROBE );
		if ( airstrike )
		{
			if ( leftClick )
				MissionInterfaceManager::instance()->doMove( world );
		}
		else if ( leftGo )
		{
			// click OR drag: continuous camera pan while held (click-to-move
			// parity on the initial press)
			eye->setPosition( world, false );
			((GameCamera*)(eye))->setTarget( nullptr );
		}
		else if ( rightClick && useLeftRightMouseProfile )
		{
			MissionInterfaceManager::instance()->doMove( world );
		}
	}

	HudImGui::endPanel();
	return true;
}

void GameTacMap::worldToTacMap( glm::vec3& world, gos_VERTEX& tac )
{
	TacMap::worldToTacMap( world, left, top, right - left, bottom - top, tac );
}	
void GameTacMap::initBuildings( unsigned char* data, int size )
{
	if ( data && size >= (int)sizeof(int32_t) )
	{
		int32_t* pData = (int32_t*)data;
		buildingCount = *pData++;

		// Bound the count by what the packet actually holds (2 int32s per
		// building); a corrupt/short-read packet must not drive the walk
		// below off the end of the buffer.
		const int32_t maxCount = (size - (int)sizeof(int32_t)) / (2 * (int)sizeof(int32_t));
		if ( buildingCount < 0 || buildingCount > maxCount )
		{
			printf("[TACMAP] initBuildings: buildingCount=%d exceeds packet capacity %d (size=%d) -- clamping to 0\n",
				buildingCount, (int)maxCount, size);
			buildingCount = 0;
		}

		if (buildingCount)
		{
			buildingPoints = std::make_unique_for_overwrite<gos_VERTEX[]>(buildingCount);
			gos_VERTEX* pTmp = buildingPoints.get();

			for ( int i = 0; i < buildingCount; ++i, pTmp++)
			{
				pTmp->x = *pData++ + left;
				pTmp->y = *pData++ + top;
				pTmp->z = 0;
				pTmp->argb = BUILDING_COLOR;
				pTmp->frgb = 0;
				pTmp->rhw = .5;
				pTmp->u = 0.f;
				pTmp->v = 0.f;
			}
		}
	}
}

void GameTacMap::drawSensor( const glm::vec3& pos, float radius, long color )
{
	if ( color == 0 )
		return;
	gos_VERTEX sqare[4];

	if ( radius > 1 )
		radius += land->metersPerCell * 6.f; // a little fudge

	radius *= ((float)(right - left))/(land->metersPerCell * land->realVerticesMapSide * 3.f);

	for ( int i = 0; i < 4; ++i )
	{
		sqare[i].z = 0;
		sqare[i].rhw = .5;
		sqare[i].argb = color;
		sqare[i].frgb = 0;
	}

	worldToTacMap( (glm::vec3&)pos, sqare[0] );

	sqare[1].x = sqare[0].x;
	sqare[1].y = sqare[0].y + 1;

	sqare[2].x = sqare[0].x + 1;
	sqare[2].y = sqare[0].y;

	sqare[3].x = sqare[0].x + 1;
	sqare[3].y = sqare[0].y + 1;

	// need to draw that round thing
	Stuff::Vector2DOf< long > center;
	Stuff::Vector2DOf< long > radii;
	center.x = float2long(sqare[1].x);
	center.y = float2long(sqare[1].y);
	radii.x = 2.0f * float2long(radius+.5);
	radii.y = 2.0f * float2long(radius+.5);


	EllipseElement circle( center, radii, color, 0 ); 
	GUI_RECT rect = { left, top, right, bottom };
	circle.setClip( rect );

	circle.draw();


}

void GameTacMap::drawBlip( const glm::vec3& pos, long color, int type )
{
	if ( color == 0 )
		return;
	gos_VERTEX triangle[4];

	gos_SetRenderState( gos_State_AlphaTest, 1);
	gos_SetRenderState( gos_State_Specular, 0);
	gos_SetRenderState( gos_State_Dither, 1);
	gos_SetRenderState( gos_State_Filter, gos_FilterBiLinear);
	gos_SetRenderState( gos_State_ZCompare, 0);
	gos_SetRenderState(	gos_State_ZWrite, 0);
	unsigned long gosID = mcTextureManager->get_gosTextureHandle( blipHandle );
	gos_SetRenderState( gos_State_Texture, gosID );

	for ( int i = 0; i < 4; ++i )
	{
		triangle[i].z = 0;
		triangle[i].rhw = .5;
		triangle[i].argb = color;
		triangle[i].frgb = 0;
		triangle[i].u = 0.f;
		triangle[i].v = 0.f;
	}

	

	worldToTacMap( (glm::vec3&)pos, triangle[0] );
	triangle[0].x -= 2.f;
	triangle[1].x = triangle[0].x;
	triangle[2].x = triangle[3].x = triangle[0].x + 4.1f;
	triangle[0].y -= 2.f;
	triangle[3].y = triangle[0].y;
	triangle[1].y = triangle[2].y = triangle[0].y + 4.1f;
	triangle[2].u = triangle[3].u = .250001f;
	triangle[1].v = triangle[2].v = .250001f;

	gos_DrawQuads( triangle, 4 );
}

void GameTacMap::setPos( const GUI_RECT& newPos )
{
	left = newPos.left;
	right = newPos.right;
	top = newPos.top;
	bottom = newPos.bottom;
}



//*************************************************************************************************
// end of file ( TacMap.cpp )

//*************************************************************************************************
// end of file ( gameTacMap.cpp )
