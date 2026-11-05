// Pathfinding: MoveMap::calcPath and calcPathJUMP.
// Declared in the class headers; shared names in move_internal.h.
#include <algorithm>

#include "move_internal.h"
#include "mc2_debug_out.h"


// WHAT: re-keys the OPEN node of a map cell. WHY: the cell is flagged OPEN, so the list should hold it; if it does not (a corrupt flag), rekey() changes
// nothing and the cell keeps its place. The first miss of a map logs one line, later ones are silent; release builds compile the assert out.
void MoveMap::reKeyOpen (long cellId, long newKey) {

	if (openHeap.rekey(static_cast<uint32_t>(cellId), static_cast<int32_t>(newKey)))
		return;
	gosASSERT(false);
	if (openHeap.rekeyMisses() == 1)
		mc2::fmt::print_err("[PATH v1] event=open_list_miss solver=movemap id={:d}\n", mc2::fmt::as_i32(cellId));
}

void MoveMap::updateOpenNode(MoveMapNode& node, long parent, long g, long cellId) {
	node.parent = parent;
	node.g = g;
	node.fPrime = g + node.hPrime;
	reKeyOpen(cellId, node.fPrime);
}

unsigned long long mc2_path_trace::hashMovePath (const MovePath* path) {
	unsigned long long h = 1469598103934665603ull;
	auto mix = [&h](const void* p, size_t n) {
		const unsigned char* b = static_cast<const unsigned char*>(p);
		for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
	};
	mix(&path->numSteps, sizeof(path->numSteps));
	mix(&path->cost, sizeof(path->cost));
	mix(&path->goal, sizeof(path->goal));
	mix(&path->target, sizeof(path->target));
	for (long i = 0; i < path->numSteps; i++) {
		const PathStep& s = path->stepList[i];
		mix(s.cell, sizeof(s.cell));
		mix(&s.distanceToGoal, sizeof(s.distanceToGoal));
		mix(&s.destination, sizeof(s.destination));
		mix(&s.direction, sizeof(s.direction));
		mix(&s.area, sizeof(s.area));
	}
	return h;
}

long MoveMap::calcPath (MovePathPtr path, glm::vec3* goalWorldPos, int* goalCell) {

	// PATHFINDING-SOLVER-ISOLATION-1: per-solve context (default-OFF).
	mc2_move::SolveContext ctx;

	// PATHFINDING-FACTS-1: gated per-solve telemetry (default-OFF, byte-identical).
	mc2_path_trace::SolveScope _pt("plain", openHeap);
	_pt.out = path;
	const bool _pathTrace = _pt.active;
	long _ptNodes = 0;
	long _ptOpenPeak = 0;
	if (!inputRevisionsValid()) {
		path->numSteps = path->numStepsWhenNotPaused = 0;
		return 0;
	}

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
			_shRHi[p] = (chMaxR + p) * cs + cs - 1; _shRHi[p] = std::min(_shRHi[p], height - 1);
			lo = (chMinC - p) * cs;            _shCLo[p] = (lo < 0 ? 0 : lo);
			_shCHi[p] = (chMaxC + p) * cs + cs - 1; _shCHi[p] = std::min(_shCHi[p], width - 1);
		}
	}

	#ifdef TIME_PATH
		L_INTEGER calcStart, calcStop;
		QueryPerformanceCounter(calcStart);
	#endif

	if ((goalR < 0) || (goalR >= height) || (goalC < 0) || (goalC >= width)) {
		glm::vec3 p;
		p.x = static_cast<float>((goalC)) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
		p.y = (Terrain::worldUnitsMapSide / 2) - (static_cast<float>((goalR)) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
		p.z = static_cast<float>(0); // How do we get the elevation for this point? Do we care?
		char msg[200];
		mc2::fmt::to_buf(msg, " Bad Move Goal: {:d} [{:d}({:d}), {:d}({:d})], ({:.2f}, {:.2f}, {:.2f})", mc2::fmt::as_i32(DebugMovePathType), mc2::fmt::as_i32(goalR), mc2::fmt::as_i32(height), mc2::fmt::as_i32(goalC), mc2::fmt::as_i32(width), p.x, p.y, p.z);
	}

	// Let's use their hPrime as a barrier for cutting off the search...
	maxHPrime = calcHPrime(startR, startC) * 2.5;
	maxHPrime = std::max<long>(maxHPrime, 500);
	
	// Storage was sized at init; each solve starts with an empty OPEN list below.
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
	openHeap.clear();
	openHeap.insert(initialVertex);
	curMapNode->setFlag(MOVEFLAG_OPEN);

	bool goalFound = false;
	int bestRow = -1;
	int bestCol = -1;

	while (!openHeap.isEmpty()) {


		if (_pathTrace) {
			long openN = openHeap.size();
			_ptOpenPeak = std::max(_ptOpenPeak, openN);
		}

		PQNode bestPQNode;
		openHeap.popMin(bestPQNode);
		_aln.n++;
		if (_pathTrace) _ptNodes++;
		bestRow = bestPQNode.row;
		bestCol = bestPQNode.col;
		if (g_moveReconEnabled) {
			if (_shPFirst) { _shPMinR = _shPMaxR = bestRow; _shPMinC = _shPMaxC = bestCol; _shPFirst = false; }
			else {
				_shPMinR = std::min(_shPMinR, bestRow);
				_shPMaxR = std::max(_shPMaxR, bestRow);
				_shPMinC = std::min(_shPMinC, bestCol);
				_shPMaxC = std::max(_shPMaxC, bestCol);
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

					if (succMapNode->hPrime < maxHPrime) {

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
								reKeyOpen(succCellIndex, succMapNode->fPrime);
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
							openHeap.insert(succPQNode);
							succMapNode->setFlag(MOVEFLAG_OPEN);
						}
					}
				}
			}
		}
	}

	
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
				_shFMinR = std::min(_shFMinR, curRow);
				_shFMaxR = std::max(_shFMaxR, curRow);
				_shFMinC = std::min(_shFMinC, curCol);
				_shFMaxC = std::max(_shFMaxC, curCol);
				unsigned long long cellKey = (static_cast<unsigned long long>(static_cast<unsigned>(curRow)) << 16) ^ static_cast<unsigned>(curCol);
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
			int maxSteps = 
			#endif

			path->init(numCells);

			#ifdef _DEBUG
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
			path->cost = map[mapRowStartTable[bestRow] + bestCol].g;
			curRow = bestRow;
			curCol = bestCol;
			int curCell = numCells;
			if (doorDirection == -1) {
				if (goalWorldPos) {
					goalWorldPos->x = static_cast<float>((ULc + bestCol)) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
					goalWorldPos->y = (Terrain::worldUnitsMapSide / 2) - (static_cast<float>((ULr + bestRow)) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
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
				long parent = reverseShift[map[mapRowStartTable[curRow] + curCol].parent];
				if (parent > 7)
					path->setDirection(curCell, parent);
				else
					path->setDirection(curCell, parent);

				glm::vec3 stepDest;
				int cell_r, cell_c;
				cell_r = ULr + curRow;
				cell_c = ULc + curCol;
				stepDest.x = static_cast<float>((cell_c)) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
				stepDest.y = (Terrain::worldUnitsMapSide / 2) - (static_cast<float>((cell_r)) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
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

				nodeAt(curRow, curCol).setFlag(MOVEFLAG_STEP);
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
				mc2::fmt::to_buf(s, "path calc: {:.4f}\n", calcTime);
				mc2::fmt::debug_out(s);
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
			mc2::fmt::to_buf(s, "path calc: {:.4f}", calcTime);
			debugger->print(s);
		}
	#endif
	
	return(0);
}

long MoveMap::calcPathJUMP (MovePathPtr path, glm::vec3* goalWorldPos, int* goalCell) {

	// PATHFINDING-SOLVER-ISOLATION-1: per-solve context (default-OFF).
	mc2_move::SolveContext ctx;

	// PATHFINDING-FACTS-1: gated per-solve telemetry (default-OFF, byte-identical).
	mc2_path_trace::SolveScope _pt("jump", openHeap);
	_pt.out = path;
	const bool _pathTrace = _pt.active;
	long _ptNodes = 0;
	long _ptOpenPeak = 0;
	if (!inputRevisionsValid()) {
		path->numSteps = path->numStepsWhenNotPaused = 0;
		return 0;
	}

	#ifdef TIME_PATH
		L_INTEGER calcStart, calcStop;
		QueryPerformanceCounter(calcStart);
	#endif



	if ((goalR < 0) || (goalR >= height) || (goalC < 0) || (goalC >= width)) {
		glm::vec3 p;
		p.x = static_cast<float>((goalC)) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
		p.y = (Terrain::worldUnitsMapSide / 2) - (static_cast<float>((goalR)) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
		p.z = static_cast<float>(0); // How do we get the elevation for this point? Do we care?
		char msg[200];
		mc2::fmt::to_buf(msg, " Bad Move Goal: {:d} [{:d}({:d}), {:d}({:d})], ({:.2f}, {:.2f}, {:.2f})", mc2::fmt::as_i32(DebugMovePathType), mc2::fmt::as_i32(goalR), mc2::fmt::as_i32(height), mc2::fmt::as_i32(goalC), mc2::fmt::as_i32(width), p.x, p.y, p.z);
		gosASSERT((goalR >= 0) && (goalR < height) && (goalC >= 0) && (goalC < width));
	}

	maxHPrime = calcHPrime(startR, startC) * 2.5;
	maxHPrime = std::max<long>(maxHPrime, 500);
	
	// Storage was sized at init; each solve starts with an empty OPEN list below.
	solveJumpOnBlocked = ctx.jumpOnBlocked();

    int curCol = startC;
	int curRow = startR;

	MoveMapNodePtr curMapNode = &nodeAt(curRow, curCol);
	curMapNode->g = 0;
	if (!ZeroHPrime)
		curMapNode->hPrime = calcHPrime(curRow, curCol);
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
	int bestRow = -1;
	int bestCol = -1;

    const int height_loc = height;
    const int width_loc = width;
    const int max_width_loc = maxWidth;

    const int num_offsets_loc = numOffsets;
    const int jump_cost_loc = jumpCost;

	while (!openHeap.isEmpty()) {


		if (_pathTrace) {
			long openN = openHeap.size();
			_ptOpenPeak = std::max(_ptOpenPeak, openN);
		}

		PQNode bestPQNode;
		openHeap.popMin(bestPQNode);
		if (_pathTrace) _ptNodes++;
		bestRow = bestPQNode.row;
		bestCol = bestPQNode.col;
		MoveMapNodePtr bestMapNode = &nodeAt(bestRow, bestCol);
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

				MoveMapNodePtr succMapNode = &std::mdspan<MoveMapNode, std::dextents<std::size_t, 2>>(map.data(), static_cast<std::size_t>(maxHeight), static_cast<std::size_t>(max_width_loc))[succRow, succCol];
                int cost = succMapNode->cost;
				if (cost < COST_BLOCKED) {
					if (succMapNode->hPrime == HPRIME_NOT_CALCED)
						succMapNode->hPrime = calcHPrime(succRow, succCol);

					if (succMapNode->hPrime < maxHPrime) {

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
								updateOpenNode(*succMapNode, dirToParent, succNodeG, succRow * MAX_MAPWIDTH + succCol);
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
		int curRow = goalCell[0] = bestRow;
		int curCol = goalCell[1] = bestCol;
		int numCells = 0;
		while ((curRow != startR) || (curCol != startC)) {
			numCells += 1;
			int cellOffsetIndex = nodeAt(curRow, curCol).parent * 2;
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
			int maxSteps =
#endif

			path->init(numCells);

#ifdef _DEBUG
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
			path->cost = nodeAt(bestRow, bestCol).g;
			curRow = bestRow;
			curCol = bestCol;
			int curCell = numCells;
			if (doorDirection == -1) {
				if (goalWorldPos) {
					goalWorldPos->x = static_cast<float>((ULc + bestCol)) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
					goalWorldPos->y = (Terrain::worldUnitsMapSide / 2) - (static_cast<float>((ULr + bestRow)) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
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
				glm::vec3 stepDest;
				stepDest.x = static_cast<float>((ULc + doorC)) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
				stepDest.y = (Terrain::worldUnitsMapSide / 2) - (static_cast<float>((ULr + doorR)) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
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
			// cellShiftDistance[] is built once at MOVE_init.

			while ((curRow != startR) || (curCol != startC)) {
				curCell--;
				int parent = reverseShift[nodeAt(curRow, curCol).parent];
				if (parent > 7)
					path->setDirection(curCell, parent);
				else
					path->setDirection(curCell, parent);

				glm::vec3 stepDest;
				int cell_r, cell_c;
				cell_r = ULr + curRow;
				cell_c = ULc + curCol;
				stepDest.x = static_cast<float>((cell_c)) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
				stepDest.y = (Terrain::worldUnitsMapSide / 2) - (static_cast<float>((cell_r)) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
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

				nodeAt(curRow, curCol).setFlag(MOVEFLAG_STEP);
				int cellOffsetIndex = nodeAt(curRow, curCol).parent << 1;
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
