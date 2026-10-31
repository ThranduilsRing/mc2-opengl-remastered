//	move.cpp - This file contains the Movement\Pathfinding routines
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include <utility>
#include <mdspan>
#include "move_internal.h"
#include "../GameOS/gameos/utils/mc2log.h"
#include "packet_exact.h"
#include <memory>


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
static std::unique_ptr<MissionMap> s_gameMapOwner;
static std::unique_ptr<GlobalMap> s_globalMoveMapOwner[3];
// Both search participants are prepared at load: frame/load thread (0), overlap
// simulation worker (1). Binding a prepared set never allocates. Global-map
// mutation and live callbacks still require synchronous service (M3).
static std::unique_ptr<MoveMap> s_pathFindMapOwner[2][2];
static thread_local int s_pathMapWorker = 0;

void MOVE_bindPathFindMaps (int worker) {
	if (worker < 0 || worker > 1)
		Fatal(0, " Invalid path scratch worker ");
	s_pathMapWorker = worker;
	for (int which = 0; which < 2; ++which)
		PathFindMap[which] = s_pathFindMapOwner[worker][which].get();
}

void MOVE_configurePathFindMaps (void) {
	// Called after mission load (including quickload), while simulation is joined.
	for (int which = 0; which < 2; ++which) {
		auto* source = PathFindMap[which];
		for (auto& worker : s_pathFindMapOwner)
			if (worker[which]) {
				worker[which]->blockedDoorCallback = source->blockedDoorCallback;
				worker[which]->placeStationaryMoversCallback = source->placeStationaryMoversCallback;
			}
	}
}

MoveMapPtr MOVE_acquirePathFindMap (int which) {
	auto& owner = s_pathFindMapOwner[s_pathMapWorker][which];
	if (!owner)
		owner = std::make_unique<MoveMap>();
	PathFindMap[which] = owner.get();
	return PathFindMap[which];
}

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

bool PreserveMapTiles = false;

thread_local MoveMapPtr PathFindMap[2] = {nullptr, nullptr};

float MoveMap::distanceFloat[DISTANCE_TABLE_DIM][DISTANCE_TABLE_DIM];
int MoveMap::distanceInt[DISTANCE_TABLE_DIM][DISTANCE_TABLE_DIM];
int MoveMap::forestCost = 50;

glm::vec3 relativePositionToPoint (glm::vec3 point, float angle, float distance, unsigned long flags) {

	// Note that the angle should be -180 <= angle <= 180, and
	// the distance is in meters...

	glm::vec3 curPos;
	curPos.x = point.x;
	curPos.y = point.y;
	curPos.z = 0.0;

	distance *= -worldUnitsPerMeter;

	glm::vec3 shiftVect;
	shiftVect.x = 0.0;
	shiftVect.y = 1.0;
	shiftVect.z = 0.0;

	Rotate(shiftVect,angle);

	shiftVect *= distance;
	glm::vec3 relPos;
	relPos = glm::vec3(0.0f);
	relPos = curPos + shiftVect;

	glm::vec3 start2d;
	glm::vec3 goal2d;
	glm::vec3 deltaVector;
	if (flags & RELPOS_FLAG_PASSABLE_START) {
		start2d  = curPos;
		goal2d = relPos;
		}
	else {
		start2d = relPos;
		goal2d = curPos;
	}

	deltaVector = goal2d - start2d;

	// First, we need to calc the delta vector--how much we extend
	// the ray everytime we check the map cell for clear placement.
	deltaVector = glm::normalize(deltaVector);
	deltaVector *= Terrain::halfWorldUnitsPerCell;
	if (glm::length(deltaVector) == 0.0)
		return(curPos);

	// Determine the max length the ray must be cast...
	float maxLength = distance_from(goal2d,start2d);

	// We'll start at the target, and if it's blocked, we'll move
	// toward our start location, looking for the first valid/open
	// cell...
	glm::vec3 curPoint = start2d;
	glm::vec3 curRay;
	curRay = glm::vec3(0.0f);
	float rayLength = 0.0;

	int cellR, cellC;
	glm::vec3 curPoint3d(curPoint.x, curPoint.y, 0.0);
	land->worldToCell(curPoint3d, cellR, cellC);
	unsigned int cellClear = GameMap->getPassable(cellR, cellC);

	glm::vec3 lastGoodPoint = curPoint;
	if (flags & RELPOS_FLAG_PASSABLE_START)
		while (cellClear && (rayLength < maxLength)) {
			lastGoodPoint = curPoint;
			curPoint += deltaVector;
			curRay = curPoint - start2d;
			rayLength = glm::length(curRay);

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
			curRay = curPoint - start2d;
			rayLength = glm::length(curRay);

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

	SimpleMovePathRange = moveRange;
	if (SimpleMovePathRange <= 20)
		Fatal(0, " MOVE_Init: MoveRange TOO SMALL. Go see Glenn. ");
	for (auto& worker : s_pathFindMapOwner) {
		for (auto& owner : worker)
			if (!owner)
				owner = std::make_unique<MoveMap>();
		worker[SECTOR_PATHMAP]->init(SECTOR_DIM*2, SECTOR_DIM*2);
		worker[SIMPLE_PATHMAP]->init(SimpleMovePathRange * 2 + 1, SimpleMovePathRange * 2 + 1);
	}
	MOVE_bindPathFindMaps(s_pathMapWorker);
}

bool EditorSave = false;
long tempNumSpecialAreas = 0;
GameObjectFootPrint* tempSpecialAreaFootPrints = nullptr;

void MOVE_buildData (long height, long width, MissionMapCellInfo* mapData, long numSpecialAreas, GameObjectFootPrint* specialAreaFootPrints) {

	printf("[MOVE_BD] enter h=%ld w=%ld mapData=%p specials=%ld\n", height, width, static_cast<void*>(mapData), numSpecialAreas); fflush(stdout);

	EditorSave = true;

	s_gameMapOwner = std::make_unique<MissionMap>();
	GameMap = s_gameMapOwner.get();
	printf("[MOVE_BD] MissionMap allocated OK, calling init(%ld,%ld)\n", height, width); fflush(stdout);

	// The four-argument initializer also populates passability for a blank map.
	// The allocation-only overload leaves every interior cell blocked.
	GameMap->init(height, width, 0, mapData);

	if (GameMap->map.empty()) { MC2_LOG(Io, Error, "move: the built mission map is {} x {} cells, so it is empty; no move data is built", height, width); return; }
	printf("[MOVE_BD] GameMap->init OK map=%p\n", static_cast<void*>(GameMap->map.data())); fflush(stdout);

	long numOffMap = 0;
	for (long r = 0; r < height; r++)
		for (long c = 0; c < width; c++) {
			glm::vec3 worldPos;
			land->cellToWorld(r, c, worldPos);
			bool set = (land->IsEditorSelectTerrainPosition(worldPos) && !land->IsGameSelectTerrainPosition(worldPos));
			GameMap->setOffMap(r, c, set);
			if (set)
				numOffMap++;
		}

	printf("[MOVE_BD] offMap loop done numOffMap=%ld\n", numOffMap); fflush(stdout);

	if (tempSpecialAreaFootPrints) {
		std::free(tempSpecialAreaFootPrints);
		tempSpecialAreaFootPrints = nullptr;
	}
	tempNumSpecialAreas = numSpecialAreas;
	if (tempNumSpecialAreas) {
		tempSpecialAreaFootPrints = static_cast<GameObjectFootPrint*>(std::malloc(sizeof(GameObjectFootPrint) * tempNumSpecialAreas));
		memcpy(tempSpecialAreaFootPrints, specialAreaFootPrints, sizeof(GameObjectFootPrint) * tempNumSpecialAreas);
	}
	if (!PathFindMap[SECTOR_PATHMAP])
		Fatal(0, " MOVE_BuildData: Must call MOVE_Init()! ");

	printf("[MOVE_BD] building GlobalMoveMap[0] (ground)\n"); fflush(stdout);
	s_globalMoveMapOwner[0] = std::make_unique<GlobalMap>();
	GlobalMoveMap[0] = s_globalMoveMapOwner[0].get();
	GlobalMoveMap[0]->build(mapData);

	printf("[MOVE_BD] building GlobalMoveMap[1] (hover)\n"); fflush(stdout);
	s_globalMoveMapOwner[1] = std::make_unique<GlobalMap>();
	GlobalMoveMap[1] = s_globalMoveMapOwner[1].get();
	GlobalMoveMap[1]->hover = true;
	GlobalMoveMap[1]->build(mapData);

	printf("[MOVE_BD] building GlobalMoveMap[2] (blank/heli)\n"); fflush(stdout);
	s_globalMoveMapOwner[2] = std::make_unique<GlobalMap>();
	GlobalMoveMap[2] = s_globalMoveMapOwner[2].get();
	GlobalMoveMap[2]->blank = true;
	GlobalMoveMap[2]->build(nullptr);
	for (auto* globalMap : GlobalMoveMap)
		globalMap->openHeap.reserve(globalMap->numDoors + NUM_DOOR_OFFSETS);

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

long MOVE_saveData (PacketFile* packetFile, long whichPacket, mc2::save::NavigationFormat format) {

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

	long numMissionMapPackets = GameMap->write(packetFile, whichPacket, format.cells);
	long numGlobalMap0Packets = GlobalMoveMap[0]->write(packetFile, whichPacket + numMissionMapPackets);
	long numGlobalMap1Packets = GlobalMoveMap[1]->write(packetFile, whichPacket + numMissionMapPackets + numGlobalMap0Packets);
	long numGlobalMap2Packets = GlobalMoveMap[2]->write(packetFile, whichPacket + numMissionMapPackets + numGlobalMap0Packets + numGlobalMap1Packets);

	long result = packetFile->writePacket(whichPacket + numMissionMapPackets + numGlobalMap0Packets + numGlobalMap1Packets + numGlobalMap2Packets, reinterpret_cast<unsigned char*>(&tempNumSpecialAreas), sizeof(long));
	if (result <= 0)
		Fatal(result, " MOVE_saveData: Unable to write num special area ");
	// If there are no special areas, we just write the specialArea count as a filler for the packet...
	if (tempNumSpecialAreas == 0)
		result = packetFile->writePacket(whichPacket + numMissionMapPackets + numGlobalMap0Packets + numGlobalMap1Packets + numGlobalMap2Packets + 1, reinterpret_cast<unsigned char*>(&tempNumSpecialAreas), sizeof(long));
	else
		result = packetFile->writePacket(whichPacket + numMissionMapPackets + numGlobalMap0Packets + numGlobalMap1Packets + numGlobalMap2Packets + 1, reinterpret_cast<unsigned char*>(tempSpecialAreaFootPrints), sizeof(GameObjectFootPrint) * tempNumSpecialAreas);
	if (result <= 0)
		Fatal(result, " MOVE_saveData: Unable to write special area footprints ");

	return(numMissionMapPackets + numGlobalMap0Packets + numGlobalMap1Packets + numGlobalMap2Packets + 2);
}

long MOVE_readData (PacketFile* packetFile, long whichPacket, bool restoreSavedCosts, mc2::save::NavigationFormat format) {

	s_gameMapOwner = std::make_unique<MissionMap>();
	GameMap = s_gameMapOwner.get();
	long numMissionMapPackets = GameMap->init(packetFile, whichPacket, format.cells);
	if (numMissionMapPackets == 0)
		Fatal(0, " MOVE_readData: the mission map was rejected; the mission is not loaded ");

	if (!PathFindMap[SECTOR_PATHMAP])
		Fatal(0, " MOVE_BuildData: Must call MOVE_Init()! ");

	s_globalMoveMapOwner[0] = std::make_unique<GlobalMap>();
	GlobalMoveMap[0] = s_globalMoveMapOwner[0].get();
	// The mission map reports a small packet count (zero is Fatal above), so this one sum cannot overflow and needs no saturation.
	long numGlobalMap0Packets = GlobalMoveMap[0]->init(packetFile, whichPacket + numMissionMapPackets, restoreSavedCosts);
	long numGlobalMap1Packets = 0;
	long numGlobalMap2Packets = 0;
	if (!GlobalMoveMap[0]->badLoad) {
		// Let's read the hover move map...
		s_globalMoveMapOwner[1] = std::make_unique<GlobalMap>();
		GlobalMoveMap[1] = s_globalMoveMapOwner[1].get();
		GlobalMoveMap[1]->hover = true;
		const long map1Start = mc2::globalmap::advanceCursor(whichPacket + numMissionMapPackets, numGlobalMap0Packets);
		numGlobalMap1Packets = GlobalMoveMap[1]->init(packetFile, map1Start, restoreSavedCosts);

		// Let's read the helicoptor move map...
		s_globalMoveMapOwner[2] = std::make_unique<GlobalMap>();
		GlobalMoveMap[2] = s_globalMoveMapOwner[2].get();
		GlobalMoveMap[2]->blank = true;
		const long map2Start = mc2::globalmap::advanceCursor(map1Start, numGlobalMap1Packets);
		numGlobalMap2Packets = GlobalMoveMap[2]->init(packetFile, map2Start, restoreSavedCosts);
		// Delete the pathExistsTable for this one, since we don't use it...
		GlobalMoveMap[2]->releasePathExistsTable();

		const long specialAreaPkt = mc2::globalmap::advanceCursor(map2Start, numGlobalMap2Packets);
		long numBytes = packetFile->readPacket(specialAreaPkt, reinterpret_cast<unsigned char*>(&tempNumSpecialAreas));
		if (numBytes <= 0)
			Fatal(numBytes, " MOVE_readData: Unable to read num special areas ");
		if (tempSpecialAreaFootPrints) {
			std::free(tempSpecialAreaFootPrints);
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
			long footprintPkt = mc2::globalmap::advanceCursor(specialAreaPkt, 1);
			packetFile->seekPacket(footprintPkt);
			size_t packetSize = static_cast<size_t>(packetFile->getPacketSize());
			size_t expectedSize = sizeof(GameObjectFootPrint) * static_cast<size_t>(tempNumSpecialAreas);
			size_t allocSize = (packetSize > expectedSize) ? packetSize : expectedSize;
			tempSpecialAreaFootPrints = static_cast<GameObjectFootPrint*>(std::malloc(allocSize));
			numBytes = packetFile->readPacket(footprintPkt, reinterpret_cast<unsigned char*>(tempSpecialAreaFootPrints));
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
	for (auto* globalMap : GlobalMoveMap)
		if (globalMap)
			globalMap->openHeap.reserve(globalMap->numDoors + NUM_DOOR_OFFSETS);
	{
		int areas = (GlobalMoveMap[0]) ? GlobalMoveMap[0]->numAreas : 0;
		int doors = (GlobalMoveMap[0]) ? GlobalMoveMap[0]->numDoors : 0;
		int cellW = (GameMap) ? static_cast<int>(GameMap->width) : 0;
		int cellH = (GameMap) ? static_cast<int>(GameMap->height) : 0;
		mc2_path_trace::emitMap(areas, doors, static_cast<int>(tempNumSpecialAreas), cellW, cellH);
	}

	long packetsRead = mc2::globalmap::advanceCursor(numMissionMapPackets, numGlobalMap0Packets);
	packetsRead = mc2::globalmap::advanceCursor(packetsRead, numGlobalMap1Packets);
	packetsRead = mc2::globalmap::advanceCursor(packetsRead, numGlobalMap2Packets);
	return mc2::globalmap::advanceCursor(packetsRead, 2);
}

void MOVE_cleanup (void) {

	s_gameMapOwner.reset();
	GameMap = nullptr;

	for (auto& owner : s_globalMoveMapOwner)
		owner.reset();
	for (auto& observer : GlobalMoveMap)
		observer = nullptr;

	for (auto& worker : s_pathFindMapOwner)
		for (auto& owner : worker)
			owner.reset();
	PathFindMap[SECTOR_PATHMAP] = nullptr;
	PathFindMap[SIMPLE_PATHMAP] = nullptr;
}





void MissionMap::init (long h, long w) {

	rev.statics++; // raw grid replacement also invalidates prepared search inputs
	height = h;
	width = w;
	gTerrainPartIdStride = (w > TERRAIN_PART_ID_STRIDE_STOCK) ? w : TERRAIN_PART_ID_STRIDE_STOCK;

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
		Terrain::cellToWorldCoord[i] = (Terrain::worldUnitsPerVertex / static_cast<float>(MAPCELL_DIM)) * i;

	for (int i = 0; i < height; i++)
		Terrain::cellRowToWorldCoord[i] = (Terrain::worldUnitsMapSide / 2.0) - (i * Terrain::worldUnitsPerCell);
	for (int i = 0; i < width; i++)
		Terrain::cellColToWorldCoord[i] = (i * Terrain::worldUnitsPerCell) - (Terrain::worldUnitsMapSide / 2.0);

	const size_t cellCount = (width > 0 && height > 0) ? static_cast<size_t>(width) * static_cast<size_t>(height) : 0;
	map.assign(cellCount, MapCell{});

	// Per-tile mine count, one counter per MAPCELL_DIM x MAPCELL_DIM tile, all zero here: setMine() keeps it current during
	// cell load and rebuildTileMineCounts() rebuilds it after a packet load (which bypasses setMine).
	tileMineCountWidth = w / MAPCELL_DIM;
	const long tileMineHeight = h / MAPCELL_DIM;
	const long tileMineSize = tileMineCountWidth * tileMineHeight;
	tileMineCount.assign(tileMineSize > 0 ? static_cast<size_t>(tileMineSize) : 0, 0);
}

void MissionMap::rebuildTileMineCounts (void) {
	if (tileMineCount.empty() || map.empty()) return;
	std::fill(tileMineCount.begin(), tileMineCount.end(), 0);
	std::mdspan<MapCell, std::dextents<std::size_t, 2>> mapView(map.data(), static_cast<std::size_t>(height), static_cast<std::size_t>(width));
	for (long r = 0; r < height; ++r) {
		for (long c = 0; c < width; ++c) {
			if (mapView[r, c].getMine() != 0) {
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

long MissionMap::init (PacketFile* packetFile, long whichPacket, bool taggedCells) {

	// Any rejected packet refuses the whole map (0 packets read): a mission on a stale planet or an all-clear grid plays on a
	// passable-everywhere map. The first rejection is the only log line.
	int32_t h = 0;
	int32_t w = 0;
	if (!mc2::pak::readExact(*packetFile, whichPacket, &h, sizeof(h), "mission map height") ||
		!mc2::pak::readExact(*packetFile, whichPacket + 1, &w, sizeof(w), "mission map width") ||
		!mc2::pak::readExact(*packetFile, whichPacket + 2, &planet, sizeof(planet), "mission map planet"))
		return 0;
	// The dimensions size the grid and index the terrain's per-cell coordinate tables.
	if (h <= 0 || w <= 0 || h > MAX_MAP_CELL_WIDTH || w > MAX_MAP_CELL_WIDTH) {
		MC2_LOG(Io, Error, "move: mission map is {} x {} cells (valid 1..{}); the map is not loaded", h, w, MAX_MAP_CELL_WIDTH);
		return 0;
	}
	init(h, w);
	bool cellsRead = false;
	if (taggedCells) {
		mc2::save::Bytes bytes(12 + map.size() * sizeof(std::uint32_t));
		cellsRead = mc2::pak::readExact(*packetFile, whichPacket + 3, bytes.data(), bytes.size(), "mission map values") &&
			mc2::save::decodeNavigationRecord(bytes, std::span(map), mc2::save::tagMapCells);
	} else {
		cellsRead = mc2::pak::readExact(*packetFile, whichPacket + 3, map.data(), map.size() * sizeof(MapCell), "mission map cell");
	}
	if (!cellsRead) {
		std::fill(map.begin(), map.end(), MapCell{});
		return 0;
	}

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

long MissionMap::write (PacketFile* packetFile, long whichPacket, bool taggedCells) {

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
	if (taggedCells) {
		auto bytes = mc2::save::encodeNavigationRecord(std::span(map), mc2::save::tagMapCells);
		result = packetFile->writePacket(whichPacket++, bytes.data(), static_cast<unsigned int>(bytes.size()));
	} else {
		result = packetFile->writePacket(whichPacket++, reinterpret_cast<unsigned char*>(map.data()), static_cast<unsigned int>(map.size() * sizeof(MapCell)));
	}
	if (result <= 0)
		Fatal(result, " MissionMap.write: Unable to write map packet ");
	return(NUM_MISSIONMAP_PACKETS);
}

bool MissionMap::getPassable (glm::vec3 cellPosition) {

	int row, col;
	land->worldToCell(cellPosition, row, col);
	return(getPassable(row, col));
}

long MissionMap::init (long cellHeight, long cellWidth, long curPlanet, MissionMapCellInfo* mapData) {

	if (map.empty())
		init(cellHeight, cellWidth);

	planet = curPlanet;

	// Every cell starts clear: a reused grid would otherwise keep the previous mission's mines.
	std::fill(map.begin(), map.end(), MapCell{});

	long terrainTypes[1024];
	long overlayTypes[1024];
	for (long i = 0; i < 256; i++)
		terrainTypes[i] = overlayTypes[i] = 0;

	if (mapData) {
		std::mdspan<MissionMapCellInfo, std::dextents<std::size_t, 2>> mapDataView(mapData, static_cast<std::size_t>(height), static_cast<std::size_t>(width));
		for (long row = 0; row < height; row++)
			for (long col = 0; col < width; col++) {
				setTerrain(row, col, mapDataView[row, col].terrain);
				terrainTypes[mapDataView[row, col].terrain]++;
				setOverlay(row, col, mapDataView[row, col].overlay);
				overlayTypes[mapDataView[row, col].overlay]++;
				setGate(row, col, mapDataView[row, col].gate);
				setRoad(row, col, mapDataView[row, col].road);
				setPassable(row, col, mapDataView[row, col].passable);
				setLocalHeight(row, col, mapDataView[row, col].lineOfSight);
				setPreserved(row, col, false);
				setMine(row, col, mapDataView[row, col].mine);
				setForest(row, col, mapDataView[row, col].forest);
				setBuildWall(row, col, false);
				setBuildGate(row, col, false);
				glm::vec3 worldPos;
				land->cellToWorld(row, col, worldPos);
				long cellWaterType = TerrainRuntime::sampleWaterClass(worldPos);
				if (cellWaterType == std::to_underlying(WATER_TYPE_SHALLOW))
					setShallowWater(row, col, true);
				if (cellWaterType == std::to_underlying(WATER_TYPE_DEEP))
					setDeepWater(row, col, true);
				for (long i = 0; i < NUM_MOVE_LEVELS; i++)
					GameMap->setPathlock(i, row, col, false);
				switch (mapDataView[row, col].specialType) {
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
	rev.statics++;
}

void MissionMap::spreadState (long cellRow, long cellCol, long radius) {

	// This does a simple depth-first spread from the center. The way it's coded,
	// it essentially grows a box from the center cell.
	if (inMapBounds(cellRow, cellCol, height, width) && (radius > 0)) {
		std::mdspan<MapCell, std::dextents<std::size_t, 2>> mapView(map.data(), static_cast<std::size_t>(height), static_cast<std::size_t>(width));
		// If we're placing moving objects for this frame only, we'll want to
		// preserve the cell states of this tile...
		if (!getPreserved(cellRow, cellCol))
			setPreserved(cellRow, cellCol, true);
		mapView[cellRow, cellCol].setPassable(false);
		rev.preserved++;   // a mover's per-frame occupancy of a preserved tile; setPreserved above only bumps the first time
		for (long dir = 0; dir < NUM_DIRECTIONS; dir++) {
			long adjR = cellRow;
			long adjC = cellCol;
			calcAdjNode(adjR, adjC, dir);
			spreadState(adjR, adjC, radius - 1);
		}
	}
}

long MissionMap::placeObject (glm::vec3 position, float radius) {

	int cellRow, cellCol;
	land->worldToCell(position, cellRow, cellCol);

	// Now, set the cell states based upon the object's size...
	float cellRadius = (radius / (Terrain::worldUnitsPerCell  * metersPerWorldUnit));
	if ((cellRadius > 0.5) && (cellRadius < 1.0))
		cellRadius = 1.0;
	spreadState(cellRow, cellCol, static_cast<long>(cellRadius));
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
	std::mdspan<MapCell, std::dextents<std::size_t, 2>> mapView(map.data(), static_cast<std::size_t>(height), static_cast<std::size_t>(width));
	
	for (long cellR = 0; cellR < height; cellR++) {
		char outString[1024];
		outString[0] = '\0';
		for (long cellC = 0; cellC < width; cellC++) {
			if (!mapView[cellR, cellC].getPassable())
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

	map = {};
	tileMineCount = {};
	rev.statics++;
}

// MOVE PATH class





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

float MovePath::getDistanceLeft (glm::vec3 position, long stepNumber) {

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
