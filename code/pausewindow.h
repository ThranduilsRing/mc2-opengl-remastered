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

		void init( FitIniFile& file );

		void handleClick( int ID );

		// PAUSE-IMGUI-SLICE-1: MC2_UI_IMGUI_PAUSE=1 draws the pause menu as an
		// ImGui window instead of the legacy sprite panel; buttons call the
		// same handleClick() side-effects. Returns true when it handled the
		// render (so render() early-returns). Default (gate off) -> legacy.
		bool renderImGui();
		static bool imguiPauseEnabled();

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
