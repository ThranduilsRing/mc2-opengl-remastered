// Pathfinding: MoveMap setup: start/goal/target marking and the two setUp variants.
// Declared in the class headers; shared names in move_internal.h.
#include "move_internal.h"



void* MoveMap::operator new (size_t ourSize) {

	void* result = systemHeap->Malloc(ourSize);
	return(result);
}

void MoveMap::operator delete (void* us) {

	systemHeap->Free(us);
}	

void MoveMap::init (long maxW, long maxH) {

	width = maxWidth = maxW;
	height = maxHeight = maxH;
	long mapByteSize = sizeof(MoveMapNode) * maxWidth * maxHeight;
	map = (MoveMapNodePtr)systemHeap->Malloc(mapByteSize);
	gosASSERT(map != nullptr);

	mapRowStartTable = (int*)systemHeap->Malloc(maxHeight * sizeof(int));
	gosASSERT(mapRowStartTable != nullptr);
	for (long r = 0; r < maxHeight; r++)
		mapRowStartTable[r] = r * maxWidth;

	mapRowTable = (int*)systemHeap->Malloc(maxHeight * maxWidth * sizeof(int));
	gosASSERT(mapRowTable != nullptr);
	mapColTable = (int*)systemHeap->Malloc(maxHeight * maxWidth * sizeof(int));
	gosASSERT(mapColTable != nullptr);
	for (int r = 0; r < maxHeight; r++)
		for (int c = 0; c < maxWidth; c++) {
			int index = mapRowStartTable[r] + c;
			mapRowTable[index] = r;
			mapColTable[index] = c;
		}

	for (int r = 0; r < maxHeight; r++)
		for (int c = 0; c < maxWidth; c++) {
			int mapCellIndex = r * maxWidth + c;
			for (int d = 0; d < NUM_ADJ_CELLS; d++) {
				int indexStart = d * 2;
				int adjRow = r + cellShift[indexStart];
				int adjCol = c + cellShift[indexStart + 1];
				if (inMapBounds(adjRow, adjCol, height, width))
					map[mapCellIndex].adjCells[d] = adjRow * maxWidth + adjCol;
				else
					map[mapCellIndex].adjCells[d] = -1;
			}
		}

	float cellLength = Terrain::worldUnitsPerCell * metersPerWorldUnit;
	for (long i = 0; i < DISTANCE_TABLE_DIM; i++)
		for (long j = 0; j < DISTANCE_TABLE_DIM; j++) {
			distanceFloat[i][j] = agsqrt(i, j) * cellLength;
			distanceInt[i][j] = (int)distanceFloat[i][j];
		}

	clear();
}

void MoveMap::clear (void) {

	int numMapCells = maxWidth * height;
	int initHPrime = ZeroHPrime ? 0 : HPRIME_NOT_CALCED;
	for (int i = 0; i < numMapCells; i++) {
		MoveMapNodePtr node = &map[i];
		// DON'T NEED TO SET THIS SINCE IS SET IN SETUP...
		node->cost = COST_BLOCKED;
		node->parent = -1;
		node->flags = 0;
		node->hPrime = initHPrime;
	}

	goal.Zero();
	target.x = -999999.0;
	target.y = -999999.0;
	target.z = -999999.0;
}

void MoveMap::setTarget (Stuff::Vector3D targetPos) {

	target = targetPos;
}

void MoveMap::setStart (Stuff::Vector3D* startPos, long startRow, long startCol) {

	if (startPos)
		start = *startPos;
	else {
		start.x = -999999.0;
		start.y = -999999.0;
		start.z = -999999.0;
	}
	if (startRow == -1) {
		land->worldToCell(*startPos, startR, startC);
		startR -= ULr;
		startC -= ULc;
		}
	else {
		startR = startRow;
		Assert(startRow > -1, 0, " HUH ");
		startC = startCol;
	}
}

void MoveMap::setGoal (Stuff::Vector3D goalPos, long goalRow, long goalCol) {

	goal = goalPos;
	if (goalRow == -1) {
		land->worldToCell(goal, goalR, goalC);
		goalR -= ULr;
		goalC -= ULc;
		}
	else {
		goalR = goalRow;
		goalC = goalCol;
	}
	doorDirection = -1;
	GoalIsDoor = false;
}

inline void adjustMoveMapCellCost (MoveMapNodePtr cell, long costAdj) {

	long cost = cell->cost + costAdj;
	if (cost < 1)
		cost = 1;
	cell->cost = cost;
}

void MoveMap::setGoal (long thruArea, long goalDoor) {

	// We need to set goalR, goalC for the calcHPrime routine (until we come up with
	// a better one that uses the door range. If the global path's final goal is on
	// the other side of this door, we should select THAT cell of the door...
	goal.x = -999999.0;
	goal.y = -999999.0;
	goal.z = -999999.0;

	GoalIsDoor = true;
	door = goalDoor;
	doorSide = (GlobalMoveMap[moveLevel]->doors[goalDoor].area[1] == thruArea);
	long doorDirTable[4][2] = {
		{-1, -1},
		{1, 3},
		{2, 0},
		{-1, -1}
	};

	long goalDoorDir = GlobalMoveMap[moveLevel]->doors[goalDoor].direction[0];
	gosASSERT((goalDoorDir == 1) || (goalDoorDir == 2));
	doorDirection = doorDirTable[goalDoorDir][doorSide];

	long doorLength = GlobalMoveMap[moveLevel]->doors[goalDoor].length;

	if ((doorDirection == 0) || (doorDirection == 2)) {
		// We need to set goalR and goalC for the calcHPrime function used
		// in pathfinding...
		goalR = GlobalMoveMap[moveLevel]->doors[goalDoor].row + doorSide - ULr;
		goalC = GlobalMoveMap[moveLevel]->doors[goalDoor].col + (doorLength / 2) - ULc;
		}
	else if ((doorDirection == 1) || (doorDirection == 3)) {
		// We need to set goalR and goalC for the calcHPrime function used
		// in pathfinding...
		goalR = GlobalMoveMap[moveLevel]->doors[goalDoor].row + (doorLength / 2) - ULr;
		goalC = GlobalMoveMap[moveLevel]->doors[goalDoor].col + doorSide - ULc;
	}
}

inline long MoveMap::markGoals (Stuff::Vector3D finalGoal) {

	// First, mark the blocked goal cells...
	// PATHFINDING-SOLVER-ISOLATION-1: was function-local static; made stack-
	// local. Fully (re)initialized [0..length) each call before use, so this is
	// behavior-identical single-threaded and removes a per-solve shared static.
	char doorCellState[1024];
	for (int j = 0; j < GlobalMoveMap[moveLevel]->doors[door].length; j++)
		doorCellState[j] = 1;

	if (blockedDoorCallback)
		(*blockedDoorCallback)(moveLevel, door, &doorCellState[0]);

	// Ultimately, we should do this conversion once for the finalGoal and then
	// store it in the moveOrders data...
	int finalGoalR, finalGoalC;
	land->worldToCell(finalGoal, finalGoalR, finalGoalC);
	finalGoalR -= ULr;
	finalGoalC -= ULc;

	int doorLength = GlobalMoveMap[moveLevel]->doors[door].length;

	int numGoalCells = 0;
	if ((doorDirection == 0) || (doorDirection == 2)) {
		int cellR = goalR;
		int cellC = goalC - (doorLength / 2);
		bool nextToGoal = false;
		if (doorSide == 0) {
			if (finalGoalR == (cellR + 1))
					if ((finalGoalC >= cellC) && (finalGoalC < (cellC + doorLength))) {
						map[cellR * width + finalGoalC].setFlag(MOVEFLAG_GOAL);
						numGoalCells++;
						nextToGoal = true;
					}
			}
		else {
			if (finalGoalR == (cellR - 1))
					if ((finalGoalC >= cellC) && (finalGoalC < (cellC + doorLength))) {
						map[cellR * maxWidth + finalGoalC].setFlag(MOVEFLAG_GOAL);
						numGoalCells++;
						nextToGoal = true;
					}
		}

		if (!nextToGoal) {
			long adjCost = clearCost / 2;
			long doorCenter = doorLength / 2;
			long cellIndex = cellR * maxWidth + cellC;
			long curCost = doorCenter * adjCost;
			for (long c = 0; c < doorCenter; c++) {
				if (doorCellState[c]) {
					map[cellIndex].setFlag(MOVEFLAG_GOAL);
					numGoalCells++;
					adjustMoveMapCellCost(&map[cellIndex], curCost);
				}
				cellIndex++;
				curCost -= adjCost;
			}
			cellIndex = cellR * maxWidth + cellC + doorCenter;
			curCost = 0;
			for (int c = doorCenter; c < doorLength; c++) {
				if (doorCellState[c]) {
					map[cellIndex].setFlag(MOVEFLAG_GOAL);
					numGoalCells++;
					adjustMoveMapCellCost(&map[cellIndex], curCost);
				}
				cellIndex++;
				curCost += adjCost;
			}
		}
		}
	else if ((doorDirection == 1) || (doorDirection == 3)) {
		long cellR = goalR - (doorLength / 2);
		long cellC = goalC;
		bool nextToGoal = false;
		if (finalGoalC == (cellC + 1 - doorSide * 2))
			if ((finalGoalR >= cellR) && (finalGoalR < (cellR + doorLength))) {
				map[finalGoalR * maxWidth + cellC].setFlag(MOVEFLAG_GOAL);
				numGoalCells++;
				nextToGoal = true;
			}
			
		if (!nextToGoal) {
			long adjCost = clearCost / 2;
			long doorCenter = doorLength / 2;
			long cellIndex = cellR * maxWidth + cellC;
			long curCost = doorCenter * adjCost;
			for (long r = 0; r < doorCenter; r++) {
				if (doorCellState[r]) {
					map[cellIndex].setFlag(MOVEFLAG_GOAL);
					numGoalCells++;
					adjustMoveMapCellCost(&map[cellIndex], curCost);
				}
				cellIndex += maxWidth;
				curCost -= adjCost;
			}
			curCost = 0;
			cellIndex = (cellR + doorCenter) * maxWidth + cellC;
			for (int r = doorCenter; r < doorLength; r++) {
				if (doorCellState[r]) {
					map[cellIndex].setFlag(MOVEFLAG_GOAL);
					numGoalCells++;
					adjustMoveMapCellCost(&map[cellIndex], curCost);
				}
				cellIndex += maxWidth;
				curCost += adjCost;
			}
		}
	}
	
	return(numGoalCells);
}

inline long MoveMap::markEscapeGoals (Stuff::Vector3D finalGoal) {

	long numGoalCells = 0;

	int finalGoalR, finalGoalC;
	land->worldToCell(finalGoal, finalGoalR, finalGoalC);
	int finalGoalArea = GlobalMoveMap[moveLevel]->calcArea(finalGoalR, finalGoalC);

	// For each tile, mark its cells as valid goals if:
	//		1) the tile's areaId == the areaId of the finalGoal
	//		2) OR, if a LR path exists between the tile and the finalGoal tile
	int cellIndex = 0;
	for (int row = 0; row < height; row++)
		for (int col = 0; col < width; col++) {
			if (GameMap->inBounds(ULr + row, ULc + col)) {
				int curArea = GlobalMoveMap[moveLevel]->calcArea(ULr + row, ULc + col);
				int confidence;
				int numLRSteps = GlobalMoveMap[moveLevel]->getPathCost(curArea, finalGoalArea, false, confidence, true);
				bool validGoal = (numLRSteps > 0);
				if (validGoal)
					map[cellIndex].setFlag(MOVEFLAG_GOAL);
			}
			cellIndex++;
		}

	return(numGoalCells);
}

long MoveMap::setUp (long mapULr,
					 long mapULc,
					 long mapWidth,
					 long mapHeight,
					 long level,
					 Stuff::Vector3D* startPos,
					 long startRow,
					 long startCol,
					 Stuff::Vector3D goalPos,
					 long goalRow,
					 long goalCol,
					 long clearCellCost,
					 long jumpCellCost,
					 long offsets,
					 unsigned long params) {

	ZoneScopedN("GameLogic.PathManager.CalcPath1");

	// If the map has not been allocated yet, then the tile height and width passed
	// is used as both the max and current dimensions. Otherwise, they are only
	// used as the current dimensions...
	if ((params & MOVEPARAM_JUMP) && (jumpCellCost > 0))
		jumpCellCost = clearCellCost;
	if (!map) {
		init(mapHeight, mapWidth);
		setClearCost(clearCellCost);
		setJumpCost(jumpCellCost, offsets);
		}
	else {
		width = mapWidth;
		height = mapHeight;
		setClearCost(clearCellCost);
		setJumpCost(jumpCellCost, offsets);
		clear();
	}

	thruAreas[0] = -1;
	thruAreas[1] = -1;

	ULr = mapULr;
	ULc = mapULc;
	minRow = 0;
	minCol = 0;
	maxRow = mapHeight - 1;
	maxCol = mapWidth - 1;
	moveLevel = level;

	startRow -= ULr;
	startCol -= ULc;
	Assert(startRow > -1, 0, " huh ");
	setStart(startPos, startRow, startCol);
	if (FindingEscapePath) {
		setGoal(goalPos, goalRow, goalCol);
		}
	else
		setGoal(goalPos, goalRow, goalCol);

	travelOffMap = false;
	cannotEnterOffMap = true;
	if (GameMap->getOffMap(ULr + startRow, ULc + startCol))
		travelOffMap = true;
	if (moverWithdrawing) {
		travelOffMap = true;
		cannotEnterOffMap = false;
	}

	bool followRoads = ((params & MOVEPARAM_FOLLOW_ROADS) != 0);
	bool traverseShallowWater = ((params & (MOVEPARAM_WATER_SHALLOW + MOVEPARAM_WATER_DEEP)) != 0);
	bool traverseDeepWater = ((params & MOVEPARAM_WATER_DEEP) != 0);
	bool avoidMines = true;
	if (params & MOVEPARAM_SWEEP_MINES)
		avoidMines = false;

	long pathLockCost = clearCost << 3;

	bool groundMover = ((moveLevel == 0) || (moveLevel == 1));
	for (long cellRow = 0; cellRow < height; cellRow++)
		for (long cellCol = 0; cellCol < width; cellCol++) {			
			if (GameMap->inBounds(ULr + cellRow, ULc + cellCol)) {
				MapCellPtr mapCell = GameMap->getCell(ULr + cellRow, ULc + cellCol);
				long moveMapIndex = cellRow * maxWidth + cellCol;

				long cost = clearCost;
				bool offMapCell = mapCell->getOffMap();
				if (offMapCell)
					map[moveMapIndex].setFlag(MOVEFLAG_OFFMAP);

				if (offMapCell && !travelOffMap)
					cost = COST_BLOCKED;
				else if (groundMover) {
					if (mapCell->getShallowWater()) {
						if (!traverseShallowWater)
							cost = COST_BLOCKED;
						else if (!mapCell->getPassable())
							cost = COST_BLOCKED;
						}
					else if (mapCell->getDeepWater()) {
						if (!traverseDeepWater)
							cost = COST_BLOCKED;
						else if (!mapCell->getPassable())
							cost = COST_BLOCKED;
						}
					else if (mapCell->getGate()) 
					{
						long areaID = GlobalMoveMap[moveLevel]->calcArea(ULr + cellRow, ULc + cellCol);
						long teamID = -1;
						if (areaID > -1) 
						{
							teamID = GlobalMoveMap[moveLevel]->areas[areaID].teamID;
							//Its possible for the ownerWIDs to be invalid for one or two frames after a quick save.
							// We handle this ok later on!!  OwnerWIDs restablish themselves right after the first unpaused update!
						}
						if (!EditorSave && (areaID > -1) && GlobalMoveMap[moveLevel]->isGateDisabledCallback(GlobalMoveMap[moveLevel]->areas[areaID].ownerWID))
							cost = COST_BLOCKED;
						else if ((teamID > -1) && (TeamRelations[teamID][moverTeamID] != RELATION_FRIENDLY)) {
							if (mapCell->getPassable())
								cost <<= 2;
							else
								cost = COST_BLOCKED;
							}
						else if (mapCell->getRoad() && followRoads)
							cost >>= 2;
						}
					else {
						if (!mapCell->getPassable())
							cost = COST_BLOCKED;
						else if (mapCell->getRoad() && followRoads)
							cost >>= 2;
					}
					if (mapCell->getForest())
						cost += forestCost;
				}
				map[moveMapIndex].cost = cost;

				// NOTE: With gates, we may want them to set the cell cost rather
				// than just adjust it. Let's see how they play. Since they're
				// set as an overlay, we'll just treat them as such for now.
				
				if (mapCell->getPathlock(moveLevel == 2))
					adjustMoveMapCellCost(&map[cellRow * maxWidth + cellCol], pathLockCost);
			}
		}

	if (FindingEscapePath)
		markEscapeGoals(goalPos);
	else
		map[goalR * maxWidth + goalC].setFlag(MOVEFLAG_GOAL);

	if (params & MOVEPARAM_STATIONARY_MOVERS)
		if (placeStationaryMoversCallback)
			(*placeStationaryMoversCallback)(this);

	return(NO_ERR);
}

long MoveMap::setUp (long level,
					 Stuff::Vector3D* startPos,
					 long startRow,
					 long startCol,
					 long thruArea[2],
					 long goalDoor,
					 Stuff::Vector3D finalGoal,
					 long clearCellCost,
					 long jumpCellCost,
					 long offsets,
					 unsigned long params) {

	ZoneScopedN("GameLogic.PathManager.CalcPath2");

	if (!map)
		init(SECTOR_DIM*2, SECTOR_DIM*2);
	else {
		width = SECTOR_DIM*2;
		height = SECTOR_DIM*2;
		clear();
	}

	thruAreas[0] = thruArea[0];
	thruAreas[1] = thruArea[1];

	long firstSectorRow = GlobalMoveMap[level]->areas[thruArea[0]].sectorR;
	long firstSectorCol = GlobalMoveMap[level]->areas[thruArea[0]].sectorC;
	if (thruArea[1] == -1) {
		ULr = firstSectorRow * SECTOR_DIM;
		ULc = firstSectorCol * SECTOR_DIM;
		minRow = 0;
		minCol = 0;
		maxRow = SECTOR_DIM - 1;
		maxCol = SECTOR_DIM - 1;
		}
	else {
		long secondSectorRow = GlobalMoveMap[level]->areas[thruArea[1]].sectorR;
		long secondSectorCol = GlobalMoveMap[level]->areas[thruArea[1]].sectorC;
		if (secondSectorRow > firstSectorRow) {
			if (secondSectorCol == firstSectorCol) {
				ULr = firstSectorRow * SECTOR_DIM;
				ULc = firstSectorCol * SECTOR_DIM;
				minRow = 0;
				minCol = 0;
				maxRow = SECTOR_DIM * 2 - 1;
				maxCol = SECTOR_DIM - 1;
				}
			else
				STOP(("MoveMap.setup: diagonal sectors not allowed #1"));
			}
		else if (secondSectorRow < firstSectorRow) {
			if (secondSectorCol == firstSectorCol) {
				ULr = secondSectorRow * SECTOR_DIM;
				ULc = secondSectorCol * SECTOR_DIM;
				minRow = 0;
				minCol = 0;
				maxRow = SECTOR_DIM * 2 - 1;
				maxCol = SECTOR_DIM - 1;
				}
			else
				STOP(("MoveMap.setup: diagonal sectors not allowed #2"));
			}
		else {
			if (secondSectorCol > firstSectorCol) {
				ULr = firstSectorRow * SECTOR_DIM;
				ULc = firstSectorCol * SECTOR_DIM;
				minRow = 0;
				minCol = 0;
				maxRow = SECTOR_DIM - 1;
				maxCol = SECTOR_DIM * 2 - 1;
				}
			else if (secondSectorCol < firstSectorCol) {
				ULr = secondSectorRow * SECTOR_DIM;
				ULc = secondSectorCol * SECTOR_DIM;
				minRow = 0;
				minCol = 0;
				maxRow = SECTOR_DIM - 1;
				maxCol = SECTOR_DIM * 2 - 1;
				}
			else {
				ULr = firstSectorRow * SECTOR_DIM;
				ULc = firstSectorCol * SECTOR_DIM;
				minRow = 0;
				minCol = 0;
				maxRow = SECTOR_DIM - 1;
				maxCol = SECTOR_DIM - 1;
			}
		}
	}

	moveLevel = level;

	setClearCost(clearCellCost);
	if ((params & MOVEPARAM_JUMP) && (jumpCellCost > 0))
		jumpCellCost = clearCellCost;
	setJumpCost(jumpCellCost, offsets);

	startRow -= ULr;
	startCol -= ULc;
	setStart(startPos, startRow, startCol);
	if (thruArea[1] == -1)
		setGoal(thruArea[0], goalDoor);
	else
		setGoal(thruArea[1], goalDoor);

	long pathLockCost = clearCost << 3;

	travelOffMap = false;
	cannotEnterOffMap = true;
	if (GameMap->getOffMap(ULr + startRow, ULc + startCol))
		travelOffMap = true;
	if (moverWithdrawing) {
		travelOffMap = true;
		cannotEnterOffMap = false;
	}

	bool followRoads = ((params & MOVEPARAM_FOLLOW_ROADS) != 0);
	bool traverseShallowWater = ((params & (MOVEPARAM_WATER_SHALLOW + MOVEPARAM_WATER_DEEP)) != 0);
	bool traverseDeepWater = ((params & MOVEPARAM_WATER_DEEP) != 0);
	bool avoidMines = true;
	if (params & MOVEPARAM_SWEEP_MINES)
		avoidMines = false;

	bool groundMover = ((moveLevel == 0) || (moveLevel == 1));
	for (long cellRow = 0; cellRow < height; cellRow++)
		for (long cellCol = 0; cellCol < width; cellCol++) {			
			if (GameMap->inBounds(ULr + cellRow, ULc + cellCol) && inBounds(cellRow, cellCol)) {
				MapCellPtr mapCell = GameMap->getCell(ULr + cellRow, ULc + cellCol);

				long moveMapIndex = cellRow * maxWidth + cellCol;

				long cost = clearCost;
				bool offMapCell = mapCell->getOffMap();
				if (offMapCell)
					map[moveMapIndex].setFlag(MOVEFLAG_OFFMAP);

				long areaID = GlobalMoveMap[moveLevel]->calcArea(ULr + cellRow, ULc + cellCol);
				if (CullPathAreas && (areaID != thruAreas[0]) && (areaID != thruAreas[1]))
					cost = COST_BLOCKED;
				else if (offMapCell && !travelOffMap)
					cost = COST_BLOCKED;
				else if (groundMover) {
					if (mapCell->getShallowWater()) {
						if (!traverseShallowWater)
							cost = COST_BLOCKED;
						else if (!mapCell->getPassable())
							cost = COST_BLOCKED;
						}
					else if (mapCell->getDeepWater()) {
						if (!traverseDeepWater)
							cost = COST_BLOCKED;
						else if (!mapCell->getPassable())
							cost = COST_BLOCKED;
						}
					else if (mapCell->getGate()) {
						long areaID = GlobalMoveMap[moveLevel]->calcArea(ULr + cellRow, ULc + cellCol);
						long teamID = -1;
						if (areaID > -1) {
							teamID = GlobalMoveMap[moveLevel]->areas[areaID].teamID;
						}
						if (!EditorSave && (areaID > -1) && GlobalMoveMap[moveLevel]->isGateDisabledCallback(GlobalMoveMap[moveLevel]->areas[areaID].ownerWID))
							cost = COST_BLOCKED;
						else if ((teamID > -1) && (TeamRelations[teamID][moverTeamID] != RELATION_FRIENDLY)) {
							if (mapCell->getPassable())
								cost <<= 2;
							else
								cost = COST_BLOCKED;
							}
						else if (mapCell->getRoad() && followRoads)
							cost >>= 2;
						}
					else {
						if (!mapCell->getPassable())
							cost = COST_BLOCKED;
						else if (mapCell->getRoad() && followRoads)
							cost >>= 2;
					}
					if (mapCell->getForest())
						cost += forestCost;
				}
				setCost(cellRow, cellCol, cost);

				if (mapCell->getPathlock(moveLevel == 2))
					adjustMoveMapCellCost(&map[cellRow * maxWidth + cellCol], pathLockCost);
			}
		}

	if (markGoals(finalGoal) == 0)
		return(-1);

	if (params & MOVEPARAM_STATIONARY_MOVERS)
		if (placeStationaryMoversCallback)
			(*placeStationaryMoversCallback)(this);

	return(NO_ERR);
}
