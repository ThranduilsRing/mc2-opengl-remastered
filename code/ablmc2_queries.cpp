// ABL native bindings: warrior/contact/weapon queries and setters.
// Declared in ablmc2_internal.h; registered, in order, by initABL in ablmc2.cpp.
#include "ablmc2_internal.h"
#include "abl_script_events.h"
#include <algorithm>
#include <iterator>
#include <span>
#include <vector>

std::span<int> ABLi_popIntBuf(void);
std::span<float> ABLi_popRealBuf(void);
namespace abl {
long clampArrayCount(long, size_t, std::string_view);
long copyIntArray(std::span<int>, std::span<const int>, std::string_view);
long copyRealArray(std::span<float>, std::span<const float>, std::string_view);
}



void execGetId (void) {

	//	GET ID function:
	//		Returns object id of current object.
	//		PARAMS:	none
	//		RETURN: integer

	long partID = 0;
	if (CurObject)
		partID = CurObject->getPartId();
	ABLi_pushInteger(partID);
}

void execGetTime (void) {

	//	GET TIME function:
	//		Returns current mission(scenario) time.
	//		PARAMS:	none
	//		RETURN: real

	ABLi_pushReal(mission->actualTime);
}

void execGetTimeLeft (void) {

	//	GET TIME function:
	//		Returns mission(scenario) time left.
	//		PARAMS:	none
	//		RETURN: real

	if (mission->m_timeLimit > -1)
		if (mission->m_timeLimit - mission->actualTime > 0)
			ABLi_pushReal(mission->m_timeLimit - mission->actualTime);
		else
			ABLi_pushReal(0.0);
	else
		ABLi_pushReal(-1.0);
}

void execSelectObject (void) {

	//	SELECT OBJECT function:
	//		Sets the specified object as the Current Object.
	//		It returns an error code if it fails, else it returns the
	//		id of the previous current object.
	//				integer		object id
	//		RETURN: integer		object id or error code (-1 = not an object)

	long objectID = ABLi_peekInteger();

	// Remember what the current object is...
	long curObjectID = 0;
	if (CurObject)
		curObjectID = CurObject->getPartId();

	GameObjectPtr newObject = getObject(objectID);
	if (newObject) {
		CurObject = newObject;
		ABLi_pokeInteger(curObjectID);
		}
	else
		ABLi_pokeInteger(-1);
}

void execSelectWarrior (void) {

	//	SELECT WARRIOR function:
	//		Sets the specified warrior as the Current Pilot. All pilot-assumed
	//		ABL functions reference the Current Pilot(Warrior).
	//		It returns an error code if it fails, else it returns the
	//		warrior id of the previous current pilot.
	//				integer				warrior id
	//		RETURN: integer				warrior id or error code (-1 = not a pilot)

	long warriorIndex = ABLi_popInteger();

	// Remember who the current warrior is...
	long curWarriorIndex = 0;
	if (CurWarrior)
		curWarriorIndex = CurWarrior->getIndex();

	CurWarrior = getWarrior(warriorIndex);
	ABLi_pushInteger(curWarriorIndex);
}

void execGetWarriorStatus (void) {

	//	GET WARRIOR STATUS function:
	//		Returns the status of the warrior.
	//		PARAMS:	integer				warrior id 
	//		RETURN: integer				status

	long warriorId = ABLi_peekInteger();

	MechWarriorPtr warrior = getWarrior(warriorId);
	if (warrior)
		ABLi_pokeInteger(warrior->getStatus());
	else
		ABLi_pokeInteger(-1);
}

void execGetContacts (void) {

	//	GET CONTACTS function:
	//		Returns a sorted list of all criteria'd contacts for the pilot.
	//		PARAMS:	integer[]			contact list
	//				integer				contact criteria
	//				integer				sort criteria
	//		RETURN: integer				result code

	const auto output = ABLi_popIntBuf();
	int contactList[MAX_CONTACTS_PER_TEAM] = {};
	int contactCriteria = ABLi_popInteger();
	int sortCriteria = ABLi_popInteger();

	long numContacts = CurObject ? CurObject->getContacts(contactList, contactCriteria, sortCriteria) : 0;
	for (long i = 0; i < numContacts; i++) {
		GameObjectPtr obj = ObjectManager->get(contactList[i]);
		contactList[i] = obj ? obj->getPartId() : 0;
	}
	ABLi_pushInteger(abl::copyIntArray(output, {contactList, static_cast<size_t>(std::max(0L, numContacts))}, "getcontacts_truncated"));
}

void execGetEnemyCount (void) {

	//	GET ENEMY COUNT function:
	//		Returns number of sighted enemies.
	//		PARAMS:	integer				object id
	//		RETURN: integer				result code

	long objectId = ABLi_popInteger();

	long result = -1;
	if ((objectId >= MIN_UNIT_PART_ID) && (objectId <= MAX_UNIT_PART_ID)) {
		if ((objectId >= OBJ_ID_FIRST_TEAM) && (objectId <= OBJ_ID_LAST_TEAM)) {
			TeamPtr team = Team::teams[objectId - OBJ_ID_FIRST_TEAM];
			if (team)
				result = SensorManager->getTeamSensor(team->getId())->numContacts;
		}
		}
	else {
		GameObjectPtr object = getObject(objectId);
		if (object) {
			if (object->isMover()) {
				if (object->getTeam())
					result = SensorManager->getTeamSensor(object->getTeamId())->numContacts;
				}
			else
				switch (object->getObjectClass()) {
					case BUILDING:
					case ARTILLERY:
						if (object->getTeamId() > -1)
							result = SensorManager->getTeamSensor(object->getTeamId())->numContacts;
						break;
				}
		}
	}

	ABLi_pushInteger(result);
}

void execSelectContact (void) {

	//	SELECT CONTACT function:
	//		Sets the specified contact as the Current Contact. All pilot
	//		contact-related ABL functions reference the Current Contact.
	//		The Current Pilot's contact lists are referenced.
	//		PARAMS:	integer				contact handle
	//		RETURN: integer				error code (1 = not a contact)

	long type = ABLi_popInteger();
	long id = ABLi_popInteger();

	long code = -1;
	if (CurObject->isMover()) {
		MoverPtr me = (MoverPtr)CurObject;
		MoverPtr object = (MoverPtr)ObjectManager->findByPartId(id);
		if (object && object->getTeam() && object->getContactStatus(me->getTeam()->getId(), true) != CONTACT_NONE) {
			CurContact = object;
			code = NO_ERR;
			}
		else
			code = 1;
	}

	ABLi_pushInteger(code);
}

void execIsContact (void) {

	long objectId = ABLi_popInteger();
	long criteria = ABLi_popInteger();
	long select = ABLi_popInteger();

	long result = -1;
	if (CurObject->isMover() && CurObject->getTeam()) {
		GameObjectPtr object = getObject(objectId);
		if (object->isMover() && (SensorManager->getTeamSensor(CurObject->getTeamId())->meetsCriteria(CurObject, (MoverPtr)object, criteria))) {
			result = objectId;
			if (select)
				CurContact = object;
			}
		else
			result = 0;
	}

	ABLi_pushInteger(result);
}

void execGetContactId (void) {

	//	GET CONTACT ID function:
	//		Retrieves the current contact's object id number.
	//		PARAMS:	none
	//		RETURN: integer				contact's object id

	long result = 0;
	if (CurContact)
		result = CurContact->getPartId();

	ABLi_pushInteger(result);
}

void execGetContactStatus (void) {

	//	GET CONTACT STATUS function:
	//		Retrieves the contact status of the current contact.
	//		PARAMS:	@integer			is set to tag state, if any
	//		RETURN: integer				contact's status

	int* contactTagged = ABLi_popIntegerPtr();

	*contactTagged = 0;

	long status = 0;
	if (CurContact && CurObject->isMover())
		status = ((MoverPtr)CurContact)->getContactStatus(CurObject->getTeamId(), true);
	ABLi_pushInteger(status);
}

void execGetContactRelativePosition (void) {

	//	GET CONTACT RELATIVE POSITION function:
	//		Retrieves the range and angle to the current contact.
	//		PARAMS:	@real				is set to the range (in meters) to contact
	//				@real				is set to the angle (in degrees) to contact
	//		RETURN: integer				error code

	float* range = ABLi_popRealPtr();
	float* angle = ABLi_popRealPtr();

	if (s_ablArgGuard && !range) {
		abl_arg_guard_log("execGetContactRelativePosition", "range");
		return;
	}
	if (s_ablArgGuard && !angle) {
		abl_arg_guard_log("execGetContactRelativePosition", "angle");
		return;
	}

	*range = -1.0;
	*angle = 0.0;

	long result = 1;
	if (CurContact && CurObject) {
		*range = CurObject->distanceFrom(CurContact->getPosition());
		*angle = CurObject->relFacingTo(CurContact->getPosition());
		result = NO_ERR;
	}

	ABLi_pushInteger(result);
}

void execSetTarget (void) {

	long attackerId = ABLi_popInteger();
	long targetId = ABLi_popInteger();

	GameObjectPtr target = getObject(targetId);
	if ((attackerId >= MIN_UNIT_PART_ID) && (attackerId <= MAX_UNIT_PART_ID)) {
		long numObjects = getMovers(attackerId, moverList);
		for (long i = 0; i < numObjects; i++) {
			MechWarriorPtr pilot = moverList[i]->getPilot();
			if (pilot) {
				pilot->setCurrentTarget(target);
				pilot->getVehicle()->calcFireRanges();
			}
		}
		}
	else {
		GameObjectPtr attacker = getObject(attackerId);
		if (attacker) {
			MechWarriorPtr pilot = ((MoverPtr)attacker)->getPilot();
			if (pilot) {
				pilot->setCurrentTarget(target);
				pilot->getVehicle()->calcFireRanges();
			}
		}
	}
}

void execGetTarget (void) {

	long objectId = ABLi_popInteger();

	long partID = 0;
	if ((objectId >= MIN_UNIT_PART_ID) && (objectId <= MAX_UNIT_PART_ID)) {
		}
	else {
		GameObjectPtr object = getObject(objectId);
		if (object && object->isMover()) {
			MechWarriorPtr pilot = ((MoverPtr)object)->getPilot();
			Assert(pilot != nullptr, 0, " execHbGetTarget:No pilot in mover! ");
			GameObjectPtr target = pilot->getCurrentTarget();
			if (target)
				partID = target->getPartId();
		}
	}
	ABLi_pushInteger(partID);
}

void execGetWeaponsReady (void) {

	const auto output = ABLi_popIntBuf();
	int listSize = ABLi_popInteger();

	long result = 0;
	if (CurObject && CurObject->isMover()) {
		auto* mover = static_cast<MoverPtr>(CurObject);
		std::vector<int> weapons;
		if (listSize == -1) {
			weapons.resize(MAX_WEAPONS_PER_MOVER);
		} else {
			if (listSize < 0) {
				abl::reportBadArgument("getweaponsready", "listSize");
				ABLi_pushInteger(0);
				return;
			}
			listSize = static_cast<int>(abl::clampArrayCount(listSize, output.size(), "getweaponsready_truncated"));
			if (listSize) weapons.assign(output.begin(), output.begin() + listSize);
			if (std::ranges::any_of(weapons, [mover](int index) { return !mover->isWeaponIndex(index); })) {
				abl::reportBadArgument("getweaponsready", "weaponIndex");
				ABLi_pushInteger(0);
				return;
			}
		}
		result = mover->getWeaponsReady(weapons.data(), listSize);
		if (result >= 0)
			result = abl::copyIntArray(output, {weapons.data(), static_cast<size_t>(result)}, "getweaponsready_truncated");
	}
	ABLi_pushInteger(result);
}

void execGetWeaponsLocked (void) {

	const auto output = ABLi_popIntBuf();
	int listSize = ABLi_popInteger();

	long result = 0;
	if (CurObject && CurObject->isMover()) {
		auto* mover = static_cast<MoverPtr>(CurObject);
		std::vector<int> weapons;
		if (listSize == -1) {
			weapons.resize(MAX_WEAPONS_PER_MOVER);
		} else {
			if (listSize < 0) {
				abl::reportBadArgument("getweaponslocked", "listSize");
				ABLi_pushInteger(0);
				return;
			}
			listSize = static_cast<int>(abl::clampArrayCount(listSize, output.size(), "getweaponslocked_truncated"));
			if (listSize) weapons.assign(output.begin(), output.begin() + listSize);
			if (std::ranges::any_of(weapons, [mover](int index) { return !mover->isWeaponIndex(index); })) {
				abl::reportBadArgument("getweaponslocked", "weaponIndex");
				ABLi_pushInteger(0);
				return;
			}
		}
		result = mover->getWeaponsLocked(weapons.data(), listSize);
		if (result >= 0)
			result = abl::copyIntArray(output, {weapons.data(), static_cast<size_t>(result)}, "getweaponslocked_truncated");
	}
	ABLi_pushInteger(result);
}

void execGetWeaponsInRange (void) {

	const auto output = ABLi_popIntBuf();
	int listSize = ABLi_popInteger();

	long result = 0;
	if (CurObject && CurObject->isMover()) {
		auto* mover = static_cast<MoverPtr>(CurObject);
		std::vector<int> weapons;
		if (listSize == -1) {
			weapons.resize(MAX_WEAPONS_PER_MOVER);
		} else {
			if (listSize < 0) {
				abl::reportBadArgument("getweaponsinrange", "listSize");
				ABLi_pushInteger(0);
				return;
			}
			listSize = static_cast<int>(abl::clampArrayCount(listSize, output.size(), "getweaponsinrange_truncated"));
			if (listSize) weapons.assign(output.begin(), output.begin() + listSize);
			if (std::ranges::any_of(weapons, [mover](int index) { return !mover->isWeaponIndex(index); })) {
				abl::reportBadArgument("getweaponsinrange", "weaponIndex");
				ABLi_pushInteger(0);
				return;
			}
		}
		auto* target = CurWarrior ? CurWarrior->getCurrentTarget() : nullptr;
		if (target)
			result = mover->getWeaponsInRange(weapons.data(), listSize, CurObject->distanceFrom(target->getPosition()));
		if (result >= 0)
			result = abl::copyIntArray(output, {weapons.data(), static_cast<size_t>(result)}, "getweaponsinrange_truncated");
	}
	ABLi_pushInteger(result);
}

void execGetWeaponShots (void) {

	long weaponIndex = ABLi_popInteger();

	long numShots = 0;
	if (CurObject->isMover())
		numShots = ((MoverPtr)CurObject)->getWeaponShots(weaponIndex);

	ABLi_pushInteger(numShots);
}

void execGetWeaponRanges (void) {

	//	GET WEAPON RANGES function:
	//		Returns the range in meters for the object's shortest,
	//		optimal and longest weapon ranges.
	//		PARAMS:	integer				object id
	//				real[3]				list to store weapon ranges
	//		RETURN: none

	long objectId = ABLi_popInteger();
	const auto output = ABLi_popRealBuf();
	float rangeList[3] = {};

	GameObjectPtr object = getObject(objectId);
	if (object && object->isMover()) {
		rangeList[0] = ((MoverPtr)object)->getMinFireRange();
		rangeList[1] = ((MoverPtr)object)->getOptimalFireRange();
		rangeList[2] = ((MoverPtr)object)->getMaxFireRange();
		}
	else
		rangeList[0] = rangeList[1] = rangeList[2] = 0.0;
	abl::copyRealArray(output, rangeList, "getweaponranges_truncated");
}

void execGetObjectPosition (void) {

	long objectId = ABLi_popInteger();
	const auto output = ABLi_popRealBuf();
	float coordList[3] = {};

	coordList[0] = 0.0;
	coordList[1] = 0.0;
	coordList[2] = 0.0;
		
	GameObjectPtr obj = getObject(objectId);
	if (obj) {
		Stuff::Vector3D pos = obj->getPosition();
		coordList[0] = pos.x;
		coordList[1] = pos.y;
		coordList[2] = pos.z;
	}
	abl::copyRealArray(output, coordList, "getobjectposition_truncated");
}

void execGetIntegerMemory (void) {

	//	GET MEMORY INTEGER function:
	//		Returns current integer value in memory cell of pilot.
	//		PARAMS:	integer
	//		RETURN: integer

	long memIndex = ABLi_peekInteger();
	ABLi_pokeInteger(CurWarrior->getIntegerMemory(memIndex));
}

void execGetRealMemory (void) {

	//	GET MEMORY REAL function:
	//		Returns current real value in memory cell of pilot.
	//		PARAMS:	integer
	//		RETURN: real

	long memIndex = ABLi_peekInteger();
	ABLi_pokeReal(CurWarrior->getRealMemory(memIndex));
}

void execGetAlarmTriggers (void) {

	//	GET ALARM TRIGGERS function:
	//		Returns number of triggers for the current combat event handler.
	//		It also fills the list with all of the trigger ids for this
	//		frame(call).
	//		PARAMS:	integer[]
	//		RETURN: integer

	const auto output = ABLi_popIntBuf();
	int triggers[MAX_ALARM_TRIGGERS] = {};
	const long count = CurWarrior && CurAlarm >= 0 && CurAlarm < NUM_PILOT_ALARMS
	    ? CurWarrior->getEventHistory(CurAlarm, triggers) : 0;
	ABLi_pushInteger(abl::copyIntArray(output, {triggers, static_cast<size_t>(count)}, "getalarmtriggers_truncated"));
}

void execGetChallenger (void) {

	//	GET CHALLENGER function:
	//		Returns the challenger for the specified object.
	//		PARAMS:	integer				object id
	//		RETURN: integer				challenger object id

	long objectId = ABLi_popInteger();

	long challengerPartID = 0;
	if ((objectId >= MIN_UNIT_PART_ID) && (objectId <= MAX_UNIT_PART_ID)) {
		}
	else {
		GameObjectPtr object = getObject(objectId);
		if (object) {
			if (object->isMover()) {
				GameObjectPtr challenger = ((MoverPtr)object)->getChallenger();
				if (challenger)
					challengerPartID = challenger->getPartId();
			}
		}
	}

	ABLi_pushInteger(challengerPartID);
}

void execGetTimeWithoutOrders (void) {

	//	GET TIME WITHOUT ORDERS function:
	//		Returns the current pilot's time (in secs) without orders.
	//		PARAMS:	none
	//		RETURN: real				time (in seconds)

	ABLi_pushReal(0.0);
	if (CurWarrior) {
		float lastTime = CurWarrior->getTimeOfLastOrders();
		if (lastTime >= 0.0)
			ABLi_pokeReal(scenarioTime - lastTime);
	}
}

void execGetFireRanges (void) {

	//	GET FIRE RANGES function:
	//		Retrieves the current settings for SHORT, MEDIUM and LONG range,
	//		as well as MAX WEAPON range
	//		PARAMS:	@real[4]			sets the game's current ranges (meters)				
	//		RETURN: none

	const auto output = ABLi_popRealBuf();
	float ranges[4] = {};

	ranges[0] = WeaponRange[FIRERANGE_SHORT];
	ranges[1] = WeaponRange[FIRERANGE_MEDIUM];
	ranges[2] = WeaponRange[FIRERANGE_LONG];
	ranges[3] = 250.0; //No one should use this in MC2!
	abl::copyRealArray(output, ranges, "getfireranges_truncated");
}

void execGetAttackers (void) {

	//	GET ATTACKERS function:
	//		Retrieves all objects that have attacked current pilot
	//		in the last x seconds.
	//		PARAMS:	@integer[]			attacker list (make it big!)
	//				real				seconds
	//		RETURN: integer				num attackers

	const auto output = ABLi_popIntBuf();
	const float seconds = ABLi_popReal();
	unsigned int attackers[MAX_ATTACKERS] = {};
	const long count = CurWarrior ? CurWarrior->getAttackers(attackers, seconds) : 0;
	const long written = abl::clampArrayCount(count, output.size(), "getattackers_truncated");
	for (long i = 0; i < written; ++i)
		output[i] = static_cast<int>(attackers[i]);
	ABLi_pushInteger(written);
}

void execGetAttackerInfo (void) {

	//	GET ATTACKER INFO function:
	//		If the object has attacked the current pilot, it returns the #
	//		of seconds since the last attack (FUTURE: may return # of hits,
	//		damage done, etc. thru params). If the object never attacked
	//		'em, returns -1.0.
	//		PARAMS:	integer				attacker id
	//		RETURN: real				seconds since last attack

	unsigned long attackerId = ABLi_popInteger();

	float timeSince = 1000000.0;
	if ((attackerId >= MIN_UNIT_PART_ID) && (attackerId <= MAX_UNIT_PART_ID)) {
		}
	else {
		if (CurWarrior) {
			AttackerRecPtr attackerRec = CurWarrior->getAttackerInfo(attackerId);
			if (attackerRec)
				timeSince = scenarioTime - attackerRec->lastTime;
		}
	}
	ABLi_pushReal(timeSince);
}

void execSetChallenger (void) {

	//	SET CHALLENGER function:
	//		Sets the challenger for the specified object.
	//		PARAMS:	integer				object1
	//				integer				object2 (challenger)
	//		RETURN: integer				error code

	long victimId = ABLi_popInteger();
	long challengerId = ABLi_popInteger();

	long result = NO_ERR;
	
	if ((challengerId >= MIN_UNIT_PART_ID) && (challengerId <= MAX_UNIT_PART_ID)) {
		result = -1;
		}
	else {
		GameObjectPtr challenger = getObject(challengerId);
		GameObjectPtr victim = getObject(victimId);
		if (victim && victim->isMover())
			((MoverPtr)victim)->setChallenger(challenger);
		else
			result = -2;
	}
	ABLi_pushInteger(result);
}

void execSetIntegerMemory (void) {

	long memIndex = ABLi_popInteger();
	long memValue = ABLi_popInteger();
	CurWarrior->setIntegerMemory(memIndex, memValue);
}

void execSetRealMemory (void) {

	long memIndex = ABLi_popInteger();
	float memValue = ABLi_popReal();
	CurWarrior->setRealMemory(memIndex, memValue);
}

void execHasMoveGoal (void) {

	bool result = false;
	if (CurWarrior)
		result = (CurWarrior->getMoveNewGoal() && CurWarrior->hasMoveGoal());
	ABLi_pushBoolean(result);
}

void execHasMovePath (void) {

	bool result = false;
	if (CurWarrior)
		result = (CurWarrior->getMovePath() && !CurWarrior->getMoveNewGoal());
	ABLi_pushBoolean(result);
}

void execSortWeapons (void) {

	const auto output = ABLi_popIntBuf();
	long listSize = ABLi_popInteger();
	const long sortType = ABLi_popInteger();
	if (!CurObject || !CurObject->isMover()) return;
	auto* mover = static_cast<MoverPtr>(CurObject);
	if (listSize < -1) {
		abl::reportBadArgument("sortweapons", "listSize");
		return;
	}
	int weapons[MAX_WEAPONS_PER_MOVER] = {};
	long values[MAX_WEAPONS_PER_MOVER] = {};
	const long count = listSize == -1 ? mover->numWeapons : abl::clampArrayCount(listSize,
	    std::min(output.size(), std::size(weapons)), "sortweapons_truncated");
	if (listSize != -1) {
		std::copy_n(output.begin(), count, weapons);
		if (std::ranges::any_of(std::span{weapons, static_cast<size_t>(count)}, [mover](int index) {
			return index != -1 && !mover->isWeaponIndex(index);
		})) {
			abl::reportBadArgument("sortweapons", "weaponIndex");
			return;
		}
		listSize = count;
	}
	if (mover->sortWeapons(weapons, values, listSize, sortType, true) == NO_ERR)
		abl::copyIntArray(output, {weapons, static_cast<size_t>(count)}, "sortweapons_truncated");
}

void execGetVisualRange (void) {

	//	GET VISUAL RANGE function:
	//		Returns visual range of the indicated object (taking into
	//		account the infamous Beagle Probe).
	//		PARAMS:	integer				object id
	//		RETURN: real				range in meters

	long objectId = ABLi_popInteger();

	float range = 0.0;
	GameObjectPtr object = nullptr;
	if ((objectId >= MIN_UNIT_PART_ID) && (objectId <= MAX_UNIT_PART_ID)) {
		range = -1;
		}
	else {
		object = getObject(objectId);
		if (object && object->isMover())
			range = ((MoverPtr)object)->getVisualRange();
	}
	ABLi_pushReal(range);
}

void execGetUnitMates (void) {

	//	GET UNIT MATES function:
	//		Returns number and list of unit mates of the specified object.
	//		PARAMS:	integer			object (unit) id
	//				integer[]		object list
	//		RETURN: integer			num objects in unit

	long objectId = ABLi_popInteger();
	const auto output = ABLi_popIntBuf();
	int mateList[MAX_MOVERS] = {};

	long numObjs = 0;
	if ((objectId >= MIN_UNIT_PART_ID) && (objectId <= MAX_UNIT_PART_ID)) {
		long numObjects = getMovers(objectId, moverList);
		for (long i = 0; i < numObjects; i++)
			mateList[i] = moverList[i]->getPartId();
		numObjs = numObjects;
		}
	else {
		GameObjectPtr object = getObject(objectId);
		if (object && object->isMover()) {
			MoverGroupPtr group = ((MoverPtr)object)->getGroup();
			if (group) {
				long numObjects = group->getMovers(moverList);
				for (long i = 0; i < numObjects; i++)
					mateList[i] = moverList[i]->getPartId();
				numObjs = numObjects;
			}
		}
	}
	ABLi_pushInteger(abl::copyIntArray(output, {mateList, static_cast<size_t>(numObjs)}, "getunitmates_truncated"));
}

void execGetTacOrder(void) {

	//	GET TACORDER:
	//		Retrieves the current tactical order for the specified object.
	//		Also sets the order time and order parameters, which are passed
	//		as call-by-ref parameters.
	//		PARAMS:	integer				object id
	//				@real				time order was given
	//				integer[]			order parameters (size of list varies by order)
	//		RETURN: integer				tac order code

	long objectId = ABLi_popInteger();
	float* time = ABLi_popRealPtr();
	const auto output = ABLi_popIntBuf();
	int paramList[19] = {};
	if (!output.empty()) std::copy_n(output.begin(), std::min(output.size(), std::size(paramList)), paramList);
	
	long code = 0;
	size_t count = 0;
	if ((objectId >= MIN_UNIT_PART_ID) && (objectId <= MAX_UNIT_PART_ID)) {
		}
	else {
		GameObjectPtr object = getObject(objectId);
		if (object && object->isMover()) {
			MechWarriorPtr pilot = ((MoverPtr)object)->getPilot();
			if (pilot) {
				code = pilot->getCurTacOrder()->getParamData(time, paramList);
				GameObjectPtr goalObj = nullptr;
				Stuff::Vector3D goalLoc;
				float goalRange = 0.0;
				paramList[17] = pilot->getMainGoal(goalObj, goalLoc, goalRange);
				paramList[18] = goalObj ? goalObj->getPartId() : 0;
				count = std::size(paramList);
			}
		}
	}

	ABLi_pushInteger(code);
	abl::copyIntArray(output, {paramList, count}, "gettacorder_truncated");
}	

void execGetLastTacOrder(void) {

	//	GET LAST TACORDER:
	//		Retrieves the last tactical order for the specified object.
	//		Also sets the order time and order parameters, which are passed
	//		as call-by-ref parameters.
	//		PARAMS:	integer				object id
	//				@real				time order was given
	//				integer[]			order parameters (size of list varies by order)
	//		RETURN: integer				tac order code

	long objectId = ABLi_popInteger();
	float* time = ABLi_popRealPtr();
	const auto output = ABLi_popIntBuf();
	int paramList[14] = {};
	if (!output.empty()) std::copy_n(output.begin(), std::min(output.size(), std::size(paramList)), paramList);
	
	long code = 0;
	size_t count = 0;
	if ((objectId >= MIN_UNIT_PART_ID) && (objectId <= MAX_UNIT_PART_ID)) {
		}
	else {
		GameObjectPtr object = getObject(objectId);
		if (object && object->isMover()) {
			MechWarriorPtr pilot = ((MoverPtr)object)->getPilot();
			if (pilot) {
				code = pilot->getLastTacOrder()->getParamData(time, paramList);
				count = 7;
				switch (code) {
					case TACTICAL_ORDER_MOVETO_POINT:
					case TACTICAL_ORDER_GUARD: count = 12; break;
					case TACTICAL_ORDER_MOVETO_OBJECT: count = 10; break;
					case TACTICAL_ORDER_ATTACK_OBJECT: count = 14; break;
					default: break;
				}
			}
		}
	}

	ABLi_pushInteger(code);
	abl::copyIntArray(output, {paramList, count}, "getlasttacorder_truncated");
}	
