// MechWarrior: debug orders, brain-state accessors and the core* orders.
// Declared in the class headers; shared names in warrior_internal.h.
#include "warrior_internal.h"


void MechWarrior::setDebugFlag (unsigned long flag, bool on) {

	if (on)
		debugFlags |= flag;
	else
		debugFlags &= (flag ^ 0xFFFFFFFF);
}

bool MechWarrior::getDebugFlag (unsigned long flag) {

	return((debugFlags & flag) != 0);
}

void MechWarrior::debugPrint (char* s, bool debugMode) {

	if (debugger) {
		debugger->print(s);
		if (debugMode)
			debugger->debugMode();
		}
}

void MechWarrior::debugOrders (void) {

	char s[256];

	switch (curTacOrder.code) {
		case TACTICAL_ORDER_NONE:
			sprintf(s, "CURRENT ORDERS: None");
			break;
		case TACTICAL_ORDER_WAIT:
			sprintf(s, "CURRENT ORDERS: Wait");
			break;
		case TACTICAL_ORDER_MOVETO_POINT:
			sprintf(s, "CURRENT ORDERS: Move to (%.2f, %.2f, %.2f)", curTacOrder.getWayPoint(0).x, curTacOrder.getWayPoint(0).y, curTacOrder.getWayPoint(0).z);
			break;
		case TACTICAL_ORDER_MOVETO_OBJECT: {
			GameObjectPtr obj = ObjectManager->getByWatchID(curTacOrder.targetWID);
			sprintf(s, "CURRENT ORDERS: Move To Object %d", obj ? obj->getPartId() : 0);
			}
			break;
		case TACTICAL_ORDER_JUMPTO_POINT:
			sprintf(s, "CURRENT ORDERS: Jump to (%.2f, %.2f, %.2f)", curTacOrder.getWayPoint(0).x, curTacOrder.getWayPoint(0).y, curTacOrder.getWayPoint(0).z);
			break;
		case TACTICAL_ORDER_JUMPTO_OBJECT: {
			GameObjectPtr obj = ObjectManager->getByWatchID(curTacOrder.targetWID);
			sprintf(s, "CURRENT ORDERS: Jump To Object %d", obj ? obj->getPartId() : 0);
			}
			break;
		case TACTICAL_ORDER_TRAVERSE_PATH:
			sprintf(s, "CURRENT ORDERS: Traverse Path");
			break;
		case TACTICAL_ORDER_PATROL_PATH:
			sprintf(s, "CURRENT ORDERS: Patrol Path");
			break;
		case TACTICAL_ORDER_ESCORT:
			sprintf(s, "CURRENT ORDERS: Escort");
			break;
		case TACTICAL_ORDER_FOLLOW:
			sprintf(s, "CURRENT ORDERS: Follow");
			break;
		case TACTICAL_ORDER_GUARD:
			sprintf(s, "CURRENT ORDERS: Guard");
			break;
		case TACTICAL_ORDER_STOP:
			sprintf(s, "CURRENT ORDERS: Stop");
			break;
		case TACTICAL_ORDER_POWERUP:
			sprintf(s, "CURRENT ORDERS: Power Up");
			break;
		case TACTICAL_ORDER_POWERDOWN:
			sprintf(s, "CURRENT ORDERS: Power Down");
			break;
		case TACTICAL_ORDER_WAYPOINTS_DONE:
			sprintf(s, "CURRENT ORDERS: Waypoints Done");
			break;
		case TACTICAL_ORDER_EJECT:
			sprintf(s, "CURRENT ORDERS: Eject");
			break;
		case TACTICAL_ORDER_ATTACK_OBJECT: {
			GameObjectPtr obj = ObjectManager->getByWatchID(curTacOrder.targetWID);
			sprintf(s, "CURRENT ORDERS: Attack Object %d", obj ? obj->getPartId() : 0);
			}
			break;
		case TACTICAL_ORDER_HOLD_FIRE:
			sprintf(s, "CURRENT ORDERS: Hold Fire");
			break;
		case TACTICAL_ORDER_WITHDRAW:
			sprintf(s, "CURRENT ORDERS: Withdraw");
			break;
		default:
			sprintf(s, "CURRENT ORDERS: Unknown Tac Order Type");
	}
	debugPrint(s);

	GameObjectPtr target = getCurrentTarget();
	sprintf(s, "     CURRENT TARGET: Object %d", target ? target->getPartId() : 0);
	debugPrint(s);
}

void MechWarrior::setMoveSpeedType (long type) {

	moveOrders.speedType = type;
}

void MechWarrior::setMoveSpeedVelocity (float speed) {

	moveOrders.speedVelocity = speed;
	MoverPtr myVehicle = getVehicle();

	long state, throttle;

	myVehicle->calcSpriteSpeed(speed, 0, state, throttle);
	
	moveOrders.speedState = state;
	moveOrders.speedThrottle = throttle;
}

void MechWarrior::initTargetPriorities (void) {

	targetPriorityList.init();
}

long MechWarrior::setTargetPriority (long index, long type, long param1, long param2, long param3) {

	targetPriorityList.list[index].type = type;
	targetPriorityList.list[index].params[0] = param1;
	targetPriorityList.list[index].params[1] = param2;
	targetPriorityList.list[index].params[2] = param3;
	if (index >= targetPriorityList.size)
		targetPriorityList.size = index + 1;
	return(0);
}

long MechWarrior::insertTargetPriority (long index, long type, long param1, long param2, long param3) {

	TargetPriority priority;
	priority.type = type;
	priority.params[0] = param1;
	priority.params[1] = param2;
	priority.params[2] = param3;
	return(targetPriorityList.insert(index, &priority));
}

long MechWarrior::setBrainState (long newState) {

	long oldBrainState = brainState;
	brainState = newState;
	return(oldBrainState);
}

long MechWarrior::getBrainState (void) {

	return(brainState);
}

// CORE COMMANDS (MC2 BEHAVIORS)

long MechWarrior::coreMoveTo (Stuff::Vector3D location, unsigned long params) {

	bool run = ((params & TACORDER_PARAM_RUN) != 0);
	bool jump = ((params & TACORDER_PARAM_JUMP) != 0);
	bool wait = ((params & TACORDER_PARAM_WAIT) != 0);
	bool layMines = ((params & TACORDER_PARAM_LAY_MINES) != 0);
	bool escape = ((params & TACORDER_PARAM_ESCAPE_TILE) != 0);
	bool setOrder = ((params & TACORDER_PARAM_DONT_SET_ORDER) == 0);
	bool keepMoving = ((params & TACORDER_PARAM_DONT_KEEP_MOVING) == 0);

	TacticalOrder tacOrder;
	tacOrder.init(ORDER_ORIGIN_COMMANDER, TACTICAL_ORDER_MOVETO_POINT, false);
	tacOrder.moveParams.wayPath.points[0] = location.x;
	tacOrder.moveParams.wayPath.points[1] = location.y;
	tacOrder.moveParams.wayPath.points[2] = location.z;
	tacOrder.selectionIndex = -1;
	tacOrder.moveParams.wayPath.mode[0] = (run ? TRAVEL_MODE_FAST : TRAVEL_MODE_SLOW);
	tacOrder.moveParams.wait = wait;
	tacOrder.moveParams.mode = (layMines ? MOVE_MODE_MINELAYING : MOVE_MODE_NORMAL);
	tacOrder.moveParams.escapeTile = escape;
	tacOrder.moveParams.jump = jump;
	tacOrder.moveParams.keepMoving = keepMoving;

	long result = tacOrder.status(this);
	if (result == TACORDER_SUCCESS)
		return(1);

	if (curTacOrder.equals(&tacOrder))
		return(0);

	if (setOrder)
		clearCurTacOrder();

	setMoveWayPath(NULL, 0);
	setMoveGoal(MOVEGOAL_LOCATION, &location);
	moveOrders.timeOfLastStep = scenarioTime;
	setMoveRun(run);
	if (setOrder)
		clearAttackOrders();

	unsigned long moveParams = MOVEPARAM_INIT | MOVEPARAM_FACE_TARGET;
	if (jump)
		moveParams |= MOVEPARAM_JUMP;
	PathManager->request(this, -1, moveParams, 20);
	if (setOrder) {
		setGeneralTacOrder(tacOrder);
		if (useGoalPlan)
			setMainGoal(GOAL_ACTION_MOVE, NULL, &location, -1.0);
		else
			setMainGoal(GOAL_ACTION_NONE, NULL, NULL, -1.0);
	}

	return(0);
}

long MechWarrior::coreMoveToObject (GameObjectPtr object, unsigned long params) {

	bool run = ((params & TACORDER_PARAM_RUN) != 0);
	bool jump = ((params & TACORDER_PARAM_JUMP) != 0);
	bool faceObject = ((params & TACORDER_PARAM_FACE_OBJECT) != 0);
	bool layMines = ((params & TACORDER_PARAM_LAY_MINES) != 0);
	bool setOrder = ((params & TACORDER_PARAM_DONT_SET_ORDER) == 0);
	bool keepMoving = ((params & TACORDER_PARAM_DONT_KEEP_MOVING) == 0);

	if (!object) {
		if (!coreScanTargetWID)
			coreScan(NULL, params);
		object = ObjectManager->getByWatchID(coreScanTargetWID);
	}
	if (!object)
		return(1);

	// Pick the best move goal based upon the object...
	Stuff::Vector3D goal;
	if (object->isMover())
		goal = object->getPosition();
	else {
		// If not a mover, then pick an open cell adjacent
		// to it. If it happens to be adjacent to blocked
		// cells, this could be off...
		Stuff::Vector3D objectPos = object->getPosition();
		goal = getVehicle()->calcOffsetMoveGoal(objectPos);
	}
	
	TacticalOrder tacOrder;
	tacOrder.init(ORDER_ORIGIN_COMMANDER, TACTICAL_ORDER_MOVETO_OBJECT, false);
	tacOrder.targetWID = object->getWatchID();
	tacOrder.selectionIndex = -1;
	tacOrder.moveParams.fromArea = -1;
	tacOrder.moveParams.wayPath.points[0] = goal.x;
	tacOrder.moveParams.wayPath.points[1] = goal.y;
	tacOrder.moveParams.wayPath.points[2] = goal.z;
	tacOrder.moveParams.wayPath.mode[0] = (run ? TRAVEL_MODE_FAST : TRAVEL_MODE_SLOW);
	tacOrder.moveParams.faceObject = faceObject;
	tacOrder.moveParams.wait = false;
	tacOrder.moveParams.mode = (layMines ? MOVE_MODE_MINELAYING : MOVE_MODE_NORMAL);
	tacOrder.moveParams.jump = jump;
	tacOrder.moveParams.keepMoving = keepMoving;

	long result = tacOrder.status(this);
	if (result == TACORDER_SUCCESS)
		return(1);

	if (curTacOrder.equals(&tacOrder))
		return(0);

	if (setOrder)
		clearCurTacOrder();

	// Is this already our current order?
	//if ((curTacOrder.code == TACTICAL_ORDER_MOVETO_OBJECT) &&
	//	(curTacOrder.params.move.targetId == target->getIdNumber()) &&
	//	(curTacOrder.params.move.selectionIndex == selectionIndex))
	//	return(TACORDER_FAILURE);
	tacOrder.targetWID = object->getWatchID();
	Stuff::Vector3D pos = object->getPosition();
	setMoveGoal(object->getWatchID(), &pos, object);
	setMoveRun(run);

	if (setOrder)
		clearAttackOrders();

	unsigned long moveParams = MOVEPARAM_INIT;
	if (jump)
		moveParams |= MOVEPARAM_JUMP;
	if (faceObject)
		moveParams |= MOVEPARAM_FACE_TARGET;
	if (setOrder && !object->isMover())
		moveParams |= MOVEPARAM_STEP_ADJACENT_TARGET;
	requestMovePath(-1, moveParams, 15);
	moveOrders.goalObjectPosition = object->getPosition();
	if (setOrder) {
		setGeneralTacOrder(tacOrder);
		if (useGoalPlan)
			setMainGoal(GOAL_ACTION_MOVE, object, NULL, -1.0);
		else
			setMainGoal(GOAL_ACTION_NONE, NULL, NULL, -1.0);
	}

	return(0);
}

long MechWarrior::coreEject (void) {

	MoverPtr myVehicle = getVehicle();
	if (myVehicle->getObjectClass() != BATTLEMECH)
		return(1);

	if (myVehicle->isDisabled())
		return(1);

	TacticalOrder tacOrder;
	tacOrder.init(ORDER_ORIGIN_COMMANDER, TACTICAL_ORDER_EJECT, false);

	//POINTLESS TO CHECK STATUS...
	//long result = tacOrder.status(this);
	//if (result == TACORDER_SUCCESS)
	//	return(1);

	if (curTacOrder.equals(&tacOrder))
		return(0);

	// Order has no move goal...
	clearMoveOrders();
	// Order has no attack target...
	clearAttackOrders();

	myVehicle->handleEjection();

	setGeneralTacOrder(tacOrder);
	return(1);
}

long MechWarrior::corePower (bool powerUp) {

	MoverPtr myVehicle = getVehicle();
	if (myVehicle->getObjectClass() != BATTLEMECH)
		return(1);

	if (myVehicle->isDisabled())
		return(1);

	if (powerUp) {
		if (myVehicle->getStatus() == OBJECT_STATUS_NORMAL)
			return(1);
		//if (myVehicle->getStatus() == OBJECT_STATUS_STARTING_UP)
		//	return(0);
	}
	else {
		//if (myVehicle->getStatus() == OBJECT_STATUS_SHUTTING_DOWN)
		//	return(0);
		if (myVehicle->getStatus() == OBJECT_STATUS_SHUTDOWN)
			return(1);
	}

	TacticalOrder tacOrder;
	tacOrder.init(ORDER_ORIGIN_COMMANDER, powerUp ? TACTICAL_ORDER_POWERUP : TACTICAL_ORDER_POWERDOWN, false);

	long result = tacOrder.status(this);
	if (result == TACORDER_SUCCESS)
		return(1);

	if (curTacOrder.equals(&tacOrder))
		return(0);

	// Order has no move goal...
	clearMoveOrders();
	// Order has no attack target...
	clearAttackOrders();

	// Check if we've cooled enough to restart...
	if (myVehicle) {
		if (powerUp) {
			if (myVehicle->canPowerUp())
				myVehicle->startUp();
			}
		else
			myVehicle->shutDown();
	}
	setGeneralTacOrder(tacOrder);
	return(0);
}

long MechWarrior::coreAttack (GameObjectPtr target, unsigned long params) {

	bool pursue = ((params & TACORDER_PARAM_PURSUE) != 0);
	bool obliterate = ((params & TACORDER_PARAM_OBLITERATE) != 0);
	//bool run = ((params & TACORDER_PARAM_RUN) != 0);
	bool jump = ((params & TACORDER_PARAM_JUMP) != 0);
	//bool scan = ((params & TACORDER_PARAM_SCAN) != 0);
	bool keepMoving = ((params & TACORDER_PARAM_DONT_KEEP_MOVING) == 0);

	long attackMethod = ATTACKMETHOD_RANGED;
	switch (params & TACORDER_ATTACK_MASK) {
		case TACORDER_PARAM_ATTACK_DFA:
			attackMethod = ATTACKMETHOD_DFA;
			break;
		case TACORDER_PARAM_ATTACK_RAMMING:
			attackMethod = ATTACKMETHOD_RAMMING;
			break;
	}

	long attackRange = FIRERANGE_OPTIMAL;
	switch (params & TACORDER_RANGE_MASK) {
		case TACORDER_PARAM_RANGE_RAMMING:
			attackRange = FIRERANGE_RAMMING;
			break;
		case TACORDER_PARAM_RANGE_LONGEST:
			attackRange = FIRERANGE_LONGEST;
			break;
		case TACORDER_PARAM_RANGE_OPTIMAL:
			attackRange = FIRERANGE_OPTIMAL;
			break;
		case TACORDER_PARAM_RANGE_SHORT:
			attackRange = FIRERANGE_SHORT;
			break;
		case TACORDER_PARAM_RANGE_MEDIUM:
			attackRange = FIRERANGE_MEDIUM;
			break;
		case TACORDER_PARAM_RANGE_LONG:
			attackRange = FIRERANGE_LONG;
			break;
	}

	long aimLocation = -1;
	switch (params & TACORDER_AIM_MASK) {
		case TACORDER_PARAM_AIM_HEAD:
			aimLocation = MECH_BODY_LOCATION_HEAD;
			break;
		case TACORDER_PARAM_AIM_ARM:
			aimLocation = MECH_BODY_LOCATION_RARM;
			break;
		case TACORDER_PARAM_AIM_LEG:
			aimLocation = MECH_BODY_LOCATION_RLEG;
			break;
	}

	long tactic = TACTIC_NONE;
	switch (params & TACORDER_TACTIC_MASK) {
		case TACORDER_PARAM_TACTIC_FLANK_RIGHT:
			tactic = TACTIC_FLANK_RIGHT;
			break;
		case TACORDER_PARAM_TACTIC_FLANK_LEFT:
			tactic = TACTIC_FLANK_LEFT;
			break;
		case TACORDER_PARAM_TACTIC_FLANK_REAR:
			tactic = TACTIC_FLANK_REAR;
			break;
		case TACORDER_PARAM_TACTIC_STOP_AND_FIRE:
			tactic = TACTIC_STOP_AND_FIRE;
			pursue = true;
			break;
		case TACORDER_PARAM_TACTIC_TURRET:
			tactic = TACTIC_TURRET;
			pursue = false;
			jump = false;
			keepMoving = false;
			break;
		case TACORDER_PARAM_TACTIC_JOUST:
			tactic = TACTIC_JOUST;
			break;
	}

	if ((getVehicle()->numWeapons == 0) && (attackMethod != ATTACKMETHOD_RAMMING)) {
		clearAttackOrders();
		return(1);
	}

	if (!target) {
		if (!coreScanTargetWID)
			coreScan(NULL, params);
		target = ObjectManager->getByWatchID(coreScanTargetWID);
	}
	LastCoreAttackTarget = target;
	if (!target)
		return(1);

	TacticalOrder tacOrder;
	tacOrder.init((OrderOriginType)ORDER_ORIGIN_COMMANDER, TACTICAL_ORDER_ATTACK_OBJECT, false);
	tacOrder.targetWID = target->getWatchID();
	tacOrder.attackParams.method = (AttackMethod)attackMethod;
	tacOrder.attackParams.aimLocation = aimLocation;
	if (attackMethod == ATTACKMETHOD_RAMMING)
		attackRange = FIRERANGE_RAMMING;
	tacOrder.attackParams.range = (FireRangeType)attackRange;
	tacOrder.attackParams.pursue = pursue;
	tacOrder.attackParams.obliterate = obliterate;
	tacOrder.attackParams.tactic = (TacticType)tactic;
	tacOrder.moveParams.keepMoving = keepMoving;
	tacOrder.moveParams.jump = jump;

	long result = tacOrder.status(this);
	if (result == TACORDER_SUCCESS)
		return(1);

	if (curTacOrder.equals(&tacOrder))
		return(0);

	clearCurTacOrder();

	setSituationFireRange(getVehicle()->getOrderedFireRange(&attackRange));

	// If we also want to pursue it, set up the move order here...
	if (pursue) {
		// Order moveToObject, making sure we face the object...
		params |= TACORDER_PARAM_FACE_OBJECT;
		//coreMoveToObject(target, params + TACORDER_PARAM_DONT_SET_ORDER);
		}
	else
		clearMoveOrders();

	// Now, set up the tactical order, and pass it to the pilot...
	setAttackType(ATTACK_TO_DESTROY);
	setAttackTarget(target);
	setAttackAimLocation(aimLocation);
	setAttackPursuit(pursue);
	setSituationOpenFire(true);

	// Remember the most recent target...
	setLastTarget(target, obliterate);

	setGeneralTacOrder(tacOrder);

	if (useGoalPlan)
		setMainGoal(GOAL_ACTION_ATTACK, target, NULL, -1.0);
	else
		setMainGoal(GOAL_ACTION_NONE, NULL, NULL, -1.0);

	return(0);
}

long MechWarrior::coreCapture (GameObjectPtr object, unsigned long params) {

	if (!object) {
		if (!coreScanTargetWID)
			coreScan(NULL, params);
		object = ObjectManager->getByWatchID(coreScanTargetWID);
	}

	if (!getTeam())
		return(1);

	if (!object || !object->isCaptureable(getTeam()->getId()) || object->isFriendly(getVehicle()))
		return(1);
	
	// Once Goal Planning is in, this should deal with the blockers rather than
	// abort the order...
//	GameObjectPtr blockerList[MAX_MOVERS];
//	long numBlockers = object->getCaptureBlocker(getVehicle(), blockerList);
//	if (numBlockers)
//		return(1);

	bool run = ((params & TACORDER_PARAM_RUN) != 0);
	bool jump = ((params & TACORDER_PARAM_JUMP) != 0);
	bool faceObject = ((params & TACORDER_PARAM_FACE_OBJECT) != 0);

	TacticalOrder tacOrder;
	tacOrder.init(ORDER_ORIGIN_COMMANDER, TACTICAL_ORDER_CAPTURE, false);
	tacOrder.targetWID = object->getWatchID();
	tacOrder.selectionIndex = -1;
	tacOrder.moveParams.fromArea = -1;
	tacOrder.moveParams.wayPath.mode[0] = (run ? TRAVEL_MODE_FAST : TRAVEL_MODE_SLOW);
	tacOrder.moveParams.faceObject = faceObject;
	tacOrder.moveParams.wait = false;
	tacOrder.moveParams.keepMoving = false;
	tacOrder.moveParams.jump = jump;

	long result = tacOrder.status(this);
	if (result == TACORDER_SUCCESS)
		return(1);

	if (curTacOrder.equals(&tacOrder))
		return(0);

	setGeneralTacOrder(tacOrder);

	if (useGoalPlan)
		setMainGoal(GOAL_ACTION_CAPTURE, object, NULL, 250.0);
	else
		setMainGoal(GOAL_ACTION_NONE, NULL, NULL, -1.0);

	return(0);
}

long MechWarrior::coreScan (GameObjectPtr object, unsigned long params) {

	if (object && object->getTeam()) {
		// If this object is currently a contact, set myTarget to it and
		// return true. Otherwise, return false.
		if (object->isMover()) {
			if (getTeam()->isContact(getVehicle(), (MoverPtr)object, (long)params)) {
				coreScanTargetWID = object->getWatchID();
				return(coreScanTargetWID);
			}
			}
		else {
			if (/*CurWarrior->*/getVehicle()->lineOfSight(object)) {
				coreScanTargetWID = object->getWatchID();
				return(coreScanTargetWID);
			}
		}
		}
	else {
		// Scan for anything matching the criteria...
		Stuff::Vector3D myPosition = getVehicle()->getPosition();
		int32_t action = -1;
		coreScanTargetWID = targetPriorityList.calcTarget(this, myPosition, params, action);
		return(coreScanTargetWID);
	}
	return(coreScanTargetWID = 0);
}

long MechWarrior::coreControl (GameObjectPtr object, unsigned long params) {

	if (!object) {
		if (!coreScanTargetWID)
			coreScan(NULL, params);
		object = ObjectManager->getByWatchID(coreScanTargetWID);
	}
	if (!object)
		return(1);

	long action = targetPriorityList.calcAction(this, object);
	if (action == TACTICAL_ORDER_ATTACK_OBJECT)
		return(coreAttack(object, params));
	else if (action == TACTICAL_ORDER_CAPTURE)
		return(coreCapture(object, params));
	return(1);
}

long MechWarrior::coreSetState (long stateID, bool thinkAgain) {

	return(0);
}
