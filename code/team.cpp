//******************************************************************************************
//
//	team.cpp - This file contains the Team Class code
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//*

#ifndef MCLIB_H
#include"mclib.h"
#endif

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef MOVER_H
#include"mover.h"
#endif

#ifdef USE_MECHS
#ifndef MECH_H
#include<mech.h>
#endif
#endif

#ifndef GVEHICL_H
#include"gvehicl.h"
#endif

#ifdef USE_ARTILLERY
#ifndef ARTLRY_H
#include<artlry.h>
#endif
#endif

#ifndef UNITDESG_H
#include"unitdesg.h"
#endif

#ifndef TEAM_H
#include"team.h"
#endif

#ifdef USE_MOVERCONTROL
#ifndef AICTRL_H
#include<aictrl.h>
#endif
#endif

#ifndef MULTPLYR_H
#include"multplyr.h"
#endif

#ifndef CONTACT_H
#include"contact.h"
#endif

#ifndef MISSION_H
#include"mission.h"
#endif

#ifndef MISSIONGUI_H
#include"missiongui.h"
#endif

#ifndef WARRIOR_H
#include"warrior.h"
#endif

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
#ifdef LAB_ONLY
extern bool drawTerrainGrid;
extern long currentLineElement;
extern LineElement *debugLines[];
#endif


extern float maxVisualRange;
extern long	visualRangeTable[];
extern unsigned long MaxTreeLOSCellBlock;

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

//---------------------------------------------------------------------------
bool Team::teamLineOfSight (Stuff::Vector3D tPos, float extRad)
{
	//-----------------------------------------------------------
	// For each member of the team, check LOS to point provided.
	for (long i = 0; i < rosterSize; i++) 
	{
		MoverPtr obj = (MoverPtr)ObjectManager->getByWatchID(roster[i]);
		if (!obj->isDisabled() && !obj->isDestroyed() && (obj->getStatus() != OBJECT_STATUS_SHUTDOWN))
		{
			Stuff::Vector3D distance;
			distance.Subtract(tPos,obj->getPosition());
			float dist = distance.GetApproximateLength();
		
			//Figure out altitude above minimum terrain altitude and look up in table.
			float baseElevation = MapData::waterDepth;
			if (MapData::waterDepth < Terrain::userMin)
				baseElevation = Terrain::userMin;
		
			float altitude = obj->getPosition().z - baseElevation;
			float altitudeIntegerRange = (Terrain::userMax - baseElevation) * 0.00390625f;
			long altLevel = 0;
			if (altitudeIntegerRange > Stuff::SMALL)
				altLevel = altitude / altitudeIntegerRange;
			
			if (altLevel < 0)
				altLevel = 0;
		
			if (altLevel > 255)
				altLevel = 255;
		
			float radius = visualRangeTable[altLevel];
			
			//Scouting specialty skill.
			if (obj->isMover())
			{
				MoverPtr mover = obj;
				if (mover->pilot && mover->pilot->isScout())
					radius += (radius * 0.2f);
					
				radius *= mover->getLOSFactor();
			}
		
			if (dist <= (radius * 25.0f * worldUnitsPerMeter))
			{
				if (lineOfSight(obj->getLOSPosition(),tPos,id,extRad,0.0f,false))
					return true;
			}
		}
	}

	//-------------------------------------------------------------------------
	// Check the lookout towers now.  You can find them in special Buildings!!
	for (long spBuilding = 0; spBuilding < ObjectManager->numSpecialBuildings; spBuilding++)
	{
		if (ObjectManager->specialBuildings[spBuilding] && 
			ObjectManager->specialBuildings[spBuilding]->getExists() &&
			ObjectManager->specialBuildings[spBuilding]->isLookoutTower() &&
			(ObjectManager->specialBuildings[spBuilding]->getTeamId() ==id))
		{
			GameObjectPtr obj = ObjectManager->specialBuildings[spBuilding];
			if (!obj->isDisabled() && !obj->isDestroyed() && (obj->getStatus() != OBJECT_STATUS_SHUTDOWN))
			{
				Stuff::Vector3D distance;
				distance.Subtract(tPos,obj->getPosition());
				float dist = distance.GetApproximateLength();
			
				//Figure out altitude above minimum terrain altitude and look up in table.
				float baseElevation = MapData::waterDepth;
				if (MapData::waterDepth < Terrain::userMin)
					baseElevation = Terrain::userMin;
			
				float altitude = obj->getPosition().z - baseElevation;
				float altitudeIntegerRange = (Terrain::userMax - baseElevation) * 0.00390625f;
				long altLevel = 0;
				if (altitudeIntegerRange > Stuff::SMALL)
					altLevel = altitude / altitudeIntegerRange;
				
				if (altLevel < 0)
					altLevel = 0;
			
				if (altLevel > 255)
					altLevel = 255;
			
				float radius = visualRangeTable[altLevel];
				
				//Scouting specialty skill.
				if (obj->isMover())
				{
					MoverPtr mover = (MoverPtr)obj;
					if (mover->pilot && mover->pilot->isScout())
						radius += (radius * 0.2f);
						
					radius *= mover->getLOSFactor();
				}
			
				if (dist <= (radius * 25.0f * worldUnitsPerMeter))
				{
					if (lineOfSight(obj->getLOSPosition(),tPos,id,0.0f,obj->getAppearRadius(),false))
						return true;
				}
			}
				
		}
	}

	return false;
}

//#define USE_OLD_LOS

#ifdef USE_OLD_LOS

//---------------------------------------------------------------------------
bool Team::lineOfSight (float startLocal, long mCellRow, long mCellCol, long tCellRow, long tCellCol, long teamId, float extRad, bool checkVisibleBits)
{
	#ifdef LAB_ONLY
	__int64 x=GetCycles();
	#endif
	
	//-----------------------------------------------------
	// Once we allow teams to have alliances (for contacts,
	// etc.), simply set all nec. team bits in this mask...

	//TILE HACK...
	long tileRow = tCellRow / 3;
	long tileCol = tCellCol / 3;

	if ((teamId < 0) || (teamId >= MAX_TEAMS))	//Not on any team.  It can see everything!
		return true;

	if (checkVisibleBits) {
		unsigned char teamMask = 0x01 << teamId;
		unsigned char visbBits[4];
	
		//----------------------------------------------------------------------------
		// First check is simple.  Is anyone within the magical line of sight radius?
		// If not, return false and move on.
		// If they are, you MUST check LOS between this object and the other one.
		bool losResult = false;
		visbBits[0] = Terrain::VisibleBits->getFlag(tileRow, tileCol);
		if (visbBits[0] & teamMask)
			losResult = true;
		
		if (!losResult)
		{
			visbBits[1] = Terrain::VisibleBits->getFlag(tileRow + 1, tileCol);
			if (visbBits[1] & teamMask)
				losResult = true;
		}
			
		if (!losResult)
		{
			visbBits[2] = Terrain::VisibleBits->getFlag(tileRow + 1, tileCol + 1);
			if (visbBits[2] & teamMask)
				losResult = true;
		}
		
		if (!losResult)
		{
			visbBits[3] = Terrain::VisibleBits->getFlag(tileRow, tileCol + 1);
			if (visbBits[3] & teamMask)
				losResult = true;
		}

		if (!losResult)
		{
			return losResult;
		}
	}

	if (useRealLOS)
	{
		//------------------------------------------------------------------------------------------
		// Within magic radius.  Check REAL LOS now.
		// Check is really simple.
		// Find deltaCellRow and deltaCellCol and iterate over them from source to dest.
		// If the magic line ever goes BELOW the terrainElevation PLUS localElevation return false.
		Stuff::Vector3D startPos, endPos;
		startPos.Zero();
		endPos.Zero();
		
		land->getCellPos(tCellRow,tCellCol,endPos);
		land->getCellPos(mCellRow,mCellCol,startPos);
		startPos.z += startLocal;

		Stuff::Vector3D deltaCellVec;
		deltaCellVec.y = tCellRow - mCellRow;
		deltaCellVec.x = tCellCol - mCellCol;
		deltaCellVec.z = 0.0f;
		float startHeight = startPos.z;
		
		float length = deltaCellVec.GetApproximateLength();
		
		if (length > Stuff::SMALL)
		{
			float colLength = deltaCellVec.x / length;
			float rowLength = deltaCellVec.y / length;
			float heightLen = (endPos.z - startPos.z) / length;
			
			float lastCol = fabs(colLength * 2.0);
			float lastRow = fabs(rowLength * 2.0);
			
			float startCellRow = mCellRow;
			float startCellCol = mCellCol;
			
			float endCellRow = tCellRow;
			float endCellCol = tCellCol;
			
			Stuff::Vector3D currentPos = startPos;
			Stuff::Vector3D dist;
			dist.Subtract(endPos,currentPos);
			
			float remainingDist = dist.GetApproximateLength();
			bool colDone = false, rowDone = false;
			while (!colDone || !rowDone)
			{
				if (fabs(startCellRow - endCellRow) > lastRow)	//DO NOT INCLUDE LAST CELL!!!!!
				{
					startCellRow += rowLength;
				}
				else
				{
//					startCellRow = (endCellRow - lastRow);
					rowDone = true;
				}
					
				if (fabs(startCellCol - endCellCol) > lastCol)	//DO NOT INCLUDE LAST CELL!!!!!
				{
					startCellCol += colLength;
				}
				else
				{
//					startCellCol = (endCellCol - lastCol);
					colDone = true;
				}

				startHeight += heightLen;
				
				long startCellC = startCellCol;
				long startCellR = startCellRow;

				land->getCellPos(startCellR,startCellC,currentPos);
				float localElev = (worldUnitsPerMeter * 4.0f * (float)GameMap->getLocalHeight(startCellR,startCellC));
				currentPos.z += localElev;
				
				if (startHeight+startLocal < currentPos.z)
				{
#ifdef LAB_ONLY
		if (drawTerrainGrid)
		{
			Stuff::Vector3D realStart = startPos;
			Stuff::Vector4D lineStart, lineEnd;
			// [PROJECTZ:DebugOnly id=debug_los_line_a0]
			eye->projectForDebugOverlay(realStart,lineStart);
			// [PROJECTZ:DebugOnly id=debug_los_line_a1]
			eye->projectForDebugOverlay(endPos,lineEnd);

			debugLines[currentLineElement++] = new LineElement(lineStart,lineEnd,SD_RED,NULL,-1);
		}
#endif
					return false;
				}

				if (extRad > Stuff::SMALL)
				{
					dist.Subtract(endPos,currentPos);
					remainingDist = dist.GetApproximateLength();
					if (remainingDist < extRad)
						break;
				}
			}
		}

#ifdef LAB_ONLY
		if (drawTerrainGrid)
		{
			Stuff::Vector3D realStart = startPos;
			Stuff::Vector4D lineStart, lineEnd;
			// [PROJECTZ:DebugOnly id=debug_los_line_b0]
			eye->projectForDebugOverlay(realStart,lineStart);
			// [PROJECTZ:DebugOnly id=debug_los_line_b1]
			eye->projectForDebugOverlay(endPos,lineEnd);

			debugLines[currentLineElement++] = new LineElement(lineStart,lineEnd,SD_GREEN,NULL,-1);
		}
#endif
	}


	return true;
}

#else

#define ACCURACY_ADJUST		1.5f
const float HALF_CELL_DIST	= (128.0f / 6.0f);
//---------------------------------------------------------------------------
bool Team::lineOfSight (float startLocal, long mCellRow, long mCellCol, float endLocal, long tCellRow, long tCellCol, long teamId, float extRad, float startExtRad, bool checkVisibleBits)
{
	#ifdef LAB_ONLY
	__int64 x=GetCycles();
	#endif
	
	//-----------------------------------------------------
	// Once we allow teams to have alliances (for contacts,
	// etc.), simply set all nec. team bits in this mask...

	//TILE HACK...
	long tileRow = tCellRow / 3;
	long tileCol = tCellCol / 3;

	if ((teamId < 0) || (teamId >= MAX_TEAMS))	//Not on any team.  It can see everything!
		return true;


	if (useRealLOS)
	{
		//------------------------------------------------------------------------------------------
		// Within magic radius.  Check REAL LOS now.
		// Check is really simple.
		// Find deltaCellRow and deltaCellCol and iterate over them from source to dest.
		// If the magic line ever goes BELOW the terrainElevation PLUS localElevation return false.
		Stuff::Vector3D startPos, endPos;
		startPos.Zero();
		endPos.Zero();
		
		land->getCellPos(tCellRow,tCellCol,endPos);
		land->getCellPos(mCellRow,mCellCol,startPos);
		startPos.z += startLocal;
		endPos.z += endLocal;

		Stuff::Vector3D deltaCellVec;
		deltaCellVec.y = tCellRow - mCellRow;
		deltaCellVec.x = tCellCol - mCellCol;
		deltaCellVec.z = 0.0f;
		float startHeight = startPos.z;
		
		float length = deltaCellVec.GetApproximateLength();
		length *= ACCURACY_ADJUST;
		
		if (length > Stuff::SMALL)
		{
			float colLength = (endPos.x - startPos.x) / length;
			float rowLength = (endPos.y - startPos.y) / length;
			float heightLen = (endPos.z - startPos.z) / (length + ACCURACY_ADJUST);
			
			Stuff::Vector3D currentPos = startPos;
			currentPos.z = land->getTerrainElevation(currentPos);
			long maxDistIter = (length - 0.5f);
			long maxTrees = 0;

			Stuff::Vector3D dist;
			dist.Subtract(endPos,currentPos);
			
			float remainingDist = dist.GetApproximateLength();
			bool checkExtent = (extRad > Stuff::SMALL);
			bool checkStart = (startExtRad > Stuff::SMALL);
			extRad += HALF_CELL_DIST;
			for (long distIter = 0;distIter < maxDistIter;distIter++)
			{
				bool outsideStartRadius = true;
				if (checkStart)
				{
					Stuff::Vector3D distance;
					distance.Subtract(currentPos,startPos);
					distance.z = 0.0f;
					float dist = distance.GetApproximateLength();
					if (dist <= startExtRad)
						outsideStartRadius = false;
				}

				startHeight += heightLen;

				int curCellRow, curCellCol;
				land->worldToCell(currentPos,curCellRow, curCellCol);

				float localElev = (worldUnitsPerMeter * 4.0f * (float)GameMap->getLocalHeight(curCellRow,curCellCol)); 
				float thisHeight = currentPos.z + localElev;

				//First, check if we are now inside the extent radius of the thing we are calcing LOS to.
				// If we are and we haven't returned false since we're here, we can see it!!!!
				if (checkExtent)
				{
					dist.Subtract(endPos,currentPos);
					remainingDist = dist.GetApproximateLength();
					if (remainingDist <= extRad)
						break;
				}

				if (outsideStartRadius && (startHeight < thisHeight))
				{
					bool isTree = false;
					if (GameMap->getForest(curCellRow,curCellCol))
					{
						maxTrees++;
						isTree = true;
					}

					if (!isTree || (maxTrees >= MaxTreeLOSCellBlock))
					{
		
#ifdef LAB_ONLY
						if (drawTerrainGrid)
						{
							Stuff::Vector3D realStart = startPos;
							Stuff::Vector4D lineStart, lineEnd;
							// [PROJECTZ:DebugOnly id=debug_los_line_c0]
							eye->projectForDebugOverlay(realStart,lineStart);
							// [PROJECTZ:DebugOnly id=debug_los_line_c1]
							eye->projectForDebugOverlay(endPos,lineEnd);

							debugLines[currentLineElement++] = new LineElement(lineStart,lineEnd,SD_RED,NULL,-1);
						}
#endif
						return false;
					}
				}

				currentPos.x += colLength;
				currentPos.y += rowLength;
				currentPos.z = land->getTerrainElevation(currentPos);
			}
		}

#ifdef LAB_ONLY
		if (drawTerrainGrid)
		{
			Stuff::Vector3D realStart = startPos;
			Stuff::Vector4D lineStart, lineEnd;
			// [PROJECTZ:DebugOnly id=debug_los_line_d0]
			eye->projectForDebugOverlay(realStart,lineStart);
			// [PROJECTZ:DebugOnly id=debug_los_line_d1]
			eye->projectForDebugOverlay(endPos,lineEnd);

			debugLines[currentLineElement++] = new LineElement(lineStart,lineEnd,SD_GREEN,NULL,-1);
		}
#endif
	}


	return true;
}
#endif

//---------------------------------------------------------------------------
bool Team::lineOfSight (Stuff::Vector3D position, Stuff::Vector3D targetPosition, long teamId, float extRad, float startExtRad, bool checkVisibleBits) 
{
	int posCellR, posCellC;
	int tarCellR, tarCellC;
	land->worldToCell(position, posCellR, posCellC);
	land->worldToCell(targetPosition, tarCellR, tarCellC);
	
	float elev = land->getTerrainElevation(position);
	float localStart = position.z - elev;

	elev = land->getTerrainElevation(targetPosition);
	float localEnd = targetPosition.z - elev;
	
	return(lineOfSight(localStart,posCellR, posCellC, localEnd, tarCellR, tarCellC,teamId,extRad, startExtRad, checkVisibleBits));
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
long Team::Save (PacketFilePtr file, long packetNum)
{
	TeamStaticData staticData;
	staticData.numTeams = numTeams;
	staticData.homeTeamId = home->getId();
	memcpy(staticData.relations,relations,sizeof(char) * MAX_TEAMS * MAX_TEAMS);
	memcpy(staticData.noPain,noPain,sizeof(bool) * MAX_TEAMS);

	file->writePacket(packetNum,(MemoryPtr)&staticData,sizeof(TeamStaticData),STORAGE_TYPE_RAW);
	packetNum++;

	for (long i=0;i<numTeams;i++)
	{
		TeamData data;
		data.id = teams[i]->getId();
		data.rosterSize = teams[i]->rosterSize;
		memcpy(data.roster,teams[i]->roster,sizeof(GameObjectWatchID) * MAX_MOVERS_PER_TEAM);

		file->writePacket(packetNum,(MemoryPtr)&data,sizeof(TeamData),STORAGE_TYPE_RAW);
		packetNum++;
	}

	return packetNum;
}

//---------------------------------------------------------------------------
long Team::Load (PacketFilePtr file, long packetNum)
{
	TeamStaticData staticData;

	file->readPacket(packetNum,(MemoryPtr)&staticData);
	packetNum++;

	numTeams = staticData.numTeams;
	memcpy(relations,staticData.relations,sizeof(char) * MAX_TEAMS * MAX_TEAMS);
	memcpy(noPain,staticData.noPain,sizeof(bool) * MAX_TEAMS);

	for (long i=0;i<numTeams;i++)
	{
		TeamData data;
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

//***************************************************************************

