//	move.cpp - This file contains the Movement\Pathfinding routines
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "move_internal.h"


int GlobalMap::minRow = 0;
int GlobalMap::maxRow = 0;
int GlobalMap::minCol = 0;
int GlobalMap::maxCol = 0;
GameLogPtr GlobalMap::log = nullptr;
bool GlobalMap::logEnabled = false;
// GLOBAL vars
bool ZeroHPrime = false;
bool CullPathAreas = false;

const float LOSincrement = 0.33f;		// based upon cell size
const float LOFincrement = 0.33f;		// based upon cell size
const float LOSensorIncrement = 2.0;	// based upon cell size (big, since we care only about tiles)
bool debugMoveMap = false;
bool ClearBridgeTiles = false;

long RamObjectWID = 0;

bool JumpOnBlocked = false;
bool FindingEscapePath = false;
bool BlockWallTiles = true;
MissionMapPtr GameMap = nullptr;
GlobalMapPtr GlobalMoveMap[3] = {nullptr, nullptr, nullptr};

long DebugMovePathType = 0;

long SimpleMovePathRange = 21;

char reverseDir[NUM_DIRECTIONS] = {4, 5, 6, 7, 0, 1, 2, 3};
char rowShift[NUM_DIRECTIONS] = {-1, -1, 0, 1, 1, 1, 0, -1};
char colShift[NUM_DIRECTIONS] = {0, 1, 1, 1, 0, -1, -1, -1};
signed char cellShift[NUM_CELL_OFFSETS * 2] = {
	-1, 0,
	-1, 1,
	0, 1,
	1, 1,
	1, 0,
	1, -1,
	0, -1,
	-1, -1,

	-2, 0,
	-2, 2,
	0, 2,
	2, 2,
	2, 0,
	2, -2,
	0, -2,
	-2, -2,

	-3, 0,
	-3, 3,
	0, 3,
	3, 3,
	3, 0,
	3, -3,
	0, -3,
	-3, -3,

	-3, 2,
	-2, 3,
	2, 3,
	3, 2,
	3, -2,
	2, -3,
	-2, -3,
	-3, -2,

	-4, 0,
	-4, 4,
	0, 4,
	4, 4,
	4, 0,
	4, -4,
	0, -4,
	-4, -4,

	-5, 0,
	-5, 5,
	0, 5,
	5, 5,
	5, 0,
	5, -5,
	0, -5,
	-5, -5,

	-6, 0,
	-6, 6,
	0, 6,
	6, 6,
	6, 0,
	6, -6,
	0, -6,
	-6, -6,

	-7, 0,
	-7, 7,
	0, 7,
	7, 7,
	7, 0,
	7, -7,
	0, -7,
	-7, -7,

	-8, 0,
	-8, 8,
	0, 8,
	8, 8,
	8, 0,
	8, -8,
	0, -8,
	-8, -8,

	-9, 0,
	-9, 9,
	0, 9,
	9, 9,
	9, 0,
	9, -9,
	0, -9,
	-9, -9,

	-10, 0,
	-10, 10,
	0, 10,
	10, 10,
	10, 0,
	10, -10,
	0, -10,
	-10, -10,

	-11, 0,
	-11, 11,
	0, 11,
	11, 11,
	11, 0,
	11, -11,
	0, -11,
	-11, -11,

	-12, 0,
	-12, 12,
	0, 12,
	12, 12,
	12, 0,
	12, -12,
	0, -12,
	-12, -12,

	-13, 0,
	-13, 13,
	0, 13,
	13, 13,
	13, 0,
	13, -13,
	0, -13,
	-13, -13,

	-14, 0,
	-14, 14,
	0, 14,
	14, 14,
	14, 0,
	14, -14,
	0, -14,
	-14, -14,

	-15, 0,
	-15, 15,
	0, 15,
	15, 15,
	15, 0,
	15, -15,
	0, -15,
	-15, -15
};

float cellShiftDistance[NUM_CELL_OFFSETS];
char reverseShift[NUM_CELL_OFFSETS] = {
	4, 5, 6, 7, 0, 1, 2, 3,
	12, 13, 14, 15, 8, 9, 10, 11,
	20, 21, 22, 23, 16, 17, 18, 19,
	28, 29, 30, 31, 24, 25, 26, 27,
	36, 37, 38, 39, 32, 33, 34, 35,
	44, 45, 46, 47, 40, 41, 42, 43,
	52, 53, 54, 55, 48, 49, 50, 51,
	60, 61, 62, 63, 56, 57, 58, 59,
	68, 69, 70, 71, 64, 65, 66, 67,
	76, 77, 78, 79, 72, 73, 74, 75,
	84, 85, 86, 87, 80, 81, 82, 83,
	92, 93, 94, 95, 88, 89, 90, 91,
	100, 101, 102, 103, 96, 97, 98, 99,
	108,109,110,111,104,105,106,107,
	116,117,118,119,112,113,114,115,
	124,125,126,127,120,121,122,123
};

long tileMulMAPCELL_DIM[MAX_MAP_CELL_WIDTH];
float MapCellDiagonal = 0.0;
float HalfMapCell = 0.0;
float VerticesMapSideDivTwo = 0.0;
float MetersMapSideDivTwo = 0.0;

bool IsDiagonalStep[NUM_CELL_OFFSETS] = {
	false, true, false, true, false, true, false, true,
	false, false, false, false, false, false, false, false,
	false, false, false, false, false, false, false, false,
	false, false, false, false, false, false, false, false,
	false, false, false, false, false, false, false, false,
	false, false, false, false, false, false, false, false,
	false, false, false, false, false, false, false, false,
	false, false, false, false, false, false, false, false,
	false, false, false, false, false, false, false, false,
	false, false, false, false, false, false, false, false,
	false, false, false, false, false, false, false, false,
	false, false, false, false, false, false, false, false,
	false, false, false, false, false, false, false, false,
	false, false, false, false, false, false, false, false,
	false, false, false, false, false, false, false, false,
	false, false, false, false, false, false, false, false
};

// Only pattern 2 is used now.
char mineLayout[4][MAPCELL_DIM * MAPCELL_DIM] = {
	{0,0,0,0,0,0,0,0,0},
	{0,0,0,0,0,0,0,0,0},
	{1,0,1,0,1,0,1,0,1},
	{0,0,0,0,0,0,0,0,0}
};

char TeamRelations[MAX_TEAMS][MAX_TEAMS] = {
	{0, 2, RELATION_NEUTRAL, 2, 2, 2, 2, 2},
	{2, 0, 2, 2, 2, 2, 2, 2},
	{RELATION_NEUTRAL, 2, 0, 2, 2, 2, 2, 2},
	{2, 2, 2, 0, 2, 2, 2, 2},
	{2, 2, 2, 2, 0, 2, 2, 2},
	{2, 2, 2, 2, 2, 0, 2, 2},
	{2, 2, 2, 2, 2, 2, 0, 2},
	{2, 2, 2, 2, 2, 2, 2, 0}
};

bool GoalIsDoor = false;
long numNodesVisited = 0;
long topOpenNodes = 0;
long MaxHPrime = 1000;
bool PreserveMapTiles = false;

MoveMapPtr PathFindMap[2] = {nullptr, nullptr};

float MoveMap::distanceFloat[DISTANCE_TABLE_DIM][DISTANCE_TABLE_DIM];
int MoveMap::distanceInt[DISTANCE_TABLE_DIM][DISTANCE_TABLE_DIM];
int MoveMap::forestCost = 50;

Stuff::Vector3D relativePositionToPoint (Stuff::Vector3D point, float angle, float distance, unsigned long flags) {

	// Note that the angle should be -180 <= angle <= 180, and
	// the distance is in meters...

	Stuff::Vector3D curPos;
	curPos.x = point.x;
	curPos.y = point.y;
	curPos.z = 0.0;

	distance *= -worldUnitsPerMeter;

	Stuff::Vector3D shiftVect;
	shiftVect.x = 0.0;
	shiftVect.y = 1.0;
	shiftVect.z = 0.0;

	Rotate(shiftVect,angle);

	shiftVect *= distance;
	Stuff::Vector3D relPos;
	relPos.Zero();
	relPos.Add(curPos,shiftVect);

	Stuff::Vector3D start2d;
	Stuff::Vector3D goal2d;
	Stuff::Vector3D deltaVector;
	if (flags & RELPOS_FLAG_PASSABLE_START) {
		start2d  = curPos;
		goal2d = relPos;
		}
	else {
		start2d = relPos;
		goal2d = curPos;
	}

	deltaVector.Subtract(goal2d,start2d);

	// First, we need to calc the delta vector--how much we extend
	// the ray everytime we check the map cell for clear placement.
	deltaVector.Normalize(deltaVector);
	deltaVector *= Terrain::halfWorldUnitsPerCell;
	if (deltaVector.GetLength() == 0.0)
		return(curPos);

	// Determine the max length the ray must be cast...
	float maxLength = distance_from(goal2d,start2d);

	// We'll start at the target, and if it's blocked, we'll move
	// toward our start location, looking for the first valid/open
	// cell...
	Stuff::Vector3D curPoint = start2d;
	Stuff::Vector3D curRay;
	curRay.Zero();
	float rayLength = 0.0;

	int cellR, cellC;
	Stuff::Vector3D curPoint3d(curPoint.x, curPoint.y, 0.0);
	land->worldToCell(curPoint3d, cellR, cellC);
	unsigned int cellClear = GameMap->getPassable(cellR, cellC);

	Stuff::Vector3D lastGoodPoint = curPoint;
	if (flags & RELPOS_FLAG_PASSABLE_START)
		while (cellClear && (rayLength < maxLength)) {
			lastGoodPoint = curPoint;
			curPoint += deltaVector;
			curRay.Subtract(curPoint,start2d);
			rayLength = curRay.GetLength();

			curPoint3d.x = curPoint.x;
			curPoint3d.x = curPoint.y;
			curPoint3d.z = 0.0;

			land->worldToCell(curPoint3d, cellR, cellC);
			cellClear = GameMap->getPassable(cellR, cellC);
		}
	else
		while (!cellClear && (rayLength < maxLength)) {
			lastGoodPoint = curPoint;
			curPoint += deltaVector;
			curRay.Subtract(curPoint,start2d);
			rayLength = curRay.GetLength();

			curPoint3d.x = curPoint.x;
			curPoint3d.x = curPoint.y;
			curPoint3d.z = 0.0;

			land->worldToCell(curPoint3d, cellR, cellC);
			cellClear = GameMap->getPassable(cellR, cellC);
		}

	// lastGoodPoint MUST be on Map or BOOM!!
	// back on map if necessary
	float maxMap = Terrain::worldUnitsMapSide / 2.0;
	float safety = maxMap - Terrain::worldUnitsPerVertex;
	bool offMap = false;

	if (lastGoodPoint.x < -safety)
	{
		lastGoodPoint.x = -safety;
		offMap = true;
	}

	if (lastGoodPoint.x > safety)
	{
		lastGoodPoint.x = safety;
		offMap = true;
	}

	if (lastGoodPoint.y < -safety)
	{
		lastGoodPoint.y = -safety;
		offMap = true;
	}

	if (lastGoodPoint.y > safety)
	{
		lastGoodPoint.y = safety;
		offMap = true;
	}
	
	curPoint3d.x = lastGoodPoint.x;
	curPoint3d.x = lastGoodPoint.y;
	curPoint3d.z = 0.0;

	curPoint3d.z = land->getTerrainElevation(curPoint3d);
	return(curPoint3d);
}

// MISSION MAP class (new Game Map class)

// PATHFINDING-SOLVER-ISOLATION-1: one-time build of the cellShiftDistance[]
// table. Historically this was lazily initialized inside the solver via three
// `static bool setTable` guards (idempotent, same values every time). Hoisted
// here so it is done ONCE at init, off any solve, before any future worker.
// Idempotent + value-identical to the lazy path => OFF byte-identical.
static void buildCellShiftDistanceTable (void) {
	float cellLength = Terrain::worldUnitsPerCell * metersPerWorldUnit;
	for (int i = 0; i < NUM_CELL_OFFSETS; i++)
		cellShiftDistance[i] = agsqrt( cellShift[i * 2], cellShift[i * 2 + 1] ) * cellLength;
}

void MOVE_init (long moveRange) {

	if (PathFindMap[SECTOR_PATHMAP])
		Fatal(0, " MOVE_Init: Already called this! ");

	// PATHFINDING-SOLVER-ISOLATION-1: force the one-time table init here so the
	// lazy in-solve `setTable` guards are never the first writer (no per-solve
	// race later once solves may run off-thread).
	buildCellShiftDistanceTable();

	PathFindMap[SECTOR_PATHMAP] = new MoveMap;
	if (!PathFindMap[SECTOR_PATHMAP])
		Fatal(0, " MOVE_Init: Cannot initialize PathFindMap ");
	PathFindMap[SECTOR_PATHMAP]->init(SECTOR_DIM*2, SECTOR_DIM*2);

	PathFindMap[SIMPLE_PATHMAP] = new MoveMap;
	if (!PathFindMap[SIMPLE_PATHMAP])
		Fatal(0, " MOVE_Init: Cannot initialize PathFindMap ");
	SimpleMovePathRange = moveRange;
	if (SimpleMovePathRange <= 20)
		Fatal(0, " MOVE_Init: MoveRange TOO SMALL. Go see Glenn. ");
	PathFindMap[SIMPLE_PATHMAP]->init(SimpleMovePathRange * 2 + 1, SimpleMovePathRange * 2 + 1);
}

bool EditorSave = false;
long tempNumSpecialAreas = 0;
GameObjectFootPrint* tempSpecialAreaFootPrints = nullptr;

void MOVE_buildData (long height, long width, MissionMapCellInfo* mapData, long numSpecialAreas, GameObjectFootPrint* specialAreaFootPrints) {

	printf("[MOVE_BD] enter h=%ld w=%ld mapData=%p specials=%ld\n", height, width, (void*)mapData, numSpecialAreas); fflush(stdout);

	EditorSave = true;

	if (GameMap)
		delete GameMap;
	GameMap = new MissionMap;
	if (!GameMap) { printf("[MOVE_BD] FATAL: new MissionMap returned NULL\n"); fflush(stdout); return; }
	printf("[MOVE_BD] MissionMap allocated OK, calling init(%ld,%ld)\n", height, width); fflush(stdout);

	if (mapData)
		GameMap->init(height, width, 0, mapData);
	else
		GameMap->init( height, width );

	if (!GameMap->map) { printf("[MOVE_BD] FATAL: GameMap->map is NULL after init — systemHeap exhausted?\n"); fflush(stdout); return; }
	printf("[MOVE_BD] GameMap->init OK map=%p\n", (void*)GameMap->map); fflush(stdout);

	long numOffMap = 0;
	for (long r = 0; r < height; r++)
		for (long c = 0; c < width; c++) {
			Stuff::Vector3D worldPos;
			land->cellToWorld(r, c, worldPos);
			bool set = (land->IsEditorSelectTerrainPosition(worldPos) && !land->IsGameSelectTerrainPosition(worldPos));
			GameMap->setOffMap(r, c, set);
			if (set)
				numOffMap++;
		}

	printf("[MOVE_BD] offMap loop done numOffMap=%ld\n", numOffMap); fflush(stdout);

	if (tempSpecialAreaFootPrints) {
		systemHeap->Free(tempSpecialAreaFootPrints);
		tempSpecialAreaFootPrints = nullptr;
	}
	tempNumSpecialAreas = numSpecialAreas;
	if (tempNumSpecialAreas) {
		tempSpecialAreaFootPrints = (GameObjectFootPrint*)systemHeap->Malloc(sizeof(GameObjectFootPrint) * tempNumSpecialAreas);
		memcpy(tempSpecialAreaFootPrints, specialAreaFootPrints, sizeof(GameObjectFootPrint) * tempNumSpecialAreas);
	}
	if (!PathFindMap[SECTOR_PATHMAP])
		Fatal(0, " MOVE_BuildData: Must call MOVE_Init()! ");

	if (GlobalMoveMap[0])
	{
		delete GlobalMoveMap[0];
		GlobalMoveMap[0] = nullptr;
	}

	if (GlobalMoveMap[1])
	{
		delete GlobalMoveMap[1];
		GlobalMoveMap[1] = nullptr;
	}

	if (GlobalMoveMap[2])
	{
		delete GlobalMoveMap[2];
		GlobalMoveMap[2] = nullptr;
	}

	printf("[MOVE_BD] building GlobalMoveMap[0] (ground)\n"); fflush(stdout);
	GlobalMoveMap[0] = new GlobalMap;
	if (!GlobalMoveMap[0])
		Fatal(0, " MOVE_Init: Cannot initialize GlobalMoveMap ");
	GlobalMoveMap[0]->build(mapData);

	printf("[MOVE_BD] building GlobalMoveMap[1] (hover)\n"); fflush(stdout);
	GlobalMoveMap[1] = new GlobalMap;
	if (!GlobalMoveMap[1])
		Fatal(0, " MOVE_Init: Cannot initialize GlobalMoveMap ");
	GlobalMoveMap[1]->hover = true;
	GlobalMoveMap[1]->build(mapData);

	printf("[MOVE_BD] building GlobalMoveMap[2] (blank/heli)\n"); fflush(stdout);
	GlobalMoveMap[2] = new GlobalMap;
	if (!GlobalMoveMap[2])
		Fatal(0, " MOVE_Init: Cannot initialize HeliGlobalMoveMap ");
	GlobalMoveMap[2]->blank = true;
	GlobalMoveMap[2]->build(nullptr);

	printf("[MOVE_BD] all GlobalMaps built OK\n"); fflush(stdout);

	// Just some debugging...
	//for (r = 0; r < height; r++)
	//	for (long c = 0; c < width; c++)
	//		if (GameMap->getOffMap(r, c))
	//			numOffMap++;

	// Since we use this bitfield during the globalmap build for something else, we can now
	// reset it...
	for (long row = 0; row < GameMap->height; row++)
		for (long col = 0; col < GameMap->width; col++)
			for (long i = 0; i < NUM_MOVE_LEVELS; i++)
				GameMap->setPathlock(i, row, col, false);
	
	EditorSave = false;
}

long MOVE_saveData (PacketFile* packetFile, long whichPacket) {

	if (!GameMap)
		Fatal(0, " MOVE_SaveData: Cannot initialize GameMap ");
	if (!GlobalMoveMap[0])
		Fatal(0, " MOVE_SaveData: Cannot initialize GlobalMoveMap ");
	if (!packetFile)
		return(GameMap->write(nullptr) + GlobalMoveMap[0]->write(nullptr) + GlobalMoveMap[1]->write(nullptr) + GlobalMoveMap[2]->write(nullptr) + 2);

	// Just some debugging...
	//long numOffMap = 0;
	//for (long r = 0; r < GameMap->height; r++)
	//	for (long c = 0; c < GameMap->width; c++)
	//		if (GameMap->getOffMap(r, c))
	//			numOffMap++;

	long numMissionMapPackets = GameMap->write(packetFile, whichPacket);
	long numGlobalMap0Packets = GlobalMoveMap[0]->write(packetFile, whichPacket + numMissionMapPackets);
	long numGlobalMap1Packets = GlobalMoveMap[1]->write(packetFile, whichPacket + numMissionMapPackets + numGlobalMap0Packets);
	long numGlobalMap2Packets = GlobalMoveMap[2]->write(packetFile, whichPacket + numMissionMapPackets + numGlobalMap0Packets + numGlobalMap1Packets);

	long result = packetFile->writePacket(whichPacket + numMissionMapPackets + numGlobalMap0Packets + numGlobalMap1Packets + numGlobalMap2Packets, (unsigned char*)&tempNumSpecialAreas, sizeof(long));
	if (result <= 0)
		Fatal(result, " MOVE_saveData: Unable to write num special area ");
	// If there are no special areas, we just write the specialArea count as a filler for the packet...
	if (tempNumSpecialAreas == 0)
		result = packetFile->writePacket(whichPacket + numMissionMapPackets + numGlobalMap0Packets + numGlobalMap1Packets + numGlobalMap2Packets + 1, (unsigned char*)&tempNumSpecialAreas, sizeof(long));
	else
		result = packetFile->writePacket(whichPacket + numMissionMapPackets + numGlobalMap0Packets + numGlobalMap1Packets + numGlobalMap2Packets + 1, (unsigned char*)tempSpecialAreaFootPrints, sizeof(GameObjectFootPrint) * tempNumSpecialAreas);
	if (result <= 0)
		Fatal(result, " MOVE_saveData: Unable to write special area footprints ");

	return(numMissionMapPackets + numGlobalMap0Packets + numGlobalMap1Packets + numGlobalMap2Packets + 2);
}

long MOVE_readData (PacketFile* packetFile, long whichPacket) {

	if (GameMap)
		delete GameMap;
	GameMap = new MissionMap;
	gosASSERT(GameMap != NULL);
	long numMissionMapPackets = GameMap->init(packetFile, whichPacket);

	if (!PathFindMap[SECTOR_PATHMAP])
		Fatal(0, " MOVE_BuildData: Must call MOVE_Init()! ");

	if (GlobalMoveMap[0])
		delete GlobalMoveMap[0];
	GlobalMoveMap[0] = new GlobalMap;
	if (!GlobalMoveMap[0])
		Fatal(0, " MOVE_Init: Cannot initialize GlobalMoveMap0 ");
	long numGlobalMap0Packets = GlobalMoveMap[0]->init(packetFile, whichPacket + numMissionMapPackets);
	long numGlobalMap1Packets = 0;
	long numGlobalMap2Packets = 0;
	if (!GlobalMoveMap[0]->badLoad) {
		// Let's read the hover move map...
		if (GlobalMoveMap[1]) {
			delete GlobalMoveMap[1];
			GlobalMoveMap[1] = nullptr;
		}
		GlobalMoveMap[1] = new GlobalMap;
		if (!GlobalMoveMap[1])
			Fatal(0, " MOVE_Init: Cannot initialize GlobalMoveMap1 ");
		GlobalMoveMap[1]->hover = true;
		numGlobalMap1Packets = GlobalMoveMap[1]->init(packetFile, whichPacket + numMissionMapPackets + numGlobalMap0Packets);

		// Let's read the helicoptor move map...
		if (GlobalMoveMap[2]) {
			delete GlobalMoveMap[2];
			GlobalMoveMap[2] = nullptr;
		}
		GlobalMoveMap[2] = new GlobalMap;
		if (!GlobalMoveMap[2])
			Fatal(0, " MOVE_Init: Cannot initialize GlobalMoveMap2 ");
		GlobalMoveMap[2]->blank = true;
		numGlobalMap2Packets = GlobalMoveMap[2]->init(packetFile, whichPacket + numMissionMapPackets + numGlobalMap0Packets + numGlobalMap1Packets);
		// Delete the pathExistsTable for this one, since we don't use it...
		systemHeap->Free(GlobalMoveMap[2]->pathExistsTable);
		GlobalMoveMap[2]->pathExistsTable = nullptr;

		long numBytes = packetFile->readPacket(whichPacket + numMissionMapPackets + numGlobalMap0Packets + numGlobalMap1Packets + numGlobalMap2Packets, (unsigned char*)&tempNumSpecialAreas);
		if (numBytes <= 0)
			Fatal(numBytes, " MOVE_readData: Unable to read num special areas ");
		if (tempSpecialAreaFootPrints) {
			systemHeap->Free(tempSpecialAreaFootPrints);
			tempSpecialAreaFootPrints = nullptr;
		}
		if (tempNumSpecialAreas > 0) {
			// Mod-tolerance: mc2_10 and other large maps write GameObjectFootPrint
			// with MAX_GAME_OBJECT_CELLS=128 (520 bytes each) rather than the
			// current MAX_GAME_OBJECT_CELLS=64 (264 bytes each). Allocating
			// sizeof(GameObjectFootPrint)*N bytes then reading the full packet
			// caused a 4864-byte heap overflow and non-deterministic crash.
			// Fix: allocate max(packetSize, sizeof*N) — same pattern as GlobalMap
			// mod-tolerance.
			long footprintPkt = whichPacket + numMissionMapPackets + numGlobalMap0Packets + numGlobalMap1Packets + numGlobalMap2Packets + 1;
			packetFile->seekPacket(footprintPkt);
			size_t packetSize = (size_t)packetFile->getPacketSize();
			size_t expectedSize = sizeof(GameObjectFootPrint) * (size_t)tempNumSpecialAreas;
			size_t allocSize = (packetSize > expectedSize) ? packetSize : expectedSize;
			tempSpecialAreaFootPrints = (GameObjectFootPrint*)systemHeap->Malloc(allocSize);
			numBytes = packetFile->readPacket(footprintPkt, (unsigned char*)tempSpecialAreaFootPrints);
			if (numBytes <= 0)
				Fatal(numBytes, " MOVE_readData: Unable to read num special areas ");
		}

	}

	// Just some debugging...
	//numOffMap[0] = 0;
	//for (r = 0; r < GameMap->height; r++)
	//	for (long c = 0; c < GameMap->width; c++)
	//		if (GameMap->getOffMap(r, c))
	//			numOffMap[0]++;

	// PATHFINDING-FACTS-1: emit map topology once at mission load (gated, default-OFF).
	{
		int areas = (GlobalMoveMap[0]) ? GlobalMoveMap[0]->numAreas : 0;
		int doors = (GlobalMoveMap[0]) ? GlobalMoveMap[0]->numDoors : 0;
		int cellW = (GameMap) ? (int)GameMap->width : 0;
		int cellH = (GameMap) ? (int)GameMap->height : 0;
		mc2_path_trace::emitMap(areas, doors, (int)tempNumSpecialAreas, cellW, cellH);
	}

	return(numMissionMapPackets + numGlobalMap0Packets + numGlobalMap1Packets + numGlobalMap2Packets + 2);
}

void MOVE_cleanup (void) {

	if (GameMap) {
		delete GameMap;
		GameMap = nullptr;
	}

	if (GlobalMoveMap[0]) 
	{
		delete GlobalMoveMap[0];
		GlobalMoveMap[0] = nullptr;
	}
	if (GlobalMoveMap[1]) 
	{
		delete GlobalMoveMap[1];
		GlobalMoveMap[1] = nullptr;
	}
	if (GlobalMoveMap[2]) 
	{
		delete GlobalMoveMap[2];
		GlobalMoveMap[2] = nullptr;
	}

	if (PathFindMap[SECTOR_PATHMAP]) {
		delete PathFindMap[SECTOR_PATHMAP];
		PathFindMap[SECTOR_PATHMAP] = nullptr;
	}

	if (PathFindMap[SIMPLE_PATHMAP]) {
		delete PathFindMap[SIMPLE_PATHMAP];
		PathFindMap[SIMPLE_PATHMAP] = nullptr;
	}
}

void* MissionMap::operator new (size_t ourSize) {

	void* result = systemHeap->Malloc(ourSize);
	return(result);
}

void MissionMap::operator delete (void* us) {

	systemHeap->Free(us);
}	

void MissionMap::init (long h, long w) {

	height = h;
	width = w;

	VerticesMapSideDivTwo = (Terrain::blocksMapSide * Terrain::verticesBlockSide) / 2.0;
	MetersMapSideDivTwo = Terrain::worldUnitsMapSide / 2;
	MapCellDiagonal = Terrain::worldUnitsPerCell * 1.4142 * metersPerWorldUnit;
	HalfMapCell = Terrain::worldUnitsPerCell / 2.0;

	for (long i = 0; i < MAX_MAP_CELL_WIDTH; i++)
		tileMulMAPCELL_DIM[i] = i * MAPCELL_DIM;

	long tileWidth = width / MAPCELL_DIM;
	long tileHeight = height / MAPCELL_DIM;
	for (int i = 0; i < tileHeight; i++)
		Terrain::tileRowToWorldCoord[i] = (Terrain::worldUnitsMapSide / 2.0) - (i * Terrain::worldUnitsPerVertex);
	for (int i = 0; i < tileWidth; i++)
		Terrain::tileColToWorldCoord[i] = (i * Terrain::worldUnitsPerVertex) - (Terrain::worldUnitsMapSide / 2.0);
	for (int i = 0; i < MAPCELL_DIM; i++)
		Terrain::cellToWorldCoord[i] = (Terrain::worldUnitsPerVertex / (float)MAPCELL_DIM) * i;

	for (int i = 0; i < height; i++)
		Terrain::cellRowToWorldCoord[i] = (Terrain::worldUnitsMapSide / 2.0) - (i * Terrain::worldUnitsPerCell);
	for (int i = 0; i < width; i++)
		Terrain::cellColToWorldCoord[i] = (i * Terrain::worldUnitsPerCell) - (Terrain::worldUnitsMapSide / 2.0);

	if (map) {
		systemHeap->Free(map);
		map = nullptr;
	}
	long mapSize = sizeof(MapCell) * width * height;
	if (mapSize > 0)
		map = (MapCellPtr)systemHeap->Malloc(sizeof(MapCell) * width * height);
	gosASSERT(map != NULL);
	memclear(map, sizeof(MapCell) * width * height);

	// Per-tile mine count tracking — sized to the tile grid (cells / MAPCELL_DIM
	// per dim). Allocated zero-filled; populated by setMine() during cell load
	// or by rebuildTileMineCounts() after a packet readPacket().
	if (tileMineCount) {
		systemHeap->Free(tileMineCount);
		tileMineCount = nullptr;
	}
	tileMineCountWidth = w / MAPCELL_DIM;
	const long tileMineHeight = h / MAPCELL_DIM;
	const long tileMineSize = tileMineCountWidth * tileMineHeight;
	if (tileMineSize > 0) {
		tileMineCount = (unsigned char*)systemHeap->Malloc(tileMineSize);
		gosASSERT(tileMineCount != NULL);
		memclear(tileMineCount, tileMineSize);
	}
}

void MissionMap::rebuildTileMineCounts (void) {
	if (!tileMineCount || !map) return;
	const long tileMineHeight = height / MAPCELL_DIM;
	const long tileMineSize = tileMineCountWidth * tileMineHeight;
	memclear(tileMineCount, tileMineSize);
	for (long r = 0; r < height; ++r) {
		for (long c = 0; c < width; ++c) {
			if (map[r * width + c].getMine() != 0) {
				const long tileIdx = (r / MAPCELL_DIM) * tileMineCountWidth + (c / MAPCELL_DIM);
				++tileMineCount[tileIdx];
			}
		}
	}
	// PR2c Stage 1c — packet-load bypass setMine, so mark the static VBO
	// dirty after the rebuild completes so the next paint cycle re-emits
	// geometry from the freshly-rebuilt cell state.
	gos_terrain_indirect::MarkMineDirty();
}

constexpr int NUM_MISSIONMAP_PACKETS = 4;

long MissionMap::init (PacketFile* packetFile, long whichPacket) {

	packetFile->readPacket(whichPacket++, (unsigned char*)&height);
	packetFile->readPacket(whichPacket++, (unsigned char*)&width);
	packetFile->readPacket(whichPacket++, (unsigned char*)&planet);
	init(height, width);
	packetFile->readPacket(whichPacket++, (unsigned char*)map);

	for (long r = 0; r < height; r++)
		for (long c = 0; c < width; c++) {
			GameMap->setMover(r, c, false);
		}

	// readPacket() writes raw cell bytes into `map` and bypasses setMine(), so
	// the incremental tileMineCount maintenance never fired. Rebuild from the
	// loaded cell contents.
	rebuildTileMineCounts();

	return(NUM_MISSIONMAP_PACKETS);
}

long MissionMap::write (PacketFile* packetFile, long whichPacket) {

	// Return number of packets if !packetFile...
	if (!packetFile)
		return(NUM_MISSIONMAP_PACKETS);

	long result = packetFile->writePacket(whichPacket++, height);
	if (result <= 0)
		Fatal(result, " MissionMap.write: Unable to write height packet ");
	result = packetFile->writePacket(whichPacket++, width);
	if (result <= 0)
		Fatal(result, " MissionMap.write: Unable to write width packet ");
	result = packetFile->writePacket(whichPacket++, planet);
	if (result <= 0)
		Fatal(result, " MissionMap.write: Unable to write planet packet ");
	result = packetFile->writePacket(whichPacket++, (unsigned char*)map, sizeof(MapCell) * height * width);
	if (result <= 0)
		Fatal(result, " MissionMap.write: Unable to write map packet ");
	return(NUM_MISSIONMAP_PACKETS);
}

bool MissionMap::getPassable (Stuff::Vector3D cellPosition) {

	int row, col;
	land->worldToCell(cellPosition, row, col);
	return(getPassable(row, col));
}

long MissionMap::init (long cellHeight, long cellWidth, long curPlanet, MissionMapCellInfo* mapData) {

	if (!map)
		init(cellHeight, cellWidth);

	planet = curPlanet;

	// Be damned sure map is empty.  This is required or mines will NOT go away.
	memset(map, 0, sizeof(MapCell) * width * height);

	long terrainTypes[1024];
	long overlayTypes[1024];
	for (long i = 0; i < 256; i++)
		terrainTypes[i] = overlayTypes[i] = 0;

	if (mapData)
		for (long row = 0; row < height; row++)
			for (long col = 0; col < width; col++) {
				setTerrain(row, col, mapData[row * width + col].terrain);
				terrainTypes[mapData[row * width + col].terrain]++;
				setOverlay(row, col, mapData[row * width + col].overlay);
				overlayTypes[mapData[row * width + col].overlay]++;
				setGate(row, col, mapData[row * width + col].gate);
				setRoad(row, col, mapData[row * width + col].road);
				setPassable(row, col, mapData[row * width + col].passable);
				setLocalHeight(row, col, mapData[row * width + col].lineOfSight);
				setPreserved(row, col, false);
				setMine(row, col, mapData[row * width + col].mine);
				setForest(row, col, mapData[row * width + col].forest);
				setBuildWall(row, col, false);
				setBuildGate(row, col, false);
				Stuff::Vector3D worldPos;
				land->cellToWorld(row, col, worldPos);
				long cellWaterType = TerrainRuntime::sampleWaterClass(worldPos);
				if (cellWaterType == WATER_TYPE_SHALLOW)
					setShallowWater(row, col, true);
				if (cellWaterType == WATER_TYPE_DEEP)
					setDeepWater(row, col, true);
				for (long i = 0; i < NUM_MOVE_LEVELS; i++)
					GameMap->setPathlock(i, row, col, false);
				switch (mapData[row * width + col].specialType) {
					case SPECIAL_WALL:
						setBuildWall(row, col, true);
						break;
					case SPECIAL_GATE:
						setBuildGate(row, col, true);
						break;
					case SPECIAL_LAND_BRIDGE:
						setBuildLandBridge(row, col, true);
						break;
					//case SPECIAL_FOREST:
					//	setBuildForest(row, col, true);
					//	break;
				}
			}
	else {
		for (long row = 0; row < height; row++)
			for (long col = 0; col < width; col++) {
				setTerrain(row, col, MC_GRASS_TYPE);
				setOverlay(row, col, 0);
				setGate(row, col, false);
				if ((row == 0) || (col == 0) || (row == (height - 1)) || (col == (width - 1)))
					setPassable(row, col, false);
				else
					setPassable(row, col, true);
				for (long i = 0; i < NUM_MOVE_LEVELS; i++)
					GameMap->setPathlock(i, row, col, false);
				setPreserved(row, col, false);
				setMine(row, col, 0);
			}
	}

	return(0);
}

void MissionMap::setPassable (long row, long col, char* footPrint, bool passable) {

	/* FootPrint Data format:
			r1, c1, l1, r2, c2, l2, r3, c3, l3, ...
			where r1 and c1 are offsets from row,col and l1 is the runlength of
			the passable/impassable block of cells. This function keeps reading in the
			(r, c, l) sets of data until (-1, -1, -1) is read in. Obviously, the data
			is a block of chars, so all values must be -127 < x < 127. This should
			allow foot prints big enuff for all buildings we have.
	*/

	char* data = footPrint;
	long r = row + *data++;
	long c = col + *data++;
	long len = *data++;
	while (len != -1) {
		MapCellPtr cell = &map[r * width + c];
		for (long i = 0; i < len; i++) {
			cell->setPassable(passable);
			cell++;
		}
		r = row + *data++;
		c = col + *data++;
		len = *data++;
	}
}

void MissionMap::spreadState (long cellRow, long cellCol, long radius) {

	// This does a simple depth-first spread from the center. The way it's coded,
	// it essentially grows a box from the center cell.
	if (inMapBounds(cellRow, cellCol, height, width) && (radius > 0)) {
		// If we're placing moving objects for this frame only, we'll want to
		// preserve the cell states of this tile...
		if (!getPreserved(cellRow, cellCol))
			setPreserved(cellRow, cellCol, true);
		map[cellRow * width + cellCol].setPassable(false);
		for (long dir = 0; dir < NUM_DIRECTIONS; dir++) {
			long adjR = cellRow;
			long adjC = cellCol;
			calcAdjNode(adjR, adjC, dir);
			spreadState(adjR, adjC, radius - 1);
		}
	}
}

long MissionMap::placeObject (Stuff::Vector3D position, float radius) {

	int cellRow, cellCol;
	land->worldToCell(position, cellRow, cellCol);

	// Now, set the cell states based upon the object's size...
	float cellRadius = (radius / (Terrain::worldUnitsPerCell  * metersPerWorldUnit));
	if ((cellRadius > 0.5) && (cellRadius < 1.0))
		cellRadius = 1.0;
	spreadState(cellRow, cellCol, (long)cellRadius);
	return(NO_ERR);
}

void MissionMap::placeTerrainObject(long objectClass,
									 long objectTypeID,
									 long cellRow,
									 long cellCol,
									 __int64 footPrint,
									 bool blocksLineOfFire,
									 long mineType) {
}

long MissionMap::getOverlayWeight (long cellR, long cellC, long moverOverlayWeightClass, long moverRelation) {

	//long overlay = getOverlay(cellR, cellC);
/*
	if (overlay) {
		long* overlayWeightTable = &OverlayWeightTable[moverOverlayWeightClass * NUM_OVERLAY_TYPES * MAPCELL_DIM * MAPCELL_DIM];
		if ((overlayType >= OVERLAY_GATE_CLAN_NS_OPEN) && (overlayType <= OVERLAY_GATE_IS_EW_CLOSED)) {
			overlayType = GateOverlayTable[moverRelation][overlayType - OVERLAY_GATE_CLAN_NS_OPEN];
			if (overlayType == -1)
				return(COST_BLOCKED * 2);
			else {
				long overlayCostIndex = OverlayWeightIndex[overlayType] + cellR * MAPCELL_DIM + cellC;
				return(overlayWeightTable[overlayCostIndex]);
			}
			}
		else {
			long overlayCostIndex = OverlayWeightIndex[overlayType] + cellR * MAPCELL_DIM + cellC;
			return(overlayWeightTable[overlayCostIndex]);
		}
	}
*/
  return(0);
}

void MissionMap::print (char* fileName) {

	File debugFile;
	debugFile.create(fileName);
	
	for (long cellR = 0; cellR < height; cellR++) {
		char outString[1024];
		outString[0] = '\0';
		for (long cellC = 0; cellC < width; cellC++) {
			if (!map[cellR * width + cellC].getPassable())
				strcat(outString, "X");
			else
				strcat(outString, ".");
		}
		strcat(outString, "\n");
		debugFile.writeString(outString);
	}
	debugFile.writeString("\n");

	debugFile.close();
}

void MissionMap::destroy (void) {

	if (map) {
		systemHeap->Free(map);
		map = nullptr;
	}
	if (tileMineCount) {
		systemHeap->Free(tileMineCount);
		tileMineCount = nullptr;
	}
}

// MOVE PATH class

void* MovePath::operator new (size_t ourSize) {

	void* result;
	result = systemHeap->Malloc(ourSize);
	return(result);
}

void MovePath::operator delete (void* us) {

	systemHeap->Free(us);
}	

long MovePath::init (long numberOfSteps) {

	numSteps = numStepsWhenNotPaused = numberOfSteps;
	static long maxNumberOfSteps = 0;
	if (numberOfSteps > maxNumberOfSteps) {
		maxNumberOfSteps = numberOfSteps;
		return(maxNumberOfSteps);
	}
	for (int i = 0; i < MAX_STEPS_PER_MOVEPATH; i++) {
		stepList[i].distanceToGoal = 0.0;
		stepList[i].destination.x = 0.0;
		stepList[i].destination.y = 0.0;
		stepList[i].destination.z = 0.0;
		stepList[i].direction = 0;
		stepList[i].area = 0; // sebi: init var
		// should we set cells[] to something as well?
		stepList[i].cell[0] = 0; 
		stepList[i].cell[1] = 0; 
	}
	return(-1);
}

void MovePath::clear (void) {

	init();
}

void MovePath::destroy (void) {

	numSteps = 0;
}

float MovePath::getDistanceLeft (Stuff::Vector3D position, long stepNumber) {

	if (stepNumber == -1)
		stepNumber = curStep;
	float distance = distance_from(position,stepList[stepNumber].destination);
	distance *= metersPerWorldUnit;
	distance += stepList[stepNumber].distanceToGoal;
	return(distance);
}

void MovePath::lock (long level, long start, long range, bool setting) {

#ifdef _DEBUG
	if (level > 1)
		STOP(("MovePath.lock: bad level %d", level));
#endif

	if (start == -1)
		start = curStep;
	long lastStep = start + range;
	if (lastStep >= numStepsWhenNotPaused)
		lastStep = numStepsWhenNotPaused;
	for (long i = start; i < lastStep; i++)
		GameMap->setPathlock(level, stepList[i].cell[0], stepList[i].cell[1], setting);
}

bool MovePath::isLocked (long level, long start, long range, bool* reachedEnd) {

#ifdef _DEBUG
	if (level > 1)
		STOP(("MovePath.isLocked: bad moveLevel %d", level));
#endif

	if (start == -1)
		start = curStep;
	long lastStep = start + range;
	if (reachedEnd)
		*reachedEnd = false;
	if (lastStep >= numStepsWhenNotPaused) {
		if (reachedEnd)
			*reachedEnd = true;
		lastStep = numStepsWhenNotPaused;
	}
	for (long i = start; i < lastStep; i++)
		if (GameMap->getPathlock(level, stepList[i].cell[0], stepList[i].cell[1]))
			return(true);
	return(false);
}

bool MovePath::isBlocked (long start, long range, bool* reachedEnd) {

	if (start == -1)
		start = curStep;
	long lastStep = start + range;
	if (reachedEnd)
		*reachedEnd = false;
	if (lastStep >= numStepsWhenNotPaused) {
		if (reachedEnd)
			*reachedEnd = true;
		lastStep = numStepsWhenNotPaused;
	}
	for (long i = start; i < lastStep; i++)
		if (!GameMap->getPassable(stepList[i].cell[0], stepList[i].cell[1]))
			return(true);
	return(false);
}

long MovePath::crossesBridge (long start, long range) {

	if (start == -1)
		start = curStep;
	long lastStep = start + range;
	if (lastStep >= numStepsWhenNotPaused)
		lastStep = numStepsWhenNotPaused;
	//We need to redo this code when bridges are in cause they are objects now!!
	return(-1);
}

long MovePath::crossesCell (long start, long range, long cellR, long cellC) {

	if (start == -1)
		start = curStep;
	long lastStep = start + range;
	if (lastStep >= numStepsWhenNotPaused)
		lastStep = numStepsWhenNotPaused;
	for (long i = start; i < lastStep; i++) {
		if ((cellR == stepList[i].cell[0]) && (cellC == stepList[i].cell[1]))
			return(i);
	}
	return(-1);
}

long MovePath::crossesClosedClanGate (long start, long range) {

	return(-1);
}

long MovePath::crossesClosedISGate (long start, long range) {

	return(-1);
}

long MovePath::crossesClosedGate (long start, long range) {

	if (start == -1)
		start = curStep;
	long lastStep = start + range;
	if (lastStep >= numStepsWhenNotPaused)
		lastStep = numStepsWhenNotPaused;
	//Probably should have redone this code cause gates aren't overlays anymore?
	return(-1);
}
