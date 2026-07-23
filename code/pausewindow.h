#ifndef PAUSEWINDOW_H
#define PAUSEWINDOW_H
/*************************************************************************************************\
PauseWindow.h			: Interface for the PauseWindow component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

//*************************************************************************************************
class ControlButton;
struct ButtonData;
class StaticInfo;
class FitIniFile;

#include"utilities.h"

#ifndef AFONT_H
#include"afont.h"
#endif

/**************************************************************************************************
CLASS DESCRIPTION
PauseWindow:
**************************************************************************************************/
struct MoveInfo
{
	float time; // in seconds
	float position; // x pixels
};


class PauseWindow
{
	public:
		
		PauseWindow();
		~PauseWindow();

		void update();
		void render();

		// NOTE (MERGE-WAVE-1): nifty slice-B pause panel (MC2_HUD_IMGUI_PAUSE,
		// usingImGui) superseded by ui-phase1 PAUSE-IMGUI slices below.

		void init( FitIniFile& file );

		void handleClick( int ID );

		// PAUSE-IMGUI-SLICE-1: MC2_UI_IMGUI_PAUSE=1 draws the pause menu as an
		// ImGui window instead of the legacy sprite panel; buttons call the
		// same handleClick() side-effects. Returns true when it handled the
		// render (so render() early-returns). Default (gate off) -> legacy.
		bool renderImGui();
		static bool imguiPauseEnabled();

		// PAUSE-IMGUI-SLICE-3: ABORT/EXIT confirm dialog as ImGui (legacy
		// mcl_dialog look: baked frame + bitmap fonts). Only called from
		// renderImGui() while bPromptToQuit/bPromptToAbort is up.
		void renderImGuiConfirm();

		// UI-COMMAND-CHANNEL-1: lets the file command channel (missiongui.cpp)
		// gate `press dialog.YES/NO` on a confirm dialog actually being up, so
		// a stray command can't poke a LogisticsOKDialog nobody is polling.
		bool confirmDialogUp() const { return bPromptToQuit || bPromptToAbort; }

		bool inRect( int mouseX, int mouseY );

		void begin(){ currentTime = 0.f; }
		void end();
		void begin(bool objectivesOn);

		enum PAUSEBUTTONS
		{
			OBJECTIVES = 0,
			SAVE = 1,
			LOAD = 2,
			OPTIONS = 3,
			RESTART = 4,
			EXIT = 5,
			RETURN = 6
		};

	private:

		ControlButton* buttons;
		ButtonData* buttonData;
		long		buttonCount;
		StaticInfo* statics;
		long		staticCount;
		GUI_RECT	backgrounds[2];

		float		finalReference;
		float		currentPos;
		float		currentTime;

		aFont	font;
		aFont	headerFont;

		bool wasDragging;
		bool objectivesAlreadyOn;

		static MoveInfo moveInfo[8];

		bool		bPromptToQuit;
		bool		bPromptToAbort;

};


//*************************************************************************************************
#endif  // end of file ( PauseWindow.h )
