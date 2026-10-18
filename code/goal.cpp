//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef GOAL_H
#include"goal.h"
#endif

#ifndef GAMEOBJ_H
#include"gameobj.h"
#endif

#ifndef TERROBJ_H
#include"terrobj.h"
#endif

#ifndef GATE_H
#include"gate.h"
#endif

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef BLDNG_H
#include"bldng.h"
#endif

#include"heap.h"
#include"mclib.h"
#include <new>

extern UserHeapPtr missionHeap;

//***************************************************************************

long adjCell[4][2] = {
	{-1, 0},
	{0, 1},
	{1, 0},
	{0, -1}
};

long recurseCount = 0;

//***************************************************************************

void* GoalObject::operator new (size_t ourSize) {

	void *result = missionHeap->Malloc(ourSize);
	return(result);
}

//---------------------------------------------------------------------------

void GoalObject::operator delete (void* us) {

	missionHeap->Free(us);
}

//---------------------------------------------------------------------------

void GoalObject::init (void) {

	used = false;
	type = GOAL_OBJECT;
	id = -1;
	name[0] = '\0';
	links = nullptr;
	controller = nullptr;
	info.object.WID = 0;
	next = nullptr;
	prev = nullptr;
}

//---------------------------------------------------------------------------

void GoalObject::initObject (char* name, GameObjectPtr obj) {
}

//---------------------------------------------------------------------------

void GoalObject::initRegion (char* name, long minRow, long minCol, long maxRow, long maxCol) {

	init();
	used = true;
	type = GOAL_REGION;
	strcpy(name, "MAP");
	info.region.minRow = minRow;
}

//---------------------------------------------------------------------------

void GoalObject::destroy (void) {

}

//---------------------------------------------------------------------------

void GoalObject::addLink (GoalObjectPtr gobject, GoalLinkType type) {

	GoalLinkPtr newLink = (GoalLink*)missionHeap->Malloc(sizeof(GoalLink));
	
}

//---------------------------------------------------------------------------

void GoalObject::addController (GoalObjectPtr gobject) {
}

//***************************************************************************
// ACTION GOAL MANAGER
//***************************************************************************

//#ifdef USE_REGION_MAP

void* GoalManager::operator new (size_t ourSize) {

	void *result = missionHeap->Malloc(ourSize);
	return(result);
}

//---------------------------------------------------------------------------

void GoalManager::operator delete (void* us) {

	missionHeap->Free(us);
}

//---------------------------------------------------------------------------

void GoalManager::init (void) {

	numGoalObjects = 0;
	goalObjectPoolSize = 0;
	goalObjects = nullptr;
	goalObjectPool = nullptr;

#ifdef USE_REGION_MAP
	for (long r = 0; r < GameMap->height; r++)
		for (long c = 0; c < GameMap->width; c++)
			regionMap[r][c] = -1;
#endif
	numRegions = 0;

	fillStack = nullptr;
	fillStackIndex = 0;
}

//---------------------------------------------------------------------------

void GoalManager::destroy (void) {

	if (goalObjectPool) {
		for (long i = 0; i < goalObjectPoolSize; ++i)
			goalObjectPool[i].~GoalObject();
		missionHeap->Free(goalObjectPool);
		goalObjectPool = nullptr;
	}
	numGoalObjects = 0;
	goalObjectPoolSize = 0;
}

//---------------------------------------------------------------------------

void GoalManager::clear (void) {

	goalObjects = nullptr;
	numGoalObjects = 0;
	for (long i = 0; i < goalObjectPoolSize; i++)
		goalObjectPool[i].used = false;

#ifdef USE_REGION_MAP
	for (long r = 0; r < GameMap->height; r++)
		for (long c = 0; c < GameMap->width; c++)
			regionMap[r][c] = -1;
#endif
	numRegions = 0;
}

//---------------------------------------------------------------------------

GoalObjectPtr GoalManager::newGoalObject (void) {

	GoalObjectPtr goalObject = nullptr;
	for (long i = 0; i < goalObjectPoolSize; i++)
		if (!goalObjectPool[i].used) {
			goalObjectPool[i].used = true;
			goalObject = &goalObjectPool[i];
			break;
		}

	if (!goalObject)
		Fatal(0, " GoalManager.newGoalObject: no more GoalObjects allowed ");
	return(goalObject);
}

//---------------------------------------------------------------------------

void GoalManager::setup (long poolSize) {

	goalObjectPoolSize = poolSize;
	if (goalObjectPoolSize  < 10)
		Fatal(0, " GoalManager.setup: goalObjectPoolSize must be greater than 10 ");

	goalObjectPool = static_cast<GoalObjectPtr>(missionHeap->Malloc(sizeof(GoalObject) * goalObjectPoolSize));
	for (long i = 0; i < goalObjectPoolSize; ++i)
		::new (static_cast<void*>(&goalObjectPool[i])) GoalObject;
	clear();
}

//---------------------------------------------------------------------------

bool GoalManager::fillWallGateRegion (long row, long col, long region) {

	recurseCount++;

	//----------------------------------------------------------------------
	// It is assumed that the bridge is erected over non-passable terrain...
	if ((row < 0) || (row >= GameMap->height) || (col < 0) || (col >= GameMap->width))
		return(false);

	if (!GameMap->getWall(row, col) && !GameMap->getGate(row, col))
		return(false);

	regionMap[row][col] = region;
	for (long dir = 0; dir < 4; dir ++) {
		long adjR = row + adjCell[dir][0];
		long adjC = col + adjCell[dir][1];
		if ((adjR >= 0) && (adjR < GameMap->height) && (adjC >= 0) && (adjC < GameMap->width))
			if (regionMap[adjR][adjC] == -1)
				fillWallGateRegion(adjR, adjC, region);
	}
	
	return(true);
}

//------------------------------------------------------------------------------------------

bool GoalManager::fillRegion (long row, long col, long region) {

#if 1

//	long overlay = GameMap->getOverlay(row, col);
//	if ((overlay == OVERLAY_WATER_BRIDGE_EW) || (overlay == OVERLAY_WATER_BRIDGE_NS))
//		return(false);
		
	if (GameMap->getWall(row, col) || GameMap->getGate(row, col))
		return(false);
		
	if (!GameMap->getPassable(row, col)) {
		regionMap[row][col] = -2;
		return(false);
	}

	fillStack[fillStackIndex++] = row;
	fillStack[fillStackIndex++] = col;

	while (fillStackIndex > 0) {
		//--------------------------------
		// Pop 'em in the reverse order...
		long col = fillStack[--fillStackIndex];
		long row = fillStack[--fillStackIndex];

		bool filling = true;
		
		if ((row < 0) || (row >= GameMap->height) || (col < 0) || (col >= GameMap->width))
			filling = false;
		
//		long overlay = GameMap->getOverlay(row, col);
//		if ((overlay == OVERLAY_WATER_BRIDGE_EW) || (overlay == OVERLAY_WATER_BRIDGE_NS))
//			filling = false;
		
		if (GameMap->getWall(row, col) || GameMap->getGate(row, col))
			filling = false;
		
		if (!GameMap->getPassable(row, col)) {
			regionMap[row][col] = -2;
			filling = false;
		}
		
		if (filling) {
			regionMap[row][col] = region;
			for (long dir = 0; dir < 4; dir ++) {
				long adjR = row + adjCell[dir][0];
				long adjC = col + adjCell[dir][1];
				if ((adjR >= 0) && (adjR < GameMap->height) && (adjC >= 0) && (adjC < GameMap->width))
					if (regionMap[adjR][adjC] == -1) {
						//--------------------------------------------
						// Cell hasn't been visited yet, so push it...
						fillStack[fillStackIndex++] = adjR;
						fillStack[fillStackIndex++] = adjC;
						//----------------------------------------------------------------
						// Mark the cell as on the stack (so we do not push it again if we
						// hit it before popping it)...
						regionMap[adjR][adjC] = -3;
					}
			}
		}
	}
	return(true);
#else
	if ((row < 0) || (row >= GameMap->height) || (col < 0) || (col >= GameMap->width))
		return(false);

	//----------------------------------------------------------------------
	// If we hit a bridge cell, politely stop expanding this area into it...
	long overlay = GameMap->getOverlay(row, col);
	if ((overlay == OVERLAY_WATER_BRIDGE_EW) || (overlay == OVERLAY_WATER_BRIDGE_NS))
		return(false);

	if (GameMap->getWall(row, col) || GameMap->getGate(row, col))
		return(false);

	if (!GameMap->getPassable(row, col)) {
		regionMap[row][col] = -2;
		return(false);
	}

	regionMap[row][col] = region;
	for (long dir = 0; dir < 4; dir ++) {
		long adjR = row + adjCell[dir][0];
		long adjC = col + adjCell[dir][1];
		if ((adjR >= 0) && (adjR < GameMap->height) && (adjC >= 0) && (adjC < GameMap->width))
			if (regionMap[adjR][adjC] == -1)
				fillRegion(adjR, adjC, region);
	}
	
	return(true);
#endif
}

//------------------------------------------------------------------------------------------

void GoalManager::calcRegions (void) {

	// GameMap can be NULL for missions loaded without MOVE data (blank-synthesis
	// fallback gives all-passable movement but no region info).  Skip gracefully
	// rather than crashing — goal pathfinding will be degraded but not fatal.
	if (!GameMap)
		return;

	//----------------------------------------------------------------------
	// This is the same method used in GlobalMap::calcAreas, so see notes...
	for (long r = 0; r < GameMap->height; r++)
		for (long c = 0; c < GameMap->width; c++)
			if (regionMap[r][c] == -1) {
				recurseCount = 0;
				#ifdef USE_OVERLAYS
				long overlay = GameMap->getOverlay(r, c);
				if (overlay == OVERLAY_WATER_BRIDGE_NS) {
					if (fillNorthSouthBridgeArea(r, c, numAreas))
						numAreas++;
					}
				else if (overlay == OVERLAY_WATER_BRIDGE_EW) {
					if (fillEastWestBridgeArea(r, c, numAreas))
						numAreas++;
					}
				#endif
				if (GameMap->getWall(r, c) || GameMap->getGate(r, c)) {
					if (fillWallGateRegion(r, c, numRegions))
						numRegions++;
					}
				else if (fillRegion(r, c, numRegions))
					numRegions++;
			}
}

//------------------------------------------------------------------------------------------

void GoalManager::build (void) {
	
	//For temps, pull 'em off the windows heap.  IT can resize.  OURS cannot!!!!
	fillStack = (short*)malloc(FILL_STACK_SIZE * sizeof(short));
	gosASSERT(fillStack != nullptr);
	fillStackIndex = 0;

	//--------------------------------
	// First, get the list of walls...
	GameObjectPtr wallObjects[MAX_WALL_OBJECTS];
	long numWalls = ObjectManager->getSpecificObjects(BUILDING, BUILDING_SUBTYPE_WALL, wallObjects, MAX_WALL_OBJECTS);
/*	short cellList[MAX_CELL_COORDS];
	for (long i = 0; i < numWalls; i++) {
		cellList[0] = MAX_CELL_COORDS;
		long numCells = wallObjects[i]->appearance->markMoveMap(true, NULL, false, cellList);
		for (long j = 0; j < numCells; j++)
			GameMap->setWall(cellList[j*2], cellList[j*2+1], true);
	}

	//--------------------------------------------------
	// Close all gates, before calcing the region map...
	for (i = 0; i < ObjectManager->getNumGates(); i++) {
		GatePtr gate = ObjectManager->getGate(i);
		gate->getAppearance()->markMoveMap(false, NULL);
	}
*/
//	calcRegions();


/*
	//--------------------------------------
	// Now, open the gates (for the game)...
	for (i = 0; i < ObjectManager->getNumGates(); i++) {
		GatePtr gate = ObjectManager->getGate(i);
		gate->getAppearance()->markMoveMap(true, NULL);
	}
*/
	free(fillStack);
	fillStack = nullptr;
	fillStackIndex = 0;
}

//---------------------------------------------------------------------------

/*
long GoalManager::setControl (ObstaclePtr controller, ObstaclePtr controllee) {

	if (controller && controllee) {
		controllee->parent = controller;
		controllee->prev = NULL;
		controllee->next = controller->controls;
		controllee->controls = NULL;
		controller->controls->prev = controllee;
		controller->controls = controllee;
	}

	return(0);
}
*/
//---------------------------------------------------------------------------

GoalObjectPtr GoalManager::addRegion (GoalObjectPtr parent, GoalLinkType linkType, char* name, long minRow, long minCol, long maxRow, long maxCol) {

	GoalObjectPtr newRegion = newGoalObject();
	newRegion->initRegion(name, minRow, minCol, maxRow, maxCol);
	if (parent)
		parent->addLink(newRegion, linkType);
	else
		goalObjects = newRegion;
	return(newRegion);
}

//---------------------------------------------------------------------------

GoalObjectPtr GoalManager::addObject (GoalObjectPtr parent, GoalLinkType linkType, char* name, GameObjectPtr object) {

	GoalObjectPtr newObject = newGoalObject();
	newObject->initObject(name, object);
	if (parent)
		parent->addLink(newObject, linkType);
	else
		goalObjects = newObject;
	return(newObject);
}

//---------------------------------------------------------------------------

/*
void GoalManager::setControlMap (long row, long col, GoalObjectPtr controller) {

	controlMap[row][col] = controller->id;
}
*/
//---------------------------------------------------------------------------

GoalObjectPtr GoalManager::calcGoal (int startCell[2], int goalCell[2]) {

	//No Warnings!
	//ObstaclePtr startObstacle = &obstaclePool[controlMap[startCell[0]][startCell[1]]];
	//ObstaclePtr goalObstacle = &obstaclePool[controlMap[goalCell[0]][goalCell[1]]];

	/* DO SEARCH HERE */

	return(nullptr);
}

//---------------------------------------------------------------------------

GoalObjectPtr GoalManager::calcGoal (GameObjectPtr attacker, GameObjectPtr target) {

	return(nullptr);
}

//---------------------------------------------------------------------------

GoalObjectPtr GoalManager::calcGoal (GameObjectPtr attacker, Stuff::Vector3D location) {

	return(nullptr);
}

//---------------------------------------------------------------------------

GoalObjectPtr GoalManager::calcGoal (Stuff::Vector3D start, Stuff::Vector3D location) {

	int startCell[2], locationCell[2];
	land->worldToCell(start, startCell[0], startCell[1]);
	land->worldToCell(location, locationCell[0], locationCell[1]);
	return(calcGoal(startCell, locationCell));
}

//***************************************************************************
