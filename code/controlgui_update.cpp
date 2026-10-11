// ControlGui: ControlGui::update with its MIF cost scopes, and the button click / stop handlers.
// Declared in the class headers; shared names in controlgui_internal.h.
#include "controlgui_internal.h"
#include "mc2fmt.h"

namespace {
	enum CgPhase { CG_PAUSE=0, CG_BTNHOVER, CG_ROSTER, CG_MOVERSTATE, CG_TACMAP, CG_INFOWND, CG_VEHTAB, CG_FGBAR };
	static const bool s_cgSplitOn = mc2::gate::get<"MC2_MIF_SPLIT">() || mc2::gate::get<"MC2_IFACE_COST_SPLIT">();
	struct CgScope {
		int idx; std::chrono::steady_clock::time_point t0;
		explicit CgScope(int i):idx(i){ if(s_cgSplitOn) t0=std::chrono::steady_clock::now(); }
		~CgScope(){ if(!s_cgSplitOn) return; g_ifaceCgFrameNs[idx]+=(unsigned long long)
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count(); }
	};
}

void ControlGui::update( bool bPaused, bool bLOS )
{
	mouseInVehicleStopButton = 0;
	bool bMouseInButton = 0;

	{ ZoneScopedN("CGui.ServerMissing"); showServerMissing(); }

	if ( bPaused )
	{ ZoneScopedN("CGui.PauseWnd"); CgScope _c(CG_PAUSE); pauseWnd->update(); }

	// INFO-CAPTURE fixture: MC2_BOOT_INFO=N force-selects the first home mover into
	// the Info window + switches the command bar to the INFO tab ONCE at frame >= N,
	// so the in-mission info panel can be captured / iterated headless (no click
	// path). Same fixture pattern as MC2_BOOT_PAUSED (missiongui.cpp). Registered
	// Trace, default unset -> byte-identical.
	{
		static const long s_bootInfo = static_cast<long>(mc2::gate::get<"MC2_BOOT_INFO">());
		if ( s_bootInfo > 0 && infoWnd && ObjectManager && Team::home )
		{
			extern uint32_t g_mc2FrameCounter;
			static bool s_infoDone = false;
			if ( !s_infoDone && (long)g_mc2FrameCounter >= s_bootInfo )
			{
				for ( int i = 0; i < ObjectManager->numMovers; i++ )
				{
					MoverPtr m = ObjectManager->getMover( i );
					if ( m && m->getTeamId() == Team::home->id && m->getExists() )
					{
						s_infoDone = true;
						infoWnd->setUnit( m );
						// Tabs are mutually exclusive; the render if/else picks
						// TACMAP first, so un-press the siblings before pressing
						// INFO or the minimap keeps winning the left-column slot.
						getButton( TACMAP_TAB )->press( false );
						getButton( VEHICLE_TAB )->press( false );
						getButton( INFO_TAB )->press( true );
						mc2::fmt::print_out("[BOOT_INFO] selected mover teamId={:d} -> INFO tab (frame {:d})\n", mc2::fmt::as_i32(m->getTeamId()), mc2::fmt::as_u32((unsigned)g_mc2FrameCounter));
						fflush(stdout);
						break;
					}
				}
			}
		}
	}

	// OBJ-CAPTURE fixture: MC2_BOOT_OBJ=N calls startObjectives(1) ONCE at frame
	// >= N, so the in-mission objectives overlay can be captured / iterated
	// headless (no click path to the pause-menu OBJECTIVES button). Same fixture
	// pattern as MC2_BOOT_INFO above / MC2_BOOT_PAUSED (missiongui.cpp).
	// Registered Trace, default unset -> byte-identical.
	{
		static const long s_bootObj = static_cast<long>(mc2::gate::get<"MC2_BOOT_OBJ">());
		if ( s_bootObj > 0 && Team::home )
		{
			extern uint32_t g_mc2FrameCounter;
			static bool s_objDone = false;
			if ( !s_objDone && (long)g_mc2FrameCounter >= s_bootObj )
			{
				s_objDone = true;
				startObjectives( 1 );
				mc2::fmt::print_out("[BOOT_OBJ] startObjectives(1) (frame {:d})\n", mc2::fmt::as_u32((unsigned)g_mc2FrameCounter));
				fflush(stdout);
			}
		}
	}

	// OBJ-COMPLETE-CAPTURE fixture: MC2_BOOT_OBJ_COMPLETE=N force-resolves the
	// FIRST active, non-hidden objective to OS_SUCCESSFUL ONCE at frame >= N
	// through the same public CObjective::Status(newStatus) setter the engine
	// uses to restore resolved objectives (code/objective.cpp:2003 — sets
	// m_resolved/m_resolvedStatus, no action-list side effects), so the
	// completed-status render states (green row + check tile, shared
	// gatherObjectiveRows status->color map) are capturable headless: no
	// objective in mc2_01 is completable inside a 30 s scripted run
	// (objective 1 = MoveAnyUnitToArea needs a player move order). The mission
	// does NOT end — the remaining primaries stay undetermined. Same fixture
	// pattern as MC2_BOOT_OBJ above. Registered Trace, default unset ->
	// byte-identical.
	{
		static const long s_bootObjComplete = static_cast<long>(mc2::gate::get<"MC2_BOOT_OBJ_COMPLETE">());
		if ( s_bootObjComplete > 0 && Team::home )
		{
			extern uint32_t g_mc2FrameCounter;
			static bool s_objCompleteDone = false;
			if ( !s_objCompleteDone && (long)g_mc2FrameCounter >= s_bootObjComplete )
			{
				s_objCompleteDone = true;
				for ( EList< CObjective*, CObjective* >::EIterator iter = Team::home->objectives.Begin();
					!iter.IsDone(); iter++ )
				{
					CObjective* pObjective = (*iter);
					if ( !pObjective->IsActive() || pObjective->IsHiddenTrigger() )
						continue;
					pObjective->Status( OS_SUCCESSFUL );
					mc2::fmt::print_out("[BOOT_OBJ_COMPLETE] objective '{}' -> OS_SUCCESSFUL (frame {:d})\n", pObjective->LocalizedDescription().Data(), mc2::fmt::as_u32((unsigned)g_mc2FrameCounter));
					fflush(stdout);
					break;
				}
			}
		}
	}

	// TIMER-CAPTURE fixture: MC2_BOOT_TIMER=N force-arms the mission timer ONCE
	// at frame >= N by giving the mission a time limit (tier1 missions ship with
	// TimeLimit unset -> m_timeLimit == -1 -> the timer panel never draws), so
	// the in-mission mission-timer display can be captured / iterated headless.
	// Same fixture pattern as MC2_BOOT_INFO / MC2_BOOT_OBJ above. Registered
	// Trace, default unset -> byte-identical.
	// UI-DEFERRAL-CLOSURE-1: MC2_BOOT_TIMER_SECS=S (registered Trace) overrides
	// the remaining-seconds value so the <5-minute red warning (minutes<5) and
	// <2-minute blink (minutes<2, seconds%2) states of the SHARED legacy
	// computation (ControlGui::render, the color/blink block above the
	// renderTimerImGui fork) are reachable headless: e.g. S=283 lands the
	// capture in the red band, S=110 in the blink band. Default 754 s keeps
	// the original far-above-warning readout.
	{
		static const long s_bootTimer = static_cast<long>(mc2::gate::get<"MC2_BOOT_TIMER">());
		// 754 s -> "012 : 34"-ish readout; far above the 5-minute red /
		// 2-minute blink bands and never expires inside a 30 s capture run.
		// The row reads 754 for an empty, non-numeric or non-positive value.
		static const float s_bootTimerSecs = mc2::gate::get<"MC2_BOOT_TIMER_SECS">();
		if ( s_bootTimer > 0 && mission )
		{
			extern uint32_t g_mc2FrameCounter;
			static bool s_timerDone = false;
			if ( !s_timerDone && (long)g_mc2FrameCounter >= s_bootTimer )
			{
				s_timerDone = true;
				mission->m_timeLimit = scenarioTime + s_bootTimerSecs;
				mc2::fmt::print_out("[BOOT_TIMER] m_timeLimit={:f} secs={:f} (frame {:d})\n", mission->m_timeLimit, s_bootTimerSecs, mc2::fmt::as_u32((unsigned)g_mc2FrameCounter));
				fflush(stdout);
			}
		}
	}

	if (moviePlaying && bMovie)
	{
		ZoneScopedN("CGui.Movie");
		bool result = bMovie->update();
		if (result)
		{
			moviePlaying = false;
			delete bMovie;
			bMovie = nullptr;
		}
	}

	//Just update the movie.  DO not update anything else if in movieMode!!
	if (eye && eye->inMovieMode)
		return;

	if ( idToUnPress != -1 )
	{
		getButton( idToUnPress )->press( false );
	}

	// HUD command buttons draw shrunk by s_hud_scale -> hit-test in HUD-inverse
	// space so a click on the drawn-inward button maps to its authored rect.
	// (getMouseHudX/Y is a no-op when the HUD scale is off / outside the band.)
	float mouseX = userInput->getMouseHudX();
	float mouseY = userInput->getMouseHudY();

	
	// also going to initialize buttons here
	{ ZoneScopedN("CGui.ButtonHover"); CgScope _c(CG_BTNHOVER);
	for ( int i = LAST_COMMAND - 1; i > -1; i-- )
	{
		if ( buttons[i].location[0].x <= mouseX && mouseX <= buttons[i].location[2].x
				&& mouseY >= buttons[i].location[0].y && mouseY <= buttons[i].location[1].y  )
		{
				
			if ( buttons[i].state != ControlButton::HIDDEN )
			{
				helpTextHeaderID = buttonData[i].helpTextHeader;
				helpTextID = buttonData[i].helpTextID;
				bMouseInButton = true;
				long lastX = mouseX - userInput->getMouseXDelta();
				long lastY = mouseY - userInput->getMouseYDelta();

				if ( buttons[i].location[0].x > lastX || lastX > buttons[i].location[2].x
				|| lastY < buttons[i].location[0].y || lastY > buttons[i].location[1].y )
				{
					soundSystem->playDigitalSample( LOG_HIGHLIGHTBUTTONS );
				}
			}
			
			if ( userInput->leftMouseReleased() && !userInput->isLeftDrag()
				&& !userInput->wasLeftDrag() )
			{
				{
					handleClick( buttons[i].ID );
				}
			}
		}
	}
	} // CGui.ButtonHover

	if ((buttonToPress != -1) && getButton(buttonToPress))
	{
		getButton(buttonToPress)->press(true);
		buttonToPress = -1;
	}
	
	// update all the attack ranges
	Team* pTeam = Team::home;
	long bMover = 0;
	bool bJump = 1;

	// if no mover is selected, disable range buttons
	for (int i = DEFAULT_RANGE; i < STOP_COMMAND; i++ )
	{
		buttons[i].disable( false );
	}

	bool bMineLayer = 1;
	bool bSalvage = 1;
	bool bRepair = 1;
	bool bGuardTower = 1;
	bool bPressGuardTower = 1;

	Mover* pSelectedMover = nullptr;
	int holdPositionCount = 0;

	{ ZoneScopedN("CGui.RosterScan"); CgScope _c(CG_ROSTER);
	for (int i = 0; i < pTeam->getRosterSize(); ++i )
	{
		Mover* pMover = (Mover*)pTeam->getMover( i );
		
		if ( pMover->isSelected() && pMover->getCommander()->getId() == Commander::home->getId() )
		{
		//	if ( !isSelectingebject() )
			{
				bMineLayer &= ( (pMover->isMineLayer()) ? 1 : 0 );
				bSalvage &=  ( pMover->isRecover() ? 1 : 0 );
				bRepair &=  ( pMover->isRefit() && pMover->getRefitPoints() ? 1 : 0 );
				bGuardTower &= ( pMover->maxMoveSpeed ? 0 : 1 );
				if ( bGuardTower )
					bPressGuardTower &= pMover->suppressionFire;
				
				if ( pMover->getPilot()->getNumTacOrdersQueued ()  ) 
					getButton( STOP_COMMAND )->disable( false );
				switch( pMover->attackRange )
				{
				case FIRERANGE_SHORT:
					getButton( SHORT_RANGE )->press( true );
					break;
				case FIRERANGE_LONG:
					getButton( LONG_RANGE )->press( true );
					break;
				case FIRERANGE_MEDIUM:
					getButton( MED_RANGE )->press( true );
					break;
				case FIRERANGE_OPTIMAL:
					getButton( DEFAULT_RANGE )->press( true );
					break;
				case FIRERANGE_CURRENT:
					holdPositionCount++;
					break;
				}

				bJump &= pMover->canJump();
				pSelectedMover = pMover;
				bMover ++;

			}

		}
	}
	} // CGui.RosterScan

	{ ZoneScopedN("CGui.MoverState"); CgScope _c(CG_MOVERSTATE);

	if ( !holdPositionCount )
	{
		getButton( CUR_RANGE )->press( 0 );
	}
	else if ( holdPositionCount != bMover )
	{
		getButton( CUR_RANGE )->makeAmbiguous( true );
	}
	else 
		getButton( CUR_RANGE )->press( 1 );
		

	int rangeCount = 0;
	for (int i = 0; i < 4; i++ )
	{
		if ( getButton( DEFAULT_RANGE + i )->state & ControlButton::PRESSED )
		{
			rangeCount ++;
		}
	}

	if ( rangeCount > 1 )
	{
		for (int i = 0; i < 4; i++ )
		{
			if ( getButton( DEFAULT_RANGE + i )->state & ControlButton::PRESSED )
			{
				getButton( DEFAULT_RANGE + i )->makeAmbiguous( true );
			}
		}
	}


	// if no mover is selected, disable range buttons
	if ( !bMover )
	{
		for (int i = DEFAULT_RANGE; i < STOP_COMMAND; i++ )
			buttons[i].disable( true );
	}
	else
	{
		getButton( STOP_COMMAND )->disable( false );
	
	}

	
	if ( bMover && bMineLayer )
	{
		if ( !bMouseInButton )
			setRolloverHelpText( IDS_MINELAYER_HELP );
		getButton( LAYMINES )->disable( 0 );
		getButton( SALVAGE )->hide( 1 );
		getButton( REPAIR )->hide( 1 );
		getButton( GUARDTOWER )->hide( 1 );
	}	
	else if ( bMover && bSalvage )
	{
		if ( !bMouseInButton )
			setRolloverHelpText( IDS_RECOVER_HELP );
		getButton( SALVAGE )->disable( 0 );
		getButton( SALVAGE )->press( 1 );
		getButton( LAYMINES )->hide( 1 );
		getButton( REPAIR )->hide( 1 );
		getButton( GUARDTOWER )->hide( 1 );
		wasLayingMines = 0;
	}
	else if ( bMover && bRepair )
	{
		if ( !bMouseInButton )
			setRolloverHelpText( IDS_REPAIR_HELP );
		getButton( REPAIR )->disable( 0 );
		getButton( REPAIR )->press( 1 );
		getButton( LAYMINES )->hide( 1 );
		getButton( SALVAGE )->hide( 1 );
		getButton( GUARDTOWER )->hide( 1 );
		wasLayingMines = 0;
	}
	else if ( bMover && bGuardTower )
	{
		getButton( REPAIR )->hide( 1 );
		getButton( LAYMINES )->hide( 1 );
		getButton( SALVAGE )->hide( 1 );
		getButton( GUARDTOWER )->hide( 0 );
		if ( bPressGuardTower )
			getButton( GUARDTOWER )->press( 1 );
		else
			getButton( GUARDTOWER )->press ( 0 );

	}
	else
	{
		getButton( LAYMINES )->hide( 1 );
		getButton( REPAIR )->hide( 1 );
		getButton( SALVAGE )->hide( 1 );
		getButton( GUARDTOWER )->hide( 1 );
		wasLayingMines = 0;
	}
	
	if ( bMover && bJump )
		getButton( JUMP_COMMAND )->disable( false );
	else
		getButton( JUMP_COMMAND )->disable( true );

	// upate all the orders	
	if ( curOrder != WALK && bMover && !isSelectingInfoObject())
	{
		if ( curOrder == JUMP )
			getButton( JUMP_COMMAND )->press( true );
		else if ( curOrder == GUARD )
			getButton( GUARD_COMMAND )->press( true );
		else if ( curOrder == RUN )
			getButton( RUN_COMMAND )->press( true );
	}

	if ( fireFromCurrentPos && bMover )
		getButton( FIRE_FROM_CURRENT_POS )->press( true );
	else
		getButton( FIRE_FROM_CURRENT_POS )->press( false );

	} // CGui.MoverState

	bool bMouseInsideTacArea = 0;

	// rectInfos[0].rect is authored at 100% HUD coords but the tac area is drawn
	// shrunk by s_hud_scale; hit-test in HUD-inverse space (matches GameTacMap
	// and the force-group bar). Raw getMouseX/Y here made this rect bleed upward.
	if ( rectInfos[0].rect.left <= userInput->getMouseHudX()
		&& rectInfos[0].rect.right >= userInput->getMouseHudX()
		&& rectInfos[0].rect.top <= userInput->getMouseHudY()
		&& rectInfos[0].rect.bottom >= userInput->getMouseHudY() )
	{
		bMouseInsideTacArea = true;
	}

	// slice C site 3 (per-tab update + help text): registry dispatch under the
	// gate (active tab's update fn + registered IDS_* help pair); legacy
	// pressed-bit chain otherwise.
	if ( hudTabsGateOn() && hudTabRegistry.activeTab() )
	{
		ZoneScopedN("CGui.HudTab");
		const HudTabs::Tab* activeT = hudTabRegistry.activeTab();
		if ( activeT->update )
			activeT->update( this );
		if ( bMouseInsideTacArea && activeT->helpTextHeaderID )
		{
			helpTextHeaderID = activeT->helpTextHeaderID;
			helpTextID = activeT->helpTextID;
		}
	}
	else if ( getButton( TACMAP_TAB )->state & ControlButton::PRESSED )
	{
		ZoneScopedN("CGui.TacMap"); CgScope _c(CG_TACMAP);
		tacMap.update();
		if ( bMouseInsideTacArea )
		{
			helpTextHeaderID = IDS_TACMAP;
			helpTextID = IDS_TACMAP_DESC;
		}
	}
	else if ( getButton( INFO_TAB )->state & ControlButton::PRESSED )
	{
		ZoneScopedN("CGui.InfoWnd"); CgScope _c(CG_INFOWND);
		infoWnd->update();
		if ( bMouseInsideTacArea )
		{
			helpTextHeaderID = IDS_INFOTAB;
			helpTextID = IDS_INFOTAB_DESC;
		}
	}
	else if ( bMouseInsideTacArea )
	{
		helpTextHeaderID = IDS_VEHICLE_TAB;
		helpTextID = IDS_VEHICLE_TAB_DESC;
	}

	{ ZoneScopedN("CGui.VehicleTab"); CgScope _c(CG_VEHTAB); updateVehicleTab( mouseX, mouseY, bLOS ); }



	if ( renderObjectives )
		getButton( OBJECTIVES_COMMAND )->press( true );
	else
		getButton( OBJECTIVES_COMMAND )->press( false );

	{ ZoneScopedN("CGui.ForceGroupBar"); CgScope _c(CG_FGBAR); forceGroupBar.update(); }

	getButton( DEFAULT_RANGE )->hide(true);
	getButton( SHORT_RANGE )->hide(true);
	getButton( MED_RANGE )->hide(true);
	getButton( LONG_RANGE )->hide(true);

}

void ControlGui::handleClick( int ID )
{
	if ( getButton( ID )->state & ControlButton::HIDDEN )
		return;

	if ( !getButton( ID )->isEnabled() )
	{
		// need to play sound here
		soundSystem->playDigitalSample(INVALID_GUI);
		return;
	}


	int i;
	int sound = LOG_SELECT;
	
	switch( ID )
	{

	case SAVE_COMMAND:
		mission->missionInterface->togglePause();
		break;
	case SHORT_RANGE:
		setRange( FIRERANGE_SHORT );
		idToUnPress = ID;
		getButton( ID )->press( true );
		break;
	case MED_RANGE:
		setRange( FIRERANGE_MEDIUM );
		idToUnPress = ID;
		getButton( ID )->press( true );
		break;
	case LONG_RANGE:
		setRange( FIRERANGE_LONG );
		idToUnPress = ID;
		getButton( ID )->press( true );
		break;
	case CUR_RANGE:
		toggleHoldPosition();
		break;
	case DEFAULT_RANGE:
		setRange( FIRERANGE_OPTIMAL );
		idToUnPress = ID;
		getButton( ID )->press( true );
		break;
	case JUMP_COMMAND:
		toggleJump();
		break;
	case RUN_COMMAND:
		toggleDefaultSpeed();
		break;
	case GUARD_COMMAND:
		toggleGuard();
		break;
	case STOP_COMMAND:
		idToUnPress = ID;
		getButton( ID )->press( true );
		doStop();
		break;
	case FIRE_FROM_CURRENT_POS:
		toggleFireFromCurrentPos();
		break;
	case TACMAP_TAB:
	case INFO_TAB:
	case VEHICLE_TAB:
		// slice C site 5 (click switcher): registry mutual-exclusion under the
		// gate (setActiveHudTab presses/unpresses the same buttons the legacy
		// +3 loop did, plus logs the dispatch); legacy loop otherwise.
		if ( hudTabsGateOn() && hudTabRegistry.indexOfButton( ID ) >= 0 )
		{
			setActiveHudTab( hudTabRegistry.indexOfButton( ID ), "click" );
		}
		else
		{
			for ( i = TACMAP_TAB; i < TACMAP_TAB + 3; i ++ )
			{
				getButton( i )->press( false );
			}
			getButton( ID )->press( true );
		}

		if ( ID == VEHICLE_TAB )
			LogisticsData::instance->rpJustAdded = 0;
		break;
	case OBJECTIVES_COMMAND:
		if ( !renderObjectives )
		{
			startObjectives( 1 );
			getButton( ID )->press( true );
		}
		else
			renderObjectives = 0;

		sound = LOG_CLICKONBUTTON;
		break;

	case LAYMINES:
	case REPAIR:
	case SALVAGE:
		getButton( ID )->toggle();
		break;
	case GUARDTOWER:
	{
		getButton( ID )->toggle();
		bool bPress = getButton( ID )->state == ControlButton::PRESSED ? 1 : 0;
		for (long i = 0; i < Team::home->getRosterSize(); i++)
		{
			Mover* pMover = (Mover*)Team::home->getMover( i );
			if ( pMover->isSelected() && pMover->isGuardTower() && pMover->getCommander()->getId() == Commander::home->getId())
			{
				pMover->suppressionFire = bPress;
				if (!bPress && MPlayer && !MPlayer->isServer())
				{
					//We send a TAC_ORDER_WAIT which will clear the suppression fire message.
					TacticalOrder tacOrder;
					tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_WAIT);

					MPlayer->sendPlayerOrder(&tacOrder, false, 1, &pMover);
				}
			}
		}
	}


		break;
	case INFO_COMMAND:
		for ( i = JUMP_COMMAND; i < INFO_COMMAND; i++ )
			{
				getButton( i )->press( false );
			}
		getButton( ID )->toggle();
		break;
	default:
		sound = -1;
		break;

	}

	soundSystem->playDigitalSample( sound );
}

void ControlGui::doStop()
{
	TacticalOrder tacOrder;
	tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_STOP );
	soundSystem->playDigitalSample(BUTTON5);

	Team* pTeam = Team::home;
	
	for (long i = 0; i < pTeam->getRosterSize(); i++)
	{
		Mover* pMover = (Mover*)pTeam->getMover( i );
		if ( pMover->isSelected() && pMover->getCommander()->getId() == Commander::home->getId() )
		{
			tacOrder.attackParams.range = (FireRangeType)pMover->attackRange;
			tacOrder.pack(nullptr, nullptr);

			//---------------------------------------------------------------------
			// Helper function--perhaps this should just be a part of the mover and
			// group handleTacticalOrder() functions?
			if (MPlayer && !MPlayer->isServer())
				MPlayer->sendPlayerOrder(&tacOrder, false, 1, &pMover);
			else
				pMover->handleTacticalOrder(tacOrder);
		}
	}

	if ( getMines() )
		getButton( LAYMINES )->press( 0 );

	if ( addingArtillery || addingVehicle || addingSalvage )
	{
		handleVehicleClick( STOP_VEHICLE );
		LogisticsData::instance->rpJustAdded = 0;
	}

	if ( getFireFromCurrentPos() )
		toggleFireFromCurrentPos();
		
	setDefaultSpeed();
}

void ControlGui::toggleFireFromCurrentPos()
{
	fireFromCurrentPos = !fireFromCurrentPos;
}
