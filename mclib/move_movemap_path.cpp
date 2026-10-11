// Pathfinding: MoveMap::calcPath and calcPathJUMP.
// Declared in the class headers; shared names in move_internal.h.
#include "move_internal.h"



long MoveMap::calcPath (MovePathPtr path, Stuff::Vector3D* goalWorldPos, int* goalCell) {

	// PATHFINDING-SOLVER-ISOLATION-1: per-solve context (default-OFF).
	mc2_move::SolveContext ctx;

	// PATHFINDING-FACTS-1: gated per-solve telemetry (default-OFF, byte-identical).
	mc2_path_trace::SolveScope _pt("plain");
	const bool _pathTrace = _pt.active;
	long _ptNodes = 0;
	long _ptOpenPeak = 0;

	MoveReconScope _recon_al(&g_moveRecon_astar_local_ns, &g_moveRecon_frame_astar_local_ns);
	MoveReconNodeCounter _aln;  // per-call local node-expansion size (Warrior.Path 2ms layer)
	// MOVE_CHUNK_SHADOW: popped-cell bbox (over-exploration vs final path) +
	// RECT_CORRIDOR_SHADOW: count popped nodes inside the start<->goal chunk
	// rectangle dilated by 0/1/2 chunks (upper bound on a constrained search).
	int _shPMinR = 0, _shPMaxR = 0, _shPMinC = 0, _shPMaxC = 0;
	bool _shPFirst = true;
	unsigned long long _shInRect0 = 0, _shInRect1 = 0, _shInRect2 = 0;
	int _shRLo[3] = {0,0,0}, _shRHi[3] = {0,0,0}, _shCLo[3] = {0,0,0}, _shCHi[3] = {0,0,0};
	if (g_moveReconEnabled) {
		g_moveRecon_astar_local_calls++;
		const int cs = 16;
		int chMinR = (startR < goalR ? startR : goalR) / cs;
		int chMaxR = (startR > goalR ? startR : goalR) / cs;
		int chMinC = (startC < goalC ? startC : goalC) / cs;
		int chMaxC = (startC > goalC ? startC : goalC) / cs;
		for (int p = 0; p < 3; p++) {
			int lo;
			lo = (chMinR - p) * cs;            _shRLo[p] = (lo < 0 ? 0 : lo);
			_shRHi[p] = (chMaxR + p) * cs + cs - 1; if (_shRHi[p] > height - 1) _shRHi[p] = height - 1;
			lo = (chMinC - p) * cs;            _shCLo[p] = (lo < 0 ? 0 : lo);
			_shCHi[p] = (chMaxC + p) * cs + cs - 1; if (_shCHi[p] > width - 1) _shCHi[p] = width - 1;
		}
	}

	#ifdef TIME_PATH
		L_INTEGER calcStart, calcStop;
		QueryPerformanceCounter(calcStart);
	#endif

	if ((goalR < 0) || (goalR >= height) || (goalC < 0) || (goalC >= width)) {
		Stuff::Vector3D p;
		p.x = (float)(goalC) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
		p.y = (Terrain::worldUnitsMapSide / 2) - ((float)(goalR) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
		p.z = (float)0; // How do we get the elevation for this point? Do we care?
		char msg[200];
		sprintf(msg, " Bad Move Goal: %d [%d(%d), %d(%d)], (%.2f, %.2f, %.2f)", DebugMovePathType, goalR, height, goalC, width, p.x, p.y, p.z);
	}

	// Let's use their hPrime as a barrier for cutting off the search...
	MaxHPrime = calcHPrime(startR, startC) * 2.5;
	if (MaxHPrime < 500)
		MaxHPrime = 500;
	
	// If we haven't already, create the OPEN list...
	// PATHFINDING-SOLVER-ISOLATION-1: OFF lazily owns the shared global exactly
	// as before; ON uses the context's private per-solve queue. solveOpen/
	// solveJumpOnBlocked mirror the globals OFF (byte-identical) and the ctx ON.
	if (!ctx.isolated && !openList) {
		openList = new PriorityQueue;
		gosASSERT(openList != nullptr);
		openList->init(5000);
	}
	solveOpen = ctx.isolated ? ctx.pq() : openList;
	solveJumpOnBlocked = ctx.jumpOnBlocked();

	int curCol = startC;
	int curRow = startR;

	MoveMapNodePtr curMapNode = &map[mapRowStartTable[curRow] + curCol];
	curMapNode->g = 0;
	if (!ZeroHPrime)
		curMapNode->hPrime = calcHPrime(curRow, curCol);
	curMapNode->fPrime = curMapNode->hPrime;

	PQNode initialVertex;
	initialVertex.key = curMapNode->fPrime;
	initialVertex.id = mapRowStartTable[curRow] + curCol;
	initialVertex.row = curRow;
	initialVertex.col = curCol;
	solveOpen->clear();
#ifdef _DEBUG
	int insertErr = 
#endif
		solveOpen->insert(initialVertex);
	gosASSERT(insertErr == NO_ERR);
	curMapNode->setFlag(MOVEFLAG_OPEN);

	bool goalFound = false;
	int bestRow = -1;
	int bestCol = -1;

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
				pathDebugFile = nullptr;
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
		_aln.n++;
		if (_pathTrace) _ptNodes++;
		bestRow = bestPQNode.row;
		bestCol = bestPQNode.col;
		if (g_moveReconEnabled) {
			if (_shPFirst) { _shPMinR = _shPMaxR = bestRow; _shPMinC = _shPMaxC = bestCol; _shPFirst = false; }
			else {
				if (bestRow < _shPMinR) _shPMinR = bestRow; else if (bestRow > _shPMaxR) _shPMaxR = bestRow;
				if (bestCol < _shPMinC) _shPMinC = bestCol; else if (bestCol > _shPMaxC) _shPMaxC = bestCol;
			}
			if (bestRow >= _shRLo[0] && bestRow <= _shRHi[0] && bestCol >= _shCLo[0] && bestCol <= _shCHi[0]) _shInRect0++;
			if (bestRow >= _shRLo[1] && bestRow <= _shRHi[1] && bestCol >= _shCLo[1] && bestCol <= _shCHi[1]) _shInRect1++;
			if (bestRow >= _shRLo[2] && bestRow <= _shRHi[2] && bestCol >= _shCLo[2] && bestCol <= _shCHi[2]) _shInRect2++;
		}
		MoveMapNodePtr bestMapNode = &map[bestPQNode.id];
		bestMapNode->clearFlag(MOVEFLAG_OPEN);

		int bestNodeG = bestMapNode->g;

		bestMapNode->setFlag(MOVEFLAG_CLOSED);

		if (bestMapNode->flags & MOVEFLAG_GOAL) {
			goalFound = true;
			_aln.found = true;
			break;
		}
		
		for (int dir = 0; dir < 8; dir++) {
			// First, make sure this is a legit direction to go. We do NOT
			// want to clip corners, so we'll check that here...
			bool isDiagonalWalk = IsDiagonalStep[dir];
			if (isDiagonalWalk) {
			
				// MINE CHECK should go in these adj tests...
				bool adj1Open = false;
				int adjCellIndex = map[bestPQNode.id].adjCells[StepAdjDir[dir]];
				if (adjCellIndex > -1)
					if ((map[adjCellIndex].flags & MOVEFLAG_MOVER_HERE) == 0)
						adj1Open = (map[adjCellIndex].cost < COST_BLOCKED);

				bool adj2Open = false;
				adjCellIndex = map[bestPQNode.id].adjCells[StepAdjDir[dir + 1]];
				if (adjCellIndex > -1)
					if ((map[adjCellIndex].flags & MOVEFLAG_MOVER_HERE) == 0)
						adj2Open = (map[adjCellIndex].cost < COST_BLOCKED);
					
				if (!adj1Open && !adj2Open)
					continue;

			}

			int succCellIndex = bestMapNode->adjCells[dir];

			// If we're doing offMapTravel, make sure we aren't going back into an offMap cell...
            
            // sebi: we do not want to address by negative offset
            if(succCellIndex > -1) {
    			if (map[succCellIndex].flags & MOVEFLAG_OFFMAP)
	    			if (cannotEnterOffMap)
		    			if ((map[bestPQNode.id].flags & MOVEFLAG_OFFMAP) == 0)
			    			continue;
            }
			
			if (succCellIndex > -1) {
				if (!inBounds(mapRowTable[succCellIndex], mapColTable[succCellIndex]))
					continue;
				MoveMapNodePtr succMapNode = &map[succCellIndex];
				if (succMapNode->cost < COST_BLOCKED) {
					if (succMapNode->hPrime == HPRIME_NOT_CALCED)
						succMapNode->hPrime = calcHPrime(mapRowTable[succCellIndex/*bestPQNode.id*/], mapColTable[succCellIndex/*bestPQNode.id*/]);

					if (succMapNode->hPrime < MaxHPrime) {

						#ifdef DEBUG_PATH
							numNodesVisited++;
						#endif
				
						char dirToParent = reverseShift[dir];

						int cost = succMapNode->cost;
						// Diagonal movement is more costly...
						gosASSERT(cost > 0);
						if (isDiagonalWalk)
							cost += (cost / 2);
						gosASSERT(cost > 0);
						
						int succNodeG = bestNodeG + cost;

						if (succMapNode->flags & MOVEFLAG_OPEN) {
							if (succNodeG < succMapNode->g) {
								succMapNode->parent = dirToParent;
								succMapNode->g = succNodeG;
								succMapNode->fPrime = succNodeG + succMapNode->hPrime;
								int openIndex = solveOpen->find(succCellIndex);
								if (!openIndex) {
									char s[128];
									sprintf(s, "MoveMap.calcPath: Cannot find movemap node [%d, %d] for change\n", succCellIndex, dir);
									gosASSERT(openIndex != 0);
									}
								else
									solveOpen->change(openIndex, succMapNode->fPrime);
							}
							}
						else if (succMapNode->flags & MOVEFLAG_CLOSED) {
							if (succNodeG < succMapNode->g) {
								succMapNode->parent = dirToParent;

								propogateCost(succCellIndex, cost, bestNodeG);
							}
							}
						else {
							succMapNode->parent = dirToParent;
							succMapNode->g = succNodeG;
							succMapNode->fPrime = succNodeG + succMapNode->hPrime;
							PQNode succPQNode;
							succPQNode.key = succMapNode->fPrime;
							succPQNode.id = succCellIndex;
							succPQNode.row = mapRowTable[succCellIndex];
							succPQNode.col = mapColTable[succCellIndex];
#ifdef _DEBUG
							int insertErr = 
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
			pathDebugFile = nullptr;
		}
	#endif
	
	if (goalFound) {
		int curRow = goalCell[0] = bestRow;
		int curCol = goalCell[1] = bestCol;
		int numCells = 0;
		// MOVE_CHUNK_SHADOW: final-path cell bbox (seed at goal end).
		int _shFMinR = bestRow, _shFMaxR = bestRow, _shFMinC = bestCol, _shFMaxC = bestCol;
		// MOVE_PATH_CACHE_SHADOW: FNV-1a hashes of the path cells and the
		// MOVER_HERE (dynamic occupancy) bit along the path.
		unsigned long long _shPathHash = 14695981039346656037ull;
		unsigned long long _shMoverHash = 14695981039346656037ull;
		while ((curRow != startR) || (curCol != startC)) {
			numCells += 1;
			int cellOffsetIndex = (map[mapRowStartTable[curRow] + curCol].parent << 1);
			curRow += cellShift[cellOffsetIndex++];
			curCol += cellShift[cellOffsetIndex];
			if (g_moveReconEnabled) {
				if (curRow < _shFMinR) _shFMinR = curRow; else if (curRow > _shFMaxR) _shFMaxR = curRow;
				if (curCol < _shFMinC) _shFMinC = curCol; else if (curCol > _shFMaxC) _shFMaxC = curCol;
				unsigned long long cellKey = ((unsigned long long)(unsigned)curRow << 16) ^ (unsigned)curCol;
				_shPathHash = (_shPathHash ^ cellKey) * 1099511628211ull;
				unsigned long long mh = (map[mapRowStartTable[curRow] + curCol].flags & MOVEFLAG_MOVER_HERE) ? 1ull : 0ull;
				_shMoverHash = (_shMoverHash ^ (cellKey * 2 + mh)) * 1099511628211ull;
			}
		}

		// MOVE_CHUNK_SHADOW: test whether this final path fits a coarse chunk
		// corridor (rectangular proxy). No-op when off / below node threshold.
		if (g_moveReconEnabled) {
			moveReconChunkSample(startR, startC, goalR, goalC, _aln.n, numCells,
				_shPMinR, _shPMaxR, _shPMinC, _shPMaxC,
				_shFMinR, _shFMaxR, _shFMinC, _shFMaxC,
				_shInRect0, _shInRect1, _shInRect2);
			moveReconPathCacheSample(startR, startC, goalR, goalC, numCells, _aln.n,
				_shPathHash, _shMoverHash, g_moveRecon_frames);
		}

		// If our goal is a door, the path will be one step longer, since
		// we need to walk "thru" the door...
		if (doorDirection != -1)
			numCells++;

		// PATHFINDING-FACTS-1: record path length + MAX_STEPS truncation (release-silent today).
		if (_pathTrace) {
			_pt.len = numCells;
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
			pathDebugFile = nullptr;
			gosASSERT(numCells <= MAX_STEPS_PER_MOVEPATH);
		}
		#endif

		path->init();
		if (numCells) {
			#ifdef _DEBUG			
			int maxSteps = 
			#endif

			path->init(numCells);

			#ifdef _DEBUG
			if (maxSteps > -1) {
				File* pathDebugFile = new File;
				pathDebugFile->create("longpath.dbg");
				char s[512];
				sprintf(s, "New Longest Path: %d steps (Max = %d)\n\n", numCells, MAX_STEPS_PER_MOVEPATH);
				pathDebugFile->writeString(s);
				writeDebug(pathDebugFile);
				pathDebugFile->close();
				delete pathDebugFile;
				pathDebugFile = nullptr;
			}
			#endif

			path->target = target;
			path->cost = map[mapRowStartTable[bestRow] + bestCol].g;
			curRow = bestRow;
			curCol = bestCol;
			int curCell = numCells;
			if (doorDirection == -1) {
				if (goalWorldPos) {
					goalWorldPos->x = (float)(ULc + bestCol) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
					goalWorldPos->y = (Terrain::worldUnitsMapSide / 2) - ((float)(ULr + bestRow) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
					goalWorldPos->z = 0.0f; // How do we get the elevation for this point? Do we care?
					path->goal = *goalWorldPos;
					}
				else
					path->goal = goal;
				}
			else {
				// It's a door, so it's the last on the path list...
				curCell--;
				path->setDirection(curCell, /*reverseShift[*/doorDirection * 2/*]*/);
				int doorR = bestRow + adjTile[doorDirection][0];
				int doorC = bestCol + adjTile[doorDirection][1];

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
					// We didn't know the exact goal world pos coming into this, since are
					// goal was an area door....
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
					for (int i = 0; i < NUM_CELL_OFFSETS; i++) {
						float distance = agsqrt( cellShift[i * 2], cellShift[i * 2 + 1] ) * cellLength;
						cellShiftDistance[i] = distance;
					}
					setTable = true;
				}
			}

			while ((curRow != startR) || (curCol != startC)) {
				curCell--;
				long parent = reverseShift[map[mapRowStartTable[curRow] + curCol].parent];
				if (parent > 7)
					path->setDirection(curCell, parent);
				else
					path->setDirection(curCell, parent);

				Stuff::Vector3D stepDest;
				int cell_r, cell_c;
				cell_r = ULr + curRow;
				cell_c = ULc + curCol;
				stepDest.x = (float)(cell_c) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
				stepDest.y = (Terrain::worldUnitsMapSide / 2) - ((float)(cell_r) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
				stepDest.z = 0.0f; // How do we get the elevation for this point? Do we care?
				if (curCell == (numCells - 1))
					path->setDistanceToGoal(curCell, 0.0);
				else {
					int tempDir = path->getDirection(curCell + 1);
					float tempDist = path->getDistanceToGoal(curCell + 1);
					path->setDistanceToGoal(curCell, cellShiftDistance[tempDir] + tempDist);
				}
				path->setDestination(curCell, stepDest);
				path->setCell(curCell, cell_r, cell_c);
				path->stepList[curCell].area = GlobalMoveMap[moveLevel]->calcArea(cell_r, cell_c);

				map[curRow * maxWidth + curCol].setFlag(MOVEFLAG_STEP);
				int cellOffsetIndex = map[mapRowStartTable[curRow] + curCol].parent << 1;
				curRow += cellShift[cellOffsetIndex++];
				curCol += cellShift[cellOffsetIndex];
			}

			if (thruAreas[1] != -1)
				if (thruAreas[1] != path->stepList[path->numSteps - 1].area) {
					for (int i = 0; i < path->numSteps; i++) {
						if (path->stepList[i].area == thruAreas[1]) {
							path->numStepsWhenNotPaused = path->numSteps = i + 1;
							goalCell[0] = path->stepList[i].cell[0];
							goalCell[1] = path->stepList[i].cell[1];
							land->cellToWorld(goalCell[0], goalCell[1], *goalWorldPos);
							break;
						}
					}
				}
			
			#ifdef BLOCKED_PATH_TEST
				for (int i = 0; i < path->numSteps; i++) {
					int tile[2], cell[2];
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

		// If we're starting on the goal cell, set the cost to 1 as an indicator that there is
		// a path--it's just that we were already there!
		if (numCells == 0)
			path->cost = 1;

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

long MoveMap::calcPathJUMP (MovePathPtr path, Stuff::Vector3D* goalWorldPos, int* goalCell) {

	// PATHFINDING-SOLVER-ISOLATION-1: per-solve context (default-OFF).
	mc2_move::SolveContext ctx;

	// PATHFINDING-FACTS-1: gated per-solve telemetry (default-OFF, byte-identical).
	mc2_path_trace::SolveScope _pt("jump");
	const bool _pathTrace = _pt.active;
	long _ptNodes = 0;
	long _ptOpenPeak = 0;

	#ifdef TIME_PATH
		L_INTEGER calcStart, calcStop;
		QueryPerformanceCounter(calcStart);
	#endif



	if ((goalR < 0) || (goalR >= height) || (goalC < 0) || (goalC >= width)) {
		Stuff::Vector3D p;
		p.x = (float)(goalC) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
		p.y = (Terrain::worldUnitsMapSide / 2) - ((float)(goalR) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
		p.z = (float)0; // How do we get the elevation for this point? Do we care?
		char msg[200];
		sprintf(msg, " Bad Move Goal: %d [%d(%d), %d(%d)], (%.2f, %.2f, %.2f)", DebugMovePathType, goalR, height, goalC, width, p.x, p.y, p.z);
		gosASSERT((goalR >= 0) && (goalR < height) && (goalC >= 0) && (goalC < width));
	}

	MaxHPrime = calcHPrime(startR, startC) * 2.5;
	if (MaxHPrime < 500)
		MaxHPrime = 500;
	
	if (!ctx.isolated && !openList) {
		openList = new PriorityQueue;
		gosASSERT(openList != nullptr);
		openList->init(5000);
	}
	solveOpen = ctx.isolated ? ctx.pq() : openList;
	solveJumpOnBlocked = ctx.jumpOnBlocked();

    int curCol = startC;
	int curRow = startR;

	MoveMapNodePtr curMapNode = &map[curRow * maxWidth + curCol];
	curMapNode->g = 0;
	if (!ZeroHPrime)
		curMapNode->hPrime = calcHPrime(curRow, curCol);
	curMapNode->fPrime = curMapNode->hPrime;

	PQNode initialVertex;
	initialVertex.key = curMapNode->fPrime;
	initialVertex.id = curRow * MAX_MAPWIDTH + curCol;
	initialVertex.row = curRow;
	initialVertex.col = curCol;
	solveOpen->clear();
#ifdef _DEBUG
	int insertErr = 
#endif
		solveOpen->insert(initialVertex);
	gosASSERT(insertErr == NO_ERR);
	curMapNode->setFlag(MOVEFLAG_OPEN);

	bool goalFound = false;
	int bestRow = -1;
	int bestCol = -1;

	#ifdef DEBUG_PATH
		topOpenNodes = 1;
		numNodesVisited = 1;
	#endif

    const int height_loc = height;
    const int width_loc = width;
    const int max_width_loc = maxWidth;

    const int num_offsets_loc = numOffsets;
    const int jump_cost_loc = jumpCost;

	while (!solveOpen->isEmpty()) {

		#ifdef DEBUG_MOVE_MAP
			if (debugMoveMap) {
				File* pathDebugFile = new File;
					pathDebugFile->create("mm.dbg");
				writeDebug(pathDebugFile);
				pathDebugFile->close();
				delete pathDebugFile;
				pathDebugFile = nullptr;
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

		int bestNodeG = bestMapNode->g;

		bestMapNode->setFlag(MOVEFLAG_CLOSED);
		
		if (bestMapNode->flags & MOVEFLAG_GOAL) {
			goalFound = true;
			break;
		}
		
		int cellOffsetIndex = 0;
		for (int dir = 0; dir < num_offsets_loc; dir++) {
			// First, make sure this is a legit direction to go. We do NOT
			// want to clip corners, so we'll check that here...
			//bool isDiagonalWalk = IsDiagonalStep[dir];
			bool isDiagonalWalk = (dir&1) && (dir<8); 
            if (isDiagonalWalk) {
                if (!adjacentCellOpenJUMP(bestRow, bestCol, StepAdjDir[dir]) && !adjacentCellOpenJUMP(bestRow, bestCol, StepAdjDir[dir + 1])) {
                    cellOffsetIndex += 2;
                    continue;
                }
            }

			int succRow = bestRow + cellShift[cellOffsetIndex++];
			int succCol = bestCol + cellShift[cellOffsetIndex++];
			if (succRow > 0 && succRow < height_loc && succCol > 0 && succCol < width_loc) {

				MoveMapNodePtr succMapNode = &map[succRow * max_width_loc + succCol];
                int cost = succMapNode->cost;
				if (cost < COST_BLOCKED) {
					if (succMapNode->hPrime == HPRIME_NOT_CALCED)
						succMapNode->hPrime = calcHPrime(succRow, succCol);

					if (succMapNode->hPrime < MaxHPrime) {

						#ifdef DEBUG_PATH
							numNodesVisited++;
						#endif
				
						char dirToParent = reverseShift[dir];

						bool jumping = false;
						gosASSERT(cost > 0);
						if (dir > 7) {
							jumping = true;
							if (solveJumpOnBlocked)   // PATHFINDING-SOLVER-ISOLATION-1
								cost = jump_cost_loc;
							else
								cost += jump_cost_loc;
							}
						else {
							if (isDiagonalWalk)
								cost += (cost / 2);
						}
						gosASSERT(cost > 0);
						
						int succNodeG = bestNodeG + cost;

						if (succMapNode->flags & MOVEFLAG_OPEN) {
							if (succNodeG < succMapNode->g) {
								succMapNode->parent = dirToParent;
								succMapNode->g = succNodeG;
								succMapNode->fPrime = succNodeG + succMapNode->hPrime;
								int openIndex = solveOpen->find(succRow * MAX_MAPWIDTH + succCol);
								if (!openIndex) {
									char s[128];
									sprintf(s, "MoveMap.calcPath: Cannot find movemap node [%d, %d, %d, %d] for change\n", succRow, succCol, succRow * MAX_MAPWIDTH + succCol, dir);
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
							int insertErr = 
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
			pathDebugFile = nullptr;
		}
	#endif

	if (goalFound) {
		int curRow = goalCell[0] = bestRow;
		int curCol = goalCell[1] = bestCol;
		int numCells = 0;
		while ((curRow != startR) || (curCol != startC)) {
			numCells += 1;
			int cellOffsetIndex = map[curRow * maxWidth + curCol].parent * 2;
			curRow += cellShift[cellOffsetIndex++];
			curCol += cellShift[cellOffsetIndex];
		}
		if (doorDirection != -1)
			numCells++;

		// PATHFINDING-FACTS-1: record path length + MAX_STEPS truncation (release-silent today).
		if (_pathTrace) {
			_pt.len = numCells;
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
			pathDebugFile = nullptr;
			gosASSERT(numCells <= MAX_STEPS_PER_MOVEPATH);
		}
#endif

		path->init();
		if (numCells) {
#ifdef _DEBUG
			int maxSteps =
#endif

			path->init(numCells);

#ifdef _DEBUG
			if (maxSteps > -1) {
				File* pathDebugFile = new File;
				pathDebugFile->create("longpath.dbg");
				char s[512];
				sprintf(s, "New Longest Path: %d steps (Max = %d)\n\n", numCells, MAX_STEPS_PER_MOVEPATH);
				pathDebugFile->writeString(s);
				writeDebug(pathDebugFile);
				pathDebugFile->close();
				delete pathDebugFile;
				pathDebugFile = nullptr;
			}
#endif
			path->target = target;
			path->cost = map[bestRow * maxWidth + bestCol].g;
			curRow = bestRow;
			curCol = bestCol;
			int curCell = numCells;
			if (doorDirection == -1) {
				if (goalWorldPos) {
					goalWorldPos->x = (float)(ULc + bestCol) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
					goalWorldPos->y = (Terrain::worldUnitsMapSide / 2) - ((float)(ULr + bestRow) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
					goalWorldPos->z = 0.0f; // How do we get the elevation for this point? Do we care?
					path->goal = *goalWorldPos;
					}
				else
					path->goal = goal;
				}
			else {
				// It's a door, so it's the last on the path list...
				curCell--;
				path->setDirection(curCell, /*reverseShift[*/doorDirection * 2/*]*/);
				int doorR = bestRow + adjTile[doorDirection][0];
				int doorC = bestCol + adjTile[doorDirection][1];

				goalCell[0] = ULr + doorR;
				goalCell[1] = ULc + doorC;
				Stuff::Vector3D stepDest;
				stepDest.x = (float)(ULc + doorC) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
				stepDest.y = (Terrain::worldUnitsMapSide / 2) - ((float)(ULr + doorR) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
				stepDest.z = 0.0f; // How do we get the elevation for this point? Do we care?
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
			// PATHFINDING-SOLVER-ISOLATION-1: ON skips the lazy static guard
			// (table built at MOVE_init). OFF unchanged.
			if (!ctx.isolated) {
				static bool setTable = false;
				if (!setTable) {
					float cellLength = Terrain::worldUnitsPerCell * metersPerWorldUnit;
					for (int i = 0; i < NUM_CELL_OFFSETS; i++) {
						float distance = agsqrt( cellShift[i * 2], cellShift[i * 2 + 1] ) * cellLength;
						cellShiftDistance[i] = distance;
					}
					setTable = true;
				}
			}

			while ((curRow != startR) || (curCol != startC)) {
				curCell--;
				int parent = reverseShift[map[curRow * maxWidth + curCol].parent];
				if (parent > 7)
					path->setDirection(curCell, parent);
				else
					path->setDirection(curCell, parent);

				Stuff::Vector3D stepDest;
				int cell_r, cell_c;
				cell_r = ULr + curRow;
				cell_c = ULc + curCol;
				stepDest.x = (float)(cell_c) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
				stepDest.y = (Terrain::worldUnitsMapSide / 2) - ((float)(cell_r) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
				stepDest.z = 0.0f; // How do we get the elevation for this point? Do we care?
				if (curCell == (numCells - 1))
					path->setDistanceToGoal(curCell, 0.0);
				else {
					int tempDir = path->getDirection(curCell + 1);
					float tempDist = path->getDistanceToGoal(curCell + 1);
					path->setDistanceToGoal(curCell, cellShiftDistance[tempDir] + tempDist);
				}
				path->setDestination(curCell, stepDest);
				path->setCell(curCell, cell_r, cell_c);

				map[curRow * maxWidth + curCol].setFlag(MOVEFLAG_STEP);
				int cellOffsetIndex = map[curRow * maxWidth + curCol].parent << 1;
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
