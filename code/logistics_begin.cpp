// Logistics: beginMission, DoBeginMission, initializeLogData, the full-screen video player and the results host-left dialog.
// Declared in the class headers; shared names in logistics_internal.h.
#include "logistics_internal.h"
#include "../GameOS/gameos/utils/mc2log.h"
#include <string_view>


//-----------------------------------------------------------------------------

extern unsigned int MultiPlayTeamId;
extern unsigned int MultiPlayCommanderId;

int _stdcall Logistics::beginMission(void*, int, void*[])
{

	if (MPlayer)
		MPlayer->setMode(MULTIPLAYER_MODE_LOADING);

	char commandersToLoad[MAX_MC_PLAYERS][3] = {{0, 0, 0}, {1, 1, 1}, {2, 2, 2}, {3, 3, 3}, {4, 4, 4}, {5, 5, 5}, {6, 6, 6}, {7, 7, 7}};
	long missionLoadType = LogisticsData::instance->skipLogistics() ? 
							MISSION_LOAD_SP_QUICKSTART : MISSION_LOAD_SP_LOGISTICS;
	if (MPlayer) {
		//---------------------------
		// Calc valid commanderIDs...
		long curCommanderID = 0;
		for (long CID = 0; CID < MAX_MC_PLAYERS; CID++) {
			MPlayer->availableCIDs[CID] = true;
			if (MPlayer->playerInfo[CID].player && (MPlayer->playerInfo[CID].commanderID > -1)) {
				if (CID != curCommanderID) {
					long oldCommanderID = CID;
					MPlayer->playerInfo[CID].commanderID = curCommanderID;
					memcpy(&MPlayer->playerInfo[curCommanderID], &MPlayer->playerInfo[CID], sizeof(MC2Player));
					MPlayer->playerInfo[CID].player = nullptr;
					MPlayer->playerInfo[CID].commanderID = -1;
					for (long j = 0; j < MAX_MC_PLAYERS; j++)
						if (MPlayer->playerList[j].player == MPlayer->playerInfo[curCommanderID].player)
							MPlayer->playerList[j].commanderID = curCommanderID;
					if (oldCommanderID == MPlayer->commanderID)
						MPlayer->commanderID = curCommanderID;
				}
				MPlayer->availableCIDs[curCommanderID] = false;
				curCommanderID++;;
			}
		}
		//----------------------
		// Calc valid teamIDs...
		long curTeamID = 0;
		for (long teamID = 0; teamID < MAX_MC_PLAYERS; teamID++) {
			bool teamFound = false;
			for (long i = 0; i < MAX_MC_PLAYERS; i++)
				if (MPlayer->playerInfo[i].player && (MPlayer->playerInfo[i].team == teamID)) {
					MPlayer->playerInfo[i].team = curTeamID;
					teamFound = true;
				}
			if (teamFound)
				curTeamID++;
		}
		if (MPlayer->isHost()) {
			// Determine drop zone order here...
			char dropZoneList[8];
			char hqs[MAX_TEAMS];
			if (MPlayer->missionSettings.missionType == MISSION_TYPE_OTHER) {
				bool goodToLoad = mission->calcComplexDropZones(const_cast<char*>(LogisticsData::instance->getCurrentMission().c_str()), dropZoneList);
				if (!goodToLoad)
					STOP(("Logisitics.beginMission: teams do not match up for complex mission"));
				for (long i = 0; i < MAX_TEAMS; i++)
					hqs[i] = i;
				}
			else
				MPlayer->calcDropZones(dropZoneList, hqs);
			if (MPlayer->missionSettings.quickStart)
				for (long i = 0; i < MAX_MC_PLAYERS; i++) {
					MPlayer->commandersToLoad[i][0] = static_cast<long>(dropZoneList[i]); //MPlayer->playerInfo[i].commanderID;
					MPlayer->commandersToLoad[i][1] = static_cast<long>((dropZoneList[i] > -1)) ? MPlayer->playerInfo[dropZoneList[i]].team : 0;
					MPlayer->commandersToLoad[i][2] = hqs[i];
				}
			else
				for (long i = 0; i < MAX_MC_PLAYERS; i++) {
					MPlayer->commandersToLoad[i][0] = dropZoneList[i]; //-1;
					MPlayer->commandersToLoad[i][1] = (dropZoneList[i] > -1) ? MPlayer->playerInfo[dropZoneList[i]].team : 0; //-1;
					MPlayer->commandersToLoad[i][2] = hqs[i];
				}
			MPlayer->sendMissionSetup(nullptr, 0, nullptr);
		}
		if (!MPlayer->waitTillStartLoading()) {
			// SERVER DROPPED
			return(0);
		}
		if (MPlayer->commandersToLoad[0][0] < -1)
			PAUSE(("Logistics.beginMission: bad commandersToLoad"));
		for (long i = 0; i < MAX_MC_PLAYERS; i++) {
			commandersToLoad[i][0] = MPlayer->commandersToLoad[i][0];
			commandersToLoad[i][1] = MPlayer->commandersToLoad[i][1];
			commandersToLoad[i][2] = MPlayer->commandersToLoad[i][2];
		}
		if (MPlayer->missionSettings.quickStart) {
			MultiPlayTeamId = MPlayer->playerInfo[MPlayer->commanderID].team;
			//if (MultiPlayTeamId < 0)
			if (MultiPlayTeamId == 0xffffffff)
				STOP(("Bad commanderID"));
			MultiPlayCommanderId = MPlayer->commanderID;
			//if (MultiPlayCommanderId < 0)
			if (MultiPlayCommanderId == 0xffffffff)
				STOP(("Bad commanderID"));
			missionLoadType = MISSION_LOAD_MP_QUICKSTART;
			}
		else {
			MultiPlayTeamId = MPlayer->playerInfo[MPlayer->commanderID].team;
			MultiPlayCommanderId = MPlayer->commanderID;
			missionLoadType = MISSION_LOAD_MP_LOGISTICS;
		}
		long maxTeam = -1;
		for (int i = 0; i < MAX_MC_PLAYERS; i++)
			if (MPlayer->playerInfo[i].team > maxTeam)
				maxTeam = MPlayer->playerInfo[i].team;
		MPlayer->numTeams = maxTeam + 1;
		}
	else if (missionLoadType == MISSION_LOAD_SP_LOGISTICS) {
		commandersToLoad[0][0] = -1;
		commandersToLoad[0][1] = -1;
		commandersToLoad[0][2] = -1;
	}
	else {
		commandersToLoad[0][0] = 0;
		commandersToLoad[0][1] = 0;
		commandersToLoad[0][2] = -1;
	}

	if (mission)
		mission->destroy();

	long numPlayers = 1;
	if ( MPlayer )
		MPlayer->getPlayers(numPlayers);
	long numMoversPerCommander[MAX_MC_PLAYERS] = {12, 12, 12, 9, 7, 6, 5, 4};
	glm::vec3 dropZoneList[255]; // ubsurdly large, but sometimes we overrun this.
	long dropZoneID = 0;
	if (MPlayer) {
		//dropZoneID = MPlayer->commanderID;
		for (long i = 0; i < MAX_MC_PLAYERS; i++)
			if (commandersToLoad[i][0] == MPlayer->commanderID) {
				dropZoneID = i;
				break;
			}
		useUnlimitedAmmo = MPlayer->missionSettings.unlimitedAmmo;
	}

	mission->init(const_cast<char*>(LogisticsData::instance->getCurrentMission().c_str()), missionLoadType, dropZoneID, dropZoneList, commandersToLoad, numMoversPerCommander[numPlayers - 1]);
	extern int g_lightProbeSetupPath; g_lightProbeSetupPath = 3; // [GPUPROPS v1] post mission->init

	LogisticsData::instance->rpJustAdded = 0;

	if (MPlayer) {
		if (missionLoadType == MISSION_LOAD_MP_LOGISTICS) {
			auto list = LogisticsData::instance->getForceGroup();

			long dropZoneIndex = 0;
			long numMechs = 0;
			for ( auto iter = list.begin(); iter != list.end(); ++iter ) {
				numMechs++;
				if ( !(*iter)->getPilot() )
					continue;
				CompressedMech mechData;
				mechData.lastMech = (std::ssize(list) == numMechs);
				mechData.objNumber =  (*iter)->getFitID();
				mechData.commanderID = MPlayer->commanderID;
				mechData.baseColor = MPlayer->colors[MPlayer->playerInfo[MPlayer->commanderID].baseColor[BASECOLOR_TEAM]];
				mechData.highlightColor1 = MPlayer->colors[MPlayer->playerInfo[MPlayer->commanderID].stripeColor];
				mechData.highlightColor2 = MPlayer->colors[MPlayer->playerInfo[MPlayer->commanderID].stripeColor];
				strcpy(mechData.pilotFile, (*iter)->getPilot()->getFileName().c_str());
				strcpy(mechData.mechFile, (*iter)->getFileName().c_str());
				strcpy(mechData.variantName, (*iter)->getName().c_str());
				mechData.variantNum = (*iter)->getVariant()->getFileID();
				mechData.cBills = (*iter)->getVariant()->getCost();
				mechData.pos[0] = dropZoneList[dropZoneIndex].x;
				mechData.pos[1] = dropZoneList[dropZoneIndex++].y;
				mechData.designerMech = (*iter)->getVariant()->isDesignerMech();
				mechData.numComponents = (*iter)->getComponentCount();
				if (mechData.numComponents)	{
					long* componentList = static_cast<long*>(std::malloc(sizeof(long) * mechData.numComponents));
					long otherCount = mechData.numComponents;
					(*iter)->getComponents(otherCount, componentList);
					if (otherCount != mechData.numComponents)
						STOP(("Heidi's getComponentCount does not agree with count returned from getComponents"));
					for (long i = 0; i < mechData.numComponents; i++)
						mechData.components[i] = static_cast<unsigned char>(componentList[i]);
				}
				MPlayer->sendMissionSetup(nullptr, 1, &mechData);
			}

			if (!MPlayer->waitTillMechDataReceived()) {
				// SERVER DROPPED
				mission->destroy();
				return(0);
			}
	
			ObjectManager->numMechs = 0;
			ObjectManager->numVehicles = 0;
			for (long i = 0; i < MAX_MC_PLAYERS; i++) {
				if (MPlayer->mechDataReceived[i]) {
					for (long j = 0; j < 12; j++) {
						if (MPlayer->mechData[i][j].objNumber > -1) {
							MoverInitData data;
							memset(&data, 0, sizeof(MoverInitData));
							data.objNumber = MPlayer->mechData[i][j].objNumber;
							data.rosterIndex = 255;
							data.controlType = 2;
							data.controlDataType = 1;
							data.position.x = MPlayer->mechData[i][j].pos[0];
							data.position.y = MPlayer->mechData[i][j].pos[1];
							data.position.z = 0.0;
							data.rotation = 0;
							data.teamID = MPlayer->playerInfo[MPlayer->mechData[i][j].commanderID].team;
							data.commanderID = MPlayer->mechData[i][j].commanderID;
							data.baseColor = MPlayer->mechData[i][j].baseColor;
							data.highlightColor1 = MPlayer->mechData[i][j].highlightColor1;
							data.highlightColor2 = MPlayer->mechData[i][j].highlightColor2;
							data.gestureID = 2;
							data.active = 1;
							data.exists = 1;
							data.capturable = 0;
							data.icon = 0;
							
							strcpy(data.pilotFileName, MPlayer->mechData[i][j].pilotFile);
							strcpy(data.brainFileName, "pbrain");
							strcpy(data.csvFileName, MPlayer->mechData[i][j].mechFile);
							data.numComponents = MPlayer->mechData[i][j].numComponents;
							for (long k = 0; k < MPlayer->mechData[i][j].numComponents; k++)
								data.components[k] = MPlayer->mechData[i][j].components[k];
							long moverHandle = mission->addMover(&data);
							if (moverHandle < 1)
								STOP(("Logistics.beginMission: unable to addMover"));
							MoverPtr mover = static_cast<MoverPtr>(ObjectManager->get(moverHandle));
							if (!mover)
								STOP(("Logistics.beginMission: NULL mover"));
							if (mover->getObjectClass() != BATTLEMECH)
								STOP(("Logistics.beginMission: not a mech"));
							(static_cast<BattleMech*>(mover))->cBills = MPlayer->mechData[i][j].cBills;
							strcpy((static_cast<BattleMech*>(mover))->variantName, MPlayer->mechData[i][j].variantName);
							data.variant = MPlayer->mechData[i][j].variantNum;
						}
					}
				}
			}
		}
		else // gotta update pilot availability
		{
			long count = 256;
			LogisticsPilot* pilots[256];
			LogisticsData::instance->getPilots( pilots, count );

			for ( int i = 0; i < count; i++ )
			{
				pilots[i]->setUsed(0);
			}

			Team* pTeam = Team::home;

			if ( pTeam )
			{
				for (int i = pTeam->getRosterSize() - 1; i > -1; i-- )
				{
					Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
					if ( pMover && commanderIsHome(pMover->getCommander()) )
					{
						LogisticsPilot* pPilot = LogisticsData::instance->getPilot( pMover->getPilot()->getName() );
						if ( pPilot )
							pPilot->setUsed( true );
					}
				}
			}
					

		}
		
		CompressedMech mechData;
		mechData.commanderID = MPlayer->commanderID;
		MPlayer->sendMissionSetup(nullptr, 2, &mechData);
		if (!MPlayer->waitTillMissionLoaded()) {
			// SERVER DROPPED
			mission->destroy();
			return(0);
		}

		}
	else if (missionLoadType == MISSION_LOAD_SP_LOGISTICS) {
		auto list = LogisticsData::instance->getForceGroup();

		float rotation = std::ssize(list) ? 360.f/std::ssize(list) : 0.f;

		MoverInitData data;
		memset( &data, 0, sizeof( data ) );
		data.rosterIndex = 255;
		data.controlType = 2;
		data.controlDataType = 1;
		data.position = glm::vec3(0.0f); // need to get the drop zone location
		data.rotation = 0;
		data.teamID = Team::home->getId();
		data.commanderID = Team::home->getId();
		data.active = 1;
		data.exists = 1;
		data.capturable = 0;
		data.baseColor = prefs.baseColor;
		data.highlightColor1 = prefs.highlightColor;
		data.highlightColor2 = prefs.highlightColor;
		

		strcpy( data.pilotFileName, "pmw00031" );
		strcpy( data.brainFileName, "pbrain" );
	
		glm::vec3 vector;
		vector.x = 0.f;
		vector.y = 1.f;
		vector.z = 0.f;
		glm::vec3 scaled;
		for ( auto iter = list.begin(); iter != list.end(); ++iter ) {
			scaled = vector;
			scaled *= 128.;
			data.position = dropZoneList[0] + scaled;
			//data.position = dropZoneList[dropZoneIndex++];
			data.objNumber = (*iter)->getFitID();
			strcpy( data.csvFileName, (*iter)->getFileName().c_str() );
			if ( !(*iter)->getPilot() )
				continue;
			strcpy( data.pilotFileName, (*iter)->getPilot()->getFileName().c_str() );
			data.overrideLoadedPilot = true;
			data.gunnerySkill = (*iter)->getPilot()->getGunnery();
			data.pilotingSkill = (*iter)->getPilot()->getPiloting();

			memcpy(data.specialtySkills,(*iter)->getPilot()->getSpecialtySkills(),sizeof(bool) * NUM_SPECIALTY_SKILLS);
			if ( !(*iter)->getVariant()->isDesignerMech())
			{		
				data.variant = 0;
				mission->addMover( &data, (*iter));
			}
			else
			{
				data.variant = (*iter)->getVariant()->getFileID();
				mission->addMover( &data );
			}
			Rotate( vector, rotation );
		}
	}

/*	if (MPlayer) {
		if (MPlayer->isHost())
			MPlayer->serverCID = MPlayer->commanderID;
		else {
			for (long i = 0; i < MAX_MC_PLAYERS; i++)
				if (MPlayer->serverPlayer && (MPlayer->playerList[i].player == MPlayer->serverPlayer))
					MPlayer->serverCID = MPlayer->playerList[i].commanderID;
			if (MPlayer->serverCID == -1)
				STOP(("Logistics.beginMission: bad serverCID"));
		}
	}
*/
	// VPL-#11: absorb any mech types registered post-Mission::init-finalize
	// by the SP force-group addMover waves above (logistics.cpp:675/780/785).
	// No-op unless a late type was staged. MUST be before any render --
	// the next render is gated by mission->update() at :808 (adversarial-
	// review no-render-in-gap invariant).
	GpuMechBatcher::instance().finalizePending();

	mission->missionInterface->initMechs();

	eye->activate();
	eye->update();
	mission->start();
	mission->update(); // force this, so we don't render first

	if (MPlayer) {
		CompressedMech mechData;
		mechData.commanderID = MPlayer->commanderID;
		MPlayer->sendMissionSetup(nullptr, 4, &mechData);
		if (!MPlayer->waitTillMissionSetup()) {
			// SERVER DROPPED
				mission->destroy();
				return(0);
		}
		MPlayer->setMode(MULTIPLAYER_MODE_MISSION);
	}

	if (MPlayer && MPlayer->hostLeft) {
		mission->destroy();
		return(0);
	}

	return(1);
}

int Logistics::DoBeginMission()
{

#ifndef TEST_SHELL
//---------------------------
// Networking startup code...
#if 0
	/*MPlayer->launchedFromLobby =*/ gos_NetCheckLobby();
	Environment.NetworkGame = TRUE;
	if (!MPlayer->launchedFromLobby) {
		if (!gos_NetStartGame(0)) {
			//gos_TerminateApplication();
			return;
		}
	}
#endif				

if (!MPlayer) {
	logistics->setLogisticsState(log_DONE);
	soundSystem->playBettySample(BETTY_NEW_CAMPAIGN);
}

#endif	//TEST_SHELL

	return 0;

}

void Logistics::initializeLogData()
{
	ZoneScopedN("Logistics::initializeLogData");
	LogisticsData::instance->removeMechsInForceGroup();

	{ ZoneScopedN("Logistics::initializeLogData LogisticsData::initFast");
	LogisticsData::instance->initFast();
	}
	Team* pTeam = Team::home;

	if ( pTeam )
	{
		for ( int i = pTeam->getRosterSize() - 1; i > -1; i-- )
		{
			Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
			LogisticsPilot* pPilot = LogisticsData::instance->getPilot(pMover->getPilot()->getName());

			DWORD base, highlight1, highlight2;
			(static_cast<Mech3DAppearance*>(pMover->getAppearance()))->getPaintScheme( highlight1,
				highlight2, base );
			
			
			if ( pMover->getObjectType()->getObjectTypeClass() == BATTLEMECH_TYPE )
			{
				LogisticsVariant* pVar = LogisticsData::instance->getVariant( (static_cast<BattleMech*>(pMover))->variantName );
				LogisticsData::instance->addMechToInventory( 
					pVar, 1, pPilot,
					base, highlight1, highlight2 );
			}
		}
	}
}

void Logistics::playFullScreenVideo( const char* fileName )
{
	if ( !fileName || !fileName[0])
		return;

	// MC2_BOOT_TO_BAY headless capture: skip all front-end fullscreen videos
	// (campaign intro PreVideo e.g. poarintro.bik, mission cutscenes) so the
	// automated boot reaches the bay/purchase screens deterministically instead
	// of sitting on the intro for the capture window.
	if ( mc2::gate::isNonEmpty<"MC2_BOOT_TO_BAY">() )
	{
		MC2_EVENT(Core, "VIDEO", 1, "event=skip name={} reason=boot_to_bay", std::string_view( fileName ));
		return;
	}

	FullPathFileName path;
	path.init( moviePath, fileName, ".bik" );

	// Fossil 800x600 pixel strip removed (was top=100 bottom=500, a
	// 400-px band that made "Houses Divided" / CNB News campaign
	// cutscenes float at the top of high-res displays with large
	// black padding below). Use the full window; letterbox math in
	// mc2video.cpp keeps source aspect correct regardless of output
	// resolution.
	RECT movieRect;
	movieRect.top    = 0;
	movieRect.left   = 0;
	movieRect.right  = Environment.screenWidth;
	movieRect.bottom = Environment.screenHeight;

	bMovie = new MC2Movie;
	bMovie->init(path,movieRect,true);
	MC2_EVENT(Core, "VIDEO", 1, "event=play name={}", std::string_view( fileName ));

	soundSystem->stopDigitalMusic();
}

void Logistics::setResultsHostLeftDlg( const char* pName )
{
	if ( missionResults && logisticsState == log_RESULTS )
	{
		missionResults->setHostLeftDlg( pName );
	}
}
