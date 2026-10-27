// Pathfinding: MoveMap::calcEscapePath, writeDebug and destroy.
// Declared in the class headers; shared names in move_internal.h.
#include "move_internal.h"
#include "mc2fmt.h"


long MoveMap::calcEscapePath (MovePathPtr path, glm::vec3* goalWorldPos, long* goalCell) {

	// PATHFINDING-SOLVER-ISOLATION-1: per-solve context (default-OFF).
	mc2_move::SolveContext ctx;

	// PATHFINDING-FACTS-1: gated per-solve telemetry (default-OFF, byte-identical).
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
		glm::vec3 p;
		p.x = (float)(goalC) * MetersPerCell + MetersPerCell / 2 - Terrain::worldUnitsMapSide / 2;
		p.y = (Terrain::worldUnitsMapSide / 2) - ((float)(goalR) * MetersPerCell) - MetersPerCell / 2;
		p.z = (float)0; // How do we get the elevation for this point? Do we care?
		char msg[200];
		sprintf(msg, " Bad Move Goal: %d [%d(%d), %d(%d)], (%.2f, %.2f, %.2f)", DebugMovePathType, goalR, cellHeight, goalC, cellWidth, p.x, p.y, p.z);
		Fatal(0, msg);
	}
*/

	// Let's use their hPrime as a barrier for cutting off the search...
	maxHPrime = 500; //float2short(calcHPrime(startR, startC) * 2.5);
	if (maxHPrime < 500)
		maxHPrime = 500;
	
	// The open list is this map's own (openHeap), sized to the window and empty at the start of every solve.
	openHeap.reserve(openHeapReserve());
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
	openHeap.clear();
	openHeap.insert(initialVertex);
	curMapNode->setFlag(MOVEFLAG_OPEN);

	bool goalFound = false;
	long bestRow = -1;
	long bestCol = -1;

	while (!openHeap.isEmpty()) {


		if (_pathTrace) {
			long openN = openHeap.size();
			if (openN > _ptOpenPeak) _ptOpenPeak = openN;
		}

		PQNode bestPQNode;
		openHeap.popMin(bestPQNode);
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

					if (succMapNode->hPrime < maxHPrime) {

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
								reKeyOpen(succRow * MAX_MAPWIDTH + succCol, succMapNode->fPrime);
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
							openHeap.insert(succPQNode);
							succMapNode->setFlag(MOVEFLAG_OPEN);
						}
					}
				}
			}
		}
	}


	if (goalFound) {
		long curRow = goalCell[0] = bestRow;
		long curCol = goalCell[1] = bestCol;
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

		// PATHFINDING-FACTS-1: record path length + MAX_STEPS truncation (release-silent today).
		if (_pathTrace) {
			_pt.len = static_cast<int>(numCells);
			_pt.fail = 0;
			_pt.nodes = _ptNodes;
			_pt.openPeak = _ptOpenPeak;
			if (numCells > MAX_STEPS_PER_MOVEPATH)
				_pt.trunc = 1;
		}

#ifdef _DEBUG
		if (numCells > MAX_STEPS_PER_MOVEPATH) {
			File pathDebugFile;
			pathDebugFile.create("longpath.dbg");
			char s[512];
			mc2::fmt::to_buf(s, "Path Too Long: {:d} steps (Max = {:d}) (please save longpath.dbg file:)", mc2::fmt::as_i32(numCells), mc2::fmt::as_i32(MAX_STEPS_PER_MOVEPATH));
			pathDebugFile.writeString(s);
			pathDebugFile.writeString("\n\n");
			writeDebug(&pathDebugFile);
			pathDebugFile.close();
			gosASSERT(numCells <= MAX_STEPS_PER_MOVEPATH);
		}
#endif

		path->init();
		if (numCells) {
			
#ifdef _DEBUG			
			long maxSteps = path->init(numCells);

			if (maxSteps > -1) {
				File pathDebugFile;
				pathDebugFile.create("longpath.dbg");
				char s[512];
				mc2::fmt::to_buf(s, "New Longest Path: {:d} steps (Max = {:d})\n\n", mc2::fmt::as_i32(numCells), mc2::fmt::as_i32(MAX_STEPS_PER_MOVEPATH));
				pathDebugFile.writeString(s);
				writeDebug(&pathDebugFile);
				pathDebugFile.close();
			}
#endif
			path->target = target;
			path->cost = map[bestRow * maxWidth + bestCol].g;
			curRow = bestRow;
			curCol = bestCol;
			long curCell = numCells;
			if (doorDirection == -1) {
				if (goalWorldPos) {
					goalWorldPos->x = static_cast<float>((ULc + bestCol)) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
					goalWorldPos->y = (Terrain::worldUnitsMapSide / 2) - (static_cast<float>((ULr + bestRow)) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
					goalWorldPos->z = static_cast<float>(0); // How do we get the elevation for this point? Do we care?
					path->goal = *goalWorldPos;
					}
				else
					path->goal = goal;
				}
			else {
				// It's a door, so it's the last on the path list...
				curCell--;
				path->setDirection(curCell, /*reverseShift[*/doorDirection * 2/*]*/);
				long doorR = bestRow + adjTile[doorDirection][0];
				long doorC = bestCol + adjTile[doorDirection][1];

				goalCell[0] = ULr + doorR;
				goalCell[1] = ULc + doorC;
				glm::vec3 stepDest;
				stepDest.x = static_cast<float>((ULc + doorC)) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
				stepDest.y = (Terrain::worldUnitsMapSide / 2) - (static_cast<float>((ULr + doorR)) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
				stepDest.z = static_cast<float>(0); // How do we get the elevation for this point? Do we care?
				path->setDestination(curCell, stepDest);
				path->setDistanceToGoal(curCell, 0.0);
				path->setCell(curCell, goalCell[0], goalCell[1]);
				path->goal = stepDest;
				if (goalWorldPos) {
					// We didn't know the exact goal world pos coming into this, since are
					// goal was an area door....
					*goalWorldPos = stepDest;
				}
			}
			
			// We need to set this table up our first time thru here...
			// cellShiftDistance[] is built once at MOVE_init.
			
			while ((curRow != startR) || (curCol != startC)) {
				curCell--;
				long parent = reverseShift[map[curRow * maxWidth + curCol].parent];
				if (parent > 7)
					path->setDirection(curCell, parent);
				else
					path->setDirection(curCell, parent);

				glm::vec3 stepDest;
				long cell[2];
				cell[0] = ULr + curRow;
				cell[1] = ULc + curCol;
				stepDest.x = static_cast<float>((cell[1])) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
				stepDest.y = (Terrain::worldUnitsMapSide / 2) - (static_cast<float>((cell[0])) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
				stepDest.z = static_cast<float>(0); // How do we get the elevation for this point? Do we care?
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
				mc2::fmt::to_buf(s, "path calc: {:.4f}\n", calcTime);
				mc2::fmt::debug_out(s);
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
			mc2::fmt::to_buf(s, "path calc: {:.4f}", calcTime);
			debugger->print(s);
		}
	#endif
	
	return(0);
}

void MoveMap::writeDebug (File* debugFile) {

	char outString[512];
	mc2::fmt::to_buf(outString, "Time = {:.6f}\n\n", calcTime);
	debugFile->writeString(outString);
	mc2::fmt::to_buf(outString, "Start = ({:d}, {:d})\n", mc2::fmt::as_i32(startR), mc2::fmt::as_i32(startC));
	debugFile->writeString(outString);
	mc2::fmt::to_buf(outString, "Goal = ({:d}, {:d})\n", mc2::fmt::as_i32(goalR), mc2::fmt::as_i32(goalC));
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
				mc2::fmt::to_buf(numStr, "S");
			else if (map[r * maxWidth + c].parent == -1)
				mc2::fmt::to_buf(numStr, ".");
			else if (map[r * maxWidth + c].flags & MOVEFLAG_STEP)
				mc2::fmt::to_buf(numStr, "X");
			else			
				mc2::fmt::to_buf(numStr, "{:d}", mc2::fmt::as_i32(map[r * maxWidth + c].parent));
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
				mc2::fmt::to_buf(numStr, "G");
			else if ((startR == r) && (startC == c))
				mc2::fmt::to_buf(numStr, "S");
			else if (map[r * maxWidth + c].cost == clearCost)
				mc2::fmt::to_buf(numStr, ".");
			else if (map[r * maxWidth + c].cost >= COST_BLOCKED)
				mc2::fmt::to_buf(numStr, " ");
			else if (map[r * maxWidth + c].cost < 256)
				mc2::fmt::to_buf(numStr, "{:x}", mc2::fmt::as_u32(map[r * maxWidth + c].cost));
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
				mc2::fmt::to_buf(numStr, "G");
			else if ((startR == r) && (startC == c))
				mc2::fmt::to_buf(numStr, "S");
			else if (map[r * maxWidth + c].flags & MOVEFLAG_STEP)
				mc2::fmt::to_buf(numStr, "*");
			else if (map[r * maxWidth + c].cost == clearCost)
				mc2::fmt::to_buf(numStr, ".");
			else if (map[r * maxWidth + c].cost >= COST_BLOCKED)
				mc2::fmt::to_buf(numStr, " ");
			else			
				mc2::fmt::to_buf(numStr, "{:d}", mc2::fmt::as_i32(map[r * maxWidth + c].cost));
			strcat(outString, numStr);
		}
		strcat(outString, "\n");
		debugFile->writeString(outString);
	}
	debugFile->writeString("\n");

}

void MoveMap::destroy (void) {

	// Frees the grid now; the next solve re-sizes it.
	map = {};
	mapRowStartTable = {};
	mapRowTable = {};
	mapColTable = {};

}
