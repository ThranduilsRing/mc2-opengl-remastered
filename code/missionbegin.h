#ifndef MISSIONNBEGIN_H
#define MISSIONNBEGIN_H
/*************************************************************************************************\
MissionBegin.h			: Interface for the MissionBegin component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

//*************************************************************************************************

/**************************************************************************************************
CLASS DESCRIPTION
MissionBegin:
**************************************************************************************************/

#include"asystem.h"
#include"alistbox.h"
#include"aanim.h"
#include"abl.h"
#include <memory>
#include <string>

class LogisticsScreen;
class MainMenu;

class MissionBegin
{
	public:

		static constexpr int DIM_SCREEN_X = 5;
		static constexpr int DIM_SCREEN_Y = 3;

		//Tutorial - SAVE with savegame, please!!
		static bool 				FirstTimePurchase;
		static bool					FirstTimeMechLab;

		MissionBegin();
		~MissionBegin();

		void init();

		const char* update();
		void render();

		void begin();
		void end();
		// The tutorial brain lives on the ABL pools: whoever closes ABL first frees it (BUGS row 76).
		void adoptLogisticsBrain(std::unique_ptr<ABLModule> brain) { logisticsBrain = std::move(brain); }
		void releaseLogisticsBrain() { logisticsBrain.reset(); }
		bool hasLogisticsBrain() const { return logisticsBrain != nullptr; }
		bool isDone(){ return bDone; }
		bool readyToLoad() { return bReadyToLoad; }

		void beginSplash( const char* playerName = 0 );

		void beginMPlayer();
		void beginSPlayer();
		void beginZone();
		void setToMissionBriefing();

		long getCurrentScreenId();	//Returns screen ID as a function of curScreenX and curScreenY
		bool startAnimation (long bId, bool isButton, float scrollTime, long nFlashes);

		bool isInCalloutAnimation()
		{
			return animationRunning;
		}

		void beginAtConnectionScreen();

		// DEV-SHELL-1 ui_reload: re-read the ACTIVE front-end screen's .fit
		// layout from disk and re-init the screen in place (LogisticsScreen::
		// init calls clear() first — idempotent). Single-player grid + main
		// menu only; substituted cells (skipLogistics reroutes) refuse.
		bool reloadCurrentScreenLayout( std::string& infoOut, std::string& errOut );

		void restartMPlayer( const char* playerName);

	private:

		// MissionBegin::update phases (missionbegin.cpp); callers use update().
		void updateDumpFrontEndState();
		bool updateSplash();
		void updateTutorialAnimation(LogisticsScreen* pCurScreen);
		void updateSoakAutoPurchase();
		void updateSoakScreenWalk(LogisticsScreen* pCurScreen, bool& soakForceNext);
		void updateScreenAdvance(LogisticsScreen* pCurScreen, bool& soakForceNext);

		bool						bDone;
		bool						bReadyToLoad;

		std::unique_ptr<ABLModule>	logisticsBrain;	// owner; freed before closeABL (end(), Logistics::releaseAblModules)
		long						logisticsScriptHandle;

		aAnimation					leftAnim;
		aAnimation					rightAnim;
		aAnimation					upAnim;
		aAnimation					downAnim;
	
		bool						bMultiplayer;

		// Raw on purpose: beginMPlayer() re-runs from restartMPlayer() while an old MP screen may still be
		// on the call stack; the old screens leak instead of being deleted under it.
		LogisticsScreen*			multiplayerScreens[DIM_SCREEN_X][DIM_SCREEN_Y];
		std::unique_ptr<LogisticsScreen>	singlePlayerScreens[DIM_SCREEN_X][DIM_SCREEN_Y];	// owned; built once by begin()
		LogisticsScreen*			screens[DIM_SCREEN_X][DIM_SCREEN_Y];	// non-owning grid view
		LogisticsScreen*			placeHolderScreen;	// raw for the same reason as multiplayerScreens

		long						curScreenX;
		long						curScreenY;

		std::unique_ptr<MainMenu>					mainMenu;
		bool						bSplash;
		// The campaign's pre-video handed out by this frame's updateSplash(); empty when there is none.
		std::string					pendingPreVideo;

		bool						animJustBegun;

		void						setUpMultiplayerLogisticsScreens();

		//-----------------------------------------------
		// Tutorial Data
		bool						animationRunning;
		float						timeLeftToScroll;
		long						targetButtonId;
		bool						targetIsButton;
		long						buttonNumFlashes;
		float						buttonFlashTime;
};





//*************************************************************************************************
#endif  // end of file ( MissionBegin.h )
