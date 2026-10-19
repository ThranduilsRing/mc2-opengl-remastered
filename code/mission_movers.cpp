// Mission: createPartObject, addMover, removeMover, tradeMover, drop zones and gate queries.
// Declared in the class headers; shared names in mission_internal.h.
#include "mission_internal.h"
#include "mc2fmt.h"




void Mission::createPartObject (long partIndex, MoverPtr mover) {

	//-----------------------------------
	// All parts are movers in this game!
	
	//
	// This could NEVER have worked.  Destroyeds still need to be created.  Otherwise,
	// why is there code inside this IF which checks if destroyed?  I blame Glenn's Crazy Bracing Style!
	// Create the part just like always, just start it disabled.
	// 
	// If the mech is not on OUR team, you can recover it during the mission.
	// If the mech IS on our team, it starts shutdown and NOT added to Heidi's interface.
	// When we get close enough to it, it adds itself to Heidi's world and we can command it!
	if (parts[partIndex].objectWID == 0)
	{
		ObjectTypePtr objType = ObjectManager->getObjectType(parts[partIndex].objNumber);
		Assert(objType != nullptr, partIndex, " Mission.createPartObject: unable to get objType ");
		// Mod-tolerance: if objType is a stub (no appearance name) but the
		// mission provided a CSVFile, back-fill it so BattleMech::init can
		// open the chassis .csv. Without this, stub-only mechs crash at
		// strlen(NULL) inside FullPathFileName::init(csvName).
		if (objType && !objType->getAppearanceTypeName() && parts[partIndex].csvFile[0]) {
			objType->setAppearanceTypeName(parts[partIndex].csvFile);
		}
		parts[partIndex].objectWID = mover->getWatchID();
		mover->init(true, objType);
		mover->setAwake(parts[partIndex].active ? true : false);
		mover->setHandle(ObjectManager->getHandle(mover));

		//
		// Heidi, this is what needs to change in the mission.fit files!
		// -fs 12/7/99		A date which will live in Infamy.
		// Must also load Ground Vehicles the OLDen WAY until further notice.
		switch (mover->getObjectClass()) 
		{
			case BATTLEMECH: 
				mover->init(parts[partIndex].variantNum);		
				break;

			case GROUNDVEHICLE:
			case ELEMENTAL:
			{
				if (ObjectTypeManager::objectFile->seekPacket(parts[partIndex].objNumber) == NO_ERR) 
				{
					FitIniFile profileFile;
					long result = profileFile.open(ObjectTypeManager::objectFile,ObjectTypeManager::objectFile->getPacketSize());
					if (result != NO_ERR)
						STOP((" Mission.createPartObject: can't open file "));

					if (result == NO_ERR) 
					{
						result = mover->init(&profileFile);
						if (result != NO_ERR)
							STOP((" Mission.createPartObject: bad Profile File "));
					}
					
					profileFile.close();
				}
			}
			break;
		}

		mover->setPilotHandle(parts[partIndex].pilot);
		mover->setTeamId(parts[partIndex].teamId, true);
		mover->setSquadId(parts[partIndex].squadId);
		mover->calcWeaponEffectiveness(TRUE);
		mover->calcWeaponEffectiveness(FALSE);
		
		if (parts[partIndex].controlType == CONTROL_PLAYER)
			mover->setControl(CONTROL_PLAYER);
		else if (MPlayer)
			mover->setControl(MPlayer->isServer() ? CONTROL_AI : CONTROL_NET);
		else
			mover->setControl(CONTROL_AI);

		mover->setPosition(parts[partIndex].position);
		mover->setLastValidPosition(parts[partIndex].position);
		mover->setRotation(parts[partIndex].rotation);
		mover->setCommanderId(parts[partIndex].commanderID);

		switch (mover->getObjectClass()) {
			case BATTLEMECH: {
				(static_cast<BattleMechPtr>(mover))->captureable = parts[partIndex].captureable;

				AppearancePtr myActor = mover->getAppearance();

				if (MPlayer)
					myActor->resetPaintScheme( MPlayer->colors[MPlayer->playerInfo[mover->commanderId].stripeColor],
											   MPlayer->colors[MPlayer->playerInfo[mover->commanderId].stripeColor],
											   MPlayer->colors[MPlayer->playerInfo[mover->commanderId].baseColor[BASECOLOR_TEAM]]);
				else {
					if (mover->getCommanderId() == Commander::home->getId())
						myActor->resetPaintScheme(prefs.highlightColor, prefs.highlightColor, prefs.baseColor);
					else
						myActor->resetPaintScheme(parts[partIndex].highlightColor1,
												parts[partIndex].highlightColor2,
												parts[partIndex].baseColor);
					//myActor->setGesture(parts[partIndex].gestureId);  DOn't do this unless you want the mechs to start shutdown and power up!
				}
				}
				break;
			case GROUNDVEHICLE: {
				AppearancePtr myActor = mover->getAppearance();
				if (MPlayer)
					myActor->resetPaintScheme(MPlayer->colors[MPlayer->playerInfo[mover->commanderId].stripeColor],
											  MPlayer->colors[MPlayer->playerInfo[mover->commanderId].stripeColor],
											  MPlayer->colors[MPlayer->playerInfo[mover->commanderId].baseColor[BASECOLOR_TEAM]]);
				else {
					if (mover->getCommanderId() == Commander::home->getId())
						myActor->resetPaintScheme(prefs.highlightColor, prefs.highlightColor, prefs.baseColor);
					else
						myActor->resetPaintScheme(parts[partIndex].highlightColor1,
												  parts[partIndex].highlightColor2,
												  parts[partIndex].baseColor);
					//myActor->setGesture(parts[partIndex].gestureId);  DOn't do this unless you want the mechs to start shutdown and power up!
				}
				}
				break;
			case ELEMENTAL:
				break;
		}


		if (MPlayer) {
			MPlayer->addToMoverRoster(static_cast<MoverPtr>(ObjectManager->getByWatchID(parts[partIndex].objectWID)));
			MPlayer->addToPlayerMoverRoster(parts[partIndex].commanderID, static_cast<MoverPtr>(ObjectManager->getByWatchID(parts[partIndex].objectWID)));
			if (parts[partIndex].commanderID == MPlayer->commanderID)
				MPlayer->addToLocalMovers(static_cast<MoverPtr>(ObjectManager->getByWatchID(parts[partIndex].objectWID)));
		}

		if (parts[partIndex].exists)
		{
			mover->setExists(true);
		}
		else
			MC2_DESTROY(mover, "mission_load_inactive");

		if (parts[partIndex].destroyed) 
		{
			if (mover->getCommanderId() == Commander::home->getId())
			{
				mover->setStatus(OBJECT_STATUS_SHUTDOWN);
				mover->startDisabled = false;
				mover->setOnGUI(false);		//Even for non-player Team Parts.  It only FALSE if we link up with it later!!!!!
			}
			else
			{
				mover->setStatus(OBJECT_STATUS_DISABLED);
				mover->disable( UNDETERMINED_DEATH );
				mover->startDisabled = true;
				if (mover->sensorSystem)
					mover->sensorSystem->disable();
				mover->setOnGUI(true);		//Even for non-player Team Parts.  It only FALSE if we link up with it later!!!!!
			}
		}
		else
		{
			mover->setOnGUI(true);		//Even for non-player Team Parts.  It only FALSE if we link up with it later!!!!!
		}
	}
}

long Mission::addMover (MoverInitData* moverSpec) {
	
	MechWarriorPtr pilot = MechWarrior::newWarrior();
	if (!pilot)
		STOP(("Too many pilots in this mission!"));

	FullPathFileName pilotFullFileName;
	pilotFullFileName.init(warriorPath, moverSpec->pilotFileName, ".fit");
			
	FitIniFile pilotFile;
		
	long result = pilotFile.open(pilotFullFileName);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1):
	// the pilot .fit is required external data; a failed open feeds a not-open
	// FitIniFile into pilot->init() below.
	MC2_VERIFY(result == NO_ERR, "Mission::addMover: pilot .fit open failed (%ld): %s",
		result, moverSpec ? moverSpec->pilotFileName : "(null)");
	result = pilot->init(&pilotFile);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::addMover: pilot init failed (%ld)", result);

	pilotFile.close();

	if (moverSpec->overrideLoadedPilot)
	{
		pilot->skills[MWS_GUNNERY] = pilot->skillRank[MWS_GUNNERY] = moverSpec->gunnerySkill;
		pilot->skills[MWS_PILOTING] = pilot->skillRank[MWS_PILOTING] = moverSpec->pilotingSkill;
		memcpy(pilot->specialtySkills,moverSpec->specialtySkills,sizeof(bool) * NUM_SPECIALTY_SKILLS);
		pilot->calcRank();
	}

	//*********************
	// NOTE: Need to send packet to other players in MP with new pilot and
	// mover data!
	//*********************

	long numErrors, numLinesProcessed;
	FullPathFileName brainFullFileName;
	if (MPlayer) {
		pilot->setBrainName("pbrain");
		brainFullFileName.init(warriorPath, "pbrain", ".abl");
		}
	else {
		pilot->setBrainName(moverSpec->brainFileName);
		brainFullFileName.init(warriorPath, moverSpec->brainFileName, ".abl");
	}
		
	long moduleHandle = ABLi_preProcess(brainFullFileName, &numErrors, &numLinesProcessed);
	pilot->setBrain(moduleHandle);

/*
typedef struct _MoverInitData {
	char			pilotFileName[50];
	char			brainFileName[50];
	char			profileName[50];
	long			objNumber;
	unsigned long	controlType;
	unsigned long	controlDataType;
	unsigned long	variant;
	float			position[2];
	long			rotation;
	char			teamID;
	char			commanderID;
	long			paintScheme;
	bool			active;
	bool			exists;
	char			icon;
	bool			capturable;
} MoverInitData;
*/
			//parts[i].gestureId = 2; // this has never changed
	ObjectTypePtr objType = ObjectManager->loadObjectType(moverSpec->objNumber);
	if (!objType)
		objType = ObjectManager->getObjectType(moverSpec->objNumber);
	if (objType) {
		MoverPtr mover = nullptr;
		switch (objType->getObjectTypeClass()) {
			case BATTLEMECH_TYPE:
				mover = static_cast<MoverPtr>(ObjectManager->newMech());
				break;
			case VEHICLE_TYPE:
				mover = static_cast<MoverPtr>(ObjectManager->newVehicle());
				break;
		}

		if (mover) 
		{
			mover->init(true, objType);
			mover->setAwake(moverSpec->active);
			mover->setHandle(ObjectManager->getHandle(mover));

			//
			// Heidi, this is what needs to change in the mission.fit files!
			// -fs 12/7/99		A date which will live in Infamy.
			// Must also load Ground Vehicles the OLDen WAY until further notice.
			switch (mover->getObjectClass()) {
				case BATTLEMECH: 
					mover->init(moverSpec->variant);		
					break;
				case GROUNDVEHICLE:
				case ELEMENTAL:	
				{
					if (ObjectTypeManager::objectFile->seekPacket(moverSpec->objNumber) == NO_ERR) 
					{
						FitIniFile profileFile;
						long result = profileFile.open(ObjectTypeManager::objectFile,ObjectTypeManager::objectFile->getPacketSize());
						if (result != NO_ERR)
							STOP((" Mission.createPartObject: can't open file "));
	
						if (result == NO_ERR) 
						{
							result = mover->init(&profileFile);
							if (result != NO_ERR)
								STOP((" Mission.createPartObject: bad Profile File "));
						}
						
						profileFile.close();
					}
				}
				break;
			}

			mover->setPilotHandle(pilot->getIndex());
			mover->setTeamId(moverSpec->teamID, true);
			mover->calcWeaponEffectiveness(true);
			mover->calcWeaponEffectiveness(false);
		
			if (moverSpec->controlType == CONTROL_PLAYER)
				mover->setControl(CONTROL_PLAYER);
			else if (MPlayer)
				mover->setControl(MPlayer->isServer() ? CONTROL_AI : CONTROL_NET);
			else
				mover->setControl(CONTROL_AI);

			mover->setPosition(moverSpec->position);
			mover->setLastValidPosition(moverSpec->position);
			mover->setRotation(moverSpec->rotation);
			mover->setCommanderId(moverSpec->commanderID);

			switch (mover->getObjectClass()) {
				case BATTLEMECH: {
					(static_cast<BattleMechPtr>(mover))->captureable = moverSpec->capturable;
					AppearancePtr myActor = mover->getAppearance();
					myActor->resetPaintScheme(moverSpec->highlightColor1,
											moverSpec->highlightColor2,
											moverSpec->baseColor);
					//myActor->setGesture(moverSpec->gestureID);  Don't do this unless you want things to start shutdown and power up!
					}
					break;
				case GROUNDVEHICLE:	{
					AppearancePtr myActor = mover->getAppearance();
					myActor->resetPaintScheme(moverSpec->highlightColor1,
											moverSpec->highlightColor2,
											moverSpec->baseColor);
					}
					break;
				case ELEMENTAL:
					break;
			}


			if (MPlayer) {
				MPlayer->addToMoverRoster(mover);
				MPlayer->addToPlayerMoverRoster(moverSpec->commanderID, mover);
				if (moverSpec->commanderID == MPlayer->commanderID)
					MPlayer->addToLocalMovers(mover);
			}

			if (moverSpec->exists)
				mover->setExists(true);
			else
				MC2_DESTROY(mover, "mission_load_inactive");

			//---------------------------------------------------------------------------------
			// If we're not playing multiplayer, make sure all home commander movers have their
			// localMoverId set to 0, so the iface can at least check if a mover is player
			// controlled...
			if (!MPlayer && (Commander::home->getId() == moverSpec->commanderID))
				mover->setLocalMoverId(0);

			ObjectManager->modifyMoverLists(mover, MOVERLIST_ADD);
			// TEAM-COMMANDER-OWNERSHIP-1 audit: the only id-indexed access to the
			// static Team::teams[] that survives OUTSIDE init's now-bounded create
			// loops. moverSpec->teamID is a file/save-supplied char (MoverInitData)
			// that indexes Team::teams[MAX_TEAMS] and is immediately dereferenced --
			// a bad/mod-supplied id is an OOB read + NULL/garbage deref (companion to
			// the init-side A5 write guards). Bound the index AND require the slot to
			// have been created before the addToRoster deref; log mode skips the
			// roster add for the malformed mover instead of AVing.
			if (MC2_VERIFY_BOUNDS(moverSpec->teamID, MAX_TEAMS, "Mission::addMover moverSpec->teamID") &&
				MC2_VERIFY_NOTNULL(Team::teams[moverSpec->teamID], "Mission::addMover Team::teams[teamID]"))
				Team::teams[moverSpec->teamID]->addToRoster(mover);
			missionInterface->addMover(mover);

			for (long i = 0; i < ObjectManager->getNumMovers(); i++) {
				MoverPtr curMover = ObjectManager->getMover(i);
				if (!curMover->isDisabled()) {
					if (curMover->sensorSystem)
						curMover->sensorSystem->scanMover(mover);
					if ( curMover->getPilot() )
						curMover->getPilot()->triggerAlarm(PILOT_ALARM_NEW_MOVER, mover->getWatchID());
				}
			}
	
			if (moverSpec->numComponents > 0) {
				if (mover->getObjectClass() != BATTLEMECH)
					STOP(("LogisticsMech was not a MECH!!"));
				(static_cast<BattleMechPtr>(mover))->resetComponents(moverSpec->numComponents, moverSpec->components);
			}	

            // sebi: !NB
            // set it so that object wil be drawn!!!
            // currently seems that newly created gvehicl wilnot be drawn because isOnGui set to false
            // (actually it was not initialized at all until I added it to GroundVehicle::init)
            // and if isOnGui == false, then alphaValue will be set to 0x00 (see gvehicl.cpp) and renderer will not render it because
            // there is no case in MC_TextureManager::renderLists() for only MC2_DRAWALPHA flag
            // how it works on Windows... who knows.
			mover->setOnGUI(true);

			return(mover->getHandle());
		}
	}
	else
		return -1;

	return(0);
}


long Mission::addMover (MoverInitData* moveSpec, LogisticsMech* mechData)
{
  
	MoverInitData mData;
	memset(&mData,0,sizeof(MoverInitData));
	
	strncpy(mData.pilotFileName,mechData->getPilot()->getFileName().c_str(),49);
	strcpy(mData.brainFileName,"pbrain");
	strncpy(mData.csvFileName,mechData->getFileName().c_str(),50);
	
	mData.rosterIndex = 255;
	mData.objNumber = mechData->getFitID();
	mData.controlType = moveSpec->controlType;
	mData.controlDataType = moveSpec->controlDataType;
	mData.variant = moveSpec->variant;
	mData.position.x = moveSpec->position.x;
	mData.position.y = moveSpec->position.y; 
	mData.position.z = moveSpec->position.z; 
	mData.rotation = moveSpec->rotation;
	mData.teamID = moveSpec->teamID;
	mData.commanderID = moveSpec->commanderID;
	mData.baseColor = moveSpec->baseColor;
	mData.highlightColor1 = moveSpec->highlightColor1;
	mData.highlightColor2 = moveSpec->highlightColor2;
	mData.gestureID = 2;
	mData.active = true;
	mData.exists = true;
	mData.icon = 0;
	mData.capturable = false;
	mData.overrideLoadedPilot = moveSpec->overrideLoadedPilot;
	mData.gunnerySkill = moveSpec->gunnerySkill;
	mData.pilotingSkill = moveSpec->pilotingSkill;
	memcpy(mData.specialtySkills,moveSpec->specialtySkills,sizeof(bool) * NUM_SPECIALTY_SKILLS);

	long moverHandle = addMover(&mData);

	MoverPtr mMech = static_cast<MoverPtr>(ObjectManager->get(moverHandle));
	// BATTLEMECH-RESET-KIND-GUARD-1: the customization below (variantName strcpy +
	// resetComponents) is BattleMech-ONLY. A non-BattleMech deployable
	// (GroundVehicle / Elemental — valid MC2X content, e.g. a salvaged Repair Truck
	// fielded via the cheat-mode lance) was only soft-STOP'd here and fell through to
	// a BattleMech downcast: variantName written to the wrong offset, then
	// resetComponents -> calcMaxTargetDamage -> null call / crash. The mover is
	// already fully created+initialised by addMover() above (vehicles via CSV), so
	// return its handle and skip the mech-only step. Uses the runtime object-class
	// discriminator (NOT name/weight). Does not strip or reject the vehicle.
	if (mMech->getObjectClass() != BATTLEMECH)
	{
		return moverHandle;
	}

	strcpy( (static_cast<BattleMech*>(mMech))->variantName, mechData->getName().c_str() );
		
	long totalComponents = mechData->getComponentCount();

	long *componentList = nullptr;
	
	if ( totalComponents )
	{
		componentList = static_cast<long *>(systemHeap->Malloc(sizeof(long) * totalComponents));
	
		long otherCount = totalComponents;
		mechData->getComponents(otherCount, componentList);

		if (otherCount != totalComponents)
			STOP(("Heidi's getComponentCount does not agree with count returned from getComponents"));

	}
	
	
	(static_cast<BattleMechPtr>(mMech))->resetComponents(totalComponents,componentList);
	
	return moverHandle;
}


void DEBUGWINS_removeGameObject (GameObjectPtr obj);

long Mission::removeMover (MoverPtr mover) {

	DEBUGWINS_removeGameObject(static_cast<GameObjectPtr>(mover));
	missionInterface->removeMover(mover);
	if (MPlayer) {
		MPlayer->removeFromLocalMovers(mover);
		MPlayer->removeFromMoverRoster(mover);
	}
	ObjectManager->freeMover(mover);
	return(0);
}


void Mission::tradeMover (MoverPtr mover, long newTeamID, long newCommanderID, char* pilotFileName, const char* brainFileName) {

	missionInterface->removeMover(mover);
	if (MPlayer) {
		MPlayer->removeFromLocalMovers(mover);
		MPlayer->removeFromPlayerMoverRoster(mover);
	}
	if (newCommanderID > -1) 
	{
		char realPilotName[256];
		strcpy(realPilotName,&(pilotFileName[MPlayer ? 4 : 3]));
		LogisticsPilot *lPilot = LogisticsData::instance->getPilot(realPilotName);
		mover->loadPilot(pilotFileName, brainFileName, lPilot);
	}
	ObjectManager->tradeMover(mover, newTeamID, newCommanderID);
	if (mover->sensorSystem)
		mover->sensorSystem->broken = false;
	if (MPlayer && (newCommanderID > -1)) {
		MPlayer->addToPlayerMoverRoster(newCommanderID, mover);
		if (newCommanderID == MPlayer->commanderID)
			MPlayer->addToLocalMovers(mover);
	}

	if (newCommanderID > -1) 
	{
		missionInterface->addMover(mover);
	}
}


bool Mission::calcComplexDropZones (char* missionName, char dropZoneCID[MAX_MC_PLAYERS]) {

	for (long p = 0; p < MAX_MC_PLAYERS; p++)
		dropZoneCID[p] = -1;

	FullPathFileName missionFileName;
	missionFileName.init(missionPath,missionName,".fit");

	FitIniFile missionFile;
	
	long result = missionFile.open(missionFileName);
	if (result != NO_ERR)
		STOP(("Unable to open Mission File %s",missionFileName));

	/*long maxTeamID = -1;
	long maxCommanderID = -1;
	for (long i = 0; i < MAX_MC_PLAYERS; i++) {
		if (MPlayer->playerInfo[i].team > maxTeamID)
			maxTeamID = MPlayer->playerInfo[i].team;
		if (MPlayer->playerInfo[i].commanderID > maxCommanderID)
			maxCommanderID = MPlayer->playerInfo[i].commanderID;
	}
	*/
	long dropZoneSetup[MAX_MC_PLAYERS] = {-1, -1, -1, -1, -1 ,-1 ,-1 ,-1};
	result = missionFile.seekBlock("Parts");
	gosASSERT(result == NO_ERR);
	result = missionFile.readIdULong("NumParts",numParts);
	gosASSERT(result == NO_ERR);
	if (numParts)
		for (long i = 1; i < long(numParts + 1); i++) {
			char partName[12];
			mc2::fmt::to_buf(partName, "Part{:d}", mc2::fmt::as_i32(i));
			
			result = missionFile.seekBlock(partName);
			gosASSERT(result == NO_ERR);

			char teamID = -1;
			result = missionFile.readIdChar("TeamId", teamID);
			gosASSERT(result == NO_ERR);
			if ((teamID < 0) || (teamID >= MAX_TEAMS))
				STOP(("Mission.calcComplexDropZones: bad teamID"));

			char commanderID = -1;
			result = missionFile.readIdChar("CommanderId", commanderID);
			if (result != NO_ERR) {
				long cID;
				result = missionFile.readIdLong("CommanderId", cID);
				gosASSERT(result == NO_ERR);
				commanderID = static_cast<char>(cID);
			}

			if ((commanderID < 0) || (commanderID >= MAX_MC_PLAYERS))
				STOP(("Mission.calcComplexDropZones: bad commanderID"));
			dropZoneSetup[commanderID] = teamID;
	}

	long teamSize[MAX_TEAMS][2];
	for (long i = 0; i < MAX_TEAMS; i++) {
		teamSize[i][0] = 0;
		teamSize[i][1] = 0;
	}
/*	for (i = 0; i < MAX_MC_PLAYERS; i++)
		if (dropZoneSetup[i] > -1)
			teamSize[dropZoneSetup[i]][0]++;
	for (i = 0; i < MAX_MC_PLAYERS; i++)
		if (MPlayer->playerInfo[i].commanderID > -1)
			teamSize[MPlayer->playerInfo[i].team][1]++;
	for (i = 0; i < MAX_MC_PLAYERS; i++)
		if (teamSize[i][0] != teamSize[i][1])
			return(false);
*/
	for (int i = 0; i < MAX_MC_PLAYERS; i++) {
		long index = RandomNumber(teamSize[MPlayer->playerInfo[i].team][0]--);
		for (long j = 0; j < MAX_MC_PLAYERS; j++)
			if (dropZoneSetup[j] == MPlayer->playerInfo[i].team) {
				if (index == 0) {
					dropZoneSetup[j] = -1;
					dropZoneCID[j] = i;
					break;
				}
				index--;
			}

	}

	missionFile.close();


	return(true);
}


bool IsGateDisabled (int objectWID) {

	if (objectWID < 1)
		return(false);
	GameObjectPtr obj = ObjectManager->getByWatchID(objectWID);
	if (obj && obj->isDisabled())
		return(true);
	if (obj->isBuilding()) {
		GameObjectPtr parent = (static_cast<BuildingPtr>(obj))->getParent();
		if (parent)
			return(parent->isDisabled());
	}
	return(false);
}


bool IsGateOpen (int objectWID) {

	if (!objectWID)
		return(false);
	GameObjectPtr obj = ObjectManager->getByWatchID(objectWID);
	if (!obj)
		return(false);
	if (obj->getObjectClass() == GATE)
		return((static_cast<Gate*>(obj))->opened);
	return(false);
}
