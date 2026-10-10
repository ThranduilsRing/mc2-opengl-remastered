// MechWarrior: move goals and waypoint paths, path calculation and the movement decision tree.
// Declared in the class headers; shared names in warrior_internal.h.
#include "warrior_internal.h"


long MechWarrior::setMoveGoal (unsigned long type, Stuff::Vector3D* location, GameObjectPtr obj) {

	moveOrders.goalType = type;
	switch (type) {
		case MOVEGOAL_NONE:
			clearMoveGoal();
			break;
		case MOVEGOAL_LOCATION:
			if (location->z < -10.0)
				location->z = TerrainRuntime::groundElevation(*location);
			moveOrders.goalLocation = *location;
			moveOrders.goalObjectWID = 0;
			break;
		default:
			if (location->z < -10.0)
				location->z = TerrainRuntime::groundElevation(*location);
			moveOrders.goalLocation = *location;
			moveOrders.goalObjectWID = type;
			break;
	}
	return(NO_ERR);
}

unsigned long MechWarrior::getMoveGoal (Stuff::Vector3D* location, GameObjectPtr* obj) {

	if (obj)
		*obj = ObjectManager->getByWatchID(moveOrders.goalObjectWID);
	if (location)
		*location = moveOrders.goalLocation;
	return(moveOrders.goalType);
}

void MechWarrior::pausePath (void) {

	if (moveOrders.path[0])
		moveOrders.path[0]->numSteps = 0;
}

void MechWarrior::resumePath (void) {

	if (moveOrders.path[0])
		moveOrders.path[0]->numSteps = moveOrders.path[0]->numStepsWhenNotPaused;
}

void MechWarrior::rethinkPath (unsigned long strategy) {

	Stuff::Vector3D nextWayPoint;
	bool haveWayPoint = getNextWayPoint(nextWayPoint, false);

	if (moveOrders.pathType == MOVEPATH_SIMPLE) {
		if (!haveWayPoint) {
			clearMoveOrders();
			if (curTacOrder.isMoveOrder() || curTacOrder.isWayPathOrder())
				clearCurTacOrder();
			triggerAlarm(PILOT_ALARM_NO_MOVEPATH, MOVEPATH_ERR_EARLY_STOP);
			}
		else {
			moveOrders.path[0]->numSteps = 0;
			unsigned long params = MOVEPARAM_RECALC | MOVEPARAM_AVOID_PATHLOCKS | MOVEPARAM_FACE_TARGET;
			if (curTacOrder.moveParams.jump)
				params |= MOVEPARAM_JUMP;
			requestMovePath(curTacOrder.selectionIndex, params, 2);
		}
		}
	else if (moveOrders.pathType == MOVEPATH_COMPLEX) {
		if (moveOrders.path[0]->globalStep == (moveOrders.numGlobalSteps - 1)) {
			if (!haveWayPoint) {
				clearMoveOrders();
				if (curTacOrder.isMoveOrder() || curTacOrder.isWayPathOrder())
					clearCurTacOrder();
				triggerAlarm(PILOT_ALARM_NO_MOVEPATH, MOVEPATH_ERR_EARLY_STOP);
			}
			}
		else {
				moveOrders.path[0]->numSteps = 0;
				unsigned long params = MOVEPARAM_RECALC | MOVEPARAM_AVOID_PATHLOCKS | MOVEPARAM_FACE_TARGET;
				if (curTacOrder.moveParams.jump)
					params |= MOVEPARAM_JUMP;
				requestMovePath(curTacOrder.selectionIndex, params, 3);
		}
	}
}

float MechWarrior::getMoveDistanceLeft (void) {

	float distanceLeft = 0.0;
	
	MovePathPtr curPath = moveOrders.path[0]; //getMovePath();
	if (curPath && (curPath->numStepsWhenNotPaused > 0)) {
		distanceLeft = curPath->getDistanceLeft(getVehicle()->getPosition());
		if (moveOrders.pathType == MOVEPATH_COMPLEX) {
			// Ultimately, we should have a pre-calced distanceToGoal field
			// for each door-to-door link which we can use as an estimate.
			// For now, we can just assume 100.0 meters per area...
			distanceLeft += (float)(moveOrders.globalPath[curPath->globalStep].costToGoal); // ((moveOrders.numGlobalSteps - curPath->globalStep) * 50.0);
		}
	}
	return(distanceLeft);
}

bool MechWarrior::isJumping (Stuff::Vector3D* jumpGoal) {

	MoverPtr myVehicle = getVehicle();
	if (myVehicle)
		return(myVehicle->isJumping(jumpGoal));
	return(false);
}

MovePathPtr MechWarrior::getMovePath (void) {

	Assert((moveOrders.path[0] != NULL) && (moveOrders.path[1] != NULL), 0, " NULL move paths ");

	if (moveOrders.path[0]->numStepsWhenNotPaused == 0) {
		clearMovePath(0);
		MovePathPtr temp = moveOrders.path[0];
		moveOrders.path[0] = moveOrders.path[1];
		moveOrders.path[1] = temp;
		if (moveOrders.path[0]->numStepsWhenNotPaused > 0) {
			Stuff::Vector3D goal;
			GameObjectPtr goalObject;
			unsigned long goalType = getMoveGoal(&goal, &goalObject);
			if (goalType == MOVEGOAL_NONE) {
				moveOrders.path[0]->numStepsWhenNotPaused = 0;
				return(moveOrders.path[0]);
			}

			GameObjectPtr target = NULL;
			if (goalType > 0) {
				if (goalObject)
					target = goalObject;
				else
					target = ObjectManager->get(goalType);
				if (target)
					moveOrders.path[0]->target = target->getPosition();
				else {
					moveOrders.path[0]->numStepsWhenNotPaused = 0;
					return(moveOrders.path[0]);
				}
			}
	
			if (isYielding()) {
				moveOrders.yieldTime = scenarioTime + 1.5;
				moveOrders.path[0]->numSteps = 0;
				}
			else if (isWaitingForPoint()) {
				moveOrders.path[0]->numSteps = 0;
				}
			else {
				moveOrders.yieldTime = -1.0;
				moveOrders.yieldState = 0;
			}
			
			setMoveGoal((target ? target->getWatchID() : MOVEGOAL_LOCATION), &(moveOrders.path[0]->goal));
			if (moveOrders.path[0]->globalStep == (moveOrders.numGlobalSteps - 1)) {
					moveOrders.globalGoalLocation = moveOrders.path[0]->stepList[moveOrders.path[0]->numStepsWhenNotPaused - 1].destination;
					curTacOrder.setWayPoint(0, moveOrders.globalGoalLocation);
			}
		}
	}

	return(moveOrders.path[0]);
}

bool InitWayPath = true;

void MechWarrior::setMoveWayPath (WayPathPtr wayPath, bool patrol) {

	if (wayPath) {
		for (long curPt = 0; curPt < wayPath->numPoints; curPt++) {
			moveOrders.wayPath[curPt].x = wayPath->points[curPt * 3];
			moveOrders.wayPath[curPt].y = wayPath->points[curPt * 3 + 1];
			moveOrders.wayPath[curPt].z = wayPath->points[curPt * 3 + 2];
		}

		moveOrders.numWayPts = wayPath->numPoints;
		}
	else
		moveOrders.numWayPts = 0;
if (InitWayPath) {
	moveOrders.curWayPt = 0;	// really points to next way point
	if (patrol)
		moveOrders.curWayDir = 1;
	else
		moveOrders.curWayDir = 0;
}
}

void MechWarrior::addMoveWayPoint (Stuff::Vector3D wayPt, bool patrol) {

	moveOrders.wayPath[moveOrders.numWayPts] = wayPt;
	moveOrders.numWayPts++;
	if (moveOrders.numWayPts == 1) {
		if (patrol)
			moveOrders.curWayDir = 1;
		else
			moveOrders.curWayDir = 0;
	}
}

void MechWarrior::setMoveGlobalPath (GlobalPathStepPtr path, long numSteps) {

	if (numSteps > MAX_GLOBAL_PATH)
		Fatal(0, " Global Path Too Long ");

    ArrayCopy(moveOrders.globalPath, path, numSteps);
	moveOrders.numGlobalSteps = numSteps;
	moveOrders.curGlobalStep = 0;
}

long MechWarrior::calcMovePath (long selectionIndex, unsigned long moveParams) {

	if (g_moveReconEnabled) g_moveRecon_calcMovePath_calls++;
	ZoneScopedN("GameLogic.Warrior.Path");

 	MoverPtr myVehicle = getVehicle();
	bool flying = (myVehicle->getMoveLevel() > 0);

	// Mod-tolerance: GlobalMap::init may have bailed (badLoad) when mod pak
	// contained oversized move-data packets (see mclib/move.cpp GlobalMap
	// bail + per-packet bounds in ae20c72). In that state, GlobalMoveMap[N]
	// exists as a partially-constructed object where areas/doors arrays are
	// NULL. Pathfinding can't run without that data — attempting it reads
	// at `0x14` (areas[startArea].open offset against NULL areas pointer).
	// Bail early with 0 steps so the mover stays put but the game keeps
	// running instead of crashing at warrior.cpp on player click.
	long moveLevel = myVehicle->getMoveLevel();
	if (!GlobalMoveMap[moveLevel] || GlobalMoveMap[moveLevel]->badLoad
			|| !GlobalMoveMap[moveLevel]->areas) {
		return 0;
	}

	Stuff::Vector3D jumpGoal;
	bool jumping = isJumping(&jumpGoal);

	Stuff::Vector3D start;
	if (jumping)
		start = jumpGoal;
	else
		start = myVehicle->getPosition();

	// If we're starting on a blocked cell, let's use our last valid
	// position as our start...
//***NOTE***:
//Does this hold up if we're on a cell that was just changed to impassable
//(e.g. on a bridge tile just blown?)?!
	if (!flying && !jumping) {
		int cellRow, cellCol;
		myVehicle->getCellPosition(cellRow, cellCol);
		if (!GameMap->getPassable(cellRow, cellCol))
			start = myVehicle->getLastValidPosition();
	}

	int posCellR, posCellC;
	land->worldToCell(start, posCellR, posCellC);
	long startArea = GlobalMoveMap[myVehicle->getMoveLevel()]->calcArea(posCellR, posCellC);

	bool escapeTile = ((moveParams & MOVEPARAM_ESCAPE_TILE) != 0);

	Stuff::Vector3D goal;
	GameObjectPtr goalObject;
	unsigned long goalType = getMoveGoal(&goal, &goalObject);
	if (goalType == MOVEGOAL_NONE) {
		LastMoveCalcErr = MOVEPATH_ERR_NO_GOAL;
		triggerAlarm(PILOT_ALARM_NO_MOVEPATH, LastMoveCalcErr);
		return(LastMoveCalcErr);
	}

	GameObjectPtr target = NULL;
	if (goalType > 0) {
		if (goalObject)
			target = goalObject;
		else
			target = ObjectManager->get(goalType);
		if (!target) {
			LastMoveCalcErr = MOVEPATH_ERR_GOALOBJ_DEAD;
			triggerAlarm(PILOT_ALARM_NO_MOVEPATH, LastMoveCalcErr);
			return(LastMoveCalcErr);
		}
	}

	long pathNum = -1;

	if (moveParams & MOVEPARAM_INIT) {
		moveOrders.pathType = MOVEPATH_UNDEFINED;
		moveOrders.numGlobalSteps = 0;
		moveOrders.originalGlobalGoal[0] = goal;
		if ((moveOrders.moveStateGoal == MOVESTATE_PIVOT_FORWARD) ||
			(moveOrders.moveStateGoal == MOVESTATE_PIVOT_REVERSE) ||
			(moveOrders.moveStateGoal == MOVESTATE_PIVOT_TARGET)) {
			moveOrders.moveState = MOVESTATE_FORWARD;
			moveOrders.moveStateGoal = MOVESTATE_FORWARD;
		}
		pathNum = 0;
	}

	bool yielding = isYielding();
  	if (moveParams & MOVEPARAM_RECALC) {
		if (target) {
			Stuff::Vector3D pos = target->getPosition();
			setMoveGoal(target->getWatchID(), &pos, target);
			}
		else
			setMoveGoal(MOVEGOAL_LOCATION, &moveOrders.originalGlobalGoal[0]);
		goalType = getMoveGoal(&goal, &goalObject);
		for (long i = 0; i < 2; i++)
			clearMovePath(i);
		moveOrders.pathType = MOVEPATH_UNDEFINED;
		moveOrders.numGlobalSteps = 0;
		moveOrders.moveState = MOVESTATE_FORWARD;
		moveOrders.moveStateGoal = MOVESTATE_FORWARD;
		pathNum = 0;
	}

	if (goal.x < -666000.0)
		return(LastMoveCalcErr = NO_ERR);

	if (pathNum == -1) {
		if (moveOrders.path[0]->numStepsWhenNotPaused == 0)
			pathNum = 0;
		else
			pathNum = 1;
	}

	__int64 startTime = 0;
	if (moveOrders.pathType == MOVEPATH_UNDEFINED/*numGlobalSteps == 0*/) {
		// Must be a new move order, so let's first calc the actual
		// move goal...
		if (!escapeTile) {
			long result = NO_ERR;
			if ((myVehicle->getCommander() == Commander::home) && (curTacOrder.code != TACTICAL_ORDER_NONE) && (curTacOrder.origin == ORDER_ORIGIN_PLAYER))
				moveParams |= MOVEPARAM_PLAYER;
			if (myVehicle->moveRadius > 0.0)
				result = myVehicle->calcMoveGoal(target, myVehicle->moveCenter, myVehicle->moveRadius, goal, selectionIndex, goal, lastGoalPathSize, lastGoalPath, moveParams);
			else
				result = myVehicle->calcMoveGoal(target, goal, -1.0, goal, selectionIndex, goal, lastGoalPathSize, lastGoalPath, moveParams);
			if (result != NO_ERR) {
				LastMoveCalcErr = MOVEPATH_ERR_NO_VALID_GOAL;
				triggerAlarm(PILOT_ALARM_NO_MOVEPATH, LastMoveCalcErr);
				return(LastMoveCalcErr);
			}
		}
		moveOrders.originalGlobalGoal[1] = goal;

		if (escapeTile) {
			Assert(pathNum == 0, pathNum, " Warrior.calcMovePath: escapePath should be pathNum 0 ");
			moveOrders.pathType = MOVEPATH_SIMPLE;
			
			myVehicle->updatePathLock(false);
			if ((curTacOrder.code == TACTICAL_ORDER_ATTACK_OBJECT) && (curTacOrder.attackParams.method == ATTACKMETHOD_RAMMING))
				RamObjectWID = curTacOrder.targetWID;
			else
				RamObjectWID = 0;
			GameObjectPtr ramObject = ObjectManager->getByWatchID(RamObjectWID);
			if (ramObject && ramObject->isMover())
				((MoverPtr)ramObject)->updatePathLock(false);
			if (myVehicle->getObjectClass() != ELEMENTAL)
				moveParams |= MOVEPARAM_AVOID_PATHLOCKS;
			Stuff::Vector3D escapeGoal;
			long numSteps = myVehicle->calcEscapePath(moveOrders.path[pathNum], start, goal, NULL, moveParams | MOVEPARAM_STATIONARY_MOVERS, escapeGoal);
			if (ramObject && ramObject->isMover())
				((MoverPtr)ramObject)->updatePathLock(true);
			myVehicle->updatePathLock(true);
			RamObjectWID = 0;

			if (numSteps > 0) {
				moveOrders.path[pathNum]->numSteps = numSteps;
				moveOrders.path[pathNum]->numStepsWhenNotPaused = numSteps;
				moveOrders.globalGoalLocation = moveOrders.path[pathNum]->stepList[numSteps - 1].destination;
				curTacOrder.setWayPoint(0, moveOrders.globalGoalLocation);
				if (target)
					moveOrders.path[pathNum]->target = target->getPosition();
				setMoveGoal((target ? target->getWatchID() : MOVEGOAL_LOCATION), &goal);
				moveOrders.nextUpdate = scenarioTime + MovementUpdateFrequency;

				if (pathNum == 0) {
					if (yielding) {
						moveOrders.yieldTime = scenarioTime + 1.5;
						moveOrders.path[pathNum]->numSteps = 0;
						}
					else if (isWaitingForPoint()) {
						moveOrders.path[pathNum]->numSteps = 0;
						}
					else {
						moveOrders.yieldTime = -1.0;
						moveOrders.yieldState = 0;
					}
				}
				LastMoveCalcErr = NO_ERR;
				}
			else {
				LastMoveCalcErr = MOVEPATH_ERR_LR_NOT_ENABLED;
				triggerAlarm(PILOT_ALARM_NO_MOVEPATH, LastMoveCalcErr);
			}
			return(LastMoveCalcErr);

		}

		bool faceTarget = (moveParams & MOVEPARAM_FACE_TARGET) != 0;
		if ((myVehicle->distanceFrom(goal) < Mover::marginOfError[1]) && !faceTarget) {
			clearMoveGoal();
			LastMoveCalcErr = MOVEPATH_ERR_ALREADY_THERE;
			triggerAlarm(PILOT_ALARM_NO_MOVEPATH, LastMoveCalcErr);
			return(LastMoveCalcErr);
		}

		// If the distance between the start and goal is within our "quickmove" range,
		// don't do any global pathfinding--do it all on one movemap. In other words,
		// make it a "simple" path. Also, if we're starting on a blocked tile (burnt
		// forest tile, for example), we'll want to do a quickmove, as well...
		int goalCellR, goalCellC;
		land->worldToCell(goal, goalCellR, goalCellC);

		bool doQuickMove = false;
		long rowDiff = goalCellR - posCellR;
		if (rowDiff < 0)
			rowDiff *= -1;
		if (rowDiff <= SimpleMovePathRange) {
			long colDiff = goalCellC - posCellC;
			if (colDiff < 0)
				colDiff *= -1;
			if (colDiff <= SimpleMovePathRange)
				doQuickMove = true;
		}

		bool startAreaOpen = (startArea >= 0) && (GlobalMoveMap[myVehicle->getMoveLevel()]->areas[startArea].open || GlobalMoveMap[myVehicle->getMoveLevel()]->areas[startArea].offMap);

		if (doQuickMove) {
			moveOrders.pathType = MOVEPATH_SIMPLE;

			myVehicle->updatePathLock(false);
			if ((curTacOrder.code == TACTICAL_ORDER_ATTACK_OBJECT) && (curTacOrder.attackParams.method == ATTACKMETHOD_RAMMING))
				RamObjectWID = curTacOrder.targetWID;
			else
				RamObjectWID = 0;
			GameObjectPtr ramObject = ObjectManager->getByWatchID(RamObjectWID);
			if (ramObject && ramObject->isMover())
				((MoverPtr)ramObject)->updatePathLock(false);
			if (myVehicle->getObjectClass() != ELEMENTAL)
				moveParams |= MOVEPARAM_AVOID_PATHLOCKS;
			long numSteps = myVehicle->calcMovePath(moveOrders.path[pathNum], MOVEPATH_SIMPLE, start, goal, NULL, moveParams | MOVEPARAM_STATIONARY_MOVERS);
			if (ramObject && ramObject->isMover())
				((MoverPtr)ramObject)->updatePathLock(true);
			myVehicle->updatePathLock(true);
			RamObjectWID = 0;

			bool foundPath = (numSteps > 0);
			if ((numSteps > 0) && (selectionIndex > 0)) {
				// We're supposed to stop early on this path...
				numSteps -= (selectionIndex / GroupMoveTrailLen[1] * GroupMoveTrailLen[0]);
				if (numSteps <= 0) {
					clearMoveOrders();
					LastMoveCalcErr = MOVEPATH_ERR_ALREADY_THERE;
					triggerAlarm(PILOT_ALARM_NO_MOVEPATH, LastMoveCalcErr);
					return(LastMoveCalcErr);
				}
			}
			if (numSteps > 0) {
				moveOrders.path[pathNum]->numSteps = numSteps;
				moveOrders.path[pathNum]->numStepsWhenNotPaused = numSteps;
				moveOrders.globalGoalLocation = moveOrders.path[pathNum]->stepList[numSteps - 1].destination;
				curTacOrder.setWayPoint(0, moveOrders.globalGoalLocation);
				if (target)
					moveOrders.path[pathNum]->target = target->getPosition();
				setMoveGoal((target ? target->getWatchID() : MOVEGOAL_LOCATION), &goal);
				moveOrders.nextUpdate  = scenarioTime + MovementUpdateFrequency;
				}
			else if (!foundPath && (startArea == -1)) {
				Assert(pathNum == 0, pathNum, " Warrior.calcMovePath: pathNum should be 0 ");
				// Starting on a non-area (i.e. bad:) tile, so try to find a SR escape
				// route to a cell that has a LR route to our desired goal (or is within
				// the same area as the goal's area)...
				TacticalOrder alarmTacOrder;
				alarmTacOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_MOVETO_POINT, false);
				alarmTacOrder.setWayPoint(0, goal);
				alarmTacOrder.moveParams.escapeTile = true;
				alarmTacOrder.moveParams.wait = false;
				alarmTacOrder.moveParams.wayPath.mode[0] = (getMoveRun() ? TRAVEL_MODE_FAST : TRAVEL_MODE_SLOW);
				setAlarmTacOrder(alarmTacOrder, 255);
				LastMoveCalcErr = MOVEPATH_ERR_ESCAPING_TILE;
				triggerAlarm(PILOT_ALARM_NO_MOVEPATH, LastMoveCalcErr);
				return(LastMoveCalcErr);
				}
			else if (!foundPath/* && longRangeEnabled*/)
				doQuickMove = false;
			else {
				LastMoveCalcErr = MOVEPATH_ERR_LR_NOT_ENABLED;
				triggerAlarm(PILOT_ALARM_NO_MOVEPATH, LastMoveCalcErr);
				return(LastMoveCalcErr);
			}
		}

		if (!doQuickMove) {
			// The path must be a multi-part, or "complex", path that travels
			// across globalmap areas/doors...
			moveOrders.globalGoalLocation = goal;
			curTacOrder.setWayPoint(0, moveOrders.globalGoalLocation);

			long goalArea = GlobalMoveMap[myVehicle->getMoveLevel()]->calcArea(goalCellR, goalCellC);

			long numSteps = -1;
			if (startAreaOpen) {
				if (useGoalPlan)
					if (myVehicle->numFunctionalWeapons > 0)
						GlobalMoveMap[myVehicle->getMoveLevel()]->useClosedAreas = true;

				GlobalMoveMap[myVehicle->getMoveLevel()]->moverTeamID = myVehicle->getTeamId();
				startTime = GetCycles();
				if (GlobalMap::logEnabled) {
					static char s[256];
					sprintf(s, "[%.2f] calcPath: [%05d]%s", scenarioTime, myVehicle->getPartId(), myVehicle->getName());
					GlobalMap::writeLog(s);
				}
				numSteps = GlobalMoveMap[myVehicle->getMoveLevel()]->calcPath(startArea,
																			  goalArea,
																			  moveOrders.globalPath,
																			  posCellR,
																			  posCellC,
																			  goalCellR,
																			  goalCellC);
				GlobalMoveMap[myVehicle->getMoveLevel()]->useClosedAreas = false;
			}
			if (numSteps == -1) {
				Assert(pathNum == 0, pathNum, " Warrior.calcMovePath: pathNum should be 0 ");
				// Starting on a non-area (i.e. bad:) tile, so try to find a SR escape
				// route to a cell that has a LR route to our desired goal (or is within
				// the same area as the goal's area)...
				TacticalOrder alarmTacOrder;
				alarmTacOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_MOVETO_POINT, false);
				alarmTacOrder.setWayPoint(0, goal);
				alarmTacOrder.moveParams.escapeTile = true;
				alarmTacOrder.moveParams.wait = false;
				alarmTacOrder.moveParams.wayPath.mode[0] = (getMoveRun() ? TRAVEL_MODE_FAST : TRAVEL_MODE_SLOW);
				setAlarmTacOrder(alarmTacOrder, 255);
				LastMoveCalcErr = MOVEPATH_ERR_ESCAPING_TILE;
				triggerAlarm(PILOT_ALARM_NO_MOVEPATH, LastMoveCalcErr);
				return(LastMoveCalcErr);
/*				//-------------------------------------------------------------------
				// Starting on a blocked tile. For now, we simply go short-range move
				// straight toward the goal...
				clearMoveOrders();
				LastMoveCalcErr = MOVEPATH_ERR_LR_START_BLOCKED;
				triggerAlarm(PILOT_ALARM_NO_MOVEPATH, LastMoveCalcErr);
				return(LastMoveCalcErr);
*/				}
			else if (numSteps == 0) {
				clearMoveOrders();
				LastMoveCalcErr = MOVEPATH_ERR_LR_NO_PATH;
				triggerAlarm(PILOT_ALARM_NO_MOVEPATH, LastMoveCalcErr);
				if (moveParams & MOVEPARAM_RADIO_RESULT) {
					clearCurTacOrder();
					radioMessage(RADIO_MOVE_BLOCKED, true);
				}
				return(LastMoveCalcErr);
			}
			moveOrders.pathType = MOVEPATH_COMPLEX;
			moveOrders.numGlobalSteps = numSteps;
			moveOrders.curGlobalStep = 0;
		}
		}
	else if (moveOrders.pathType == MOVEPATH_COMPLEX) {
		moveOrders.curGlobalStep++;
	}

	if (moveOrders.pathType == MOVEPATH_COMPLEX) {
		long curGlobalStep = moveOrders.curGlobalStep;
		long numGlobalSteps = moveOrders.numGlobalSteps;

		if (curGlobalStep == numGlobalSteps) {
			return(LastMoveCalcErr = NO_ERR);
		}

		// NEW: Now, our goal will be determined by our current global step in our global
		// path. Our moveOrders goal will contain our final goal of the entire global
		// path. Thus, our local (current) goal may not nec. be the same as our moveOrders
		// (final) goal...
		long prevThruArea = -1;
		long prevGoalDoor = -1;
		if ((curGlobalStep > 0) && (curGlobalStep < (numGlobalSteps - 1))) {
			GlobalPathStep prevGlobalStep = moveOrders.globalPath[curGlobalStep - 1];
			prevThruArea = moveOrders.globalPath[curGlobalStep - 1].thruArea;
			prevGoalDoor = moveOrders.globalPath[curGlobalStep - 1].goalDoor;
			land->cellToWorld(prevGlobalStep.goalCell[0], prevGlobalStep.goalCell[1], start);
		}

		GlobalPathStepPtr globalStep = &moveOrders.globalPath[curGlobalStep];
		GlobalMapDoorPtr globalDoor = &GlobalMoveMap[myVehicle->getMoveLevel()]->doors[globalStep->goalDoor];
		if (curGlobalStep < (numGlobalSteps - 1))
			if (!globalDoor->open) {
				LastMoveCalcErr = MOVEPATH_ERR_LR_DOOR_CLOSED;
				setMoveWayPath(NULL, 0);
				setMoveTimeOfLastStep(scenarioTime);
				setMoveGlobalPath(NULL, 0);
				PathManager->request(this, selectionIndex, MOVEPARAM_RECALC + MOVEPARAM_FACE_TARGET, 19);
				triggerAlarm(PILOT_ALARM_NO_MOVEPATH, LastMoveCalcErr);
				return(LastMoveCalcErr);
			}
		
		long numSteps = 0;
		if (curGlobalStep < (numGlobalSteps - 2)) {
			clearMovePath(pathNum);
			myVehicle->updatePathLock(false);
			if ((curTacOrder.code == TACTICAL_ORDER_ATTACK_OBJECT) && (curTacOrder.attackParams.method == ATTACKMETHOD_RAMMING))
				RamObjectWID = curTacOrder.targetWID;
			else
				RamObjectWID = 0;
			GameObjectPtr ramObject = ObjectManager->getByWatchID(RamObjectWID);
			if (ramObject && ramObject->isMover())
				((MoverPtr)ramObject)->updatePathLock(false);
			if (myVehicle->getObjectClass() != ELEMENTAL)
				moveParams |= MOVEPARAM_AVOID_PATHLOCKS;
			startTime = GetCycles();
			long thruArea[2] = {-1, -1};
			long goalDoor = -1;
			if (curGlobalStep == (numGlobalSteps - 2)) {
				// Second from last, so the actual goal cell instead of the door
				thruArea[0] = globalStep->thruArea;
				goalDoor = globalStep->goalDoor;
			}
			else {
				thruArea[0] = globalStep->thruArea;
				thruArea[1] = moveOrders.globalPath[curGlobalStep+1].thruArea;
				goalDoor = moveOrders.globalPath[curGlobalStep+1].goalDoor;
			}
			numSteps = myVehicle->calcMovePath(
                    moveOrders.path[pathNum], start, thruArea, goalDoor, moveOrders.globalGoalLocation, &goal, globalStep->goalCell, moveParams | MOVEPARAM_STATIONARY_MOVERS);
			if (ramObject && ramObject->isMover())
				((MoverPtr)ramObject)->updatePathLock(true);
			myVehicle->updatePathLock(true);
			RamObjectWID = 0;
			}
		else if (curGlobalStep == (numGlobalSteps - 2)) {
			clearMovePath(pathNum);
			goal = moveOrders.originalGlobalGoal[1];
			myVehicle->updatePathLock(false);
			if ((curTacOrder.code == TACTICAL_ORDER_ATTACK_OBJECT) && (curTacOrder.attackParams.method == ATTACKMETHOD_RAMMING))
				RamObjectWID = curTacOrder.targetWID;
			else
				RamObjectWID = 0;
			GameObjectPtr ramObject = ObjectManager->getByWatchID(RamObjectWID);
			if (ramObject && ramObject->isMover())
				((MoverPtr)ramObject)->updatePathLock(false);
			if (myVehicle->getObjectClass() != ELEMENTAL)
				moveParams |= MOVEPARAM_AVOID_PATHLOCKS;
			numSteps = myVehicle->calcMovePath(moveOrders.path[pathNum], MOVEPATH_COMPLEX, start, goal, globalStep->goalCell, moveParams | MOVEPARAM_STATIONARY_MOVERS);
			if (ramObject && ramObject->isMover())
				((MoverPtr)ramObject)->updatePathLock(true);
			myVehicle->updatePathLock(true);
			RamObjectWID = 0;
			if (numSteps > 0) {
				if (selectionIndex > 0) {
					// We're supposed to stop early on this path...
					numSteps -= (selectionIndex / GroupMoveTrailLen[1] * GroupMoveTrailLen[0]);
					if (numSteps <= 0) {
						clearMoveOrders();
						LastMoveCalcErr = MOVEPATH_ERR_ALREADY_THERE;
						triggerAlarm(PILOT_ALARM_NO_MOVEPATH, LastMoveCalcErr);
						return(LastMoveCalcErr);
					}
					if (pathNum == 0) {
						moveOrders.globalGoalLocation = moveOrders.path[pathNum]->stepList[numSteps - 1].destination;
						curTacOrder.setWayPoint(0, moveOrders.globalGoalLocation);
					}
				}
				curGlobalStep++;
			}
		}
		else if (curGlobalStep != (numGlobalSteps - 1))
			STOP((" Mechwarrior.calcMovePath: bad global step (%d/%d)", curGlobalStep, numGlobalSteps));

		if (numSteps > 0) {
			moveOrders.path[pathNum]->numSteps = numSteps;
			moveOrders.path[pathNum]->numStepsWhenNotPaused = numSteps;
			moveOrders.path[pathNum]->globalStep = curGlobalStep;
			if (pathNum == 0) {
				if (target)
					moveOrders.path[pathNum]->target = target->getPosition();
				setMoveGoal((target ? target->getWatchID() : MOVEGOAL_LOCATION), &goal);
			}
			}
		else if (curGlobalStep != (numGlobalSteps - 1)) {
			moveOrders.curGlobalStep--;
			if (numSteps == -999) {
				// Moving to a blocked door (all possible goal cells for the door are currently blocked).
				LastMoveCalcErr = MOVEPATH_ERR_LR_DOOR_BLOCKED;
				}
			else
				LastMoveCalcErr = MOVEPATH_ERR_LR_NO_SR_PATH;
			triggerAlarm(PILOT_ALARM_NO_MOVEPATH, LastMoveCalcErr);
			return(LastMoveCalcErr);
		}
		}
	else if ((moveOrders.pathType != MOVEPATH_SIMPLE) && (moveOrders.pathType != MOVEPATH_UNDEFINED))
		Fatal(0, " Bad Move Path Type ");

	if (pathNum == 0) {
		if (yielding) {
			moveOrders.yieldTime = scenarioTime + 1.5;
			moveOrders.path[pathNum]->numSteps = 0;
			}
		else if (isWaitingForPoint()) {
			moveOrders.path[pathNum]->numSteps = 0;
			}
		else {
			moveOrders.yieldTime = -1.0;
			moveOrders.yieldState = 0;
		}
	}

	return(LastMoveCalcErr = NO_ERR);
}

bool MechWarrior::getNextWayPoint (Stuff::Vector3D& nextPoint, bool incWayPoint) {

	if (!curTacOrder.isWayPathOrder())
		return(false);

	// if curWayDir == 0, then the path is one-time only (loop through it once).
	// if curWayDir == 1, then loop through it forward. Once end is hit, loop back...
	// if curWayDir == -1, then loop through it backward. Once start is hit, loop forward...
	if (moveOrders.curWayDir == 0) {
		long nextWayPt = moveOrders.curWayPt + 1;
		if (nextWayPt < moveOrders.numWayPts)
			nextPoint = moveOrders.wayPath[nextWayPt];
		if (incWayPoint) {
			moveOrders.curWayPt = nextWayPt;
			if (curTacOrder.moveParams.wayPath.mode[nextWayPt] == TRAVEL_MODE_FAST)
				setMoveRun(true);
			else
				setMoveRun(false);
			curTacOrder.moveParams.wayPath.curPoint = nextWayPt;
			if (nextWayPt == moveOrders.numWayPts)
				curTacOrder.setStage(2);
		}
		if (nextWayPt == moveOrders.numWayPts)
			return(false);
		}
	else {
		long nextWayPt = 0;
		long nextWayDir = moveOrders.curWayDir;
		if (moveOrders.curWayDir == 1) {
			if (moveOrders.curWayPt == (moveOrders.numWayPts - 1)) {
				nextWayDir = -1;
				nextWayPt = moveOrders.numWayPts - 2;
				}
			else
				nextWayPt = moveOrders.curWayPt + 1;
			}
		else {
			if (moveOrders.curWayPt == 0) {
				nextWayDir = 1;
				nextWayPt = 1;
				}
			else
				nextWayPt = moveOrders.curWayPt - 1;
		}
		nextPoint = moveOrders.wayPath[nextWayPt];
		if (incWayPoint) {
			moveOrders.curWayDir = nextWayDir;
			moveOrders.curWayPt = nextWayPt;
			if (curTacOrder.moveParams.wayPath.mode[nextWayPt] == TRAVEL_MODE_FAST)
				setMoveRun(true);
			else
				setMoveRun(false);
			curTacOrder.moveParams.wayPath.curPoint = nextWayPt;
		}
	}
	return(true);
}


Stuff::Vector3D MechWarrior::calcWithdrawGoal (float withdrawRange) {

	Stuff::Vector3D escapeVector;

	return(getVehicle()->getPosition());
}

bool MechWarrior::movingOverBlownBridge (void) {

	if (getMovePath()) {
#ifdef USE_BRIDGES
		MoverPtr myVehicle = getVehicle();
		long tileRow = 0, tileCol = 0, cellRow = 0, cellCol = 0;
		myVehicle->getTileCellPosition(tileRow, tileCol, cellRow, cellCol);
		long overlay = GameMap->getOverlayType(tileRow, tileCol);
		if (OverlayIsBridge[overlay]) {
			long bridgeArea = GlobalMoveMap->calcArea(tileRow, tileCol);
			if (GlobalMoveMap->isClosedArea(bridgeArea))
				return(true);
		}
		long bridgeArea = getMovePath()->crossesBridge(-1, 3);
		if (bridgeArea > -1)
			if (GlobalMoveMap->isClosedArea(bridgeArea))
				return(true);
		if (moveOrders.pathType == MOVEPATH_COMPLEX) {
			long curGlobalStep = moveOrders.curGlobalStep;
			long numGlobalSteps = moveOrders.numGlobalSteps;
			for (long i = curGlobalStep; i < numGlobalSteps; i++) {
				long thruArea = moveOrders.globalPath[i].thruArea;
				if (GlobalMoveMap->isBridgeArea(thruArea)) {
					if (GlobalMoveMap->isClosedArea(thruArea))
						return(true);
					}
				else
					return(false);
			}
		}
#endif
	}
	return(false);
}

bool MechWarrior::movementDecisionTree (void) {

	if (moveOrders.timeOfLastStep > -1.0) {
		if (moveOrders.timeOfLastStep < (scenarioTime - MoveTimeOut)) {
			clearMoveOrders();
			if (curTacOrder.isMoveOrder() || curTacOrder.isWayPathOrder()) {
				if (curTacOrder.time < (scenarioTime - MoveTimeOut)) {
					radioMessage(RADIO_MOVE_BLOCKED, true);
					clearCurTacOrder();
				}
			}
			triggerAlarm(PILOT_ALARM_NO_MOVEPATH, MOVEPATH_ERR_TIME_OUT);
		}
	}

	MoverPtr myVehicle = getVehicle();

	if (isYielding()) {
		if (getMoveYieldTime() < scenarioTime) {
			setMoveYieldTime(scenarioTime + MoveYieldTime);
			// For now, we'll use random chance to decide if we should recalc or not.
			// DEFINITELY use a smarter priority scheme before this thing ships :)
			bool onBlownBridge = false;
			MoverPtr myVehicle = getVehicle();
			int cellRow = 0, cellCol = 0;
			myVehicle->getCellPosition(cellRow, cellCol);
			#ifdef USE_OVERLAYS
			long overlay = GameMap->getOverlay(cellRow, cellCol);
			if (OverlayIsBridge[overlay]) {
				long bridgeArea = GlobalMoveMap->calcArea(tileRow, tileCol);
				if (GlobalMoveMap->isClosedArea(bridgeArea))
					onBlownBridge = true;
			}
			#endif
			bool reroute = onBlownBridge || (RandomNumber(100) < 75);
			if (getMovePath()) {
				long bridgeArea = getMovePath()->crossesBridge(-1, 3);
				if (bridgeArea > -1)
					reroute = reroute || movingOverBlownBridge();
			}
			if (reroute) {
				unsigned long params = MOVEPARAM_RECALC | MOVEPARAM_FACE_TARGET;
				if (curTacOrder.moveParams.jump)
					params |= MOVEPARAM_JUMP;
				requestMovePath(curTacOrder.selectionIndex, params, 4);
			}
		}
	}

	if (getMovePathType() == MOVEPATH_COMPLEX) {
		if (getMoveCurGlobalStep() < (getMoveNumGlobalSteps() - 1)) {
			if (moveOrders.path[1]->numStepsWhenNotPaused == 0) {
				// Currently, we don't have any paths queued up...
				if (!movePathRequest) {
					// And, we don't already have a request to fill the queue for this
					// complex path, so let's do it...
					TacticalOrderPtr curTacOrder = getCurTacOrder();
					unsigned long params = MOVEPARAM_FACE_TARGET;
					if (curTacOrder->moveParams.jump)
						params |= MOVEPARAM_JUMP;
					requestMovePath(curTacOrder->selectionIndex, params, 5);
				}
			}
		}
	}

	// Everything up to this point is done EVERY frame update. Now, see
	// if it's time to do the ugly stuff...
	if (movementUpdate > scenarioTime)
		return(true);

	movementUpdate = scenarioTime + MovementUpdateFrequency;


	if ((curTacOrder.code == TACTICAL_ORDER_NONE) || (curTacOrder.code == TACTICAL_ORDER_STOP)) {
		GameObjectPtr target = getLastTarget();
		if (target)
			setMoveStateGoal(MOVESTATE_PIVOT_TARGET);
		return(true);
	}

	if (getVehicle()->attackRange == FIRERANGE_CURRENT)
		if (!curTacOrder.isMoveOrder() && (curTacOrder.code != TACTICAL_ORDER_CAPTURE)) {
			GameObjectPtr target = getLastTarget();
			if (target)
				setMoveStateGoal(MOVESTATE_PIVOT_TARGET);
			return(true);
		}

	GameObjectPtr goalObject = NULL;
	unsigned long goalType = getMoveGoal(NULL, &goalObject);

	if (goalType == MOVEGOAL_NONE) {
		if (curTacOrder.code == TACTICAL_ORDER_WITHDRAW) {
			Stuff::Vector3D escapeGoal = calcWithdrawGoal();
			setMoveGoal(MOVEGOAL_LOCATION, &escapeGoal);
			unsigned long params = MOVEPARAM_INIT | MOVEPARAM_FACE_TARGET;
			if (curTacOrder.moveParams.jump)
				params |= MOVEPARAM_JUMP;
			requestMovePath(curTacOrder.selectionIndex, params, 6);
			}
		else if (curTacOrder.code == TACTICAL_ORDER_MOVETO_POINT) {
			long msg;
			curTacOrder.execute(this, msg);
			}
		else if ((curTacOrder.code == TACTICAL_ORDER_MOVETO_OBJECT) || (curTacOrder.code == TACTICAL_ORDER_CAPTURE)) {
			GameObjectPtr moveTarget = curTacOrder.getTarget();
			Stuff::Vector3D targetPos = moveTarget->getPosition();
			if (moveTarget->isMover())
				targetPos = moveTarget->getPosition();
			else {
				// If not a mover, then pick an open cell adjacent
				// to it. If it happens to be adjacent to blocked
				// cells, this could be off...
				if (moveTarget->isBuilding()) {
					BuildingPtr building = (BuildingPtr)moveTarget;
					long goalRow = 0, goalCol = 0;
					bool foundGoal = building->calcAdjacentAreaCell(getVehicle()->moveLevel, -1, goalRow, goalCol);
					if (foundGoal)
						land->cellToWorld(goalRow, goalCol, targetPos);
					else {
						Stuff::Vector3D objectPos = moveTarget->getPosition();
						targetPos = getVehicle()->calcOffsetMoveGoal(objectPos);
					}
					}
				else {
					Stuff::Vector3D objectPos = moveTarget->getPosition();
					targetPos = getVehicle()->calcOffsetMoveGoal(objectPos);
				}
			}
			setMoveGoal(moveTarget->getWatchID(), &targetPos, moveTarget);
			unsigned long params = MOVEPARAM_INIT | MOVEPARAM_FACE_TARGET;
			if (curTacOrder.moveParams.jump)
				params |= MOVEPARAM_JUMP;
			if (!moveTarget->isMover())
				requestMovePath(curTacOrder.selectionIndex, params | MOVEPARAM_STEP_ADJACENT_TARGET, 7);
			else
				requestMovePath(curTacOrder.selectionIndex, params, 8);
			}
		else if (curTacOrder.isCombatOrder()) {
			Stuff::Vector3D targetPosition;
			GameObjectPtr target = getCurrentTarget();
			if (target)
				targetPosition = target->getPosition();
			else if (curTacOrder.code == TACTICAL_ORDER_ATTACK_POINT)
				targetPosition = getAttackTargetPoint();
			else
				return(true);
			if (curTacOrder.attackParams.method == ATTACKMETHOD_RAMMING) {
				Stuff::Vector3D pos = target->getPosition();
				setMoveGoal(target->getWatchID(), &pos, target);
				unsigned long params = MOVEPARAM_INIT | MOVEPARAM_FACE_TARGET;
				if (curTacOrder.moveParams.jump)
					params |= MOVEPARAM_JUMP;
				requestMovePath(curTacOrder.selectionIndex, params, 9);
				}
			else if (/*!getAttackPursuit() && */!curTacOrder.attackParams.pursue)
				setMoveStateGoal(MOVESTATE_PIVOT_TARGET);
			else {
				bool recalcPath = false;
				unsigned long moveParams = MOVEPARAM_NONE;

				// We will want to re-evaluate optimal range if our optimal range
				// has changed...
				if (target && target->isMover())
					if (((MoverPtr)target)->getLastWeaponEffectivenessCalc() > myVehicle->getLastOptimalRangeCalc())
						if (myVehicle->calcFireRanges()) {
							recalcPath = true;
							moveParams = MOVEPARAM_SOMEWHERE_ELSE;
						}

				float distanceToTarget = myVehicle->distanceFrom(targetPosition);
				float desiredAttackRange = myVehicle->getOrderedFireRange();
				float attackRangeDelta = distanceToTarget - desiredAttackRange;
				float recalcDelta = Terrain::worldUnitsPerVertex * metersPerWorldUnit;
				if ((curTacOrder.attackParams.tactic != TACTIC_STOP_AND_FIRE) && (curTacOrder.attackParams.tactic != TACTIC_TURRET) && ((attackRangeDelta > recalcDelta) || (attackRangeDelta < -recalcDelta))) {
					recalcPath = true;
					moveParams = MOVEPARAM_SOMEWHERE_ELSE;
					}
				else if ((curTacOrder.attackParams.tactic != TACTIC_TURRET) && (weaponsStatusResult == WEAPONS_STATUS_OUT_OF_RANGE)) {
					recalcPath = true;
					moveParams = MOVEPARAM_SOMEWHERE_ELSE;
					}
				else if (weaponsStatusResult >= 0) {
					long numWeaponsNotReady = 0;
					long numWeaponsOutOfAmmo = 0;
					long numWeaponsOutOfRange = 0;
					long numWeaponsNotLocked = 0;
					long numWeaponsNoChance = 0;
					long numWeaponsTooHot = 0;
					for (long i = 0; i < myVehicle->numWeapons; i++) {
						if (weaponsStatus[i] == WEAPON_STATUS_NOT_READY)
							numWeaponsNotReady++;
						if (weaponsStatus[i] == WEAPON_STATUS_OUT_OF_AMMO)
							numWeaponsOutOfAmmo++;
						if (weaponsStatus[i] == WEAPON_STATUS_OUT_OF_RANGE)
							numWeaponsOutOfRange++;
						if (weaponsStatus[i] == WEAPON_STATUS_NOT_LOCKED)
							numWeaponsNotLocked++;
						if (weaponsStatus[i] == WEAPON_STATUS_NO_CHANCE)
							numWeaponsNoChance++;
						if (weaponsStatus[i] == WEAPON_STATUS_TOO_HOT)
							numWeaponsTooHot++;
					}
					switch (myVehicle->getObjectClass()) {
						case BATTLEMECH:
							if (weaponsStatusResult == 0) {
								if ((target && !myVehicle->lineOfSight(target)) ||
									(!target && !myVehicle->lineOfSight(targetPosition))) {
										recalcPath = true;
										moveParams = MOVEPARAM_SOMEWHERE_ELSE;
									}
								else if ((numWeaponsNotReady > 0) || (numWeaponsNotLocked > 0) || (numWeaponsTooHot > 0)) {
									if ((getMovePath()->numSteps == 0) && !getMoveTwisting())
										setMoveStateGoal(MOVESTATE_PIVOT_TARGET);
									}
								else {
									recalcPath = true;
									moveParams = MOVEPARAM_SOMEWHERE_ELSE;
								}
							}
							break;
						case GROUNDVEHICLE:
							if (weaponsStatusResult == 0) {
								if ((target && !myVehicle->lineOfSight(target)) ||
									(!target && !myVehicle->lineOfSight(target))) {
										recalcPath = true;
										moveParams = MOVEPARAM_SOMEWHERE_ELSE;
									}
								else if (numWeaponsNotReady == 0) {
									if ((numWeaponsNotLocked > 0) || (numWeaponsTooHot > 0)) {
										if ((getMovePath()->numSteps == 0) && !getMoveTwisting())
											setMoveStateGoal(MOVESTATE_PIVOT_TARGET);
										}
									else {
										recalcPath = true;
										moveParams = MOVEPARAM_SOMEWHERE_ELSE;
									}
								}
                            }
							break;
						case ELEMENTAL:
							if (weaponsStatusResult == 0) {
								if (numWeaponsNotReady == 0) {
									if ((numWeaponsNotLocked > 0) || (numWeaponsTooHot > 0)) {
										if ((getMovePath()->numSteps == 0) && !getMoveTwisting())
											setMoveStateGoal(MOVESTATE_PIVOT_TARGET);
										}
									else {
										recalcPath = true;
										moveParams = MOVEPARAM_SOMEWHERE_ELSE;
									}
								}
                            }
							break;
					}
				}
				if (!recalcPath && (curTacOrder.attackParams.tactic != TACTIC_STOP_AND_FIRE))
					if (keepMoving && curTacOrder.moveParams.keepMoving)
						if (RandomNumber(100) < 60) {
							recalcPath = true;
							moveParams = MOVEPARAM_SOMEWHERE_ELSE + MOVEPARAM_RANDOM_OPTIMAL;
						}
				if (recalcPath && (curTacOrder.attackParams.tactic != TACTIC_TURRET)) {
					if (target) {
						setMoveGoal(target->getWatchID(), &targetPosition, target);
						if (curTacOrder.moveParams.jump)
							moveParams |= MOVEPARAM_JUMP;
						requestMovePath(curTacOrder.selectionIndex, moveParams | MOVEPARAM_INIT | MOVEPARAM_FACE_TARGET, 10);
						}
					else {
						setMoveGoal(MOVEGOAL_LOCATION, &targetPosition);
						unsigned long params = MOVEPARAM_INIT | MOVEPARAM_FACE_TARGET;
						if (curTacOrder.moveParams.jump)
							params |= MOVEPARAM_JUMP;
						requestMovePath(curTacOrder.selectionIndex, params, 11);
					}
				}
			}
			}
		else {
			GameObjectPtr target = getLastTarget();
			if (target)
				setMoveStateGoal(MOVESTATE_PIVOT_TARGET);
		}
		}
	else if (goalType == MOVEGOAL_LOCATION) {
		if (!isYielding() && !isJumping() && !getMoveTwisting() && !isWaitingForPoint() && (getMovePath()->numSteps == 0)) {
			// We may be so close that we don't need a move
			// path...
			if (!getMovePathRequest()) {
				unsigned long params = MOVEPARAM_INIT | MOVEPARAM_FACE_TARGET;
				if (curTacOrder.moveParams.jump)
					params |= MOVEPARAM_JUMP;
				requestMovePath(curTacOrder.selectionIndex, params, 12);
			}
		}
		}
	else {
		if (goalObject) {
			// We may want to call a modified orderMoveToObject routine (but,
			// we don't want to play with the tacOrders)...
			Stuff::Vector3D targetPos = goalObject->getPosition();
			if (!curTacOrder.isCombatOrder()) {
				if (distance_from(targetPos, moveOrders.goalObjectPosition) > 50.0) {
					Stuff::Vector3D pos = goalObject->getPosition();
					setMoveGoal(goalObject->getWatchID(), &pos, goalObject);
					unsigned long params = MOVEPARAM_INIT | MOVEPARAM_FACE_TARGET;
					if (curTacOrder.moveParams.jump)
						params |= MOVEPARAM_JUMP;
					requestMovePath(curTacOrder.selectionIndex, params, 13);
					}
				else if (getMovePath()->numStepsWhenNotPaused == 0) {
					Stuff::Vector3D pos = goalObject->getPosition();
					setMoveGoal(goalObject->getWatchID(), &pos, goalObject);
					unsigned long params = MOVEPARAM_INIT | MOVEPARAM_FACE_TARGET;
					if (curTacOrder.moveParams.jump)
						params |= MOVEPARAM_JUMP;
					requestMovePath(curTacOrder.selectionIndex, params, 13);
				}
				}
			else {
				GameObjectPtr target = getCurrentTarget();
				if (target && (target == goalObject) && getAttackPursuit()) {
					bool recalcPath = false;
					unsigned long moveParams = MOVEPARAM_NONE;

					// We will want to re-evaluate optimal range if our optimal range
					// has changed...
					if (target->isMover()) {
						if (((MoverPtr)target)->getLastWeaponEffectivenessCalc() > myVehicle->getLastOptimalRangeCalc())
							if (myVehicle->calcFireRanges()) {
								recalcPath = true;
								moveParams = MOVEPARAM_SOMEWHERE_ELSE;
							}
						}
					else {
						if (getMovePath()->numSteps > 0)
							return(true);
					}

					float distanceToTarget = myVehicle->distanceFrom(target->getPosition());
					float desiredAttackRange = myVehicle->getOrderedFireRange();
					float attackRangeDelta = distanceToTarget - desiredAttackRange;
					float recalcDelta = Terrain::worldUnitsPerVertex * metersPerWorldUnit;
					if ((curTacOrder.attackParams.tactic != TACTIC_STOP_AND_FIRE) && (curTacOrder.attackParams.tactic != TACTIC_TURRET) && ((attackRangeDelta > recalcDelta) || (attackRangeDelta < -recalcDelta))) {
						recalcPath = true;
						moveParams = MOVEPARAM_SOMEWHERE_ELSE;
						}
					else if ((curTacOrder.attackParams.tactic != TACTIC_TURRET) && (weaponsStatusResult == WEAPONS_STATUS_OUT_OF_RANGE)) {
						recalcPath = true;
						moveParams = MOVEPARAM_SOMEWHERE_ELSE;
						}
					else if (weaponsStatusResult >= 0) {
						long numWeaponsNotReady = 0;
						long numWeaponsOutOfRange = 0;
						long numWeaponsOutOfAmmo = 0;
						long numWeaponsNotLocked = 0;
						long numWeaponsNoChance = 0;
						long numWeaponsTooHot = 0;
						for (long i = 0; i < myVehicle->numWeapons; i++) {
							if (weaponsStatus[i] == WEAPON_STATUS_NOT_READY)
								numWeaponsNotReady++;
							if (weaponsStatus[i] == WEAPON_STATUS_OUT_OF_AMMO)
								numWeaponsOutOfAmmo++;
							if (weaponsStatus[i] == WEAPON_STATUS_OUT_OF_RANGE)
								numWeaponsOutOfRange++;
							if (weaponsStatus[i] == WEAPON_STATUS_NOT_LOCKED)
								numWeaponsNotLocked++;
							if (weaponsStatus[i] == WEAPON_STATUS_NO_CHANCE)
								numWeaponsNoChance++;
							if (weaponsStatus[i] == WEAPON_STATUS_TOO_HOT)
								numWeaponsTooHot++;
						}
						switch (myVehicle->getObjectClass()) {
							case BATTLEMECH:
								if (weaponsStatusResult == 0) {
									if (!myVehicle->lineOfSight(target)) {
										recalcPath = true;
										moveParams = MOVEPARAM_SOMEWHERE_ELSE;
										}
									else if ((numWeaponsNotReady > 0) || (numWeaponsTooHot > 0) || (numWeaponsNotLocked > 0)) {
										// If we already have a path, don't pivot, finish the move...
										if ((getMovePath()->numSteps == 0) && !getMoveTwisting())
											setMoveStateGoal(MOVESTATE_PIVOT_TARGET);
										}
									else {
										recalcPath = true;
										moveParams = MOVEPARAM_SOMEWHERE_ELSE;
									}
								}
								break;
							case GROUNDVEHICLE:
								if (weaponsStatusResult == 0) {
									if (!myVehicle->lineOfSight(target)) {
										recalcPath = true;
										moveParams = MOVEPARAM_SOMEWHERE_ELSE;
										}
									else if (numWeaponsNotReady == 0) {
										if ((numWeaponsNotLocked > 0) || (numWeaponsTooHot > 0)) {
											if ((getMovePath()->numSteps == 0) && !getMoveTwisting())
												setMoveStateGoal(MOVESTATE_PIVOT_TARGET);
											}
										else {
											recalcPath = true;
											moveParams = MOVEPARAM_STEP_TOWARD_TARGET;
										}
									}
                                }
								break;
							case ELEMENTAL:
								if (weaponsStatusResult == 0) {
									if (numWeaponsNotReady == 0) {
										if ((numWeaponsNotLocked > 0) || (numWeaponsTooHot > 0)) {
											if ((getMovePath()->numSteps == 0) && !getMoveTwisting())
												setMoveStateGoal(MOVESTATE_PIVOT_TARGET);
											}
										else {
											recalcPath = true;
											moveParams = MOVEPARAM_SOMEWHERE_ELSE;
										}
									}
                                }
								break;
						}
					}
					if (!recalcPath && (curTacOrder.attackParams.tactic != TACTIC_STOP_AND_FIRE))
						if (keepMoving && curTacOrder.moveParams.keepMoving)
							if (RandomNumber(100) < 60) {
								recalcPath = true;
								moveParams = MOVEPARAM_SOMEWHERE_ELSE + MOVEPARAM_RANDOM_OPTIMAL;
							}
					if (recalcPath && (curTacOrder.attackParams.tactic != TACTIC_TURRET)) {
						Stuff::Vector3D pos = target->getPosition();
						setMoveGoal(target->getWatchID(), &pos, target);
						if (curTacOrder.moveParams.jump)
							moveParams |= MOVEPARAM_JUMP;
						requestMovePath(curTacOrder.selectionIndex, moveParams | MOVEPARAM_INIT | MOVEPARAM_FACE_TARGET, 14);
					}
				}
			}
		}
	}

	return(true);
}
