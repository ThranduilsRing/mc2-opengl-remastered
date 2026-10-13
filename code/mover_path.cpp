// Mover: move goals, line of sight, global/local path calculation and path locking.
// Declared in mover.h; shared names in mover_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "mover_internal.h"


Stuff::Vector3D Mover::calcOffsetMoveGoal (Stuff::Vector3D target) {

	Stuff::Vector3D start = getPosition();

	Stuff::Vector2DOf<float> start2d;
	start2d.x = start.x;
	start2d.y = start.y;
	Stuff::Vector2DOf<float> target2d;
	target2d.x = target.x;
	target2d.y = target.y;

	Stuff::Vector2DOf<float> deltaVector;
	deltaVector.Subtract(start2d, target2d);
	if ((deltaVector.x == 0.0) && (deltaVector.y == 0.0)) {
		return(start);
	}
	deltaVector.Normalize(deltaVector);
	float cellLength = (Terrain::worldUnitsPerCell);
	cellLength *= 0.5;
	deltaVector *= cellLength;
	//if (deltaVector.GetLength() == 0.0) {
	//	newGoal = start;
	//	return(NO_ERR);
	//}
	// Determine how far the ray must be cast...
	float distanceToTarget = distance_from(start, target) /* * metersPerWorldUnit*/;

	Stuff::Vector2DOf<float> curPoint;
	curPoint.x = target.x;
	curPoint.y = target.y;
	Stuff::Vector2DOf<float> curRay;
	curRay.x = 0.0;
	curRay.y = 0.0;
	float rayLength = 0.0;

	int cellR, cellC;
	Stuff::Vector3D curPoint3d(curPoint.x, curPoint.y, 0.0);
	land->worldToCell(curPoint3d, cellR, cellC);
	if (!GameMap->getPassable(cellR, cellC))
		while (rayLength < distanceToTarget) {
			curPoint3d.x = curPoint.x;
			curPoint3d.y = curPoint.y;
			curPoint3d.z = 0.0;
			land->worldToCell(curPoint3d, cellR, cellC);
			if (GameMap->getPassable(cellR, cellC))
				break;
			curPoint += deltaVector;
			curRay.Subtract(curPoint, start2d);
			rayLength = curRay.GetLength() /* * metersPerWorldUnit*/;
		}
	curPoint3d.z = land->getTerrainElevation(curPoint3d);
	return(curPoint3d);
}

constexpr int MAX_MOVE_GOALS = 60;

inline void insertMoveGoal (long goalList[MAX_MOVE_GOALS][3], long r, long c, long wt) {

	// This routine assumes wt > goaList[MAX_MOVE_GOALS - 1][2]...
	// AND that MAX_MOVE_GOALS >= 2...
    int i;
	for (i = MAX_MOVE_GOALS - 2; i > -1; i--)
		if (wt < goalList[i][2])
			break;

	if (i < (MAX_MOVE_GOALS - 2))
		memmove(&goalList[i + 2][0], &goalList[i + 1][0], (MAX_MOVE_GOALS - 2 - i) * sizeof(long) * 3);
	goalList[i + 1][0] = r;
	goalList[i + 1][1] = c;
	goalList[i + 1][2] = wt;
}

inline bool inMapBounds (long r, long c, long mapHeight, long mapWidth) {

	return((r >= 0) && (r < mapHeight) && (c >= 0) && (c < mapWidth));
}

long calcBestGoalFromTarget (int targetPos[2], int maxPos[2], float minRange, float bestRange, int* bestCell) {
float startLocal = 40.0f;

	Stuff::Vector3D startPos, endPos;
	startPos.Zero();
	endPos.Zero();
		
	land->getCellPos(maxPos[0], maxPos[1], endPos);
	land->getCellPos(targetPos[0], targetPos[1], startPos);

	Stuff::Vector3D deltaCellVec;
	deltaCellVec.y = maxPos[0] - targetPos[0];
	deltaCellVec.x = maxPos[1] - targetPos[1];
	deltaCellVec.z = 0.0f;
	float startHeight = startPos.z;

	float cellLength = (Terrain::worldUnitsPerCell * metersPerWorldUnit);
	float length = deltaCellVec.GetApproximateLength();

	long lastCellR = -1;
	long lastCellC = -1;
	if (length > Stuff::SMALL) {
		float colLength = deltaCellVec.x / length;
		float rowLength = deltaCellVec.y / length;
		float heightLen = (endPos.z - startPos.z) / length;
			
		float lastCol = fabs(colLength * 2.0);
		float lastRow = fabs(rowLength * 2.0);
			
		float startCellRow = targetPos[0];
		float startCellCol = targetPos[1];
			
		float endCellRow = maxPos[0];
		float endCellCol = maxPos[1];
			
		Stuff::Vector3D currentPos = startPos;
		Stuff::Vector3D dist;
		dist.Subtract(endPos,currentPos);
			
		float remainingDist = dist.GetApproximateLength();
		float bestRemainingDist = remainingDist - (bestRange * worldUnitsPerMeter);

		bool colDone = false, rowDone = false;
		while (!colDone || !rowDone) {
			if (fabs(startCellRow - endCellRow) > lastRow)	//DO NOT INCLUDE LAST CELL!!!!!
				startCellRow += rowLength;
			else {
				startCellRow = (endCellRow - lastRow);
				rowDone = true;
			}
				
			if (fabs(startCellCol - endCellCol) > lastCol)	//DO NOT INCLUDE LAST CELL!!!!!
				startCellCol += colLength;
			else {
				startCellCol = (endCellCol - lastCol);
				colDone = true;
			}
			startHeight += heightLen;
				
			long startCellC = startCellCol;
			long startCellR = startCellRow;

			if (!GameMap->inBounds(startCellR, startCellC))
				continue;

			land->getCellPos(startCellR,startCellC,currentPos);
			float localElev = (worldUnitsPerMeter * (float)GameMap->getLocalHeight(startCellR,startCellC));
			currentPos.z += localElev;
				
			if (startHeight+startLocal < currentPos.z)
				if (GameMap->inBounds(lastCellR, lastCellC) && GameMap->getCell(lastCellR, lastCellC)->getPassable()) {
					bestCell[0] = lastCellR;
					bestCell[1] = lastCellC;
					return(1);
				}

			dist.Subtract(endPos,currentPos);
			remainingDist = dist.GetApproximateLength();

			if (fabs(remainingDist - bestRemainingDist) < cellLength)
				if (remainingDist > bestRemainingDist)
					if (GameMap->inBounds(startCellR, startCellC) && GameMap->getCell(startCellR, startCellC)->getPassable()) {
						bestCell[0] = startCellR;
						bestCell[1] = startCellC;
						return(1);
					}

			if (remainingDist < 10.0) {
				if (GameMap->inBounds(startCellR, startCellC) && GameMap->getCell(startCellR, startCellC)->getPassable()) {
					bestCell[0] = startCellR;
					bestCell[1] = startCellC;
					return(1);
					}
				else
					return(0);
			}

			if (GameMap->inBounds(startCellR, startCellC) && GameMap->getCell(startCellR, startCellC)->getPassable()) {
				lastCellR = startCellR;
				lastCellC = startCellC;
			}
		}
	}

	if (lastCellR != -1) {
		bestCell[0] = lastCellR;
		bestCell[1] = lastCellC;
		return(1);
	}
	return(0);
}

void Mover::setMoveType (long type) {

	moveType = type;
	// For now, level = type. This can change in the future, so always set
	// it here!
	if (moveType == MOVETYPE_GROUND)
		moveLevel = 0;
	else if (moveType == MOVETYPE_AIR)
		moveLevel = 2;
	else
		STOP(("Mover.setMoveType: bad moveType %d", type));
}


long Mover::calcGlobalPath (GlobalPathStep* globalPath, GameObjectPtr obj, Stuff::Vector3D* location, bool useClosedAreas) {

#if 1
	long startArea = GlobalMoveMap[moveLevel]->calcArea(cellPositionRow, cellPositionCol);
	long goalArea = -1;
	int goalCell[2] = {-1, -1};
	if (obj) {
		goalArea = GlobalMoveMap[moveLevel]->calcArea(obj->cellPositionRow, obj->cellPositionCol);
		if (goalArea == -1) {
			Stuff::Vector3D goalPos = obj->getPosition();
			if (obj->isBuilding()) {
				BuildingPtr building = (BuildingPtr)obj;
				long goalRow = 0, goalCol = 0;
				bool foundGoal = building->calcAdjacentAreaCell(moveLevel, -1, goalRow, goalCol);
				if (foundGoal)
					land->cellToWorld(goalRow, goalCol, goalPos);
				else {
					Stuff::Vector3D objectPos = obj->getPosition();
					goalPos = calcOffsetMoveGoal(objectPos);
				}
				}
			else {
				Stuff::Vector3D objectPos = obj->getPosition();
				goalPos = calcOffsetMoveGoal(objectPos);
			}
			land->worldToCell(goalPos, goalCell[0], goalCell[1]);
			goalArea = GlobalMoveMap[moveLevel]->calcArea(goalCell[0], goalCell[1]);
		}
		}
	else if (location) {
		land->worldToCell(*location, goalCell[0], goalCell[1]);
		goalArea = GlobalMoveMap[moveLevel]->calcArea(goalCell[0], goalCell[1]);
	}
	if (goalArea == -1)
		return(0);

	bool startAreaOpen = (startArea >= 0) && GlobalMoveMap[moveLevel]->areas[startArea].open;
	long numSteps = -1;
	if (startAreaOpen) {
		if (numFunctionalWeapons > 0)
			GlobalMoveMap[moveLevel]->useClosedAreas = useClosedAreas;
		GlobalMoveMap[moveLevel]->moverTeamID = getTeamId();
		numSteps = GlobalMoveMap[moveLevel]->calcPath(startArea,
													  goalArea,
													  globalPath,
													  cellPositionRow,
													  cellPositionCol,
													  goalCell[0],
													  goalCell[1]
													  );
		GlobalMoveMap[moveLevel]->useClosedAreas = false;
	}
	return(numSteps);
#else
	return(0);
#endif
}

bool Mover::canMoveHere (Stuff::Vector3D worldPos) {

	if (moveLevel == 2)
		return(true);

	if (!GameMap->getPassable(worldPos))
		return(false);

	if (moveLevel == 1)
		return(true);

	return(TerrainRuntime::sampleWaterClass(worldPos) < 2);
}

long Mover::calcMoveGoal (GameObjectPtr target,
						  Stuff::Vector3D moveCenter,
						  float moveRadius,
						  Stuff::Vector3D moveGoal,
						  long selectionIndex,
						  Stuff::Vector3D& newGoal,
						  long numValidAreas,
						  short* validAreas,
						  unsigned long moveParams) {

	__int64 startTime = 0;

	if (goalMapRowStart[0] == -1) {
		for (long i = 0; i < GOALMAP_CELL_DIM; i++)
			goalMapRowStart[i] = i * GOALMAP_CELL_DIM;
		for (long r = 0; r < GOALMAP_CELL_DIM; r++)
			for (long c = 0; c < GOALMAP_CELL_DIM; c++) {
				long index = r * GOALMAP_CELL_DIM + c;
				goalMapRowCol[index][0] = r;
				goalMapRowCol[index][1] = c;
			}
	}

	bool playerMove = ((moveParams & MOVEPARAM_PLAYER) != 0);
	bool movingToRepair = (pilot->getCurTacOrder()->code == TACTICAL_ORDER_REFIT);
	bool movingToCapture = (pilot->getCurTacOrder()->code == TACTICAL_ORDER_CAPTURE);
	bool avoidStationaryMovers = movingToRepair;
	bool noTravelOffMap = !GameMap->getOffMap(cellPositionRow, cellPositionCol);

	if (moveParams & MOVEPARAM_MYSTERY_PARAM) {
		newGoal = moveGoal;
		return(NO_ERR);
	}

	if (moveParams & MOVEPARAM_STEP_TOWARD_TARGET) {
		Stuff::Vector3D aimVector;
		aimVector.Subtract(moveGoal, position);
		aimVector.z = 0;
		if (aimVector.GetLength() > 0.0) {
			aimVector.Normalize(aimVector);
			float stepLength = (Terrain::worldUnitsPerVertex * 1.5);
			aimVector.x *= stepLength;
			aimVector.y *= stepLength;
			aimVector.z = land->getTerrainElevation(aimVector);
			Stuff::Vector3D targetGoal;
			targetGoal.Add(position, aimVector);
			newGoal = calcOffsetMoveGoal(targetGoal);
		}

		return(NO_ERR);
	}

	if (moveParams & MOVEPARAM_STEP_ADJACENT_TARGET) {
		newGoal = calcOffsetMoveGoal(moveGoal);
		return(NO_ERR);
	}
	
	// The Goal Map is in CELLS...
	//memset(goalMap, 0, sizeof(long) * GOALMAP_CELL_DIM * GOALMAP_CELL_DIM);
    MemSet(goalMap, 0);

	int centerCell[2];
	land->worldToCell(moveCenter, centerCell[0], centerCell[1]);
	int goalCell[2];
	land->worldToCell(moveGoal, goalCell[0], goalCell[1]);

	int mapCellUL[2];
	mapCellUL[0] = centerCell[0] - GOALMAP_CELL_DIM / 2;
	mapCellUL[1] = centerCell[1] - GOALMAP_CELL_DIM / 2;
	
				Stuff::Vector3D start;
				land->cellToWorld(mapCellUL[0], mapCellUL[1], start);

	bool isAttackOrder = pilot->getCurTacOrder()->isCombatOrder();
	if (!target && !isAttackOrder) {
		newGoal = calcOffsetMoveGoal(moveGoal);
		return(NO_ERR);
	}

	// HACK!!!
	if (target && (pilot->getCurTacOrder()->code == TACTICAL_ORDER_GUARD))
		isAttackOrder = true;

	if (!target)
		moveGoal.z = land->getTerrainElevation(moveGoal);
	long optimalCellRange = 2;
	if (movingToRepair)
		optimalCellRange = 1;
	if (isAttackOrder) {
		float metersPerCell = Terrain::worldUnitsPerCell * metersPerWorldUnit;
		float fireRangeInMeters = pilot->getSituationFireRange();
		long fireCellRange = 0;
		if (fireRangeInMeters > 0.0) {
			fireCellRange = (long)(pilot->getSituationFireRange() / metersPerCell);
			if (fireCellRange < 1)
				fireCellRange = 1;
			if (fireCellRange > (MAX_ATTACK_CELLRANGE - 1)/*(GOALMAP_CELL_DIM / 2)*/)
				fireCellRange = (MAX_ATTACK_CELLRANGE - 1)/*(GOALMAP_CELL_DIM / 2)*/;
			}
		else if (fireRangeInMeters == -1.0)
			fireCellRange = 2;
		long minCellRange = (long)(getMinFireRange() / metersPerCell);
		if (minCellRange > (MAX_ATTACK_CELLRANGE - 1)/*(GOALMAP_CELL_DIM / 2)*/)
			minCellRange = (MAX_ATTACK_CELLRANGE - 1)/*(GOALMAP_CELL_DIM / 2)*/;
		long maxCellRange = (long)(getMaxFireRange() / metersPerCell);
		if (maxCellRange > (MAX_ATTACK_CELLRANGE - 1)/*(GOALMAP_CELL_DIM / 2)*/)
			maxCellRange = (MAX_ATTACK_CELLRANGE - 1)/*(GOALMAP_CELL_DIM / 2)*/;

		long fireRange = (long)pilot->getCurTacOrder()->attackParams.range;
		if (/*!playerMove && */(fireRange != FIRERANGE_SHORT) && (fireRange != FIRERANGE_MEDIUM) && (fireRange != FIRERANGE_LONG)) {
			long firstIndex = rangedCellsIndices[maxCellRange + 1][0];
			long lastIndex = rangedCellsIndices[MAX_ATTACK_CELLRANGE - 1][1];
			for (long i = firstIndex; i < lastIndex; i++) {
				long r = goalCell[0] - mapCellUL[0] + rangedCells[i][0];
				long c = goalCell[1] - mapCellUL[1] + rangedCells[i][1];
				if (inMapBounds(r, c, GOALMAP_CELL_DIM, GOALMAP_CELL_DIM))
					goalMap[goalMapRowStart[r] + c] -= 500;
			}
		}

		startTime = GetCycles();
		if (moveRadius > 0.0) {
			long moveCellRange = moveRadius / metersPerCell;
			if (moveCellRange < 1)
				moveCellRange = 1;
			if (moveCellRange > (MAX_ATTACK_CELLRANGE - 1))
				moveCellRange = (MAX_ATTACK_CELLRANGE - 1);
			long firstIndex = rangedCellsIndices[moveCellRange + 1][0];
			long lastIndex = rangedCellsIndices[MAX_ATTACK_CELLRANGE - 1][1];
			for (long i = firstIndex; i < lastIndex; i++) {
				long r = centerCell[0] - mapCellUL[0] + rangedCells[i][0];
				long c = centerCell[1] - mapCellUL[1] + rangedCells[i][1];
				if (inMapBounds(r, c, GOALMAP_CELL_DIM, GOALMAP_CELL_DIM))
					goalMap[goalMapRowStart[r] + c] -= 5000;
			}
		}
		if (fireRangeInMeters > 0.0) {
			// Due to new minimum range rule, we do NOT want to be within our
			// min range...
			if (minCellRange > 0) {
				long lastIndex = rangedCellsIndices[minCellRange][0];
				for (long i = 0; i < lastIndex; i++) {
					long r = goalCell[0] - mapCellUL[0] + rangedCells[i][0];
					long c = goalCell[1] - mapCellUL[1] + rangedCells[i][1];
					if (inMapBounds(r, c, GOALMAP_CELL_DIM, GOALMAP_CELL_DIM))
						goalMap[goalMapRowStart[r] + c] -= 500;
				}
			}
			}
		else if (fireRangeInMeters == -1.0)
			fireCellRange = 2;
		optimalCellRange = fireCellRange;
	}
	if (moveParams & MOVEPARAM_RANDOM_OPTIMAL)
		for (long i = 0; i < numOptimalIncrements; i++) {
			long r = goalCell[0] - mapCellUL[0] + optimalCells[optimalCellRange][i][0];
			long c = goalCell[0] - mapCellUL[0] + optimalCells[optimalCellRange][i][1];
			if (inMapBounds(r, c, GOALMAP_CELL_DIM, GOALMAP_CELL_DIM))
				goalMap[goalMapRowStart[r] + c] += (500 + RandomNumber(40) * 5);
		}
	else
		for (long i = 0; i < numOptimalIncrements; i++) {
			long r = goalCell[0] - mapCellUL[0] + optimalCells[optimalCellRange][i][0];
			long c = goalCell[0] - mapCellUL[0] + optimalCells[optimalCellRange][i][1];
			if (inMapBounds(r, c, GOALMAP_CELL_DIM, GOALMAP_CELL_DIM))
				goalMap[goalMapRowStart[r] + c] += 500;
		}

	int startCell[2];
	land->worldToCell(position, startCell[0], startCell[1]);
	startCell[0] -= mapCellUL[0];
	startCell[1] -= mapCellUL[1];
	if (startCell[0] < 0)
		startCell[0] = 0;
	else if (startCell[0] >= GOALMAP_CELL_DIM)
		startCell[0] = GOALMAP_CELL_DIM - 1;
	if (startCell[1] < 0)
		startCell[1] = 0;
	else if (startCell[1] >= GOALMAP_CELL_DIM)
		startCell[1] = GOALMAP_CELL_DIM - 1;

	if (!playerMove)
		for (int cellR = -1; cellR < 2; cellR++)
			for (int cellC = -1; cellC < 2; cellC++) {
				int r = goalCell[0] - mapCellUL[0] + cellR;
				int c = goalCell[1] - mapCellUL[1] + cellC;
				if (inMapBounds(r, c, GOALMAP_CELL_DIM, GOALMAP_CELL_DIM))
					goalMap[goalMapRowStart[r] + c] += 100;
			}

	// If we must move somewhere else...
	if (moveParams & MOVEPARAM_SOMEWHERE_ELSE)
		goalMap[goalMapRowStart[startCell[0]] + startCell[1]] -= 10000;

	for (int r = 0; r < GOALMAP_CELL_DIM; r++)
		for (int c = 0; c < GOALMAP_CELL_DIM; c++) {
			int dist = 0;
			if (r > startCell[0])
				dist += (r - startCell[0]);
			else
				dist += (startCell[0] - r);
			if (c > startCell[1])
				dist += (c - startCell[1]);
			else
				dist += (startCell[1] - c);
			goalMap[goalMapRowStart[r] + c] -= (dist * 1);
		}

	if (getGroup()) {
		MoverPtr mates[MAX_MOVERGROUP_COUNT];
		int numMates = getGroup()->getMovers(mates);
		for (int i = 0; i < numMates; i++) {
			if (mates[i] == this)
				continue;
			MechWarriorPtr pilot = mates[i]->getPilot();
			Stuff::Vector3D goal;
			goal.x = goal.y = goal.z = 0.0f;
			if (pilot && pilot->getMoveGlobalGoal(goal)) {
			    int worldCell[2];
				land->worldToCell(goal, worldCell[0], worldCell[1]);
				worldCell[0] -= mapCellUL[0];
				worldCell[1] -= mapCellUL[1];
				if (inMapBounds(worldCell[0], worldCell[1], GOALMAP_CELL_DIM, GOALMAP_CELL_DIM))
					goalMap[goalMapRowStart[worldCell[0]] + worldCell[1]] -= 100;
			}
		}
	}

	char validAreaTable[MAX_GLOBALMAP_AREAS];
	for (long i = 0; i < GlobalMoveMap[moveLevel]->numAreas; i++)
		validAreaTable[i] = -1;
	long curArea = GlobalMoveMap[moveLevel]->calcArea(cellPositionRow, cellPositionCol);
	long goalArea = GlobalMoveMap[moveLevel]->calcArea(goalCell[0], goalCell[1]);
	if (numValidAreas > 0) {
		for (long i = 0; i < numValidAreas; i++) {
			if (validAreas[i] == goalArea) {
				numValidAreas = i + 1;
				break;
			}
		}
	}
	for (int i = 0; i < numValidAreas; i++)
		validAreaTable[validAreas[i]] = 1;

	startTime = GetCycles();
	long deepWaterWeight = ((moveLevel == 1) ? 0 : 999999);

	for (int r = 0; r < GOALMAP_CELL_DIM; r++)
		for (long c = 0; c < GOALMAP_CELL_DIM; c++) {
			long curCellRow = mapCellUL[0] + r;
			long curCellCol = mapCellUL[1] + c;
			if (GameMap->inBounds(curCellRow, curCellCol)) 
			{
				MapCellPtr mapCell = GameMap->getCell(curCellRow, curCellCol);

				long goalMapIndex = goalMapRowStart[r] + c; 
				if (!mapCell->getPassable())
					goalMap[goalMapIndex] -= 10000;
				if (mapCell->getOffMap())
					goalMap[goalMapIndex] -= 10000;
				if (mapCell->getDeepWater())
					goalMap[goalMapIndex] -= deepWaterWeight;

				bool moverHere = mapCell->getMover();
				if (avoidStationaryMovers && moverHere)
					goalMap[goalMapIndex] -= 1000;

				if (noTravelOffMap && GameMap->getOffMap(curCellRow, curCellCol))
					goalMap[goalMapIndex] -= 50000;

				long area = GlobalMoveMap[moveLevel]->calcArea(curCellRow, curCellCol);
				if ((area != curArea) && (area != -1) && (curArea != -1)) {
					if (moveLevel < 2) {
						if (GlobalMoveMap[moveLevel]->getPathExists(curArea, area) == GLOBALPATH_EXISTS_UNKNOWN) {
							if (CalcValidAreaTable) {
								GlobalPathStep globalPath[128];
								long numSteps = GlobalMoveMap[moveLevel]->calcPath(curArea, area, globalPath);
								if (numSteps > 0) {
									for (long j = 0; j < numSteps; j++)
										for (long k = 0; k < numSteps; k++)
											GlobalMoveMap[moveLevel]->setPathExists(globalPath[j].thruArea, globalPath[k].thruArea, GLOBALPATH_EXISTS_TRUE);
									}
								else {
									GlobalMoveMap[moveLevel]->setPathExists(area, curArea, GLOBALPATH_EXISTS_FALSE);
									GlobalMoveMap[moveLevel]->setPathExists(curArea, area, GLOBALPATH_EXISTS_FALSE);
								}
								}
							else
								GlobalMoveMap[moveLevel]->setPathExists(curArea, area, GLOBALPATH_EXISTS_FALSE);
						}
						if (GlobalMoveMap[moveLevel]->getPathExists(curArea, area) == GLOBALPATH_EXISTS_FALSE)
							goalMap[goalMapRowStart[r] + c] -= 50000;
					}
				}
				}
			else
				goalMap[goalMapRowStart[r] + c] = -999999;
		}
#ifdef LAB_ONLY
		MCTimeCalcGoal1Update += (GetCycles() - startTime);
#endif

	startTime = GetCycles();
	long goalList[MAX_MOVE_GOALS][2];
	for (int i = 0; i < MAX_MOVE_GOALS; i++) {
		goalList[i][0] = -1;
		goalList[i][1] = -99999;
	}
	for (int i = 0; i < MAX_MOVE_GOALS; i++) {
		// This assumes that MAX_MOVE_GOALS >= 2...
		long weight = goalMap[i];
        int j;
		for (j = MAX_MOVE_GOALS - 2; j > -1; j--)
			if (weight < goalList[j][1])
				break;

		if (j < (MAX_MOVE_GOALS - 2))
			memmove(&goalList[j + 2][0], &goalList[j + 1][0], (MAX_MOVE_GOALS - 2 - j) * sizeof(long) * 2);
		goalList[j + 1][0] = i;
		goalList[j + 1][1] = weight;
	}
	for (int i = MAX_MOVE_GOALS; i < (GOALMAP_CELL_DIM * GOALMAP_CELL_DIM); i++) {
		// This routine assumes wt > goaList[MAX_MOVE_GOALS - 1][2]...
		// AND that MAX_MOVE_GOALS >= 2...
		long weight = goalMap[i];
		if (weight > goalList[MAX_MOVE_GOALS - 1][1]) {
            int j;
			for (j = MAX_MOVE_GOALS - 2; j > -1; j--)
				if (weight < goalList[j][1])
					break;
			if (j < (MAX_MOVE_GOALS - 2))
				memmove(&goalList[j + 2][0], &goalList[j + 1][0], (MAX_MOVE_GOALS - 2 - j) * sizeof(long) * 2);
			goalList[j + 1][0] = i;
			goalList[j + 1][1] = weight;
		}
	}

	if ((getObjectClass() == ELEMENTAL) && target && getGroup()) {
		MoverPtr mates[MAX_MOVERGROUP_COUNT];
		long numMates = getGroup()->getMovers(mates);
		selectionIndex = 0;
		for (long i = 0; i < numMates; i++) {
			if (mates[i] == this)
				break;
			MechWarriorPtr pilot = mates[i]->getPilot();
			if (pilot) {
				if (pilot->getCurrentTarget() == target)
					selectionIndex++;
			}
		}
	}

	// so they don't select wacky goals
	selectionIndex = -1;

	long curGoalIndex = 0;
	if ((selectionIndex > 0) && (selectionIndex < MAX_MOVE_GOALS))
		curGoalIndex = selectionIndex;
	long curGoalCell[2];
	bool noLOF = true;
	curGoalCell[0] = mapCellUL[0] + goalMapRowCol[goalList[0][0]][0];
	curGoalCell[1] = mapCellUL[1] + goalMapRowCol[goalList[0][0]][1];
		
	if (hasWeaponNode()) {
		Stuff::Vector3D curMoverPosition;
		curMoverPosition = position;
		long curMoverRow = cellPositionRow;
		long curMoverCol = cellPositionCol;

		startTime = GetCycles();
		long i = 0;
		ObjectManager->useMoverLineOfSightTable = false;
		while (noLOF && (i < MaxMoveGoalChecks)) {
			long index = goalList[i][0];
            long weight =  goalList[i][1];
			curGoalCell[0] = mapCellUL[0] + goalMapRowCol[index][0];
			curGoalCell[1] = mapCellUL[1] + goalMapRowCol[index][1];
			i++;
            // sebi: ORIG BUG FIX because we increased i here, we will get weight from one iteration after we took index, and to make situation worse
            // we'll also read from behind goalList array at last iteration (when i == MaxMoveGoalChecks)
			//if (goalList[i][1] > -900000) {
			if (weight > -900000) {
				Stuff::Vector3D start;
				land->cellToWorld(curGoalCell[0], curGoalCell[1], start);
				start.z = land->getTerrainElevation(start)/* + 25.0f*/;		//For that matter, so do we!
				position = start;
				cellPositionRow = curGoalCell[0];
				cellPositionCol = curGoalCell[1];
				__int64 lstartTime = GetCycles();
				if (weight > -10000) {
					if (movingToCapture)
						noLOF = false;
					else if (target)
						noLOF = !lineOfSight(target, 0.0f);
					else
						noLOF = !lineOfSight(moveGoal);
				}
			}
		}
		ObjectManager->useMoverLineOfSightTable = true;
		position = curMoverPosition;
		cellPositionRow = curMoverRow;
		cellPositionCol = curMoverCol;
	}

	GameMap->clearCellDebugs(2);

	startTime = GetCycles();
	if (noLOF && hasWeaponNode()) {
		int targetPos[2], maxPos[2], bestCell[4][2];
		float castRange;
		int result[4];
		int resultCost[4] = {0, 0, 0, 0};
		int confidence[4];
		int bestBest = 0;
		if (moveRadius > 0.0) {
			targetPos[0] = centerCell[0];
			targetPos[1] = centerCell[1];
			maxPos[0] = goalCell[0];
			maxPos[1] = goalCell[1];
			castRange = moveRadius;
			result[0] = calcBestGoalFromTarget(targetPos, maxPos, 0.0, castRange, bestCell[0]);
			if (result[0])
				resultCost[0] = GlobalMoveMap[moveLevel]->getPathCost(GlobalMoveMap[moveLevel]->calcArea(cellPositionRow, cellPositionCol),GlobalMoveMap[moveLevel]->calcArea(bestCell[0][0], bestCell[0][1]), false, confidence[0], true);
			bestCell[1][0] = centerCell[0];
			bestCell[1][1] = centerCell[1];
			result[1] = 1;
			if (result[1])
				resultCost[1] = GlobalMoveMap[moveLevel]->getPathCost(GlobalMoveMap[moveLevel]->calcArea(cellPositionRow, cellPositionCol),GlobalMoveMap[moveLevel]->calcArea(bestCell[1][0], bestCell[1][1]), false, confidence[1], true);
			if (resultCost[1] > 0)
				if (resultCost[1] < resultCost[bestBest])
					bestBest = 1;
			if (resultCost[bestBest] > 0) {
				curGoalCell[0] = bestCell[bestBest][0];
				curGoalCell[1] = bestCell[bestBest][1];
			}
			if (DebugGameObject[0] == this) {
				if (result[0])
					GameMap->setCellDebug(bestCell[0][0], bestCell[0][1], 3, 2);
				if (result[1])
					GameMap->setCellDebug(bestCell[1][0], bestCell[1][1], 3, 2);
			}
 			}
		else {
			targetPos[0] = goalCell[0];
			targetPos[1] = goalCell[1];
			maxPos[0] = targetPos[0] - 10;
			maxPos[1] = targetPos[1];
			castRange = pilot->getSituationFireRange();
			long bestBest = 0;
			result[0] = calcBestGoalFromTarget(targetPos, maxPos, 0.0, castRange, bestCell[0]);
			if (result[0])
				resultCost[0] = GlobalMoveMap[moveLevel]->getPathCost(GlobalMoveMap[moveLevel]->calcArea(cellPositionRow, cellPositionCol),GlobalMoveMap[moveLevel]->calcArea(bestCell[0][0], bestCell[0][1]), false, confidence[0], true);
			maxPos[0] = targetPos[0];
			maxPos[1] = targetPos[1] + 10;
			result[1] = calcBestGoalFromTarget(targetPos, maxPos, 0.0, pilot->getSituationFireRange(), bestCell[1]);
			if (result[1])
				resultCost[1] = GlobalMoveMap[moveLevel]->getPathCost(GlobalMoveMap[moveLevel]->calcArea(cellPositionRow, cellPositionCol),GlobalMoveMap[moveLevel]->calcArea(bestCell[1][0], bestCell[1][1]), false, confidence[1], true);
			if (resultCost[1] > 0)
				if (resultCost[1] < resultCost[bestBest])
					bestBest = 1;
			maxPos[0] = targetPos[0] + 10;
			maxPos[1] = targetPos[1];
			result[2] = calcBestGoalFromTarget(targetPos, maxPos, 0.0, pilot->getSituationFireRange(), bestCell[2]);
			if (result[2])
				resultCost[2] = GlobalMoveMap[moveLevel]->getPathCost(GlobalMoveMap[moveLevel]->calcArea(cellPositionRow, cellPositionCol), GlobalMoveMap[moveLevel]->calcArea(bestCell[2][0], bestCell[2][1]), false, confidence[2], true);
			if (resultCost[2] > 0)
				if (resultCost[2] < resultCost[bestBest])
					bestBest = 2;
			maxPos[0] = targetPos[0];
			maxPos[1] = targetPos[1] - 10;
			result[3] = calcBestGoalFromTarget(targetPos, maxPos, 0.0, pilot->getSituationFireRange(), bestCell[3]);
			if (result[3])
				resultCost[3] = GlobalMoveMap[moveLevel]->getPathCost(GlobalMoveMap[moveLevel]->calcArea(cellPositionRow, cellPositionCol),GlobalMoveMap[moveLevel]->calcArea(bestCell[3][0], bestCell[3][1]), false, confidence[3], true);
			if (resultCost[3] > 0)
				if (resultCost[3] < resultCost[bestBest])
					bestBest = 3;
			if (resultCost[bestBest] > 0) {
				curGoalCell[0] = bestCell[bestBest][0];
				curGoalCell[1] = bestCell[bestBest][1];
			}
			if (DebugGameObject[0] == this) {
				if (result[0])
					GameMap->setCellDebug(bestCell[0][0], bestCell[0][1], 3, 2);
				if (result[1])
					GameMap->setCellDebug(bestCell[1][0], bestCell[1][1], 3, 2);
				if (result[2])
					GameMap->setCellDebug(bestCell[2][0], bestCell[2][1], 3, 2);
				if (result[3])
					GameMap->setCellDebug(bestCell[3][0], bestCell[3][1], 3, 2);
			}
		}
	}
	GameMap->setCellDebug(curGoalCell[0], curGoalCell[1], 3, 2);	
	land->cellToWorld(curGoalCell[0], curGoalCell[1], newGoal);
	newGoal = calcOffsetMoveGoal(newGoal);
	return(NO_ERR);
}

int Mover::calcMovePath (MovePathPtr path,
						  long pathType,
						  Stuff::Vector3D start,
						  Stuff::Vector3D goal,
						  int* goalCell,
						  unsigned long moveParams) {

	// This assumes the goal is already the "optimum" goal (it should have been
	// passed thru "calcMoveGoal" before being sent here)...

	if (!PathFindMap[SECTOR_PATHMAP] || !PathFindMap[SIMPLE_PATHMAP])
		Fatal(0, " No PathFindMap in Mover::calcMovePath ");

	int posCellR, posCellC;
	land->worldToCell(start, posCellR, posCellC);

	int goalCellR, goalCellC;
	land->worldToCell(goal, goalCellR, goalCellC);

	path->clear();
	int result = 0;

	if (pathType == MOVEPATH_SIMPLE) {
		long mapULr = posCellR - SimpleMovePathRange;
		if (mapULr < 0)
			mapULr = 0;
		long mapULc = posCellC - SimpleMovePathRange;
		if (mapULc < 0)
			mapULc = 0;

		float cellLength = (Terrain::worldUnitsPerCell * metersPerWorldUnit);
		long clearCost = 0;
		if (maxMoveSpeed != 0.0)
			clearCost = (float2short)(cellLength / maxMoveSpeed * 50.0);
		if (clearCost > 0) {
			long jumpCost = 0;
			long numOffsets = 8;
			if (!pilot->onHomeTeam() && !MPlayer)
				getJumpRange(&numOffsets, &jumpCost);
			#ifdef USE_ELEMENTALS
			if (getObjectClass() == ELEMENTAL) {
				GameObjectPtr target = pilot->getLastTarget();
				if (target && (distanceFrom(target->getPosition()) < ElementalTargetNoJumpDistance)) {
					jumpCost = 0;
					numOffsets = 8;
					}
				else
					JumpOnBlocked = true;
			}
			#endif
			if (isMineSweeper())
				moveParams |= MOVEPARAM_SWEEP_MINES;
			if (followRoads)
				moveParams |= MOVEPARAM_FOLLOW_ROADS;
			if (isMech())
				moveParams |= MOVEPARAM_WATER_SHALLOW;
			if (moveLevel == 1)
				moveParams |= (MOVEPARAM_WATER_SHALLOW + MOVEPARAM_WATER_DEEP);
			PathFindMap[SIMPLE_PATHMAP]->setMover(getWatchID(), getTeamId(), isLayingMines());
			PathFindMap[SIMPLE_PATHMAP]->setUp(mapULr,
							   mapULc,
							   SimpleMovePathRange * 2 + 1,
							   SimpleMovePathRange * 2 + 1,
							   moveLevel,
							   &start,
							   posCellR,
							   posCellC,
							   goal,
							   goalCellR - mapULr,
							   goalCellC - mapULc,
							   clearCost,
							   jumpCost,
							   numOffsets,
							   moveParams);

			DebugMovePathType = pathType;

			int goalCell[2];
			if (numOffsets > 8) {
				result = PathFindMap[SIMPLE_PATHMAP]->calcPathJUMP(path, nullptr, goalCell);
				// PATHFINDING-JUMP-FAIL-1: doomed jump solve -> stamp the REQUESTED
				// goal cell (goalCell out-param is only written on success). No-op
				// unless gate on. See requestMovePath.
				if ((result <= 0) && pilot)
					pilot->noteJumpPathFailed();
			}
			else
				result = PathFindMap[SIMPLE_PATHMAP]->calcPath(path, nullptr, goalCell);
			PathFindMap[SIMPLE_PATHMAP]->setMover(0);
			JumpOnBlocked = false;
		}
		}
	else {
		// We are now assuming all local pathfinding will fit within two adjacent sectors.
		// So, we can assume both the start and goal locations are within the two adj sectors...
		float cellLength = (Terrain::worldUnitsPerCell * metersPerWorldUnit);
		long clearCost = 0;
		if (maxMoveSpeed != 0.0)
			clearCost = (long)(cellLength / maxMoveSpeed * 50.0);
		if (clearCost > 0) {
			long jumpCost = 0;
			long numOffsets = 8;
			if (!pilot->onHomeTeam() && !MPlayer)
				getJumpRange(&numOffsets, &jumpCost);
			long startSector[2];
			startSector[0] = posCellR / SECTOR_DIM;
			startSector[1] = posCellC / SECTOR_DIM;
			long goalSector[2];
			goalSector[0] = goalCellR / SECTOR_DIM;
			goalSector[1] = goalCellC / SECTOR_DIM;
			long sectorULr = startSector[0] * SECTOR_DIM;
			if (startSector[0] > goalSector[0])
				sectorULr = goalSector[0] * SECTOR_DIM;
			long sectorULc = startSector[1] * SECTOR_DIM;
			if (startSector[1] > goalSector[1])
				sectorULc = goalSector[1] * SECTOR_DIM;

			#ifdef USE_ELEMENTALS
			if (getObjectClass() == ELEMENTAL) {
				GameObjectPtr target = pilot->getLastTarget();
				if (target && (distanceFrom(target->getPosition()) < ElementalTargetNoJumpDistance)) {
					jumpCost = 0;
					numOffsets = 8;
					}
				else
					JumpOnBlocked = true;
			}
			#endif
			if (isMineSweeper())
				moveParams |= MOVEPARAM_SWEEP_MINES;
			if (followRoads)
				moveParams |= MOVEPARAM_FOLLOW_ROADS;
			if (isMech())
				moveParams |= MOVEPARAM_WATER_SHALLOW;
			if (moveLevel == 1)
				moveParams |= (MOVEPARAM_WATER_SHALLOW + MOVEPARAM_WATER_DEEP);
			if (moveParams & MOVEPARAM_JUMP)
				moveParams |= 0;
			PathFindMap[SECTOR_PATHMAP]->setMover(getWatchID(), getTeamId(), isLayingMines());
			PathFindMap[SECTOR_PATHMAP]->setUp(sectorULr,
							   sectorULc,
							   SECTOR_DIM * 2,
							   SECTOR_DIM * 2,
							   moveLevel,
							   &start,
							   posCellR,
							   posCellC,
							   goal,
							   goalCellR - sectorULr,
							   goalCellC - sectorULc,
							   clearCost,
							   jumpCost,
							   numOffsets,
							   moveParams);

			DebugMovePathType = pathType;

			if (numOffsets > 8) {
				result = PathFindMap[SECTOR_PATHMAP]->calcPathJUMP(path, nullptr, goalCell);
				// PATHFINDING-JUMP-FAIL-1: doomed jump solve -> stamp the REQUESTED
				// goal cell (goalCell out-param is only written on success). No-op
				// unless gate on. See requestMovePath.
				if ((result <= 0) && pilot)
					pilot->noteJumpPathFailed();
			}
			else
				result = PathFindMap[SECTOR_PATHMAP]->calcPath(path, nullptr, goalCell);
			PathFindMap[SECTOR_PATHMAP]->setMover(0);
			JumpOnBlocked = false;
		}
	}

	return(result);
}

int Mover::calcEscapePath (MovePathPtr path,
							Stuff::Vector3D start,
							Stuff::Vector3D goal,
							int* goalCell,
							unsigned long moveParams,
							Stuff::Vector3D& escapeGoal) {

	escapeGoal.x = -999999.0;
	escapeGoal.y = -999999.0;
	escapeGoal.z = -999999.0;

	if (!PathFindMap[SECTOR_PATHMAP] || !PathFindMap[SIMPLE_PATHMAP])
		Fatal(0, " No PathFindMap in Mover::calcMovePath ");

	int posCellR, posCellC;
	land->worldToCell(start, posCellR, posCellC);

	int goalCellR, goalCellC;
	land->worldToCell(goal, goalCellR, goalCellC);

	path->clear();
	long result = 0;

	long mapULr = posCellR - SimpleMovePathRange;
	if (mapULr < 0)
		mapULr = 0;
	long mapULc = posCellC - SimpleMovePathRange;
	if (mapULc < 0)
		mapULc = 0;

	float cellLength = (Terrain::worldUnitsPerCell * metersPerWorldUnit);
	long clearCost = 0;
	if (maxMoveSpeed != 0.0)
		clearCost = (float2short)(cellLength / maxMoveSpeed * 50.0);
	if (clearCost > 0) {
		long jumpCost = 0;
		long numOffsets = 8;
		if (!pilot->onHomeTeam() && !MPlayer)
			getJumpRange(&numOffsets, &jumpCost);

#ifdef USE_ELEMENTALS
		if (getObjectClass() == ELEMENTAL) {
			GameObjectPtr target = pilot->getLastTarget();
			if (target && (distanceFrom(target->getPosition()) < ElementalTargetNoJumpDistance)) {
				jumpCost = 0;
				numOffsets = 8;
				}
			else
				JumpOnBlocked = true;
		}
#endif
		if (isMineSweeper())
			moveParams |= MOVEPARAM_SWEEP_MINES;
		if (followRoads)
			moveParams |= MOVEPARAM_FOLLOW_ROADS;
		FindingEscapePath = true;
		PathFindMap[SIMPLE_PATHMAP]->setMover(getWatchID(), getTeamId(), isLayingMines());
		PathFindMap[SIMPLE_PATHMAP]->setUp(mapULr,
						   mapULc,
						   SimpleMovePathRange * 2 + 1,
						   SimpleMovePathRange * 2 + 1,
						   moveLevel,
						   &start,
						   posCellR,
						   posCellC,
						   goal,
						   goalCellR - mapULr,
						   goalCellC - mapULc,
						   clearCost,
						   jumpCost,
						   numOffsets,
						   moveParams);

		DebugMovePathType = 0;

		long goalCell[2];
		result = PathFindMap[SIMPLE_PATHMAP]->calcEscapePath(path, &escapeGoal, goalCell);
		JumpOnBlocked = false;
		FindingEscapePath = false;
	}

	return(result);
}

bool Mover::getAdjacentCellPathLocked (long level, long cellRow, long cellCol, long dir) {

	static long adjCellTable[8][2] = {
		{-1, 0},
		{-1, 1},
		{0, 1},
		{1, 1},
		{1, 0},
		{1, -1},
		{0, -1},
		{-1, -1}
	};
	long adjCellRow = cellRow + adjCellTable[dir][0];
	long adjCellCol = cellCol + adjCellTable[dir][1];
	if (level < 0)
		return(false);
	if (level > (NUM_MOVE_LEVELS - 1))
		return(false);
	return(GameMap->getPathlock(level, adjCellRow, adjCellCol));
}

bool Mover::getPathRangeLock (long range, bool* reachedEnd) {

	MovePathPtr path = pilot->getMovePath();
	if (path)
		return(path->isLocked((moveLevel == 2), -1, range, reachedEnd));
    if(reachedEnd)
       *reachedEnd = true; // sebi: !NB false ? need to init to something, to not leave garbage there
	return(false);
}

long Mover::setPathRangeLock (bool set, long range) {

	MovePathPtr path = pilot->getMovePath();
	long lockLevel = (moveLevel == 2);
	if (set) {
		if (pathLockLength > 0)
			setPathRangeLock(false);
		if (path && (path->numSteps > 0)) {
			long start = path->curStep;
			long lastStep = start + range;
			if (lastStep >= path->numStepsWhenNotPaused)
				lastStep = path->numStepsWhenNotPaused;
			pathLockLength = 0;
			for (long i = start; i < lastStep; i++) {
				if (GameMap->getPathlock(lockLevel, path->stepList[i].cell[0], path->stepList[i].cell[1]))
					return(-1);
				GameMap->setPathlock((moveLevel == 2), path->stepList[i].cell[0], path->stepList[i].cell[1], true);
				pathLockList[pathLockLength][0] = path->stepList[i].cell[0];
				pathLockList[pathLockLength][1] = path->stepList[i].cell[1];
				pathLockLength++;
			}
		}
		}
	else {
		for (long i = 0; i < pathLockLength; i++)
			GameMap->setPathlock(lockLevel, pathLockList[i][0], pathLockList[i][1], false);
		pathLockLength = 0;
	}

	return(NO_ERR);
}

void Mover::updatePathLock (bool set)
{
	MoveReconScope _recon_pl(&g_moveRecon_pathlock_ns, &g_moveRecon_frame_pathlock_ns);

	if (getObjectClass() == BATTLEMECH)
		if (((BattleMechPtr)this)->inJump)
			return;

	if (!pathLocks)
		return;
		
	GameMap->setPathlock((moveLevel == 2), cellPositionRow, cellPositionCol, set);

	// If we're yielding, we should NOT set our path range on. If we are yielding, we can always
	// remove what we've already pathlocked...
	if (!set || !pilot->isYielding())
		setPathRangeLock(set, crashBlockPath);
}

bool Mover::getPathRangeBlocked (long range, bool* reachedEnd) {

	if (moveLevel > 0)
		return(false);
	MovePathPtr path = pilot->getMovePath();
	if (path)
		return(path->isBlocked(-1, range, reachedEnd));
    if(reachedEnd)
        *reachedEnd = true; // sebi: !NB set to soemthing to not leave garbage
	return(false);
}

void Mover::updateHustleTime (void) {
	// Intentionally empty: the bridge-blocking hustle timer is not needed until bridges come back.
}

long Mover::bounceToAdjCell (void) {

	for (long dir = 0; dir < 8; dir++) {
		static long adjCellTable[8][2] = {
			{-1, 0},
			{-1, 1},
			{0,	1},
			{1, 1},
			{1, 0},
			{1, -1},
			{0, -1},
			{-1, -1}
		};
		long adjRow = cellPositionRow + adjCellTable[dir][0];
		long adjCol = cellPositionCol + adjCellTable[dir][1];
		bool cellPathLocked = GameMap->getPathlock((moveLevel == 2), adjRow, adjCol);
		bool cellPassable = GameMap->getPassable(adjRow, adjCol);

		if (!cellPathLocked && cellPassable) {
			bool pathLockMarked = GameMap->getPathlock((moveLevel == 2), cellPositionRow, cellPositionCol);
			if (pathLockMarked)
				updatePathLock(false);
			Stuff::Vector3D newPosition;
			land->cellToWorld(adjRow, adjCol, newPosition);
			setPosition(newPosition);
			pilot->pausePath();
			if (pathLockMarked)
				updatePathLock(true);
			return(dir);
		}
	}
	return(-1);
}

int Mover::calcMovePath (MovePathPtr path,
						  Stuff::Vector3D start,
						  long thruArea[2],
						  long goalDoor,
						  Stuff::Vector3D finalGoal,
						  Stuff::Vector3D* goal,
						  int* goalCell,
						  unsigned long moveParams) {

	// This assumes the goal is already the "optimum" goal (it should have been
	// passed thru "calcMoveGoal" before being sent here)...
	if (!PathFindMap[SECTOR_PATHMAP] || !PathFindMap[SIMPLE_PATHMAP])
		Fatal(0, " No PathFindMap in Mover::calcMovePath ");

	// We are now assuming all local pathfinding will fit within one sector. So, we can
	// assume both the start and goal locations are within the sector...

	long result = 0;
	path->clear();

	float cellLength = (Terrain::worldUnitsPerCell * metersPerWorldUnit);
	long clearCost = 0;
	if (maxMoveSpeed != 0)
		clearCost = (long)(cellLength / maxMoveSpeed * 50.0);
	if (clearCost > 0) {
		long jumpCost = 0;
		long numOffsets = 8;
		if (!pilot->onHomeTeam() && !MPlayer)
			getJumpRange(&numOffsets, &jumpCost);

		int posCellR, posCellC;
		land->worldToCell(start, posCellR, posCellC);

		#ifdef USE_ELEMENTALS
		if (getObjectClass() == ELEMENTAL) {
			GameObjectPtr target = pilot->getLastTarget();
			if (target && (distanceFrom(target->getPosition()) < ElementalTargetNoJumpDistance)) {
				jumpCost = 0;
				numOffsets = 8;
				}
			else
				JumpOnBlocked = true;
		}
		#endif

		if (isMineSweeper())
			moveParams |= MOVEPARAM_SWEEP_MINES;
		if (followRoads)
			moveParams |= MOVEPARAM_FOLLOW_ROADS;
		if (isMech())
			moveParams |= MOVEPARAM_WATER_SHALLOW;
		if (moveLevel == 1)
			moveParams |= (MOVEPARAM_WATER_SHALLOW + MOVEPARAM_WATER_DEEP);
		PathFindMap[SECTOR_PATHMAP]->setMover(getWatchID(), getTeamId(), isLayingMines());
		result = PathFindMap[SECTOR_PATHMAP]->setUp(
									moveLevel,
									&start,
									posCellR,
									posCellC,
									thruArea,
									goalDoor,
									finalGoal,
									clearCost,
									jumpCost,
									numOffsets,
									moveParams);
		if (result == -1) {
			JumpOnBlocked = false;
			return(-999);
		}

		if (numOffsets > 8) {
			result = PathFindMap[SECTOR_PATHMAP]->calcPathJUMP(path, goal, goalCell);
			// PATHFINDING-JUMP-FAIL-1: doomed jump solve -> stamp the REQUESTED goal
			// cell (goalCell out-param is only written on success). No-op unless gate
			// on. See requestMovePath.
			if ((result <= 0) && pilot)
				pilot->noteJumpPathFailed();
		}
		else
			result = PathFindMap[SECTOR_PATHMAP]->calcPath(path, goal, goalCell);
		PathFindMap[SECTOR_PATHMAP]->setMover(0);
		JumpOnBlocked = false;
	}

	return(result);
}
