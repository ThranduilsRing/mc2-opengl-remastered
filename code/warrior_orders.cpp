// MechWarrior: order* entry points and the event handlers.
// Declared in the class headers; shared names in warrior_internal.h.
#include "warrior_internal.h"
#include "tactic_table.h"
#include <array>


long MechWarrior::orderWait (bool unitOrder, long origin, long seconds, bool clearLastTarget) {
    if (absorbsGeneralOrder(this, origin)) return TACORDER_SUCCESS;

	TacticalOrder tacOrder;
	tacOrder.init(static_cast<OrderOriginType>(origin), TACTICAL_ORDER_WAIT, unitOrder);
	tacOrder.delayedTime = scenarioTime + seconds;

	clearMoveOrders();
	clearAttackOrders();

	if (clearLastTarget)
		setLastTarget(nullptr);

	if (origin == ORDER_ORIGIN_COMMANDER)
		setGeneralTacOrder(tacOrder);

	return(tacOrder.status(this));
}

long MechWarrior::orderStop (bool unitOrder, bool setTacOrder) {

	TacticalOrder tacOrder;
	tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_STOP, unitOrder);

	clearTacOrderQueue();

	clearMoveOrders();
	clearAttackOrders();

		setLastTarget(nullptr);


	getVehicle()->suppressionFire = 0;

	return(tacOrder.status(this));
}

long MechWarrior::orderMoveToPoint (bool unitOrder, bool setTacOrder, long origin, glm::vec3 location, long selectionIndex, unsigned long params) {
    if (absorbsGeneralOrder(this, origin)) return TACORDER_SUCCESS;

	bool jump = ((params & TACORDER_PARAM_JUMP) != 0);
	bool run = ((params & TACORDER_PARAM_RUN) != 0);
	bool wait = ((params & TACORDER_PARAM_WAIT) != 0);
	bool layMines = ((params & TACORDER_PARAM_LAY_MINES) != 0);
	bool escape = ((params & TACORDER_PARAM_ESCAPE_TILE) != 0);

	TacticalOrder tacOrder;
	tacOrder.init(static_cast<OrderOriginType>(origin), TACTICAL_ORDER_MOVETO_POINT, unitOrder);
	tacOrder.moveParams.wayPath.points[0] = location.x;
	tacOrder.moveParams.wayPath.points[1] = location.y;
	tacOrder.moveParams.wayPath.points[2] = location.z;
	tacOrder.selectionIndex = selectionIndex;
	tacOrder.moveParams.wayPath.mode[0] = (run ? TRAVEL_MODE_FAST : TRAVEL_MODE_SLOW);
	tacOrder.moveParams.wait = wait;
	tacOrder.moveParams.mode = (layMines ? MOVE_MODE_MINELAYING : MOVE_MODE_NORMAL);
	tacOrder.moveParams.escapeTile = escape;
	tacOrder.moveParams.jump = jump;

	long result = tacOrder.status(this);
	if (result == TACORDER_SUCCESS)
		return(TACORDER_SUCCESS);

	setMoveGoal(MOVEGOAL_NONE, nullptr);
	setMoveWayPath(nullptr, 0);
	
	setMoveGoal(MOVEGOAL_LOCATION, &location);
	moveOrders.timeOfLastStep = scenarioTime;

	setMoveRun(run);

	if (setTacOrder)
		clearAttackOrders();

	unsigned long moveParams = MOVEPARAM_INIT | MOVEPARAM_FACE_TARGET;
	if (escape)
		moveParams |= MOVEPARAM_ESCAPE_TILE;
	if (jump)
		moveParams |= MOVEPARAM_JUMP;
	if (setTacOrder && (origin == ORDER_ORIGIN_PLAYER))
		if (!unitOrder || (getPoint() == getVehicle()))
			moveParams |= MOVEPARAM_RADIO_RESULT;
		PathManager->request(this, selectionIndex, moveParams, setTacOrder ? 21 : 23);
	if (setTacOrder && (result == TACORDER_FAILURE) && (origin == ORDER_ORIGIN_COMMANDER))
		setGeneralTacOrder(tacOrder);

	return(result);
}

void MechWarrior::clearMovePath (long pathNum) {

	if (moveOrders.path[pathNum]) {
		moveOrders.path[pathNum]->clear();
	}
}

long MechWarrior::orderMoveToObject (bool unitOrder, bool setTacOrder, long origin, GameObjectPtr target, long fromArea, long selectionIndex, unsigned long params) {
    if (absorbsGeneralOrder(this, origin)) return TACORDER_SUCCESS;

	bool jump = ((params & TACORDER_PARAM_JUMP) != 0);
	bool run = ((params & TACORDER_PARAM_RUN) != 0);
	bool faceObject = ((params & TACORDER_PARAM_FACE_OBJECT) != 0);
	bool layMines = ((params & TACORDER_PARAM_LAY_MINES) != 0);
		
	if (!target)
		return(TACORDER_SUCCESS);

	glm::vec3 goal;
	if (target->isMover())
		goal = target->getPosition();
	else {
		// Non-movers need an adjacent destination because their own cell may be blocked.
		if (target->isBuilding()) {
			BuildingPtr building = static_cast<BuildingPtr>(target);
			long goalRow = 0, goalCol = 0;
			bool foundGoal = building->calcAdjacentAreaCell(getVehicle()->moveLevel, fromArea, goalRow, goalCol);
			if (foundGoal)
				land->cellToWorld(goalRow, goalCol, goal);
			else {
				glm::vec3 objectPos = target->getPosition();
				goal = getVehicle()->calcOffsetMoveGoal(objectPos);
			}
			}
		else {
			glm::vec3 objectPos = target->getPosition();
			goal = getVehicle()->calcOffsetMoveGoal(objectPos);
		}
	}

	TacticalOrder tacOrder;
	tacOrder.init(static_cast<OrderOriginType>(origin), TACTICAL_ORDER_MOVETO_OBJECT, unitOrder);
	tacOrder.targetWID = target->getWatchID();
	tacOrder.selectionIndex = selectionIndex;
	tacOrder.moveParams.fromArea = fromArea;
	tacOrder.moveParams.wayPath.points[0] = goal.x;
	tacOrder.moveParams.wayPath.points[1] = goal.y;
	tacOrder.moveParams.wayPath.points[2] = goal.z;
	tacOrder.moveParams.wayPath.mode[0] = (run ? TRAVEL_MODE_FAST : TRAVEL_MODE_SLOW);
	tacOrder.moveParams.faceObject = faceObject;
	tacOrder.moveParams.wait = false;
	tacOrder.moveParams.mode = (layMines ? MOVE_MODE_MINELAYING : MOVE_MODE_NORMAL);
	tacOrder.moveParams.jump = jump;

	long result = tacOrder.status(this);
	if (result == TACORDER_SUCCESS)
		return(TACORDER_SUCCESS);

	tacOrder.targetWID = target->getWatchID();
	glm::vec3 pos = target->getPosition();
	setMoveGoal(target->getWatchID(), &pos, target);
	setMoveRun(run);

	if (setTacOrder)
		clearAttackOrders();

	unsigned long moveParams = MOVEPARAM_INIT;
	if (jump)
		moveParams |= MOVEPARAM_JUMP;
	if (faceObject)
		moveParams |= MOVEPARAM_FACE_TARGET;
	if (setTacOrder && !target->isMover())
		moveParams |= MOVEPARAM_STEP_ADJACENT_TARGET;
	if (setTacOrder && (origin == ORDER_ORIGIN_PLAYER))
		if (!unitOrder || (getPoint() == getVehicle()))
				moveParams |= MOVEPARAM_RADIO_RESULT;
	requestMovePath(selectionIndex, moveParams, 16);
	moveOrders.goalObjectPosition = target->getPosition();
	if (setTacOrder && (result == TACORDER_FAILURE) && (origin == ORDER_ORIGIN_COMMANDER))
		setGeneralTacOrder(tacOrder);

	return(result);
}

long MechWarrior::orderJumpToPoint (bool unitOrder, bool setTacOrder, long origin, glm::vec3 location, long selectionIndex) {
    if (absorbsGeneralOrder(this, origin)) return TACORDER_SUCCESS;

	MoverPtr myVehicle = getVehicle();
	if (myVehicle->getJumpRange() < myVehicle->distanceFrom(location))
		return(TACORDER_SUCCESS);

	if (myVehicle->getObjectClass() == BATTLEMECH) {
		int cellR, cellC;
		land->worldToCell(location, cellR, cellC);
		if (!GameMap->getPassable(cellR, cellC))
			return(TACORDER_SUCCESS);
	}

	TacticalOrder tacOrder;
	tacOrder.init(static_cast<OrderOriginType>(origin), TACTICAL_ORDER_JUMPTO_POINT, unitOrder);
	tacOrder.selectionIndex = selectionIndex;
	tacOrder.moveParams.wayPath.points[0] = location.x;
	tacOrder.moveParams.wayPath.points[1] = location.y;
	tacOrder.moveParams.wayPath.points[2] = location.z;

	long result = tacOrder.status(this);
	if (result == TACORDER_SUCCESS)
		return(TACORDER_SUCCESS);

	if (setTacOrder) {
		clearMoveOrders();
		clearAttackOrders();
	}

	if (setTacOrder && (result == TACORDER_FAILURE) && (origin == ORDER_ORIGIN_COMMANDER))
		setGeneralTacOrder(tacOrder);

	return(TACORDER_SUCCESS);
}

long MechWarrior::orderJumpToObject (bool unitOrder, bool setTacOrder, long origin, GameObjectPtr target, long selectionIndex) {
    if (absorbsGeneralOrder(this, origin)) return TACORDER_SUCCESS;

	MoverPtr myVehicle = getVehicle();
	glm::vec3 location = target->getPosition();

	if (target->isMover()) {
		if (target->getTeam() == myVehicle->getTeam())
			return(TACORDER_SUCCESS);
	}

	if (myVehicle->getJumpRange() < myVehicle->distanceFrom(location))
		return(TACORDER_SUCCESS);

	if (myVehicle->getObjectClass() == BATTLEMECH) {
		int cellR, cellC;
		land->worldToCell(location, cellR, cellC);
		if (!GameMap->getPassable(cellR, cellC))
			return(TACORDER_SUCCESS);
	}

	TacticalOrder tacOrder;
	tacOrder.init(static_cast<OrderOriginType>(origin), TACTICAL_ORDER_JUMPTO_POINT, unitOrder);
	tacOrder.selectionIndex = selectionIndex;
	tacOrder.moveParams.wayPath.points[0] = location.x;
	tacOrder.moveParams.wayPath.points[1] = location.y;
	tacOrder.moveParams.wayPath.points[2] = location.z;
	tacOrder.targetWID = target->getWatchID();

	long result = tacOrder.status(this);
	if (result == TACORDER_SUCCESS)
		return(TACORDER_SUCCESS);

	if (setTacOrder) {
		clearMoveOrders();
		clearAttackOrders();
	}

	if (setTacOrder && (result == TACORDER_FAILURE) && (origin == ORDER_ORIGIN_COMMANDER))
		setGeneralTacOrder(tacOrder);

	return(TACORDER_SUCCESS);
}

long MechWarrior::orderTraversePath (bool unitOrder, bool setTacOrder, long origin, WayPathPtr wayPath, unsigned long params) {
    if (absorbsGeneralOrder(this, origin)) return TACORDER_SUCCESS;

	bool layMines = ((params & TACORDER_PARAM_LAY_MINES) != 0);

	TacticalOrder tacOrder;
	tacOrder.init(static_cast<OrderOriginType>(origin), TACTICAL_ORDER_TRAVERSE_PATH, unitOrder);
    tacOrder.moveParams.wayPath = *wayPath;
	tacOrder.moveParams.mode = (layMines ? MOVE_MODE_MINELAYING : MOVE_MODE_NORMAL);

	long result = tacOrder.status(this);
	if (result == TACORDER_SUCCESS)
		return(TACORDER_SUCCESS);

	glm::vec3 location;
	location.x = tacOrder.moveParams.wayPath.points[0];
	location.y = tacOrder.moveParams.wayPath.points[1];
	location.z = tacOrder.moveParams.wayPath.points[2];
	setMoveGoal(MOVEGOAL_LOCATION, &location);
	setMoveWayPath(wayPath, false);

	if (setTacOrder)
		clearAttackOrders();

	requestMovePath(-1, MOVEPARAM_INIT | MOVEPARAM_FACE_TARGET, 17);
	if (setTacOrder && (result == TACORDER_FAILURE) && (origin == ORDER_ORIGIN_COMMANDER))
		setGeneralTacOrder(tacOrder);

	return(result);	
}

long MechWarrior::orderPatrolPath (bool unitOrder, bool setTacOrder, long origin, WayPathPtr wayPath) {
    if (absorbsGeneralOrder(this, origin)) return TACORDER_SUCCESS;

	TacticalOrder tacOrder;
	tacOrder.init(static_cast<OrderOriginType>(origin), TACTICAL_ORDER_PATROL_PATH, unitOrder);
    tacOrder.moveParams.wayPath = *wayPath;
	
	long result = tacOrder.status(this);
	if (result == TACORDER_SUCCESS)
		return(TACORDER_SUCCESS);

	glm::vec3 location;
	location.x = tacOrder.moveParams.wayPath.points[0];
	location.y = tacOrder.moveParams.wayPath.points[1];
	location.z = tacOrder.moveParams.wayPath.points[2];
	setMoveGoal(MOVEGOAL_LOCATION, &location);
	setMoveWayPath(wayPath, true);
	if (setTacOrder)
		clearAttackOrders();

	requestMovePath(-1, MOVEPARAM_INIT | MOVEPARAM_FACE_TARGET, 18);
	if (setTacOrder && (result == TACORDER_FAILURE) && (origin == ORDER_ORIGIN_COMMANDER))
		setGeneralTacOrder(tacOrder);

	return(result);	
}

long MechWarrior::orderPowerUp (bool unitOrder, long origin) {
    if (absorbsGeneralOrder(this, origin)) return TACORDER_SUCCESS;

	MoverPtr myVehicle = getVehicle();
	if ((myVehicle->getStatus() != OBJECT_STATUS_SHUTDOWN))
		return TACORDER_SUCCESS;

	TacticalOrder tacOrder;
	tacOrder.init(static_cast<OrderOriginType>(origin), TACTICAL_ORDER_POWERUP, unitOrder);

	long result = tacOrder.status(this);
	if (result == TACORDER_SUCCESS)
		return(TACORDER_SUCCESS);

	clearMoveOrders();
	clearAttackOrders();

	if (myVehicle)
		if (myVehicle->canPowerUp())
			myVehicle->startUp();
	if (origin == ORDER_ORIGIN_COMMANDER)
		setGeneralTacOrder(tacOrder);
	else if (origin == ORDER_ORIGIN_SELF)
		setAlarmTacOrder(tacOrder, 255);

	return(result);	
}

long MechWarrior::orderPowerDown (bool unitOrder, long origin) {
    if (absorbsGeneralOrder(this, origin)) return TACORDER_SUCCESS;

	MoverPtr myVehicle = getVehicle();
	if ((myVehicle->getStatus() == OBJECT_STATUS_SHUTDOWN) || (myVehicle->getStatus() == OBJECT_STATUS_SHUTTING_DOWN))
		return(TACORDER_SUCCESS);
	if (myVehicle->isDisabled())
		return(TACORDER_SUCCESS);
	
	TacticalOrder tacOrder;
	tacOrder.init(static_cast<OrderOriginType>(origin), TACTICAL_ORDER_POWERDOWN, unitOrder);

	long result = tacOrder.status(this);
	if (result == TACORDER_SUCCESS)
		return(TACORDER_SUCCESS);

	clearMoveOrders();
	clearAttackOrders();

	if (myVehicle)
		myVehicle->shutDown();
	if (origin == ORDER_ORIGIN_COMMANDER)
		setGeneralTacOrder(tacOrder);

	return(result);	
}

long MechWarrior::orderUseSpeed (float speed) {

	// The speed we're given is in meters/sec. We need to convert
	// this to our proper "sprite speed", using gesture state and
	// (if appropriate) throttle setting...
	moveOrders.speedVelocity = speed;
	MoverPtr myVehicle = getVehicle();
	long state, throttle;
	myVehicle->calcSpriteSpeed(speed, 0, state, throttle);
	moveOrders.speedState = state;
	moveOrders.speedThrottle = throttle;

	return(TACORDER_SUCCESS);
}

long MechWarrior::orderOrbitPoint (glm::vec3 location) {

	return(1);
}

long orderOrbitObject (GameObjectPtr target) {

	return(1);
}

long orderUseOrbitRange (long type, float range) {

	return(1);
}

long MechWarrior::orderAttackObject (bool unitOrder, long origin, GameObjectPtr target, long type, long method, long range, long aimLocation, long fromArea, unsigned long params) {
    if (absorbsGeneralOrder(this, origin)) return TACORDER_SUCCESS;

	bool run = ((params & TACORDER_PARAM_RUN) != 0);
	bool jump = ((params & TACORDER_PARAM_JUMP) != 0);
	bool pursue = ((params & TACORDER_PARAM_PURSUE) != 0);
	bool obliterate = ((params & TACORDER_PARAM_OBLITERATE) != 0);
	bool conserve = (type == ATTACK_CONSERVING_AMMO);
	bool keepMoving = ((params & TACORDER_PARAM_DONT_KEEP_MOVING) == 0);

	// Explicit table decode (legacy flag bits or the extended-tactic field); only SF and TURRET change the order flags.
	long tactic = tactic_table::from_params(params);
	switch (tactic) {
		case TACTIC_STOP_AND_FIRE:
			pursue = true;
			break;
		case TACTIC_TURRET:
			jump = false;
			pursue = false;
			keepMoving = false;
			break;
		default:
			break;
	}

	if (getVehicle()->attackRange == FIRERANGE_CURRENT) {
		range = FIRERANGE_CURRENT;
		pursue = false;
		jump = false;
	}

	if (!target) {
		clearAttackOrders();
		return(TACORDER_SUCCESS);
	}

	if ((getVehicle()->numWeapons == 0) && (method != std::to_underlying(ATTACKMETHOD_RAMMING))) {
		clearAttackOrders();
		return(TACORDER_SUCCESS);
	}

	TacticalOrder tacOrder;
	tacOrder.init(static_cast<OrderOriginType>(origin), TACTICAL_ORDER_ATTACK_OBJECT, unitOrder);
	tacOrder.targetWID = target->getWatchID();
	tacOrder.attackParams.type = static_cast<AttackType>(type);
	tacOrder.attackParams.method = static_cast<AttackMethod>(method);
	tacOrder.attackParams.aimLocation = aimLocation;
	tacOrder.attackParams.tactic = static_cast<TacticType>(tactic);
	if (method == std::to_underlying(ATTACKMETHOD_RAMMING))
		range = FIRERANGE_RAMMING;
	tacOrder.attackParams.range = static_cast<FireRangeType>(range);
	tacOrder.attackParams.pursue = pursue;
	tacOrder.attackParams.obliterate = obliterate;
	tacOrder.moveParams.fromArea = fromArea;
	tacOrder.moveParams.jump = jump;
	tacOrder.moveParams.keepMoving = keepMoving;
	long result = tacOrder.status(this);
	if (result == TACORDER_SUCCESS)
		return(TACORDER_SUCCESS);
	if (curTacOrder.equals(&tacOrder))
		return(0);

	setSituationFireRange(getVehicle()->getOrderedFireRange(&range));

	if (pursue) {
		setMoveRun(run);
		params |= TACORDER_PARAM_FACE_OBJECT;
		// Pursuit needs movement while retaining the outer attack order.
		orderMoveToObject(unitOrder, false, origin, target, fromArea, -1/*unitMateId*/, params);
		}
	else
		clearMoveOrders();

	setAttackType(type);
	setAttackTarget(target);
	setAttackAimLocation(aimLocation);
	setAttackPursuit(pursue);
	setSituationOpenFire(true);

	setLastTarget(target, obliterate, conserve);

	if (origin == ORDER_ORIGIN_COMMANDER)
		setGeneralTacOrder(tacOrder);

	/*
	if (GameSystemWindow) {
		char debugStr[200];
		GameSystemWindow->print("");
		GameSystemWindow->print("-----------------------------------");
		sprintf(debugStr, "%s:", name);
		GameSystemWindow->print(debugStr);
		sprintf(debugStr, "Longest Range Weapon = %s (%.4f)",
			MasterComponentList[(MoverPtr(BaseObjectPtr(vehicle)))->inventory[(MoverPtr(BaseObjectPtr(vehicle)))->longestRangeWeapon].masterID].getName(),
			MasterComponentList[(MoverPtr(BaseObjectPtr(vehicle)))->inventory[(MoverPtr(BaseObjectPtr(vehicle)))->longestRangeWeapon].masterID].getWeaponLongRange());
		GameSystemWindow->print(debugStr);
		sprintf(debugStr, "Optimal Range = %.4f", (MoverPtr(BaseObjectPtr(vehicle)))->optimalRange);
		GameSystemWindow->print(debugStr);
		GameSystemWindow->print("-----------------------------------");
	}
	*/

	return(TACORDER_FAILURE);
}

long MechWarrior::orderAttackPoint (bool unitOrder, long origin, glm::vec3 location, long type, long method, long range, unsigned long params) {
    if (absorbsGeneralOrder(this, origin)) return TACORDER_SUCCESS;

	bool pursue = ((params & TACORDER_PARAM_PURSUE) != 0);
	bool jump = ((params & TACORDER_PARAM_JUMP) != 0);

	TacticalOrder tacOrder;
	tacOrder.init(static_cast<OrderOriginType>(origin), TACTICAL_ORDER_ATTACK_POINT, unitOrder);
	tacOrder.attackParams.type = static_cast<AttackType>(type);
	tacOrder.attackParams.method = static_cast<AttackMethod>(method);
	tacOrder.attackParams.range = static_cast<FireRangeType>(range);
	tacOrder.attackParams.pursue = pursue;
	tacOrder.attackParams.targetPoint = location;
	tacOrder.moveParams.jump = jump;

	long result = tacOrder.status(this);
	if (result == TACORDER_SUCCESS)
		return(TACORDER_SUCCESS);

	setSituationFireRange(getVehicle()->getOrderedFireRange(&range));

	if (pursue)
		orderMoveToPoint(unitOrder, false, origin, location, -1/*unitMateId*/, params);
	else
		clearMoveOrders();

	setAttackType(type);
	setAttackTarget(nullptr);
	setAttackTargetPoint(location);
	setAttackAimLocation(-1);
	setAttackPursuit(pursue);
	setSituationOpenFire(true);

	setLastTarget(nullptr);

	if (origin == ORDER_ORIGIN_COMMANDER)
		setGeneralTacOrder(tacOrder);
	
	/*
	if (GameSystemWindow) {
		char debugStr[200];
		GameSystemWindow->print("");
		GameSystemWindow->print("-----------------------------------");
		sprintf(debugStr, "%s:", name);
		GameSystemWindow->print(debugStr);
		sprintf(debugStr, "Longest Range Weapon = %s (%.4f)",
			MasterComponentList[(MoverPtr(BaseObjectPtr(vehicle)))->inventory[(MoverPtr(BaseObjectPtr(vehicle)))->longestRangeWeapon].masterID].getName(),
			MasterComponentList[(MoverPtr(BaseObjectPtr(vehicle)))->inventory[(MoverPtr(BaseObjectPtr(vehicle)))->longestRangeWeapon].masterID].getWeaponLongRange());
		GameSystemWindow->print(debugStr);
		sprintf(debugStr, "Optimal Range = %.4f", (MoverPtr(BaseObjectPtr(vehicle)))->optimalRange);
		GameSystemWindow->print(debugStr);
		GameSystemWindow->print("-----------------------------------");
	}
	*/

	return(TACORDER_FAILURE);
}

long MechWarrior::orderWithdraw (bool unitOrder, long origin, glm::vec3 location) {
    if (absorbsGeneralOrder(this, origin)) return TACORDER_SUCCESS;

	TacticalOrder tacOrder;
	tacOrder.init(static_cast<OrderOriginType>(origin), TACTICAL_ORDER_WITHDRAW, unitOrder);
	tacOrder.moveParams.wayPath.points[0] = location.x;
	tacOrder.moveParams.wayPath.points[1] = location.y;
	tacOrder.moveParams.wayPath.points[2] = location.z;

	location = calcWithdrawGoal();
	long result = orderMoveToPoint(unitOrder, true, origin, location, -1, TACORDER_PARAM_RUN);
	MoverPtr myVehicle = getVehicle();
	Assert(myVehicle != nullptr, 0, " orderWithdraw:Warrior has no Vehicle ");

	myVehicle->withdrawing = true;

	if (origin == ORDER_ORIGIN_COMMANDER)
		setGeneralTacOrder(tacOrder);

	curTacOrder.code = TACTICAL_ORDER_WITHDRAW;

	return(result);
}

long MechWarrior::orderEject (bool unitOrder, bool setTacOrder, long origin) {

	TacticalOrder tacOrder;
	tacOrder.init(static_cast<OrderOriginType>(origin), TACTICAL_ORDER_EJECT, unitOrder);

	MoverPtr myVehicle = getVehicle();
	Assert(myVehicle != nullptr, 0, " orderWithdraw:Warrior has no Vehicle ");

	myVehicle->handleEjection();

	if (origin == ORDER_ORIGIN_COMMANDER)
		setGeneralTacOrder(tacOrder);
	
	return(TACORDER_SUCCESS);
}

long MechWarrior::orderRefit (long origin, GameObjectPtr target, unsigned long params)
{
	if (!target || target->getObjectClass() != BATTLEMECH)
		return(TACORDER_SUCCESS);

	bool run = ((params & TACORDER_PARAM_RUN) != 0);

	TacticalOrder tacOrder;
	tacOrder.init(static_cast<OrderOriginType>(origin), TACTICAL_ORDER_REFIT, false);
	tacOrder.targetWID = target->getWatchID();
	tacOrder.selectionIndex = -1;
	tacOrder.moveParams.wayPath.mode[0] = (run ? TRAVEL_MODE_FAST : TRAVEL_MODE_SLOW);
	tacOrder.moveParams.faceObject = true;
	tacOrder.moveParams.wait = false;

	if (origin == ORDER_ORIGIN_COMMANDER)
		setGeneralTacOrder(tacOrder);

	return(NO_ERR);
}

long MechWarrior::orderRecover (long origin, GameObjectPtr target, unsigned long params)
{
	if (!target || target->getObjectClass() != BATTLEMECH)
		return(TACORDER_SUCCESS);

	bool run = ((params & TACORDER_PARAM_RUN) != 0);

	TacticalOrder tacOrder;
	tacOrder.init(static_cast<OrderOriginType>(origin), TACTICAL_ORDER_RECOVER, false);
	tacOrder.targetWID = target->getWatchID();
	tacOrder.selectionIndex = -1;
	tacOrder.moveParams.wayPath.mode[0] = (run ? TRAVEL_MODE_FAST : TRAVEL_MODE_SLOW);
	tacOrder.moveParams.faceObject = true;
	tacOrder.moveParams.wait = false;

	if (origin == ORDER_ORIGIN_COMMANDER)
		setGeneralTacOrder(tacOrder);

	return(NO_ERR);
}

long MechWarrior::orderGetFixed (long origin, GameObjectPtr target, unsigned long params)
{

	if (!target || (target->getObjectClass() != TREEBUILDING && target->getRefitPoints() <= 0.0))
		return(TACORDER_SUCCESS);
#ifdef USE_BUILDINGS
	if ((getVehicle()->getObjectClass() == BATTLEMECH && (reinterpret_cast<TreeBuildingPtr>(target))->mechBay == false) ||
		(getVehicle()->getObjectClass() == GROUNDVEHICLE && (reinterpret_cast<TreeBuildingPtr>(target))->mechBay == true))
		return(TACORDER_SUCCESS);
#endif

	bool run = ((params & TACORDER_PARAM_RUN) != 0);

	TacticalOrder tacOrder;
	tacOrder.init(static_cast<OrderOriginType>(origin), TACTICAL_ORDER_GETFIXED, false);
	tacOrder.targetWID = target->getWatchID();
	tacOrder.selectionIndex = -1;
	tacOrder.moveParams.wayPath.mode[0] = (run ? TRAVEL_MODE_FAST : TRAVEL_MODE_SLOW);
	tacOrder.moveParams.faceObject = true;
	tacOrder.moveParams.wait = false;

	if (origin == ORDER_ORIGIN_COMMANDER)
		setGeneralTacOrder(tacOrder);

	return(NO_ERR);
}

long MechWarrior::orderLoadIntoCarrier (long origin, GameObjectPtr target, unsigned long params) {

	bool run = ((params & TACORDER_PARAM_RUN) != 0);

	TacticalOrder tacOrder;
	tacOrder.init(static_cast<OrderOriginType>(origin), TACTICAL_ORDER_LOAD_INTO_CARRIER, false);
	tacOrder.targetWID = target->getWatchID();
	tacOrder.selectionIndex = -1;
	tacOrder.moveParams.wayPath.mode[0] = (run ? TRAVEL_MODE_FAST : TRAVEL_MODE_SLOW);
	tacOrder.moveParams.faceObject = true;
	tacOrder.moveParams.wait = false;

	if (origin == ORDER_ORIGIN_COMMANDER)
		setGeneralTacOrder(tacOrder);

	return(NO_ERR);
}

long MechWarrior::orderDeployElementals (long origin, unsigned long params) {

	bool run = ((params & TACORDER_PARAM_RUN) != 0);

	TacticalOrder tacOrder;
	tacOrder.init(static_cast<OrderOriginType>(origin), TACTICAL_ORDER_DEPLOY_ELEMENTALS, false);
	tacOrder.moveParams.wayPath.mode[0] = (run ? TRAVEL_MODE_FAST : TRAVEL_MODE_SLOW);
	tacOrder.moveParams.wait = false;

	if (origin == ORDER_ORIGIN_COMMANDER)
		setGeneralTacOrder(tacOrder);

	return(NO_ERR);
}

long MechWarrior::orderCapture (long origin, GameObjectPtr target, long fromArea, unsigned long params) {

	if (!target || /*!target->isCaptureable(getTeam()->getId()) ||*/ target->isFriendly(getVehicle()) || 
			target->getCaptureBlocker(getVehicle()) != 0)
		return(1);

	bool run = ((params & TACORDER_PARAM_RUN) != 0);
	bool jump = ((params & TACORDER_PARAM_JUMP) != 0);

	TacticalOrder tacOrder;
	tacOrder.init(static_cast<OrderOriginType>(origin), TACTICAL_ORDER_CAPTURE, false);
	tacOrder.targetWID = target->getWatchID();
	tacOrder.selectionIndex = -1;
	tacOrder.moveParams.wayPath.mode[0] = (run ? TRAVEL_MODE_FAST : TRAVEL_MODE_SLOW);
	tacOrder.moveParams.faceObject = true;
	tacOrder.moveParams.wait = false;
	tacOrder.moveParams.fromArea = fromArea;
	tacOrder.moveParams.jump = jump;

	if (origin == ORDER_ORIGIN_COMMANDER)
		setGeneralTacOrder(tacOrder);

	return(NO_ERR);
}


long MechWarrior::handleTargetOfWeaponFire (void) {


#ifdef USE_IFACE
	if (myVehicle)
		theInterface->ObjectAttacked(myVehicle->getPartId());
#endif
	
	return(NO_ERR);
}

long MechWarrior::handleHitByWeaponFire (void) {

	long attackerWID = alarm[PILOT_ALARM_HIT_BY_WEAPONFIRE].trigger[0];

	if (attackerWID == 0) 
	{
		// Ammo Explosion?
	}
	else if (attackerWID == -3)
		radioMessage(RADIO_UNDER_AIRSTRIKE, true);
	else if (attackerWID == -2)
		radioMessage(RADIO_HITTING_MINES, true);
	else
	{
		long targetWID = 0;
		if (getCurTacOrder()->code != TACTICAL_ORDER_NONE)
			targetWID = getCurTacOrder()->targetWID;

		if (!targetWID)
			radioMessage(RADIO_UNDER_ATTACK, true);
	}

	return(NO_ERR);
}

long MechWarrior::handleCollision (void) {

	// Colliding with trees should not get here...


	return(NO_ERR);
}

long MechWarrior::handleDamageTakenRate (void) {


	return(NO_ERR);
}

long MechWarrior::handleUnitMateDeath (void) {

	unsigned long mateWID = alarm[PILOT_ALARM_DEATH_OF_MATE].trigger[0];

	MoverPtr myVehicle = getVehicle();
	if (myVehicle->getWatchID() == mateWID)
		return(NO_ERR);
	MoverPtr mateVehicle = static_cast<MoverPtr>(ObjectManager->getByWatchID(mateWID));
	if (!mateVehicle)
		return(-1);

	return(NO_ERR);
}

long MechWarrior::handleFriendlyVehicleCrippled (void) {

	return(NO_ERR);
}

long MechWarrior::handleFriendlyVehicleDestruction (void) {

	return(NO_ERR);
}

long MechWarrior::handleOwnVehicleIncapacitation (unsigned long cause) {

	MoverPtr myVehicle = getVehicle();
	Assert(myVehicle != nullptr, 0, " pilot has no vehicle ");

	switch (cause) {
		case 0:
		case 1:
		case 66:
			// Center Torso Internal Structure gone. Automatically ejects, if
			// can...
			myVehicle->handleEjection();
			break;
	}

	clearCurTacOrder(false);
	setOrderState(ORDERSTATE_GENERAL);
	std::array<MovePathPtr, 2> tempPath;
	for (long i = 0; i < 2; i++) {
		tempPath[i] = moveOrders.path[i];
		tempPath[i]->numSteps = 0;
	}
	moveOrders.init();
	PathManager->remove(this);
	moveOrders.path[0] = tempPath[0];
	moveOrders.path[1] = tempPath[1];
	attackOrders.init();
	setLastTarget(nullptr);

	// Let's let the unit know we're dying if we're a point...
	// No, Don't!!!
//	if (getPoint() == vehicle) {
//		GameObjectPtr newPointVehicle = getGroup()->selectPoint(true);
		// If there is no new point, all units must be blown away.
		// How do we want to handle this?
//	}

	return(NO_ERR);
}

long MechWarrior::handleOwnVehicleDestruction (unsigned long cause) {



	// Let's let the unit know we're dying if we're a point...
	// No, Don't!!!
//	if (getPoint() == myVehicle) {
//		GameObjectPtr newPointVehicle = getGroup()->selectPoint(true);
		// If there is no new point, all units must be blown away.
		// How do we want to handle this?
//	}

	return(NO_ERR);
}

long MechWarrior::handleOwnVehicleWithdrawn (void) {


	// Let's let the unit know we're dying if we're a point...
	// No, Don't!!!
//	if (getPoint() == myVehicle) {
//		GameObjectPtr newPointVehicle = getGroup()->selectPoint(true);
		// If there is no new point, all units must be blown away.
		// How do we want to handle this?
//	}

	setStatus(WARRIOR_STATUS_WITHDRAWN);

	return(NO_ERR);
}

long MechWarrior::handleAttackOrder (void) {

	return(NO_ERR);
}

long MechWarrior::handleFiredWeapon (void) {

	return(NO_ERR);
}

long MechWarrior::handleNewMover (void) {

	return(NO_ERR);
}

long MechWarrior::handleCollisionAlert (void) {

	return(NO_ERR);
}

long MechWarrior::handleKilledTarget (void) {

	unsigned long targetWID = alarm[PILOT_ALARM_KILLED_TARGET].trigger[0];
	GameObjectPtr target = ObjectManager->getByWatchID(targetWID);
	if (target) 
	{
		long message = -1;
		float skillIncrement = 0;
		long vehicleClass = -1;
		switch (target->getObjectClass()) 
		{
			case BATTLEMECH:
				if ((static_cast<MoverPtr>(target))->getMoveType() != MOVETYPE_AIR)	//Killing Helicopters does not count as a mech.
				{
					vehicleClass = std::to_underlying(target->getMechClass());
					skillIncrement = KillSkill[vehicleClass];
					numMechKills[vehicleClass][COMBAT_STAT_MISSION]++;
					message = RADIO_MECH_DEAD;
					if (numKilled < MAX_WARRIOR_KILLED)
						killed[numKilled++] = target->getWatchID();
				}
				else
				{
					vehicleClass = VEHICLE_CLASS_GROUND;
					skillIncrement = KillSkill[4];
					numMechKills[VEHICLE_CLASS_GROUND][COMBAT_STAT_MISSION]++;
					message = RADIO_VEHICLE_DEAD;
					if (numKilled < MAX_WARRIOR_KILLED)
						killed[numKilled++] = target->getWatchID();
				}
				break;

			case GROUNDVEHICLE:
			{
				MoverPtr gv = static_cast<MoverPtr>(target);
				if (gv->pathLocks)		//Killing Infantry/Powered Armor is meaningless!
				{
					vehicleClass = VEHICLE_CLASS_GROUND;
					skillIncrement = KillSkill[4];
					numMechKills[VEHICLE_CLASS_GROUND][COMBAT_STAT_MISSION]++;
					message = RADIO_VEHICLE_DEAD;
					if (numKilled < MAX_WARRIOR_KILLED)
						killed[numKilled++] = target->getWatchID();
				}
			}
			break;

			case TURRET:
			case GATE:
				message = RADIO_OBJECT_DEAD;
				break;

			case ELEMENTAL:
				vehicleClass = VEHICLE_CLASS_ELEMENTAL;
				skillIncrement = KillSkill[5];
				numMechKills[VEHICLE_CLASS_ELEMENTAL][COMBAT_STAT_MISSION]++;

			case BUILDING:
				message = RADIO_BUILDING_DEAD;
				break;

			default:
				message = RADIO_OBJECT_DEAD;
		}

		radioMessage(message);
		skillPoints[MWS_GUNNERY] += skillIncrement;
		if (MPlayer && MPlayer->isServer() && (vehicleClass != -1))
			MPlayer->addPilotKillStat(getVehicle(), vehicleClass);
	}

	return(NO_ERR);
}

long MechWarrior::handleUnitMateFiredWeapon (void) {


	return(NO_ERR);
}

long MechWarrior::handlePlayerOrder (void) {

	if ((getVehicleStatus() == OBJECT_STATUS_SHUTDOWN) && (curTacOrder.code != TACTICAL_ORDER_POWERDOWN))
		orderPowerUp(false, ORDER_ORIGIN_SELF);

	return(NO_ERR);
}

long MechWarrior::handleNoMovePath (void) {
	if (getCurTacOrder()->code == TACTICAL_ORDER_GETFIXED)
	{
		clearCurTacOrder();
		radioMessage(RADIO_MOVE_BLOCKED);
	}
	return(NO_ERR);	
}

long MechWarrior::handleGateClosing (void) {

/*
	// Do this here for now... but, we may want to put it into the
	// player brain instead and allow kamikazee pilots :)
	for (long i = 0; i < alarm[PILOT_ALARM_GATE_CLOSING].numTriggers; i++) {
		long gateTileRow = alarm[PILOT_ALARM_GATE_CLOSING].trigger[i] / MAX_MAPWIDTH;
		long gateTileCol = alarm[PILOT_ALARM_GATE_CLOSING].trigger[i] % MAX_MAPWIDTH;
		long overlayType = GameMap->getOverlayType(gateTileRow, gateTileCol);
		bool check = true;
		if (alignment == CLANS)
			check = ((overlayType == OVERLAY_GATE_IS_NS_CLOSED) || (overlayType == OVERLAY_GATE_IS_EW_CLOSED));
		else
			check = ((overlayType == OVERLAY_GATE_CLAN_NS_CLOSED) || (overlayType == OVERLAY_GATE_IS_EW_CLOSED));
		if (check && getMovePath()) {
			long firstStep = getMovePath()->crossesTile(-1, 100, gateTileRow, gateTileCol);
			if (firstStep > 1) {
				//setMoveGoal(MOVEGOAL_NONE, NULL);
				setMoveWayPath(0);
				for (long i = 0; i < 2; i++)
					clearMovePath(i);
				setMoveState(MOVESTATE_FORWARD);
				setMoveStateGoal(MOVESTATE_FORWARD);
				setMoveYieldTime(-1.0);
				setMoveYieldState(0);
				setMoveWaitForPointTime(-1.0);
				setMoveTimeOfLastStep(-1.0);
				setMoveTwisting(false);
				setMoveGlobalPath(NULL, 0);
				PathManager->remove(this);
				break;
			}
		}
	}
*/
	return(NO_ERR);
}

long MechWarrior::missionLog (FilePtr file, long unitLevel) 
{
	return(0);
}

void MechWarrior::calcRank (void)
{
	float skillAverage = skillRank[MWS_GUNNERY] + skillRank[MWS_PILOTING];
	skillAverage /= 2.0f;

	float avg = skillAverage;

	if ( avg > 79.f )
		rank = WARRIOR_RANK_ACE;
	else if ( avg > 70.f )
		rank = WARRIOR_RANK_ELITE;
	else if ( avg > 60.f )
		rank = WARRIOR_RANK_VETERAN;
	else if ( avg > 50.f )
		rank = WARRIOR_RANK_REGULAR;
	else
		rank = WARRIOR_RANK_GREEN;
}

void MechWarrior::updateMissionSkills()
{
	// DO NOT CALL THIS IF THE MISSION FAILED!

	float MaxPilotSkill = 80.f;
	
	skillRank[MWS_GUNNERY] += skillPoints[MWS_GUNNERY] * 0.01f;
	skillRank[MWS_GUNNERY] = std::min(skillRank[MWS_GUNNERY], MaxPilotSkill);

	skillRank[MWS_PILOTING]++;		//One free point just for coming along!

	skillRank[MWS_PILOTING] += skillPoints[MWS_PILOTING];
	skillRank[MWS_PILOTING] = std::min(skillRank[MWS_PILOTING], MaxPilotSkill);
}
