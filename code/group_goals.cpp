// MoverGroup: the move / jump goal-cell searches (calcMoveGoals, calcJumpGoals, sortMovers) and their spiral tables.
// Declared in the class headers; shared names in group_internal.h.
#include "group_internal.h"


//---------------------------------------------------------------------------

inline bool inMapBounds (long r, long c, long mapHeight, long mapWidth) {

	return((r >= 0) && (r < mapHeight) && (c >= 0) && (c < mapWidth));
}

//---------------------------------------------------------------------------

#define	JUMPMAP_TILE_DIM	3
#define	JUMPMAP_CELL_DIM	MAPCELL_DIM * JUMPMAP_TILE_DIM

char CellSpiralIncrement[JUMPMAP_CELL_DIM * JUMPMAP_CELL_DIM * 2] = {
	-1, 0,
	0, 1,
	1, 0,
	1, 0,
	0, -1,
	0, -1,
	-1, 0,
	-1, 0,

	-1, 0,
	0, 1,
	0, 1,
	0, 1,
	1, 0,
	1, 0,
	1, 0,
	1, 0,
	0, -1,
	0, -1,
	0, -1,
	0, -1,
	-1, 0,
	-1, 0,
	-1, 0,
	-1, 0,

	-1, 0,
	0, 1,
	0, 1,
	0, 1,
	0, 1,
	0, 1,
	1, 0,
	1, 0,
	1, 0,
	1, 0,
	1, 0,
	1, 0,
	0, -1,
	0, -1,
	0, -1,
	0, -1,
	0, -1,
	0, -1,
	-1, 0,
	-1, 0,
	-1, 0,
	-1, 0,
	-1, 0,
	-1, 0,

	-1, 0,
	0, 1,
	0, 1,
	0, 1,
	0, 1,
	0, 1,
	0, 1,
	0, 1,
	1, 0,
	1, 0,
	1, 0,
	1, 0,
	1, 0,
	1, 0,
	1, 0,
	1, 0,
	0, -1,
	0, -1,
	0, -1,
	0, -1,
	0, -1,
	0, -1,
	0, -1,
	0, -1,
	-1, 0,
	-1, 0,
	-1, 0,
	-1, 0,
	-1, 0,
	-1, 0,
	-1, 0,
	-1, 0
};

//---------------------------------------------------------------------------

void MoverGroup::sortMovers (long numMoversInGroup, MoverPtr* moverList, Stuff::Vector3D destination) {

	Mover::sortList->clear();
	for (long i = 0; i < numMoversInGroup; i++) {
		long index = -1;
		float dist = (float)3.48E+37;
		if (moverList[i]) {
			index = i;
			Stuff::Vector3D resultVector;
			resultVector.Subtract(moverList[i]->getPosition(), destination);
			dist = resultVector.GetLength();
		}
		Mover::sortList->setId(i, index);
		Mover::sortList->setValue(i, dist);
	}
	Mover::sortList->sort(false);
	for (int i = 0; i < numMoversInGroup; i++) {
		long moverIndex = Mover::sortList->getId(i);
		if (moverIndex != -1)
			moverList[moverIndex]->selectionIndex = i;
	}
}

//---------------------------------------------------------------------------

#define	GOALMAP_DIM	61

long MoverGroup::calcMoveGoals (Stuff::Vector3D goal, long numMovers, Stuff::Vector3D* goalList) {

	if ( !numMovers ) // 07/24/HKG: crashes if no movers
		return 0;

	if (!goalMap) {
		goalMap = (GoalMapNode*)systemHeap->Malloc(sizeof(GoalMapNode) * GOALMAP_DIM * GOALMAP_DIM);
		if (!goalMap)
			Fatal(0, " MoverGroup.calcMoveGoals: unable to malloc goalMap ");
	}

	if (!GameMap)
		return(-1);

	int goalRow, goalCol;
	land->worldToCell(goal, goalRow, goalCol);
	int topLeftRow = goalRow - GOALMAP_DIM / 2;
	int topLeftCol = goalCol - GOALMAP_DIM / 2;
	for (int r = 0; r < GOALMAP_DIM; r++)
		for (int c = 0; c < GOALMAP_DIM; c++) {
			if (!inMapBounds(topLeftRow + r, topLeftCol + c, GameMap->height, GameMap->width))
				continue;
			int index = r * GOALMAP_DIM + c;
			goalMap[index].cost = GameMap->getPassable(topLeftRow + r, topLeftCol + c) ? 100 : COST_BLOCKED;
			goalMap[index].flags = GOALFLAG_AVAILABLE + GOALFLAG_NO_NEIGHBORS;
			goalMap[index].g = 0;
		}

	//-----------------------------------------------
	// If we haven't already, create the OPEN list...
	if (!openList) {
		openList = new PriorityQueue;
		gosASSERT(openList != NULL);
		openList->init(5000);
	}

	int curRow = GOALMAP_DIM / 2;
	int curCol = GOALMAP_DIM / 2;
	
	GoalMapNode* curMapNode = &goalMap[curRow * GOALMAP_DIM + curCol];

	//-----------------------------------------------------------------
	// Put the START (the goal, in this case) on the empty OPEN list...
	PQNode initialVertex;
	initialVertex.key = curMapNode->g;
	initialVertex.id = curRow * GOALMAP_DIM + curCol;
	initialVertex.row = curRow;
	initialVertex.col = curCol;
	openList->clear();
	openList->insert(initialVertex);
	curMapNode->setFlag(GOALFLAG_OPEN);

	long numGoalsFound = 0;
	while (!openList->isEmpty()) {

		//----------------------
		// Grab the best node...
		PQNode bestPQNode;
		openList->remove(bestPQNode);
		curRow = bestPQNode.row;
		curCol = bestPQNode.col;
		GoalMapNode* bestMapNode = &goalMap[curRow * GOALMAP_DIM + curCol];
		bestMapNode->clearFlag(GOALFLAG_OPEN);

		//----------------------------
		// Now, close the best node...
		bestMapNode->setFlag(GOALFLAG_CLOSED);

		bool markNeighbors = false;
		//------------------------------
		// Have we found a valid goal...
		if (bestMapNode->cost < COST_BLOCKED)
			if (bestMapNode->flags & GOALFLAG_NO_NEIGHBORS)
				if (bestMapNode->flags & GOALFLAG_AVAILABLE) {
					bestMapNode->clearFlag(GOALFLAG_AVAILABLE);
					land->cellToWorld(topLeftRow + curRow, topLeftCol + curCol, goalList[numGoalsFound++]);
					markNeighbors = true;
				}
		if (numGoalsFound == numMovers)
			break;

		if (markNeighbors) {
			static long cellShift[8][2] = {{-1, 0}, {-1, 1}, {0, 1}, {1, 1}, {1, 0}, {1, -1}, {0, -1}, {-1, -1}};
			for (long i = 0; i < 8; i++) {
				long nRow = curRow + cellShift[i][0];
				long nCol = curRow + cellShift[i][1];
				if (inMapBounds(nRow, nCol, GOALMAP_DIM, GOALMAP_DIM))
					goalMap[nRow * GOALMAP_DIM + nCol].clearFlag(GOALFLAG_NO_NEIGHBORS);
			}
		}

		long bestNodeG = bestMapNode->g;
		for (long dir = 0; dir < 4; dir++) {
			//------------------------------------------------------------
			// First, make sure this is a legit direction to go. We do NOT
			// move diagonally, when checking these cells...
			//------------------------------------------------------------
			// Now, process this direction. First, calc the cell to check,
			// offset from the current cell...
			static long cellShift[4][2] = {{-1, 0}, {0, 1}, {1, 0}, {0, -1}};
			long succRow = curRow + cellShift[dir][0];
			long succCol = curCol + cellShift[dir][1];
			//--------------------------------
			// If it's on the map, check it...
			if (inMapBounds(succRow, succCol, GOALMAP_DIM, GOALMAP_DIM)) {
				GoalMapNode* succMapNode = &goalMap[succRow * GOALMAP_DIM + succCol];
				if (succMapNode->cost < COST_BLOCKED) {
					long succNodeG = bestNodeG + succMapNode->cost;
					if ((succMapNode->flags & (GOALFLAG_OPEN + GOALFLAG_CLOSED)) == 0) {
						//-------------------------------------------------
						// This node is neither OPEN nor CLOSED, so toss it
						// into the OPEN list...
						//succMapNode->parent = dirToParent;
						succMapNode->g = succNodeG;
						PQNode succPQNode;
						succPQNode.key = succMapNode->g;
						succPQNode.id = succRow * GOALMAP_DIM + succCol;
						succPQNode.row = succRow;
						succPQNode.col = succCol;
						openList->insert(succPQNode);
						succMapNode->setFlag(MOVEFLAG_OPEN);
					}
				}
			}
		}
	}		

	return(numGoalsFound);
}

//---------------------------------------------------------------------------

#define DEBUGJUMPGOALS TRUE

long MoverGroup::calcJumpGoals (Stuff::Vector3D goal, long numMovers, Stuff::Vector3D* goalList, GameObjectPtr DFATarget) {

	long numJumping = 0;

	//-----------------------------
	// First, build the jump map...
	long jumpMap[JUMPMAP_CELL_DIM][JUMPMAP_CELL_DIM];

	//------------------------------------------------------------
	// The initial goal tile is placed at the center of the map...
	int goalCell[2] = {0, 0};
	land->worldToCell(goal, goalCell[0], goalCell[1]);

	int mapCellUL[2] = {0, 0};
	mapCellUL[0] = goalCell[0] - JUMPMAP_CELL_DIM / 2;
	mapCellUL[1] = goalCell[1] - JUMPMAP_CELL_DIM / 2;

	// -1 = OPEN
	// -2 = BLOCKED
	// 0 thru # = already selected for that # mover in the group
	for (long r = 0; r < JUMPMAP_CELL_DIM; r++)
		for (long c = 0; c < JUMPMAP_CELL_DIM; c++) {
			long cellRow = mapCellUL[0] + r;
			long cellCol = mapCellUL[1] + c;
			if (GameMap->inBounds(cellRow, cellCol)) {
				MapCellPtr mapCell = GameMap->getCell(cellRow, cellCol);

				//-----------------------
				// Tile (terrain) type...
				//long tileType = curTile.getTileType();
				if (!mapCell->getPassable())
					jumpMap[r][c] = -2;
				else
					jumpMap[r][c] = -1;

#ifdef USE_OVERLAYS_IN_MC2
				long overlay = mapCell->getOverlay();
				if (OverlayIsBridge[overlay]) {
					switch (overlay) {
						case OVERLAY_WATER_BRIDGE_NS:
						case OVERLAY_RAILROAD_WATER_BRIDGE_NS:
							jumpMap[row][col] = -2;
							jumpMap[row][col + 2] = -2;
							jumpMap[row + 1][col] = -2;
							jumpMap[row + 1][col + 2] = -2;
							jumpMap[row + 2][col] = -2;
							jumpMap[row + 2][col + 2] = -2;
							break;
						case OVERLAY_WATER_BRIDGE_EW:
						case OVERLAY_RAILROAD_WATER_BRIDGE_EW:
							jumpMap[row][col] = -2;
							jumpMap[row][col + 1] = -2;
							jumpMap[row][col + 2] = -2;
							jumpMap[row + 2][col] = -2;
							jumpMap[row + 2][col + 1] = -2;
							jumpMap[row + 2][col + 2] = -2;
							break;
						case OVERLAY_WATER_BRIDGE_NS_DESTROYED:
						case OVERLAY_RAILROAD_WATER_BRIDGE_NS_DESTROYED:
						case OVERLAY_WATER_BRIDGE_EW_DESTROYED:
						case OVERLAY_RAILROAD_WATER_BRIDGE_EW_DESTROYED:
							jumpMap[row][col] = -2;
							jumpMap[row][col + 1] = -2;
							jumpMap[row][col + 2] = -2;
							jumpMap[row + 1][col] = -2;
							jumpMap[row + 1][col + 1] = -2;
							jumpMap[row + 1][col + 2] = -2;
							jumpMap[row + 2][col] = -2;
							jumpMap[row + 2][col + 1] = -2;
							jumpMap[row + 2][col + 2] = -2;
							break;
					}
				}
#endif
				}
			else
				jumpMap[r][c] = -2;
		}

	int moverCount = ObjectManager->getNumMovers();
	for (int i = 0; i < moverCount; i++) {
		MoverPtr mover = ObjectManager->getMover(i);
		if ((mover->getObjectClass() != ELEMENTAL) && (mover != DFATarget) && !mover->isDisabled()) {
			int mapCellRow, mapCellCol;
			mover->getCellPosition(mapCellRow, mapCellCol);
			mapCellRow -= mapCellUL[0];
			mapCellCol -= mapCellUL[1];
			if (inMapBounds(mapCellRow, mapCellCol, JUMPMAP_CELL_DIM, JUMPMAP_CELL_DIM))
				jumpMap[mapCellRow][mapCellCol] = -2;
		}
	}

#ifdef _DEBUG
#if DEBUGJUMPGOALS
	char debugStr[256];
	sprintf(debugStr, "GROUP JUMP(%.2f,%.2f,%.2f)--UL = %d,%d: ", goal.x, goal.y, goal.z,mapCellUL[0], mapCellUL[1]);
#endif
#endif

	//-----------------------------------------------------------------
	// Now, for each jumper, select a closest cell to the goal, mark it
	// as theirs and close it...
	for (int i = 0; i < numMovers; i++) {
		//int startCellRow = 0;
		//int startCellCol = 0;

		int curCellRow = goalCell[0] - mapCellUL[0];
		int curCellCol = goalCell[1] - mapCellUL[1];
		bool notFound = true;
		int spiralIndex = 0;
		while (notFound) {
			if (jumpMap[curCellRow][curCellCol] == -1) {
				// Should check to see if the cell is within range...
				//----------------------------------
				// Found an open cell, so take it...
				jumpMap[curCellRow][curCellCol] = i;
				land->cellToWorld(mapCellUL[0] + curCellRow, mapCellUL[1] + curCellCol, goalList[i]);
				numJumping++;
				notFound = false;
#ifdef _DEBUG
#if DEBUGJUMPGOALS
				char s[30];
				sprintf(s, "[%d,%d] ", curCellRow, curCellCol);
				strcat(debugStr, s);
#endif
#endif
				}
			else {
				//-------------------------------------
				// Go to the next cell in our search...
				do {
					if (spiralIndex == (JUMPMAP_CELL_DIM * JUMPMAP_CELL_DIM * 2)) {
						goalList[i].x = -99999.0;
						goalList[i].y = -99999.0;
						goalList[i].z = -99999.0;
						notFound = false;
						break;
					}
					curCellRow += CellSpiralIncrement[spiralIndex++];
					curCellCol += CellSpiralIncrement[spiralIndex++];
				} while (!inMapBounds(curCellRow, curCellCol, JUMPMAP_CELL_DIM, JUMPMAP_CELL_DIM));
			}
		}
	}

	return(numJumping);
}

//---------------------------------------------------------------------------

long MoverGroup::calcJumpGoals (Stuff::Vector3D goal, Stuff::Vector3D* goalList, GameObjectPtr DFATarget) {

	return(calcJumpGoals(goal, numMovers, goalList, DFATarget));
}
