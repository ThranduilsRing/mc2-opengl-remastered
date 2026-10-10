// ABL native bindings: unit status, repair, messaging, path queries, pilot state and debug bindings.
// Declared in ablmc2_internal.h; registered, in order, by initABL in ablmc2.cpp.
#include "ablmc2_internal.h"


void execIsGateOpen (void) {

	//	IsGateOpen function:
	//		Returns whether or not a gate is open
	//		PARAMS:	integer				object ID
	//		RETURN: BOOL				Open


	ABLi_pokeBoolean(false);
#ifdef USE_GATES
	GameObjectPtr obj = getObject(objectId);
	if (obj && (obj->getObjectClass() == GATE))	{
		ABLi_pokeBoolean(((GatePtr)object)->opened);
#endif
}

void execGetRelativePositionToPoint (void) {

	//	Get Relative Position To Point function:
	//		Calcs a position relative to the given point
	//		PARAMS:	real[3]				point
	//				real				angle (-180 <= angle <= 180.0, + is left, - is right)
	//				real				distance (in meters)
	//				integer				flags
	//				real[3]				new point
	//		RETURN: NONE

	float* pos = ABLi_popRealPtr();
	float angle = ABLi_popReal();
	float distance = ABLi_popReal();
	long flags = ABLi_popInteger();
	float* relPos = ABLi_popRealPtr();

	// H2: guard sibling of execGetRelativePositionToObject.
	// pos is READ (curPos.x = pos[0..2]); relPos is WRITE (relPos[0..2] = newPos).
	// Both crash on null. Guard both independently so each gets a distinct log line.
	if (s_ablArgGuard && !pos) {
		abl_arg_guard_log("execGetRelativePositionToPoint", "pos");
		return;
	}
	if (s_ablArgGuard && !relPos) {
		abl_arg_guard_log("execGetRelativePositionToPoint", "relPos");
		return;
	}

	Stuff::Vector3D curPos;
	curPos.x = pos[0];
	curPos.y = pos[1];
	curPos.z = 0;
	Stuff::Vector3D newPos;
	newPos = relativePositionToPoint(curPos, angle, distance, flags);
	relPos[0] = newPos.x;
	relPos[1] = newPos.y;
	relPos[2] = newPos.z;
}

void execGetRelativePositionToObject (void) {

	//	Get Relative Position To Object function:
	//		Calcs a position relative to the given object
	//		PARAMS:	integer				object id
	//				real				angle (-180 <= angle <= 180.0, + is left, - is right)
	//				real				distance (in meters)
	//				integer				flags
	//				real[3]				new point
	//		RETURN: NONE

	long objectId = ABLi_popInteger();
	float angle = ABLi_popReal();
	float distance = ABLi_popReal();
	long flags = ABLi_popInteger();
	float* relPos = ABLi_popRealPtr();

	// H1: guard the proven crash site (ablmc2.cpp ~4397 null-write).
	// relPos is NULL when the calling ABL script passes a wrong-type or missing
	// argument. The write below faults without this check.
	// Gate MC2_ABL_ARG_GUARD=1 enables the guard; gate OFF = byte-identical
	// to previous behaviour (crash on bad script — same as before this patch).
	if (s_ablArgGuard && !relPos) {
		abl_arg_guard_log("execGetRelativePositionToObject", "relPos");
		return;
	}

	GameObjectPtr object = getObject(objectId);
	if (object && object->isMover()) {
		Stuff::Vector3D newPos;
		newPos = ((MoverPtr)object)->relativePosition(angle, distance, flags);
		relPos[0] = newPos.x;
		relPos[1] = newPos.y;
		relPos[2] = newPos.z;
	}
}

void execGetUnitStatus (void) {

	//Returns the current state of the health bar in percent format from 0 to 100
	//		PARAMS:	integer
	//		Returns: real (result)

	long objectNum = ABLi_popInteger();

	GameObjectPtr obj = getObject(objectNum);
	ABLi_pushReal(0.0);
	if (obj)
		ABLi_pokeReal(obj->getStatusRating() * 100.0);
}	

void execRepair (void) {

	//	Repair function:
	//		Repairs armor and internals. Currently only works on mechs
	//		PARAMS:	integer				object id
	//				real				points (of armor or structure) to repair
	//		RETURN: NONE

	long objectId = ABLi_popInteger();
	float points = ABLi_popReal();

	GameObjectPtr repairTarget = getObject(objectId);
	if (repairTarget && (repairTarget->getObjectClass() == BATTLEMECH)) {
		BattleMechPtr mech = (BattleMechPtr) repairTarget;
		for (long i = 0; i < mech->numBodyLocations; i++) {
			float diff = (float)mech->body[i].maxInternalStructure - mech->body[i].curInternalStructure;
			if (mech->body[i].damageState == IS_DAMAGE_DESTROYED)	// can't repair it if it ain't there!
				continue;
			if (diff > 0) {
				if (diff < points) {
					points -= diff;
					mech->body[i].curInternalStructure = mech->body[i].maxInternalStructure;
					}
				else {
					mech->body[i].curInternalStructure += points;
					points = 0;
					break;
				}
			}
		}
		for (int i = 0; i < mech->numArmorLocations; i++) {
			float diff = (float) mech->armor[i].maxArmor - mech->armor[i].curArmor;
			if (i < mech->numBodyLocations)	{
				if (mech->body[i].damageState == IS_DAMAGE_DESTROYED)	// can't repair it if it ain't there!
					continue;
				}
			else {	// for back armor, be sure you're looking at the front...
				if (mech->body[i - mech->numBodyLocations + 1].damageState == IS_DAMAGE_DESTROYED)	// can't repair it if it ain't there!
					continue;
			}
			if (diff > 0) {
				if (diff < points) {
					points -= diff;
					mech->armor[i].curArmor= mech->armor[i].maxArmor;
					}
				else {
					mech->armor[i].curArmor += points;
					points = 0;
					break;
				}
			}
		}
	}
}

void execGetFixed (void) {

	//	Get Fixed function:
	//		Sends mech or vehicle to appropriate repair bay
	//		returns error code if call is illegal
	//		PARAMS:	integer				mech or vehicle (fixee) to repair id
	//				integer				repair bay id
	//				integer				parameters
	//		RETURN: integer				error code
	//		-1	unknown error
	//	   *	1	bay is out of repair points (probably means the bay is destroyed)
	//	   *	2	wrong kind of bay (either sending a vehicle to a mech bay, or mech to vehicle bay)
	//	   *	3	bay is already repairing this fixee
	//	   *	4	bay is repairing something else
	//	   *	5	fixee is being repaired by something else (either another bay or a refit vehicle)
	//	   *	6	fixee doesn't need repair
	//	   *	7	illegal fixee object (either there's no object with that ID, or the object isn't a mech or a vehicle)
	//	   *	8	illegal bay object (either there's no object with that ID, or the object is not a repair bay)
	//	   *	9	alignment mismatch (bay and fixee are on different sides.)
	//	* is checked by -fs on 5/5/97
	
	long fixeeId = ABLi_popInteger();
	long bayId = ABLi_popInteger();
	long params = ABLi_popInteger();

	BuildingPtr bay = nullptr;
	GameObjectPtr obj = getObject(bayId);
	if (obj && obj->isBuilding() && obj->getFlag(OBJECT_FLAG_CANREFIT))
		bay = (BuildingPtr)obj;

	long result = 8;
	if (bay) {
		result = 1;
		if (bay->getRefitPoints() > 0.0) {
			result = 7;
			obj = getObject(fixeeId);
			if (obj && ((obj->getObjectClass() == BATTLEMECH) || (obj->getObjectClass() == GROUNDVEHICLE))) {
				result = 9;
				if (obj->getTeam() && bay->isFriendly(obj->getTeam())) {
					GameObjectPtr refitBuddy = ObjectManager->getByWatchID(bay->refitBuddyWID);
					if (refitBuddy) {
						if (refitBuddy == obj)
							result = 3;
						else
							result = 4;
						}
					else if (obj->getObjectClass() == BATTLEMECH) {
						BattleMechPtr mech = (BattleMechPtr)obj;
						result = 2;
						if (bay->getFlag(OBJECT_FLAG_MECHBAY)) {
							result = 5;
							refitBuddy = ObjectManager->getByWatchID(mech->refitBuddyWID);
							if (!refitBuddy) {
								result = 6;
								if (mech->needsRefit()) {
									if (mech->getPilot()->orderGetFixed(ORDER_ORIGIN_COMMANDER, bay, params) == NO_ERR)
										result = 0;
									else
										result = -1;
								}
							}
						}
						}
					else {
						GroundVehiclePtr vehicle = (GroundVehiclePtr)obj;
						result = 2;
						if (bay->getFlag(OBJECT_FLAG_MECHBAY)) {
							result = 5;
							refitBuddy = ObjectManager->getByWatchID(vehicle->refitBuddyWID);
							if (!refitBuddy) {
								result = 6;
								if (vehicle->needsRefit()) {
									if (vehicle->getPilot()->orderGetFixed(ORDER_ORIGIN_COMMANDER, bay, params) == NO_ERR)
										result = 0;
									else
										result = -1;
								}
							}
						}
					}
				}
			}
		}
	}
	ABLi_pushInteger(result);
}

void execGetRepairState (void) {

	//Returns the percentage of repairable armor and IS points of the objectId passed in
	//		PARAMS: integer
	//		Returns: integer (result)

	long objectId = ABLi_peekInteger();

	ABLi_pokeInteger(0);
	long max = 0;
	long cur = 0.0;
	GameObjectPtr object = getObject(objectId);
	if (object && object->isMover()) {
		MoverPtr mover = (MoverPtr) object;
		for (long i = 0; i < mover->numBodyLocations; i++) {
			// can't repair it if it ain't there, so don't report it if destroyed...
			if (mover->body[i].damageState == IS_DAMAGE_DESTROYED)
				continue;
			max += mover->body[i].maxInternalStructure;
			cur += mover->body[i].curInternalStructure;
		}
		for (int i = 0; i < mover->numArmorLocations; i++) {
			if (i < mover->numBodyLocations) {
				if (mover->body[i].damageState == IS_DAMAGE_DESTROYED)	// can't repair it if it ain't there!
					continue;
				}
			else {
				// for back armor, be sure you're looking at the front...
				if (mover->body[i - mover->numBodyLocations + 1].damageState == IS_DAMAGE_DESTROYED)	// can't repair it if it ain't there!
					continue;
			}
			max += mover->armor[i].maxArmor;
			cur += mover->armor[i].curArmor;
		}
	}
	cur *= 100;
	ABLi_pokeInteger(cur / max);
}

void execIsTeamTargeting (void) {

	//	Is Team Targeting function:
	//		Returns whether anyone of the specified team is currently
	//		targeting the specified target with the possible exception
	//		of the one team member (which may be set to 0, if desired).
	//		PARAMS:	integer				team id
	//				integer				target id
	//				integer				except id (may be 0)
	//		RETURN: boolean				yes or no

	long teamId = ABLi_popInteger();
	long targetId = ABLi_popInteger();
	long exceptId = ABLi_popInteger();

	bool targeting = false;
	if ((teamId >= OBJ_ID_FIRST_TEAM) && (teamId <= OBJ_ID_LAST_TEAM)) {
		TeamPtr team = Team::teams[teamId - OBJ_ID_FIRST_TEAM];
		if (team)
			targeting = team->isTargeting(targetId, exceptId);
		}
	else if (teamId == 0) {
		GameObjectPtr target = ObjectManager->get(targetId);
		if (target)
			targeting = (target->getNumAttackers() > 0);
	}

	ABLi_pushBoolean(targeting);
}

void execIsTeamCapturing (void) {

	//	Is Team Capturing function:
	//		Returns whether anyone of the specified team is currently
	//		capturing the specified target with the possible exception
	//		of the one team member (which may be set to 0, if desired).
	//		PARAMS:	integer				team id
	//				integer				target id
	//		RETURN: boolean				yes or no

	long teamId = ABLi_popInteger();
	long targetId = ABLi_popInteger();
	long exceptId = ABLi_popInteger();

	bool targeting = false;
	GameObjectPtr target = getObject(targetId);
	unsigned long targetWID = target ? target->getWatchID(false) : 0;
	if (targetWID) {
		if ((teamId >= OBJ_ID_FIRST_TEAM) && (teamId <= OBJ_ID_LAST_TEAM)) {
			TeamPtr team = Team::teams[teamId - OBJ_ID_FIRST_TEAM];
			if (team)
				targeting = ObjectManager->isTeamCapturing(team, targetWID);
			}
		else if (teamId == 0) {
			targeting = ObjectManager->isTeamCapturing(nullptr, targetWID);
		}
	}

	ABLi_pushBoolean(targeting);
}

void execSendMessage (void) {

	CurMultiplayCode = ABLi_popInteger();
	CurMultiplayParam = ABLi_popInteger();

	if (MPlayer && MPlayer->isServer()) {
		// We must be server if we got here...
		MPlayer->addMissionScriptMessageChunk(CurMultiplayCode, CurMultiplayParam);
#ifdef DEBUG_MISSION_SCRIPT
		if (NumMissionScriptMessages == MAX_MISSION_MSGS) {
			DebugMissionScriptMessages();
			Assert(false, NumMissionScriptMessages, " Way too many Mission Script Messages! ");
		}
		MissionScriptMessageLog[NumMissionScriptMessages][0] = execLineNumber;
		MissionScriptMessageLog[NumMissionScriptMessages][1] = CurMultiplayCode;
		MissionScriptMessageLog[NumMissionScriptMessages][2] = CurMultiplayParam;
		NumMissionScriptMessages++;
#endif
	}
}

void execGetMessage (void) {

	int* messageParam = ABLi_peekIntegerPtr();

	*messageParam = CurMultiplayParam;
	ABLi_pokeInteger(CurMultiplayCode);
}

void execGetHomeTeam (void) {

	//	Get HomeTeam function:
	//		Returns id for home team: 500 = IS, 501 = CLAN, 502 = ALLIED
	//		PARAMS:	NONE
	//		RETURN: integer				hometeam id

	ABLi_pushInteger(Team::home->getId() + MIN_TEAM_PART_ID);
}

void execIsServer (void) {

	//	Is Server function:
	//		Returns TRUE if I'm the server in a multi-player game.
	//		PARAMS:	NONE
	//		RETURN: boolean				TRUE = yes I am:)

	if (MPlayer && MPlayer->isServer())
		pushInteger(1);
	else
		pushInteger(0);
}

void execCalcPartID (void) {

	long objectClass = ABLi_popInteger();
	long param1 = ABLi_popInteger();
	long param2 = ABLi_popInteger();
	long param3 = ABLi_popInteger();

	long partId = ObjectManager->calcPartId(objectClass, param1, param2, param3);

	ABLi_pushInteger(partId);
}

void execSetDebugString (void) {

	long objectId = ABLi_popInteger();
	long stringNum = ABLi_popInteger();
	char* debugString = ABLi_popCharPtr();
	GameObjectPtr obj = getObject(objectId);
	if (obj) {
		MechWarriorPtr pilot = obj->getPilot();
		if (pilot)
			pilot->setDebugString(stringNum, debugString);
	}
}

void execBreak (void) {

	if (ABLi_getCurrentState()) {
		DEBUGWINS_print("BREAK", 0);
	}
}

void execPathExists (void) {

	//	PATH EXISTS function:
	//		Returns number and list of objects of the specified object type.
	//		PARAMS:	integer		mover id. if set to 0, assumes a mech-type
	//				integer		startCellRow
	//				integer		startCellCol
	//				integer		goalCellRow
	//				integer		goalCellCol
	//		RETURN: integer		answer: 0 = no, else number of areas in
	//										LR path

	int moverID = ABLi_popInteger();
	int startRow = ABLi_popInteger();
	int startCol = ABLi_popInteger();
	int goalRow = ABLi_popInteger();
	int goalCol = ABLi_popInteger();

	// For now, assumes we mean the ground level (0)...
	int startArea = GlobalMoveMap[0]->calcArea(startRow, startCol);
	int goalArea = GlobalMoveMap[0]->calcArea(goalRow, goalCol);
	int confidence;
	long areaPathCost = GlobalMoveMap[0]->getPathCost(startArea, goalArea, false, confidence, true);

	ABLi_pushInteger(areaPathCost);
}

void execConvertCoords (void) {

	int convertType = ABLi_popInteger();
	float* worldPos = ABLi_popRealPtr();
	int* cellPos = ABLi_popIntegerPtr();

	ABLi_pushInteger(0);

	if (s_ablArgGuard && !worldPos) { abl_arg_guard_log("execConvertCoords", "worldPos"); return; }
	if (s_ablArgGuard && !cellPos) { abl_arg_guard_log("execConvertCoords", "cellPos"); return; }

	if (convertType == 0) {
		Stuff::Vector3D worldP;
		worldP.x = worldPos[0];
		worldP.y = worldPos[1];
		worldP.z = worldPos[2];
		land->worldToCell(worldP, cellPos[0], cellPos[1]);
		}
	else {
		Stuff::Vector3D worldP;
		land->cellToWorld(cellPos[0], cellPos[1], worldP);
		worldPos[0] = worldP.x;
		worldPos[1] = worldP.y;
		worldPos[2] = 0.0;
	}
}

void execSetPilotState (void) {

	// SetPilotState: Sets the current pilot's brain state.
	//		PARAMS:	integer		new state ID
	//		RETURN: integer		previous state ID

	unsigned long newStateHandle = ABLi_popInteger();
	
	long curStateHandle = ABLi_getCurrentState();
	if (curStateHandle > 0) {
		ABLi_transState(newStateHandle);
		ABLi_pushInteger(curStateHandle);
		}
	else {
		ABLi_resetOrders();
		long prevState = CurWarrior->setBrainState(newStateHandle);
		ABLi_pushInteger(prevState);
	}
}

void execGetPilotState (void) {

	// GetPilotState: Gets the current pilot's brain state.
	//		PARAMS:	NONE
	//		RETURN: integer		current state ID

	ABLi_pushInteger(CurWarrior->getBrainState());
}

void execGetNextPilotEvent (void) {

	// GetNextPilotEvent:	Removes & returns the next queued pilot event.
	//						Any params for the event are put into the
	//						integer array passed in.
	//		PARAMS:	@integer[3]		parameter list
	//		RETURN: integer			event ID

	int* paramList = ABLi_popIntegerPtr();
	
	long eventID = CurWarrior->getNextEventHistory(paramList);

	ABLi_pushInteger(eventID);
}

void execSetTargetPriority (void) {

	// SetTargetPriority:	Sets the target priority specified for the
	//						current pilot.
	//		PARAMS:	integer			target priority index (0 thru ?)
	//				integer			target priority type
	//				integer			param 1
	//				integer			param 2
	//				integer			param 3
	//		RETURN: integer			error code (0 = OK)

	long index = ABLi_popInteger();
	long type = ABLi_popInteger();
	long param1 = ABLi_popInteger();
	long param2 = ABLi_popInteger();
	long param3 = ABLi_popInteger();
	
	long err = CurWarrior->setTargetPriority(index, type, param1, param2, param3);
	
	ABLi_pushInteger(err);
}

void execSetDebugWindow (void) {

	// SetDebugWindow:	Sets the game object for the specified debug window.
	//		PARAMS:	integer			debug window id (0 thru 3)
	//				integer			object id
	//		RETURN: integer			object id, if set. Else 0.

	long windowIndex = ABLi_popInteger();
	long objectID = ABLi_popInteger();
	
	GameObjectPtr obj = getObject(objectID);
	if (obj)
		DEBUGWINS_setGameObject(windowIndex, obj);
	else
		objectID = 0;

	ABLi_pushInteger(objectID);
}
