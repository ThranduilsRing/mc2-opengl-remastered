// MoverGroup: handleTacticalOrder, the order* wrappers and the mate-event handlers.
// Declared in the class headers; shared names in group_internal.h.
#include "group_internal.h"


//---------------------------------------------------------------------------

long MoverGroup::handleTacticalOrder (TacticalOrder tacOrder, long priority, Stuff::Vector3D* jumpGoalList, bool queueGroupOrder) {

	if (numMovers == 0)
		return(NO_ERR);

	if (queueGroupOrder)
		tacOrder.pack(nullptr, nullptr);

	//bool processOrder = true;
	bool isJump = false;
	bool isMove = false;
	Stuff::Vector3D goalList[MAX_MOVERGROUP_COUNT];
	Stuff::Vector3D location = tacOrder.getWayPoint(0);
	//MoverPtr pointVehicle = getPoint();

	if (tacOrder.code == TACTICAL_ORDER_ATTACK_OBJECT)
		if (tacOrder.attackParams.method == ATTACKMETHOD_DFA) {
			//-------------------------------------------------
			// Let's just make it a move/jump order, for now...
			tacOrder.code = TACTICAL_ORDER_JUMPTO_OBJECT;
			tacOrder.moveParams.wait = false;
			tacOrder.moveParams.wayPath.mode[0] = TRAVEL_MODE_SLOW;
			GameObjectPtr target = ObjectManager->getByWatchID(tacOrder.targetWID);
			Assert(tacOrder.targetWID != 0, 0, " DFA AttackObject WID is 0 ");
			if (!target)
				return(NO_ERR);
			tacOrder.setWayPoint(0, target->getPosition());
		}
	if (tacOrder.code == TACTICAL_ORDER_JUMPTO_OBJECT) {
		tacOrder.code = TACTICAL_ORDER_JUMPTO_POINT;
		GameObjectPtr target = ObjectManager->get(tacOrder.targetWID);
		Assert(tacOrder.targetWID != 0, 0, " DFA AttackObject WID is 0 ");
		if (!target)
			return(NO_ERR);
		tacOrder.setWayPoint(0, target->getPosition());
	}

	//vector_3d offsetTable[MAX_GROUPMOVE_OFFSETS];
	//long numOffsets = 0;

	switch (tacOrder.code) {
		case TACTICAL_ORDER_WAIT:
			break;

		case TACTICAL_ORDER_MOVETO_POINT:
		case TACTICAL_ORDER_MOVETO_OBJECT: {
Fatal(0, "Need to support jumpGoalList (and goalList) for MOVETO as well in mc2 ");
			isMove = true;
			//-----------------------------------------------------------
			// Sort by distance to destination. Their selectionIndex will
			// be set to modify this goal...
			SortListPtr list = Mover::sortList;
			if (list) {
				list->clear(false);
				long moverCount = 0;
				for (long i = 0; i < numMovers; i++) {
					MoverPtr mover = getMover(i);
					Assert(mover != nullptr, moverWIDs[i], " MoverGroup.handleTacticalOrder: NULL mover ");
					if (!mover->isDisabled()) {
						list->setId(moverCount, i);
						list->setValue(moverCount, mover->distanceFrom(location));
						moverCount++;
					}
				}
				list->sort(false);
				//--------------------------------
				// Let's build the offset table...
				/*
				numOffsets = moverCount - 1;
				if (numOffsets > MAX_GROUPMOVE_OFFSETS)
					numOffsets = MAX_GROUPMOVE_OFFSETS;
				long offsetsStart = GroupMoveOffsetsIndex[numOffsets - 1];
				for (i = 0; i < numOffsets; i++)
					offsetTable[i] = relativePositionToPoint(location, GroupMoveOffsets[offsetsStart + i][0], GroupMoveOffsets[offsetsStart + i][1], RELPOS_FLAG_PASSABLE_START);
				*/
				//-----------------------------------
				// Now, calc the order of movement...
				long curIndex = 1;
				for (int i = 0; i < moverCount; i++) {
					MoverPtr mover = getMover(list->getId(i));
					if (mover->getWatchID() == pointWID)
						mover->selectionIndex = 0;
					else
						mover->selectionIndex = curIndex++;
				}
			}
			}
			break;

		case TACTICAL_ORDER_JUMPTO_POINT:
		case TACTICAL_ORDER_JUMPTO_OBJECT: {
			//-----------------------------------------------------------
			// Sort by distance to destination. Their selectionIndex will
			// be set to modify this goal...
			isJump = true;
			//-------------------------------------------------------------------------
			// We can assume that all movers in this group are jump-capable. Otherwise,
			// the group wouldn't be allowed to jump by the interface. In addition,
			// we KNOW that all movers in this group can jump to the selected
			// goal (of course, they won't due to terrain and crowding)...
			GameObjectPtr target = ObjectManager->getByWatchID(tacOrder.targetWID);
			if (jumpGoalList)
				for (long j = 0; j < numMovers; j++)
					goalList[j] = jumpGoalList[j];
			else
				calcJumpGoals(tacOrder.getWayPoint(0), goalList, target);

			for (long i = 0; i < numMovers; i++) {
				MoverPtr mover = getMover(i);
				bool canJump = (goalList[i].x > -99000.0);
				if (canJump)
					mover->selectionIndex = 0;
				else
					mover->selectionIndex = -2;
			}
			}
			break;

		case TACTICAL_ORDER_TRAVERSE_PATH:
		case TACTICAL_ORDER_PATROL_PATH:
		case TACTICAL_ORDER_ESCORT:
		case TACTICAL_ORDER_FOLLOW:
		case TACTICAL_ORDER_GUARD:
		case TACTICAL_ORDER_STOP:
		case TACTICAL_ORDER_POWERUP:
		case TACTICAL_ORDER_POWERDOWN:
		case TACTICAL_ORDER_WAYPOINTS_DONE:
		case TACTICAL_ORDER_EJECT:
		case TACTICAL_ORDER_ATTACK_OBJECT:
		case TACTICAL_ORDER_ATTACK_POINT:
		case TACTICAL_ORDER_HOLD_FIRE:
		case TACTICAL_ORDER_WITHDRAW:
		case TACTICAL_ORDER_CAPTURE:
		case TACTICAL_ORDER_LOAD_INTO_CARRIER:
		case TACTICAL_ORDER_REFIT:
		case TACTICAL_ORDER_RECOVER:
		case TACTICAL_ORDER_GETFIXED:
			break;

		default: {
			char s[256];
			sprintf(s, "Unit::handleTacticalOrder->Bad TacOrder Code (%d)", tacOrder.code);
			Assert(false, tacOrder.code, s);
			return(1);
			}
	}

	tacOrder.unitOrder = true;
	for (long i = 0; i < numMovers; i++) {
		MoverPtr mover = getMover(i);
		if (mover && !mover->isDisabled()) {
			if (mover->selectionIndex == -2) {
				mover->selectionIndex = -1;
				continue;
			}
			tacOrder.selectionIndex = mover->selectionIndex;
			if (tacOrder.selectionIndex != -1) {
				if (isMove)
					tacOrder.setWayPoint(0, location);
				else if (isJump)
					tacOrder.setWayPoint(0, goalList[i]);
				tacOrder.delayedTime = scenarioTime + (mover->selectionIndex * DelayedOrderTime);
			}
			switch (tacOrder.origin) {
				case ORDER_ORIGIN_PLAYER:
					if (queueGroupOrder)
						mover->getPilot()->addQueuedTacOrder(tacOrder);
					else {
						if (mover->getPilot()->getNumTacOrdersQueued())
							//This is a hack to simply trigger the execution of
							//the queued orders. The current order is ignored (and
							//is simply used for this trigger)...
							mover->getPilot()->executeTacOrderQueue();
						else
							mover->getPilot()->setPlayerTacOrder(tacOrder);
					}
					break;
				case ORDER_ORIGIN_COMMANDER:
					mover->getPilot()->setGeneralTacOrder(tacOrder);
					break;
				case ORDER_ORIGIN_SELF:
					mover->getPilot()->setAlarmTacOrder(tacOrder, priority);
					break;
			}
			mover->selectionIndex = -1;
		}
	}

	return(NO_ERR);
}

//---------------------------------------------------------------------------
// TAC ORDERS
//---------------------------------------------------------------------------

long MoverGroup::orderMoveToPoint (bool setTacOrder, long origin, Stuff::Vector3D location, unsigned long params) {

	long result = TACORDER_FAILURE;

	for (long i = 0; i < numMovers; i++) {
		Assert(getMover(i) != nullptr, 0, " MoverGroup.orderMoveToPoint: NULL mover ");
		MechWarriorPtr pilot = getMover(i)->getPilot();
		if (pilot)
			result = pilot->orderMoveToPoint(true, setTacOrder, origin, location, -1, params);
	}

	return(result);
}

//---------------------------------------------------------------------------

long MoverGroup::orderMoveToObject (bool setTacOrder, long origin, GameObjectPtr target, long fromArea, unsigned long params) {

	long result = TACORDER_FAILURE;

	for (long i = 0; i < numMovers; i++) {
		Assert(getMover(i) != nullptr, 0, " MoverGroup.orderMoveToObject: NULL mover ");
		MechWarriorPtr pilot = getMover(i)->getPilot();
		if (pilot)
			result = pilot->orderMoveToObject(true, setTacOrder, origin, target, fromArea, -1, params);
	}

	return(result);
}

//---------------------------------------------------------------------------

long MoverGroup::orderTraversePath (long origin, WayPathPtr wayPath, unsigned long params) {

	long result = TACORDER_FAILURE;

	for (long i = 0; i < numMovers; i++) {
		Assert(getMover(i) != nullptr, 0, " MoverGroup.orderTraversePath: NULL mover ");
		MechWarriorPtr pilot = getMover(i)->getPilot();
		if (pilot)
			result = pilot->orderTraversePath(true, true, origin, wayPath, params);
	}

	return(result);
}

//---------------------------------------------------------------------------

long MoverGroup::orderPatrolPath (long origin, WayPathPtr wayPath) {

	long result = TACORDER_FAILURE;

	for (long i = 0; i < numMovers; i++) {
		Assert(getMover(i) != nullptr, 0, " MoverGroup.orderPatrolPath: NULL mover ");
		MechWarriorPtr pilot = getMover(i)->getPilot();
		if (pilot)
			result = pilot->orderPatrolPath(true, true, origin, wayPath);
	}

	return(result);
}

//---------------------------------------------------------------------------

long MoverGroup::orderPowerDown (long origin) {

	long result = TACORDER_FAILURE;

	for (long i = 0; i < numMovers; i++) {
		Assert(getMover(i) != nullptr, 0, " MoverGroup.orderPowerDown: NULL mover ");
		MechWarriorPtr pilot = getMover(i)->getPilot();
		if (pilot)
			result = pilot->orderPowerDown(true, origin);
	}

	return(result);
}

//---------------------------------------------------------------------------

long MoverGroup::orderPowerUp (long origin) {

	long result = TACORDER_FAILURE;

	for (long i = 0; i < numMovers; i++) {
		Assert(getMover(i) != nullptr, 0, " MoverGroup.orderPowerUp: NULL mover ");
		MechWarriorPtr pilot = getMover(i)->getPilot();
		if (pilot)
			result = pilot->orderPowerUp(true, origin);
	}

	return(result);
}

//---------------------------------------------------------------------------

long MoverGroup::orderAttackObject (long origin, GameObjectPtr target, long attackType, long attackMethod, long attackRange, long aimLocation, long fromArea, unsigned long params) {

	long result = TACORDER_FAILURE;

	for (long i = 0; i < numMovers; i++) {
		Assert(getMover(i) != nullptr, 0, " MoverGroup.orderAttackObject: NULL mover ");
		MechWarriorPtr pilot = getMover(i)->getPilot();
		if (pilot)
			result = pilot->orderAttackObject(true, origin, target, attackType, attackMethod, attackRange, aimLocation, fromArea, params);
	}

	return(result);
}

//---------------------------------------------------------------------------

long MoverGroup::orderWithdraw (long origin, Stuff::Vector3D location) {

	long result = TACORDER_FAILURE;

	for (long i = 0; i < numMovers; i++) {
		Assert(getMover(i) != nullptr, 0, " MoverGroup.orderWithdraw: NULL mover ");
		MechWarriorPtr pilot = getMover(i)->getPilot();
		if (pilot)
			result = pilot->orderWithdraw(true, origin, location);
	}

	return(result);
}

//---------------------------------------------------------------------------

long MoverGroup::orderEject (long origin) {

	long result = TACORDER_FAILURE;

	for (long i = 0; i < numMovers; i++) {
		Assert(getMover(i) != nullptr, 0, " MoverGroup.orderEject: NULL mover ");
		MechWarriorPtr pilot = getMover(i)->getPilot();
		if (pilot)
			result = pilot->orderEject(true, true, origin);
	}

	return(result);
}

//---------------------------------------------------------------------------
// COMBAT EVENTS
//---------------------------------------------------------------------------

void MoverGroup::triggerAlarm (long alarmCode, unsigned long triggerId) {

	for (long i = 0; i < numMovers; i++) {
		MechWarriorPtr pilot = getMover(i)->getPilot();
		if (pilot)
			pilot->triggerAlarm(alarmCode, triggerId);
	}
}

//---------------------------------------------------------------------------

long MoverGroup::handleMateCrippled (unsigned long mateWID) {

	triggerAlarm(PILOT_ALARM_FRIENDLY_VEHICLE_CRIPPLED, mateWID);
	return(NO_ERR);
}

//---------------------------------------------------------------------------

long MoverGroup::handleMateDisabled (unsigned long mateWID) {

	//triggerAlarm(PILOT_ALARM_DEATH_OF_MATE, mateId);
	return(NO_ERR);
}

//---------------------------------------------------------------------------

long MoverGroup::handleMateDestroyed (unsigned long mateWID) {

	triggerAlarm(PILOT_ALARM_DEATH_OF_MATE, mateWID);
	return(NO_ERR);
}

//---------------------------------------------------------------------------

long MoverGroup::handleMateEjected (unsigned long mateWID) {

	//triggerAlarm(PILOT_ALARM_DEATH_OF_MATE, mateId);
	return(NO_ERR);
}

//---------------------------------------------------------------------------

void MoverGroup::handleMateFiredWeapon (unsigned long mateWID) {

	triggerAlarm(PILOT_ALARM_MATE_FIRED_WEAPON, mateWID);
}
