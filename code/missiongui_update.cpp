// MissionInterfaceManager: the per-frame update() and the hover/target pick (updateTarget).
// Declared in the class headers; shared names in missiongui_shared_internal.h.
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <system_error>
#include "missiongui_shared_internal.h"
#include "mc2fmt.h"
#include "cursor_world_pick.h"
#include "hover_name.h"
#include "pick_query.h"
#include "../GameOS/gameos/utils/mc2log.h"

namespace {

	// MC2_TERRAIN_PICK_PARITY: every 40 cursor-change frames compare the ray pick
	// against the legacy inverseProject over a 7x4 grid of screen points and log the
	// per-sweep and cumulative result. Diagnostic only; never changes wPos.
	void pickRayParitySweep (Camera* cam)
	{
		if (turn < 30)
			return;
		static unsigned long s_frames = 0, s_sweeps = 0;
		static unsigned long s_tot = 0, s_both = 0, s_exact = 0, s_adj = 0, s_far = 0, s_rayOnly = 0, s_legOnly = 0, s_neither = 0;
		static double s_rayNs = 0.0, s_legNs = 0.0, s_maxD = 0.0;
		if ((++s_frames % 40) != 1)
			return;
		const float W = Environment.screenWidth, H = Environment.screenHeight;
		for (int gy = 0; gy < 4; ++gy)
			for (int gx = 0; gx < 7; ++gx)
			{
				Stuff::Vector2DOf<long> sp;
				sp.x = static_cast<long>(W * (gx + 1) / 8.0f);
				sp.y = static_cast<long>(H * (gy + 1) / 5.0f);
				glm::vec3 a(0.f), b(0.f), raw(0.f);
				const auto r0 = std::chrono::steady_clock::now();
				const bool rayOk = cam->pickTerrainRay(sp.x, sp.y, a, &raw);
				const auto r1 = std::chrono::steady_clock::now();
				const unsigned long legRet = cam->inverseProject(sp, b);
				const auto r2 = std::chrono::steady_clock::now();
				++s_tot;
				s_rayNs += static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(r1 - r0).count());
				s_legNs += static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(r2 - r1).count());
				// legacy ret 1 = "no quad under the cursor": b is then the closest map vertex (the production
				// ground pick with an empty quadList), still a valid point to compare against.
				const bool legOk = (legRet == 0) || (b.x != 0.0f || b.y != 0.0f);
				if (rayOk && legOk)
				{
					++s_both;
					const double dx = a.x - b.x, dy = a.y - b.y;
					const double dist = sqrt(dx*dx + dy*dy);
					s_maxD = std::max(s_maxD, dist);
					if (dist < 0.01) ++s_exact;
					else if (dist <= Terrain::worldUnitsPerCell * 1.5) ++s_adj;
					else
					{
						++s_far;
						mc2::fmt::print_out("[PICK_RAY_PARITY v1] event=far px=({:d},{:d}) ray=({:.1f},{:.1f},{:.1f}) legacy=({:.1f},{:.1f},{:.1f}) dist={:.1f}\n",
							mc2::fmt::as_i32(sp.x), mc2::fmt::as_i32(sp.y), a.x, a.y, a.z, b.x, b.y, b.z, dist);
					}
				}
				else if (rayOk) ++s_rayOnly;
				else if (legOk) { ++s_legOnly;
					mc2::fmt::print_out("[PICK_RAY_PARITY v1] event=ray_miss_legacy_hit px=({:d},{:d}) legacy=({:.1f},{:.1f},{:.1f})\n",
						mc2::fmt::as_i32(sp.x), mc2::fmt::as_i32(sp.y), b.x, b.y, b.z); }
				else ++s_neither;
			}
		++s_sweeps;
		mc2::fmt::print_out("[PICK_RAY_PARITY v1] event=sweep n={:d} points={:d} both_hit={:d} exact_cell={:d} within_1p5_cells={:d} far={:d} ray_only={:d} legacy_only={:d} neither={:d} max_dist_wu={:.1f} ray_avg_us={:.1f} legacy_avg_ms={:.2f}\n",
			mc2::fmt::as_u64(s_sweeps), mc2::fmt::as_u64(s_tot), mc2::fmt::as_u64(s_both), mc2::fmt::as_u64(s_exact), mc2::fmt::as_u64(s_adj),
			mc2::fmt::as_u64(s_far), mc2::fmt::as_u64(s_rayOnly), mc2::fmt::as_u64(s_legOnly), mc2::fmt::as_u64(s_neither), s_maxD,
			s_tot ? s_rayNs / s_tot / 1000.0 : 0.0, s_tot ? s_legNs / s_tot / 1.0e6 : 0.0);
		std::fflush(stdout);
	}
}

void updateSaveFixtures()
{
	// Tick-boundary quicksave fixtures: MC2_BOOT_QUICKSAVE_AT=N / MC2_BOOT_QUICKLOAD_AT=M (sim ticks)
	// (default unset). One-shot at the first non-movie frame >= N/M, setting the
	// SAME saveInMissionSave/loadInMissionSave flags the pause-menu QUICK
	// SAVE/LOAD buttons set (pausewindow.cpp handleMessage SAVE/LOAD; consumed
	// mission.cpp / mechcmd2.cpp -> savePath/testgame.ims). Lets agents drive a
	// deterministic quicksave->quickload cycle headlessly (field bug 2026-07-18:
	// airport cement overlay vanishes after quickload). Set M > N for a
	// save-then-load in one run. Same fixture pattern as MC2_BOOT_PAUSED above.
	{
		extern bool saveInMissionSave;
		extern bool loadInMissionSave;
		extern bool g_resaveAfterQuickload;
		extern long turn;  // sim tick (SIMID simframe = turn - 1): host load cannot move the save/load points
		static const long s_qsAt = static_cast<long>(mc2::gate::get<"MC2_BOOT_QUICKSAVE_AT">());
		static const long s_qlAt = static_cast<long>(mc2::gate::get<"MC2_BOOT_QUICKLOAD_AT">());
		static long s_qsIssuedAt = -1;
		if (s_qsAt > 0) {
			static bool s_qsDone = false;
			if (!s_qsDone && turn >= s_qsAt &&
				eye && !eye->inMovieMode) {
				s_qsDone = true;
				s_qsIssuedAt = turn;
				saveInMissionSave = true;
			}
		}
		if (s_qlAt > 0) {
			static bool s_qlDone = false;
			// An intro camera can delay both thresholds. Keep the requested gap
			// after the actual save, so load never races it or reads an older file.
			// capture --load arms the quicksave at 10^9 (never fires): a planted load,
			// quickloaded at s_qlAt with no prior quicksave, then re-saved.
			const bool planted = s_qsAt >= 1000000000L;
			const long requestedGap = std::max(1L, s_qlAt - s_qsAt);
			const long loadAt = s_qsAt > 0 && !planted && s_qsIssuedAt >= 0
				? std::max(s_qlAt, s_qsIssuedAt + requestedGap) : s_qlAt;
			if (!s_qlDone && (s_qsAt <= 0 || planted || s_qsIssuedAt >= 0) &&
				turn >= loadAt &&
				eye && !eye->inMovieMode) {
				s_qlDone = true;
				loadInMissionSave = true;
				g_resaveAfterQuickload = (s_qsAt > 0);
			}
		}
	}

}

extern void updateMissionSupportPreloads();

void MissionInterfaceManager::update (void)
{
	if (mission && mission->isActive())
		updateMissionSupportPreloads();

	// IMGUI-PAUSE-INPUT-FIX-1: tell the input layer when the pause menu is up so its
	// clicks win over any open ImGui window (Graphics Options etc.). Set every frame,
	// before any early return below, so it clears the moment the menu closes.
	// PAUSE-IMGUI-SLICE-1: game-modal keeps clicks for the LEGACY sprite pause
	// menu (its own hit-test). When the pause menu is the ImGui page instead,
	// let ImGui capture normally (gameModal off) so its buttons win AND the
	// world behind doesn't take stray clicks.
	input::setGameModalActive(isPaused() && !isPausedWithoutMenu() &&
	                          !PauseWindow::imguiPauseEnabled());

	// PAUSE-IMGUI-SLICE-0: MC2_BOOT_PAUSED=N fixture (default unset). Auto-toggles
	// the pause menu ONCE at frame >= N, so agents can capture / iterate on the
	// pause window headlessly (no keypress path headless). Same fixture pattern
	// as MC2_FX_FORCE_SPAWN / the fire-event capture pin. Registered (Trace).
	{
		static const long s_bootPaused = static_cast<long>(mc2::gate::get<"MC2_BOOT_PAUSED">());
		if (s_bootPaused > 0) {
			extern std::atomic<uint32_t> g_mc2FrameCounter;
			static bool s_done = false;
			// Wait out the intro swoop: pausing while eye->inMovieMode freezes
			// the swoop itself and the HUD (drawGUIOn) never arrives — the pause
			// WINDOW only draws on the live HUD. Trigger at the first frame >= N
			// that is NOT movie mode.
			if (!s_done && static_cast<long>(g_mc2FrameCounter.load(std::memory_order_relaxed)) >= s_bootPaused && !isPaused() &&
				eye && !eye->inMovieMode) {
				s_done = true;
				togglePause();
			}
		}
	}

	// DEBUG-VIEW-SWEEP-1: MC2_DEBUG_VIEW=N boot fixture (default unset ->
	// one cached getenv, zero behavior change). Applies the global debug
	// render view once at the first update frame; the command-channel verb
	// `debugview N` switches it at runtime thereafter. Registered (Trace).
	{
		static const long s_bootView = static_cast<long>(mc2::gate::get<"MC2_DEBUG_VIEW">());
		if (s_bootView > 0) {
			static bool s_dvDone = false;
			if (!s_dvDone) {
				s_dvDone = true;
				gos_SetGlobalDebugView(static_cast<int>(s_bootView));
			}
		}
	}

	// DEBUG-VIEW-SWEEP-1: MC2_DEBUG_VIEW_SWEEP="<startFrame>,<settleFrames>"
	// frame-scheduled auto-sweep (default unset -> one cached getenv, zero
	// behavior change). Wall-clock command scheduling lands captures on
	// DIFFERENT sim frames run-to-run (animated sim/overlays -> diffs), so this
	// sequencer keys everything to absolute frame numbers instead: for each
	// view v in 1..6, at frame start+(v-1)*(settle+2) set the view, settle
	// frames later schedule dev_shell_out/view_<v>.tga (scene-FBO source;
	// needs MC2_DEV_SHELL=1), then restore view 0. With MC2_SMOKE_MODE=1 +
	// MC2_SMOKE_FIXED_TIMESTEP=1 + MC2_CAMERA_PARK, two runs capture
	// byte-comparable frames. Driven by tools/debug_view_sweep.py. Registered
	// (Trace).
	{
		static const std::string s_swText = mc2::gate::str<"MC2_DEBUG_VIEW_SWEEP">();   // "" when unset or empty
		static const char* s_sw = s_swText.c_str();
		if (s_sw && s_sw[0]) {
			extern std::atomic<uint32_t> g_mc2FrameCounter;
			static long s_start = 600, s_settle = 30;
			static int  s_stage = -1;   // monotonic: 2*v = view set, 2*v+1 = shot, 12 = done
			static bool s_init = false;
			if (!s_init) {
				s_init = true;
				long a = 0, b = 0;
				if (sscanf(s_sw, "%ld,%ld", &a, &b) >= 1 && a > 0) s_start = a;
				if (b >= 5) s_settle = b;
			}
			const long f = static_cast<long>(g_mc2FrameCounter.load(std::memory_order_relaxed));
			if (s_stage < 12 && f >= s_start) {
				const long per = s_settle + 2;
				const long rel = f - s_start;
				const int  idx = static_cast<int>((rel / per));        // 0..5 = view idx
				if (idx > 5) {
					s_stage = 12;
					gos_SetGlobalDebugView(0);
				} else {
					const long inner = rel % per;
					if (inner >= s_settle && s_stage < idx * 2 + 1) {
						// set-stage must already have run this period
						if (s_stage < idx * 2) {
							s_stage = idx * 2;
							gos_SetGlobalDebugView(idx + 1);
						} else {
							s_stage = idx * 2 + 1;
							std::error_code mkdirEc;
							std::filesystem::create_directory("dev_shell_out", mkdirEc);
							char nm[32];
							mc2::fmt::to_buf(nm, "view_{:d}", mc2::fmt::as_i32(idx + 1));
							gos_dev_shell::scheduleScreenshot(nm);
						}
					} else if (s_stage < idx * 2) {
						s_stage = idx * 2;
						gos_SetGlobalDebugView(idx + 1);
					}
				}
			}
		}
	}

	// UI-COMMAND-CHANNEL-1: MC2_UI_COMMAND_FILE=<path> runtime command channel
	// (default unset -> one cached getenv check, byte-identical). See
	// processUiCommandChannel() below for the command set. Placed before the
	// movie-mode early return so commands are still consumed (and acked
	// blocked) during the intro swoop instead of piling up.
	processUiCommandChannel();

	// VK-FIELD-SELECT-1 test hook: MC2_VK_SELECT_TEST=1 auto-selects the visible
	// home lance every frame so the deterministic headless capture (which ignores
	// synthetic input under the sim-freeze sweep) exercises the selection
	// brackets/bars. Gated, default-OFF -> zero effect on normal play. Runs before
	// the mover render (Camera::render) so isSelected() is set when brackets draw.
	{
		static const int s_vkSelectTest = false;
		if (s_vkSelectTest)
			selectVisible();
	}

	const std::chrono::steady_clock::time_point _mifT0 =
		s_mfOn ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	if ( Environment.screenWidth != resolution )
	{
		swapResolutions();
	}

	bForcedShot = false;
	bAimedShot = false;

	// Scripted picks do not need to wait for the mission's intro movie. The
	// objects and their presentation bounds are live after the first turns.
	if (turn > 90 && ObjectManager && false) {
		ObjectManager->findObjectByMouse(-999998, -999998);
	}
	if (eye && eye->inMovieMode)
	{
        MC2_LOG(Core, Debug, "eye->inMovieMode: {:d}", mc2::fmt::as_i32(eye->inMovieMode));
		drawGUIOn = false;
		userInput->mouseOff();
		
		//Need to check for ESC key so user can end IMI IMMEDIATELY.
		// After that check, just return.  Do NOT update any of the UI!!!!
		if (userInput->getKeyDown(KEY_ESCAPE))
			eye->forceMovieToEnd();

		controlGui.update(isPaused() && !isPausedWithoutMenu(), false );
			
		return;
	}
	else
	{
		drawGUIOn = toggledGUI;
		userInput->mouseOn();
	}
	
	if (animationRunning)
	{
		if (targetIsButton)
		{
			ControlButton *targetButton = controlGui.getButton(targetButtonId);
			if (!targetButton)
			{
				animationRunning = false;
				return;
			}

			userInput->setMouseCursor(mState_TUTORIALS);

			float buttonPosX = (targetButton->location[0].x + targetButton->location[1].x +
								targetButton->location[2].x + targetButton->location[3].x) * 0.25f;

			float buttonPosY = (targetButton->location[0].y + targetButton->location[1].y +
								targetButton->location[2].y + targetButton->location[3].y) * 0.25f;

			float realMouseX = userInput->realMouseX();
			float realMouseY = userInput->realMouseY();

			if (timeLeftToScroll > 0.0f)
			{
				float xDistLeft = buttonPosX - realMouseX;
				float yDistLeft = buttonPosY - realMouseY;

				float xDistThisFrame = xDistLeft / timeLeftToScroll * frameLength;
				float yDistThisFrame = yDistLeft / timeLeftToScroll * frameLength;

				userInput->setMousePos(realMouseX + xDistThisFrame, realMouseY+yDistThisFrame);

				timeLeftToScroll -= frameLength;
			}
			else
			{
				userInput->setMousePos(buttonPosX,buttonPosY);

				if (buttonNumFlashes)
				{
					buttonFlashTime += frameLength;
					if ( buttonFlashTime > .5f )
					{
						controlGui.getButton( targetButtonId )->setColor( 0xffffffff );
						buttonFlashTime = 0.0f;
						buttonNumFlashes--;
					}
					else if ( buttonFlashTime > .25f )
					{
						controlGui.getButton( targetButtonId )->setColor( 0xff7f7f7f );
					}
				}
				else
				{
					animationRunning = false;
					controlGui.getButton( targetButtonId )->setColor( 0xffffffff );
					if (targetIsPressed)
						controlGui.pushButton(targetButtonId);
				}
			}
		}
		else
		{
			ControlGui::RectInfo *targetButton = controlGui.getRect(targetButtonId);
			if (!targetButton)
			{
				animationRunning = false;
				return;
			}

			userInput->setMouseCursor(mState_TUTORIALS);

			float buttonPosX = (targetButton->rect.left + targetButton->rect.right) * 0.5f;

			float buttonPosY = (targetButton->rect.top + targetButton->rect.bottom) * 0.5f;

			float realMouseX = userInput->realMouseX();
			float realMouseY = userInput->realMouseY();

			if (timeLeftToScroll > 0.0f)
			{
				float xDistLeft = buttonPosX - realMouseX;
				float yDistLeft = buttonPosY - realMouseY;

				float xDistThisFrame = xDistLeft / timeLeftToScroll * frameLength;
				float yDistThisFrame = yDistLeft / timeLeftToScroll * frameLength;

				userInput->setMousePos(realMouseX + xDistThisFrame, realMouseY+yDistThisFrame);

				timeLeftToScroll -= frameLength;
			}
			else
			{
				userInput->setMousePos(buttonPosX,buttonPosY);

				if (buttonNumFlashes)
				{
					buttonFlashTime += frameLength;
					if ( buttonFlashTime > .5f )
					{
						targetButton->color = 0xff000000;
						buttonFlashTime = 0.0f;
						buttonNumFlashes--;
					}
					else if ( buttonFlashTime > .25f )
					{
						targetButton->color = 0xffffffff;
					}
				}
				else
				{
					animationRunning = false;
					targetButton->color = 0xff000000;
				}
			}
		}
		
		return;		//Don't let anything else in the GUI run!!
	}
	
	//Tutorial has locked out the gui.  Move mouse to screen center and do nothing else!!
	if (guiFrozen)
	{
		userInput->setMouseCursor(mState_TUTORIALS);
		userInput->setMousePos(Environment.screenWidth * 0.5f,Environment.screenHeight * 0.5f);
		controlGui.update( isPaused() && !isPausedWithoutMenu(), false );
		return;
	}

	//---------------------------------------------------
	// Tactical overview: advance the blend-state and drive the camera from the
	// current blend. Hotkey detection lives in GameCamera::update (a proven
	// per-frame path with input already polled this frame).
	g_tacticalOverview.advance(frameLength);
	g_tacticalOverview.driveCamera(eye);

	// Overview squad-card click: select that force group and consume the click so
	// the world-pick under the card doesn't also fire. Runs before the mouse
	// handling below. Cards draw at raw screen coords (HUD-scale exempt).
	if ( g_tacticalOverview.active() && userInput->isLeftClick() && !userInput->isLeftDrag() )
	{
		const TacticalOverview::CardHit* hit = g_tacticalOverview.cardHitAt(
		             static_cast<float>(userInput->getRawMouseX()), static_cast<float>(userInput->getRawMouseY()) );
		if ( hit )
		{
			if ( hit->forceGroup >= 0 )
			{
				selectForceGroup( hit->forceGroup, true );
			}
			else if ( hit->unit )
			{
				Team* pTeam = Team::home;
				if ( pTeam )
					for ( long i = 0; i < pTeam->getRosterSize(); i++ )
					{
						Mover* m = static_cast<Mover*>(pTeam->getMover( i ));
						if ( m && m->isOnGUI() && commanderIsHome(m->getCommander()) )
							m->setSelected( 0 );
					}
				(static_cast<Mover*>(hit->unit))->setSelected( 1 );
			}
			userInput->clearLeftClick();
			g_tacticalOverview.armReleaseSuppression();	// stop the move on mouse-up
		}
	}

	// Formation line draw mode (MC2_TACMAP_FORMATION_LINE): owns LMB while
	// armed/dragging. Runs BEFORE the world mouse handlers below so the press
	// cannot select/deselect and the release cannot fire a normal move.
	// Rendering (ghost line + pips) stays in controlgui.
	if ( TacticalOverview::formationLineEnabled()
		&& g_tacticalOverview.flState() != TacticalOverview::FL_IDLE
		&& eye && Team::home )
	{
		long fmx = userInput->getMouseX();
		long fmy = userInput->getMouseY();

		if ( g_tacticalOverview.flState() == TacticalOverview::FL_ARMED )
		{
			if ( userInput->isLeftClick() )
			{
				// Legacy quadList picker: the worldToClipGL INVERSE is broken in
				// the game camera (far-plane unproject lands above the camera,
				// X response collapses - see inverseProject Phase 7B notes), so
				// the approx unprojectors cannot be used here. inverseProject is
				// the production picker; its O(quads) cost only runs during an
				// active formation drag.
				Stuff::Vector2DOf<long> sXY;
				sXY.x = fmx; sXY.y = fmy;
				glm::vec3 w;
				eye->inverseProject( sXY, w );
				{
					Mover* flMovers[32];
					int nm = 0;
					Team* pTeam = Team::home;
					for ( long i = 0; i < pTeam->getRosterSize() && nm < 32; i++ )
					{
						Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
						if ( pMover && pMover->isSelected()
							&& commanderIsHome(pMover->getCommander())
							&& pMover->getExists() && !pMover->isDestroyed() && !pMover->isDisabled() )
							flMovers[nm++] = pMover;
					}
					if ( nm == 0 )
					{
						g_tacticalOverview.flOnCancel();	// nothing selected
					}
					else
					{
						g_tacticalOverview.flSetMovers( flMovers, nm );
						g_tacticalOverview.flOnDragStart( w );
					}
					// Either way this press belongs to draw mode, not the
					// world: consume the click now and eat the matching
					// release later (suppression persists until consumed).
					userInput->clearLeftClick();
					g_tacticalOverview.armReleaseSuppression();
				}
			}
		}
		else // FL_DRAGGING
		{
			Stuff::Vector2DOf<long> sXY;
			sXY.x = fmx; sXY.y = fmy;
			glm::vec3 w;
			eye->inverseProject( sXY, w );
			g_tacticalOverview.flOnDragMove( w );

				// Spacing wheel: stretches/tightens the squad spread while
				// drawing. Consume it so the overview zoom-on-wheel (later this
				// frame) does not also fire.
				long flWheel = userInput->getMouseWheelDelta();
				if ( flWheel != 0 )
				{
					g_tacticalOverview.flAdjustSpacing( flWheel );
					userInput->clearMouseWheelDelta();
				}

			if ( userInput->leftMouseReleased() )
			{
				// Accidental click guard: tiny line = cancel, no orders.
				const float kMinLineLenWorld = 50.0f;	// tune later
				float ldx = g_tacticalOverview.flEnd().x - g_tacticalOverview.flStart().x;
				float ldy = g_tacticalOverview.flEnd().y - g_tacticalOverview.flStart().y;
				if ( ldx * ldx + ldy * ldy < kMinLineLenWorld * kMinLineLenWorld )
				{
					g_tacticalOverview.flOnCancel();
				}
				else
				{
					// v1 assignment: iterate snapshot order, each mover takes
					// nearest free slot. Orders FIRST, then flOnRelease
					// (release clears the snapshot).
					glm::vec3 slots[32];
					int ns = g_tacticalOverview.flComputeSlots( slots, 32 );
					// Endpoints sit on the terrain; interpolated slots can
					// float over dips - drop each onto the surface.
					if ( land )
						for ( int s = 0; s < ns; s++ )
							slots[s].z = land->getTerrainElevation( slots[s] );
					bool slotUsed[32] = { false };
					int nm = g_tacticalOverview.flMoverCount();
					for ( int i = 0; i < nm; i++ )
					{
						Mover* pMover = g_tacticalOverview.flMover( i );
						// Revalidate: mover may have died mid-drag.
						if ( !pMover || !pMover->getExists()
							|| pMover->isDestroyed() || pMover->isDisabled() )
							continue;
						int   best = -1;
						float bestD2 = 0.0f;
						glm::vec3 mp = pMover->getPosition();
						for ( int s = 0; s < ns; s++ )
						{
							if ( slotUsed[s] ) continue;
							float dx = slots[s].x - mp.x, dy = slots[s].y - mp.y;
							float d2 = dx * dx + dy * dy;
							if ( best < 0 || d2 < bestD2 ) { best = s; bestD2 = d2; }
						}
						if ( best < 0 ) break;
						slotUsed[best] = true;

						LocationNode path;
						path.location = slots[best];
						path.run = true;
						path.next = nullptr;
						TacticalOrder tacOrder;
						tacOrder.init( ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_MOVETO_POINT, false );
						tacOrder.initWayPath( &path );
						tacOrder.moveParams.wait = false;
						tacOrder.moveParams.wayPath.mode[0] = TRAVEL_MODE_FAST;
						tacOrder.pack( nullptr, nullptr );
						// Single-unit dispatch on purpose: handleOrders/
						// calcMoveGoals would re-cluster the slots.
						pMover->handleTacticalOrder( tacOrder );
					}
					g_tacticalOverview.flOnRelease();
					soundSystem->playDigitalSample( BUTTON5 );
				}
			}
		}
	}

	//---------------------------------------------------
	// Per Andy G.  One check per frame saves log file!
	bool shiftDn = userInput->shift();
	bool altDn = userInput->alt();
	bool ctrlDn = userInput->ctrl();

#ifndef FINAL
	constexpr auto MC2_DAMAGE_HEAD = 0;
	constexpr auto MC2_DAMAGE_LARM = 1;
	constexpr auto MC2_DAMAGE_RARM = 2;
	constexpr auto MC2_DAMAGE_LTORSO = 3;
	constexpr auto MC2_DAMAGE_RTORSO = 4;
	constexpr auto MC2_DAMAGE_CTORSO = 5;
	constexpr auto MC2_DAMAGE_LLEG = 6;
	constexpr auto MC2_DAMAGE_RLEG = 7;
	constexpr auto MC2_DAMAGE_RLTORSO = 8;
	constexpr auto MC2_DAMAGE_RRTORSO = 9;
	constexpr auto MC2_DAMAGE_RCTORSO = 10;
			
	DWORD CheatCommand = 0xffffffff;
	if (userInput->getKeyDown(KEY_1) && shiftDn && ctrlDn && altDn)
		CheatCommand = MC2_DAMAGE_HEAD;
	if (userInput->getKeyDown(KEY_2) && shiftDn && ctrlDn && altDn)
		CheatCommand = MC2_DAMAGE_LARM;
	if (userInput->getKeyDown(KEY_3) && shiftDn && ctrlDn && altDn)
		CheatCommand = MC2_DAMAGE_RARM;
	if (userInput->getKeyDown(KEY_4) && shiftDn && ctrlDn && altDn)
		CheatCommand = MC2_DAMAGE_LTORSO;
	if (userInput->getKeyDown(KEY_5) && shiftDn && ctrlDn && altDn)
		CheatCommand = MC2_DAMAGE_RTORSO;
	if (userInput->getKeyDown(KEY_6) && shiftDn && ctrlDn && altDn)
		CheatCommand = MC2_DAMAGE_CTORSO;
	if (userInput->getKeyDown(KEY_7) && shiftDn && ctrlDn && altDn)
		CheatCommand = MC2_DAMAGE_LLEG;
	if (userInput->getKeyDown(KEY_8) && shiftDn && ctrlDn && altDn)
		CheatCommand = MC2_DAMAGE_RLEG;
	if (userInput->getKeyDown(KEY_9) && shiftDn && ctrlDn && altDn)
		CheatCommand = MC2_DAMAGE_RLTORSO;
	if (userInput->getKeyDown(KEY_0) && shiftDn && ctrlDn && altDn)
		CheatCommand = MC2_DAMAGE_RRTORSO;
	if (userInput->getKeyDown(KEY_MINUS) && shiftDn && ctrlDn && altDn)
		CheatCommand = MC2_DAMAGE_RCTORSO;

	WeaponShotInfo cheatHit;

	switch (CheatCommand)
	{
		case 0xffffffff:
			break;
			
		case MC2_DAMAGE_HEAD:
			cheatHit.init(0,-2,CheatHitDamage,MECH_ARMOR_LOCATION_HEAD,0.0f);
		break;
		
		case MC2_DAMAGE_LARM:
			cheatHit.init(0,-2,CheatHitDamage,MECH_ARMOR_LOCATION_LARM,0.0f); 
		break;
		
 		case MC2_DAMAGE_RARM:
			cheatHit.init(0,-2,CheatHitDamage,MECH_ARMOR_LOCATION_RARM,0.0f); 
 		break;
		
    	case MC2_DAMAGE_LTORSO:
			cheatHit.init(0,-2,CheatHitDamage,MECH_ARMOR_LOCATION_LTORSO,0.0f); 
 		break;
		
    	case MC2_DAMAGE_RTORSO:
			cheatHit.init(0,-2,CheatHitDamage,MECH_ARMOR_LOCATION_RTORSO,0.0f); 
 		break;
		  
    	case MC2_DAMAGE_CTORSO:
			cheatHit.init(0,-2,CheatHitDamage,MECH_ARMOR_LOCATION_CTORSO,0.0f); 
 		break;
		
    	case MC2_DAMAGE_LLEG:
			cheatHit.init(0,-2,CheatHitDamage,MECH_ARMOR_LOCATION_LLEG,0.0f); 
 		break;
		
    	case MC2_DAMAGE_RLEG:
			cheatHit.init(0,-2,CheatHitDamage,MECH_ARMOR_LOCATION_RLEG,0.0f); 
 		break;
		
    	case MC2_DAMAGE_RLTORSO:
			cheatHit.init(0,-2,CheatHitDamage,MECH_ARMOR_LOCATION_RLTORSO,0.0f); 
 		break;
		
    	case MC2_DAMAGE_RRTORSO:
			cheatHit.init(0,-2,CheatHitDamage,MECH_ARMOR_LOCATION_RRTORSO,0.0f); 
 		break;
		
    	case MC2_DAMAGE_RCTORSO:
			cheatHit.init(0,-2,CheatHitDamage,MECH_ARMOR_LOCATION_RCTORSO,0.0f); 
 		break;
    }
	
	if (CheatCommand != -1)
	{
		Team* pTeam = Team::home;
		for (long i = 0; i < pTeam->getRosterSize(); i++)
		{
			Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
			if ( pMover->isSelected() && pMover->isMech())
			{
				pMover->handleWeaponHit(&cheatHit);
			}
		}
	}
#endif
	
	if ( bDrawHotKeys )
	{
		keyboardRef->update();
		//sebi: also check keys, otherwise no way to exit from keyboardref screen (when pressing F1)
		update(false, false, -1, -1, nullptr,  false);
		return;
	}

	mouseX = userInput->getMouseX();
	mouseY = userInput->getMouseY();

	// ---- MC2_VK_PICK_PROBE (BUG-A regression fence) ------------------------
	// Deterministic proof that the mouse->pick coordinate chain is DPI/resolution
	// consistent. Runs once, post-settle. Backend-independent (gl or vk both hit
	// the same gos_GetMouseInfo normalize). See gos_GetMouseInfo BUG-A fix.
	{
		static bool s_pickProbeDone = false;
		if (!s_pickProbeDone && (turn > 20) && false)
		{
			s_pickProbeDone = true;
			const char* be = mc2::gate::raw<"MC2_RENDER_BACKEND">();   // echoed as typed
			const float logW = static_cast<float>(Environment.screenWidth);
			const float logH = static_cast<float>(Environment.screenHeight);
			const int savedDW = Environment.drawableWidth;
			const int savedDH = Environment.drawableHeight;
			// preserve the live raw mouse so injecting synthetic points does not
			// corrupt real input on the next frame's poll.
			const input::MouseInfo* miSave = input::getMouseInfo();
			const float rawSaveX = miSave ? miSave->x_ : 0.f;
			const float rawSaveY = miSave ? miSave->y_ : 0.f;

			auto readPick = [&](float rawX, float rawY, long& px, long& py) {
				gos_TestSetRawMouse(rawX, rawY);
				float nx = 0.f, ny = 0.f; int a = 0, b = 0, c = 0; DWORD d = 0;
				gos_GetMouseInfo(&nx, &ny, &a, &b, &c, &d);
				userInput->pickProbeSetNormalized(nx, ny);
				px = userInput->getMouseX();
				py = userInput->getMouseY();
			};

			GameObjectPtr targetMover = nullptr; float tcx = 0.f, tcy = 0.f;
			if (Team::home)
			{
				for (long i = 0; i < Team::home->getRosterSize(); ++i)
				{
					Mover* m = static_cast<Mover*>(Team::home->getMover(i));
					if (!m || !m->getExists()) continue;
					AppearancePtr ap = m->getAppearance();
					if (!ap || !ap->canBeSeen()) continue;
					const float bw = ap->lowerRight.x - ap->upperLeft.x;
					const float bh = ap->lowerRight.y - ap->upperLeft.y;
					if (bw < 2.f || bh < 2.f) continue;
					const float bcx = 0.5f * (ap->upperLeft.x + ap->lowerRight.x);
					const float bcy = 0.5f * (ap->upperLeft.y + ap->lowerRight.y);
					if (bcx < 0.f || bcx > logW || bcy < 0.f || bcy > logH) continue;
					targetMover = m; tcx = bcx; tcy = bcy; break;
				}
			}

			// Sweep the exact field DPI ratio (4K @225% -> drawable/window = 2.25) and
			// the unscaled ratio (1080p @100% -> 1.0, where the bug is invisible so this
			// is the no-regression check). The fix normalizes the mouse by the real SDL
			// window (== canvas), so faking Environment.drawableWidth only exercises the
			// OLD divide-by-drawable divisor -> a clean discriminator on any display.
			struct Cfg { const char* label; float ratio; };
			const Cfg cfgs[2] = { {"4K@225%", 2.25f}, {"1080p@100%", 1.0f} };
			bool allOK = true;
			for (const auto & cfg : cfgs)
			{
				Environment.drawableWidth  = static_cast<int>((logW * cfg.ratio + 0.5f));
				Environment.drawableHeight = static_cast<int>((logH * cfg.ratio + 0.5f));

				// (A) identity: a cursor physically over on-canvas (cx,cy) arrives from
				// SDL as window-points (cx,cy); getMouseX must return it unchanged.
				const float cx = 0.75f * logW, cy = 0.5f * logH;
				long px = 0, py = 0; readPick(cx, cy, px, py);
				const long dxErr = (px > (long)cx) ? (px - static_cast<long>(cx)) : (static_cast<long>(cx) - px);
				const long dyErr = (py > (long)cy) ? (py - static_cast<long>(cy)) : (static_cast<long>(cy) - py);
				const bool idOK = (dxErr <= 2) && (dyErr <= 2);
				// NEGATIVE CONTROL: what the OLD divide-by-drawable formula would give.
				const long oldPx = static_cast<long>(((cx / (logW * cfg.ratio)) * logW));
				mc2::fmt::print_out("[VK_PICK_PROBE] backend={} res={} canvas={:.0f}x{:.0f} drawable={:d}x{:d} identity raw=({:.0f},{:.0f}) pick=({:d},{:d}) err=({:d},{:d}) oldBugWouldGive={:d} -> {}\n", mc2::fmt::safe(be ? be : "gl"), mc2::fmt::safe(cfg.label), logW, logH, mc2::fmt::as_i32(Environment.drawableWidth), mc2::fmt::as_i32(Environment.drawableHeight), cx, cy, mc2::fmt::as_i32(px), mc2::fmt::as_i32(py), mc2::fmt::as_i32(dxErr), mc2::fmt::as_i32(dyErr), mc2::fmt::as_i32(oldPx), mc2::fmt::safe(idOK ? "PASS" : "FAIL"));

				// (B) object: aim at the mover's screen-bbox centre; the full pick
				// (findObjectByMouse) must resolve back to that same mover.
				bool objOK = true;
				if (targetMover)
				{
					long ox = 0, oy = 0; readPick(tcx, tcy, ox, oy);
					GameObjectPtr picked = ObjectManager->findObjectByMouse(ox, oy);
					objOK = (picked == targetMover);
					mc2::fmt::print_out("[VK_PICK_PROBE] backend={} res={} object aimCenter=({:.0f},{:.0f}) pick=({:d},{:d}) target={:016X} picked={:016X} -> {}\n", mc2::fmt::safe(be ? be : "gl"), mc2::fmt::safe(cfg.label), tcx, tcy, mc2::fmt::as_i32(ox), mc2::fmt::as_i32(oy), mc2::fmt::as_ptr(static_cast<void*>(targetMover)), mc2::fmt::as_ptr(static_cast<void*>(picked)), mc2::fmt::safe(objOK ? "PASS" : "FAIL"));
				}
				else
				{
					mc2::fmt::print_out("[VK_PICK_PROBE] backend={} res={} object SKIP (no visible home mover w/ bbox)\n", mc2::fmt::safe(be ? be : "gl"), mc2::fmt::safe(cfg.label));
				}
				allOK = allOK && idOK && objOK;
			}

			Environment.drawableWidth  = savedDW;
			Environment.drawableHeight = savedDH;
			gos_TestSetRawMouse(rawSaveX, rawSaveY);
			mc2::fmt::print_out("[VK_PICK_PROBE] VERDICT={}\n", mc2::fmt::safe(allOK ? "PASS" : "FAIL"));
			fflush(stdout);
		}
	}

	bool bGui = false;

	// Raw-space GUI test. ae7e1794 switched the bGui hit-test below to
	// HUD-inverse space so a click on a drawn-shrunk command-bar widget would
	// correctly register / suppress a spurious world order. But gos_HudInverseMousePoint
	// maps the WHOLE s_hud_scale shrink band (raw y >= ~0.66*sh) outward toward the
	// bottom-center anchor, so an OPEN world pixel sitting above the actual drawn
	// chrome (but inside the shrink band) gets pushed DOWN into an authored widget
	// rect -> inRegion(HUD-inverse) wrongly returns true over the lower-center world.
	// That over-claim made updateTarget zero a genuinely-hovered unit (target=0 at
	// the `if(bGui)` below), so the hovered mech/vehicle/building name stopped
	// rendering for units in the lower screen band (regression since 2026-06-10).
	// findObjectByMouse picks in RAW space (what the user sees), so the name/target
	// gate must also use a RAW-space GUI test; the drawn-shrunk widget rect is a
	// subset of its authored rect, so raw-in-authored-region still returns true when
	// the cursor is genuinely over a drawn widget. bGui (HUD-inverse) is kept for
	// order/drag suppression so the ae7e1794 widget-click fix is unchanged.
	bool bRawGui = controlGui.inRegion( mouseX, mouseY, isPaused() && !isPausedWithoutMenu() );

	// check and see if its in the control area
	// HUD command bar draws shrunk -> test the region in HUD-inverse space (the
	// raw mouseX/mouseY locals stay raw for terrain inverseProject below).
	if ( controlGui.inRegion( userInput->getMouseHudX(), userInput->getMouseHudY(), isPaused() && !isPausedWithoutMenu() ) )
	{
		bGui = true;
		if ( userInput->isLeftClick() && !userInput->isLeftDrag() )
		{
			dragStart.x = 0.f;
			dragStart.y = 0.f;
		}
	}
	// Clear stale help text only when the cursor is genuinely over a drawn HUD
	// widget (raw space). Using bGui (HUD-inverse) here would leave the previous
	// frame's text up over the over-claimed world band; using bRawGui keeps the
	// hovered-unit name path (set in updateTarget below) authoritative for the world.
	if ( !bRawGui )
	{
		helpTextHeaderID = helpTextID = 0;
	}

	if (bGui)
		userInput->setMouseCursor( mState_NORMAL );

	Stuff::Vector2DOf<long>	mouseXY;
	mouseXY.x = mouseX;
	mouseXY.y = mouseY;

	// VPL-retirement Step 3 3b: mandatory per-frame delta-cache. This is the
	// SOLE production Camera::inverseProject caller and runs every frame; the
	// repointed inverseProject (Step 3 3a) walks every quad. Skip the walk
	// when neither the cursor pixel (exact, >=1px, no tolerance) nor the
	// camera world->clip matrix changed since the last real projection.
	const glm::mat4& curW2C = eye->getWorldToClip();
	const bool mouseSame = (mouseX == prevMouseX) && (mouseY == prevMouseY);
	const bool camSame   = inverseProjectCacheValid &&
		(memcmp(&cachedWorldToClip, &curW2C, sizeof(glm::mat4)) == 0);
	if (inverseProjectCacheValid && mouseSame && camSame)
	{
		wPos = cachedWPos;
		if (s_mfOn) { ++s_mfInvHits; ++s_mfWinHits; }
	}
	else
	{
		// Coarse cache-miss-frames-only zone (not per-quad): the O(quads)
		// inverseProject walk was invisible in Tracy without it.
		ZoneScopedN("MIF.InverseProject");
		MifScope _mInv(MifPhase::MF_INVPROJ);
		if (s_mfOn) { ++s_mfInvWalks; ++s_mfWinWalks; }
		// Per-frame cursor world position: ray-march the camera ray over the
		// heightfield (pickTerrainRay, O(ray length), forward-projection verified).
		// History: the z=0 plane approx (screenToGroundPlaneApprox) was dropped here
		// because it returned a near-constant garbage point (hover passability showed a
		// red-X "dead area", dragStart anchored off-screen). Root cause: worldToClipGL()
		// already has the MC2->GL axis swap as its first factor, so its inverse returns MC2
		// world directly; those helpers swapped a second time, which collapsed the X response
		// and put the far point above the camera. pickTerrainRay does not swap again, builds the
		// ray from the matrix alone, intersects the real heightfield, and rejects any hit that
		// does not project back onto the cursor.
		// The legacy inverseProject walked every quad per cursor/camera-change frame
		// (85-126 ms on a 1024-cell map); it is now only the discrete-click path.
		static const bool s_pickParityDiag = false;
		const std::chrono::steady_clock::time_point _cpuT0 =
			s_gpParityOn ? std::chrono::steady_clock::now()
			             : std::chrono::steady_clock::time_point{};
		pickCursorWorld(eye, mouseX, mouseY, wPos, cachedWPos, inverseProjectCacheValid);
		if (s_pickParityDiag)
			pickRayParitySweep(eye);
		// GPU-GROUND-PICK-PARITY-1: oracle fires only on the quad-walk branch (the
		// exact O(quads) cost the GPU depth-unproject would replace); the cache-hit
		// branch above skips both paths, matching the recon's "per cursor-move
		// frame" comparison basis (R1). Parity-only; wPos is untouched.
		if (s_gpParityOn) {
			const unsigned long long _cpuNs = (unsigned long long)
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - _cpuT0).count();
			gpParitySite(mouseX, mouseY, _cpuNs, wPos.x, wPos.y, wPos.z, "hover");
			// GPU-GROUND-PICK-PARITY-1 center probe: unattended smoke parks the
			// cursor at (0,0) so the cursor path never samples real terrain
			// (compared=0). When MC2_GPU_GROUND_PICK_PARITY_CENTER=1, run a
			// screen-CENTER CPU-vs-GPU parity comparison over the terrain the
			// camera is actually looking at (incl. displaced maps). CPU pick at
			// center is a separate inverseProject; wPos above is NOT disturbed.
			if (s_gpCenter) {
				const int cxPix = Environment.screenWidth  / 2;
				const int cyPix = Environment.screenHeight / 2;
				Stuff::Vector2DOf<long> centerXY;
				centerXY.x = cxPix; centerXY.y = cyPix;
				glm::vec3 centerW;
				eye->inverseProject(centerXY, centerW);
				gpCenterProbe(cxPix, cyPix, centerW.x, centerW.y, centerW.z);
			}
		}
		prevMouseX = mouseX;
		prevMouseY = mouseY;
		cachedWPos = wPos;
		cachedWorldToClip = curW2C;
		inverseProjectCacheValid = true;
	}

	int cellR, cellC;
	bool passable = 1;
	bool lineOfSight = 0;
	{
		ZoneScopedN("MIF.LOS"); MifScope _mLOS(MifPhase::MF_LOS);
		if ( Terrain::IsGameSelectTerrainPosition( wPos ) )
		{
			land->worldToCell(wPos, cellR, cellC);
			if (Team::home)	   //May go NULL during multiplayer when a player first dies?
				lineOfSight = Team::home->teamLineOfSight(wPos,0.0f);
		}
	}


	// update buttons and stuff, even if not in region, it draws objectives and stuff
	{
		ZoneScopedN("MIF.ControlGui"); MifScope _mCG(MifPhase::MF_CTRLGUI);
		controlGui.update( isPaused() && !isPausedWithoutMenu(), lineOfSight );
	}

	bool leftClicked = (!userInput->isLeftDrag() && !userInput->isRightDrag() && userInput->isLeftClick());
	bool rightClicked = (!userInput->isLeftDrag() && !userInput->wasRightDrag() && userInput->rightMouseReleased());
	bool bLeftDouble = userInput->isLeftDoubleClick();


	{
		ZoneScopedN("MIF.UpdateTarget"); MifScope _mUT(MifPhase::MF_UPDTGT);
		// Pass the RAW-space GUI flag (not the HUD-inverse bGui): the world pick +
		// hovered-unit name must be suppressed only when the cursor is over a widget
		// in the space the pick (findObjectByMouse) and the user actually use.
		updateTarget(bRawGui);
	}

	{ ZoneScopedN("MIF.PostTarget");
	attilaXAxis 	= userInput->getAttilaXAxis();  
	attilaYAxis		= userInput->getAttilaYAxis();
	attilaRZAxis	= userInput->getAttilaRAxis();
	
	int row, col;
	land->worldToCell(wPos, row, col);
	mc2::fmt::to_buf(DebugStatusBarString, "TIME: {:06d}, MOUSE: [{:d}, {:d}] {:d},{:d},{:d} ({:.2f}, {:.2f}, {:.2f}), PATHMGR: {:02d}({:02d})", mc2::fmt::as_i32(static_cast<long>(scenarioTime)), mc2::fmt::as_i32(row), mc2::fmt::as_i32(col), mc2::fmt::as_i32(GlobalMoveMap[0] ? GlobalMoveMap[0]->calcArea(row, col) : -1), mc2::fmt::as_i32(GlobalMoveMap[1] ? GlobalMoveMap[1]->calcArea(row, col) : -1), mc2::fmt::as_i32(GlobalMoveMap[2] ? GlobalMoveMap[2]->calcArea(row, col) : -1), wPos.x, wPos.y, wPos.z, mc2::fmt::as_i32(PathManager->numPaths), mc2::fmt::as_i32(PathManager->peakPaths));
	if (MPlayer) {
		char mpStr[256];
		if (MPlayer->isServer())
			mc2::fmt::to_buf(mpStr, ", MULTIPLAY: {}-({:d})SERVER {{{:d},{:d}}}", mc2::fmt::safe(MPlayer->getPlayerName()), mc2::fmt::as_i32(MPlayer->commanderID), mc2::fmt::as_i32(MPlayer->maxReceiveLoad), mc2::fmt::as_i32(MPlayer->maxReceiveSize));
		else
			mc2::fmt::to_buf(mpStr, ", MULTIPLAY: {}-({:d})CLIENT {{{:d},{:d}}}", mc2::fmt::safe(MPlayer->getPlayerName()), mc2::fmt::as_i32(MPlayer->commanderID), mc2::fmt::as_i32(MPlayer->maxReceiveLoad), mc2::fmt::as_i32(MPlayer->maxReceiveSize));
		strcat(DebugStatusBarString, mpStr);
	}
	if (EnemiesGoalPlan)
		strcat(DebugStatusBarString, " [ENEMIES GOALPLAN]");
	if (ShowMovers)
		strcat(DebugStatusBarString, " [SHOW MOVERS]");
	if (CullPathAreas)
		strcat(DebugStatusBarString, " [CULL PATH AREAS]");
	if (ZeroHPrime)
		strcat(DebugStatusBarString, " [ZERO HPRIME]");
	if (CalcValidAreaTable)
		strcat(DebugStatusBarString, " [CALC VALID AREA]");
	if (GlobalMap::logEnabled)
		strcat(DebugStatusBarString, " [GLOBAL LOG]");
	if (!MechWarrior::brainsEnabled[1])
		strcat(DebugStatusBarString, " [BRAINDEAD: 1]");

	if ( bLeftDouble && target && target->isMover() && target->getTeam() == Team::home )
	{
		int forceGroup = -1;
		for ( int i = 0; i < 10; i++ )
		{
			if ( (static_cast<Mover*>(target))->isInUnitGroup( i ) )
			{
				forceGroup = i;
				break;
			}
		}

		if ( forceGroup != -1 )
			selectForceGroup( forceGroup, true );

		lastUpdateDoubleClick = true;
		
	}

	Team* pTeam = Team::home;

	int moverCount = 0;
	int nonMoverCount = 0;
	for (long i=0;i<pTeam->getRosterSize();i++)
	{
		Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
		if ( pMover->isSelected() && commanderIsHome(pMover->getCommander())  )
		{
			if ( pMover->maxMoveSpeed )
			{
				passable &= pMover->canMoveHere( wPos );
				moverCount++;
			}
			else
				nonMoverCount++;
		}

	}
	
	{
		ZoneScopedN("MIF.InputStyle");
		if( useLeftRightMouseProfile ) // using AOE control style
		{
			if ( WAYPOINT_KEY == -1 )
				WAYPOINT_KEY = KEY_LCONTROL;

			commandClicked = rightClicked;
			selectClicked = !bLeftDouble && !lastUpdateDoubleClick && userInput->leftMouseReleased() && !userInput->getKeyDown( KEY_T) && !isDragging;
			cameraClicked = gos_GetKeyStatus( KEY_LMENU ) == KEY_HELD;
			if ( moveCameraAround( lineOfSight, passable, ctrlDn, bGui, moverCount, nonMoverCount ) )
			{
				bool leftClicked = (!userInput->isLeftDrag() && !userInput->isRightDrag() && userInput->isLeftClick());
				bool rightClicked = (!userInput->isLeftDrag() && !userInput->wasRightDrag() && userInput->rightMouseReleased());

				update( leftClicked, rightClicked, mouseX, mouseY, target,  lineOfSight );

				return;
			}

			updateAOEStyle(shiftDn, altDn, ctrlDn, bGui, lineOfSight, passable, moverCount, nonMoverCount);


		}
		else // using mc1 style
		{
			commandClicked = leftClicked;
			selectClicked = leftClicked && !lastUpdateDoubleClick;
			cameraClicked = userInput->isRightDrag();

			if ( WAYPOINT_KEY == -1 )
				WAYPOINT_KEY = KEY_LCONTROL;
			if ( moveCameraAround( lineOfSight, passable, ctrlDn, bGui, moverCount, nonMoverCount ) )
			{
				bool leftClicked = (!userInput->isLeftDrag() && !userInput->isRightDrag() && userInput->isLeftClick());
				bool rightClicked = (!userInput->isLeftDrag() && !userInput->wasRightDrag() && userInput->rightMouseReleased());

				update( leftClicked, rightClicked, mouseX, mouseY, target,  lineOfSight );
				return;
			}

			updateOldStyle(shiftDn, altDn, ctrlDn, bGui, lineOfSight, passable, moverCount, nonMoverCount);
		}
	}

	{
		ZoneScopedN("MIF.DrawBars"); MifScope _mDB(MifPhase::MF_DRAWBARS);
		for (int i = 0; i < Team::home->getRosterSize(); i++ )
		{
			Mover* pMover = static_cast<Mover*>(Team::home->getMover( i ));
			MechWarrior* pilot = pMover->getPilot();
			if ( pilot && commanderIsHome(pMover->getCommander()))
			{
				GameObject* pTmpTarget = pilot->getCurrentTarget( );
				if ( pTmpTarget && (i < MAX_ICONS))	//Must check this because old test maps have more then 16 movers on them!!
				{
					pTmpTarget->setDrawBars(true);
					oldTargets[i] = pTmpTarget;
					s_oldTargetWatchID[i] = pTmpTarget->getWatchID(); // for next-frame liveness check
				}
			}
		}
	}

	} // MIF.PostTarget

	if ( !bLeftDouble && !( lastUpdateDoubleClick &&
		userInput->getMouseLeftButtonState() == MC2_MOUSE_DOWN ) )// check for the hold )
		lastUpdateDoubleClick = false;

	{
		ZoneScopedN("MIF.Rollovers"); MifScope _mRO(MifPhase::MF_ROLLOVERS);
		updateRollovers();
	}

	if (s_mfOn)
		mfFrameEnd(static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - _mifT0).count()));
}

	
// bGui here is the RAW-space "cursor over a drawn HUD widget" flag (bRawGui at the
// caller), NOT the HUD-inverse order-suppression bGui. It only gates whether a
// genuinely-picked world object (and its name help-text) is kept. See bRawGui note
// in update().
long g_hoverTipNameID = 0;
long g_hoverTipX = 0;
long g_hoverTipY = 0;

void MissionInterfaceManager::updateTarget( bool bGui)
{
	g_hoverTipNameID = 0;
	
	// MISSION-START-HOVER-TARGET-LIFETIME-1 (completed): suppress hover BEFORE any deref.
	// During mission start / settle frames / teardown the persistent target + oldTargets[]
	// can point at freed objects, and findObjectByMouse can return a stale/garbage pointer.
	// Clear hover state WITHOUT dereferencing (no setDrawBars/setTargeted -> no virtual call
	// on freed memory) and bail. s_hoverSettleFrames keeps picking off for a few live frames
	// after arming, so the first beginMission update can't pick a not-yet-ready world.
	if ( s_hoverSuppressed || s_hoverSettleFrames > 0 )
	{
		if ( s_hoverSettleFrames > 0 )
			s_hoverSettleFrames--;
		memset( oldTargets, 0, sizeof(GameObject*) * MAX_ICONS );
		memset( s_oldTargetWatchID, 0, sizeof(s_oldTargetWatchID) );
		target = nullptr;
		s_targetWatchID = 0;
		return;
	}

	// Validate the PERSISTENT target + oldTargets are still live before dereferencing them:
	// an object targeted last frame may have died this frame. getByWatchID(wid) is a registry
	// lookup (NO deref of the possibly-freed pointer); if it no longer maps to our pointer the
	// object is gone -> drop it silently (its draw-bars died with it).
	if ( target && ObjectManager->getByWatchID( s_targetWatchID ) != target )
	{
		target = nullptr;
		s_targetWatchID = 0;
	}
	for ( int i = 0; i < MAX_ICONS; i++ )
	{
		if ( oldTargets[i] && ObjectManager->getByWatchID( s_oldTargetWatchID[i] ) != oldTargets[i] )
		{
			oldTargets[i] = nullptr;
			s_oldTargetWatchID[i] = 0;
		}
	}

	mc2::pick::refreshHover(bGui, target, [&] {
		// Clear previous hover even when the pointer is covered by GUI.
		for ( int i = 0; i < MAX_ICONS; i++ )
		{
			if ( oldTargets[i] )
			{
				oldTargets[i]->setDrawBars( 0 );
				oldTargets[i] = nullptr;
				s_oldTargetWatchID[i] = 0;
			}
		}
		if ( target )
			target->setTargeted( 0 );
	}, [&] { return ObjectManager->findObjectByMouse(mouseX, mouseY); });
	if (bGui)
		s_targetWatchID = 0;
	if ( target )
	{
		// Remember the freshly-picked (live) target's watch-ID so next frame's top-of-function
		// validation can detect if its object dies. Picking only runs post-settle, so target
		// here is a live object -> getWatchID() is safe. target may be nulled by the checks
		// below (bGui/sensor/destroyed); that's fine -- the WID is only consulted when target
		// is non-null next frame, and target is never reassigned to a different object here.
		s_targetWatchID = target->getWatchID();
		// Drop a stale/freed pick before ANY virtual dispatch below. getWatchID()
		// is a non-virtual member read (safe even on freed memory); round-trip it
		// through the ObjectManager to confirm the object is still live+registered.
		// Without this, a mech that died the same frame (common under cheat-mode
		// mass-kill) leaves a dangling target whose first virtual call
		// (isDisabled / getDescription) jumps through a freed vtable -> EXEC at 0x0.
		if ( ObjectManager->getByWatchID( s_targetWatchID ) != target )
			target = nullptr;
		if ( target && target->isMover() && !ShowMovers && !(MPlayer && MPlayer->allUnitsDestroyed[MPlayer->commanderID]))
		{
			if (!static_cast<Mover*>(target)->contactView.blip)
				target = nullptr;
		}
		
		// STABILITY: a disabled mech may be mid-teardown (null/partial appearance,
		// stale vtable) — the virtual derefs below (getDescription / getAppearance)
		// then jump through a freed slot and crash (EXEC at 0x0). This surfaces under
		// cheat-mode soak mass-disable but is a latent hazard whenever a hovered mech
		// dies the same frame. Don't compute hover info for a disabled target.
		// ISSUE-42 (v0.5.0 salvage regression): the original guard dropped EVERY
		// disabled target, but disabled-not-destroyed is exactly the salvageable
		// downed-mech state — it made the info cursor, info window, and salvage
		// craft/order unreachable for downed mechs. Narrow it to targets that are
		// genuinely torn down (appearance already gone). Freed/stale targets were
		// already dropped by the watch-ID round-trip above; a LIVE disabled mech
		// with an appearance is safe to hover (stock v0.4c behavior restored).
		if ( target && target->isDisabled() && !target->getAppearance() )
			target = nullptr;

		if ( target )
		{
			// MISSION-START-HOVER-TARGET-LIFETIME-1: secondary defensive guard (insurance,
			// not the fix). The lifecycle invalidation + suppression above is what prevents a
			// stale/freed target reaching this virtual call at mission start.
			if ( !target )
				return;
			int descID = target->getDescription();
			long hoverName = descID;
			if (target->isMover()) {
				const auto& contact = static_cast<Mover*>(target)->contactView;
				hoverName = contact.tag ? mc2::hoverNameId(contact.quality,
					target->getTonnage(), descID, target->getObjectClass() != BATTLEMECH) : 0;
			}
			// ISSUE-42 deref hardening: getAppearance() may be NULL for a
			// partially-initialized/mid-teardown object; guard the deref.
			if ( descID != -1 && target->getAppearance() )
			{
				int nameID = target->isMover() ? hoverName : target->getAppearance()->getObjectNameId();

				helpTextHeaderID = nameID;
				helpTextID = target->isMover() ? hoverName : descID;
			}
			// Only objects that own a name: movers and the building classes. Scenery terrain objects (fences, lights,
			// tents) keep the appearance default (the Processing Plant string) and have no name of their own.
			const long hoverClass = target->getObjectClass();
			if ( ( target->isMover() || hoverClass == BUILDING || hoverClass == TURRET || hoverClass == GATE ) && target->getAppearance() && (target->isMover() ? hoverName : target->getAppearance()->getObjectNameId()) > 0 )
			{
				g_hoverTipNameID = target->isMover() ? hoverName : target->getAppearance()->getObjectNameId();
				g_hoverTipX = mouseX;
				g_hoverTipY = mouseY;
			}
			if ( target->isDestroyed() )
			{
				target = nullptr;
			}
			
			else if ( target->isDisabled() && !target->isMover() )
				target = nullptr;
			else if ( !target->isSelectable())
			{
				if ( !target->isDisabled() && !target->isMover() )
					target = nullptr;
			}
		}
	}

	// CURSOR_TARGET symptom trace (INPUT-CURSOR-OFFSCREEN-TARGET-RECON-1): emit when a
	// resolved hover/command target is anomalous — projects behind the camera (w<=1e-4)
	// or lands far from the cursor. This is the offscreen-pick symptom; pairs with the
	// pick_rect mechanism emit in objmgr.cpp. Gate MC2_CURSOR_TARGET_TRACE +
	// MC2_DIAG_TAGS=CURSOR_TARGET. Read via get_diagnostic_events("CURSOR_TARGET").
	// Recon only — no behavior change.
	{
		static const bool s_cursorTargetTrace = false;
		if ( s_cursorTargetTrace && target && eye && mc2_diag::tagEnabled("CURSOR_TARGET") )
		{
			glm::vec3 _ct_pos = target->getPosition();
			Stuff::Vector4D _ct_sp;
			eye->projectForScreenXY( _ct_pos, _ct_sp );
			bool  _ct_behind = (_ct_sp.w <= 1e-4f);
			float _ct_dx = _ct_sp.x - static_cast<float>(mouseX);
			float _ct_dy = _ct_sp.y - static_cast<float>(mouseY);
			float _ct_distSq = _ct_dx*_ct_dx + _ct_dy*_ct_dy;
			if ( _ct_behind || _ct_distSq > 90000.0f )   // behind camera, or >300px from cursor
			{
				char _ct_buf[320];
				mc2::fmt::to_buf(_ct_buf, "{{\"site\":\"update_target\",\"mouse\":[{:d},{:d}],\"target_screen\":[{:.1f},{:.1f}],\"w\":{:.4f},\"behind\":{:d},\"dist_sq\":{:.0f},\"is_mover\":{:d},\"watch_id\":{:d}}}", mc2::fmt::as_i32(static_cast<long>(mouseX)), mc2::fmt::as_i32(static_cast<long>(mouseY)), _ct_sp.x, _ct_sp.y, _ct_sp.w, mc2::fmt::as_i32(_ct_behind?1:0), _ct_distSq, mc2::fmt::as_i32(target->isMover()?1:0), mc2::fmt::as_i32(static_cast<int>(s_targetWatchID)));
				mc2_diag::writeEvent("CURSOR_TARGET", 1, 0, _ct_buf);
			}
		}
	}
}
