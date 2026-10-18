// MoverGroup: the order* wrappers and the mate-event handlers.
// Declared in the class headers; shared names in group_internal.h.
#include "group_internal.h"
#include "mc2fmt.h"


//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TAC ORDERS
//---------------------------------------------------------------------------

long MoverGroup::orderMoveToPoint (bool setTacOrder, long origin, glm::vec3 location, unsigned long params) {

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

long MoverGroup::orderWithdraw (long origin, glm::vec3 location) {

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
