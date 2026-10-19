// ABL native bindings: object state get/set, salvage, capture, strikes, prisoners and gates.
// Declared in ablmc2_internal.h; registered, in order, by initABL in ablmc2.cpp.
#include "ablmc2_internal.h"
#include "abl_script_events.h"
#include <algorithm>
#include <iterator>
#include <span>

void execSetObjectActive(void) {

	//Turns on/off the AI for the object(s) passed in
	//		PARAMS:	integer				object id
	//				boolean				true = on, false = off
	//		Returns: integer (result)

	long objectId1 = ABLi_popInteger();
	bool active = ABLi_peekBoolean();

	GameObjectPtr object1 = nullptr;
	long numActivated = 0;

	if ((objectId1 >= MIN_UNIT_PART_ID) && (objectId1 <= MAX_UNIT_PART_ID)) {
		long numObjects = getMovers(objectId1, moverList);
		for (long i = 0; i < numObjects; i++)
			if (moverList[i]->getAwake() != active) {
				object1->setAwake(active);
#ifdef USE_IFACE
				theInterface->ActivateMech(moverList[i]->getPartId());
#endif
				numActivated++;
			}
		}
	else  {
		object1 = getObject(objectId1);
		if (object1 && (object1->getAwake() != active)) {
			object1->setAwake(active);
#ifdef USE_IFACE
			theInterface->ActivateMech(object1->getPartId());
#endif
			numActivated++;
		}
	}

	ABLi_pokeInteger(numActivated);
}	

void execObjectTypeID (void) {

	//Returns the objectTypeId for the ObjectNum passed in
	//		PARAMS:	integer
	//		Returns: integer (result)

	long objectId1 = ABLi_popInteger();
	
	long partID = -1;
	GameObjectPtr object1 = getObject(objectId1);
	if (object1)
		partID = object1->getObjectType()->whatAmI();
	ABLi_pushInteger(partID);		
}	

void execGetTerrainObjectPartID (void) {

	//Returns the object PartID for a terrain Object
	//		PARAMS: integer, integer
	//		Returns: integer (result)

	long row = ABLi_popInteger();
	long col = ABLi_popInteger();
	
	long partID = MIN_TERRAIN_PART_ID + row * MAX_MAP_CELL_WIDTH + col;
	pushInteger(partID);
}	

void execGetWeaponAmmo (void) {

	//Returns number of rounds left based on objectId and weaponId 
	//		PARAMS:	integer, integer
	//		Returns: integer (result)

	long objectNum = ABLi_popInteger();
	long weaponNum = ABLi_popInteger();

	GameObjectPtr object1 = getObject(objectNum);
	
	if (object1 && object1->isMover())
		ABLi_pushInteger((static_cast<MoverPtr>(object1))->getWeaponShots(weaponNum));
	else
		ABLi_pushInteger(-1);
}	

void execInArea (void) {

	//returns TRUE if object/unit/team inside area.
	//		PARAMS:	integer				object/unit/team id
	//				real[3]				center of area
	//				real				radius of area
	//				integer				min number to pass (-1 = ALL must be in)
	//		RETURN: boolean				yes or no?

	long objectId = ABLi_popInteger();
	float* areaCenter = ABLi_popRealPtr();
	float areaRadius = ABLi_popReal();
	long minIn = ABLi_peekInteger();

	if (s_ablArgGuard && !areaCenter) {
		abl_arg_guard_log("execInArea", "areaCenter");
		return;
	}

	glm::vec3 center;
	center.x = areaCenter[0];
	center.y = areaCenter[1];
	center.z = areaCenter[2];

	if ((objectId >= MIN_UNIT_PART_ID) && (objectId <= MAX_UNIT_PART_ID)) {
		long numObjects = getMovers(objectId, moverList);
		if (minIn == -1) {
			ABLi_pokeInteger(1);
			long numAlive = 0;
			for (long i = 0; i < numObjects; i++) {
				GameObjectPtr obj = moverList[i];
				if (!obj->isDisabled()) {
					numAlive++;
					if (obj->getExists() && obj->getAwake()) {
						if (obj->distanceFrom(center) > areaRadius) {
							ABLi_pokeInteger(0);
							break;
						}
					}
				}
			}
			if (numAlive == 0)
				ABLi_pokeInteger(0);
			}
		else {
			ABLi_pokeInteger(0);
			long numIn = 0;
			for (long i = 0; i < numObjects; i++) {
				MoverPtr mover = moverList[i];
				if (mover->getExists() && mover->getAwake() && !mover->isDisabled() && !mover->isDestroyed())
					if (mover->distanceFrom(center) <= areaRadius) {
						if (++numIn == minIn) {
							ABLi_pokeInteger(1);
							break;
						}
					}
			}
		}
		}
	else {
		ABLi_pokeInteger(1);
		if (minIn != 0) {
			ABLi_pokeInteger(0);
			GameObjectPtr obj = getObject(objectId);
			if (obj && obj->getExists() && obj->getAwake() && !obj->isDisabled() && !obj->isDestroyed()) {
				if (obj->distanceFrom(center) <= areaRadius)
					ABLi_pokeInteger(1);
			}
		}
	}
}

void execObjectRemove (void) {

	//	Object Remove
	//		Remove the object from the game (COMPLETELY removes).
	//		Currently only works for Movers.
	//		PARAMS:	integer			object id
	//		RETURN:	integer			result (0 = success)

	long objectId = ABLi_popInteger();
	
	GameObjectPtr object = getObject(objectId);
	if (object) {
		if (object->isMover()) {
			// If the object we're removing is the CurObject, then we can't
			// do it until after we finish this object's update!
			// So, we'll just set the removed flag, and then check it
			// at the end of the update.
			if (object == CurObject)
				CurObject->setFlag(OBJECT_FLAG_REMOVED, true);
			else
				mission->removeMover(static_cast<MoverPtr>(object));
			ABLi_pushInteger(0);
			}	
		else
			ABLi_pushInteger(1);
	}
}

void execCreateInfantry (void) {

	long teamID = ABLi_popInteger();
	long commanderID = ABLi_popInteger();
	float* worldPos = ABLi_popRealPtr();
	long buildingPartID = ABLi_popInteger();

	MoverInitData moverData;
	memset(&moverData, 0, sizeof(MoverInitData));
	strcpy(moverData.pilotFileName, "pmw00080");
	strcpy(moverData.brainFileName, "infantry001");
	strcpy(moverData.csvFileName, "PM101100");
	moverData.objNumber = 53;
	moverData.rosterIndex = 255;
	moverData.controlType = 2;
	moverData.controlDataType = 0;
	moverData.variant = 0;
	moverData.position.x = worldPos[0];
	moverData.position.y = worldPos[1];
	moverData.position.z = 0.0;
	moverData.rotation = 0.0;
	moverData.teamID = teamID;
	moverData.commanderID = commanderID;
	moverData.gestureID = 2;
	moverData.active = true;
	moverData.exists = true;
	moverData.icon = 0;
	moverData.capturable = false;
	long infantryHandle = mission->addMover(&moverData);

	MoverPtr infantry = static_cast<MoverPtr>(ObjectManager->get(infantryHandle));
	if (infantry)
		infantry->getPilot()->setIntegerMemory(0, buildingPartID);

	ABLi_pushInteger(0);
}

void execGetSensorsWorking (void) {

	//Returns whether or not sensors are operative for object passed in
	//		PARAMS:	integer
	//		Returns: integer (result)

	long objectNum = ABLi_popInteger();

	long result = -1;
	GameObjectPtr object1 = getObject(objectNum);
	if (object1 && object1->isMover() && (static_cast<MoverPtr>(object1))->sensorSystem)
		result = (static_cast<MoverPtr>(object1))->sensorSystem->enabled();
	
	ABLi_pushInteger(result);
}	

void execGetCurrentBRValue (void) {

	//Returns the current BR value for the object passed in.
	//		PARAMS:	integer
	//		Returns: integer (result)

	long objectNum = ABLi_popInteger();

	long result = -1;
	GameObjectPtr object1 = getObject(objectNum);
	if (object1)
		result = object1->getCurCV();

	ABLi_pushInteger(result);		
}	

void execSetCurrentBRValue (void) {

	//Sets the current BR value for the object passed in.
	//		PARAMS:	integer, integer
	//		Returns: None

	long objectNum = tos->integer;
	long newBRValue = tos->integer;

	GameObjectPtr object1 = getObject(objectNum);
	if (object1)
		object1->setCurCV(newBRValue);
}	

void execGetArmorPts (void) {

	//Returns the current number of armor points remaining on the object passed in
	//		PARAMS:	integer
	//		Returns: integer (result)

	long objectNum = ABLi_peekInteger();

	GameObjectPtr obj = getObject(objectNum);
	long armorPts = 0;
	ABLi_pokeInteger(0);	
	if (obj && obj->isMover()) {
		MoverPtr mover = static_cast<MoverPtr>(obj);
		for (long i = 0; i < mover->numArmorLocations; i++)
			armorPts += mover->armor[i].curArmor;
		ABLi_pokeInteger(armorPts);
	}
}	

void execGetMaxArmor (void) {

	//Returns the max number of armor points on the object passed in
	//		PARAMS:	integer
	//		Returns: integer (result)

	long objectNum = ABLi_peekInteger();

	ABLi_pokeInteger(0);
	GameObjectPtr obj = getObject(objectNum);
	long armorPts = 0;
	if (obj && obj->isMover()) {
		MoverPtr mover = static_cast<MoverPtr>(obj);
		for (long i = 0; i < mover->numArmorLocations; i++)
			armorPts += mover->armor[i].maxArmor;
		ABLi_pokeInteger(armorPts);
	}
}	

void execGetPilotID (void) {

	//Returns the pilot ID of the driver of the object passed in.
	//		PARAMS:	integer
	//		Returns: integer (result)

	long objectNum = ABLi_peekInteger();

	GameObjectPtr obj = getObject(objectNum);
	ABLi_pokeInteger(-1);
	if (obj) {
		MechWarriorPtr pilot = obj->getPilot();
		if (pilot)
			ABLi_pokeInteger(pilot->getIndex());
	}
}	

void execGetPilotWounds (void) {

	//Returns number of wounds pilot of vehicle has sustained.
	//		PARAMS:	integer
	//		Returns: real (result)

	long objectNum = ABLi_peekInteger();

	GameObjectPtr obj = getObject(objectNum);
	ABLi_pokeReal(0.0);
	if (obj) {
		MechWarriorPtr pilot = obj->getPilot();
		if (pilot)
			ABLi_pokeReal(pilot->getWounds());
	}
}	

void execSetPilotWounds (void) {

	//Returns number of wounds pilot of vehicle has sustained.
	//		PARAMS:	integer
	//		Returns: real (result)

	long objectNum = ABLi_popInteger();
	long wounds = ABLi_popInteger();

	if (wounds > 6)
		wounds = 6;
		
	GameObjectPtr obj = getObject(objectNum);
	if (obj) {
		MechWarriorPtr pilot = obj->getPilot();
		if (pilot)
			pilot->setWounds(wounds);
	}
}	

void execGetObjectActive (void) {

	//Checks to see if the object(s) passed in are active
	//		PARAMS:	integer, real
	//		Returns: integer (result)

	long objectId = ABLi_popInteger();

	long result = 0;
	GameObjectPtr object1 = nullptr;
	if ((objectId >= MIN_UNIT_PART_ID) && (objectId <= MAX_UNIT_PART_ID)) {
		long numObjects = getMovers(objectId, moverList);
		long numAwake = 0;
		for (long i = 0; i < numObjects; i++)
			if (moverList[i]->getAwake())
				numAwake++;
		result = numAwake;
		}
	else {
		object1 = getObject(objectId);
		if (object1 && object1->getAwake())
			result = 1;
	}

	ABLi_pushInteger(result);
}	

void execGetObjectDamage (void) {

	//Returns the damage level of the object passed in
	//		PARAMS:	integer
	//		Returns: integer (result)

	long objectNum = ABLi_peekInteger();

	ABLi_pokeInteger(0);
	GameObjectPtr obj = getObject(objectNum);
	if (obj && obj->isTerrainObject()) {
		float dmgLevel = obj->getDamageLevel();
		//Assert(dmgLevel > 0.0, (long)dmgLevel, " ABL.execHbGetObjDamage: ZERO damageLevel ");
		float dmgResult = 100.0;
		if (dmgLevel > 0.0)
			dmgResult = floor((obj->getDamage() / dmgLevel) * 100.0);
		ABLi_pokeInteger(static_cast<long>(dmgResult));
	}
}	

void execGetObjectDmgPts (void) {

	//Returns the damage points of the object passed in
	//		PARAMS:	integer
	//		Returns: integer (result)

	long objectNum = ABLi_peekInteger();

	ABLi_pokeInteger(0);
	GameObjectPtr obj = getObject(objectNum);
	if (obj && obj->isTerrainObject()) {
		long dmgResult = static_cast<long>(obj->getDamage());
		ABLi_pokeInteger(dmgResult);
	}
}	

void execGetObjectMaxDmg (void) {

	//Returns the damage level of the object passed in
	//		PARAMS:	integer
	//		Returns: integer (result)

	long objectNum = ABLi_peekInteger();

	ABLi_pokeInteger(0);
	GameObjectPtr obj = getObject(objectNum);
	if (obj) {
		float damage = obj->getDamage();
		ABLi_pokeInteger(static_cast<long>(damage));
	}
}	

void execSetObjectDamage (void) {

	//Sets the damage level of the object passed in
	//Note that the damage can only be given, not taken away!
	//		PARAMS:	integer, integer
	//		Returns: none

	long objectNum = ABLi_popInteger();
	long dmgPercentage = ABLi_popInteger();
	if (dmgPercentage > 100)
		dmgPercentage = 100;

	GameObjectPtr obj = getObject(objectNum);
	if (obj && (dmgPercentage > 0)) {
		float maxDmg = obj->getDamageLevel();
		float curDmg = obj->getDamage();
		float dmgPoints = (static_cast<float>(dmgPercentage) / 100.0) * maxDmg - curDmg;
		dmgPoints += 1.0f;	//One to be sure.
		if (dmgPoints > 0.0) 
		{
			WeaponShotInfo shot;
			shot.init(0, -1, dmgPoints, 0, 0);
			if (MPlayer) 
			{
				if (MPlayer->isServer()) 
				{
					obj->handleWeaponHit(&shot, true);
				}
			}
			else
				obj->handleWeaponHit(&shot);
		}
	}
}	

void execGetGlobalValue (void) {

	//Gets the value stored in global variable specified by passed in number.
	//		PARAMS:	integer
	//		Returns: real (result)

	long variableNum = ABLi_peekInteger();

	ABLi_pokeReal(0.0);
	if ((variableNum >= 0) && (variableNum < MAX_GLOBAL_MISSION_VALUES))
		ABLi_pokeReal(globalMissionValues[variableNum]);
}	

void execSetGlobalValue (void) {

	//Sets the value stored in global variable specified by passed in number.
	//		PARAMS:	integer, real
	//		Returns: nothing

	long variableNum = ABLi_popInteger();
	float value = ABLi_popReal();

	if ((variableNum >= 0) && (variableNum < MAX_GLOBAL_MISSION_VALUES))
		globalMissionValues[variableNum] = value;
}	

void execSetObjectivePos (void) {

	//Sets the position for objective passed in.
	//		PARAMS:	integer, real, real, real
	//		Returns: nothing

	long variableNum = ABLi_popInteger();
	float X = ABLi_popReal();
	float Y = ABLi_popReal();
	float Z = ABLi_popReal();

	mission->setObjectivePos(variableNum, X, Y, Z);
}	

void execSetSensorRange (void) {

	//	SET SENSOR RANGE function:
	//		Sets the sensor range of the object.
	//		PARAMS:	integer				object id
	//				real				range
	//		RETURN: NONE

	long objectId = ABLi_popInteger();
	float range = ABLi_popReal();

	GameObjectPtr obj = getObject(objectId);
	if (obj)
		obj->setSensorRange(range);
}	

void execSetTonnage (void) {

	//	SET TONNAGE function:
	//		Sets the tonnage of the object.
	//		PARAMS:	integer				object id
	//				real				tons
	//		RETURN: nothing

	long objectId = ABLi_popInteger();
	float tonnage = ABLi_popReal();

	GameObjectPtr object = getObject(objectId);
	if (object)
		object->setTonnage(tonnage);
}	

void execSetExplosionDamage (void) {

	//	SET Explosion Damage function:
	//		Sets the Explosion Damage of the object.
	//		PARAMS:	integer				object id
	//				real				points of damage
	//		RETURN: nothing

	long objectId = ABLi_popInteger();
	float dmg = ABLi_popReal();

	GameObjectPtr obj = getObject(objectId);
	if (obj)
		obj->setExplDmg(dmg);
}	

void execSetExplosionRadius (void) {

	//	SET Explosion Radius function:
	//		Sets the explosion radius of the object.
	//		PARAMS:	integer				object id
	//				real				radius (in Meters)
	//		RETURN: nothing

	long objectId = ABLi_popInteger();
	float radius = ABLi_popReal();

	GameObjectPtr obj = getObject(objectId);
	if (obj)
		obj->setExplRad(radius);
}	

void execSetSalvage (void) {

	//	SET Salvage function:
	//		Sets the salvage for a building object.
	//		PARAMS:	integer				object id 
	//				integer				MasterComponentID of salvage
	//				integer				Number of these components
	//		RETURN: BOOLEAN				True if function succeeds

	ABLi_popInteger();
	ABLi_popInteger();

	ABLi_pokeInteger(0);
#ifdef USE_SALVAGE
	GameObjectPtr obj = getObject(objectId);
	if (obj) {
		SalvageItemPtr tmpSalvage = object->getSalvage();
		SalvageItemPtr newSalvage = new SalvageItem;

		if (newSalvage) {
			ABLi_pokeInteger(1);
			newSalvage->itemID = component;
			newSalvage->numItems = itemCount;
			while (tmpSalvage && tmpSalvage->next)
				tmpSalvage = tmpSalvage->next;

			if (object->getSalvage())
				tmpSalvage->next = newSalvage;
			else
				object->setSalvage(newSalvage);
		}
	}
#endif
}	

void execSetSalvageStatus (void){

	//	SET Salvage function:
	//		Sets a mover on or off the salvage list
	//		PARAMS:	integer				object id 
	//				BOOLEAN				TRUE = on list, FALSE = off
	//		RETURN: BOOLEAN				True if function succeeds

	ABLi_popInteger();

	ABLi_pokeBoolean(false);
#ifdef USE_SALVAGE
	GameObjectPtr obj = getObject(objectId);
	if (obj && TACMAP && (object->isMover() || object->isBuilding())) {
		if (on)
			ABLi_pokeBoolean(TACMAP->AddSalvage(object));
		else
			ABLi_pokeBoolean(TACMAP->RemoveSalvage(object));
	}
#endif
}	

void execSetAnimation (void) {

	//	SET Animation function:
	//		Sets the salvage for a building object.	(Say what?! --jm)
	//		ALL RIGHT, ALLRIGHT!!!!  Sets the animation State and subState. -fs
	//		PARAMS:	integer				object id 
	//				integer				Animation State
	//				integer				Animation Sub State
	//		RETURN: Nothing

	long objectId = ABLi_popInteger();
	ABLi_popInteger();
	ABLi_popInteger();

	GameObjectPtr obj = getObject(objectId);
	if (obj && obj->isBuilding()) {
#ifdef USE_BUILDING_APPR
		(static_cast<BuildingPtr>(building))->setAnimState(newState, newSubState);
#endif
	}
}	

void execSetRevealed (void) {

	//Reveals terrain around point a distance out.
	//		PARAMS:	integer, real, real[2]
	//		Returns: nothing
	
	// Visibility is exact line of sight, so there is no revealed-area state to set; the arguments are still consumed so the script stack stays balanced.
	ABLi_popInteger();
	ABLi_popReal();
	ABLi_popRealPtr();

	// This is sloppy, and we need a better way to update their scans...
	for (long i = 0; i < Team::numTeams; i++)
		SensorManager->getTeamSensor(i)->scanBattlefield();
}
	
void execGetSalvage (void) {

	//	GET Salvage function:
	//		Returns two list of itemIds and quantities of salvage in an object
	//		PARAMS:	integer				building ID
	//				integer				number of item in array
	//				integer[]			item list
	//				integer[]			quantity list
	//		RETURN: none

	long objectId = ABLi_popInteger();
	long itemCount = ABLi_popInteger();
	const auto items = ABLi_popIntBuf();
	const auto quantities = ABLi_popIntBuf();
	if (itemCount < 0) {
		abl::reportBadArgument("getsalvage", "itemCount");
		return;
	}
	itemCount = abl::clampArrayCount(itemCount, std::min(items.size(), quantities.size()), "getsalvage_truncated");
	std::fill_n(items.begin(), itemCount, -1);
	std::fill_n(quantities.begin(), itemCount, -1);
#ifdef USE_SALVAGE
	if (auto* object = getObject(objectId)) {
		auto* salvage = object->getSalvage();
		for (long i = 0; salvage && i < itemCount; ++i, salvage = salvage->next) {
			items[i] = salvage->itemID;
			quantities[i] = salvage->numItems;
		}
	}
#else
	(void)objectId;
#endif
}

void execOrderRefit (void) {

	//	Refit function:
	//		Current Object starts refiting mover
	//		PARAMS:	integer				mover ID
	//				integer				order params
	//		RETURN: none

	long refiteeId = ABLi_popInteger();
	long params = ABLi_popInteger();

	if (CurObject) {
		MechWarriorPtr pilot = CurObject->getPilot();
		if (pilot) {
			GameObjectPtr refitee = getObject(refiteeId);
			if (refitee && refitee->getObjectClass() == BATTLEMECH)
				pilot->orderRefit(ORDER_ORIGIN_COMMANDER, refitee, params);
		}
	}
}

void execSetCaptured (void) {

	//	Set Captured function:
	//		Sets an objects captured flag to TRUE, and starts the capture display
	//		PARAMS:	integer				building ID

	long objectId = ABLi_popInteger();

	GameObjectPtr object = getObject(objectId);
	if (object)
		object->setCaptured(true);
}

void execOrderCapture (void) {

	//	Capture Object function:
	//		Sets CurObject to capture object specified by target ID
	//		PARAMS:	integer				target ID
	//				integer				order params
	//		RETURN:	NONE

	long targetId = ABLi_popInteger();
	long params = ABLi_popInteger();

	GameObjectPtr target = nullptr;
	if (CurObject && CurObject->isMover())
		target = getObject(targetId);

	if (target)
		CurObject->getPilot()->orderCapture(ORDER_ORIGIN_COMMANDER, target, -1, params);
}

void execSetCapturable (void) {

	//	Set Capturable function:
	//		Sets an objects captureabl flag to captureable.
	//		PARAMS:	integer				building ID
	//				boolean				captureable

	long objectId = ABLi_popInteger();
	ABLi_popBoolean();

	GameObjectPtr obj = getObject(objectId);
	if (obj) {
		if (MPlayer)
			obj->clearCaptured();
		if (obj->isBuilding() || (obj->getObjectClass() == GROUNDVEHICLE))
			obj->setFlag(OBJECT_FLAG_CAPTURABLE, true);
	}
}

void execIsCaptured (void) {

	//	Is Captured function:
	//		Returns the number of captured object in the group 
	//		(0 or 1 if called for a separate object)
	//		PARAMS:	integer				object ID
	//		RETURN: integer

	long objectId = ABLi_peekInteger();

	long numCaptured = 0;
	if (isUnitId(objectId)) {
		long numObjects = getMovers(objectId, moverList);
		for (long i = 0; i < numObjects; i++)
			if (moverList[i]->isCaptured())
				numCaptured++;
		}
	else {
		GameObjectPtr obj = getObject(objectId);
		if (obj && obj->isCaptured())
			numCaptured++;
	}

	ABLi_pokeInteger(numCaptured);
}

void execIsCapturable (void) {

	//	Is Capturable function:
	//		Returns whether or not an object is capturable
	//		PARAMS:	integer				object ID
	//				integer				capturing team ID
	//		RETURN: BOOL

	long objectId = ABLi_popInteger();
	long capturingTeamID = ABLi_popInteger();

	ABLi_pushBoolean(false);
	GameObjectPtr obj = getObject(objectId);
	if (capturingTeamID == -2) {
		if (obj && obj->getFlag(OBJECT_FLAG_CAPTURABLE) && !obj->isDestroyed())
			ABLi_pokeBoolean(true);
	}
	else {
		if (obj && obj->isCaptureable(capturingTeamID))
			ABLi_pokeBoolean(true);
	}
}

void execWasEverCapturable (void) {

	//	Was Ever Is Capturable function:
	//		Returns whether or not object was ever capturable
	//			(regardless of whether it's dead, currently captured, etc.)
	//			Only applies to vehicles!
	//		PARAMS:	integer				object ID
	//		RETURN: BOOL

	long objectId = ABLi_peekInteger();

	GameObjectPtr obj = getObject(objectId);
	ABLi_pokeBoolean(false);
	if (obj && obj->getFlag(OBJECT_FLAG_CAPTURABLE))
		ABLi_pokeBoolean(true);
}

void execSetBuildingName (void) {

	//	SetBuildingName Function:
	//		Sets a building's name
	//		PARAMS:	integer				building ID
	//				integer				name Resource ID

	long objectId = ABLi_popInteger();
	ABLi_popInteger();

	GameObjectPtr obj = getObject(objectId);
	if (obj) {
#ifdef USE_RESOURCE_STRINGS
		char tmpString[255];
		cLoadString(thisInstance,newName+languageOffset,tmpString,254);
		obj->setName(tmpString);
#endif
	}
}

void execCallStrike (void) {

	//	CallStrike function:
	//		calls an air or artillery strike at the specified location
	//		PARAMS:	integer			strike type (248 = big arty, 249 = sm arty, 250 sensor arty,
	//											 507 = big air strike, 508 = sm air, 509 = sensor air
	//				integer			id of target object
	//				real			x coord of strike
	//				real			y coord of strike
	//				real			z coord of strike
	//				bool			clan (is this coming from the clan?)
	//		RETURN: nothing

	// Since artillery strikes are commander(player) based, not team based,
	// & current missions are coded to assume team based, we'll just
	// fatal if we're not in a campaign mission. Then, we can decide if
	// it's worth re-coding ABL scripts (since we'll have to change the
	// parameters for this call to specify the player that's calling the
	// strike)...
	if (MPlayer)
		Fatal(0, " ABL: Calling ArtilleryStrike in Multiplayer game ");

	long strikeType = ABLi_popInteger();
	long targetId = ABLi_popInteger();
	glm::vec3 strikeLoc;
	strikeLoc.x = ABLi_popReal();
	strikeLoc.y = ABLi_popReal();
	strikeLoc.z = ABLi_popReal();
	bool clanStrike = ABLi_popBoolean();

	GameObjectPtr target = getObject(targetId);

	if (target)
		IfaceCallStrike(strikeType, nullptr, target, false);
	else {
		strikeLoc.z = land->getTerrainElevation(strikeLoc);
		IfaceCallStrike(strikeType, &strikeLoc, nullptr, false, clanStrike);
	}
}

void execCallStrikeEx (void) {

	//	CallStrikeEx function:
	//		calls an air or artillery strike at the specified location delayed by time
	//		PARAMS:	integer			strike type (248 = big arty, 249 = sm arty, 250 sensor arty,
	//											 507 = big air strike, 508 = sm air, 509 = sensor air
	//				integer			id of target object
	//				real			x coord of strike
	//				real			y coord of strike
	//				real			z coord of strike
	//				bool			clan (is this coming from the clan?)
	//				real			time to impact
	//		RETURN: nothing

	// Since artillery strikes are commander(player) based, not team based,
	// & current missions are coded to assume team based, we'll just
	// fatal if we're not in a campaign mission. Then, we can decide if
	// it's worth re-coding ABL scripts (since we'll have to change the
	// parameters for this call to specify the player that's calling the
	// strike)...
	if (MPlayer)
		Fatal(0, " ABL: Calling ArtilleryStrike in Multiplayer game ");

	long strikeType = ABLi_popInteger();
	long targetId = ABLi_popInteger();
	glm::vec3 strikeLoc;
	strikeLoc.x = ABLi_popReal();
	strikeLoc.y = ABLi_popReal();
	strikeLoc.z = ABLi_popReal();
	bool clanStrike = ABLi_popBoolean();
	float timeToImpact = ABLi_popReal();

	if (timeToImpact < 0.0)
		timeToImpact = 0.0;

	GameObjectPtr target = getObject(targetId);

	if (target)
		IfaceCallStrike(strikeType, nullptr, target, false, clanStrike, timeToImpact);
	else {
		strikeLoc.z = land->getTerrainElevation(strikeLoc);
		IfaceCallStrike(strikeType, &strikeLoc, nullptr, false, clanStrike, timeToImpact);
	}
}

void execOrderLoadElementals (void) {

	// LoadElementals function:	tells elementals of selected group to load into specified carrier
	//		PARAMS:	integer			carrier id
	//		RETURN: nothing

	ABLi_popInteger();

#ifdef USE_ELEMENTALS
	GameObjectPtr carrior = nullptr;
	if (CurObject && CurObject->getObjectClass() == ELEMENTAL)
		carrier = getObject(carrierID);

	if (carrier && carrier->getObjectClass() == GROUNDVEHICLE && (static_cast<GroundVehiclePtr>(carrier))->elementalCarrier)
		CurObject->getPilot()->orderLoadIntoCarrier(ORDER_ORIGIN_COMMANDER, carrier);
#endif
}

void execOrderDeployElementals (void) {

	// CallStrike function:	tells elemental carrier to deploy any loaded elementals
	//		PARAMS:	real[3]			coord of deployment location
	//				integer			misc. order params (set to 0, typically)
	//		RETURN: nothing

	ABLi_popInteger();

#ifdef USE_ELEMENTALS
	if (CurObject && CurObject->getObjectClass() == GROUNDVEHICLE && (static_cast<GroundVehiclePtr>(CurObject))->elementalCarrier)
		CurObject->getPilot()->orderDeployElementals(ORDER_ORIGIN_COMMANDER, params);
#endif
}

void execAddPrisoner (void) {

	// AddPrisoner function:	adds specified prisoner to specified building
	//		PARAMS:	integer		building id
	//				integer		prisoner id
	//		RETURN: 0 if successful

	ABLi_popInteger();

	ABLi_pokeInteger(-1);

#ifdef USE_PRISONS
	if (prison && prison->isBuilding() && mission)
	{
		for (i=1; i<=scenario->getNumWarriors(); i++)
		{
			prisoner = scenario->getWarrior(i);
			if (prisoner && prisoner->getIndex() == prisonerID)
				break;
			prisoner = nullptr;
		}
		if (prisoner)
		{
			if (prison->getObjectClass() == BUILDING)
			{
			BuildingPtr bPrison = static_cast<BuildingPtr>(prison);
				
				for (i=0; i<MAX_PRISONERS; i++)
					if (bPrison->prisoners[i] == nullptr)
					{
						bPrison->prisoners[i] = prisoner;
						result = 0;
					}
			}
			if (prison->getObjectClass() == TREEBUILDING)
			{
			TreeBuildingPtr tPrison = reinterpret_cast<TreeBuildingPtr>(prison);
				
				for (i=0; i<MAX_PRISONERS; i++)
					if (tPrison->prisoners[i] == nullptr)
					{
						tPrison->prisoners[i] = prisoner;
						result = 0;
					}
			}
		}
	}
#endif
}

void execLockGateOpen (void) {

	// LockGateOpen function:	Forces a gate to be Open Forever
	//		PARAMS:	integer		gate id
	//		RETURN: none

	ABLi_popInteger();

#ifdef USE_GATES
	GameObjectPtr obj = getObject(gateID);
	if (obj && obj->getObjectClass() == GATE)
		(static_cast<GatePtr>(obj))->setLockedOpen();
#endif
}

void execLockGateClosed (void) {

	// LockGateOpen function:	Forces a gate to be Closed Forever
	//		PARAMS:	integer		gate id
	//		RETURN: none

	ABLi_popInteger();

#ifdef USE_GATES
	GameObjectPtr obj = getObject(gateID);
	if (obj && obj->getObjectClass() == GATE)
		(static_cast<GatePtr>(obj))->setLockedClose();
#endif
}

void execReleaseGateLock (void) {

	// LockGateOpen function:	Releases any lock on a gate.
	//		PARAMS:	integer		gate id
	//		RETURN: none

	ABLi_popInteger();

#ifdef USE_GATES
	GameObjectPtr obj = getObject(gateID);
	if (obj && obj->getObjectClass() == GATE)
		(static_cast<GatePtr>(obj))->releaseLocks();
#endif
}
