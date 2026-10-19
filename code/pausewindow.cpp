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
#include"hud_imgui_font.h"  // FONT-IMGUI-SLICE-1: legacy bitmap-font text
#include"txmmgr.h"          // PAUSE-IMGUI-SLICE-2: mcTextureManager (baked frame load)
#include<cstdint>           // PAUSE-IMGUI-SLICE-2: intptr_t (ImTextureID cast)

// NOTE (MERGE-WAVE-1): nifty slice-B pause panel (no such gate) superseded
// by ui-phase1 PAUSE-IMGUI slices (field-tested). hudimguipanel.h retained for tab registry.
#include"hudimguipanel.h"
#include "imgui.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include "mc2fmt.h"

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
	buttons = nullptr;
	buttonData = nullptr;
	buttonCount = 0;
	statics = nullptr;
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
		buttons.reset();
	if  ( buttonData )
		buttonData.reset();
	if ( statics )
		statics.reset();
}

void PauseWindow::update()
{
	// PAUSE-IMGUI-SLICE-3: MC2_BOOT_PAUSED_CONFIRM=abort|exit fixture (default
	// unset -> byte-identical). ONCE, after the pause menu slide-in has settled,
	// invoke the ABORT MISSION / EXIT GAME confirm dialog through the SAME
	// handleClick() path a user click takes (sets bPromptToAbort/bPromptToQuit +
	// LogisticsOKDialog setText/begin). Headless agents cannot click the pause
	// buttons, so this is the capture fixture for the confirm dialogs (same
	// pattern as MC2_BOOT_PAUSED). Registered (Trace).
	{
		static const std::string s_confirmText = mc2::gate::str<"MC2_BOOT_PAUSED_CONFIRM">();   // "" when unset or empty
		static const char* s_confirm = s_confirmText.c_str();
		static bool s_confirmDone = false;
		if ( s_confirm[0] && !s_confirmDone && currentTime > 0.7f
			&& !bPromptToQuit && !bPromptToAbort )
		{
			s_confirmDone = true;
			handleClick( _stricmp( s_confirm, "exit" ) == 0 ? EXIT : RESTART );
		}
	}

	if ( bPromptToQuit || bPromptToAbort )
	{
		if ( imguiPauseEnabled() )
		{
			// PAUSE-IMGUI-SLICE-3: the ImGui page owns dialog rendering/input —
			// advance only the enter/exit anims + bDone latch (the legacy rect
			// hit-test runs at unremapped screen coords and would double-handle
			// or cross-fire clicks). YES/NO come from the ImGui buttons in
			// renderImGuiConfirm(), which call the SAME handleMessage() the
			// legacy buttons send; the status handling below is shared verbatim.
			LogisticsOKDialog::instance()->updateAnimsOnly();

			// Keyboard mirror of LogisticsDialog::update(): RETURN=YES (gated
			// on the enter anim, like legacy), ESCAPE=NO (ungated, like legacy).
			if ( LogisticsOKDialog::instance()->getEnterAnim().isDone()
				&& gos_GetKeyStatus( KEY_RETURN ) == KEY_RELEASED )
				LogisticsOKDialog::instance()->handleMessage( aMSG_LEFTMOUSEDOWN, LogisticsScreen::YES );
			if ( gos_GetKeyStatus( KEY_ESCAPE ) == KEY_RELEASED )
				LogisticsOKDialog::instance()->handleMessage( aMSG_LEFTMOUSEDOWN, LogisticsScreen::NO );
		}
		else
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

	const std::string& campaignName = LogisticsData::instance->getCampaignName();
	char campName[1024];
	_splitpath(campaignName.c_str(),nullptr,nullptr,campName,nullptr);
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
	static const bool s_on = mc2::gate::get<"MC2_UI_IMGUI_PAUSE">();
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
	// OBJ-IMGUI-SLICE-1 lesson (field-confirmed 2026-07-18: font-atlas garbage
	// in the frame rect after other UI churned the texture cache): pin
	// (nFlush=0x1) — an unpinned texture that never draws through the legacy
	// quad path gets cached out mid-mission and its GL name recycled — and
	// re-resolve the GL name every frame.
	static DWORD s_frameMcH = 0;
	static bool s_frameTried = false;
	if ( !s_frameTried )
	{
		s_frameTried = true;
		s_frameMcH = mcTextureManager->loadTexture(
			"data\\art\\gui\\mc2_pause_frame.png", gos_Texture_Alpha, 0, 0, 0x1 );
	}
	unsigned s_frameGlTex = 0;
	if ( s_frameMcH )
	{
		DWORD gosID = mcTextureManager->get_gosTextureHandle( s_frameMcH );
		if ( gosID && gosID != 0xffffffff )
			s_frameGlTex = gos_GetGLTextureName( gosID );
	}

	// POLISH-IMGUI-SLICE-1 note: a PRESSED-state bake was evaluated (fit
	// UPressed/VPressed — PauseButton0 (0,22) vs normal (0,0)) but the two
	// crops are PIXEL-IDENTICAL in the shipped MCUI_Universal1 atlas, so the
	// legacy pressed feedback is entirely the label color (fit TextPressed
	// 0xffffffff via textColors[PRESSED]) — handled in the button loop below.

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
	// legacy header font color 0xff5c96c2. FONT-IMGUI-SLICE-1: drawn with the
	// legacy bitmap headerFont (IDS_PAUSEDFONT_800) via HudFont; centering
	// mirrors the legacy WrapType-3 math (advance-sum width, lineSkip height)
	// in authored pixels, then maps through the canvas anchor.
	char header[256];
	cLoadString( IDS_GAMEPAUSED, header, 256 );
	{
		HGOSFONT3D hf = headerFont.getTempHandle();
		const ImVec2 tsA = HudFont::Measure( hf, header, 1.0f, 1.0f );
		const float hxA = 661.0f + ( ( 795.0f - 661.0f ) - tsA.x ) * 0.5f;
		const float hyA =  91.0f + ( ( 115.0f -  91.0f ) - tsA.y ) * 0.5f;
		HudFont::Draw( dl, hf, AX( hxA ), AY( hyA ),
			IM_COL32( 0x5C, 0x96, 0xC2, 255 ), header, sx, sy );
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
		// POLISH-IMGUI-SLICE-1: while the mouse is down the legacy panel is in
		// state PRESSED. The pressed BITMAP is pixel-identical to normal in the
		// shipped atlas (see note above), so the visible legacy feedback is the
		// label color (fit TextPressed via textColors[PRESSED]) — mapped in the
		// state logic below.
		const bool heldNow = enabled && ImGui::IsItemActive();
		if ( enabled && clicked )
			handleClick( b.id );

		// hover tint over the slot (legacy highlights the pressed/hover slot);
		// skip while held (legacy pressed state shows no hover highlight).
		if ( hovered && !heldNow )
			dl->AddRectFilled( ImVec2( s0x, s0y ), ImVec2( s0x + sw, s0y + sh ),
				IM_COL32( 0x5C, 0x96, 0xC2, 48 ) );

		// FONT-IMGUI-SLICE-1: label text + font + state colors from the SAME
		// ButtonData the legacy ControlButton::render uses (localized textID
		// string, bitmap textFont, fit-loaded textColors[state]); centering
		// mirrors the legacy math — X by advance-sum width (WrapType 2), Y by
		// visual ink bounds — in authored pixels.
		char label[256];
		label[0] = 0;
		HGOSFONT3D bf = nullptr;
		ImU32 col = enabled
			? ( hovered ? IM_COL32( 0xC8, 0xE6, 0xFF, 255 ) : IM_COL32( 0x5C, 0x96, 0xC2, 255 ) )
			: IM_COL32( 0x7B, 0x7B, 0x7B, 255 );
		if ( b.id < buttonCount && buttons[b.id].data )
		{
			ButtonData* bd = buttons[b.id].data;
			if ( bd->textID )
				cLoadString( bd->textID, label, 256 );
			bf = bd->textFont.getTempHandle();
			// POLISH-IMGUI-SLICE-1: held -> PRESSED (fit TextPressed, white),
			// matching legacy ControlButton::render's textColors[state].
			const int state = !enabled ? ControlButton::DISABLED
				: ( heldNow ? ControlButton::PRESSED
				: ( hovered ? ControlButton::AMBIGUOUS : buttons[b.id].state ) );
			const unsigned long argb = (unsigned long)bd->textColors[
				( state >= 0 && state < 4 ) ? state : 0 ];
			col = IM_COL32( (argb >> 16) & 0xff, (argb >> 8) & 0xff,
				argb & 0xff, 255 );
		}
		if ( !label[0] )
			strcpy( label, b.label );

		const float slotXA = 664.0f;
		const float slotYA = 116.0f + i * 27.0f;
		const ImVec2 tsA = HudFont::Measure( bf, label, 1.0f, 1.0f );
		float lyA = slotYA + ( 22.0f - tsA.y ) * 0.5f;
		if ( bf )
		{
			DWORD visW; int visTop, visBot;
			gos_TextSetAttributes( bf, 0, 1.0f, true, true, false, false, 2 );
			gos_TextVisualBounds( &visW, &visTop, &visBot, "%s", label );
			lyA = slotYA + 11.0f - (float)( ( visTop + visBot + 1 ) / 2 );
		}
		HudFont::Draw( dl, bf,
			AX( slotXA + ( 128.0f - tsA.x ) * 0.5f ), AY( lyA ),
			col, label, sx, sy );
	}

	ImGui::End();
	ImGui::PopStyleVar( 1 );   // WindowPadding

	// PAUSE-IMGUI-SLICE-3: ABORT/EXIT confirm dialogs as ImGui with the legacy
	// look (baked mcl_dialog frame + legacy bitmap fonts). Drawn AFTER the
	// pause window so it sits on top.
	if ( bPromptToQuit || bPromptToAbort )
		renderImGuiConfirm();
	return true;
}

// PAUSE-IMGUI-SLICE-3: the LogisticsOKDialog (mcl_dialog.fit) recreated as an
// ImGui window. Chrome = offline-reconstructed frame (tools/ui_art_reconstruct.py
// on mcl_dialog.fit statics+buttons -> data/art/gui/mc2_dialog_frame.png, FULL
// 800x600 authored canvas; repo copy data/ui_reconstructed/dialog_frame.png).
// Layout constants below are the authored mcl_dialog.fit values; the enter/exit
// slide + screen dim replay the live legacy anims (getEnterAnim/getExitAnim).
// YES/NO InvisibleButtons send the SAME handleMessage(status) the legacy aButtons
// send — update() reads getStatus() and runs the unchanged abort/quit code.
void PauseWindow::renderImGuiConfirm()
{
	LogisticsOKDialog* dlg = LogisticsOKDialog::instance();
	if ( !dlg )
		return;

	// Baked legacy dialog frame. OBJ-IMGUI-SLICE-1 lesson: pin (nFlush=0x1) —
	// an unpinned texture that never draws through the legacy quad path gets
	// cached out mid-mission — and re-resolve the GL name every frame.
	static DWORD s_dlgFrameMcH = 0;
	static bool s_dlgTried = false;
	if ( !s_dlgTried )
	{
		s_dlgTried = true;
		s_dlgFrameMcH = mcTextureManager->loadTexture(
			"data\\art\\gui\\mc2_dialog_frame.png", gos_Texture_Alpha, 0, 0, 0x1 );
	}
	unsigned dlgTex = 0;
	if ( s_dlgFrameMcH )
	{
		DWORD gosID = mcTextureManager->get_gosTextureHandle( s_dlgFrameMcH );
		if ( gosID && gosID != 0xffffffff )
			dlgTex = gos_GetGLTextureName( gosID );
	}

	// POLISH-IMGUI-SLICE-1 note: a PRESSED-state bake was evaluated (fit
	// UPressed/VPressed (50,177) vs UNormal (50,118), mcl_dialog.fit) but the
	// two crops are PIXEL-IDENTICAL in the shipped mcl_dialog_3 atlas — the
	// legacy pressed feedback is entirely the TEXT color animation
	// (AnimateText=1 / AnimateBmp=0), replayed in the button loop below.

	// Dialog fonts (mcl_dialog.fit: buttons Font=27655 IDS_DIALOG_FONT,
	// Text0 Font=27656 IDS_DIALOG_TEXT_FONT).
	static aFont s_dlgBtnFont;
	static aFont s_dlgTextFont;
	static bool s_fontsInit = false;
	if ( !s_fontsInit )
	{
		s_fontsInit = true;
		s_dlgBtnFont.init( IDS_DIALOG_FONT );
		s_dlgTextFont.init( IDS_DIALOG_TEXT_FONT );
	}

	// Same authored->display anchor as the pause page (the legacy dialog draw
	// is HUD-scale-EXEMPT, so only the canvas remap applies).
	int bx = 0, by = 0, bw = 0, bh = 0;
	if ( !gos_ComputeHudCanvasBox( Environment.drawableWidth, Environment.drawableHeight,
			&bx, &by, &bw, &bh ) )
	{ bx = 0; by = 0; bw = Environment.drawableWidth; bh = Environment.drawableHeight; }
	const float sx = (float)bw / 800.0f;
	const float sy = (float)bh / 600.0f;

	// Live legacy anims: slide offset + screen-dim fade (LogisticsDialog::render).
	aAnimation& enterAnim = dlg->getEnterAnim();
	aAnimation& exitAnim  = dlg->getExitAnim();
	float xOffA = 0.f, yOffA = 0.f;
	unsigned long dim = 0x7f000000;
	if ( enterAnim.isAnimating() && !enterAnim.isDone() )
	{
		float endTime = enterAnim.getMaxTime();
		if ( endTime )
			dim = interpolateColor( 0x00000000, 0x7f000000, enterAnim.getCurrentTime() / endTime );
	}
	else if ( exitAnim.isAnimating() && !exitAnim.isDone() )
	{
		float endTime = exitAnim.getMaxTime();
		if ( endTime )
			dim = interpolateColor( 0x7f000000, 0x00000000, exitAnim.getCurrentTime() / endTime );
	}
	if ( enterAnim.isAnimating() )
	{
		xOffA = enterAnim.getXDelta();
		yOffA = enterAnim.getYDelta();
	}
	else if ( exitAnim.isAnimating() )
	{
		xOffA = exitAnim.getXDelta();
		yOffA = exitAnim.getYDelta();
	}

	auto AX = [&]( float ax ){ return bx + ( ax + xOffA ) * sx; };
	auto AY = [&]( float ay ){ return by + ( ay + yOffA ) * sy; };
	auto ARGB = []( unsigned long c ){
		return IM_COL32( (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff, (c >> 24) & 0xff ); };

	const float dispW = (float)Environment.drawableWidth;
	const float dispH = (float)Environment.drawableHeight;

	// Fullscreen modal window: hosts the dim, the dialog, and the buttons; being
	// fullscreen it also keeps clicks from reaching the pause-menu buttons under
	// the dim (the legacy quirk let those through — intentional modal fix).
	ImGui::SetNextWindowPos( ImVec2( 0.f, 0.f ) );
	ImGui::SetNextWindowSize( ImVec2( dispW, dispH ) );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0.0f, 0.0f ) );
	ImGui::Begin( "##mc2_pause_confirm", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav );

	ImDrawList* dl = ImGui::GetWindowDrawList();

	// Screen dim (legacy: full-screen 0x7f000000, anim-faded).
	dl->AddRectFilled( ImVec2( 0.f, 0.f ), ImVec2( dispW, dispH ), ARGB( dim ) );

	// Legacy draw order (LogisticsScreen::render(x,y)): opaque rects -> statics
	// -> outline rects -> buttons -> texts.
	// Rect0: black backing behind the text area (237,256, 326x93).
	dl->AddRectFilled( ImVec2( AX(237.f), AY(256.f) ), ImVec2( AX(563.f), AY(349.f) ),
		IM_COL32( 0, 0, 0, 255 ) );

	// Baked frame (statics + NORMAL-state button bitmaps): authored bbox
	// (187,0)..(613,467) sampled from the FULL 800x600 canvas PNG.
	if ( dlgTex )
	{
		ImVec2 uv0( 187.0f / 800.0f,   0.0f / 600.0f );
		ImVec2 uv1( 613.0f / 800.0f, 467.0f / 600.0f );
		dl->AddImage( (ImTextureID)(intptr_t)dlgTex,
			ImVec2( AX(187.f), AY(0.f) ), ImVec2( AX(613.f), AY(467.f) ), uv0, uv1 );
	}

	// Rect1: outline around the text area (247,266, 305x72, color 0xff43311c).
	const float lineTh = ( sx + sy ) * 0.5f;
	dl->AddRect( ImVec2( AX(247.f), AY(266.f) ), ImVec2( AX(552.f), AY(338.f) ),
		ARGB( 0xff43311c ), 0.f, 0, lineTh );

	const bool enterDone = enterAnim.isDone();
	const bool running = ( dlg->getStatus() == LogisticsScreen::RUNNING );

	// YES/NO buttons (fit Button1/Button0). Bitmaps are baked (pressed crop is
	// pixel-identical — see note above); live parts = InvisibleButton hit-rect
	// (full 128x59 bitmap rect), centered label + TextOutline rect at the fit
	// textRect, legacy text-color ANIMS (POLISH-IMGUI-SLICE-1) below.
	struct DB { float x; const char* imId; int strID; long status; };
	static const DB kDlgButtons[2] = {
		{ 237.f, "##dlgNO",  IDS_DIALOG_NO,  (long)LogisticsScreen::NO  },
		{ 435.f, "##dlgYES", IDS_DIALOG_YES, (long)LogisticsScreen::YES },
	};
	// POLISH-IMGUI-SLICE-1: legacy hover/pressed label-color animation.
	// mcl_dialog.fit buttons carry AnimateText=1 / AnimateBmp=0 plus per-state
	// color animations (install data/defs/ui/legacy/game/mcl_dialog.fit):
	//   Highlight (loops): 0.0 -> 0xffffffff, 0.4 -> 0xff5C96C2, 0.75 -> 0xffffffff
	//   Pressed (one-shot): 0.0 -> 0xffffffff, 0.5 -> 0xff5C96C2
	// aAnimButton drives data.textColors[state] from aAnimation::getColor
	// (gui/aanim.cpp: wrap time by the last keyframe when looping, then linear
	// interpolateColor per segment — mclib/utilities.cpp); the anim restarts
	// when the hover begins (gui/abutton.cpp: highlightData.begin() on entry).
	// The outline rect uses the same textColors[state], so it pulses too.
	static float s_hoverT[2] = { -1.f, -1.f };
	static float s_pressT[2] = { -1.f, -1.f };
	auto animColor = []( float t, float tMid, float tEnd, unsigned long cEnds,
			unsigned long cMid, bool loops ) -> unsigned long {
		if ( t > tEnd )
		{
			if ( !loops || tEnd <= 0.f )
				return cMid;   // one-shot: hold the last keyframe color
			t -= (float)(long)( t / tEnd ) * tEnd;   // aAnimation::getColor wrap
		}
		if ( t <= tMid || tEnd - tMid <= 0.f )
			return (unsigned long)interpolateColor( (long)cEnds, (long)cMid,
				tMid > 0.f ? t / tMid : 1.f );
		return (unsigned long)interpolateColor( (long)cMid, (long)cEnds,
			( t - tMid ) / ( tEnd - tMid ) );
	};

	HGOSFONT3D bf = s_dlgBtnFont.getTempHandle();
	for ( int i = 0; i < 2; i++ )
	{
		const DB& b = kDlgButtons[i];
		ImGui::SetCursorScreenPos( ImVec2( AX(b.x), AY(367.f) ) );
		const bool clicked = ImGui::InvisibleButton( b.imId, ImVec2( 128.f * sx, 59.f * sy ) );
		const bool hovered = ImGui::IsItemHovered();
		const bool held = ImGui::IsItemActive();   // mouse down on the button
		if ( enterDone && running && clicked )
			dlg->handleMessage( aMSG_LEFTMOUSEDOWN, (unsigned long)b.status );

		// advance/restart the state anims (mirrors aAnimButton::update():
		// begin on entry, update by frameLength while active)
		s_hoverT[i] = hovered ? ( s_hoverT[i] < 0.f ? 0.f : s_hoverT[i] + frameLength ) : -1.f;
		s_pressT[i] = held    ? ( s_pressT[i] < 0.f ? 0.f : s_pressT[i] + frameLength ) : -1.f;

		char label[64];
		label[0] = 0;
		cLoadString( b.strID, label, 64 );
		// legacy state precedence (aAnimButton::render): pressed > highlight >
		// normal (NormalColor is constant 0xff005392)
		unsigned long argb = 0xff005392;
		if ( held )
			argb = animColor( s_pressT[i], 0.5f, 0.5f, 0xffffffff, 0xff5C96C2, false );
		else if ( hovered )
			argb = animColor( s_hoverT[i], 0.4f, 0.75f, 0xffffffff, 0xff5C96C2, true );

		// textRect: (x+14, 381) 100x30 — label centered like the legacy aButton
		// (align 2 horizontally, visual-ink bounds vertically); TextOutline=TRUE
		// draws an empty rect around the textRect in the text color.
		const float trL = b.x + 14.f, trT = 381.f, trR = trL + 100.f, trB = trT + 30.f;
		float lyA = trT + 15.f;
		if ( bf )
		{
			DWORD visW; int visTop, visBot;
			gos_TextSetAttributes( bf, 0, 1.0f, true, true, false, false, 2 );
			gos_TextVisualBounds( &visW, &visTop, &visBot, "%s", label );
			lyA = ( trT + trB ) * 0.5f - (float)( ( visTop + visBot + 1 ) / 2 );
		}
		const ImVec2 tsA = HudFont::Measure( bf, label, 1.0f, 1.0f );
		HudFont::Draw( dl, bf, AX( trL + ( 100.f - tsA.x ) * 0.5f ), AY( lyA ),
			ARGB( argb ), label, sx, sy );
		dl->AddRect( ImVec2( AX(trL), AY(trT) ), ImVec2( AX(trR), AY(trB) ),
			ARGB( argb ), 0.f, 0, lineTh );
	}

	// Prompt text (fit Text0: rect 247,266 306x73, font 27656, color 0xffc66600,
	// Alignment=3 -> block centered in the rect). String IDs are the ones
	// handleClick() passed to setText().
	{
		char prompt[256];
		prompt[0] = 0;
		cLoadString( bPromptToQuit ? IDS_DIALOG_QUIT_PROMPT : IDS_DIALOG_ABORT_PROMPT,
			prompt, 256 );
		HudFont::DrawWrappedCentered( dl, s_dlgTextFont.getTempHandle(),
			AX(247.f), AY(266.f), AX(553.f), AY(339.f),
			ARGB( 0xffc66600 ), prompt, sx, sy );
	}

	// Legacy click-outside-cancels (LogisticsDialog::update): a left-release
	// outside the dialog panel = NO, only once the enter anim is done.
	if ( enterDone && running && ImGui::IsMouseReleased( 0 ) )
	{
		const ImVec2 mp = ImGui::GetMousePos();
		if ( mp.x < AX(187.f) || mp.x > AX(613.f) || mp.y < AY(0.f) || mp.y > AY(467.f) )
			dlg->handleMessage( aMSG_LEFTMOUSEDOWN, LogisticsScreen::NO );
	}

	ImGui::End();
	ImGui::PopStyleVar( 1 );   // WindowPadding
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
		buttons.reset();

	if ( buttonData )
		buttonData.reset();

	if ( statics )
		statics.reset();

	buttonData = nullptr;
	buttons = nullptr;
	statics = nullptr;
		

	if ( buttonCount )
	{
		buttons = std::make_unique_for_overwrite<ControlButton[]>(buttonCount);
		buttonData = std::make_unique_for_overwrite<ButtonData[]>(buttonCount);
		font.init( IDS_PAUSEBUTTON800 );
		headerFont.init( IDS_PAUSEDFONT_800 );
		
		ControlButton::initButtons( file, buttonCount, buttons.get(), buttonData.get(), "PauseButton", &font );

		for ( int i = 0; i < buttonCount; i++ )
		{
			buttons[i].move( 0, -ControlGui::hiResOffsetY );
		}
	}

	if ( staticCount )
	{
		statics = std::make_unique_for_overwrite<StaticInfo[]>(staticCount);
		char buffer[256];
		for ( int i = 0; i < staticCount; i++ )
		{
			mc2::fmt::to_buf(buffer, "PauseStatic{:d}", mc2::fmt::as_i32(i));
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
