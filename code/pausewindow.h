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

		// MC2_HUD_IMGUI_PAUSE (IN-MISSION-HUD-IMGUI-SCOPE-1 slice B):
		// true when the gated ImGui render path owns this window this
		// session (gate ON and not pinned-to-legacy). Legacy render AND
		// the legacy hover/click hit-test loop are skipped while true.
		bool usingImGui() const;

		void init( FitIniFile& file );

		void handleClick( int ID );

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

		// slice B: ImGui render body; returns false -> render legacy this
		// frame (helper pins the session to legacy on hard failure).
		bool renderImGui();

};


//*************************************************************************************************
#endif  // end of file ( PauseWindow.h )
