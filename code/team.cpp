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
SortListPtr		Team::sortList = nullptr;

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
		CObjectives::EIterator it = objectives.Begin();
		while (!it.IsDone()) {
			if (1 == (*it)->Priority())
				numPrimaryObjectives++;
			it++;
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
		CObjectives::EIterator it = objectives.Begin();
		while (!it.IsDone()) {
			if (1 == (*it)->Priority())
				numPrimaryObjectives++;
			it++;
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
		return((MoverPtr)ObjectManager->getByWatchID(roster[index]));
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
				target = ((MoverPtr)object)->getPilot()->getCurrentTarget();
			if (target && target->isMover())
				((MoverPtr)target)->disable(66);
		}
	}
}

//---------------------------------------------------------------------------

void Team::eject (void) {

	for (long i = 0; i < rosterSize; i++) {
		MoverPtr mover = (MoverPtr)ObjectManager->getByWatchID(roster[i]);
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
				target = ((MoverPtr)object)->getPilot()->getCurrentTarget();
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

void Team::markRadiusSeen (Stuff::Vector3D& location, float radius) {

	//---------------------------------------------------------------
	// Once we get visibits in for all 8 teams, we should change this
	// appropriately...
	land->markRadiusSeen(location, radius, id);
}

//---------------------------------------------------------------------------

void Team::markRadiusSeenToTeams (Stuff::Vector3D& location, float radius, bool shrinkForNight) {

	if (radius < 0.0)
		radius = fireVisualRange;
		
	if (shrinkForNight)
		radius -= (radius * 0.25f);

	bool didTeam[MAX_TEAMS] = {false, false, false, false, false, false, false, false};
	for (long i = 0; i < ObjectManager->getNumMovers(); i++) {
		MoverPtr mover = ObjectManager->getMover(i);
		if (mover->getTeam() && !didTeam[mover->getTeamId()] && !isFriendly(mover->getTeam())) {
			Stuff::Vector3D result;
			result.x = location.x - mover->getPosition().x;
			result.y = location.y - mover->getPosition().y;
			result.z = 0.0;
			float dist = result.GetLength() * metersPerWorldUnit;
			if (dist < maxVisualRange) {
				markRadiusSeen(location, radius);
				didTeam[mover->getTeamId()] = true;
			}
		}
	}
}

//---------------------------------------------------------------------------

void Team::markSeen (Stuff::Vector3D& location, float specialUnitExpand) {

	//---------------------------------------------------------------
	// Once we get visibits in for all 8 teams, we should change this
	// appropriately...
	land->markSeen(location, id, specialUnitExpand);
}

//---------------------------------------------------------------------------


//---------------------------------------------------------------------------

Stuff::Vector3D Team::calcEscapeVector (MoverPtr mover, float threatRange) {

	static float distance[100];
	static Stuff::Vector3D delta[100];

	Stuff::Vector3D escapeVector;
	escapeVector.Zero();

	//------------------------------
	// Get the initial delta info...
	long shortest = 0;
	long longest = 0;
	for (long i = 0; i < rosterSize; i++) {
		GameObjectPtr obj = ObjectManager->getByWatchID(roster[i]);
		if (obj) {
			float distanceToObj = mover->distanceFrom(obj->getPosition());
			if (distanceToObj <= threatRange) {
				delta[i].Subtract(mover->getPosition(), obj->getPosition());
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
	escapeVector.Normalize(escapeVector);

	return(escapeVector);
}

//---------------------------------------------------------------------------

void Team::statusCount (int* statusTally) {

	//----------------------------------------------------------
	// statusTally counts the number of objects in the team with
	// each of the statuses...
	for (long i = 0; i < rosterSize; i++) 
	{
		MoverPtr obj = (MoverPtr)ObjectManager->getByWatchID(roster[i]);
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

	file->writePacket(packetNum,(MemoryPtr)&staticData,sizeof(TeamStaticData),STORAGE_TYPE_RAW);
	packetNum++;

	for (long i=0;i<numTeams;i++)
	{
		MC2_SAVE_STRUCT(TeamData, data);
		data.id = teams[i]->getId();
		data.rosterSize = teams[i]->rosterSize;
		// Only the live entries are written: the rest of roster[] is never initialised, and copying it would put heap leftovers into the save.
		memcpy(data.roster,teams[i]->roster,sizeof(GameObjectWatchID) * std::min<long>(teams[i]->rosterSize, MAX_MOVERS_PER_TEAM));

		file->writePacket(packetNum,(MemoryPtr)&data,sizeof(TeamData),STORAGE_TYPE_RAW);
		packetNum++;
	}

	return packetNum;
}

//---------------------------------------------------------------------------
long Team::Load (PacketFilePtr file, long packetNum)
{
	TeamStaticData staticData{};

	file->readPacket(packetNum,(MemoryPtr)&staticData);
	packetNum++;

	numTeams = staticData.numTeams;
	memcpy(relations,staticData.relations,sizeof(char) * MAX_TEAMS * MAX_TEAMS);
	memcpy(noPain,staticData.noPain,sizeof(bool) * MAX_TEAMS);

	for (long i=0;i<numTeams;i++)
	{
		TeamData data{};
		file->readPacket(packetNum,(MemoryPtr)&data);
		packetNum++;

		teams[i] = new Team;

		//Yet another Haxor.
		// void Init increments numTeams.  Decrement here.
		numTeams--;

		teams[i]->init(data.id);
		teams[i]->rosterSize = data.rosterSize;
		memcpy(teams[i]->roster,data.roster,sizeof(GameObjectWatchID) * MAX_MOVERS_PER_TEAM);

		if (data.id == staticData.homeTeamId)
			home = teams[i];
	}


	return packetNum;
}
