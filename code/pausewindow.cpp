#define PAUSEWINDOW_CPP
/*************************************************************************************************\
PauseWindow.cpp			: Implementation of the PauseWindow component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"pausewindow.h"
#include"controlgui.h"
#include "../resource.h"
#include"userinput.h"
#include"missiongui.h"
#include"logisticsdialog.h"
#include"gamesound.h"
#include"mission.h"
#include"logisticsdata.h"
#include"multplyr.h"
#include"gameos.hpp"        // PAUSE-IMGUI-SLICE-1: gos_ComputeHudCanvasBox + Environment; gos_GetGLTextureName
#include"imgui.h"           // PAUSE-IMGUI-SLICE-1: ImGui pause page
#include"txmmgr.h"          // PAUSE-IMGUI-SLICE-2: mcTextureManager (baked frame load)
#include<cstdint>           // PAUSE-IMGUI-SLICE-2: intptr_t (ImTextureID cast)

extern float frameLength;

extern bool bInvokeOptionsScreenFlag;
extern long helpTextHeaderID;
extern long helpTextID;
extern unsigned long scenarioResult;
		
extern bool loadInMissionSave;
extern bool saveInMissionSave;

bool aborted = false;

MoveInfo PauseWindow::moveInfo[8] = 
{
	0.0f,	820.f,
	0.04f,	820.f,
	0.28f,	661.f,
	.36f,	678.f,
	.44f,	661.f,
	.52f,	666.f,
	.60f,	661.f,
	3.00f,  661.f
};

PauseWindow::PauseWindow()
{
	buttons = 0;
	buttonData = 0;
	buttonCount = 0;
	statics = 0;
	staticCount = 0;

	finalReference = 0;
	currentTime = 0;
	wasDragging = 0;
	objectivesAlreadyOn = 0;
	bPromptToQuit = bPromptToAbort = 0;

}

//-------------------------------------------------------------------------------------------------

PauseWindow::~PauseWindow()
{
	if ( buttons )
		delete [] buttons;
	if  ( buttonData )
		delete [] buttonData;
	if ( statics )
		delete [] statics;
}

void PauseWindow::update()
{
	if ( bPromptToQuit || bPromptToAbort )
	{
		LogisticsOKDialog::instance()->update();
		if ( LogisticsOKDialog::instance()->getStatus() == LogisticsScreen::YES )
		{
			if ( bPromptToQuit )
				gos_TerminateApplication();
			else {
				//if (MPlayer) {
				//	MPlayer->leaveSession();
				//	}
				//else
				scenarioResult = mis_PLAYER_LOST_BIG;
				aborted = true;
			}

			bPromptToQuit = bPromptToAbort = 0;
		}
		else if ( LogisticsOKDialog::instance()->getStatus() == LogisticsScreen::NO )
		{
			if ( LogisticsOKDialog::instance()->isDone() )
				bPromptToQuit = bPromptToAbort = 0;
		}		
		
	}
	
	// UI-ASPECT-ANCHOR-1: the pause menu is HUD chrome — drawn canvas-remapped
	// by flushHUDBatch — so hit-test in the same space (getMouseHudX/Y inverts
	// the canvas remap; the bottom-band shrink inverse is a no-op up here).
	// Raw getMouseX left the click rects at the pre-remap screen position
	// (clicks landed right of the drawn buttons on wide displays).
	long mouseX = userInput->getMouseHudX();
	long mouseY = userInput->getMouseHudY();

	gosEnum_KeyIndex key;
	bool bShift, bCtrl, bAlt;
	MissionInterfaceManager::instance()->getHotKey( OBJECTVIES_COMMAND_KEY, key, bShift, bCtrl, bAlt );

	
	// hack, mission gui message isn't getting here...
	if ( gos_GetKeyStatus( key ) != KEY_HELD 
		&& gos_GetKeyStatus( key ) == KEY_PRESSED )
	{
		buttons[OBJECTIVES].toggle(); // big big hack.
	}


	if ( currentTime != 0 )
	{
		currentTime += frameLength;
		float p0 = 0.f;
		float p1 = 0.f;
		float t0 = 0.f;
		float t1 = 0.f;
		// figure out position based on time
		for ( int j = 0; j < 7; j++ )
		{
			if ( moveInfo[j].time <= currentTime && moveInfo[j+1].time > currentTime )
			{
				t0 = moveInfo[j].time;
				t1 = moveInfo[j + 1].time;
				p0 = -(800.f - moveInfo[j].position)  + ((float)Environment.screenWidth);
				p1 = -(800.f - moveInfo[j + 1].position) + ((float)Environment.screenWidth);
				break;
			}
		}
		if ( p1 )
		{
			float dT = currentTime - t0;
			float currentPosition = p0 + dT * ( (p1 - p0)/(t1 -t0) );
			float delta = currentPosition - currentPos;
			currentPos += delta;

			for ( int i = 0; i < buttonCount; i++ )
			{
				buttons[i].move( delta, 0 );
			}

			for (int i = 0; i < staticCount; i++ )
			{
				statics[i].move( delta, 0 );				
			}


			for (int i = 0; i < 2; i++ )
			{
				float dif = backgrounds[i].right - backgrounds[i].left;
				backgrounds[i].left = .5 + currentPos;
				backgrounds[i].right = .5 + currentPos + dif;
			}
		}
	}



	// PAUSE-IMGUI-SLICE-1: when the ImGui pause page owns rendering, ImGui
	// handles button clicks — skip the legacy rect hit-test so a click is not
	// double-handled (once by ImGui, once here). Dialog/save/objectives-hotkey
	// logic below still runs.
	for ( int i = 0; !imguiPauseEnabled() && i < buttonCount; i++ )
	{
		if ( buttons[i].location[0].x <= mouseX && mouseX <= buttons[i].location[2].x
				&& mouseY >= buttons[i].location[0].y && mouseY <= buttons[i].location[1].y  )
		{
			if ( buttons[i].isEnabled() )
			{
				helpTextHeaderID = buttonData[i].helpTextHeader;
				helpTextID = buttonData[i].helpTextID;

				long lastX = mouseX - userInput->getMouseXDelta();
				long lastY = mouseY - userInput->getMouseYDelta();

				if ( buttons[i].location[0].x >= lastX || lastX >= buttons[i].location[2].x
				|| lastY <= buttons[i].location[0].y || lastY >= buttons[i].location[1].y )
				{
					soundSystem->playDigitalSample( LOG_HIGHLIGHTBUTTONS );
				}

				if ( buttons[i].state != ControlButton::PRESSED )
					buttons[i].makeAmbiguous( true );
			}
			else
			{
				helpTextHeaderID = 0;
				helpTextID = 0;
				continue;
			}

			if ( userInput->leftMouseReleased() && !wasDragging )
			{
				{
					handleClick( buttons[i].ID );
				}
			}
		}
		else if ( buttons[i].isEnabled() && buttons[i].state != ControlButton::PRESSED  )
			buttons[i].makeAmbiguous( 0 );
	}

	if ( currentTime == 0 )
	{
		currentTime = .0001f;
		currentPos = -(800 - PauseWindow::moveInfo[0].position) + ((float)Environment.screenWidth);
		float delta = backgrounds[0].left - currentPos;

		for ( int i = 0; i < buttonCount; i++ )
		{
			for ( int j = 0; j < 4; j++ )
			{
				buttons[i].location[j].x -= delta;
			}
		}

		for (int i = 0; i < staticCount; i++ )
		{
			for ( int j = 0; j < 4; j++ )
			{
				statics[i].location[j].x -= delta;
			}
		}

	
		for (int i = 0; i < 2; i++ )
		{
			float dif = backgrounds[i].right - backgrounds[i].left;
			backgrounds[i].left = .5 + currentPos;
			backgrounds[i].right = .5 + currentPos + dif;
		}

		
	}

	wasDragging = userInput->wasLeftDrag();

	const char * campaignName = LogisticsData::instance->getCampaignName().Data();
	char campName[1024];
	_splitpath(campaignName,NULL,NULL,campName,NULL);
	if ( MPlayer || 
		LogisticsData::instance->isSingleMission() ||
		(S_stricmp("tutorial",campName) == 0))
	{
		buttons[ SAVE ].disable( true );
		buttons[ LOAD ].disable( true );
	}
	if ( MPlayer )
	{
		buttons[OPTIONS].disable( true );
	}
	
}

// PAUSE-IMGUI-SLICE-1: gate + ImGui pause page. Default OFF -> legacy sprite
// panel, byte-identical. When on, render() delegates to renderImGui(); the
// button side-effects reuse handleClick() so save/load/options/abort/exit/
// return/objectives behave exactly as the legacy panel.
bool PauseWindow::imguiPauseEnabled()
{
	static const bool s_on = []{
		const char* v = getenv("MC2_UI_IMGUI_PAUSE");
		return v && v[0] && v[0] != '0';
	}();
	return s_on;
}

bool PauseWindow::renderImGui()
{
	if ( !currentTime )
		return true;   // paused but pre-init: legacy also draws nothing yet

	// PAUSE-IMGUI-SLICE-2: one-time load of the offline-reconstructed legacy
	// pause frame (baked by tools/ui_art_reconstruct.py --panel Pause into
	// data/art/gui/mc2_pause_frame.png; FULL 800x600 authored canvas). This fn
	// only runs under MC2_UI_IMGUI_PAUSE, so the gate-OFF legacy path never
	// touches the texture manager here.
	static unsigned s_frameGlTex = 0;
	static bool s_frameTried = false;
	if ( !s_frameTried )
	{
		s_frameTried = true;
		DWORD mcH = mcTextureManager->loadTexture(
			"data\\art\\gui\\mc2_pause_frame.png", gos_Texture_Alpha, 0, 0, 0 );
		if ( mcH )
		{
			DWORD gosID = mcTextureManager->get_gosTextureHandle( mcH );
			if ( gosID && gosID != 0xffffffff )
				s_frameGlTex = gos_GetGLTextureName( gosID );
		}
	}

	// Panel authored rect from buttonLayout800.fit Pause statics bbox:
	// origin (638,86) size 162x239 in the 800x600 design space. Map into the
	// 16:9 HUD canvas box so it tracks the same aspect anchor as the HUD chrome.
	int bx = 0, by = 0, bw = 0, bh = 0;
	if ( !gos_ComputeHudCanvasBox( Environment.drawableWidth, Environment.drawableHeight,
			&bx, &by, &bw, &bh ) )
	{ bx = 0; by = 0; bw = Environment.drawableWidth; bh = Environment.drawableHeight; }
	const float sx = (float)bw / 800.0f;
	const float sy = (float)bh / 600.0f;
	const float px = bx + 638.0f * sx;
	const float py = by +  86.0f * sy;
	const float pw = 162.0f * sx;
	const float ph = 239.0f * sy;

	// authored (800x600) -> display-pixel mapping (same anchor as px/py above)
	auto AX = [&]( float ax ){ return bx + ax * sx; };
	auto AY = [&]( float ay ){ return by + ay * sy; };

	ImGui::SetNextWindowPos( ImVec2( px, py ) );
	ImGui::SetNextWindowSize( ImVec2( pw, ph ) );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0.0f, 0.0f ) );
	ImGui::Begin( "##mc2_pause", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoBackground );   // baked frame supplies the chrome

	ImDrawList* dl = ImGui::GetWindowDrawList();

	// Black backing behind the frame — the legacy PauseBackRect (661,91..795,304)
	// is drawn 0xff000000. Fills the header gap + slot interiors so any
	// transparent parts of the baked PNG read black like the legacy panel.
	dl->AddRectFilled( ImVec2( AX(661.0f), AY(91.0f) ), ImVec2( AX(795.0f), AY(304.0f) ),
		IM_COL32( 0, 0, 0, 255 ) );

	// Baked legacy frame: hazard-stripe border + blue metal + 7 blue button
	// slots. FULL 800x600 PNG -> sample the authored panel sub-rect
	// (638,86)..(800,325).
	if ( s_frameGlTex )
	{
		ImVec2 uv0( 638.0f / 800.0f, 86.0f / 600.0f );
		ImVec2 uv1( 800.0f / 800.0f, 325.0f / 600.0f );
		dl->AddImage( (ImTextureID)(intptr_t)s_frameGlTex,
			ImVec2( px, py ), ImVec2( px + pw, py + ph ), uv0, uv1 );
	}

	// Header "GAME PAUSED" centered in the authored header rect (661,91..795,115),
	// legacy header font color 0xff5c96c2.
	char header[256];
	cLoadString( IDS_GAMEPAUSED, header, 256 );
	{
		const ImVec2 ts = ImGui::CalcTextSize( header );
		const float hcx = ( AX(661.0f) + AX(795.0f) ) * 0.5f;
		const float hcy = ( AY(91.0f)  + AY(115.0f) ) * 0.5f;
		dl->AddText( ImVec2( hcx - ts.x * 0.5f, hcy - ts.y * 0.5f ),
			IM_COL32( 0x5C, 0x96, 0xC2, 255 ), header );
	}

	// Buttons over the baked slots. Authored slot0 (664,116,128,22), 27px Y step
	// (PauseButton0..6). Invisible hit-rects reuse handleClick(); labels centered.
	struct PB { int id; const char* label; };
	static const PB kButtons[] = {
		{ OBJECTIVES, "OBJECTIVES" }, { SAVE, "QUICK SAVE" }, { LOAD, "QUICK LOAD" },
		{ OPTIONS, "OPTIONS" }, { RESTART, "ABORT MISSION" }, { EXIT, "EXIT GAME" },
		{ RETURN, "RETURN TO GAME" },
	};
	for ( int i = 0; i < 7; i++ )
	{
		const PB& b = kButtons[i];
		const float s0x = AX( 664.0f );
		const float s0y = AY( 116.0f + i * 27.0f );
		const float sw  = 128.0f * sx;
		const float sh  =  22.0f * sy;
		const bool enabled = ( b.id < buttonCount ) ? buttons[b.id].isEnabled() : true;

		ImGui::SetCursorScreenPos( ImVec2( s0x, s0y ) );
		const bool clicked = ImGui::InvisibleButton( b.label, ImVec2( sw, sh ) );
		const bool hovered = enabled && ImGui::IsItemHovered();
		if ( enabled && clicked )
			handleClick( b.id );

		// hover tint over the slot (legacy highlights the pressed/hover slot)
		if ( hovered )
			dl->AddRectFilled( ImVec2( s0x, s0y ), ImVec2( s0x + sw, s0y + sh ),
				IM_COL32( 0x5C, 0x96, 0xC2, 48 ) );

		// label color: legacy TextNormal 0x005392 / TextHighlight 0x5C96C2 /
		// TextDisabled 0x7B7B7B. Use the brighter highlight tone as the resting
		// color (matches the legacy screenshot's readable blue).
		const ImU32 col = enabled
			? ( hovered ? IM_COL32( 0xC8, 0xE6, 0xFF, 255 ) : IM_COL32( 0x5C, 0x96, 0xC2, 255 ) )
			: IM_COL32( 0x7B, 0x7B, 0x7B, 255 );
		const ImVec2 ts = ImGui::CalcTextSize( b.label );
		dl->AddText( ImVec2( s0x + ( sw - ts.x ) * 0.5f, s0y + ( sh - ts.y ) * 0.5f ),
			col, b.label );
	}

	ImGui::End();
	ImGui::PopStyleVar( 1 );   // WindowPadding

	// Confirm dialogs stay on the legacy path this slice.
	if ( bPromptToQuit || bPromptToAbort )
		LogisticsOKDialog::instance()->render();
	return true;
}

void PauseWindow::render()
{
	// PAUSE-IMGUI-SLICE-1: ImGui page when gated on (buttons reuse handleClick).
	if ( imguiPauseEnabled() )
	{
		renderImGui();
		return;
	}
	if ( !currentTime )
		return;
	drawRect( backgrounds[0], 0xff000000 );
	drawRect( backgrounds[1], 0xff000000 );

	for ( int i = 0; i < buttonCount; i++ )
	{
		buttons[i].render();
	}

	for (int i = 0; i < staticCount; i++ )
	{
		statics[i].render();
	}	


	char buffer[256];
	cLoadString( IDS_GAMEPAUSED, buffer, 256 );
	headerFont.render( buffer, backgrounds[1].left, backgrounds[1].top, 
		backgrounds[1].right - backgrounds[1].left,
		backgrounds[1].bottom - backgrounds[1].top,
		0xff5c96c2, 0, 3 );
		

	if ( bPromptToQuit || bPromptToAbort )
	{
		LogisticsOKDialog::instance()->render();
	}


}

void PauseWindow::init( FitIniFile& file )
{
	file.seekBlock( "PauseWindow" );
	file.readIdLong( "ButtonCount", buttonCount );
	file.readIdLong( "staticCount", staticCount );

	if ( buttons )
		delete [] buttons;

	if ( buttonData )
		delete [] buttonData;

	if ( statics )
		delete [] statics;

	buttonData = 0;
	buttons = 0;
	statics = 0;
		

	if ( buttonCount )
	{
		buttons = new ControlButton[buttonCount];
		buttonData = new ButtonData[buttonCount];
		font.init( IDS_PAUSEBUTTON800 );
		headerFont.init( IDS_PAUSEDFONT_800 );
		
		ControlButton::initButtons( file, buttonCount, buttons, buttonData, "PauseButton", &font );

		for ( int i = 0; i < buttonCount; i++ )
		{
			buttons[i].move( 0, -ControlGui::hiResOffsetY );
		}
	}

	if ( staticCount )
	{
		statics = new StaticInfo[staticCount];
		char buffer[256];
		for ( int i = 0; i < staticCount; i++ )
		{
			sprintf( buffer, "PauseStatic%ld", i );
			statics[i].init( file, buffer, ControlGui::hiResOffsetX, 0);
		}
	}

	if ( NO_ERR == file.seekBlock( "PauseBackRect" ) )
	{
		file.readIdLong( "left", backgrounds[0].left );
		file.readIdLong( "right", backgrounds[0].right );
		file.readIdLong( "top", backgrounds[0].top );
		file.readIdLong( "bottom", backgrounds[0].bottom );

		backgrounds[0].left += ControlGui::hiResOffsetX;
		backgrounds[0].right += ControlGui::hiResOffsetX;
//		backgrounds[0].top += ControlGui::hiResOffsetY;
//		backgrounds[0].bottom += ControlGui::hiResOffsetY;
	}

	if ( NO_ERR == file.seekBlock( "PauseText" ) )
	{
		file.readIdLong( "left", backgrounds[1].left );
		file.readIdLong( "right", backgrounds[1].right );
		file.readIdLong( "top", backgrounds[1].top );
		file.readIdLong( "bottom", backgrounds[1].bottom );

		backgrounds[1].left += ControlGui::hiResOffsetX;
		backgrounds[1].right += ControlGui::hiResOffsetX;
//		backgrounds[1].top += ControlGui::hiResOffsetY;
//		backgrounds[1].bottom += ControlGui::hiResOffsetY;
	}


}

void PauseWindow::handleClick( int id )
{
	int sound = LOG_SELECT;
	switch ( id )
	{
	case OBJECTIVES:
		if ( buttons[OBJECTIVES].state == ControlButton::PRESSED )
		{
			ControlGui::instance->startObjectives( 0 );		
			buttons[OBJECTIVES].press( 0 );
			objectivesAlreadyOn = 0;
		}
		else
		{
			ControlGui::instance->startObjectives( 1 );		
			buttons[OBJECTIVES].press( 1 );
		}
		break;
	case SAVE:
		if (!MPlayer && !LogisticsData::instance->isSingleMission())
			saveInMissionSave = true;
		else
			sound = INVALID_GUI;
		break;
	case LOAD:
		if (!MPlayer && !LogisticsData::instance->isSingleMission())
			loadInMissionSave = true;
		else
			sound = INVALID_GUI;
		break;
	case RESTART:
		bPromptToAbort = true;
		LogisticsOKDialog::instance()->setText( IDS_DIALOG_ABORT_PROMPT,
				IDS_DIALOG_NO, IDS_DIALOG_YES );				
		LogisticsOKDialog::instance()->begin();
		sound = LOG_MAINMENUBUTTON;
		break;
	case EXIT:
		// i suppose I should put a do you really mean it dialog here
		bPromptToQuit = true;
		LogisticsOKDialog::instance()->setText( IDS_DIALOG_QUIT_PROMPT,
					IDS_DIALOG_NO, IDS_DIALOG_YES );				
		LogisticsOKDialog::instance()->begin();
		sound = LOG_MAINMENUBUTTON;
		break;
	case OPTIONS:
		if (!MPlayer)
		{
			sound = LOG_CLICKONBUTTON;
			bInvokeOptionsScreenFlag = true;
		}
		else
		{
			sound = INVALID_GUI;
		}
		//break;
		//fallthrough
	case RETURN:
		MissionInterfaceManager::instance()->togglePause();
		if ( buttons[OBJECTIVES].state & ControlButton::PRESSED && !objectivesAlreadyOn )
		{
			ControlGui::instance->startObjectives( 0 );		
			buttons[OBJECTIVES].press( 0 );
		}
	
		break;
	}

	soundSystem->playDigitalSample( sound );
}

bool PauseWindow::inRect( int mouseX, int mouseY )
{
	return ( mouseX >= backgrounds[0].left && mouseX <= backgrounds[0].right &&
		 mouseY >= backgrounds[0].top && mouseY <= backgrounds[0].bottom );

}

void PauseWindow::end()
{ 
	currentTime = 5.f;
	if ( buttons[OBJECTIVES].state & ControlButton::PRESSED && !objectivesAlreadyOn )
	{
		ControlGui::instance->startObjectives( 0 );		
		buttons[OBJECTIVES].press( 0 );
	}
}

void PauseWindow::begin(bool objectivesOn)
{
	currentTime = 0.f;
	objectivesAlreadyOn = objectivesOn;
	if ( !objectivesAlreadyOn )
		buttons[OBJECTIVES].press( 0 );
	else
		buttons[OBJECTIVES].press( 1 );
}
//*************************************************************************************************
// end of file ( PauseWindow.cpp )
