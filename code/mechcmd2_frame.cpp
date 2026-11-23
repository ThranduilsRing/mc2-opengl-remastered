#include "icon_registry.h"
#include "vk_scene_renderer.h"
#include <atomic>
#include "render_submissions.h"
// Game main: the per-frame renderer update, engine termination and DoGameLogic.
// Declared in the class headers; shared names in mechcmd2_internal.h.
#include "mechcmd2_internal.h"
#include "render_resource_owners.h"
#include "prewarm.h"
#include "Gates/gate.h"
#include "mc2fmt.h"
#include "screen_settle_capture.h"
#include "startup_preload.h"
#include "net/runtime.h"
#include "missionbegin.h"
#ifdef MC2_IMGUI
#include "imgui.h"
#include "ui_texture.h"
#include "RenderCore/CursorMap.h"
#include "diagnostic_trace.h"
#include <cmath>
#include <cstdio>
#endif
#include "startup_preload.h"
#ifdef MC2_ALLOC_STATS
#include "artlry.h"
#include "startup_preload.h"
#endif


void __stdcall UpdateRenderers()
{
	ZoneScopedN("UpdateRenderers");
	{
		hasGuardBand = true;
		canMultiTextureDetail = (gos_GetMachineInformation(gos_Info_CanMultitextureDetail) != 0);

		//---------------------------------------------------------------------------------
		//Doesn't work.  Always returns 0 for TNT in Win2K build 2183 with 3.55 detonator
		//Assume worst case is +/- 8.0 for now.
		//MaxMinUV = gos_GetMachineInformation(gos_Info_GetMaximumUVSize);

		DWORD bColor = 0x0;
		if (eye && mission->isActive())
			bColor = eye->currentLighting().fogColor;

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
			// CURSOR-TOPMOST-1: when the ImGui pass will run this frame, the cursor is painted
			// by it (foreground draw list = last 2D draw); the HUD-batch copy would sit
			// under every ImGui HUD panel, so skip it.
#ifdef MC2_IMGUI
			if (!GuiRuntime::FrameActive())
#endif
				userInput->render();
			gos_SetRenderState( gos_State_IsHUD, 0 );
#ifdef MC2_IMGUI
			// CURSOR-TOPMOST-1 (supersedes CURSOR-ON-TOP-OF-IMGUI-1, whose post-ImGui
			// GL callback never ran under Vulkan): the HUD batch flushes BEFORE
			// GuiRuntime::Render(), so the cursor sprite sat under every ImGui
			// panel. Paint it into the ImGui foreground list instead.
			if ( GuiRuntime::FrameActive() )
			GuiRuntime::RegisterForegroundCursor( []() -> bool {
				// Native ownership also suppresses ImGui's frame-paced fallback arrow.
				if (userInput && userInput->usesNativeCursor()) return true;
				gos_VERTEX q[4];
				unsigned long tex = 0;
				if ( !userInput || !userInput->captureCursorQuad( q, tex ) || !tex || tex == 0xffffffff )
					return false;
				const uint64_t glTex = gos_GetUiTextureId( tex );
				if ( !glTex )
					return false;
				// q[] is in the logical 2D space (800x600 on menus, window size in a
				// mission), exactly like the old HUD-batch copy. Map it to ImGui display
				// space the way flushHUDBatch maps it to the drawable: UI canvas box
				// (menus only), then drawable -> display. Sprite size scales the same way.
				const ImVec2 disp = ImGui::GetIO().DisplaySize;
				const float dw = static_cast<float>( Environment.drawableWidth );
				const float dh = static_cast<float>( Environment.drawableHeight );
				int bx = 0, by = 0, bw = 0, bh = 0;
				gos_ComputeUiCanvasBox( Environment.drawableWidth, Environment.drawableHeight, &bx, &by, &bw, &bh );
				const mc2::CursorMap m = mc2::makeCursorMap( userInput->logicalW(), userInput->logicalH(),
					static_cast<float>( bx ), static_cast<float>( by ), static_cast<float>( bw ), static_cast<float>( bh ),
					dw, dh, disp.x, disp.y );
				const ImVec2 p0( m.x( q[0].x ), m.y( q[0].y ) );
				const ImVec2 p1( m.x( q[2].x ), m.y( q[2].y ) );
				const DWORD c = q[0].argb;
				ImGui::GetForegroundDrawList()->AddImage(
					static_cast<ImTextureID>( static_cast<intptr_t>( glTex ) ),
					p0, p1,
					ImVec2( q[0].u, q[0].v ), ImVec2( q[2].u, q[2].v ),
					IM_COL32( ( c >> 16 ) & 0xff, ( c >> 8 ) & 0xff, c & 0xff, ( c >> 24 ) & 0xff ) );
				static const bool s_cursorTag = mc2_diag::tagRequested( "CURSOR" );   // per-frame path: ask once
				if ( s_cursorTag )
				{
					static int s_n = 0;
					if ( ( s_n++ % 30 ) == 0 )
					{
						// mapped = display-space image of the logical pointer; expect = pointer
						// as a fraction of the canvas, in display px (must agree within 2 px).
						const float mapX = m.x( userInput->realMouseX() ), mapY = m.y( userInput->realMouseY() );
						const float kx = disp.x / dw, ky = disp.y / dh;
						const float expX = ( bx + userInput->normMouseX() * bw ) * kx;
						const float expY = ( by + userInput->normMouseY() * bh ) * ky;
						char js[384];
						mc2::fmt::to_buf( js, "{{\"logical\":\"{}x{}\",\"logicalPos\":[{:.1f},{:.1f}],\"quad0\":[{:.1f},{:.1f}],\"mapped\":[{:.1f},{:.1f}],\"expect\":[{:.1f},{:.1f}],\"display\":\"{:.0f}x{:.0f}\",\"scale\":{:.3f},\"ok\":{}}}",
							static_cast<int>( userInput->logicalW() ), static_cast<int>( userInput->logicalH() ),
							userInput->realMouseX(), userInput->realMouseY(), p0.x, p0.y, mapX, mapY, expX, expY,
							disp.x, disp.y, m.sx,
							( std::fabs( mapX - expX ) <= 2.0f && std::fabs( mapY - expY ) <= 2.0f ) ? 1 : 0 );
						mc2_diag::writeEvent( "CURSOR", 1, 0, js );
					}
				}
				return true;
			} );
#endif
		}

		{
			ZoneScopedN("UpdateRenderers debugWindows");
			gos_SetRenderState( gos_State_IsHUD, 1 );
			DEBUGWINS_render();
			gos_SetRenderState( gos_State_IsHUD, 0 );
		}

	}
}


void __stdcall TerminateGameEngine()
{
	ZoneScopedN("Shutdown.Engine");
	if (!gameStarted)
		return;

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
			ZoneScopedN("Shutdown.Mission");
			mission->destroy(false);
			ownedMission.reset();
			mission = nullptr;
		}

		if (logistics)
		{
			ZoneScopedN("Shutdown.Logistics");
			logistics->destroy();
			ownedLogistics.reset();
			logistics = nullptr;
		}

		endDialogs();

		if (optionsScreenWrapper)
		{
			optionsScreenWrapper->destroy();
			optionsScreenWrapper.reset();
			optionsScreenWrapper = nullptr;
		}

		destroyColorTables();

		GameLog::cleanup();

		ownedTimerManager.reset();
		timerManager = nullptr;

		if (globalPane)
		{
			ownedPane.reset();
			globalPane = nullptr;
		}

		if (globalWindow)
		{
			ownedWindow.reset();
			globalWindow = nullptr;
		}

		if (soundSystem)
		{
			ZoneScopedN("Shutdown.Sound");
			soundSystem->destroy();

			ownedSoundSystem.reset();
			soundSystem = nullptr;
		}

        if (mc2::VkSceneRenderer::active())
            mc2::VkSceneRenderer::shutdown();

		ownedFloatHelp.reset();
		globalFloatHelp = nullptr;







		gos_PushCurrentHeap(gosFX::Heap);

		ReleaseEffectStream();
		delete gosFX::LightManager::Instance;

		gos_PopCurrentHeap();

		//
		//
		gosFX::TerminateClasses();
		MidLevelRenderer::TerminateClasses();
		Stuff::TerminateClasses();

		ownedUserInput.reset();
		userInput = nullptr;

		//Redundant.  Something else is shutting this down.
		//GOS sure does think its bad to delete something multiple times though.
		//Even though it simply never is!
		//gos_DeleteFont(gosFontHandle);

		gos_CloseResourceDLL(gosResourceHandle);

		//
		//
		stopStartupPreload();
		mc2::prewarm::service().shutdown();
		FastFileFini();

		//
		//
		if (ForceGroupIcon::jumpJetIcon)
		{
			ForceGroupIcon::jumpJetIcon.reset();
			ForceGroupIcon::jumpJetIcon = nullptr;
		}

        unit_icons::shutdown();

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
void __stdcall DoGameLogic()
{
	ZoneScopedN("GameLogic");
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
		frameLength = std::min(frameLength, 0.25f);

		// frameLength here is the RENDER frame's length: UI, camera and the presentation passes use it. The sim does not.
		// Mission::update banks it (times the game speed) into the fixed 30 Hz tick clock and sets frameLength to the tick
		// length around each tick (code/sim_tick_clock.h). Under the harness clock the render frame is virtual:
		// 1/30 s unless MC2_FPS_CAP names another rate, so the committed SIMID baselines (one frame = one tick) hold and
		// a different rate proves the sim does not follow it.
		if (SmokeMode::fixedTimestepEnabled())
			frameLength = SmokeMode::fixedFrameSeconds();

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
			
			// timerManager->update() moved into Mission's sim ticks: ABL timers are sim time.
	
			if (logistics)
			{
				long result = logistics->update();
				const auto coopMission = mc2::net::takeLobbyStart();
				if (!coopMission.empty()) {
					logistics->getMissionBegin()->end();
					logistics->stop();
					std::snprintf(missionName, sizeof(missionName), "%s", coopMission.c_str());
					justStartMission = true;
					logistics->setLogisticsState(log_STARTMISSIONFROMCMDLINE);
					mission->destroy();
					char commanders[MAX_MC_PLAYERS][3] = {{0,0,0},{1,1,1},{2,0,2},{3,3,3},{4,4,4},{5,5,5},{6,6,6},{7,7,7}};
					mission->init(missionName, MissionLoadType::MISSION_LOAD_SP_QUICKSTART, 0, nullptr, commanders, 2);
					eye->activate(); eye->update(); mission->start();
				}
				if (result == log_DONE)
				{
					mc2::screen_settle::onMissionStarted();   // MC2_SHOT_SCREEN: the front end ended before the screen was captured
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
						optionsScreenWrapper = std::make_unique_for_overwrite<OptionsScreenWrapper>();
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
					mc2::textures::flushSources(false);
                    mc2::VkSceneRenderer::reset();

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
		extern void mc2AbortReloadFixtureUpdate();
		mc2AbortReloadFixtureUpdate();
	
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
			extern std::atomic<uint32_t> g_mc2FrameCounter;
			static int s_barrage = 0;
			static const int s_barrageN = std::getenv("S10_BARRAGE") ? std::atoi(std::getenv("S10_BARRAGE")) : 0;
			if (s_barrage < s_barrageN && g_mc2FrameCounter.load(std::memory_order_relaxed) >= 420 && !loadInMissionSave && mission && eye)
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
			extern std::atomic<uint32_t> g_mc2FrameCounter;
			extern bool saveInMissionSave;
			static int s_cycle = 0;
			static uint32_t s_nextAt = 420;
			// Proof-only knobs (plain names so no gate registry row is needed): S10_RELOADS cycles (default 8), S10_STRIKE=0 skips the
			// artillery call-in, S10_RELOAD=0 only samples (the no-reload control).
			static const int s_cycles = std::getenv("S10_RELOADS") ? std::atoi(std::getenv("S10_RELOADS")) : 8;
			static const bool s_strike = !std::getenv("S10_STRIKE") || std::atoi(std::getenv("S10_STRIKE")) != 0;
			static const bool s_reload = !std::getenv("S10_RELOAD") || std::atoi(std::getenv("S10_RELOAD")) != 0;
			if (s_cycle < s_cycles && g_mc2FrameCounter.load(std::memory_order_relaxed) >= s_nextAt && !loadInMissionSave && mission && eye)
			{
				extern void mc2AllocStatsSnapshot(long long*, long long*);
				long long liveTick = 0, bytesTick = 0;
				mc2AllocStatsSnapshot(&liveTick, &bytesTick);
				extern unsigned g_mc2ArtCreated, g_mc2ArtReinit, g_mc2ArtReinitFreed;
				mc2::fmt::print_out("[ALLOC_ART] n={} created={} reinit={} reinit_freed_appearance={}\n", mc2::fmt::as_i32(s_cycle), mc2::fmt::as_u32(g_mc2ArtCreated), mc2::fmt::as_u32(g_mc2ArtReinit), mc2::fmt::as_u32(g_mc2ArtReinitFreed));
				mc2::fmt::print_out("[ALLOC_TICK] n={} frame={} live={} bytes={}\n", mc2::fmt::as_i32(s_cycle), mc2::fmt::as_i64(g_mc2FrameCounter.load(std::memory_order_relaxed)), mc2::fmt::as_i64(liveTick), mc2::fmt::as_i64(bytesTick));
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
				s_nextAt = g_mc2FrameCounter.load(std::memory_order_relaxed) + 90;
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
}
