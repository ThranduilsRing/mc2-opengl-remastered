// MissionInterfaceManager: air strikes, reinforcements, VTOL calls, salvage, repair, pause API, nav markers and sensor strikes.
// Declared in the class headers; shared names in missiongui_shared_internal.h.
#include "missiongui_shared_internal.h"
#include "mc2fmt.h"


void MissionInterfaceManager::addAirstrike()
{
	if ( controlGui.isButtonPressed( ControlGui::LARGE_AIRSTRIKE ) )
		bigAirStrike();
	else if ( controlGui.isButtonPressed( ControlGui::LARGE_AIRSTRIKE ) )
		smlAirStrike();
	else if ( controlGui.isButtonPressed( ControlGui::SENSOR_PROBE ) )
		snsAirStrike();
	else
		gosASSERT( false );

	controlGui.unPressAllVehicleButtons();
}


MoverPtr BringInReinforcement (long vehicleID, long rosterIndex, long commanderID, glm::vec3 pos, bool exists) {

	MoverInitData data;
	memset( &data, 0, sizeof( data ) );
	char* vehicleFile = const_cast<char*>(MissionInterfaceManager::instance()->getSupportVehicleNameFromID(vehicleID));
	if (!vehicleFile) {
		char s[1024];
		mc2::fmt::to_buf(s, "BringInReinforcement: null behicleFile ({:d})", mc2::fmt::as_i32(vehicleID));
		STOP((s));
	}
	strcpy( data.pilotFileName, vehicleFile );
	strcpy( data.brainFileName, "pbrain" );
	data.controlType = 2;
	data.controlDataType = 2;
	data.rosterIndex = rosterIndex;
	data.objNumber = vehicleID;
	data.position = pos;
	data.rotation = 0;
	data.teamID = Commander::commanders[commanderID]->team->getId();
	data.commanderID = commanderID;
	data.active = 1;
	data.exists = exists;
	data.capturable = 0;
	data.baseColor = prefs.baseColor;
	data.highlightColor1 = prefs.highlightColor;
	data.highlightColor2 = prefs.highlightColor;

	if ( MPlayer )
	{
		MC2Player* pInfo = MPlayer->getPlayerInfo( commanderID );
		if ( pInfo )
		{
			data.baseColor = MPlayer->colors[pInfo->baseColor[BASECOLOR_TEAM]];
			data.highlightColor1 = MPlayer->colors[pInfo->stripeColor];
			data.highlightColor2 = MPlayer->colors[pInfo->stripeColor];
		}
	}

	long moverId = mission->addMover( &data );
	
	if ((moverId != -1) && (moverId != 0))
	{
		//Start the helicopters landed!!
		MoverPtr newMover = static_cast<MoverPtr>(ObjectManager->get(moverId));
		if (!newMover)
			return nullptr;

		if (newMover->getMoveType() == MOVETYPE_AIR)
			newMover->startShutDown();

		if (!MPlayer)
			newMover->setLocalMoverId(0);

		//Somehow this is set during a quickLoad?
		newMover->disableThisFrame = false;

		return(newMover);
	}

	return nullptr;
}


void MissionInterfaceManager::addVehicle( const glm::vec3& pos )
{
	


	// Check if vehicleFile is NULL.  If it is, simply get it.
	// Can happen if we get here BEFORE beginVtol?
	// Happens when we select a vehicle which costs exactly the number of points we have left.
	// Heidi deletes the points, which shuts off the button, which causes getVehicleName to return NULL.
	// D'OH!!!
	// Delete the points in getVehicleName when the vehicle is actually added!!!
	//
	if (!vehicleFile)
		vehicleFile = controlGui.getVehicleName(vehicleID[Commander::home->getId()]);

	if ( !vehicleFile )
		return;

	reinforcement = BringInReinforcement(vehicleID[Commander::home->getId()], 255, Commander::home->getId(), pos, true);
}

void MissionInterfaceManager::beginVtol (long supportID, long commanderID, glm::vec3* reinforcePos, MoverPtr salvageTarget)
{
	glm::vec3 vtolPos = wPos;
	if (reinforcePos)
		vtolPos = *reinforcePos;

	if (supportID == -1) {
		vehicleFile = controlGui.getVehicleName( vehicleID[commanderID] );
		}
	else 
	{
		//-----------------------
		// used in multiplayer...
		vehicleID[commanderID] = supportID;
		vehicleFile = controlGui.getVehicleNameFromID(supportID);
	}

	if (vehicleID[commanderID] == 0) {
		char s[1024];
		mc2::fmt::to_buf(s, "beginvtol: supportID = {:d}, commanderID = {:d}, vehicleID = {:d}, vehicleFile = {}", mc2::fmt::as_i32(supportID), mc2::fmt::as_i32(commanderID), mc2::fmt::as_i32(vehicleID[commanderID]), mc2::fmt::safe(vehicleFile));
		PAUSE((s));
	}

	if ( !vTol[commanderID] )
	{
		if (vehicleID[commanderID] != 147)		//NOT a recovery vehicle.  Standard VTOL
		{
			if (supportID == -1) 
			{
				//Do not call this unless the commanderID passed in equals us or will you do something outside here?
				if (MPlayer && !reinforcePos) 
				{
					vPos[commanderID] = vtolPos;
					MPlayer->sendReinforcement(vehicleID[commanderID], 255, "noone", commanderID, vPos[commanderID], 0);
					return;
				}
			}
			
			AppearanceType* appearanceType = appearanceTypeList->getAppearance( BLDG_TYPE << 24, "vtol" );
			vTol[commanderID] = std::make_unique<BldgAppearance>();
			vTol[commanderID]->init( appearanceType );
			
			if (!dustCloud[commanderID] && prefs.useNonWeaponEffects)
			{
				if (strcmp(weaponEffects->GetEffectName(VTOL_DUST_CLOUD),"NONE") != 0)
				{
					unsigned flags = 0;
		
					Check_Object(gosFX::EffectLibrary::Instance);
					gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(VTOL_DUST_CLOUD));
					
					if (gosEffectSpec)
					{
						dustCloud[commanderID] = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
						gosASSERT(dustCloud[commanderID] != nullptr);
						
						MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
					}
				}
			}
		}
		else						//IS a recovery vehicle.  DO NOT create VTOL, use KARNOV instead.  NO Vehicle created either
		{
			if (supportID == -1) 
			{
				if (MPlayer && !reinforcePos) 	//Probably same as above here, too.
				{
					vPos[commanderID] = vtolPos;
					if (!salvageTarget)
						STOP(("MissionGUI.beginvtol: null target PASSED IN."));
					char* newPilotName = const_cast<char*>(LogisticsData::instance->getBestPilot( salvageTarget->tonnage ));
					if (vehicleID[commanderID] != 147)
						STOP(("missiongui.beginvtol: bad vehicleID"));
					MPlayer->sendReinforcement(vehicleID[commanderID], salvageTarget->netRosterIndex, newPilotName, commanderID, vPos[commanderID], 3);
					return;
				}
			}

			AppearanceType* appearanceType = appearanceTypeList->getAppearance( BLDG_TYPE << 24, "karnov" );
			vTol[commanderID] = std::make_unique<BldgAppearance>();
			vTol[commanderID]->init( appearanceType );
			
			if (MPlayer)
				mechToRecover[commanderID] = MPlayer->moverRoster[MPlayer->reinforcements[commanderID][1]];
			else
				mechToRecover[commanderID] = salvageTarget;
			mechRecovered[commanderID] = false;	//Need to know when mech is done so Karnov can fly away.
			
			if (!dustCloud[commanderID] && prefs.useNonWeaponEffects)
			{
				if (strcmp(weaponEffects->GetEffectName(KARNOV_DUST_CLOUD),"NONE") != 0)
				{
					unsigned flags = 0;
		
					Check_Object(gosFX::EffectLibrary::Instance);
					gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(KARNOV_DUST_CLOUD));
					
					if (gosEffectSpec)
					{
						dustCloud[commanderID] = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
						gosASSERT(dustCloud[commanderID] != nullptr);
						
						MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
					}
				}
			}
			
			if (!recoveryBeam[commanderID])
			{
				if (strcmp(weaponEffects->GetEffectName(KARNOV_RECOVERY_BEAM),"NONE") != 0)
				{
					unsigned flags = 0;
		
					Check_Object(gosFX::EffectLibrary::Instance);
					gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(KARNOV_RECOVERY_BEAM));
					
					if (gosEffectSpec)
					{
						recoveryBeam[commanderID] = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
						gosASSERT(recoveryBeam[commanderID] != nullptr);
						
						MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
					}
				}
			}
		}
	}

	if (commanderID == Commander::home->getId())
	{
		switch (vehicleID[commanderID])
		{
			case 120:	//Minelayer
				soundSystem->playSupportSample(SUPPORT_MINELAYER);
			break;

			case 182:	//Repair
				soundSystem->playSupportSample(SUPPORT_REPAIR);
			break;

			case 393:	//Scout
				soundSystem->playSupportSample(SUPPORT_SCOUT);
			break;

			case 147:	//Recovery
				soundSystem->playSupportSample(SUPPORT_RECOVER);
			break;

			case 415:	//Artillery
				soundSystem->playSupportSample(SUPPORT_ARTILLERY);

			break;
		}
	}

	glm::vec3 newPos = vtolPos;
	float rotation = 0.f;  // this needs to be pointing 180 degrees from drop point
	
	if (vehicleID[commanderID] == 147)
	{
		if (mechToRecover[commanderID] && mechToRecover[commanderID]->appearance)
			newPos = mechToRecover[commanderID]->appearance->getNodeNamePosition("cockpit");
	}
	
	if (vehicleID[commanderID] != 147)
		soundSystem->playDigitalSample(VTOL_ANIMATE,newPos);
	else
		soundSystem->playDigitalSample(SALVAGE_CRAFT,newPos);
	
	vTol[commanderID]->setObjectParameters( newPos, 0, false, Team::home->id, 0);
	eye->update();
	vTol[commanderID]->update();
	vTol[commanderID]->setInView( true );
	vTol[commanderID]->setVisibility( 1, 1 );
	vTol[commanderID]->rotation = rotation;
	
	if (vehicleID[commanderID] != 147)
		vTol[commanderID]->setGesture(0);		//Start Animation at beginning
	else
		vTol[commanderID]->setGesture(1);		//Start Animation with Landing -- KARNOV

	if (dustCloud[commanderID])
	{
		glm::mat4 	shapeOrigin;
		glm::mat4	localToWorld;
		glm::mat4	localResult;
					
		glm::vec3 dustPos = vTol[commanderID]->position;
		glm::vec3 wakePos;
		mc2ToRender(wakePos, dustPos);
			
		mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
		mc2::math::buildTranslation(shapeOrigin, wakePos);
					
		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);
	
		dustCloud[commanderID]->SetLoopOff();
		dustCloud[commanderID]->SetExecuteOn();
		dustCloud[commanderID]->Start(&info);
	}
				
  	paintingVtol[commanderID] = 1;
	vTolSpeed = -75.f;

	vPos[commanderID] = vtolPos;

	vehicleDropped[commanderID] = false;
	
	if ( !MPlayer || commanderID == MPlayer->commanderID )
		controlGui.disableAllVehicleButtons();
}

bool MissionInterfaceManager::canSalvage( GameObject* pMover )
{
	bool lineOfSight = Team::home->teamLineOfSight(pMover->getPosition(),0.0f);

	return (controlGui.forceGroupBar.getIconCount() < MAX_ICONS
		&& target->isDisabled() && lineOfSight && 
		target->getObjectClass() == BATTLEMECH ) ? 1 : 0;
}

void MissionInterfaceManager::doSalvage()
{
	if ( canSalvage(target) )
	{
		TacticalOrder tacOrder;
	
		tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_RECOVER );
		tacOrder.targetWID = target->getWatchID();
		tacOrder.attackParams.type = ATTACK_NONE;
		tacOrder.attackParams.method = ATTACKMETHOD_RAMMING;
		tacOrder.attackParams.pursue = true;
		tacOrder.moveParams.wayPath.mode[0] = TRAVEL_MODE_FAST;

		handleOrders( tacOrder );
	}

}

bool MissionInterfaceManager::canRepair( GameObject* pMover )
{
	if ( !pMover )
		return 0;
	
	if ( !pMover->isMover() )
		return 0;

	if ( Team::home->isEnemy( pMover->getTeam() ) )
		return 0;

	if (pMover->isDisabled() || pMover->isDestroyed())
		return 0;

	int watchID = 0;
	Team* pTeam = Team::home;
	for (long i = 0; i < pTeam->getRosterSize(); i++)
	{
		Mover* pTmpMover = static_cast<Mover*>(pTeam->getMover( i ));
		if ( pTmpMover->isSelected() && pTmpMover->getCommander()->getId() == Commander::home->getId() )
		{
			if ( !watchID )
				watchID = pTmpMover->getWatchID();
			else
				watchID = -1;
		}
	}
	
	if  ( (static_cast<Mover*>(pMover))->needsRefit() || ( (static_cast<Mover*>(pMover))->refitBuddyWID == watchID && watchID) )
		return true;

	return 0;

}
void MissionInterfaceManager::doRepair(GameObject* who)
{
	TacticalOrder tacOrder;
	tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_REFIT, false);
	tacOrder.targetWID = who->getWatchID();
	tacOrder.selectionIndex = -1;
	tacOrder.moveParams.wayPath.mode[0] = controlGui.getWalk() ?  TRAVEL_MODE_SLOW : TRAVEL_MODE_FAST;
	tacOrder.moveParams.faceObject = true;
	tacOrder.moveParams.fromArea = -1;
	tacOrder.moveParams.wait = false;

	handleOrders( tacOrder );

	soundSystem->playDigitalSample(BUTTON5);
	controlGui.setDefaultSpeed();
}

bool MissionInterfaceManager::canRepairBay( GameObject* bay)
{
	if (bay && bay->getRefitPoints() > 0.0f)
	{
		Team* pTeam = Team::home;
		for (long i = 0; i < pTeam->getRosterSize(); i++)
		{
			Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
			if ( pMover->isSelected() && pMover->needsRefit() && pMover->getCommander()->getId() == Commander::home->getId() )
			{
				return true;
			}
		}
	}

	return false;
}
void MissionInterfaceManager::doRepairBay(GameObject* who)
{
	TacticalOrder tacOrder;
	tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_GETFIXED, false);
	tacOrder.targetWID = who->getWatchID();
	tacOrder.selectionIndex = -1;
	tacOrder.moveParams.wayPath.mode[0] = controlGui.getWalk() ?  TRAVEL_MODE_SLOW : TRAVEL_MODE_FAST;
	tacOrder.moveParams.faceObject = true;
	tacOrder.moveParams.fromArea = -1;
	tacOrder.moveParams.wait = false;

	handleOrders( tacOrder );

	soundSystem->playDigitalSample(BUTTON5);
	controlGui.setDefaultSpeed();
}

int MissionInterfaceManager::vehicleCommand()
{
	if ( controlGui.getVehicleCommand() )
		controlGui.setVehicleCommand( false );
	else
		controlGui.setVehicleCommand( true );

	return 0;
}

int MissionInterfaceManager::showObjectives()
{
	controlGui.startObjectives( !controlGui.objectivesStarted( ) );
	return 0;
}

int MissionInterfaceManager::showObjectives(bool on)
{
	controlGui.startObjectives( on );
	return 0;
}

int MissionInterfaceManager::togglePause()
{
	if ( !bPausedWithoutMenu )
	{
		bPaused ^= 1;
	}
	else
		bPausedWithoutMenu = 0;

	soundSystem->playDigitalSample( LOG_NEXTBACKBUTTONS );
		

	if ( bPaused )
	{
		controlGui.beginPause();
	}
	else
	{
		controlGui.endPause();
	}

	return 1;
}

int MissionInterfaceManager::togglePauseWithoutMenu()
{

	if ( !bPausedWithoutMenu )
	{
		if ( !bPaused )
		{
			bPausedWithoutMenu = 1;
			bPaused = 1;
			return 1;
		}
		else
		{
			return 0;
		}
	}
	else
	{
		gosASSERT(bPaused);
		bPausedWithoutMenu = 0;
		bPaused = 0;
		return 1;
	}
}

bool MissionInterfaceManager::isPaused()
{
	return bPaused;
}

// [VISUAL_CAPTURE v1.5] Freeze/unfreeze the sim for a deterministic capture
// sweep WITHOUT raising the pause menu or playing a sound. We mirror the
// menu-less pause flags directly: scenarioTime + unit/sensor/collision updates
// are gated on isPaused() (mission.cpp:531), so this is exactly the clock the
// capture needs frozen. We do NOT touch controlGui (no beginPause/endPause) so
// no HUD pause overlay is drawn into the captured frame. The pre-sweep flag
// pair is restored verbatim by the caller via getCapturePauseState(), so a
// continuing sim resumes exactly as before.
void MissionInterfaceManager::setPausedForCapture(bool paused)
{
	bPaused = paused ? 1 : 0;
	bPausedWithoutMenu = paused ? 1 : 0;
}

int MissionInterfaceManager::getCapturePauseState()
{
	return ((bPaused ? 1 : 0) << 1) | (bPausedWithoutMenu ? 1 : 0);
}

// Restore the exact (bPaused, bPausedWithoutMenu) pair captured before the
// sweep. Resumes a continuing sim exactly as it was, including the case where
// the player had paused without the menu before the capture fired.
void MissionInterfaceManager::restoreCapturePauseState(int packed)
{
	bPaused = (packed >> 1) & 1;
	bPausedWithoutMenu = packed & 1;
}

// [VISUAL_CAPTURE v1.5] extern-C shims so the GameOS-side capture TU
// (gos_visual_capture.cpp) can drive the mission pause flags WITHOUT including
// missiongui.h (which pulls in mclib.h/mover.h/controlgui.h, not on that TU's
// include path). Return -1 when no mission interface exists yet (e.g. in menus)
// so the capture site can no-op gracefully.
extern "C" int mc2VisualCaptureGetPauseState()
{
	MissionInterfaceManager* mim = MissionInterfaceManager::instance();
	return mim ? mim->getCapturePauseState() : -1;
}

extern "C" void mc2VisualCaptureSetPaused(int paused)
{
	MissionInterfaceManager* mim = MissionInterfaceManager::instance();
	if (mim) mim->setPausedForCapture(paused != 0);
}

extern "C" void mc2VisualCaptureRestorePauseState(int packed)
{
	MissionInterfaceManager* mim = MissionInterfaceManager::instance();
	if (mim) mim->restoreCapturePauseState(packed);
}

bool MissionInterfaceManager::isPausedWithoutMenu()
{
	return bPausedWithoutMenu;
}

void	MissionInterfaceManager::swapResolutions()
{
	controlGui.swapResolutions( Environment.screenWidth, Environment.screenHeight );
	resolution = Environment.screenWidth;
	keyboardRef->init();
}

int MissionInterfaceManager::switchTab()
{
	controlGui.switchTabs(1);		
	return 1;
}

int MissionInterfaceManager::reverseSwitchTab()
{
	controlGui.switchTabs(-1);		
	return 1;
}


int MissionInterfaceManager::infoCommand()
{
	if ( !controlGui.infoButtonPressed() )
		controlGui.pressInfoButton();
	return 1;
}

int MissionInterfaceManager::infoButtonReleased()
{
	if ( controlGui.infoButtonPressed() )
		controlGui.pressInfoButton();

	return 1;
}
int MissionInterfaceManager::energyWeapons()
{
	if ( gos_GetKeyStatus( (gosEnum_KeyIndex)(commands[ENERGY_WEAPON_INDEX].key & 0x0000ffff) ) == KEY_HELD )
		bEnergyWeapons = 1;	
	else
		bEnergyWeapons = 0;
	return 0;
}
int MissionInterfaceManager::sendAirstrike()
{
	controlGui.pressAirstrikeButton();
	return 1;
}
int MissionInterfaceManager::sendLargeAirstrike()
{
	controlGui.pressLargeAirstrikeButton();
	return 1;
}
int MissionInterfaceManager::gotoNextNavMarker()
{
	for ( CObjective* pObjective : Team::home->objectives )
		{
			if ( pObjective->Status(Team::home->objectives) == OS_UNDETERMINED  && pObjective->DisplayMarker() == NAV )
			{
				glm::vec3 pos;
				pos.x = pObjective->MarkerX();
				pos.y = pObjective->MarkerY();
				if ( !pos.x && !pos.y )
					continue;
				pos.z = land->getTerrainElevation( pos );
				LocationNode path;
				path.location = pos;
				path.run = true;
				path.next = nullptr;

				TacticalOrder tacOrder;
				tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_MOVETO_POINT, false);
				tacOrder.initWayPath(&path);
				tacOrder.moveParams.wait = false;
				tacOrder.moveParams.wayPath.mode[0] =  controlGui.getWalk() ? TRAVEL_MODE_SLOW : TRAVEL_MODE_FAST;
				if ( controlGui.getMines() )
					tacOrder.moveParams.mode = MOVE_MODE_MINELAYING;
				tacOrder.pack(nullptr, nullptr);


				handleOrders(tacOrder);
				
				soundSystem->playDigitalSample(BUTTON5);

				return 1;
			}
		}

	soundSystem->playDigitalSample( INVALID_GUI );
	return 0;

}
int MissionInterfaceManager::sendSensorStrike()
{
	controlGui.pressSensorStrikeButton();
	return 1;
}
