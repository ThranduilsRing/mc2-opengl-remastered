// ABL native bindings: movement/attack orders and the core* tactical order bindings.
// Declared in ablmc2_internal.h; registered, in order, by initABL in ablmc2.cpp.
#include "ablmc2_internal.h"
#include "abl_script_events.h"
#include "../GameOS/gameos/diagnostic_trace.h"
#include <algorithm>
#include <iterator>
#include <span>

void execGetObjects (void) {

	//	GET OBJECTS function:
	//		Returns number and list of unit mates of the specified object.
	//		PARAMS:	integer			object criteria
	//				integer[]		object list
	//		RETURN: integer			num objects in unit

	long criteria = ABLi_popInteger();
	const auto output = ABLi_popIntBuf();

	size_t numObjects = 0;
	const auto append = [&](int id) {
		if (numObjects < output.size()) output[numObjects] = id;
		++numObjects;
	};
	if (!ObjectManager) { ABLi_pushInteger(0); return; }
	switch (criteria) {
		case 0: {
			// MOVERS
			long listSize = ObjectManager->getNumMovers();
			for (long i = 0; i < listSize; i++) {
				MoverPtr mover = ObjectManager->getMover(i);
				if (mover && mover->getExists())
					append(mover->getPartId());
			}
			}
			break;
		case 1: {
			// TERRAIN OBJECTS
			long listSize = ObjectManager->getNumTerrainObjects();
			for (long i = 0; i < listSize; i++) {
				TerrainObjectPtr terObj = ObjectManager->getTerrainObject(i);
				if (terObj) {
					append(terObj->getPartId());
				}
			}
			}
			break;
		case 2: {
			// BUILDINGS
			long listSize = ObjectManager->getNumBuildings();
			for (long i = 0; i < listSize; i++) {
				BuildingPtr building = ObjectManager->getBuilding(i);
				if (building) {
					append(building->getPartId());
				}
			}
			}
			break;
		case 3: {
			// TURRETS
			long listSize = ObjectManager->getNumTurrets();
			for (long i = 0; i < listSize; i++) {
				TurretPtr turret = ObjectManager->getTurret(i);
				if (turret) {
					append(turret->getPartId());
				}
			}
			}
			break;
		case 4: {
			// GATES
			long listSize = ObjectManager->getNumGates();
			for (long i = 0; i < listSize; i++) {
				GatePtr gate = ObjectManager->getGate(i);
				if (gate) {
					append(gate->getPartId());
				}
			}
			}
			break;
		case 5: {
			// ALL TERRAIN OBJECTS, including buildings, turrets and gates
			long listSize = ObjectManager->getNumTerrainObjects();
			for (long i = 0; i < listSize; i++) {
				TerrainObjectPtr terObj = ObjectManager->getTerrainObject(i);
				if (terObj) {
					append(terObj->getPartId());
				}
			}
			listSize = ObjectManager->getNumBuildings();
			for (int i = 0; i < listSize; i++) {
				BuildingPtr building = ObjectManager->getBuilding(i);
				if (building) {
					append(building->getPartId());
				}
			}
			listSize = ObjectManager->getNumTurrets();
			for (int i = 0; i < listSize; i++) {
				TurretPtr turret = ObjectManager->getTurret(i);
				if (turret) {
					append(turret->getPartId());
				}
			}
			listSize = ObjectManager->getNumGates();
			for (int i = 0; i < listSize; i++) {
				GatePtr gate = ObjectManager->getGate(i);
				if (gate) {
					append(gate->getPartId());
				}
			}
			}
			break;
	}
	
	ABLi_pushInteger(abl::clampArrayCount(static_cast<long>(numObjects), output.size(), "getobjects_truncated"));
}

void execOrderWait (void) {

	float seconds = ABLi_popReal();
	bool clearLastTarget = ABLi_peekBoolean();

	if (ABLi_getSkipOrder()) {
		ABLi_pokeInteger(1);
		return;
	}

	long result = TACORDER_FAILURE;
	result = CurWarrior->orderWait(false, ORDER_ORIGIN_COMMANDER, double2long(seconds), clearLastTarget);
	ABLi_pokeInteger(result);
}

void execOrderMoveTo (void) {

	//	MOVE TO POINT tactical order:
	//		Set tactical order to move to specified location, using the
	//		current attitude orders and other move factors. Returns tacorder
	//		result code.
	//		PARAMS:	real[3]					location
	//				boolean					run
	//		RETURN: integer

	float* coordList = ABLi_popRealPtr();
	bool run = ABLi_popBoolean();

	if (s_ablArgGuard && !coordList) {
		abl_arg_guard_log("execOrderMoveTo", "coordList");
		ABLi_pushInteger(TACORDER_FAILURE);
		return;
	}

	if (ABLi_getSkipOrder()) {
		ABLi_pushInteger(1);
		return;
	}

	long result = TACORDER_FAILURE;
	glm::vec3 location;
	location.x = coordList[0];
	location.y = coordList[1];
	location.z = coordList[2];
	
	unsigned long params = TACORDER_PARAM_NONE;
	if (run)
		params |= TACORDER_PARAM_RUN;

	result = CurWarrior->orderMoveToPoint(false, true, ORDER_ORIGIN_COMMANDER, location, -1, params);

	ABLi_pushInteger(result);
}

void execOrderMoveToObject (void) {

	//	MOVE TO OBJECT tactical order:
	//		Set tactical order to move to specified object, using the given
	//		movement parameters (stealth, etc.). Returns error code.
	//		PARAMS:	integer		contact id
	//				boolean		run
	//		RETURN: integer		error code

	unsigned long objectId = ABLi_popInteger();
	bool run = ABLi_popBoolean();

	if (ABLi_getSkipOrder()) {
		ABLi_pushInteger(1);
		return;
	}

	long result = 1;
	GameObjectPtr object = nullptr;
	if ((objectId >= MIN_UNIT_PART_ID) && (objectId <= MAX_UNIT_PART_ID)) {
		// We have a group.  Act accordingly.
		// Run through all objects fitting the group.
		result = 1;
		}
	else {
		object = getObject(objectId);
		if (object)
			result = CurWarrior->orderMoveToObject(false, true, ORDER_ORIGIN_COMMANDER, object, -1, -1, run);
	}

	ABLi_pushInteger(result);
}

void execOrderMoveToContact (void) {

	//	MOVE TO CONTACT tactical order:
	//		Set tactical order to move to specified contact, using the given
	//		movement parameters (stealth, etc.). Returns error code.
	//		PARAMS:	boolean		run
	//		RETURN: integer		error code

	bool run = ABLi_popBoolean();

	if (ABLi_getSkipOrder()) {
		ABLi_pushInteger(1);
		return;
	}

	long result = TACORDER_FAILURE;
	if (CurContact) {
		result = CurWarrior->orderMoveToObject(false, true, ORDER_ORIGIN_COMMANDER, CurContact, -1, -1, run);
	}

	ABLi_pushInteger(result);
}

void execOrderPowerDown (void) {

	//	ORDER POWER DOWN function:
	//		Set tactical order to power down.
	//		Returns tacorder result	code.
	//		PARAMS:	NONE
	//		RETURN: integer

	if (ABLi_getSkipOrder()) {
		ABLi_pushInteger(1);
		return;
	}

	ABLi_pushInteger(CurWarrior->orderPowerDown(false, TacOrderOrigin));
}

void execOrderPowerUp (void) {

	//	ORDER POWER UP function:
	//		Set tactical order to power up.
	//		Returns tacorder result	code.
	//		PARAMS:	NONE
	//		RETURN: integer

	if (ABLi_getSkipOrder()) {
		ABLi_pushInteger(1);
		return;
	}

	ABLi_pushInteger(CurWarrior->orderPowerUp(false, TacOrderOrigin));
}

void execOrderAttackObject (void) {

	//	ORDER ATTACK OBJECT tactical order:
	//		Set tactical order to direct all weapons fire at the specified
	//		object. Returns an error code.
	//		PARAMS:	integer		object id
	//				integer		attack type (0 = none, 1 = destroy, 2 = disable)
	//				integer		method (0 = ranged, 1 = dfa, 2 = ramming)
	//				integer		range (0 = short, 1 = medium, 2 = long)
	//				integer		pursue (0 = False, 1 = True)
	//		RETURN: integer		error code

	unsigned long objectId = ABLi_popInteger();
	long attackType = ABLi_popInteger();
	long attackMethod = ABLi_popInteger();
	long attackRange = ABLi_popInteger();
	long pursue = ABLi_popInteger();

	long result = 1;
	if (!ABLi_getSkipOrder()) {
		unsigned long params = TACORDER_PARAM_NONE;
		if (pursue)
			params |= TACORDER_PARAM_PURSUE;

		GameObjectPtr object = nullptr;
		if ((objectId >= MIN_UNIT_PART_ID) && (objectId <= MAX_UNIT_PART_ID)) {
			// We have a group.  Act accordingly.
			// Run through all objects fitting the group.
			}
		else {
			if (objectId > 0)
				object = ObjectManager->findByPartId(objectId);
			result = CurWarrior->orderAttackObject(false, ORDER_ORIGIN_COMMANDER, object, attackType, attackMethod, attackRange, -1, -1, params);
		}
	}

	ABLi_pushInteger(result);
}

void execOrderAttackContact (void) {

	//	ORDER ATTACK CONTACT tactical order:
	//		Set tactical order to direct all weapons fire at the current
	//		contact. Returns an error code.
	//		PARAMS:	integer		attack type (0 = none, 1 = destroy, 2 = disable)
	//				integer		range (0 = short, 1 = medium, 2 = long)
	//				integer		method (0 = ranged, 1 = dfa, 2 = ramming)
	//				integer		pursue (0 = False, 1 = True)
	//		RETURN: integer		error code

	long attackType = ABLi_popInteger();
	long attackMethod = ABLi_popInteger();
	long attackRange = ABLi_popInteger();
	long pursue = ABLi_popInteger();

	long result = 1;
	if (!ABLi_getSkipOrder()) {
		unsigned long params = TACORDER_PARAM_NONE;
		if (pursue)
			params |= TACORDER_PARAM_PURSUE;
		result = -2;
		if (CurContact)
			result = CurWarrior->orderAttackObject(false, ORDER_ORIGIN_COMMANDER, CurContact, attackType, attackMethod, attackRange, -1, -1, params);
	}

	ABLi_pushInteger(result);
}

void execOrderWithdraw(void) {

	//Makes the object(s) passed in withdraw 
	//		PARAMS:	NONE
	//		Returns: integer (result)

	ABLi_pushInteger(1);
	if (!ABLi_getSkipOrder()) {
		glm::vec3 location;
		location = glm::vec3(0.0f);
		ABLi_pushInteger(0);

		if (CurWarrior)
			CurWarrior->orderWithdraw(false, ORDER_ORIGIN_COMMANDER, location);
		else
			ABLi_pokeInteger(-2);
	}
}	

void execObjectInWithdrawal(void) {

	//Checks the object(s) passed in to see if they are withdrawing
	//		PARAMS:	integer
	//		Returns: integer (result)

	long objectId1 = ABLi_popInteger();

	long result = 1;
	GameObjectPtr object1 = nullptr;
	if ((objectId1 >= MIN_UNIT_PART_ID) && (objectId1 <= MAX_UNIT_PART_ID)) {
		// We have a group.  Act accordingly.
		// Run through all objects fitting the group.
		long numObjects = getMovers(objectId1, moverList);
		for (long i = 0; i < numObjects; i++) {
			if (!moverList[i]->isWithdrawing()) {
				result = 0;
				break;
			}
		}
		}
	else {
		object1 = getObject(objectId1);
		if (object1 && !object1->isWithdrawing())
			result = 0;			
	}
	
	ABLi_pushInteger(result);
}	

void execCoreMoveTo (void) {

	float* location = ABLi_popRealPtr();
	unsigned long params = static_cast<unsigned long>(ABLi_popInteger());

	if (s_ablArgGuard && !location) { abl_arg_guard_log("execCoreMoveTo", "location"); ABLi_pushInteger(0); return; }

	if (ABLi_getSkipOrder()) {
		ABLi_pushInteger(1);
		return;
	}

	if (isnan(location[0]) || isnan(location[1]))
	{
		STOP(("Move to order location is Not a Number in Brain %s.  Check patrol path array indices!",CurWarrior->getBrainString()));
	}

	long result = 0;
	if (CurWarrior) 
	{
		glm::vec3 loc(location[0], location[1], 0.0);
		result = CurWarrior->coreMoveTo(loc, params);
	}

	ABLi_pushInteger(result);
}

void execCoreMoveToObject (void) {

	long objectID = ABLi_popInteger();
	unsigned long params = static_cast<unsigned long>(ABLi_popInteger());

	if (ABLi_getSkipOrder()) {
		ABLi_pushInteger(1);
		return;
	}

	GameObjectPtr target = getObject(objectID);
	long result = 0;
	if (CurWarrior)
		result = CurWarrior->coreMoveToObject(target, params);

	ABLi_pushInteger(result);
}

void execCorePower (void) {

	bool up = ABLi_popBoolean();

	if (ABLi_getSkipOrder()) {
		ABLi_pushInteger(1);
		return;
	}

	long result = 0;
	if (CurWarrior)
		result = CurWarrior->corePower(up);

	ABLi_pushInteger(result);
}

void execCorePowerPart (void) {

	//	newpower(partId, up) from a mission script (REAP3): power the mover with that part id up or down.
	//		PARAMS: integer partId, boolean up
	//		RETURN: integer. An unknown part id or non-mover is a logged no-op.

	long partId = ABLi_popInteger();
	bool up = ABLi_popBoolean();

	if (ABLi_getSkipOrder()) {
		ABLi_pushInteger(1);
		return;
	}

	long result = 1;
	GameObjectPtr obj = getObject(partId);
	if (obj && obj->isMover() && static_cast<MoverPtr>(obj)->getPilot())
	{
		result = static_cast<MoverPtr>(obj)->getPilot()->corePower(up);
		if (mc2_diag::tagRequested("ABL_NEWPOWER_PART"))
			mc2::fmt::print_err("[ABL_NEWPOWER_PART] partId={} up={} result={} status={}\n", partId, up ? 1 : 0, result, static_cast<long>(static_cast<MoverPtr>(obj)->getStatus()));
	}
	else
		{ if (mc2_diag::tagRequested("ABL_NEWPOWER_PART")) mc2::fmt::print_err("[ABL_NEWPOWER_PART] partId={} not a live mover, ignored\n", partId); }

	ABLi_pushInteger(result);
}

void execCoreAttack (void) {

	long objectID = ABLi_popInteger();
	unsigned long params = static_cast<unsigned long>(ABLi_popInteger());

	if (ABLi_getSkipOrder()) {
		ABLi_pushInteger(1);
		return;
	}

	GameObjectPtr target = getObject(objectID);
	long result = 0;
	if (CurWarrior)
		result = CurWarrior->coreAttack(target, params);

	ABLi_pushInteger(result);
}

void execCoreCapture (void) {

	long objectID = ABLi_popInteger();
	unsigned long params = static_cast<unsigned long>(ABLi_popInteger());

	if (ABLi_getSkipOrder()) {
		ABLi_pushInteger(1);
		return;
	}

	long targetID = 0;
	GameObjectPtr obj = getObject(objectID);
	if (CurWarrior && ((objectID == 0) || obj))
		targetID = CurWarrior->coreCapture((objectID == 0) ? nullptr : obj, params);

	ABLi_pushInteger(targetID);
}

void execCoreScan (void) {

	long objectID = ABLi_popInteger();
	unsigned long params = static_cast<unsigned long>(ABLi_popInteger());

	if (ABLi_getSkipOrder()) {
		ABLi_pushInteger(1);
		return;
	}

	long targetID = 0;
	GameObjectPtr obj = getObject(objectID);
	if (CurWarrior && ((objectID == 0) || obj))
		targetID = CurWarrior->coreScan((objectID == 0) ? nullptr : obj, params);

	ABLi_pushInteger(targetID);
}

void execCoreControl (void) {

	long objectID = ABLi_popInteger();
	unsigned long params = static_cast<unsigned long>(ABLi_popInteger());

	if (ABLi_getSkipOrder()) {
		ABLi_pushInteger(1);
		return;
	}

	long targetID = 0;
	GameObjectPtr obj = getObject(objectID);
	if (CurWarrior && ((objectID == 0) || obj))
		targetID = CurWarrior->coreControl((objectID == 0) ? nullptr : obj, params);

	ABLi_pushInteger(targetID);
}

void execCoreEject (void) {

	if (ABLi_getSkipOrder()) {
		ABLi_pushInteger(1);
		return;
	}

	long result = 0;
	if (CurWarrior)
		result = CurWarrior->coreEject();

	ABLi_pushInteger(result);
}
