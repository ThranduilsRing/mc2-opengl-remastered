// Pathfinding: MoveMap::calcEscapePath, writeDebug and destroy.
// Declared in the class headers; shared names in move_internal.h.
#include "move_internal.h"


long MoveMap::calcEscapePath (MovePathPtr path, Stuff::Vector3D* goalWorldPos, long* goalCell) {

	mc2_move::SolveContext ctx;

	mc2_path_trace::SolveScope _pt("escape");
	const bool _pathTrace = _pt.active;
	long _ptNodes = 0;
	long _ptOpenPeak = 0;

	#ifdef TIME_PATH
		L_INTEGER calcStart, calcStop;
		QueryPerformanceCounter(calcStart);
	#endif


	// If the start tile is blocked, let's clear it so we may move off of
	// it, at least. Change the blocked tiles to just barely passable...

/*
	if ((goalR < 0) || (goalR >= cellHeight) || (goalC < 0) || (goalC >= cellWidth)) {
		Stuff::Vector3D p;
		p.x = (float)(goalC) * MetersPerCell + MetersPerCell / 2 - Terrain::worldUnitsMapSide / 2;
		p.y = (Terrain::worldUnitsMapSide / 2) - ((float)(goalR) * MetersPerCell) - MetersPerCell / 2;
		p.z = (float)0; // How do we get the elevation for this point? Do we care?
		char msg[200];
		sprintf(msg, " Bad Move Goal: %d [%d(%d), %d(%d)], (%.2f, %.2f, %.2f)", DebugMovePathType, goalR, cellHeight, goalC, cellWidth, p.x, p.y, p.z);
		Fatal(0, msg);
	}
*/

	// Let's use their hPrime as a barrier for cutting off the search...
	MaxHPrime = 500; //float2short(calcHPrime(startR, startC) * 2.5);
	if (MaxHPrime < 500)
		MaxHPrime = 500;
	
	if (!ctx.isolated && !openList) {
		openList = new PriorityQueue;
		gosASSERT(openList != NULL);
		openList->init(5000);
	}
	solveOpen = ctx.isolated ? ctx.pq() : openList;
	solveJumpOnBlocked = ctx.jumpOnBlocked();

	long curCol = startC;
	long curRow = startR;

	MoveMapNodePtr curMapNode = &map[curRow * maxWidth + curCol];
	curMapNode->g = 0;
	curMapNode->hPrime = 10; //calcHPrime(curRow, curCol);
	curMapNode->fPrime = curMapNode->hPrime;

	PQNode initialVertex;
	initialVertex.key = curMapNode->fPrime;
	initialVertex.id = curRow * MAX_MAPWIDTH + curCol;
	initialVertex.row = curRow;
	initialVertex.col = curCol;
	solveOpen->clear();
#ifdef _DEBUG
	long insertErr = 
#endif
		solveOpen->insert(initialVertex);
	gosASSERT(insertErr == NO_ERR);
	curMapNode->setFlag(MOVEFLAG_OPEN);

	bool goalFound = false;
	long bestRow = -1;
	long bestCol = -1;

	#ifdef DEBUG_PATH
		topOpenNodes = 1;
		numNodesVisited = 1;
	#endif

	while (!solveOpen->isEmpty()) {

		#ifdef DEBUG_MOVE_MAP
			if (debugMoveMap) {
				File* pathDebugFile = new File;
					pathDebugFile->create("mm.dbg");
				writeDebug(pathDebugFile);
				pathDebugFile->close();
				delete pathDebugFile;
				pathDebugFile = NULL;
			}
		#endif

		#ifdef DEBUG_PATH
			if (topOpenNodes < solveOpen->getNumItems())
				topOpenNodes = solveOpen->getNumItems();
		#endif

		if (_pathTrace) {
			long openN = solveOpen->getNumItems();
			if (openN > _ptOpenPeak) _ptOpenPeak = openN;
		}

		PQNode bestPQNode;
		solveOpen->remove(bestPQNode);
		if (_pathTrace) _ptNodes++;
		bestRow = bestPQNode.row;
		bestCol = bestPQNode.col;
		MoveMapNodePtr bestMapNode = &map[bestRow * maxWidth + bestCol];
		bestMapNode->clearFlag(MOVEFLAG_OPEN);

		long bestNodeG = bestMapNode->g;

		bestMapNode->setFlag(MOVEFLAG_CLOSED);
		
		if (bestMapNode->flags & MOVEFLAG_GOAL) {
			goalFound = true;
			break;
		}
		
		long cellOffsetIndex = 0;
		for (long dir = 0; dir < numOffsets; dir++) {
			// First, make sure this is a legit direction to go. We do NOT
			// want to clip corners, so we'll check that here...
			bool isDiagonalWalk = IsDiagonalStep[dir];
			if (isDiagonalWalk) {
				if (!adjacentCellOpenJUMP(bestRow, bestCol, StepAdjDir[dir]) && !adjacentCellOpenJUMP(bestRow, bestCol, StepAdjDir[dir + 1])) {
					cellOffsetIndex += 2;
					continue;
				}
			}

			long succRow = bestRow + cellShift[cellOffsetIndex++];
			long succCol = bestCol + cellShift[cellOffsetIndex++];
			if (succRow > 0 && succRow < height && succCol > 0 && succCol < width) {

				MoveMapNodePtr succMapNode = &map[succRow * maxWidth + succCol];
				if (succMapNode->cost < COST_BLOCKED) {
					if (succMapNode->hPrime == HPRIME_NOT_CALCED)
						succMapNode->hPrime = 10; //calcHPrime(succRow, succCol);

					if (succMapNode->hPrime < MaxHPrime) {

						#ifdef DEBUG_PATH
							numNodesVisited++;
						#endif
				
						char dirToParent = reverseShift[dir];

						bool jumping = false;
						long cost = succMapNode->cost;
						if (dir > 7) {
							jumping = true;
							if (solveJumpOnBlocked)   // PATHFINDING-SOLVER-ISOLATION-1
								cost = jumpCost;
							else
								cost += jumpCost;
							}
						else {
							if (isDiagonalWalk)
								cost += (cost / 2);
						}
						gosASSERT(cost > 0);
						
						long succNodeG = bestNodeG + cost;

						if (succMapNode->flags & MOVEFLAG_OPEN) {
							if (succNodeG < succMapNode->g) {
								succMapNode->parent = dirToParent;
								succMapNode->g = succNodeG;
								succMapNode->fPrime = succNodeG + succMapNode->hPrime;
								long openIndex = solveOpen->find(succRow * MAX_MAPWIDTH + succCol);
								if (!openIndex) {
									char s[128];
									sprintf(s, "MoveMap.calcEscapePath: Cannot find movemap node [%d, %d, %d, %d] for change\n", succRow, succCol, succRow * MAX_MAPWIDTH + succCol, dir);
									gosASSERT(openIndex != 0);
									}
								else
									solveOpen->change(openIndex, succMapNode->fPrime);
							}
							}
						else if (succMapNode->flags & MOVEFLAG_CLOSED) {
							if (succNodeG < succMapNode->g) {
								succMapNode->parent = dirToParent;

								propogateCostJUMP(succRow, succCol, cost, bestNodeG);
							}
							}
						else {
							succMapNode->parent = dirToParent;
							succMapNode->g = succNodeG;
							succMapNode->fPrime = succNodeG + succMapNode->hPrime;
							PQNode succPQNode;
							succPQNode.key = succMapNode->fPrime;
							succPQNode.id = succRow * MAX_MAPWIDTH + succCol;
							succPQNode.row = succRow;
							succPQNode.col = succCol;
#ifdef _DEBUG
							long insertErr = 
#endif
								solveOpen->insert(succPQNode);
							gosASSERT(insertErr == NO_ERR);
							succMapNode->setFlag(MOVEFLAG_OPEN);
						}
					}
				}
			}
		}
	}

	#ifdef DEBUG_MOVE_MAP
		if (debugMoveMap) {
			File* pathDebugFile = new File;
			pathDebugFile->create("mm.dbg");
			writeDebug(pathDebugFile);
			pathDebugFile->close();
			delete pathDebugFile;
			pathDebugFile = NULL;
		}
	#endif

	if (goalFound) {
		long curRow = goalCell[0] = (long)bestRow;
		long curCol = goalCell[1] = (long)bestCol;
		long numCells = 0;
		while ((curRow != startR) || (curCol != startC)) {
			numCells += 1;
			long cellOffsetIndex = map[curRow * maxWidth + curCol].parent * 2;
			curRow += cellShift[cellOffsetIndex++];
			curCol += cellShift[cellOffsetIndex];
		}
		// If our goal is a door, the path will be one step longer, since
		// we need to walk "thru" the door...
		if (doorDirection != -1)
			numCells++;

		if (_pathTrace) {
			_pt.len = (int)numCells;
			_pt.fail = 0;
			_pt.nodes = _ptNodes;
			_pt.openPeak = _ptOpenPeak;
			if (numCells > MAX_STEPS_PER_MOVEPATH)
				_pt.trunc = 1;
		}

#ifdef _DEBUG
		if (numCells > MAX_STEPS_PER_MOVEPATH) {
			File* pathDebugFile = new File;
			pathDebugFile->create("longpath.dbg");
			char s[512];
			sprintf(s, "Path Too Long: %d steps (Max = %d) (please save longpath.dbg file:)", numCells, MAX_STEPS_PER_MOVEPATH);
			pathDebugFile->writeString(s);
			pathDebugFile->writeString("\n\n");
			writeDebug(pathDebugFile);
			pathDebugFile->close();
			delete pathDebugFile;
			pathDebugFile = NULL;
			gosASSERT(numCells <= MAX_STEPS_PER_MOVEPATH);
		}
#endif

		path->init();
		if (numCells) {
			
#ifdef _DEBUG			
			long maxSteps = path->init(numCells);

			if (maxSteps > -1) {
				File* pathDebugFile = new File;
				pathDebugFile->create("longpath.dbg");
				char s[512];
				sprintf(s, "New Longest Path: %d steps (Max = %d)\n\n", numCells, MAX_STEPS_PER_MOVEPATH);
				pathDebugFile->writeString(s);
				writeDebug(pathDebugFile);
				pathDebugFile->close();
				delete pathDebugFile;
				pathDebugFile = NULL;
			}
#endif
			path->target = target;
			path->cost = map[bestRow * maxWidth + bestCol].g;
			curRow = (long)bestRow;
			curCol = (long)bestCol;
			long curCell = numCells;
			if (doorDirection == -1) {
				if (goalWorldPos) {
					goalWorldPos->x = (float)(ULc + bestCol) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
					goalWorldPos->y = (Terrain::worldUnitsMapSide / 2) - ((float)(ULr + bestRow) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
					goalWorldPos->z = (float)0; // How do we get the elevation for this point? Do we care?
					path->goal = *goalWorldPos;
					}
				else
					path->goal = goal;
				}
			else {
				curCell--;
				path->setDirection(curCell, /*reverseShift[*/doorDirection * 2/*]*/);
				long doorR = bestRow + adjTile[doorDirection][0];
				long doorC = bestCol + adjTile[doorDirection][1];

				goalCell[0] = ULr + doorR;
				goalCell[1] = ULc + doorC;
				Stuff::Vector3D stepDest;
				stepDest.x = (float)(ULc + doorC) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
				stepDest.y = (Terrain::worldUnitsMapSide / 2) - ((float)(ULr + doorR) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
				stepDest.z = (float)0; // How do we get the elevation for this point? Do we care?
				path->setDestination(curCell, stepDest);
				path->setDistanceToGoal(curCell, 0.0);
				path->setCell(curCell, goalCell[0], goalCell[1]);
				path->goal = stepDest;
				if (goalWorldPos) {
					*goalWorldPos = stepDest;
				}
			}
			
			// We need to set this table up our first time thru here...
			// PATHFINDING-SOLVER-ISOLATION-1: ON skips the lazy static guard
			// (table built at MOVE_init). OFF unchanged.
			if (!ctx.isolated) {
				static bool setTable = false;
				if (!setTable) {
					float cellLength = Terrain::worldUnitsPerCell * metersPerWorldUnit;
					for (long i = 0; i < NUM_CELL_OFFSETS; i++) {
						float distance = agsqrt( cellShift[i * 2], cellShift[i * 2 + 1] ) * cellLength;
						cellShiftDistance[i] = distance;
					}
					setTable = true;
				}
			}
			
			while ((curRow != startR) || (curCol != startC)) {
				curCell--;
				long parent = reverseShift[map[curRow * maxWidth + curCol].parent];
				if (parent > 7)
					path->setDirection(curCell, parent);
				else
					path->setDirection(curCell, parent);

				Stuff::Vector3D stepDest;
				long cell[2];
				cell[0] = ULr + curRow;
				cell[1] = ULc + curCol;
				stepDest.x = (float)(cell[1]) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
				stepDest.y = (Terrain::worldUnitsMapSide / 2) - ((float)(cell[0]) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
				stepDest.z = (float)0; // How do we get the elevation for this point? Do we care?
				if (curCell == (numCells - 1) && (parent <= 7))
					path->setDistanceToGoal(curCell, 0.0);
				else
					path->setDistanceToGoal(curCell, cellShiftDistance[path->getDirection(curCell + 1)] + path->getDistanceToGoal(curCell + 1));
				path->setDestination(curCell, stepDest);
				path->setCell(curCell, cell[0], cell[1]);

				map[curRow * maxWidth + curCol].setFlag(MOVEFLAG_STEP);
				long cellOffsetIndex = map[curRow * maxWidth + curCol].parent << 1;
				curRow += cellShift[cellOffsetIndex++];
				curCol += cellShift[cellOffsetIndex];
			}
			
			#ifdef BLOCKED_PATH_TEST
				for (long i = 0; i < path->numSteps; i++) {
					long tile[2], cell[2];
					GameMap->worldToMapPos(path->stepList[i].destination, tile[0], tile[1], cell[0], cell[1]);
					bool cellPassable = GameMap->cellPassable(tile[0], tile[0], cell[0], cell[1]);
					if (!cellPassable)
						cellPassable = GameMap->cellPassable(tile[0], tile[0], cell[0], cell[1]);
					Assert(cellPassable || (i < 3), 0, " Bad Path Point ");
				}
			#endif
		}

		#ifdef TIME_PATH
			QueryPerformanceCounter(calcStop);
			calcTime = float(calcStop.LowPart - calcStart.LowPart) / float(countsPerSecond.LowPart);
			if (debugger && (scenarioTime > 0.0)) {
				char s[50];
				sprintf(s, "path calc: %.4f\n", calcTime);
				OutputDebugString(s);
				debugger->print(s);
			}
		#endif

		return(path->numSteps);
	}

	#ifdef TIME_PATH
		QueryPerformanceCounter(calcStop);
		calcTime = float(calcStop.LowPart - calcStart.LowPart) / float(countsPerSecond.LowPart);
		if (debugger && (scenarioTime > 0.0)) {
			char s[50];
			sprintf(s, "path calc: %.4f", calcTime);
			debugger->print(s);
		}
	#endif
	
	return(0);
}

void MoveMap::writeDebug (File* debugFile) {

	char outString[512];
	sprintf(outString, "Time = %.6f\n\n", calcTime);
	debugFile->writeString(outString);
	sprintf(outString, "Start = (%d, %d)\n", startR, startC);
	debugFile->writeString(outString);
	sprintf(outString, "Goal = (%d, %d)\n", goalR, goalC);
	debugFile->writeString(outString);
	strcpy(outString, "\n");
	debugFile->writeString(outString);
	
	debugFile->writeString("PARENT:\n");
	debugFile->writeString("-------\n");
	for (long r = 0; r < height; r++) {
		outString[0] = '\0';
		char numStr[10];
		for (long c = 0; c < width; c++) {
			if ((startR == r) && (startC == c))
				sprintf(numStr, "S");
			else if (map[r * maxWidth + c].parent == -1)
				sprintf(numStr, ".");
			else if (map[r * maxWidth + c].flags & MOVEFLAG_STEP)
				sprintf(numStr, "X");
			else			
				sprintf(numStr, "%d", map[r * maxWidth + c].parent);
			strcat(outString, numStr);
		}
		strcat(outString, "\n");
		debugFile->writeString(outString);
	}
	debugFile->writeString("\n");

	debugFile->writeString("MAP:\n");
	debugFile->writeString("-------\n");
	for (int r = 0; r < height; r++) {
		outString[0] = '\0';
		char numStr[10];
		for (long c = 0; c < width; c++) {
			if ((goalR == r) && (goalC == c))
				sprintf(numStr, "G");
			else if ((startR == r) && (startC == c))
				sprintf(numStr, "S");
			else if (map[r * maxWidth + c].cost == clearCost)
				sprintf(numStr, ".");
			else if (map[r * maxWidth + c].cost >= COST_BLOCKED)
				sprintf(numStr, " ");
			else if (map[r * maxWidth + c].cost < 256)
				sprintf(numStr, "%x", map[r * maxWidth + c].cost);
			strcat(outString, numStr);
		}
		strcat(outString, "\n");
		debugFile->writeString(outString);
	}
	debugFile->writeString("\n");

	debugFile->writeString("PATH:\n");
	debugFile->writeString("-------\n");
	for (int r = 0; r < height; r++) {
		outString[0] = '\0';
		char numStr[10];
		for (long c = 0; c < width; c++) {
			if ((goalR == r) && (goalC == c))
				sprintf(numStr, "G");
			else if ((startR == r) && (startC == c))
				sprintf(numStr, "S");
			else if (map[r * maxWidth + c].flags & MOVEFLAG_STEP)
				sprintf(numStr, "*");
			else if (map[r * maxWidth + c].cost == clearCost)
				sprintf(numStr, ".");
			else if (map[r * maxWidth + c].cost >= COST_BLOCKED)
				sprintf(numStr, " ");
			else			
				sprintf(numStr, "%d", map[r * maxWidth + c].cost);
			strcat(outString, numStr);
		}
		strcat(outString, "\n");
		debugFile->writeString(outString);
	}
	debugFile->writeString("\n");

}

void MoveMap::destroy (void) {

	if (map) 
	{
		systemHeap->Free(map);
		map = NULL;
	}

	if (mapRowStartTable)
	{
		systemHeap->Free(mapRowStartTable);
		mapRowStartTable = NULL;
	}

	if (mapRowTable)
	{
		systemHeap->Free(mapRowTable);
		mapRowTable = NULL;
	}

	if (mapColTable)
	{
		systemHeap->Free(mapColTable);
		mapColTable = NULL;
	}

}
