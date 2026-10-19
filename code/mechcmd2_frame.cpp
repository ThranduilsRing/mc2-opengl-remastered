// Game main: the per-frame renderer update, engine termination and DoGameLogic.
// Declared in the class headers; shared names in mechcmd2_internal.h.
#include "mechcmd2_internal.h"
#include "Gates/gate.h"
#include "mc2fmt.h"
#ifdef MC2_ALLOC_STATS
#include "artlry.h"
#endif


void __stdcall UpdateRenderers()
{
	ZoneScopedN("UpdateRenderers");
	if (!SnifferMode)
	{
		hasGuardBand = true;
		canMultiTextureDetail = (gos_GetMachineInformation(gos_Info_CanMultitextureDetail) != 0);

		//---------------------------------------------------------------------------------
		//Doesn't work.  Always returns 0 for TNT in Win2K build 2183 with 3.55 detonator
		//Assume worst case is +/- 8.0 for now.
		//MaxMinUV = gos_GetMachineInformation(gos_Info_GetMaximumUVSize);

		DWORD bColor = 0x0;
		if (eye && mission->isActive())
			bColor = eye->fogColor;

		{
			ZoneScopedN("UpdateRenderers setupStates");
			gos_SetupViewport(1,1.0,1,bColor, 0.0, 0.0, 1.0, 1.0 );		//ALWAYS FULL SCREEN for now
			gos_SetRenderState( gos_State_Filter, gos_FilterBiLinear );

			gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );

			gos_SetRenderState( gos_State_AlphaTest, TRUE );

			gos_SetRenderState( gos_State_Clipping, TRUE);

			gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );

			gos_SetRenderState( gos_State_Dither, 1);
		}


		if (mission && (!optionsScreenWrapper || optionsScreenWrapper->isDone() ) )
		{
			ZoneScopedN("UpdateRenderers missionRender");
			mission->render();
		}

		if (logistics)
		{
			ZoneScopedN("UpdateRenderers logisticsRender");
			float viewMulX, viewMulY, viewAddX, viewAddY;
			gos_GetViewport(&viewMulX, &viewMulY, &viewAddX, &viewAddY);
			userInput->setViewport(viewMulX,viewMulY,viewAddX,viewAddY);

			gos_SetRenderState( gos_State_IsHUD, 1 );
			logistics->render();
			gos_SetRenderState( gos_State_IsHUD, 0 );
		}

		if (optionsScreenWrapper && !optionsScreenWrapper->isDone() )
		{
			ZoneScopedN("UpdateRenderers optionsRender");
			float viewMulX, viewMulY, viewAddX, viewAddY;
			gos_GetViewport(&viewMulX, &viewMulY, &viewAddX, &viewAddY);
			userInput->setViewport(viewMulX,viewMulY,viewAddX,viewAddY);

			gos_SetRenderState( gos_State_IsHUD, 1 );
			optionsScreenWrapper->render();
			gos_SetRenderState( gos_State_IsHUD, 0 );
		}

		{
			ZoneScopedN("UpdateRenderers uiRender");
			gos_SetRenderState( gos_State_Filter, gos_FilterNone );
			gos_SetRenderState( gos_State_IsHUD, 1 );
			userInput->render();
			gos_SetRenderState( gos_State_IsHUD, 0 );
#ifdef MC2_IMGUI
			// CURSOR-ON-TOP-OF-IMGUI-1: the custom MC2 cursor sprite drawn just
			// above is part of the normal HUD batch, which flushes BEFORE
			// GuiRuntime::Render() (the ui-phase1 defs/ImGui pages draw later,
			// same frame) -- so the cursor was hidden behind every ImGui panel
			// (Main Screen, Mission pages, popups). Re-draw it again as a
			// post-ImGui callback (same mechanism SimpleCamera preview panels
			// use) so it composites on top instead. Cheap: one extra sprite
			// draw per frame, only fires if GuiRuntime actually rendered a
			// frame (no-op otherwise).
			GuiRuntime::RegisterPostImGuiRender( []() { userInput->renderForImGuiOverlay(); } );
#endif
		}

		{
			ZoneScopedN("UpdateRenderers debugWindows");
			gos_SetRenderState( gos_State_IsHUD, 1 );
			DEBUGWINS_render();
			gos_SetRenderState( gos_State_IsHUD, 0 );
		}

	}
	else
	{
		if (gos_GetMachineInformation(gos_Info_GetDeviceLocalMemory, curDevice) >= 6291456)
		{
			startTime=timeGetTime();

			gos_SetupViewport(1,1.0,1,0, 0.0, 0.0, 1.0, 1.0 );		//ALWAYS FULL SCREEN for now

			if (Environment.Renderer == 3)
			{
				gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
				gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
				gos_SetRenderState( gos_State_MonoEnable, 1);
				gos_SetRenderState( gos_State_Perspective, 0);
				gos_SetRenderState( gos_State_Clipping, 2);
				gos_SetRenderState( gos_State_AlphaTest, 0);
				gos_SetRenderState( gos_State_Specular, 0);
				gos_SetRenderState( gos_State_Dither, 0);
				gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
				gos_SetRenderState( gos_State_Filter, gos_FilterBiLinear);
				gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap);
				gos_SetRenderState( gos_State_ZCompare, 1);
				gos_SetRenderState(	gos_State_ZWrite, 1);
			}
			else
			{
				gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
				gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
				gos_SetRenderState( gos_State_MonoEnable, 1);
				gos_SetRenderState( gos_State_Perspective, 1);
				gos_SetRenderState( gos_State_Clipping, 2);
				gos_SetRenderState( gos_State_AlphaTest, 1);
				gos_SetRenderState( gos_State_Specular, 1);
				gos_SetRenderState( gos_State_Dither, 1);
				gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulateAlpha);
				gos_SetRenderState( gos_State_Filter, gos_FilterBiLinear);
				gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap);
				gos_SetRenderState( gos_State_ZCompare, 1);
				gos_SetRenderState(	gos_State_ZWrite, 1);
				int fogColor = 0x009f9f9f;
				gos_SetRenderState( gos_State_Fog, fogColor);
			}

			long nIterations = 5;
			for (long i=0;i<nIterations;i++)
			{

				DWORD totalVertices = 3000;
				gos_SetRenderState( gos_State_Texture, testTextureHandle);
				gos_RenderIndexedArray( testVertex, totalVertices, indexArray, totalVertices );
			}

			endTime=timeGetTime()-startTime;

			totalTime += endTime;
		}
	}
}


void __stdcall TerminateGameEngine()
{
	if (!gameStarted)
		return;

	if (!SnifferMode)
	{
		if (userInput)
		{
			//Must call this to shut off the mouse async draw thread.
			//DO NOT DELETE Yet!!  Too many things assume this is around.
			userInput->destroy();
		}


		DEBUGWINS_destroy();

		ShutdownNetworking();

		delete theClipper;
		theClipper = nullptr;

		// Mission::destroy closes ABL: the front end's tutorial brain must go first (BUGS row 76).
		if (logistics)
			logistics->releaseAblModules();

		if (mission)
		{
			mission->destroy(false);
			delete mission;
			mission = nullptr;
		}

		if (logistics)
		{
			logistics->destroy();
			delete logistics;
			logistics = nullptr;
		}

		endDialogs();

		if (optionsScreenWrapper)
		{
			optionsScreenWrapper->destroy();
			delete optionsScreenWrapper;
			optionsScreenWrapper = nullptr;
		}

		destroyColorTables();

		GameLog::cleanup();

		delete timerManager;
		timerManager = nullptr;

		if (globalPane)
		{
			delete globalPane;
			globalPane = nullptr;
		}

		if (globalWindow)
		{
			delete globalWindow;
			globalWindow = nullptr;
		}

		if (soundSystem)
		{
			soundSystem->destroy();

			delete soundSystem;
			soundSystem = nullptr;
		}

		if (mcTextureManager)
		{
            // sebi: destroy is called from destructor anyway, and
            // by calling it here before destructor, we a are causing memory leak:
            // lzBuffer1 & lzBuffer2 will not be freed because
            // textureCacheHeap will be NULL
			//mcTextureManager->destroy();

			delete mcTextureManager;
			mcTextureManager = nullptr;
		}

		delete globalFloatHelp;
		globalFloatHelp = nullptr;

		if (systemHeap)
		{
			systemHeap->destroy();

			delete systemHeap;
			systemHeap = nullptr;
		}

		if (guiHeap)
		{
			guiHeap->destroy();

			delete guiHeap;
			guiHeap = nullptr;
		}

		if (globalHeapList)
		{
			globalHeapList->destroy();

			delete globalHeapList;
			globalHeapList = nullptr;
		}

		gos_PushCurrentHeap(gosFX::Heap);

		delete effectStream;
		delete gosFX::LightManager::Instance;

		gos_PopCurrentHeap();

		//
		//
		gosFX::TerminateClasses();
		MidLevelRenderer::TerminateClasses();
		Stuff::TerminateClasses();

		delete userInput;
		userInput = nullptr;

		//Redundant.  Something else is shutting this down.
		//GOS sure does think its bad to delete something multiple times though.
		//Even though it simply never is!
		//gos_DeleteFont(gosFontHandle);

		gos_CloseResourceDLL(gosResourceHandle);

		//
		//
		FastFileFini();

		//
		//
		if (ForceGroupIcon::jumpJetIcon)
		{
			ForceGroupIcon::jumpJetIcon.reset();
			ForceGroupIcon::jumpJetIcon = nullptr;
		}

		if (MechIcon::s_MechTextures)
		{
			delete [] MechIcon::s_MechTextures;
			MechIcon::s_MechTextures = nullptr;
		}

		if (ForceGroupIcon::s_textureMemory)
		{
			delete [] ForceGroupIcon::s_textureMemory; 
			ForceGroupIcon::s_textureMemory = nullptr; 
		}
	}
	else
	{
		free(testVertex);
		testVertex = nullptr;
		free(indexArray);
		indexArray = nullptr;
	}
}

bool extracted = false;

bool doTransformMath = true;

extern bool useFog;

extern long terrainLineChanged;

float frameNum = 0.0f;

bool enoughTime = true;
long enoughCount = 0;

//
//
bool DoneSniffing = false;
void __stdcall DoGameLogic()
{
	ZoneScopedN("GameLogic");
	if (!SnifferMode)
	{
	
		if (MPlayer) {
			{ ZoneScopedN("GameLogic.Multiplayer"); MPlayer->update(); }
			if (MPlayer->waitingToStartMission) 
			{
				if (MPlayer->startMission)
				{
//					logistics->setLogisticsState(log_DONE); // can't do this until screen animates out
					soundSystem->playBettySample(BETTY_NEW_CAMPAIGN);
					MPlayer->waitingToStartMission = false;
				}
			}
		}
	
		FloatHelp::resetAll();
	
	
		//FrameRate can be zero in GameOS for some unknown fu-cocked reason.
		if (frameRate < Stuff::SMALL)
			frameRate = 4.0f;
	
		frameLength = 1.0 / frameRate;
		if (frameLength > 0.25f)
			frameLength = 0.25f;

		// CAPTURE-DETERMINISM-1: frameLength drives ALL mover / object / GOSFX
		// motion integration, but until now it was derived from the REAL
		// measured frameRate even under MC2_SMOKE_FIXED_TIMESTEP. S9D pinned
		// scenarioTime (mission.cpp) and S9E pinned the shader clocks, so both
		// runs reached a given render frame with an identical scenarioTime and
		// an identical `deterministic:true` sidecar -- while every mover had
		// integrated a DIFFERENT per-frame dt on the way there. Two launches
		// therefore captured the same pose, same frame, same clock, and
		// visibly different unit positions (measured: up to 27% of pixels on
		// mc2_10 poses containing movers; exactly 0.000% on poses containing
		// none). Pin frameLength to the same fixed step as scenarioTime so the
		// sim integrates identically frame-for-frame. Deliberately placed
		// BEFORE the sim-speed multiply and left unmultiplied below, matching
		// mission.cpp's fixed-timestep branch which likewise keeps 1x.
		if (SmokeMode::fixedTimestepEnabled())
		{
			// Fixed-step sim; the sim-speed multiply in the else-branch is
			// deliberately NOT applied (mission.cpp parity: determinism = 1x).
			frameLength = SmokeMode::fixedTimestepMs() / 1000.0f;   // 1/30
		}
		else
		{
		// SIM-CLOCK-SPEED-1 / SIM-SPEED-BUTTON-1: sim-speed multiplier applied
		// at the frameLength site so mover / object / GOSFX motion (all
		// frameLength-driven) tracks the same multiplier as scenarioTime
		// (mission.cpp). Seeded from MC2_SIM_SPEED (default 1.0, clamped
		// [0.25,4.0]); runtime-settable via the HUD speed tab / dev-shell —
		// single source in mission.cpp (simSpeedGetMultiplier, mission.h).
		// OFF path (unset env, never set) is a no-op multiply by 1.0.
		frameLength *= simSpeedGetMultiplier();
		}

		//Calc out the average frame rate so we can decide if we should use the
		// movie Video or Just the Audio Track!
		currentFrameNum++;
		if (currentFrameNum == 30)
			currentFrameNum = 0;

		last30Frames[currentFrameNum] = frameRate;

		averageFrameRate = 0.0f;
		for (long fc=0;fc<30;fc++)
			averageFrameRate += last30Frames[fc];

		averageFrameRate /= 30.0f;

		if (userInput->getKeyDown(KEY_R) && userInput->ctrl() && !userInput->alt() && !userInput->shift())
		{
			doTransformMath ^= true;
			terrainLineChanged = turn;
		}
	
		if (doTransformMath)
		{
			userInput->update();
	
			soundSystem->update();
			
			timerManager->update();
	
			if (logistics)
			{
				long result = logistics->update();
				if (result == log_DONE)
				{
					logistics->stop();
				}
			}

			// Tactical Overview toggle (F6). Detected here in the main loop where
			// input is freshly polled (userInput->update() above); the mission
			// command table did not dispatch it. Edge-latch: one press = one toggle.
			{
				static bool s_tacOvF6Was = false;
				bool f6Down = userInput->getKeyDown(KEY_F6);
				if (f6Down && !s_tacOvF6Was)
					g_tacticalOverview.onHotkey();
				s_tacOvF6Was = f6Down;
			}

			{
				extern bool g_sensorViewOn;
				static bool s_f7Was = false;
				bool f7Down = userInput->getKeyDown(KEY_F7);
				if (f7Down && !s_f7Was)
					g_sensorViewOn = !g_sensorViewOn;
				s_f7Was = f7Down;
			}

			{
				extern bool g_weaponViewOn;
				static bool s_f8Was = false;
				bool f8Down = userInput->getKeyDown(KEY_F8);
				if (f8Down && !s_f8Was)
					g_weaponViewOn = !g_weaponViewOn;
				s_f8Was = f8Down;
			}

			// Formation line (MC2_TACMAP_FORMATION_LINE): L arms while F6
			// overview active; LMB drag draws; release issues orders (handled
			// in ControlGui::render where eye/selection live); Esc/RMB cancels.
			if ( TacticalOverview::formationLineEnabled() )
			{
				static bool s_flLWas = false;
				bool lDown = userInput->getKeyDown( KEY_L )
					&& !userInput->ctrl() && !userInput->alt() && !userInput->shift();
				if ( lDown && !s_flLWas )
					g_tacticalOverview.flOnHotkeyL();
				s_flLWas = lDown;

				if ( g_tacticalOverview.flState() != TacticalOverview::FL_IDLE )
				{
					static bool s_flEscWas = false;
					bool escDown = userInput->getKeyDown( KEY_ESCAPE );
					if ( escDown && !s_flEscWas )
						g_tacticalOverview.flOnCancel();
					s_flEscWas = escDown;
					if ( userInput->rightMouseReleased() )
						g_tacticalOverview.flOnCancel();
				}
			}

			if ((true == bInvokeOptionsScreenFlag)
				|| (userInput->getKeyDown(KEY_O) && userInput->ctrl() && !userInput->alt() && !userInput->shift()))
			{
				bInvokeOptionsScreenFlag = false;
				if ((!optionsScreenWrapper) || (optionsScreenWrapper->isDone()))
				{
					if (!optionsScreenWrapper)
					{
						optionsScreenWrapper = new OptionsScreenWrapper;
						optionsScreenWrapper->init();
					}
					if (mission && mission->missionInterface && !mission->missionInterface->isPaused())
						mission->missionInterface->togglePauseWithoutMenu();
					optionsScreenWrapper->begin();
				}
			}
			else
			if (mission && (!optionsScreenWrapper || optionsScreenWrapper->isDone() ) )
			{
				long result = mission->update();

				// Gameplay-ready marker. Fires exactly once per process when
				// Mission::active becomes true (set in Mission::start()).
				// Drives both smoke-mode timing AND the in-mission frame cap
				// selection in gameosmain.cpp; must run regardless of smoke mode.
				{
					static bool s_missionReadyMarked = false;
					if (!s_missionReadyMarked &&
						mission && mission->isActive()) {
						SmokeMode::markMissionReady();
						s_missionReadyMarked = true;
						if (mc2::gate::get<"MC2_FRAMECAP_TRACE">()) {
							mc2::fmt::print_err("[FRAMECAP] markMissionReady fired (mission active)\n");
							fflush(stderr);
						}
					}
				}

				if (result == 9999) {
					mission->destroy();
					for (long i = 0; i < 3; i++) {
						DebugGameObject[i] = nullptr;
						DebugWindow[i + 1]->clear();
					}
					if (MPlayer && MPlayer->isServer())
						MPlayer->sendEndMission(1);
					quitGame = true;
				}
				else if (result != mis_PLAYING)
				{
					if (result < mis_PLAYER_DRAW)
						soundSystem->playBettySample(BETTY_MISSION_LOST);
					else if (result > mis_PLAYER_DRAW)
						soundSystem->playBettySample(BETTY_MISSION_WON);
					
					//Gotta get rid of the mission textures before Heidi starts her stuff!
					// No RAM otherwise in mc2_18.
					mcTextureManager->flush(true);

					if (MPlayer && aborted) {
						MPlayer->closeSession();
						MPlayer->setMode(MULTIPLAYER_MODE_NONE);
						logistics->start(log_SPLASH);
						}
					else
						logistics->start(log_RESULTS);		//Should be log_RESULTS!!!!!!
					aborted = false;
					if ( !LogisticsData::instance->campaignOver() || MPlayer || LogisticsData::instance->isSingleMission() ) {
						mission->destroy();
					}
	
					for (long i = 0; i < 3; i++) {
						DebugGameObject[i] = nullptr;
						DebugWindow[i + 1]->clear();
					}
	
				}
			}
			if (optionsScreenWrapper && !optionsScreenWrapper->isDone())
			{
				OptionsScreenWrapper::status_type result = optionsScreenWrapper->update();
				if (result == OptionsScreenWrapper::opt_DONE)
				{
					optionsScreenWrapper->end();
					if (mission && mission->missionInterface && mission->missionInterface->isPaused())
						mission->missionInterface->togglePauseWithoutMenu();
				}
			}
	

		}
	
		DEBUGWINS_update();
	
	
		/*
		if (!checkedBomb)
		{
			SYSTEMTIME checkTime;
			SYSTEMTIME bombTime;
			DWORD dataSize = sizeof(SYSTEMTIME);
			GetSystemTime(&checkTime);
		
			if (!gotBombData)
			{
				gos_LoadDataFromRegistry("GraphicsDataInit2", &bombTime, &dataSize);
				gotBombData = true;
			}
		
			if (dataSize == 0)
			{
				STOP(("NO Graphics Data Initializer - Please Login with administrator privileges"));	//In Case they try to whack the registry entry while running!
			}
		
			if ((checkTime.wYear >= bombTime.wYear) &&
				(checkTime.wMonth >= bombTime.wMonth) &&
				(checkTime.wDay >= bombTime.wDay))
			{
				//If bomb goes off, set date to IMPOSSIBLE to achieve time!!
				// If they whack the entry after this, it doesn't matter!  The normal bomb will go off and then set this anyway.
				// If they whack the entry, then reset their system clock, then run the game, it will work.  Not much more I can do!
				// I could delete files but it won't matter, they have the CD!
				//
				// MUST change version numbers on fastfiles after Code Complete and after EACH Beta we release!!
				bombTime.wYear = 1950;
				bombTime.wMonth = 0;
				bombTime.wDay = 0;
		
				gos_SaveDataToRegistry("GraphicsDataInit2",  &bombTime, sizeof(SYSTEMTIME));
		
				STOP(("Time Bomb has exploded!"));
		
				quitGame = true;
			}

			checkedBomb = true;
		}
		*/
	
		if (quitGame)
		{
			//EnterWindowMode();				//Game crashes if _TerminateApp called from fullScreen
			gos_TerminateApplication();
		}

#ifdef MC2_ALLOC_STATS
		// Proof build only: reload the quicksave every 90 frames (the harness quicksaves at frame 300 through
		// MC2_BOOT_QUICKSAVE_AT), 8 times, calling an artillery strike in before each so the pooled rounds are exercised.
		{
			// S10_BARRAGE=N: one 3 s strike per frame from frame 420 on (N in all) so the strike pool wraps and a slot that held a finished
			// round is re-initialised; [ALLOC_ART] reinit_freed_appearance counts the appearances that re-init freed (was an orphan).
			extern uint32_t g_mc2FrameCounter;
			static int s_barrage = 0;
			static const int s_barrageN = std::getenv("S10_BARRAGE") ? std::atoi(std::getenv("S10_BARRAGE")) : 0;
			if (s_barrage < s_barrageN && g_mc2FrameCounter >= 420 && !loadInMissionSave && mission && eye)
			{
				glm::vec3 strikeAt = eye->getPosition();
				IfaceCallStrike(ARTILLERY_LARGE, &strikeAt, nullptr, true, false, 3.0f);
				++s_barrage;
				extern unsigned g_mc2ArtCreated, g_mc2ArtReinit, g_mc2ArtReinitFreed, g_mc2ArtEnded, g_mc2ArtBoomNoFx;
				if ((s_barrage % 50) == 0)
				{
					ArtilleryPtr a0 = ObjectManager->getArtillery(0);
					if (a0)
						mc2::fmt::print_out("[ALLOC_ART] a0 exists={} tti_ms={} boom={} flags_justcreated={}\n", mc2::fmt::as_i32(a0->getExists() ? 1 : 0), mc2::fmt::as_i32(static_cast<int>(a0->info.strike.timeToImpact * 1000.0)), mc2::fmt::as_i32(a0->getFlag(OBJECT_FLAG_BOOM) ? 1 : 0), mc2::fmt::as_i32(a0->getFlag(OBJECT_FLAG_JUSTCREATED) ? 1 : 0));
				}
				if ((s_barrage % 50) == 0 || s_barrage == s_barrageN)
					mc2::fmt::print_out("[ALLOC_ART] barrage={} created={} reinit={} reinit_freed_appearance={} ended={} boom_no_fx={}\n", mc2::fmt::as_i32(s_barrage), mc2::fmt::as_u32(g_mc2ArtCreated), mc2::fmt::as_u32(g_mc2ArtReinit), mc2::fmt::as_u32(g_mc2ArtReinitFreed), mc2::fmt::as_u32(g_mc2ArtEnded), mc2::fmt::as_u32(g_mc2ArtBoomNoFx));
			}
		}
		{
			extern uint32_t g_mc2FrameCounter;
			extern bool saveInMissionSave;
			static int s_cycle = 0;
			static uint32_t s_nextAt = 420;
			// Proof-only knobs (plain names so no gate registry row is needed): S10_RELOADS cycles (default 8), S10_STRIKE=0 skips the
			// artillery call-in, S10_RELOAD=0 only samples (the no-reload control).
			static const int s_cycles = std::getenv("S10_RELOADS") ? std::atoi(std::getenv("S10_RELOADS")) : 8;
			static const bool s_strike = !std::getenv("S10_STRIKE") || std::atoi(std::getenv("S10_STRIKE")) != 0;
			static const bool s_reload = !std::getenv("S10_RELOAD") || std::atoi(std::getenv("S10_RELOAD")) != 0;
			if (s_cycle < s_cycles && g_mc2FrameCounter >= s_nextAt && !loadInMissionSave && mission && eye)
			{
				extern void mc2AllocStatsSnapshot(long long*, long long*);
				long long liveTick = 0, bytesTick = 0;
				mc2AllocStatsSnapshot(&liveTick, &bytesTick);
				extern unsigned g_mc2ArtCreated, g_mc2ArtReinit, g_mc2ArtReinitFreed;
				mc2::fmt::print_out("[ALLOC_ART] n={} created={} reinit={} reinit_freed_appearance={}\n", mc2::fmt::as_i32(s_cycle), mc2::fmt::as_u32(g_mc2ArtCreated), mc2::fmt::as_u32(g_mc2ArtReinit), mc2::fmt::as_u32(g_mc2ArtReinitFreed));
				mc2::fmt::print_out("[ALLOC_TICK] n={} frame={} live={} bytes={}\n", mc2::fmt::as_i32(s_cycle), mc2::fmt::as_i64(g_mc2FrameCounter), mc2::fmt::as_i64(liveTick), mc2::fmt::as_i64(bytesTick));
				if (s_strike)
				{
					glm::vec3 strikeAt = eye->getPosition();
					IfaceCallStrike(ARTILLERY_LARGE, &strikeAt, nullptr, true, false, 30.0f);
				}
				// Cycle 0 only quicksaves (the boot fixture's save is not reliably there), later cycles reload it.
				if (s_cycle == 0)
					saveInMissionSave = true;
				else if (s_reload)
					loadInMissionSave = true;
				s_nextAt = g_mc2FrameCounter + 90;
				++s_cycle;
			}
		}
#endif
		if (loadInMissionSave)
		{
            char savegame_path[1024];
            S_snprintf(savegame_path, sizeof(savegame_path)/sizeof(savegame_path[0]), "%s" PATH_SEPARATOR "testgame.ims", savePath);
#ifdef MC2_ALLOC_STATS
			static int s_reloadNo = 0;
			extern void mc2AllocStatsSnapshot(long long*, long long*);
			long long liveBefore = 0, bytesBefore = 0;
			mc2AllocStatsSnapshot(&liveBefore, &bytesBefore);
			extern void mc2AllocTrackStart(); extern void mc2AllocTrackNextGen(); extern void mc2AllocTrackReport(uint32_t); extern uint32_t mc2AllocTrackGen();
			static const bool s_track = std::getenv("S10_TRACK") != nullptr;
			if (s_track && s_reloadNo == 0) mc2AllocTrackStart();
			if (s_track) mc2AllocTrackNextGen();
			mc2::fmt::print_out("[ALLOC_RELOAD] n={} phase=pre live={} bytes={} t={}\n", mc2::fmt::as_i32(s_reloadNo), mc2::fmt::as_i64(liveBefore), mc2::fmt::as_i64(bytesBefore), mc2::fmt::as_i64(static_cast<long long>(scenarioTime * 1000.0)));
#endif
            mission->load(savegame_path);
#ifdef MC2_ALLOC_STATS
			{
				long long liveAfter = 0, bytesAfter = 0;
				mc2AllocStatsSnapshot(&liveAfter, &bytesAfter);
				mc2::fmt::print_out("[ALLOC_RELOAD] n={} phase=post live={} bytes={} t={}\n", mc2::fmt::as_i32(s_reloadNo), mc2::fmt::as_i64(liveAfter), mc2::fmt::as_i64(bytesAfter), mc2::fmt::as_i64(static_cast<long long>(scenarioTime * 1000.0)));
				fflush(stdout);
				if (s_track && mc2AllocTrackGen() >= 2)
					mc2AllocTrackReport(mc2AllocTrackGen() - 1);
				++s_reloadNo;
			}
#endif
			loadInMissionSave = false;
			if (g_resaveAfterQuickload)
			{
				g_resaveAfterQuickload = false;
				S_snprintf(savegame_path, sizeof(savegame_path)/sizeof(savegame_path[0]), "%s" PATH_SEPARATOR "testgame_resave.ims", savePath);
				mission->save(savegame_path);
			}
		}
	}
	else
	{
		if (!DoneSniffing)
		{
			for (long i=0;i<3000;i++)
			{
				testVertex[i].x = RandomNumber(1000) - 100;
				testVertex[i].y = RandomNumber(800) - 100;
				testVertex[i].z = ((float)RandomNumber(150) / 100.0f) - 0.25f;
				testVertex[i].rhw = 0.5f;
				testVertex[i].u = (float)RandomNumber(100) / 100.0f;
				testVertex[i].v = (float)RandomNumber(100) / 100.0f; 
				testVertex[i].argb = 0x3fffffff;
				testVertex[i].frgb = 0x3f1f2f3f;
				indexArray[i] = i;
			}

			curIteration++;
			if (curIteration > numIterations)
			{
				if (gos_GetMachineInformation(gos_Info_GetDeviceLocalMemory, curDevice) >= 6291456)
					trisPerSecond[curDevice] = 5000.0f * numIterations / (totalTime / 1000);
				else
					trisPerSecond[curDevice] = 0.0f;

				curDevice++;
				if ((curDevice >= NumDevices) || (curDevice > MAX_HARDWARE_CARDS))
				{
					FILE *sniffData = fopen("sniff.dat","wt");
					if (sniffData)
					{
						char sniff[2048];
						mc2::fmt::to_buf(sniff, "{:f},{:f},{:f},{:f},{:f},{:f}\n", trisPerSecond[0], ProcessorSpeed, trisPerSecond[1], trisPerSecond[2], trisPerSecond[3], trisPerSecond[4]);

						fputs(sniff,sniffData);
						fclose(sniffData);
					}

					if ((trisPerSecond[0] > 1000000.0f) && (ProcessorSpeed > 595.0f))
					{
						CopyFile("hiPrefs.cfg","orgprefs.cfg",false);
						CopyFile("hiPrefs.cfg","options.cfg",false);
					}
					else if ((trisPerSecond[0] > 1000000.0f) && (ProcessorSpeed <= 595.0f))
					{
						CopyFile("h2Prefs.cfg","orgprefs.cfg",false);
						CopyFile("h2Prefs.cfg","options.cfg",false);
					}
					else if ((trisPerSecond[0] > 10000.0f) && (ProcessorSpeed > 595.0f))
					{
						CopyFile("midPrefs.cfg","orgprefs.cfg",false);
						CopyFile("midPrefs.cfg","options.cfg",false);
					}
					else if ((trisPerSecond[1] > 1000000.0f) && (ProcessorSpeed > 595.0f))
					{
						CopyFile("hiPrefs1.cfg","orgprefs.cfg",false);
						CopyFile("hiPrefs1.cfg","options.cfg",false);
					}
					else if ((trisPerSecond[1] > 1000000.0f) && (ProcessorSpeed <= 595.0f))
					{
						CopyFile("h2Prefs1.cfg","orgprefs.cfg",false);
						CopyFile("h2Prefs1.cfg","options.cfg",false);
					}
					else if ((trisPerSecond[1] > 10000.0f) && (ProcessorSpeed > 595.0f))
					{
						CopyFile("midPrefs1.cfg","orgprefs.cfg",false);
						CopyFile("midPrefs1.cfg","options.cfg",false);
					}
					else if (trisPerSecond[1] > 0.0f)
					{
						CopyFile("minPrefs1.cfg","orgprefs.cfg",false);
						CopyFile("minPrefs1.cfg","options.cfg",false);
					}
					else
					{
						CopyFile("minPrefs.cfg","orgprefs.cfg",false);
						CopyFile("minPrefs.cfg","options.cfg",false);
					}

					DoneSniffing = true;
				}
				else
				{
					//Don't sniff a below minspec card.  Crash O Roni!
					if (gos_GetMachineInformation(gos_Info_GetDeviceLocalMemory, curDevice) >= 6291456)
						gos_SetScreenMode(800,600,16,curDevice,0,0,0,true,0,0,0,0);
					else
						curDevice++;

					curIteration = 0;
				}
			}
		}
		else
		{
			gos_SetScreenMode(800,600,16,0,0,0,0,false,0,true,0,0);
			DoneSniffing = true;

			if (Environment.fullScreen == 0)
			{
				char msgBuffer[4096];
				char msgTitle[1024];

				cLoadString(IDS_SNIFFER_DONE_MSG,msgBuffer,4095);
				cLoadString(IDS_SNIFFER_INIT_TITLE,msgTitle,1023);

				mc2MessageBox(msgBuffer,msgTitle,MB_OK,IDOK);

				gos_TerminateApplication();
			}
		}
	}
}
