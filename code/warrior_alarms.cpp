// MechWarrior: tactical-order switching, pilot alarms and event history.
// Declared in the class headers; shared names in warrior_internal.h.
#include "warrior_internal.h"
#include "mission.h"
#include "mc2fmt.h"
#include "Gates/gate.h"



bool absorbsGeneralOrder(MechWarrior* pilot, long origin) {
    // ABL reissues cannot steer the standing route, even during an alarm.
    return origin == ORDER_ORIGIN_COMMANDER && mission && mission->ownedRoute(pilot);
}

void suspendAuthoredRoute(MechWarrior* warrior) {
    if (mission) if (auto* r = mission->ownedRoute(warrior))
        mission->signalRoute(*r, mc2::sim::RouteAction::Alarm);
}

void MechWarrior::clearCurTacOrder (bool updateTacOrder) {

	if (curTacOrder.isCombatOrder())
	{
		numWarriorsInCombat--;
	}

	gosASSERT(numWarriorsInCombat >= 0);

	if (getVehicle())
		getVehicle()->suppressionFire = 0;

	curTacOrder.init();
	clearMoveOrders();
	triggerAlarm(PILOT_ALARM_NO_MOVEPATH, MOVEPATH_ERR_TACORDER_CLEARED);
	clearAttackOrders();
	lastTacOrder.lastTime = scenarioTime;

	if (updateTacOrder) {
		TacticalOrder newTacOrder;
		switch (orderState) {
			case ORDERSTATE_GENERAL:
				if (!newTacOrderReceived[ORDERSTATE_GENERAL])
					tacOrder[ORDERSTATE_GENERAL].init();
				break;
			case ORDERSTATE_PLAYER:
				if (!newTacOrderReceived[ORDERSTATE_PLAYER]) {
					tacOrder[ORDERSTATE_PLAYER].init();
					curPlayerOrderFromQueue = false;
				}
				newTacOrder = tacOrder[ORDERSTATE_GENERAL];
				orderState = ORDERSTATE_GENERAL;
				break;
			case ORDERSTATE_ALARM:
				if (!newTacOrderReceived[ORDERSTATE_ALARM])
					tacOrder[ORDERSTATE_ALARM].init();
				alarmPriority = 0;
				if (tacOrder[ORDERSTATE_PLAYER].code != TACTICAL_ORDER_NONE) {
					newTacOrder = tacOrder[ORDERSTATE_PLAYER];
					orderState = ORDERSTATE_PLAYER;
					}
				else {
					newTacOrder = tacOrder[ORDERSTATE_GENERAL];
					orderState = ORDERSTATE_GENERAL;
				}
				break;

		}
		if (newTacOrder.code != TACTICAL_ORDER_NONE) {
			Assert((moveOrders.path[0] != nullptr) && (moveOrders.path[1] != nullptr), 0, " bad warrior path ");
			MovePathPtr tempPath[2];
			for (long i = 0; i < 2; i++) {
				tempPath[i] = moveOrders.path[i];
				tempPath[i]->numSteps = 0;
			}
			long tempMoveState = moveOrders.moveState;
			long tempMoveStateGoal = moveOrders.moveStateGoal;
			moveOrders.init();
			PathManager->remove(this);
			moveOrders.moveState = tempMoveState;
			moveOrders.moveStateGoal = tempMoveStateGoal;
			moveOrders.path[0] = tempPath[0];
			moveOrders.path[1] = tempPath[1];
			attackOrders.init();
			long message = -1;
			newTacOrder.execute(this, message);
			if (orderState == ORDERSTATE_PLAYER)
				radioMessage(message, true);
			setCurTacOrder(newTacOrder);
			if (useGoalPlan) {
				glm::vec3 movePoint = newTacOrder.getWayPoint(0);
				if (!newTacOrder.subOrder)
					switch (newTacOrder.code) {
						case TACTICAL_ORDER_MOVETO_POINT:
							setMainGoal(GOAL_ACTION_MOVE, nullptr, &movePoint, -1.0);
							break;
						case TACTICAL_ORDER_ATTACK_OBJECT:
							setMainGoal(GOAL_ACTION_ATTACK, newTacOrder.getTarget(), nullptr, -1.0);
							break;
						case TACTICAL_ORDER_CAPTURE:
							setMainGoal(GOAL_ACTION_CAPTURE, newTacOrder.getTarget(), nullptr, 400.0);
							break;
						default:
							setMainGoal(GOAL_ACTION_NONE, nullptr, nullptr, -1.0);
					}
			}
			timeOfLastOrders = -1.0;
		}
		// These must be cleared every frame...
		newTacOrderReceived[ORDERSTATE_GENERAL] = false;
		newTacOrderReceived[ORDERSTATE_PLAYER] = false;
		newTacOrderReceived[ORDERSTATE_ALARM] = false;
	}

}

bool MechWarrior::anyPlayerInCombat (void)
{
	for (auto & i : warriorList)
	{
		if (i && 
			(i->getVehicle() && (i->getVehicle()->getCommanderId() == Commander::home->getId())) &&
			(i->getCurrentTarget()))
		{
			return true;
		}
	}
	
	return false;
}

void MechWarrior::setCurTacOrder (TacticalOrder newTacOrder) {

	curTacOrder = newTacOrder;
	lastTacOrder = newTacOrder;
	if (curTacOrder.isCombatOrder())
	{
		numWarriorsInCombat++;
	}
}

void MechWarrior::setGeneralTacOrder (TacticalOrder newTacOrder) {

    if (mission) if (auto* r = mission->ownedRoute(this)) {
        if (!r->generalWriteDiagnosed) {
            mc2::fmt::print_err("[ROUTE_EXECUTION v1] general_write_absorbed execution={}\n", r->key.executionId);
            r->generalWriteDiagnosed = true;
        }
        return;
    }
	tacOrder[ORDERSTATE_GENERAL] = newTacOrder;
	newTacOrderReceived[ORDERSTATE_GENERAL] = true;
	// MOD-HEALTH-1: first AI/script-issued general order (tools/mod_smoke.py health check).
	static bool s_firstGeneralOrder = false;
	if (!s_firstGeneralOrder && mc2::gate::get<"MC2_MOD_HEALTH_DIAG">()) {
		s_firstGeneralOrder = true;
		mc2::fmt::print_out("[MOD_HEALTH v1] event=first_general_order scenarioTime={:.2f}\n",
			static_cast<double>(scenarioTime));
		fflush(stdout);
	}
}

void MechWarrior::setPlayerTacOrder (TacticalOrder newTacOrder, bool fromQueue) {

    if (mission) if (auto* r = mission->ownedRoute(this))
        mission->signalRoute(*r, mc2::sim::RouteAction::PlayerOrder);
	tacOrder[ORDERSTATE_PLAYER] = newTacOrder;
	newTacOrderReceived[ORDERSTATE_PLAYER] = true;
	curPlayerOrderFromQueue = fromQueue;
	if (!fromQueue)
		clearTacOrderQueue();
}

void MechWarrior::setAlarmTacOrder (TacticalOrder newTacOrder, long priority) {

	if (priority >= alarmPriority) {
		tacOrder[ORDERSTATE_ALARM] = newTacOrder;
		newTacOrderReceived[ORDERSTATE_ALARM] = true;
		alarmPriority = priority;
	}
}

long MechWarrior::triggerAlarm (long alarmCode, unsigned long triggerId) {

	if (alarm[alarmCode].numTriggers == MAX_ALARM_TRIGGERS)
		return(-1);
	if (alarmHistory[alarmCode].numTriggers == MAX_ALARM_TRIGGERS)
		return(-1);

	alarm[alarmCode].trigger[alarm[alarmCode].numTriggers++] = triggerId;
	alarmHistory[alarmCode].trigger[alarmHistory[alarmCode].numTriggers++] = triggerId;
	return(NO_ERR);
}

long MechWarrior::getEventHistory (long alarmCode, int* paramList) {

	long numValidTriggers = 0;
	switch (alarmCode) {
		case PILOT_ALARM_ATTACK_ORDER:
		case PILOT_ALARM_DEATH_OF_MATE:
		case PILOT_ALARM_KILLED_TARGET:
		case PILOT_ALARM_FRIENDLY_VEHICLE_CRIPPLED:
		case PILOT_ALARM_VEHICLE_DESTROYED:
		case PILOT_ALARM_MATE_FIRED_WEAPON:
		case PILOT_ALARM_COLLISION:
		case PILOT_ALARM_TARGET_OF_WEAPONFIRE:
		case PILOT_ALARM_FIRED_WEAPON:
		{
			for (long i = 0; i < alarmHistory[alarmCode].numTriggers; i++) 
			{
				GameObjectPtr obj = ObjectManager->getByWatchID(alarmHistory[alarmCode].trigger[i]);
				if (obj)
					paramList[numValidTriggers++] = obj->getPartId();
			}
		}
		break;
		case PILOT_ALARM_HIT_BY_WEAPONFIRE: {
			for (long i = 0; i < alarmHistory[alarmCode].numTriggers; i++)
				if (alarmHistory[alarmCode].trigger[i] < 0)
					paramList[numValidTriggers++] = alarmHistory[alarmCode].trigger[i];
				else {
					GameObjectPtr obj = ObjectManager->getByWatchID(alarmHistory[alarmCode].trigger[i]);
					if (obj)
						paramList[numValidTriggers++] = obj->getPartId();
				}
			}
			break;
		default:
			for (long i = 0; i < alarmHistory[alarmCode].numTriggers; i++)
				paramList[i] = alarmHistory[alarmCode].trigger[i];
			numValidTriggers = alarmHistory[alarmCode].numTriggers;
			break;
	}
	return(numValidTriggers);
}

MechWarrior::EventHistoryResult MechWarrior::getNextEventHistory (int* paramList) {

	if (curEventID < NUM_PILOT_ALARMS) {
		while ((curEventID < NUM_PILOT_ALARMS) && (alarmHistory[curEventID].numTriggers == 0))
			curEventID++;
		if (curEventID < NUM_PILOT_ALARMS) {
			const long count = getEventHistory(curEventID, paramList);
			curEventID++;
			return {curEventID - 1, count};
		}
	}
	return {};
}

long MechWarrior::handleAlarm (long alarmCode, unsigned long triggerId) {

	MoverPtr myVehicle = getVehicle();
	Assert(myVehicle != nullptr, 0, " bad vehicle for pilot ");

	if (!myVehicle->getAwake())
		return(NO_ERR);

	// Handle Alarm Events. Only three alarms may be called outside the
	// normal pilot alarm procedure: basically, the three that cause the
	// mover to be wiped from the game before the normal pilot alarm
	// update would be called.
	switch (alarmCode) {
		case PILOT_ALARM_VEHICLE_INCAPACITATED:
			// NOTE: This will never happen, since a disabled mech won't
			// get into this routine. The alarm is handled IMMEDIATELY
			// when it happens...
			handleOwnVehicleIncapacitation(triggerId);
			break;
		case PILOT_ALARM_VEHICLE_DESTROYED:
			// NOTE: This will never happen, since a destroyed mech won't
			// get into this routine. The alarm is handled IMMEDIATELY
			// when it happens...
			handleOwnVehicleDestruction(triggerId);
			break;
		case PILOT_ALARM_VEHICLE_WITHDRAWN:
			// NOTE: This will never happen, since a withdrawn mech won't
			// get into this routine. The alarm is handled IMMEDIATELY
			// when it happens...
			handleOwnVehicleWithdrawn();
			break;
	}

	if (MPlayer && !MPlayer->isServer())
		return(NO_ERR);

	if (brainAlarmCallback[alarmCode]) {
		CurGroup = getGroup();
		CurObject = static_cast<GameObjectPtr>(getVehicle());
		CurObjectClass = getVehicle()->getObjectClass();
		CurWarrior = this;
		CurContact = nullptr;
		CurAlarm = alarmCode;

		brain->execute(nullptr, brainAlarmCallback[alarmCode]);
		
		CurGroup = nullptr;
		CurObject = nullptr;
		CurObjectClass = 0;
		CurWarrior = nullptr;
		CurContact = nullptr;
	}

	return(NO_ERR);
}

int32_t MechWarrior::getAlarmTriggers (int alarmCode, int32_t* triggerList) {

    ArrayCopy(triggerList, alarm[alarmCode].trigger, alarm[alarmCode].numTriggers);
	return(alarm[alarmCode].numTriggers);
}

int32_t MechWarrior::getAlarmTriggersHistory (int32_t alarmCode, int32_t* triggerList) {

    ArrayCopy(triggerList, alarmHistory[alarmCode].trigger, alarmHistory[alarmCode].numTriggers);
	return(alarmHistory[alarmCode].numTriggers);
}

void MechWarrior::clearAlarm (long alarmCode) {

	alarm[alarmCode].numTriggers = 0;
}

void MechWarrior::clearAlarms (void) {

	for (long code = 0; code < NUM_PILOT_ALARMS; code++)
		clearAlarm(code);
}

void MechWarrior::clearAlarmHistory (long alarmCode) {

	alarmHistory[alarmCode].numTriggers = 0;
}

void MechWarrior::clearAlarmsHistory (void) {

	for (long code = 0; code < NUM_PILOT_ALARMS; code++)
		clearAlarmHistory(code);
}

long MechWarrior::checkAlarms (void) {

	if (brain) {
		CurGroup = getGroup();
		CurObject = static_cast<GameObjectPtr>(getVehicle());
		CurObjectClass = getVehicle()->getObjectClass();
		CurWarrior = this;
		CurContact = nullptr;
	}

	for (long code = 0; code < NUM_PILOT_ALARMS; code++)
		if (alarm[code].numTriggers > 0) {
			switch (code) {
				case PILOT_ALARM_TARGET_OF_WEAPONFIRE:
					handleTargetOfWeaponFire();
					break;
				case PILOT_ALARM_HIT_BY_WEAPONFIRE:
					handleHitByWeaponFire();
					break;
#ifdef USEHEAT
				case PILOT_ALARM_OVERHEAT:
					handleOverHeat();
					break;
#endif

				case PILOT_ALARM_DAMAGE_TAKEN_RATE:
					handleDamageTakenRate();
					break;
				case PILOT_ALARM_DEATH_OF_MATE:
					handleUnitMateDeath();
					break;
				case PILOT_ALARM_FRIENDLY_VEHICLE_CRIPPLED:
					handleFriendlyVehicleCrippled();
					break;
				case PILOT_ALARM_FRIENDLY_VEHICLE_DESTROYED:
					handleFriendlyVehicleDestruction();
					break;
				case PILOT_ALARM_VEHICLE_INCAPACITATED:
					// NOTE: This will never happen, since a disabled mech won't
					// get into this routine. Thei alarm is handled IMMEDIATELY
					// when it happens...
					handleOwnVehicleIncapacitation(0);
					break;
				case PILOT_ALARM_VEHICLE_DESTROYED:
					// NOTE: This will never happen, since a disabled mech won't
					// get into this routine. Thei alarm is handled IMMEDIATELY
					// when it happens...
					handleOwnVehicleDestruction(0);
					break;
				case PILOT_ALARM_VEHICLE_WITHDRAWN:
					// NOTE: This will never happen, since a disabled mech won't
					// get into this routine. Thei alarm is handled IMMEDIATELY
					// when it happens...
					handleOwnVehicleWithdrawn();
					break;
				case PILOT_ALARM_ATTACK_ORDER:
					handleAttackOrder();
					break;
				case PILOT_ALARM_COLLISION:
					handleCollision();
					break;
				case PILOT_ALARM_KILLED_TARGET:
					handleKilledTarget();
					break;
				case PILOT_ALARM_MATE_FIRED_WEAPON:
					handleUnitMateFiredWeapon();
					break;
				case PILOT_ALARM_PLAYER_ORDER:
					handlePlayerOrder();
					break;
				case PILOT_ALARM_NO_MOVEPATH:
					handleNoMovePath();
					break;
				case PILOT_ALARM_GATE_CLOSING:
					handleGateClosing();
					break;
				case PILOT_ALARM_FIRED_WEAPON:
					handleFiredWeapon();
					break;
				case PILOT_ALARM_NEW_MOVER:
					handleNewMover();
					break;
			}
			if (MPlayer) {
				if (MPlayer->isServer())
					if (brain && brainAlarmCallback[code]) {
						CurAlarm = code;
						brain->execute(nullptr, brainAlarmCallback[code]);
					}
				}
			else {
				if (brain && brainAlarmCallback[code]) {
					CurAlarm = code;
					brain->execute(nullptr, brainAlarmCallback[code]);
				}
			}
			// Clear these alarms every frame...
			alarm[code].numTriggers = 0;
		}

	if (brain) {
		CurGroup = nullptr;
		CurObject = nullptr;
		CurObjectClass = 0;
		CurWarrior = nullptr;
		CurContact = nullptr;
	}

	return(NO_ERR);
}
