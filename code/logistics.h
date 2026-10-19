//******************************************************************************************
// logistics.h - This file contains the logistics class header
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef LOGISTICS_H
#define LOGISTICS_H
//----------------------------------------------------------------------------------
// Include Files
#ifndef MCLIB_H
#include"mclib.h"
#endif

#ifndef MC2movie_H
#include"mc2movie.h"
#endif

#include"logisticsdata.h"
#include <memory>

//----------------------------------------------------------------------------------
// Macro Definitions
inline constexpr int log_DONE = 0;
inline constexpr int log_STARTMISSIONFROMCMDLINE = 1;
inline constexpr int log_SPLASH = 2;
inline constexpr int log_RESULTS = 3;
inline constexpr int log_ZONE = 4;

class MissionResults;
class MissionBegin;


//----------------------------------------------------------------------------------
class Logistics
{
	protected:
	
		
		bool							active;						//Am I currently in control?
		
		long							logisticsState;
		long							prevState;					//Used to cleanup previous state
		
		LogisticsData					logisticsData;

		
		std::unique_ptr<MissionResults>					missionResults;
		std::unique_ptr<MissionBegin>					missionBegin;
	
	public:
	
		Logistics (void);
		
		~Logistics (void);
		
		void init (void);
		
		void destroy (void);
		// Frees the ABL modules the front end holds; the shutdown calls it before Mission::destroy closes ABL (BUGS row 76).
		void releaseAblModules (void);
		
		void initSplashScreen (char *screenFile, char *artFile);
		void destroySplashScreen (void);
		
		void start (long logState);		//Actually Starts execution of logistics in state Specified
		bool initObjectiveFixture(const char* missionName);
		void stop (void);				//Guess what this does!
				
		long update (void);
		
		void render (void);

		void setResultsHostLeftDlg( const char* pName );
		
		void setLogisticsState (long state)
		{
			prevState = logisticsState;
			logisticsState = state;
		}

		MissionBegin *getMissionBegin (void)
		{
			return missionBegin.get();
		}

		static int _stdcall beginMission( void*, int, void*[] );

		int DoBeginMission();
		void playFullScreenVideo( const char* fileName );

		MC2MoviePtr	bMovie;


	private:

		

		void initializeLogData();
		bool		bMissionLoaded;
		bool objectiveFixture = false;
		long		lastMissionResult;
};

extern Logistics *logistics;
//----------------------------------------------------------------------------------
#endif
