// MechWarrior: tac-order calculation, goal manager, save/load and the data copy.
// Declared in the class headers; shared names in warrior_internal.h.
#include "warrior_internal.h"
#include "mc2fmt.h"


BldgAppearance* MechWarrior::getWayPointMarker( const Stuff::Vector3D& pos, const char* name )
{
	int appearanceType = (BLDG_TYPE << 24);
	AppearanceTypePtr buildingAppearanceType = appearanceTypeList->getAppearance(appearanceType, name );

	BldgAppearance* appearance = new BldgAppearance;
	gosASSERT(appearance != nullptr);

	// The only appearance type for buildings is MLR_APPEARANCE.
	gosASSERT(buildingAppearanceType->getAppearanceClass() == BLDG_TYPE);
	
	appearance->init((BldgAppearanceType*)buildingAppearanceType, (GameObjectPtr)this);

	appearance->setObjectParameters( (const_cast<Stuff::Vector3D&>(pos)), 0, 1, 0, 0);
	appearance->recalcBounds();
	appearance->update();

	// Task 6 (Track B): late-spawn registration. Waypoint marker has position
	// set and full init completed — eligible for early static-prop registration.
	// Gated on MC2_STATIC_PROP_LATE_SPAWN_REG; no-op when disabled.
	// M1 Slice (Task 12) routes through adapter so the
	// late-spawn path produces a real RenderObjectHandle via
	// adoptStaticPropRecipe (m5 fix). Handle is unused at this
	// call site; legacy recipe index propagated via OUT param.
	int32_t lateRecipe = -1;
	(void)GameAdapters::StaticProp::syncStaticPropLateSpawn(
		appearance, &lateRecipe);

	return appearance;
}

long MechWarrior::calcTacOrder (long goalAction,
								long goalWID,
								Stuff::Vector3D goalLocation,
								float controlRadius,
								long aggressiveness,
								long searchDepth,
								float turretRange,
								long turretThreat,
								TacticalOrder& newTacOrder) {

	if (getCommander() == Commander::home)
		STOP(("Mechwarrior.runBrain: player pilot using goalplanning"));

	bool canCapture = ((getVehicle()->getObjectClass() != GROUNDVEHICLE) && (getVehicle()->moveType != MOVETYPE_AIR));

	bool alreadyAssessedTurrets = false;
	if (goalWID == 0) {
		if (turretRange > 0.0) {
			GameObjectPtr turretTarget = calcTurretThreats(turretRange, turretThreat);
			if (turretTarget) {
				if (turretTarget->getObjectClass() == TURRET)
					goalAction = GOAL_ACTION_ATTACK;
				else if (turretTarget->getCaptureBlocker(getVehicle()))
					goalAction = GOAL_ACTION_ATTACK;
				else if (canCapture)
					goalAction = GOAL_ACTION_CAPTURE;
				else
					goalAction = GOAL_ACTION_ATTACK;
				goalWID = turretTarget->getWatchID();
				goalLocation.Zero();
			}
			alreadyAssessedTurrets = true;
		}
	}

	// At this point, if we don't have any goal, we're done.
	GameObjectPtr mainTarget = nullptr;
	if (goalWID) {
		mainTarget = ObjectManager->getByWatchID(goalWID);
		if (!mainTarget) {
			goalAction = GOAL_ACTION_NONE;
			goalWID = 0;
			goalLocation.Zero();
			lastGoalPathSize = 0;
			return(-1);
		}
	}

	long currentAction = goalAction;
	long currentWID = goalWID;
	Stuff::Vector3D currentLocation = goalLocation;
	GameObjectPtr currentTarget = mainTarget;

	OrderOriginType orderOrigin = ORDER_ORIGIN_COMMANDER;
	if (getCommander() == Commander::home)
		orderOrigin = ORDER_ORIGIN_PLAYER;

	if (currentAction == GOAL_ACTION_UNDECIDED) {
		if (currentTarget)
			if (currentTarget->isMover())
				if (getTeam()->isEnemy(currentTarget->getTeam())) {
					if (!currentTarget->isDisabled())
						currentAction = GOAL_ACTION_ATTACK;
					}
				else {
					if (!currentTarget->isDisabled())
						currentAction = GOAL_ACTION_GUARD;
				}
			else
				switch (currentTarget->getObjectClass()) {
					case BUILDING:
						if (currentTarget->getFlag(OBJECT_FLAG_CONTROLBUILDING))
							currentAction = GOAL_ACTION_ATTACK; //CONTROL_ACTION_CAPTURE;
						break;
					case TURRET:
						currentAction = GOAL_ACTION_ATTACK;
						break;
				}
		else {
		}
	}

	if (!alreadyAssessedTurrets && (turretRange > 0.0)) {
		if (currentAction == GOAL_ACTION_ATTACK)
			if (getLastTarget())
				if (getLastTarget()->getThreatRating() > turretThreat)
					turretThreat = getLastTarget()->getThreatRating();
		GameObjectPtr turretTarget = calcTurretThreats(turretRange, turretThreat);
		if (turretTarget) {
			if (turretTarget->getObjectClass() == TURRET)
				currentAction = GOAL_ACTION_ATTACK;
			else if (turretTarget->getCaptureBlocker(getVehicle()))
				goalAction = GOAL_ACTION_ATTACK;
			else
				currentAction = canCapture ? GOAL_ACTION_CAPTURE : GOAL_ACTION_ATTACK;
			currentWID = turretTarget->getWatchID();
			currentLocation.Zero();
			currentTarget = turretTarget;
		}
	}

	TacticalOrder newestTacOrder;
	newestTacOrder.init();
	if (newTacOrderReceived[ORDERSTATE_GENERAL])
		newestTacOrder = tacOrder[ORDERSTATE_GENERAL];
	else if (newTacOrderReceived[ORDERSTATE_PLAYER])
		newestTacOrder = tacOrder[ORDERSTATE_PLAYER];
	else if (curTacOrder.code != TACTICAL_ORDER_NONE)
		newestTacOrder = curTacOrder;

	if (!canCapture)
		if (currentAction == GOAL_ACTION_CAPTURE)
			STOP(("Mechwarrior.calcTacOrder: vehicle cannot capture")); //currentAction = GOAL_ACTION_ATTACK;

	newTacOrder.init(ORDER_ORIGIN_COMMANDER, TACTICAL_ORDER_NONE);
	if (currentAction == GOAL_ACTION_MOVE) {
		GlobalPathStep globalPath[50];
		long numSteps = 0;
		if (currentTarget)
			numSteps = getVehicle()->calcGlobalPath(globalPath, currentTarget, nullptr, true);
		else
			numSteps = getVehicle()->calcGlobalPath(globalPath, nullptr, &currentLocation, true);
		if (numSteps > 0) {
			lastGoalPathSize = numSteps;
			for (long i = 0; i < numSteps; i++)
				lastGoalPath[i] = globalPath[i].thruArea;
		}
		for (long i = 0; i < numSteps; i++) {
			GlobalMapAreaPtr area = &GlobalMoveMap[getVehicle()->moveLevel]->areas[globalPath[i].thruArea];
			if (area->type == AREA_TYPE_GATE) {
				GatePtr gate = (GatePtr)ObjectManager->getByWatchID(area->ownerWID);
				if (gate && !gate->isDestroyed() && (gate->getTeam() != getTeam())) {
					GameObjectPtr controlBuilding = ObjectManager->getByWatchID(gate->parent);
					if (canCapture && controlBuilding && (searchDepth > 0)) {
						Stuff::Vector3D tempLocation;
						tempLocation.Zero();
						long result = calcTacOrder(GOAL_ACTION_CAPTURE, controlBuilding->getWatchID(), tempLocation, -1.0, aggressiveness, searchDepth - 1, -1.0, 0, newTacOrder);
						return(result);
						}
					else {
						newTacOrder.init(orderOrigin, TACTICAL_ORDER_ATTACK_OBJECT);
						newTacOrder.subOrder = true;
						newTacOrder.targetWID = gate->getWatchID();
						newTacOrder.attackParams.type = ATTACK_TO_DESTROY;
						newTacOrder.attackParams.method = ATTACKMETHOD_RANGED;
						newTacOrder.attackParams.range = FIRERANGE_OPTIMAL;
						newTacOrder.attackParams.tactic = newestTacOrder.attackParams.tactic;
						newTacOrder.attackParams.pursue = true;
						newTacOrder.moveParams.wayPath.mode[0] = newestTacOrder.moveParams.wayPath.mode[0];
						newTacOrder.moveParams.jump = newestTacOrder.moveParams.jump;
						newTacOrder.moveParams.keepMoving = newestTacOrder.moveParams.keepMoving;
						return(0);
					}
				}
			}
			else if (area->type == AREA_TYPE_WALL) {
				BuildingPtr wall = (BuildingPtr)ObjectManager->getByWatchID(area->ownerWID);
				if (wall && (!wall->isDestroyed())) {
					newTacOrder.init(orderOrigin, TACTICAL_ORDER_ATTACK_OBJECT);
					newTacOrder.subOrder = true;
					newTacOrder.targetWID = wall->getWatchID();
					newTacOrder.attackParams.type = ATTACK_TO_DESTROY;
					newTacOrder.attackParams.method = ATTACKMETHOD_RANGED;
					newTacOrder.attackParams.range = FIRERANGE_OPTIMAL;
					newTacOrder.attackParams.tactic = newestTacOrder.attackParams.tactic;
					newTacOrder.attackParams.pursue = true;
					newTacOrder.moveParams.wayPath.mode[0] = newestTacOrder.moveParams.wayPath.mode[0];
					newTacOrder.moveParams.jump = newestTacOrder.moveParams.jump;
					newTacOrder.moveParams.keepMoving = newestTacOrder.moveParams.keepMoving;
					return(0);
				}
			}
		}

		if (currentTarget) {
			long fromArea = -1;	// change this?
			Stuff::Vector3D goal;
			if (currentTarget->isMover())
				goal = currentTarget->getPosition();
			else {
				// If not a mover, then pick an open cell adjacent
				// to it. If it happens to be adjacent to blocked
				// cells, this could be off...
				if (currentTarget->isBuilding()) {
					BuildingPtr building = (BuildingPtr)currentTarget;
					long goalRow = 0, goalCol = 0;
					bool foundGoal = building->calcAdjacentAreaCell(getVehicle()->moveLevel, fromArea, goalRow, goalCol);
					if (foundGoal)
						land->cellToWorld(goalRow, goalCol, goal);
					else {
						Stuff::Vector3D objectPos = currentTarget->getPosition();
						goal = getVehicle()->calcOffsetMoveGoal(objectPos);
					}
					}
				else {
					Stuff::Vector3D objectPos = currentTarget->getPosition();
					goal = getVehicle()->calcOffsetMoveGoal(objectPos);
				}
			}
			newTacOrder.init(orderOrigin, TACTICAL_ORDER_MOVETO_OBJECT);
			newTacOrder.subOrder = true;
			newTacOrder.targetWID = currentTarget->getWatchID();
			newTacOrder.selectionIndex = -1;
			newTacOrder.moveParams.fromArea = fromArea;
			newTacOrder.moveParams.wayPath.points[0] = goal.x;
			newTacOrder.moveParams.wayPath.points[1] = goal.y;
			newTacOrder.moveParams.wayPath.points[2] = goal.z;
			newTacOrder.moveParams.wayPath.mode[0] = newestTacOrder.moveParams.wayPath.mode[0];
			newTacOrder.moveParams.faceObject = true;
			newTacOrder.moveParams.wait = false;
			newTacOrder.moveParams.mode = MOVE_MODE_NORMAL;
			newTacOrder.moveParams.jump = newestTacOrder.moveParams.jump;
			newTacOrder.moveParams.keepMoving = newestTacOrder.moveParams.keepMoving;
			}
		else {
			newTacOrder.init(orderOrigin, TACTICAL_ORDER_MOVETO_POINT);
			newTacOrder.subOrder = true;
			newTacOrder.setWayPoint(0, currentLocation);
			newTacOrder.moveParams.wayPath.mode[0] =  newestTacOrder.moveParams.wayPath.mode[0];
			newTacOrder.moveParams.wait = false;
			newTacOrder.moveParams.mode = MOVE_MODE_NORMAL;
			newTacOrder.moveParams.jump = newestTacOrder.moveParams.jump;
			newTacOrder.moveParams.keepMoving = newestTacOrder.moveParams.keepMoving;
		}
		}
	else if (currentAction == GOAL_ACTION_ATTACK) {
		GlobalPathStep globalPath[50];
		long numSteps = getVehicle()->calcGlobalPath(globalPath, currentTarget, nullptr, true);
		if (numSteps > 0) {
			lastGoalPathSize = numSteps;
			for (long i = 0; i < numSteps; i++)
				lastGoalPath[i] = globalPath[i].thruArea;
		}
		for (long i = 0; i < numSteps; i++) {
			GlobalMapAreaPtr area = &GlobalMoveMap[getVehicle()->moveLevel]->areas[globalPath[i].thruArea];
			if (area->type == AREA_TYPE_GATE) {
				GatePtr gate = (GatePtr)ObjectManager->getByWatchID(area->ownerWID);
				if (gate && !gate->isDestroyed() && (gate->getTeam() != getTeam())) {
					GameObjectPtr controlBuilding = ObjectManager->getByWatchID(gate->parent);
					if (canCapture && controlBuilding && (searchDepth > 0)) {
						Stuff::Vector3D tempLocation;
						tempLocation.Zero();
						long result = calcTacOrder(GOAL_ACTION_CAPTURE, controlBuilding->getWatchID(), tempLocation, -1.0, aggressiveness, searchDepth - 1, -1.0, 0, newTacOrder);
						return(result);
						}
					else {
						newTacOrder.init(orderOrigin, TACTICAL_ORDER_ATTACK_OBJECT);
						newTacOrder.subOrder = true;
						newTacOrder.targetWID = gate->getWatchID();
						newTacOrder.attackParams.type = ATTACK_TO_DESTROY;
						newTacOrder.attackParams.method = ATTACKMETHOD_RANGED;
						newTacOrder.attackParams.range = FIRERANGE_OPTIMAL;
						newTacOrder.attackParams.tactic = newestTacOrder.attackParams.tactic;
						newTacOrder.attackParams.pursue = true;
						newTacOrder.moveParams.wayPath.mode[0] = newestTacOrder.moveParams.wayPath.mode[0];
						newTacOrder.moveParams.jump = newestTacOrder.moveParams.jump;
						newTacOrder.moveParams.keepMoving = newestTacOrder.moveParams.keepMoving;
						return(0);
					}
				}
			}
			else if (area->type == AREA_TYPE_WALL) {
				BuildingPtr wall = (BuildingPtr)ObjectManager->getByWatchID(area->ownerWID);
				if (wall && (!wall->isDestroyed())) {
					newTacOrder.init(orderOrigin, TACTICAL_ORDER_ATTACK_OBJECT);
					newTacOrder.subOrder = true;
					newTacOrder.targetWID = wall->getWatchID();
					newTacOrder.attackParams.type = ATTACK_TO_DESTROY;
					newTacOrder.attackParams.method = ATTACKMETHOD_RANGED;
					newTacOrder.attackParams.range = FIRERANGE_OPTIMAL;
					newTacOrder.attackParams.tactic = newestTacOrder.attackParams.tactic;
					newTacOrder.attackParams.pursue = true;
					newTacOrder.moveParams.wayPath.mode[0] = newestTacOrder.moveParams.wayPath.mode[0];
					newTacOrder.moveParams.jump = newestTacOrder.moveParams.jump;
					newTacOrder.moveParams.keepMoving = newestTacOrder.moveParams.keepMoving;
					return(0);
				}
			}
		}

		if (currentTarget->isDisabled())
			return(1);
		newTacOrder.init(orderOrigin, TACTICAL_ORDER_ATTACK_OBJECT);
		newTacOrder.subOrder = true;
		newTacOrder.targetWID = currentTarget->getWatchID();
		newTacOrder.attackParams.type = ATTACK_TO_DESTROY;
		newTacOrder.attackParams.method = ATTACKMETHOD_RANGED;
		newTacOrder.attackParams.range = FIRERANGE_OPTIMAL;
		newTacOrder.attackParams.tactic = newestTacOrder.attackParams.tactic;
		newTacOrder.attackParams.pursue = true;
		newTacOrder.moveParams.wayPath.mode[0] = newestTacOrder.moveParams.wayPath.mode[0];
		newTacOrder.moveParams.jump = newestTacOrder.moveParams.jump;
		newTacOrder.moveParams.keepMoving = newestTacOrder.moveParams.keepMoving;
		}
	else if (currentAction == GOAL_ACTION_CAPTURE) {
		bool mainGoalControlled = (currentTarget->getTeam() == getTeam());
		if (!mainGoalControlled) {
			GameObjectPtr blockerList[MAX_MOVERS];
			long numBlockers = currentTarget->getCaptureBlocker(getVehicle(), blockerList);
			if (numBlockers > 0) {
				// Why is it blocked, and can we do something about it?
				/*
				if ((origin == ORDER_ORIGIN_SELF) && (numBlockers > 3)) {
					TacticalOrder alarmTacOrder;
					alarmTacOrder.init(orderOrigin,  TACTICAL_ORDER_ATTACK_OBJECT);
					alarmTacOrder.playerSubOrder = true;
					alarmTacOrder.targetWID = target->getWatchID();
					alarmTacOrder.attackParams.type = ATTACK_TO_DESTROY;
					alarmTacOrder.attackParams.method = ATTACKMETHOD_RANGED;
					alarmTacOrder.attackParams.aimLocation = -1;
					alarmTacOrder.attackParams.range = FIRERANGE_OPTIMAL;
					alarmTacOrder.attackParams.pursue = true;
					alarmTacOrder.attackParams.obliterate = false;
					alarmTacOrder.moveParams.wayPath.mode[0] = newestTacOrder.moveParams.wayPath.mode[0];
					warrior->setAlarmTacOrder(alarmTacOrder, 255);
					}
				else {
				*/
				GameObjectPtr curTarget = getLastTarget();
				long blockerToAttack = -1;
				for (long i = 0; i < numBlockers; i++)
					if (blockerList[i] == curTarget) {
						blockerToAttack = i;
						break;
					}
				if (blockerToAttack == -1)
					blockerToAttack = 0;

				currentAction = GOAL_ACTION_ATTACK;
				currentWID = blockerList[blockerToAttack]->getWatchID();
				currentLocation.Zero();
				currentTarget = blockerList[blockerToAttack];
			}
			GlobalPathStep globalPath[50];
			long numSteps = getVehicle()->calcGlobalPath(globalPath, currentTarget, nullptr, true);
			if (numSteps > 0) {
				lastGoalPathSize = numSteps;
				for (long i = 0; i < numSteps; i++)
					lastGoalPath[i] = globalPath[i].thruArea;
			}
			for (long i = 0; i < numSteps; i++) {
				GlobalMapAreaPtr area = &GlobalMoveMap[getVehicle()->moveLevel]->areas[globalPath[i].thruArea];
				if (area->type == AREA_TYPE_GATE) {
					GatePtr gate = (GatePtr)ObjectManager->getByWatchID(area->ownerWID);
					if (gate && !gate->isDestroyed() && (gate->getTeam() != getTeam())) {
						GameObjectPtr controlBuilding = ObjectManager->getByWatchID(gate->parent);
						if (controlBuilding && (searchDepth > 0)) {
							Stuff::Vector3D tempLocation;
							tempLocation.Zero();
							calcTacOrder(GOAL_ACTION_CAPTURE, controlBuilding->getWatchID(), tempLocation, -1.0, aggressiveness, searchDepth - 1, -1.0, 0, newTacOrder);
							return(0);
							}
						else {
							newTacOrder.init(orderOrigin, TACTICAL_ORDER_ATTACK_OBJECT);
							newTacOrder.subOrder = true;
							newTacOrder.targetWID = gate->getWatchID();
							newTacOrder.attackParams.type = ATTACK_TO_DESTROY;
							newTacOrder.attackParams.method = ATTACKMETHOD_RANGED;
							newTacOrder.attackParams.range = FIRERANGE_OPTIMAL;
							newTacOrder.attackParams.tactic = newestTacOrder.attackParams.tactic;
							newTacOrder.attackParams.pursue = true;
							newTacOrder.moveParams.wayPath.mode[0] = newestTacOrder.moveParams.wayPath.mode[0];
							newTacOrder.moveParams.jump = newestTacOrder.moveParams.jump;
							newTacOrder.moveParams.keepMoving = newestTacOrder.moveParams.keepMoving;
							return(0);
						}
					}
				}
				else if (area->type == AREA_TYPE_WALL) {
					BuildingPtr wall = (BuildingPtr)ObjectManager->getByWatchID(area->ownerWID);
					if (wall && (!wall->isDestroyed())) {
						newTacOrder.init(orderOrigin, TACTICAL_ORDER_ATTACK_OBJECT);
						newTacOrder.subOrder = true;
						newTacOrder.targetWID = wall->getWatchID();
						newTacOrder.attackParams.type = ATTACK_TO_DESTROY;
						newTacOrder.attackParams.method = ATTACKMETHOD_RANGED;
						newTacOrder.attackParams.range = FIRERANGE_OPTIMAL;
						newTacOrder.attackParams.tactic = newestTacOrder.attackParams.tactic;
						newTacOrder.attackParams.pursue = true;
						newTacOrder.moveParams.wayPath.mode[0] = newestTacOrder.moveParams.wayPath.mode[0];
						newTacOrder.moveParams.jump = newestTacOrder.moveParams.jump;
						newTacOrder.moveParams.keepMoving = newestTacOrder.moveParams.keepMoving;
						return(0);
					}
				}
			}
			newTacOrder.init(orderOrigin, TACTICAL_ORDER_CAPTURE );
			newTacOrder.subOrder = true;
			newTacOrder.targetWID = currentTarget->getWatchID();
			newTacOrder.attackParams.type = ATTACK_NONE;
			newTacOrder.attackParams.method = ATTACKMETHOD_RAMMING;
			newTacOrder.attackParams.pursue = true;
			newTacOrder.moveParams.fromArea = globalPath[numSteps - 2].thruArea;
			newTacOrder.moveParams.wayPath.mode[0] = newestTacOrder.moveParams.wayPath.mode[0];
			newTacOrder.moveParams.jump = newestTacOrder.moveParams.jump;
			newTacOrder.moveParams.keepMoving = newestTacOrder.moveParams.keepMoving;
			}
		else if (controlRadius > 0.0) {
			bool noTargetSelected = true;
			GameObjectPtr nextTarget = getLastTarget();
			if (nextTarget)
				if (nextTarget->getObjectClass() == TURRET)
					if (getVehicle()->isEnemy(nextTarget->getTeam()))
						if (mainTarget/*getVehicle()*/->distanceFrom(nextTarget->getPosition()) <= ((Turret*)nextTarget)->maxRange)
							noTargetSelected = false;
			if (noTargetSelected) {
				long numTurrets = ObjectManager->getNumTurrets();
				for (long i = 0; i < numTurrets; i++) {
					Turret* turret = ObjectManager->getTurret(i);
					if (!turret->isDisabled())
						if (getVehicle()->isEnemy(turret->getTeam()))
							if (mainTarget/*getVehicle()*/->distanceFrom(turret->getPosition()) <= turret->maxRange) {
								nextTarget = turret;
								noTargetSelected = false;
								break;
							}
				}
			}
			if (noTargetSelected) {
				if (nextTarget)
					if (nextTarget->isMover())
						if (mainTarget/*getVehicle()*/->distanceFrom(nextTarget->getPosition()) <= controlRadius)
							noTargetSelected = false;
			}
			if (noTargetSelected) {
				long numMovers = ObjectManager->getNumMovers();
				MoverPtr biggestMoverThreat = nullptr;
				long biggestThreatRating = 0;
				for (long i = 0; i < numMovers; i++) {
					MoverPtr mover = ObjectManager->getMover(i);
					if (getVehicle()->isEnemy(mover->getTeam()))
						if (!mover->isDisabled())
							if (mainTarget/*getVehicle()*/->distanceFrom(mover->getPosition()) <= controlRadius)
								if (mover->getThreatRating() > biggestThreatRating) {
									biggestThreatRating = mover->getThreatRating();
									biggestMoverThreat = mover;
								}
				}
				nextTarget = biggestMoverThreat;
			}
			if (nextTarget) {
				if (nextTarget->getObjectClass() == TURRET) {
					GameObjectPtr turretControl = ObjectManager->getByWatchID(((TurretPtr)nextTarget)->parent);
					if (turretControl && (mainTarget/*getVehicle()*/->distanceFrom(turretControl->getPosition()) < controlRadius))
						if (!turretControl->isDisabled()) {
							Stuff::Vector3D tempLocation;
							tempLocation.Zero();
							calcTacOrder(GOAL_ACTION_CAPTURE, turretControl->getWatchID(), tempLocation, -1.0, aggressiveness, searchDepth - 1, -1.0, 0, newTacOrder);
							return(0);
						}
				}
				Stuff::Vector3D tempLocation;
				tempLocation.Zero();
				calcTacOrder(GOAL_ACTION_ATTACK, nextTarget->getWatchID(), tempLocation, -1.0, aggressiveness, searchDepth, -1.0, 0, newTacOrder);
				return(0);
			}
		}
	}
	else if (currentAction == GOAL_ACTION_GUARD) {
	}

	return(0);
}

void MechWarrior::initGoalManager (long poolSize) {

	goalManager = new GoalManager;
	gosASSERT(goalManager != nullptr);
	goalManager->setup(poolSize);
	goalManager->build();

}

void MechWarrior::logPilots (GameLogPtr log) {

	for (long i = 0; i < ObjectManager->getNumMovers(); i++) {
		MoverPtr mover = ObjectManager->getMover(i);
		if (mover) {
			char s[256];
			mc2::fmt::to_buf(s, "pilot = ({:d}){}", mc2::fmt::as_i32(i), mc2::fmt::safe(mover->getPilot()->getName()));
			log->write(s);
			mc2::fmt::to_buf(s, "     vehicle = ({:d}){}", mc2::fmt::as_i32(mover->getPartId()), mc2::fmt::safe(mover->getName()));
			log->write(s);
		}
	}
}

void MechWarrior::copyToData (MechWarriorData &data)
{
	data.used = used;

	strcpy(data.name,name);
	strcpy(data.callsign,callsign);
	strcpy(data.videoStr,videoStr);
	strcpy(data.audioStr,audioStr);
	strcpy(data.brainStr,brainStr);
	data.index = index;
	data.paintScheme = paintScheme;
	data.photoIndex = photoIndex;
	
	data.rank = rank;
    MemCpy(data.skills,skills);
	data.professionalism = professionalism;
	data.professionalismModifier = professionalismModifier;
	data.decorum = decorum;
	data.decorumModifier = decorumModifier;
	data.aggressiveness = aggressiveness;
	data.courage = courage;
	data.baseCourage = baseCourage;
	
	data.wounds = wounds;
	data.health = health;
	data.status = status;
	data.escapesThruEjection = escapesThruEjection;
	data.radioLog = radioLog;
	data.notMineYet = notMineYet;
	
	data.teamId = teamId;
	data.vehicleWID = vehicleWID;
	
	MemCpy2(data.numSkillUses,numSkillUses);
	MemCpy2(data.numSkillSuccesses,numSkillSuccesses);
	MemCpy2(data.numMechKills,numMechKills);
	MemCpy2(data.numPhysicalAttacks,numPhysicalAttacks);
	MemCpy(data.skillRank,skillRank);
	MemCpy(data.skillPoints,skillPoints);
	MemCpy(data.originalSkills,originalSkills);
	MemCpy(data.startingSkills,startingSkills);
	MemCpy(data.specialtySkills,specialtySkills);
	MemCpy(data.killed,killed);
	data.numKilled = numKilled;
	
	data.descID = descID;
	data.nameIndex = nameIndex;
	
	data.timeOfLastOrders = timeOfLastOrders;
	MemCpy(data.attackers,attackers);
	data.numAttackers = numAttackers;
	data.attackRadius = attackRadius;
	
	MemCpy(data.memory,memory);
    MemCpy2(data.debugStrings,debugStrings);
	
	data.brainUpdate = brainUpdate;
	data.combatUpdate = combatUpdate;
	data.movementUpdate = movementUpdate;
	MemCpy(data.weaponsStatus,weaponsStatus);
	data.weaponsStatusResult = weaponsStatusResult;
	
	data.useGoalPlan = useGoalPlan;
	data.mainGoalAction = mainGoalAction;
	data.mainGoalObjectWID = mainGoalObjectWID;
	data.mainGoalLocation = mainGoalLocation;
	data.mainGoalControlRadius = mainGoalControlRadius;
	data.lastGoalPathSize = lastGoalPathSize;
	MemCpy(data.lastGoalPath,lastGoalPath);
	
	MemCpy(data.newTacOrderReceived,newTacOrderReceived);
	MemCpy(data.tacOrder,tacOrder);
	data.lastTacOrder = lastTacOrder;
	data.curTacOrder = curTacOrder;
	MemCpy(data.alarm,alarm);
	MemCpy(data.alarmHistory,alarmHistory);	// used by brain update in ABL
	data.alarmPriority = alarmPriority;
	data.curPlayerOrderFromQueue = curPlayerOrderFromQueue;
	data.tacOrderQueueLocked = tacOrderQueueLocked;
	data.tacOrderQueueExecuting = tacOrderQueueExecuting;
	data.tacOrderQueueLooping = tacOrderQueueLooping;
	data.numTacOrdersQueued = numTacOrdersQueued;	

	//Pilots which are not used may not have this assigned.
	if (tacOrderQueue) {
		MemCpy(data.tacOrderQueue,tacOrderQueue);
    }

	data.tacOrderQueueIndex = tacOrderQueueIndex;
	data.nextTacOrderId = nextTacOrderId;
	data.lastTacOrderId = lastTacOrderId;
	
	data.coreScanTargetWID = coreScanTargetWID;
	data.coreAttackTargetWID = coreAttackTargetWID;
	data.coreMoveTargetWID = coreMoveTargetWID;
	data.targetPriorityList = targetPriorityList;
	data.brainState = brainState;
	data.willHelp = willHelp;
	
	data.moveOrders.copy(moveOrders);
	data.attackOrders = attackOrders;
	data.situationOrders = situationOrders;
	data.lastTargetWID = lastTargetWID;
	data.lastTargetTime = lastTargetTime;
	data.lastTargetObliterate = lastTargetObliterate;
	data.lastTargetFriendly = lastTargetFriendly;
	data.lastTargetConserveAmmo = lastTargetConserveAmmo;
	data.keepMoving = keepMoving;
	
	data.orderState = orderState;
	
	data.debugFlags = debugFlags;
	
	data.oldPilot = oldPilot;

	if (brain)
		data.warriorBrainHandle = brain->getRealId();
	else
		data.warriorBrainHandle = -1;
}

void MechWarrior::copyFromData (MechWarriorData &data)
{
	used = data.used;

	strcpy(name,data.name);
	strcpy(callsign,data.callsign);
	strcpy(videoStr,data.videoStr);
	strcpy(audioStr,data.audioStr);
	strcpy(brainStr,data.brainStr);

	index = data.index;
	paintScheme = data.paintScheme;
	photoIndex = data.photoIndex;
	
	rank = data.rank;
	MemCpy(skills,data.skills);

	professionalism = data.professionalism;
	professionalismModifier = data.professionalismModifier;
	decorum = data.decorum;
	decorumModifier = data.decorumModifier;
	aggressiveness = data.aggressiveness;
	courage = data.courage;
	baseCourage = data.baseCourage;
	
	wounds = data.wounds;
	health = data.health;
	status = data.status;
	escapesThruEjection = data.escapesThruEjection;
	radioLog = data.radioLog;
	notMineYet = data.notMineYet;
	
	teamId = data.teamId;
	vehicleWID = data.vehicleWID;
	
	MemCpy2(numSkillUses,data.numSkillUses);
	MemCpy2(numSkillSuccesses,data.numSkillSuccesses);
	MemCpy2(numMechKills,data.numMechKills);
	MemCpy2(numPhysicalAttacks,data.numPhysicalAttacks);
	MemCpy(skillRank,data.skillRank);
	MemCpy(skillPoints,data.skillPoints);
	MemCpy(originalSkills,data.originalSkills);
	MemCpy(startingSkills,data.startingSkills);
	MemCpy(specialtySkills,data.specialtySkills);
	MemCpy(killed,data.killed);
	numKilled = data.numKilled;
	
	descID = data.descID;
	nameIndex = data.nameIndex;
	
	timeOfLastOrders = data.timeOfLastOrders;
	MemCpy(attackers,data.attackers);
	numAttackers = data.numAttackers;
	attackRadius = data.attackRadius;
	
	MemCpy(memory,data.memory);
	MemCpy2(debugStrings,data.debugStrings);
	
	brainUpdate = data.brainUpdate;
	combatUpdate = data.combatUpdate;
	movementUpdate = data.movementUpdate;
	MemCpy(weaponsStatus,data.weaponsStatus);
	weaponsStatusResult = data.weaponsStatusResult;
	
	useGoalPlan = data.useGoalPlan;
	mainGoalAction = data.mainGoalAction;
	mainGoalObjectWID = data.mainGoalObjectWID;
	mainGoalLocation = data.mainGoalLocation;
	mainGoalControlRadius = data.mainGoalControlRadius;
	lastGoalPathSize = data.lastGoalPathSize;
	MemCpy(lastGoalPath,data.lastGoalPath);
	
	MemCpy(newTacOrderReceived, data.newTacOrderReceived);
	MemCpy(tacOrder, data.tacOrder);
	lastTacOrder = data.lastTacOrder;
	curTacOrder = data.curTacOrder;
	MemCpy(alarm,data.alarm);
	MemCpy(alarmHistory,data.alarmHistory);	// used by brain update in ABL
	alarmPriority = data.alarmPriority;
	curPlayerOrderFromQueue = data.curPlayerOrderFromQueue;
	tacOrderQueueLocked = data.tacOrderQueueLocked;
	tacOrderQueueExecuting = data.tacOrderQueueExecuting;
	tacOrderQueueLooping = data.tacOrderQueueLooping;
	numTacOrdersQueued = data.numTacOrdersQueued;
	MemCpy(tacOrderQueue,data.tacOrderQueue);
	for (long i=0;i<MAX_QUEUED_TACORDERS_PER_WARRIOR;i++)
	{
		//Set the Marker pointers to NULL and then recreate them from the movemode data.
		tacOrderQueue[i].marker = nullptr;

		if ( tacOrderQueue[i].moveMode == TRAVEL_MODE_SLOW )
			tacOrderQueue[i].marker = getWayPointMarker( tacOrderQueue[i].point, "WalkWayPoint" );

		else if ( tacOrderQueue[i].moveMode == TRAVEL_MODE_FAST )
			tacOrderQueue[i].marker = getWayPointMarker( tacOrderQueue[i].point, "RunWayPoint" );

		else if ( tacOrderQueue[i].moveMode == TRAVEL_MODE_JUMP )
			tacOrderQueue[i].marker = getWayPointMarker( tacOrderQueue[i].point, "JumpWayPoint" );
	}
	tacOrderQueueIndex = data.tacOrderQueueIndex;
	nextTacOrderId = data.nextTacOrderId;
	lastTacOrderId = data.lastTacOrderId;
	
	coreScanTargetWID = data.coreScanTargetWID;
	coreAttackTargetWID = data.coreAttackTargetWID;
	coreMoveTargetWID = data.coreMoveTargetWID;
	targetPriorityList = data.targetPriorityList;
	brainState = data.brainState;
	willHelp = data.willHelp;
	
	data.moveOrders.copyTo(moveOrders);

	attackOrders = data.attackOrders;
	situationOrders = data.situationOrders;
	lastTargetWID = data.lastTargetWID;
	lastTargetTime = data.lastTargetTime;
	lastTargetObliterate = data.lastTargetObliterate;
	lastTargetFriendly = data.lastTargetFriendly;
	lastTargetConserveAmmo = data.lastTargetConserveAmmo;
	keepMoving = data.keepMoving;
								
	orderState = data.orderState;
								
	debugFlags = data.debugFlags;
	
	oldPilot = data.oldPilot;

	warriorBrainHandle = data.warriorBrainHandle;
}

void MechWarrior::init (MechWarriorData data)
{
	init(false);

	copyFromData(data);

	if (warriorBrainHandle > -1) 
	{
		brain = ABLi_getModule(warriorBrainHandle);

		for (long i = 0; i < NUM_PILOT_ALARMS; i++)
			brainAlarmCallback[i] = brain->findFunction(pilotAlarmFunctionName[i], true);
	}
	else
	{
		brain = nullptr;
	}

	movePathRequest = nullptr;

	if (audioStr[0] != 0)
	{
		radio = new Radio;
		long result = radio->init(audioStr,102400,videoStr);		//Size of radio heap should be data driven?
		if (result != NO_ERR)
		{
			delete radio;	//Radio couldn't start, so no radio.
			radio = nullptr;
		}
	}

	isPlayingMsg = false;	//Never playing when we reload an in-mission save!

	if (radio)
		radio->setOwner(this);
}

void MechWarrior::save (PacketFilePtr file, long packetNum) 
{
	MechWarriorData warriorData{};
	copyToData(warriorData);
	file->writePacket(packetNum,(MemoryPtr)&warriorData,sizeof(warriorData),STORAGE_TYPE_ZLIB);
}

long MechWarrior::Save (PacketFilePtr file, long packetNum)
{
	StaticMechWarriorData staticData{};
	staticData.numWarriors 					= numWarriors;
	staticData.numWarriorsInCombat			= numWarriorsInCombat;
	MemCpy(staticData.brainsEnabled,brainsEnabled);
	staticData.minSkill						= minSkill;
	staticData.maxSkill						= maxSkill;
	staticData.increaseCap					= increaseCap;
	staticData.maxVisualRadius				= maxVisualRadius;
	staticData.curEventID					= curEventID;
	staticData.curEventTrigger				= curEventTrigger;

	file->writePacket(packetNum,(MemoryPtr)&staticData,sizeof(StaticMechWarriorData),STORAGE_TYPE_RAW);
	packetNum++;

	for (long i = 0; i < numWarriors; i++)
	{
		MechWarrior::warriorList[i]->save(file,packetNum);
		packetNum++;
	}

	return packetNum;
}

void MechWarrior::load (PacketFilePtr file, long packetNum)
{
	MechWarriorData warriorData{};
	file->readPacket(packetNum,(MemoryPtr)&warriorData);
	init(warriorData);
}

long MechWarrior::Load (PacketFilePtr file, long packetNum)
{
	StaticMechWarriorData staticData{};
	file->readPacket(packetNum,(MemoryPtr)&staticData);
	packetNum++;

	numWarriors 			= staticData.numWarriors;
	numWarriorsInCombat		= staticData.numWarriorsInCombat;

	MemCpy(brainsEnabled,staticData.brainsEnabled);

	minSkill				= staticData.minSkill;
	maxSkill				= staticData.maxSkill;
	increaseCap				= staticData.increaseCap;
	maxVisualRadius			= staticData.maxVisualRadius;
	curEventID				= staticData.curEventID;
	curEventTrigger			= staticData.curEventTrigger;

	TacOrderQueuePos = 0;

	for (long i = 0; i < numWarriors; i++)
	{
		MechWarrior::warriorList[i]->load(file,packetNum);
		packetNum++;

		//Kind of a hack but Glenn increments numWarriors with each init.
		// Take it back to the real number here!!
		numWarriors--;
	}


	if (!sortList) 
	{
		sortList = new SortList;
		if (!sortList)
			Fatal(0, " Unable to create Warrior::sortList ");
		sortList->init(100);
	}

	return packetNum;
}

bool MechWarrior::warriorInUse (char *warriorName)
{
	for (long i=0; i < numWarriors;i++)
	{
		if (S_stricmp(MechWarrior::warriorList[i]->name,warriorName) == 0)
			return true;
	}

	return false;
}
