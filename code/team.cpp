//******************************************************************************************
//
//	team.cpp - This file contains the Team Class code
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//*

#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "team_internal.h"
#include <algorithm>
#include "save_archive.h"
#include "../GameOS/gameos/utils/mc2log.h"


char Team::relations[MAX_TEAMS][MAX_TEAMS] = {
	{0, 2, RELATION_NEUTRAL, 2, 2, 2, 2, 2},
	{2, 0, 2, 2, 2, 2, 2, 2},
	{RELATION_NEUTRAL, 2, 0, 2, 2, 2, 2, 2},
	{2, 2, 2, 0, 2, 2, 2, 2},
	{2, 2, 2, 2, 0, 2, 2, 2},
	{2, 2, 2, 2, 2, 0, 2, 2},
	{2, 2, 2, 2, 2, 2, 0, 2},
	{2, 2, 2, 2, 2, 2, 2, 0}
};
bool Team::noPain[MAX_TEAMS] = {false, false, false, false, false, false, false, false};

long			Team::numTeams = 0;
TeamPtr			Team::home = nullptr;
TeamPtr			Team::teams[MAX_TEAMS] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

bool			useRealLOS = true;

//***************************************************************************
// TEAM class
//***************************************************************************

void Team::init (void) {

	id = 0;
	rosterSize = 0;
	objectives.Clear();
	numPrimaryObjectives = 0;

	//numMechs = 0;
	//numVehicles = 0;
	//numElementals = 0;
	//mechHandle = -1;
	//vehicleHandle = -1;
	//elementalHandle = -1;

/*	nextContactId = 1;
	numContactUpdatesPerPass = NUM_CONTACT_UPDATES_PER_PASS;
	curContactUpdate = 0;
	numEnemyContacts = 0;
	numLOSContacts = 0;
	numSensorContacts = 0;
*/

/*	maxSensors = 0;
	numSensors = 0;

	jammers = NULL;
	ecms = NULL;
*/
	numTeams++;
}

//---------------------------------------------------------------------------

long Team::init (long _id, FitIniFile *pMissionFile) {

	id = _id;
	objectives.Alignment(id);
	if (pMissionFile) {
		objectives.Read(pMissionFile);
		numPrimaryObjectives = 0;
		for (CObjective* pObjective : objectives) {
			if (1 == pObjective->Priority())
				numPrimaryObjectives++;
		}
		ReadNavMarkers(pMissionFile, objectives);
	}

	return(NO_ERR);
}

//---------------------------------------------------------------------------

long Team::loadObjectives (FitIniFile *pMissionFile) {

	objectives.Alignment(id);
	if (pMissionFile) {
		objectives.Read(pMissionFile);
		numPrimaryObjectives = 0;
		for (CObjective* pObjective : objectives) {
			if (1 == pObjective->Priority())
				numPrimaryObjectives++;
		}
		ReadNavMarkers(pMissionFile, objectives);
	}

	return(NO_ERR);
}

//---------------------------------------------------------------------------

void Team::buildRoster (void) {

	//---------------------------------------------------------------------
	// This function builds a roster for the team from the movers currently
	// registered in the object manager. It assumes the teamId is already
	// set for each mover, and compares this team's id to that of the
	// objects to determine which objects belong on this team...

	rosterSize = 0;
	for (long i = 0; i < ObjectManager->getNumMovers(); i++) {
		MoverPtr mover = ObjectManager->getMover(i);
		if (mover->getTeamId() == id)
			roster[rosterSize++] = mover->getWatchID();
	}
	
//	numMechs = objClassTally[0];
//	numVehicles = objClassTally[1];
//	numElementals = objClassTally[2];
}

//---------------------------------------------------------------------------

MoverPtr Team::getMover (long index) {

	if (roster[index] > 0)
		return(static_cast<MoverPtr>(ObjectManager->getByWatchID(roster[index])));
	return(nullptr);
}

//----------------------------------------------------------------------------

void Team::addToRoster (MoverPtr mover) {

	if (mover)
		roster[rosterSize++] = mover->getWatchID();
}

//---------------------------------------------------------------------------

void Team::removeFromRoster (MoverPtr mover) {

	for (long i = 0; i < rosterSize; i++)
		if (roster[i] == mover->getWatchID()) {
			roster[i] = roster[--rosterSize];
			break;
		}
}

//---------------------------------------------------------------------------

bool Team::isContact (GameObjectPtr looker, MoverPtr mover, long contactCriteria) {

	return(SensorManager->getTeamSensor(id)->meetsCriteria(looker, mover, contactCriteria));
}

//---------------------------------------------------------------------------

long Team::getContacts (GameObjectPtr looker, int* contactList, int contactCriteria, int sortType) {

	return(SensorManager->getTeamSensor(id)->getContacts(looker, contactList, contactCriteria, sortType));
}

//---------------------------------------------------------------------------

bool Team::hasSensorContact (long teamID) {

	return(SensorManager->getTeamSensor(id)->hasSensorContact(teamID));
}

//---------------------------------------------------------------------------

long Team::getRoster (GameObjectPtr* objList, bool existsOnly) {

	long count = 0;
	if (existsOnly) {
		for (long i = 0; i < rosterSize; i++) {
			GameObjectPtr object = ObjectManager->getByWatchID(roster[i]);
			if (object && object->getExists())
				objList[count++] = object;
		}
		}
	else {
		for (long i = 0; i < rosterSize; i++) {
			GameObjectPtr object = ObjectManager->getByWatchID(roster[i]);
			if (object)
				objList[count++] = object;
		}
	}
	return(count);
}

///////////////////////////////////

//---------------------------------------------------------------------------

void Team::disableTargets (void) {

	for (long i = 0; i < rosterSize; i++) {
		GameObjectPtr object = ObjectManager->getByWatchID(roster[i]);
		if (object) {
			GameObjectPtr target = nullptr;
			if (object->isMover())
				target = (static_cast<MoverPtr>(object))->getPilot()->getCurrentTarget();
			if (target && target->isMover())
				(static_cast<MoverPtr>(target))->disable(66);
		}
	}
}

//---------------------------------------------------------------------------

void Team::eject (void) {

	for (long i = 0; i < rosterSize; i++) {
		MoverPtr mover = static_cast<MoverPtr>(ObjectManager->getByWatchID(roster[i]));
		if (mover) {
			if (mover->getObjectClass() == BATTLEMECH)
				mover->getPilot()->orderEject(false, true, ORDER_ORIGIN_COMMANDER);
			else {
				WeaponShotInfo shot;
				shot.init(0, -3, 254.0, 0, 0);
				mover->handleWeaponHit(&shot, (MPlayer != nullptr));
			}
		}
	}
}

//---------------------------------------------------------------------------

void Team::destroyTargets (void) {

	for (long i = 0; i < rosterSize; i++) {
		GameObjectPtr object = ObjectManager->getByWatchID(roster[i]);
		if (object) {
			GameObjectPtr target = nullptr;
			if (object->isMover())
				target = (static_cast<MoverPtr>(object))->getPilot()->getCurrentTarget();
			if (target && target->isMover()) 
			{
				//-----------------------------------------
				//-- Need to pound these guys to death.
				WeaponShotInfo shot;
				shot.init(0, -3, 5, 0, 0);

				for (long i=0;i<100;i++)
				{
					if (RollDice(30))
						shot.hitLocation = target->calcHitLocation(nullptr,-1,ATTACKSOURCE_DFA,0);
					else
						shot.hitLocation = target->calcHitLocation(nullptr,-1,ATTACKSOURCE_ARTILLERY,0);
					if (MPlayer) {
						target->handleWeaponHit(&shot, true);
						}
					else
						target->handleWeaponHit(&shot);
				}
			}
		}
	}
}

//---------------------------------------------------------------------------

bool Team::isTargeting (GameObjectWatchID targetWID, GameObjectWatchID exceptWID) {

	if (exceptWID)
		for (long i = 0; i < rosterSize; i++) {
			if (roster[i] == exceptWID)
				continue;
			MoverPtr mover = dynamic_cast<MoverPtr>(ObjectManager->getByWatchID(roster[i]));
			Assert(mover != nullptr, roster[i], " Team.isTargeting: NULL mover ");
			MechWarriorPtr pilot = mover->getPilot();
			if (pilot) {
				GameObjectPtr target = pilot->getCurrentTarget();
				if (target && (target->getWatchID() == targetWID))
					return(true);
			}
		}
	else
		for (long i = 0; i < rosterSize; i++) {
			MoverPtr mover = dynamic_cast<MoverPtr>(ObjectManager->getByWatchID(roster[i]));
			Assert(mover != nullptr, roster[i], " Team.isTargeting: NULL mover ");
			MechWarriorPtr pilot = mover->getPilot();
			if (pilot) {
				GameObjectPtr target = pilot->getCurrentTarget();
				if (target && (target->getWatchID() == targetWID))
					return(true);
			}
		}
	return(false);
}

//---------------------------------------------------------------------------

bool Team::isCapturing (GameObjectWatchID targetWID, GameObjectWatchID exceptWID) {

	if (exceptWID)
		for (long i = 0; i < rosterSize; i++) {
			if (roster[i] == exceptWID)
				continue;
			MoverPtr mover = dynamic_cast<MoverPtr>(ObjectManager->getByWatchID(roster[i]));
			Assert(mover != nullptr, roster[i], " Team.isTargeting: NULL mover ");
			MechWarriorPtr pilot = mover->getPilot();
			if (pilot && (pilot->getCurTacOrder()->code == TACTICAL_ORDER_CAPTURE)) {
				GameObjectPtr target = pilot->getCurTacOrder()->getTarget();
				if (target && (target->getWatchID() == targetWID))
					return(true);
			}
		}
	else
		for (long i = 0; i < rosterSize; i++) {
			MoverPtr mover = dynamic_cast<MoverPtr>(ObjectManager->getByWatchID(roster[i]));
			Assert(mover != nullptr, roster[i], " Team.isTargeting: NULL mover ");
			MechWarriorPtr pilot = mover->getPilot();
			if (pilot && (pilot->getCurTacOrder()->code == TACTICAL_ORDER_CAPTURE)) {
				GameObjectPtr target = pilot->getCurTacOrder()->getTarget();
				if (target && (target->getWatchID() == targetWID))
					return(true);
			}
		}
	return(false);
}

//---------------------------------------------------------------------------


//---------------------------------------------------------------------------

glm::vec3 Team::calcEscapeVector (MoverPtr mover, float threatRange) {

	static float distance[100];
	static glm::vec3 delta[100];

	glm::vec3 escapeVector;
	escapeVector = glm::vec3(0.0f);

	//------------------------------
	// Get the initial delta info...
	long shortest = 0;
	long longest = 0;
	for (long i = 0; i < rosterSize; i++) {
		GameObjectPtr obj = ObjectManager->getByWatchID(roster[i]);
		if (obj) {
			float distanceToObj = mover->distanceFrom(obj->getPosition());
			if (distanceToObj <= threatRange) {
				delta[i] = mover->getPosition() - obj->getPosition();
				distance[i] = distanceToObj;
				if (distance[i] > longest)
					longest = i;
				if (distance[i] < shortest)
					shortest = i;
				}
			else
				distance[i] = -999.0;
			}
		else
			distance[i] = -999.0;
	}

	//-----------------------------------------------------------------
	// Now, find the furthest enemy and scale the deltas accordingly...
	for (int i = 0; i < rosterSize; i++)
		if (distance[i] >= 0.0) {
			float scale = distance[longest] / distance[i];
			delta[i] *= scale;
			escapeVector += delta[i];
		}

	//--------------------------------------------------------------------------------
	// We don't care about the length, just the direction (we assume you want to go as
	// FAR as necessary)...
	escapeVector = glm::normalize(escapeVector);

	return(escapeVector);
}

//---------------------------------------------------------------------------

void Team::statusCount (int* statusTally) {

	//----------------------------------------------------------
	// statusTally counts the number of objects in the team with
	// each of the statuses...
	for (long i = 0; i < rosterSize; i++) 
	{
		MoverPtr obj = static_cast<MoverPtr>(ObjectManager->getByWatchID(roster[i]));
		Assert(obj != nullptr, i, " Team.statusCount: NULL roster object ");
		MechWarriorPtr pilot = obj->getPilot();
		if (!obj->getExists())
			statusTally[8]++;
		else if (!obj->getAwake())
			statusTally[7]++;
		else if (pilot && (pilot->getStatus() == WARRIOR_STATUS_WITHDRAWN))
			statusTally[6]++;
		else
		{
			long status = obj->getStatus();
			if ((status < 0) || (status > 5))
				Fatal(status," Status out of bounds ");
			statusTally[obj->getStatus()]++;
		}
	}
}

//---------------------------------------------------------------------------

void Team::destroy (void) {
	objectives.Clear();

}

//***************************************************************************

void disableHomeTeamTargets (void) {

	Team::home->disableTargets();
}

//---------------------------------------------------------------------------

void killHomeTeamTargets (void) {

	Team::home->destroyTargets();
}

//---------------------------------------------------------------------------
#include "save_struct_zero.h"
long Team::Save (PacketFilePtr file, long packetNum)
{
	MC2_SAVE_STRUCT(TeamStaticData, staticData);
	staticData.numTeams = numTeams;
	staticData.homeTeamId = home->getId();
	memcpy(staticData.relations,relations,sizeof(char) * MAX_TEAMS * MAX_TEAMS);
	memcpy(staticData.noPain,noPain,sizeof(bool) * MAX_TEAMS);

	file->writePacket(packetNum,reinterpret_cast<MemoryPtr>(&staticData),sizeof(TeamStaticData),STORAGE_TYPE_RAW);
	packetNum++;

	for (long i=0;i<numTeams;i++)
	{
		MC2_SAVE_STRUCT(TeamData, data);
		data.id = teams[i]->getId();
		// The written size is the one the loader accepts. Only the live entries are written: the rest of roster[] is never initialised, and copying it
		// would put heap leftovers into the save.
		const long liveRoster = std::clamp<long>(teams[i]->rosterSize, 0, MAX_MOVERS_PER_TEAM);
		data.rosterSize = liveRoster;
		memcpy(data.roster,teams[i]->roster,sizeof(GameObjectWatchID) * std::min<long>(liveRoster, 120));
		mc2::save::Bytes bytes(sizeof(data));
		memcpy(bytes.data(), &data, sizeof(data));
		if (liveRoster > 120) {
			std::vector<uint32_t> roster(teams[i]->roster, teams[i]->roster + liveRoster);
			mc2::save::Archive archive;
			archive.sequence(roster, MAX_MOVERS_PER_TEAM);
			if (!archive.good()) Fatal(0, "Invalid team roster save");
			bytes.insert(bytes.end(), archive.bytes().begin(), archive.bytes().end());
		}
		file->writePacket(packetNum, bytes.data(), static_cast<unsigned int>(bytes.size()), STORAGE_TYPE_RAW);
		packetNum++;
	}

	return packetNum;
}

//---------------------------------------------------------------------------
long Team::Load (PacketFilePtr file, long packetNum)
{
	TeamStaticData staticData{};

	file->readPacket(packetNum,reinterpret_cast<MemoryPtr>(&staticData));
	packetNum++;

	numTeams = staticData.numTeams;
	memcpy(relations,staticData.relations,sizeof(char) * MAX_TEAMS * MAX_TEAMS);
	memcpy(noPain,staticData.noPain,sizeof(bool) * MAX_TEAMS);

	for (long i=0;i<numTeams;i++)
	{
		TeamData data{};
		file->seekPacket(packetNum);
		const auto size = file->getPacketSize();
		if (size < sizeof(data) || size > sizeof(data) + 4 + MAX_MOVERS_PER_TEAM * 4) Fatal(0, "Invalid team packet size");
		mc2::save::Bytes bytes(size);
		file->readPacket(packetNum, bytes.data());
		memcpy(&data, bytes.data(), sizeof(data));
		std::vector<uint32_t> roster;
		if (size > sizeof(data)) {
			mc2::save::Archive archive(std::span(bytes).subspan(sizeof(data)));
			archive.sequence(roster, MAX_MOVERS_PER_TEAM);
			if (!archive.finish() || roster.size() != data.rosterSize) Fatal(0, "Invalid extended roster");
		} else if (data.rosterSize > 120) Fatal(0, "Missing extended roster");
		packetNum++;

		teams[i] = new Team;

		//Yet another Haxor.
		// void Init increments numTeams.  Decrement here.
		numTeams--;

		teams[i]->init(data.id);
		// A roster size outside the array is malformed content: the team is loaded with an empty roster.
		if (data.rosterSize < 0 || data.rosterSize > MAX_MOVERS_PER_TEAM)
		{
			MC2_LOG(Io, Warn, "team {}: saved roster size {} is outside 0..{}, loaded with an empty roster", data.id, data.rosterSize, MAX_MOVERS_PER_TEAM);
			data.rosterSize = 0;
		}
		teams[i]->rosterSize = data.rosterSize;
		if (roster.empty())
			memcpy(teams[i]->roster, data.roster, sizeof(GameObjectWatchID) * data.rosterSize);
		else
			std::copy(roster.begin(), roster.end(), teams[i]->roster);

		if (data.id == staticData.homeTeamId)
			home = teams[i];
	}


	return packetNum;
}
