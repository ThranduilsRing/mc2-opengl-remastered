//******************************************************************************************
// logistics.cpp - This file contains the logistics class header
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//----------------------------------------------------------------------------------

#include "logistics_internal.h"
#include "mc2fmt.h"
#include "../GameOS/gameos/utils/mc2log.h"
#include <string>
#include <string_view>

void DEBUGWINS_print (const char* s, long window = 0);


//----------------------------------------------------------------------------------
//class Logistics
Logistics *logistics = nullptr;


		
//----------------------------------------------------------------------------------
void Logistics::destroy (void)
{
	delete missionBegin;
	missionBegin = nullptr;

	delete missionResults;
	missionResults = nullptr;
}	
		
//----------------------------------------------------------------------------------
void Logistics::start (long startMode)
{
	ZoneScopedN("Logistics::start");
	bMissionLoaded  = 0;
	userInput->setMouseCursor( mState_LOGISTICS );
//	userInput->mouseOn();

	// ---- MC2_LOG_CURSOR diagnostics (instrumentation only) ----
	// Logistics::start does NOT call mouseOn() (it is commented out) -- it relies
	// on drawMouse already being true from mainmenu.begin / loadscreen. If DEE
	// reaches here with drawMouse==false, the cursor stays invisible the whole
	// logistics/deployment screen. This logs the inherited state on entry.
	if ( mc2::gate::get<"MC2_LOG_CURSOR">() )
	{
		FILE* f = fopen("cursor_debug.log", "a");
		if (f) { mc2::fmt::print(f, "[CURSOR] Logistics::start startMode={:d} drawMouse={:d}\n", mc2::fmt::as_i32(startMode), mc2::fmt::as_i32((int)UserInput::drawMouse)); fflush(f); fclose(f); }
	}

	// FIX (DEE / old-MC2X no-intro-video flow): loadscreen turns the mouse cursor OFF
	// during mission load (loadscreen.cpp:~397) and the original line 115 mouseOn() is
	// commented out -- logistics relied on the cursor already being on from the intro
	// movie / menu path. Campaigns with empty Video fields (DEE/DWE/HPE) skip that path
	// and reach the deployment screen with drawMouse==false -> no cursor for the whole
	// screen. Force it on here: a no-op for stock (already on), restores it for these.
	userInput->mouseOn();

	DWORD localRenderer = prefs.renderer;
	if (prefs.renderer != 0 && prefs.renderer != 3)
		localRenderer = 0;

   	bool localFullScreen = prefs.fullScreen;
   	bool localWindow = !prefs.fullScreen;
   	if (Environment.fullScreen && prefs.fullScreen)
   		localFullScreen = false;

	switch (startMode)
	{
		case log_RESULTS: // pull out results later...
			active = true;
			//Heading back to logistics now.  Change screen back to 800x600
			if (prefs.renderer == 3)
				gos_SetScreenMode(800,600,16,0,0,0,true,localFullScreen,0,localWindow,0,localRenderer);
			else
				gos_SetScreenMode(800,600,16,prefs.renderer,0,0,0,localFullScreen,0,localWindow,0,localRenderer);

			lastMissionResult = scenarioResult;
			if ( !missionResults )
			{
				LogisticsData::instance->init();
				missionResults = new MissionResults;
				missionResults->init();
			}

			if ( scenarioResult == mis_PLAYER_WIN_BIG || MPlayer )
			{
				LogisticsData::instance->removeMechsInForceGroup();
				if ( !MPlayer )
					LogisticsData::instance->setMissionCompleted( );

				// if mission is over, play video then quit when  done
				if ( LogisticsData::instance->campaignOver() && !MPlayer && !LogisticsData::instance->isSingleMission() )
				{
					// CRASH-SOAK harness: campaign reached its end. Under the soak,
					// terminate cleanly instead of running final-video -> splash ->
					// main-menu (which would idle until the harness timed out). The
					// run ends and the parent sees the process exit.
					if ( mc2::gate::get<"MC2_SOAK_AUTOWIN">() )
					{
						mc2::fmt::print_out("[SOAK] campaign-complete -- terminating\n");
						fflush(stdout);
						mission->destroy();
						gos_TerminateApplication();
						break; // skip final video + splash; clean exit
					}
					mission->destroy();
					playFullScreenVideo( LogisticsData::instance->getFinalVideo().c_str() );
					setLogisticsState(log_SPLASH);
					missionBegin->beginSplash();
					userInput->mouseOn();
					char tmp[256];
					cLoadString( IDS_FINAL_MISSION, tmp, 255 );
					FullPathFileName path;
					path.init( savePath, tmp, ".fit" );
					FitIniFile file;
					file.create( path );
					LogisticsData::instance->save(file);
					active = true;
				}
				else if ( LogisticsData::instance->isSingleMission() && !MPlayer )
				{
					missionBegin->beginSplash();
					logisticsState = log_SPLASH;
				}
				else
				{
			
					missionResults->begin();
					logisticsState = log_RESULTS;
				}

			}
			else if ( !justStartMission )
			{
				missionResults->bDone = true;
				logisticsState = log_RESULTS;
			}
			else // end the game
				quitGame = true;

			break;
		case log_SPLASH:
		{
			if (aborted) {
				if (prefs.renderer == 3)
					gos_SetScreenMode(800,600,16,0,0,0,true,localFullScreen,0,localWindow,0,localRenderer);
				else
					gos_SetScreenMode(800,600,16,prefs.renderer,0,0,0,localFullScreen,0,localWindow,0,localRenderer);

				if ( missionBegin )
					missionBegin->beginSplash();

				if ( MPlayer )
				{
					delete MPlayer;
					MPlayer = nullptr;
				}
			}

			// Join any previous bg init thread before (re-)launching.
			if (m_initFuture.valid())
				m_initFuture.get();

			// Launch LogisticsData::init() (mech/variant/availability scan) on a
			// background thread so the intro movie starts without waiting for it.
			// Safety: FastFile globals and g_modIndex are read-only by this point.
			// LogisticsData::init() has an early-return guard after first run.
			// Main thread joins in update() before any interactive LogisticsData use.
			m_initFuture = std::async(std::launch::async, [this]{ initializeLogData(); });

			bool bTestScript = false;
			if ( bTestScript )
			{
				// bTestScript is always false — dead code retained for reference.
				FitIniFile loadFile;
				loadFile.open( "data" PATH_SEPARATOR "missions" PATH_SEPARATOR "save.fit" );
				LogisticsData::instance->load( loadFile );
				FitIniFile saveFile;
				saveFile.open("data" PATH_SEPARATOR "missions" PATH_SEPARATOR "save.fit", CREATE);
				LogisticsData::instance->save( saveFile );

			}

			active = TRUE;
			setLogisticsState(log_SPLASH);

			// [STARTUP-INIT-ASYNC-1 FIX] missionBegin->init() below synchronously
			// reaches MissionSelectionScreen::begin(), which reads logistics mission
			// data (getCurrentOperationFile etc.). Join the bg init future first: a
			// skipped or fast intro otherwise reaches that read before
			// initializeLogData() populates missionInfo->groups -> null-deref crash
			// at LogisticsMissionInfo::getCurrentOperationFile. Normal play (intro
			// plays >5s) finds the future already done, so this join is ~instant.
			if (m_initFuture.valid())
				m_initFuture.get();

			if ( !missionBegin )
			{
				ZoneScopedN("Logistics::start missionBeginInit");
				missionBegin = new MissionBegin;
				missionBegin->beginSplash();
				missionBegin->init();
			}

		}
		break;

		case log_ZONE:
		{
			{ ZoneScopedN("Logistics::start logisticsData.init");
			logisticsData.init();
			}
			if ( !missionBegin )
			{
				ZoneScopedN("Logistics::start missionBeginInit");
				missionBegin = new MissionBegin;
				missionBegin->init();
			}

			missionBegin->beginZone();
			active = TRUE;
			setLogisticsState( log_SPLASH );
			userInput->mouseOn();
			userInput->setMouseCursor( mState_NORMAL );

		}
		break;
	}
}	

//----------------------------------------------------------------------------------
void Logistics::stop (void)
{
	switch (prevState)
	{
		case log_SPLASH:
			break;
			
		case log_RESULTS:
			break;
	}
		
	active = FALSE;
}	

//----------------------------------------------------------------------------------
long Logistics::update (void)
{
	//MUST do this every frame.  The movie movies will play wrong, otherwise!!

	if ( bMovie )
	{
		userInput->mouseOff();
		if (userInput->getKeyDown(KEY_SPACE) || userInput->getKeyDown(KEY_ESCAPE) || userInput->getKeyDown(KEY_LMOUSE))
		{
			bMovie->stop();
		}

		bool result = bMovie->update();
		if (result)
		{
			if ( LogisticsData::instance->campaignOver() && (S_strnicmp(bMovie->getMovieName(),"credits",7) == 0) )
			{
				missionBegin->beginSplash();
			}

			//Check if we are cinema5.  If we are, spool up the credits and play those.
			// Otherwise, same as before Mr. Sulu.  Stay with him...
			if (S_strnicmp(bMovie->getMovieName(),"cinema5",7) == 0)
			{
				//OLD Movie's Over.
				//Whack it.
				delete bMovie;
				bMovie = nullptr;

				FullPathFileName path;
				path.init( moviePath, "credits", ".bik" );

				RECT movieRect;
				movieRect.top = 0;
				movieRect.left = 0;
				movieRect.right = Environment.screenWidth;
				movieRect.bottom = Environment.screenHeight;

				bMovie = new MC2Movie;
				bMovie->init(path,movieRect,true);
				soundSystem->playDigitalMusic(33);
			}
			else
			{
				//Movie's Over.
				//Whack it.
				delete bMovie;
				bMovie = nullptr;

				soundSystem->playDigitalMusic(LogisticsData::instance->getCurrentMissionTune());
				userInput->mouseOn();	
			}
		}

		return logisticsState;
	}

	//Don't keep setting the mouse cursor to logistics cursor during mission!!
	if (!mission->isActive())
		userInput->setMouseCursor( mState_LOGISTICS );
	//Stop doing this every frame.  ONLY turn it on when we need to!!

	if ( logisticsState == log_RESULTS )
	{
		if ( missionResults )
			missionResults->update();

		if ( missionResults->isDone() )
		{
			missionResults->end();
			if ( !justStartMission && !MPlayer )
			{
					logisticsState = log_SPLASH;
					if ( !missionBegin )
					{
						missionBegin = new MissionBegin();
						missionBegin->init( );
					}
					
					missionBegin->begin();

 					if ( lastMissionResult == mis_PLAYER_LOST_BIG && !MPlayer )
					{
						if ( LogisticsData::instance->skipLogistics()  )
						{
							if ( LogisticsData::instance->showChooseMission() )
								missionBegin->begin();

							else
								missionBegin->beginSplash();
						}
						else
							missionBegin->setToMissionBriefing();
					}
					else
					{
						const std::string videoName = LogisticsData::instance->getCurrentBigVideo();
						MC2_EVENT(Core, "VIDEO", 1, "event=pre_video site=won_mission name={}", videoName.empty() ? std::string_view( "none" ) : std::string_view( videoName ));

						if ( !videoName.empty() )
						{
							playFullScreenVideo( videoName.c_str() );
						}
					}
				
			}
			else if ( MPlayer )
			{
				logisticsState = log_SPLASH;
				if ( !missionBegin )
				{
					missionBegin = new MissionBegin();
					missionBegin->init( );
				}

				if ( MPlayer->hostLeft || MPlayer->commanderID < 0 )
				{
					missionBegin->beginSplash( nullptr );
					MPlayer->closeSession();
				}
				else
					missionBegin->restartMPlayer(nullptr);


				
			}
			else
				quitGame = true;
		}
	}
	//Used to start mission from command line, bypassing logistics
	else if (logisticsState == log_STARTMISSIONFROMCMDLINE)
	{
		
	}
	else if ( logisticsState != log_DONE )
	{
		// Join bg init thread (once) before driving the main menu.
		// Runs in parallel with MissionBegin::begin() + initDialogs in InitializeGameEngine,
		// so this join is usually instant (thread done before first update() call).
		// Blocks at most (T_init - T_missionBeginSetup) on the very first update() frame.
		if (m_initFuture.valid())
			m_initFuture.get();

		if ( missionBegin )
		{
			const char* pVid =  missionBegin->update();
			if ( pVid && (0 != strcmp("", pVid)) )
			{
				playFullScreenVideo( pVid );
				return logisticsState;
			}
			if ( missionBegin->readyToLoad() && !bMissionLoaded )
			{
				missionBegin->end();	//Called to clean up ABL!!

				lastMissionResult = 0;

				if (!beginMission(nullptr, 0, nullptr)) {
					missionBegin->beginAtConnectionScreen();
					return(logisticsState);
				}
				bMissionLoaded = true;
			}
			if ( missionBegin->isDone() )
			{				
				logisticsState = log_DONE;
			}
		}
	}

	return(logisticsState);
}	
		
//----------------------------------------------------------------------------------
void Logistics::render (void)
{
	if (bMovie)
	{
		bMovie->render();
	}
	else if ( active )
	{
		if ( logisticsState == log_RESULTS )
			missionResults->render();
		else if ( logisticsState == log_SPLASH && missionBegin )
			missionBegin->render();
	}
}
