// MechWarrior: target priority list, attacker/target bookkeeping and weapons-status queries.
// Declared in the class headers; shared names in warrior_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "warrior_internal.h"
#include "sim_trace.h"
#include "mc2fmt.h"







void TargetPriorityList::init (void) {

	size = 0;
}

void TargetPriorityList::destroy (void) {

	size = 0;
}

int32_t TargetPriorityList::insert (int32_t index, TargetPriorityPtr priority) {

	if (size == MAX_TARGET_PRIORITIES)
		return(1);
	if (index == -1)
		index = size;
	if (index > size)
		return(2);

	if (index < size) {
		memmove(&list[index + 1], &list[index], (size - index) * sizeof(TargetPriority));
	}
	memcpy(&list[index], priority, sizeof(TargetPriority));
	size++;
	return(0);
}

void TargetPriorityList::remove (int32_t index) {

	if ((index > -1) && (index < size)) {
		if (index < (size - 1)) {
			memmove(&list[index], &list[index + 1], (size - index - 1) * sizeof(TargetPriority));
		}
		size--;
	}
};

int32_t TargetPriorityList::calcAction (MechWarriorPtr pilot, GameObjectPtr target) {

	if (target->isMover()) {
		if (!pilot->getTeam() || pilot->getTeam()->isEnemy(target->getTeam())) {
			if (!target->isDisabled())
				return(TACTICAL_ORDER_ATTACK_OBJECT);

		}
		}
	else {
		switch (target->getObjectClass()) {
			case BUILDING:
				if (target->getTeam() != pilot->getTeam()) {
					if (target->getFlag(OBJECT_FLAG_CONTROLBUILDING))
						return(TACTICAL_ORDER_CAPTURE);
					return(TACTICAL_ORDER_ATTACK_OBJECT);
				}
				break;
			case TURRET:
				break;
		}
	}
	return(TACTICAL_ORDER_NONE);
}

int32_t TargetPriorityList::calcTarget (MechWarriorPtr pilot, glm::vec3 location, int32_t contactCriteria, int32_t& action) {

	if (!pilot->getTeam())
		return(0);

	for (long i = 0; i < size; i++) {
		switch (list[i].type) {
			case TARGET_PRIORITY_NONE:
				return(TARGET_PRIORITY_NONE);
				break;
			case TARGET_PRIORITY_GAMEOBJECT: {
				// param 0 = object WID
				// param 1 = engage radius
				// param 2 = contact criteria
				GameObjectPtr obj = nullptr;
				if (list[i].params[0])
					obj = ObjectManager->findByPartId(list[i].params[0]);
				//HACK! For now, we will not target unarmed movers...
				list[i].params[2] += CONTACT_CRITERIA_ARMED;
				/*
				if (!obj) {
					// select best gameobject that fits params
					if (list[i].params[2]) {
						// for now, just returns the closest contact...
						long contactList[MAX_MOVERS];
						long numContacts = CurObject->getContacts(contactList, list[i].params[2], CONTACT_SORT_DISTANCE);
						float smallestDistance = 999999.0;
						for (long i = 0; i < numContacts; i++) {
							GameObjectPtr contact = ObjectManager->get(contactList[i]);
							float dist = pilot->getVehicle()->distanceFrom(contact->getPosition());
							if (dist < smallestDistance) {
								obj = contact;
								smallestDistance = dist;
							}
						}
						}
					else {
						Fatal(0, " TargetPriorityList.calcTarget: bad criteria for mover target ");
					}
				}
				*/
				if (obj) 
				{
					if (pilot->getVehicle()->distanceFrom(obj->getPosition()) < list[i].params[1]) 
					{
						if (list[i].params[2] && obj->isMover() && (!obj->isMarine() || (obj->getTeam() != Team::home)))
						{
							if (pilot->getTeam()->isContact(pilot->getVehicle(), static_cast<MoverPtr>(obj), list[i].params[2]))
							{
								action = calcAction(pilot, obj);
								return(obj->getWatchID());
							}
						}
						else
							return(obj->getWatchID());
					}
				}
				}
				break;
			case TARGET_PRIORITY_MOVER: {
				// param 0 = object WID
				// param 1 = engage radius
				// param 2 = contact criteria
				GameObjectPtr obj = nullptr;
				if (list[i].params[0])
					obj = ObjectManager->findByPartId(list[i].params[0]);
				//HACK! For now, we will not target unarmed movers...
				list[i].params[2] += CONTACT_CRITERIA_ARMED;
				if (!obj) {
					if (list[i].params[2]) {
						int contactList[MAX_MOVERS];
						long numContacts = CurObject->getContacts(contactList, list[i].params[2], CONTACT_SORT_DISTANCE);
						float smallestDistance = 999999.0;
						for (long i = 0; i < numContacts; i++) {
							GameObjectPtr contact = ObjectManager->get(contactList[i]);
							if (!contact->isMarine() || (contact->getTeam() != Team::home))
							{
								float dist = pilot->getVehicle()->distanceFrom(contact->getPosition());
								if (dist < smallestDistance) {
									obj = contact;
									smallestDistance = dist;
								}
							}
						}
						}
					else {
						Fatal(0, " TargetPriorityList.calcTarget: bad criteria for mover target ");
					}
				}
				if (obj && obj->isMover()) {
					if (pilot->getVehicle()->distanceFrom(obj->getPosition()) < list[i].params[1]) {
						if (list[i].params[2]) {
							if (pilot->getTeam()->isContact(pilot->getVehicle(), static_cast<MoverPtr>(obj), list[i].params[2])) {
								action = calcAction(pilot, obj);
								return(obj->getWatchID());
							}
							}
						else
							return(obj->getWatchID());
					}
				}
				}
				break;
			case TARGET_PRIORITY_BUILDING: {
				// param 0 = object WID
				// param 1 = engage radius
				// param 2 = if -1, looks for any enemy building. Else, specified alignment.
				GameObjectPtr obj = nullptr;
				if (list[i].params[0])
					obj = ObjectManager->findByPartId(list[i].params[0]);
				if (!obj) {
					if (list[i].params[2] == -1) {
						float smallestDistance = 999999.0;
						for (long j = 0; j < ObjectManager->getNumBuildings(); j++) {
							BuildingPtr building = ObjectManager->getBuilding(j);
							if (building->isEnemy(pilot->getTeam()))
								if (building->getObjectType()->getSubType() != BUILDING_SUBTYPE_WALL)
									if (!building->isDestroyed()) {
										float dist = pilot->getVehicle()->distanceFrom(building->getPosition());
										if (dist < smallestDistance) {
											obj = building;
											smallestDistance = dist;
										}
									}
						}
						}
					else {
						Fatal(0, " TargetPriorityList.calcTarget: bad criteria for mover target ");
					}
				}
				if (obj) {
					if (pilot->getVehicle()->distanceFrom(obj->getPosition()) < list[i].params[1])
						return(obj->getWatchID());
				}
				}
				break;
			case TARGET_PRIORITY_CURTARGET: {
				// param 0 = disengage relative to point # (-1 for cur position)
				// param 1 = disengage radius
				// param 2 = contact criteria (0 for doesn't matter)
				GameObjectPtr target = nullptr;
				// For now, the current order's target is considered the "current target",
				// even if the mover is moving while shooting. This can be changed easily,
				// if we want it to act differently...
				if (pilot->getCurTacOrder()->code != TACTICAL_ORDER_NONE) {
					target = ObjectManager->get(pilot->getCurTacOrder()->targetWID);
					if (target && target->isDisabled())
						if (!pilot->getCurTacOrder()->attackParams.obliterate)
							target = nullptr;
				}
				if (!target) {
					target = pilot->getLastTarget();
				}
				if (target) {
					glm::vec3 disengageCenter;
					glm::vec3 targetPosition;
					if (list[i].params[0] == -1)
						disengageCenter = pilot->getVehicle()->getPosition();
					else
						disengageCenter = pilot->getVehicle()->getPosition(); //FOR NOW... should ref point list
					targetPosition = target->getPosition();
					glm::vec3 delta;
					delta.x = disengageCenter.x - targetPosition.x;
					delta.y = disengageCenter.y - targetPosition.y;
					delta.z = 0.0;
					float distanceToTarget = glm::length(delta) * metersPerWorldUnit;
					if (distanceToTarget < list[i].params[1]) {
						if (list[i].params[2] && target->isMover()) {
							if (pilot->getTeam()->isContact(pilot->getVehicle(), static_cast<MoverPtr>(target), list[i].params[2])) {
								action = calcAction(pilot, target);
								return(target->getWatchID());
							}
							}
						else {
							action = calcAction(pilot, target);
							return(target->getWatchID());
						}
					}
				}
				}
				break;
			case TARGET_PRIORITY_TURRET: {
				float shortestDistance = 999999.0;
				GameObjectPtr closestTurret = nullptr;
                int i;
				for (i = 0; i < ObjectManager->getNumTurrets(); i++) {
					GameObjectPtr turret = static_cast<GameObjectPtr>(ObjectManager->getTurret(i));
					if (!turret->getTeam()->isFriendly(pilot->getTeam())/* && turret->isCaptureable(pilot->getTeam()->getId())*/) {
						float distance = pilot->getVehicle()->distanceFrom(turret->getPosition());
						if (distance < shortestDistance) {
							shortestDistance = distance;
							closestTurret = turret;
						}
					}
				}
				if (closestTurret) {
					if (pilot->getVehicle()->distanceFrom(closestTurret->getPosition()) < list[i].params[1]) {
						action = calcAction(pilot, closestTurret);
						return(closestTurret->getWatchID());
					}
				}
				return(0);
				}
				break;
			case TARGET_PRIORITY_TURRET_CONTROL: {
				// For now, we'll pick the closest Turret Control...
				// WE CANNOT ASSUME CAPTURE OR ATTACK HERE! FIX BELOW...
				float shortestDistance = 999999.0;
				GameObjectPtr closestControl = nullptr;
				for (long i = 0; i < ObjectManager->getNumTurretControls(); i++) {
					GameObjectPtr turretControl = static_cast<GameObjectPtr>(ObjectManager->getTurretControl(i));
					if (!turretControl->getTeam()->isFriendly(pilot->getTeam()) && turretControl->isCaptureable(pilot->getTeam()->getId())) {
						float distance = pilot->getVehicle()->distanceFrom(turretControl->getPosition());
						if (distance < shortestDistance) {
							shortestDistance = distance;
							closestControl = turretControl;
						}
					}
				}
				if (closestControl) {
					action = calcAction(pilot, closestControl);
					return(closestControl->getWatchID());
				}
				}
				break;
			case TARGET_PRIORITY_GATE:
				break;
			case TARGET_PRIORITY_GATE_CONTROL: {
				// For now, we'll pick the closest Turret Control...
				// WE CANNOT ASSUME CAPTURE OR ATTACK HERE! FIX BELOW...
				float shortestDistance = 999999.0;
				GameObjectPtr closestControl = nullptr;
				for (long i = 0; i < ObjectManager->getNumGateControls(); i++) {
					GameObjectPtr gateControl = static_cast<GameObjectPtr>(ObjectManager->getGateControl(i));
					if (!gateControl->getTeam()->isFriendly(pilot->getTeam()) && gateControl->isCaptureable(pilot->getTeam()->getId())) {
						float distance = pilot->getVehicle()->distanceFrom(gateControl->getPosition());
						if (distance < shortestDistance) {
							shortestDistance = distance;
							closestControl = gateControl;
						}
					}
				}
				if (closestControl) {
					action = calcAction(pilot, closestControl);
					return(closestControl->getWatchID());
				}
				}
				break;
			case TARGET_PRIORITY_SALVAGE:
				break;
			case TARGET_PRIORITY_MECHBAY:
				break;
			case TARGET_PRIORITY_LOCATION:
				break;
			case TARGET_PRIORITY_EVENT_TARGETED:
				break;
			case TARGET_PRIORITY_EVENT_HIT:
				// param 0 = selection criteria
				// param 1 = engage radius
				// param 2 = contact criteria
/*				GameObjectPtr obj = NULL;
				if (list[i].params[0])
					obj = (GameObjectPtr)ObjectManager->get(list[i].params[0]);
				if (!obj) {
					// select best gameobject that fits params
					if (list[i].params[2]) {
						// for now, just returns the closest contact...
						long contactList[MAX_MOVERS];
						long numContacts = CurObject->getContacts(contactList, list[i].params[2], CONTACT_SORT_DISTANCE);
						float smallestDistance = 999999.0;
						for (long i = 0; i < numContacts; i++) {
							GameObjectPtr contact = ObjectManager->get(contactList[i]);
							float dist = pilot->getVehicle()->distanceFrom(contact->getPosition());
							if (dist < smallestDistance) {
								obj = contact;
								smallestDistance = dist;
							}
						}
						}
					else {
						Fatal(0, " TargetPriorityList.calcTarget: bad criteria for mover target ");
					}
				}
				if (obj) {
					if (pilot->getVehicle()->distanceFrom(obj->getPosition()) < list[i].params[1]) {
						if (list[i].params[2] && obj->isMover()) {
							if (pilot->getTeam()->isContact(pilot->getVehicle(), (MoverPtr)obj, list[i].params[2])) {
								action = calcAction(pilot, obj);
								return(obj->getWatchID());
							}
							}
						else
							return(obj->getWatchID());
					}
				}
				}
				*/break;
			case TARGET_PRIORITY_EVENT_DAMAGED:
				break;
			case TARGET_PRIORITY_EVENT_MATE_DIED:
				break;
			case TARGET_PRIORITY_EVENT_FRIENDLY_DISABLED:
				break;
			case TARGET_PRIORITY_EVENT_FRIENDLY_DESTROYED:
				break;
			case TARGET_PRIORITY_EVENT_FRIENDLY_WITHDRAWS:
				break;
			case TARGET_PRIORITY_SKIP:
				break;
		}
	}

	return(0);
}

void MechWarrior::updateAttackerStatus (unsigned long attackerWID, float time) {

	long curAttacker;
	for (curAttacker = 0; curAttacker < numAttackers; curAttacker++)
		if (attackers[curAttacker].WID == attackerWID)
			break;
	if (curAttacker == numAttackers) {
		if (numAttackers == MAX_ATTACKERS)
			return;
		attackers[numAttackers++].WID = attackerWID;
	}
	attackers[curAttacker].lastTime = time;
}

GameObjectPtr MechWarrior::calcTurretThreats (float threatRange, long minThreat) {

	if (minThreat < 1)
		minThreat = 1;

	long turretThreat[256];
	long turretControlThreat[256];
	for (int i = 0; i < ObjectManager->getNumTurretControls(); i++)
		turretControlThreat[i] = 0;
	for (int i = 0; i < ObjectManager->getNumTurrets(); i++)
		turretThreat[i] = 0;
	unsigned long vehicleWID = getVehicle()->getWatchID();
	for (int i = 0; i < ObjectManager->getNumTurrets(); i++) {
		Turret* turret = ObjectManager->getTurret(i);
		if (!turret->isDisabled() && (turret->targetWID == vehicleWID)) {
			turretThreat[i] += turret->getThreatRating();
			if (turret->parent)		//Pop-up turrets do NOT have parents!!
				turretControlThreat[(static_cast<BuildingPtr>(ObjectManager->getByWatchID(turret->parent)))->listID] += turret->getThreatRating();
		}
	}

	long biggestThreat = -1;
	for (int i = 0; i < ObjectManager->getNumTurretControls(); i++) {
		BuildingPtr controlBuilding = ObjectManager->getTurretControl(i);
		float distance = getVehicle()->distanceFrom(controlBuilding->getPosition());
		if (distance > threatRange)
			turretControlThreat[i] = 0;
		if (controlBuilding->getTeam() == getTeam())
			turretControlThreat[i] = 0;
		if (controlBuilding->isDisabled())
			turretControlThreat[i] = 0;
		if (turretControlThreat[i] < minThreat)
			turretControlThreat[i] = 0;
		else {
			if (biggestThreat == -1)
				biggestThreat = i;
			else if (turretControlThreat[i] > turretControlThreat[biggestThreat])
				biggestThreat = i;
		}
	}

	if (biggestThreat > -1)
		return(ObjectManager->getTurretControl(biggestThreat));
	return(nullptr);
}

AttackerRecPtr MechWarrior::getAttackerInfo (unsigned long attackerWID) {

	for (long i = 0; i < numAttackers; i++)
		if (attackers[i].WID == attackerWID)
			return(&attackers[i]);
	return(nullptr);
}

long MechWarrior::getAttackers (unsigned int* attackerList, float seconds) {

	long count = 0;
	float earliestTime = scenarioTime - seconds;
	for (long i = 0; i < numAttackers; i++)
		if (attackers[i].lastTime >= earliestTime)
			attackerList[count++] = attackers[i].WID;
	return(count);
	
}

long MechWarrior::setAttackTarget (GameObjectPtr object) {

	if ( object )
		attackOrders.targetWID = object->getWatchID();
	else 
		attackOrders.targetWID = 0;
	
	attackOrders.targetTime = scenarioTime;
	return(NO_ERR);
}

GameObjectPtr MechWarrior::getLastTarget (void) {

	GameObjectPtr target = ObjectManager->getByWatchID(lastTargetWID);

	if (target) {
		bool clearTarget = target->isDestroyed() ||
						   (target->isDisabled() && !lastTargetObliterate) ||
						   (target->isFriendly(getVehicle()) && !lastTargetFriendly);
		if (clearTarget) {
			setLastTarget(nullptr);
			lastTargetTime = -1.0;
			lastTargetObliterate = false;
			lastTargetFriendly = false;
			if (curTacOrder.isCombatOrder())
				clearCurTacOrder();
			return(nullptr);
		}
	}

	if (lastTargetConserveAmmo)
		curTacOrder.attackParams.type = ATTACK_CONSERVING_AMMO;

	return(target);
}

void MechWarrior::setLastTarget (GameObjectPtr target, bool obliterate, bool conserveAmmo) {

	Assert(getVehicle() != nullptr, vehicleWID, " MechWarrior.setLastTarget: NULL vehicle ");
	if (getCommander() == Commander::home) {
		if (lastTargetWID) {
			GameObjectPtr lastTarget = ObjectManager->getByWatchID(lastTargetWID);
			if (lastTarget)
				lastTarget->decrementAttackers();
		}
		if (target)
			target->incrementAttackers();
	}

	MC2_SIM_TRACE_LINE("SETTGT pilot={:d} tgt={:d}", mc2::fmt::as_i32(getVehicle()->getPartId()), mc2::fmt::as_i32((target ? target->getPartId() : -1)));
	if (target) {
		lastTargetWID = target->getWatchID();
		lastTargetFriendly = target->isFriendly(getVehicle());
		}
	else {
		lastTargetWID = 0;
		lastTargetFriendly = false;
	}
	lastTargetTime = scenarioTime;
	lastTargetObliterate = obliterate;
	lastTargetConserveAmmo = conserveAmmo;

}

GameObjectPtr MechWarrior:: getAttackTarget (void) {

	return(ObjectManager->getByWatchID(attackOrders.targetWID));
}

void MechWarrior::setCurrentTarget (GameObjectPtr target) {

	setLastTarget(target);
}

GameObjectPtr MechWarrior::getAttackTargetPosition (glm::vec3& pos) {

	GameObjectPtr object = ObjectManager->getByWatchID(attackOrders.targetWID);
	if (!object) {
		clearAttackOrders();
		return(nullptr);
	}
	pos = object->getPosition();
	return(object);
}

void MechWarrior::clearAttackOrders (void) {

	attackOrders.origin = ORDER_ORIGIN_COMMANDER;
	attackOrders.type = ATTACK_NONE;
	attackOrders.targetWID = 0;
	attackOrders.aimLocation = -1;
	attackOrders.pursue = false;
	attackOrders.targetTime = -1.0;
}

void MechWarrior::clearMoveOrders (void) {

	setMoveGoal(MOVEGOAL_NONE, nullptr);
	setMoveWayPath(nullptr);
	for (long i = 0; i < 2; i++)
		clearMovePath(i);
	setMoveState(MOVESTATE_FORWARD);
	setMoveStateGoal(MOVESTATE_FORWARD);
	setMoveYieldTime(-1.0);
	setMoveYieldState(0);
	setMoveWaitForPointTime(-1.0);
	setMoveTimeOfLastStep(-1.0);
	setMoveTwisting(false);
	setMoveGlobalPath(nullptr, 0);
	PathManager->remove(this);
}

GameObjectPtr MechWarrior::getSituationGuardObject (void) {

	return(ObjectManager->getByWatchID(situationOrders.guardObjectWID));
}

void MechWarrior::setSituationGuardObject (GameObjectWatchID objWID) {

	situationOrders.guardObjectWID = objWID;
}

unsigned long MechWarrior::getSituationGuardObjectPartId (void) {

	GameObjectPtr object = getSituationGuardObject();
	if (!object)
		return(0);
	return(object->getPartId());
}


long MechWarrior::calcWeaponsStatus (GameObjectPtr target, int* weaponList, glm::vec3* targetPoint) {

	MoverPtr myVehicle = getVehicle();
	if (!myVehicle->canFireWeapons())
		return(WEAPONS_STATUS_CANNOT_FIRE);

	glm::vec3 targetPosition;
	if (target)
		targetPosition = target->getPosition();
	else if (targetPoint)
		targetPosition = *targetPoint;
	else
		return(WEAPONS_STATUS_NO_TARGET);

	float distanceToTarget = myVehicle->distanceFrom(targetPosition);
	if ((distanceToTarget <= myVehicle->getMinFireRange()) || (distanceToTarget >= myVehicle->getMaxFireRange()))
		return(WEAPONS_STATUS_OUT_OF_RANGE);

	long numWeaponsWithShot = 0;
	for (long curWeapon = 0; curWeapon < myVehicle->numWeapons; curWeapon++) 
	{
		long weaponIndex = myVehicle->numOther + curWeapon;
		if (!myVehicle->isWeaponReady(weaponIndex))
			weaponList[curWeapon] = WEAPON_STATUS_NOT_READY;
		else 
		{
			long curShots = myVehicle->getWeaponShots(weaponIndex);
			if (curShots < 1)
				weaponList[curWeapon] = WEAPON_STATUS_OUT_OF_AMMO;
			else 
			{
				if (!myVehicle->weaponInRange(weaponIndex, distanceToTarget, MapCellDiagonal))
					weaponList[curWeapon] = WEAPON_STATUS_OUT_OF_RANGE;
				else 
				{
					float relAngle = myVehicle->weaponLocked(weaponIndex, targetPosition);
					float fireArc = myVehicle->getFireArc();
					bool lineOfFire = false;
					//ALWAYS do the SIMPLE calculation first.  If they have indirect fire weapons, direct LOS is Still VALID!
					if (target)
						lineOfFire = myVehicle->lineOfSight(target);
					else
						lineOfFire = myVehicle->lineOfSight(targetPosition);

					//Indirect fire weapons can fire if ANYONE can see target! 
					if (!lineOfFire && myVehicle->getWeaponIndirectFire(weaponIndex))
					{
						if (!myVehicle->getTeam())
							lineOfFire = false;
						else if (target)
							lineOfFire = myVehicle->getTeam()->teamLineOfSight(target->getLOSPosition(), target->getAppearRadius());
						else
							lineOfFire = myVehicle->getTeam()->teamLineOfSight(targetPosition, CELL_DISTANCE);
					}
					
					if (!lineOfFire || (relAngle < -fireArc) || (relAngle > fireArc))
						weaponList[curWeapon] = WEAPON_STATUS_NOT_LOCKED;
					else 
					{
						long aimLocation = -1;
						if (curTacOrder.isCombatOrder())
							aimLocation = curTacOrder.attackParams.aimLocation;
						float odds = myVehicle->calcAttackChance(target, aimLocation, scenarioTime, weaponIndex, 0.0, nullptr, targetPoint);
						if (odds < 1.0)
							weaponList[curWeapon] = WEAPON_STATUS_NO_CHANCE;
						else 
						{
							weaponList[curWeapon] = odds;
							numWeaponsWithShot++;
						}
					}
				}
			}
		}
	}
					
	return(numWeaponsWithShot);
}

void MechWarrior::printWeaponsStatus (char* s) {

	if (weaponsStatusResult == WEAPONS_STATUS_OUT_OF_RANGE)
		sprintf(s, "Out of Range");
	else if (getCurrentTarget() && !getVehicle()->lineOfSight(getCurrentTarget()))
		sprintf(s, "No Line-of-sight");
	else if (weaponsStatusResult == WEAPONS_STATUS_CANNOT_FIRE)
		sprintf(s, "Cannot Fire");
	else if (weaponsStatusResult == WEAPONS_STATUS_NO_TARGET)
		sprintf(s, "No Target");
	else {
		long tally[6] = {0, 0, 0, 0, 0, 0};
		long numWithChance = 0;
		for (long curWeapon = 0; curWeapon < getVehicle()->numWeapons; curWeapon++) {
			if (weaponsStatus[curWeapon] < 0)
				tally[weaponsStatus[curWeapon] + 6]++;
			else if (weaponsStatus[curWeapon] > 0)
				numWithChance++;
		}
		sprintf(s, "#=%02d(%02d),RD=%02d,RN=%02d,LK=%02d,CH=%02d,AM=%02d",
			getVehicle()->numWeapons,
			numWithChance,
			tally[5],
			tally[3],
			tally[2],
			tally[1],
			tally[4]);
	}
}
