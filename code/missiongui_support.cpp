#include "vtol_simulation.h"
// MissionInterfaceManager: air strikes, reinforcements, VTOL calls, salvage, repair, pause API, nav markers and sensor strikes.
// Declared in the class headers; shared names in missiongui_shared_internal.h.
#include "missiongui_shared_internal.h"
#include "mc2fmt.h"
#include "mech3d.h"
#include "brain_execution_policy.h"
#include <utility>
#include <string_view>
#include "tick_commit.h"
#include "diagnostic_trace.h"

extern void preloadSupportRadioArchive(std::string_view name);
extern void preloadSupportSample(unsigned id);
namespace {
// These actors are never submitted or simulated. Their sources and types stay
// live until the mission interface releases them, before the appearance registry.
std::vector<std::unique_ptr<Appearance>> supportPreloads;
int supportPreloadStep = -1;
std::chrono::steady_clock::time_point supportPreloadStart;
double supportPreloadMs = 0;
double supportPreloadWorstMs = 0;
}

void releaseMissionSupportPreloads()
{
	supportPreloads.clear();
	supportPreloadStep = -1;
	supportPreloadMs = supportPreloadWorstMs = 0;
}

static void runSupportPreloadStep()
{
	if (supportPreloadStep >= 20) return;
	ZoneScopedN("Mission.SupportPreloadStep");
	const auto start = std::chrono::steady_clock::now();
	auto* logistics = LogisticsData::instance;
	// Match the mission permissions used by ControlGui, not temporary RP/unit
	// limits. A missing logistics mission permits the whole palette there too.
	const bool allowed[MAX_SUPPORT_SAMPLES] = {
		!logistics || logistics->canHaveAirStrike(),
		!logistics || logistics->canHaveMineLayer(),
		!logistics || logistics->canHaveSensorStrike(),
		!logistics || logistics->canHaveSalavageCraft(),
		!logistics || logistics->canHaveRepairTruck(),
		!logistics || logistics->canHaveScoutCopter(),
		!logistics || logistics->canHaveArtilleryPiece()
	};
	const int step = supportPreloadStep++;
	const auto preloadStep = [&] {
		if (step < MAX_SUPPORT_SAMPLES) {
			if (allowed[step]) preloadSupportSample(step);
		}
		else if (step < 15) {
			const int vehicle = (step - 7) / 2;
			const long ids[] = {182L, 393L, 120L, 415L};
			const int samples[] = {SUPPORT_REPAIR, SUPPORT_SCOUT, SUPPORT_MINELAYER, SUPPORT_ARTILLERY};
			const long id = ids[vehicle];
			if (!allowed[samples[vehicle]]) return;
			if ((step - 7) % 2 == 0) {
				const char* pilot = MissionInterfaceManager::instance()->getSupportVehicleNameFromID(id);
				if (!pilot) return;
				FullPathFileName path;
				path.init(warriorPath, pilot, ".fit");
				FitIniFile profile;
				char audio[256]{};
				if (profile.open(path) == NO_ERR && profile.seekBlock("General") == NO_ERR &&
					profile.readIdString("pilotAudio", audio, sizeof(audio) - 1) == NO_ERR && audio[0])
					preloadSupportRadioArchive(audio);
				return;
			}
			auto* type = ObjectManager->getObjectType(id);
			if (!type || !type->getAppearanceTypeName()) return;
			const auto cls = type->getObjectTypeClass();
			if (cls != VEHICLE_TYPE && cls != BATTLEMECH_TYPE) return;
			auto* appearanceType = appearanceTypeList->getAppearance(
				(cls == VEHICLE_TYPE ? GV_TYPE : MECH_TYPE) << 24, type->getAppearanceTypeName());
			if (!appearanceType) return;
			std::unique_ptr<Appearance> appearance;
			if (cls == VEHICLE_TYPE) appearance = std::make_unique<GVAppearance>();
			else appearance = std::make_unique<Mech3DAppearance>();
			appearance->init(appearanceType);
			appearance->resetPaintScheme(prefs.highlightColor, prefs.highlightColor, prefs.baseColor);
			supportPreloads.push_back(std::move(appearance));
		}
		else if (step < 19) {
			const bool needed[] = {
				allowed[SUPPORT_REPAIR] || allowed[SUPPORT_SCOUT] || allowed[SUPPORT_MINELAYER] || allowed[SUPPORT_ARTILLERY],
				allowed[SUPPORT_RECOVER], allowed[SUPPORT_AIRSTRIKE], allowed[SUPPORT_PROBE]
			};
			const char* names[] = {"vtol", "karnov", "airstrikemarker", "sensormarker"};
			const char* name = names[step - 15];
			if (!needed[step - 15]) return;
			auto* type = appearanceTypeList->getAppearance(BLDG_TYPE << 24, name);
			if (!type) return;
			auto appearance = std::make_unique<BldgAppearance>();
			appearance->init(type);
			supportPreloads.push_back(std::move(appearance));
		}
		else if (allowed[SUPPORT_AIRSTRIKE]) {
			auto* type = appearanceTypeList->getAppearance(GV_TYPE << 24, "Shilone");
			if (type) {
				auto appearance = std::make_unique<GVAppearance>();
				appearance->init(type);
				supportPreloads.push_back(std::move(appearance));
			}
		}
	};
	preloadStep();
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	supportPreloadMs += ms;
	supportPreloadWorstMs = std::max(supportPreloadWorstMs, ms);
	if (supportPreloadStep == 20)
		mc2::fmt::print_out("[SUPPORT_PRELOAD] appearances={} ms={:.3f} worst_step_ms={:.3f} playable_ms={:.3f}\n",
			supportPreloads.size(), supportPreloadMs, supportPreloadWorstMs,
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - supportPreloadStart).count());
}

// Behind the loading screen: file/appearance registries are main-thread owned and the steps are IO-bound (one cold step was 501 ms inside
// gameplay), so the whole palette is warmed at the end of mission init, where a stall is invisible. Same permission gating as the steps use.
void preloadMissionSupportAtLoad()
{
	releaseMissionSupportPreloads();
	supportPreloadStep = 0;
	supportPreloadStart = std::chrono::steady_clock::now();
	while (supportPreloadStep < 20) runSupportPreloadStep();
}

// Fallback for a mission whose load path did not warm the palette: one step per later frame, after the first playable frame.
void updateMissionSupportPreloads()
{
	if (supportPreloadStep < 0) {
		supportPreloadStep = 0;
		supportPreloadStart = std::chrono::steady_clock::now();
		return;
	}
	runSupportPreloadStep();
}


// Dev commander purchases obey the same permissions, prices and delivery path as the UI.
bool MissionInterfaceManager::requestPlayerSupport(int command, glm::vec3 position, MoverPtr salvage)
{
    if (!mc2::gate::get<"MC2_PLAY_MISSION">() || !mc2::brainpolicy::techBrain()
        || MPlayer || !Commander::home || !Team::home || !LogisticsData::instance) return false;
    auto* logistics = LogisticsData::instance;
    const bool allowed = command == ControlGui::LARGE_AIRSTRIKE ? logistics->canHaveAirStrike()
        : command == ControlGui::GUARD_TOWER ? logistics->canHaveArtilleryPiece()
        : command == ControlGui::REPAIR_VEHICLE ? logistics->canHaveRepairTruck()
        : command == ControlGui::RECOVERY_TEAM ? logistics->canHaveSalavageCraft() : false;
    const long cost = controlGui.playerSupportCost(command);
    if (!allowed || cost < 0 || logistics->getResourcePoints() < cost
        || controlGui.isAddingVehicle() || controlGui.isAddingAirstrike() || controlGui.isAddingSalvage()) return false;
    const bool strike = command == ControlGui::LARGE_AIRSTRIKE;
    const long vehicle = command == ControlGui::REPAIR_VEHICLE ? 182
        : command == ControlGui::RECOVERY_TEAM ? 147 : 415;
    if (!Team::home->teamLineOfSight(position, 0.0f)) return false;
    if (!strike) {
        if (!controlGui.getVehicleNameFromID(vehicle)) return false;
        if (paintingVtol[Commander::home->getId()] || rosterIconCount() >= MAX_ICONS) return false;
        if (command == ControlGui::RECOVERY_TEAM) {
            if (!salvage || !salvage->getExists() || !salvage->isDisabled() || salvage->isDestroyed()
                || salvage->getObjectClass() != BATTLEMECH || !logistics->gotPilotsLeft()) return false;
        } else if (!canAddVehicle(position)) return false;
    }
    position.z = land->getTerrainElevation(position);
    logistics->decrementResourcePoints(cost);
    if (strike) IfaceCallStrike(ARTILLERY_LARGE, &position, nullptr);
    else beginVtol(vehicle, Commander::home->getId(), &position, salvage);
    mc2::fmt::print_err("[PLAYER_SUPPORT v1] time={:.2f} command={} target={} cost={} rp={} x={:.1f} y={:.1f}\n",
        scenarioTime, command, salvage ? salvage->getWatchID() : 0, cost,
        logistics->getResourcePoints(), position.x, position.y);
    return true;
}

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
	ZoneScopedN("Support.Reinforcement");

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
	data.teamID = Commander::commanders[commanderID]->team ? Commander::commanders[commanderID]->team->getId() : -1;
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
	ZoneScopedN("Support.CallIn");
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
		ZoneScopedN("Support.TransportInit");
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
					char* newPilotName = const_cast<char*>(LogisticsData::instance->getBestPilot( salvageTarget->tonnage ).transform([](auto name) { return name.get().c_str(); }).value_or(nullptr));
					if (!newPilotName)
					{
						// No pilot available: unwind the queued recovery (refund the RP, release the buttons).
						if (MPlayer->isServer())
						{
							glm::vec3 refundPos;
							MPlayer->sendReinforcement(10000, 0, "noname", commanderID, refundPos, 6);
						}
						if (commanderID == MPlayer->commanderID)
							controlGui.unPressAllVehicleButtons();
						return;
					}
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
			mechRecovered[commanderID] = false;
			mechRecoverSkipped[commanderID] = false;	//Need to know when mech is done so Karnov can fly away.
			
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
		ZoneScopedN("Support.Voice");
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
			newPos = mechToRecover[commanderID]->appearance->drawNodeNamePosition("cockpit");
	}
	
	{
		ZoneScopedN("Support.FlightAudio");
		if (vehicleID[commanderID] != 147)
			soundSystem->playDigitalSample(VTOL_ANIMATE,newPos);
		else
			soundSystem->playDigitalSample(SALVAGE_CRAFT,newPos);
	}
	
	vTol[commanderID]->setObjectParameters( newPos, 0, false, Team::home->id, 0);
	eye->update();
	// The tick advances its authoritative clip; presentation poses the new actor.
	vTol[commanderID]->setInView( true );
	vTol[commanderID]->setVisibility( 1, 1 );
	vTol[commanderID]->rotation = rotation;
	
	if (vehicleID[commanderID] != 147)
		setVtolSimulationGesture(*vTol[commanderID], 0);		//Start Animation at beginning
	else
		setVtolSimulationGesture(*vTol[commanderID], 1);		//Start Animation with Landing -- KARNOV

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
					
		gosFX::Effect::ExecuteInfo info(static_cast<Stuff::Time>(scenarioTime),&shapeOrigin,nullptr);
	
		dustCloud[commanderID]->SetLoopOff();
		dustCloud[commanderID]->SetExecuteOn();
		dustCloud[commanderID]->Start(&info);
	}
				
  	paintingVtol[commanderID] = 1;
	if (mc2_diag::tagRequested("SUPPORT_CRAFT"))
		mc2::fmt::print_err("[SUPPORT_CRAFT] event=spawn vehicle={} commander={} name={} x={} y={} z={} appearance={:016X} shape={:016X}\n",
			vehicleID[commanderID], commanderID, mc2::fmt::safe(vTol[commanderID]->appearType->name),
			newPos.x, newPos.y, newPos.z, mc2::fmt::as_ptr(static_cast<void*>(vTol[commanderID].get())),
			mc2::fmt::as_ptr(static_cast<void*>(vTol[commanderID]->bldgShape)));
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
		if ( pTmpMover->isSelected() && commanderIsHome(pTmpMover->getCommander()) )
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
			if ( pMover->isSelected() && pMover->needsRefit() && commanderIsHome(pMover->getCommander()) )
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
	if (mc2::tick::defer(mc2::tick::Queue::MoverUi, [this, on] { showObjectives(on); }))
		return 0;
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
	if ( gos_GetKeyStatus( (gosEnum_KeyIndex)(commands[std::to_underlying(ENERGY_WEAPON_INDEX)].key & 0x0000ffff) ) == KEY_HELD )
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
