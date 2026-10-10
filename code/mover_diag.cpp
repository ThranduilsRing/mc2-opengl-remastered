// Mover: diagnostic dumps: chunk dump helpers, updateDebugWindow and the combat-log weapon fire/hit printers.
// Declared in mover.h; shared names in mover_internal.h.
#include "mover_internal.h"


void DebugMoveChunk (MoverPtr mover, MoveChunkPtr chunk1, MoveChunkPtr chunk2) {

	ChunkDebugMsg[0] = '\0';
	char outString[512];

	if (mover) {
		sprintf(outString, "Mover = %s (%d)\n",
			mover->getName(),
			mover->getPartId());
		strcat(ChunkDebugMsg, outString);

		sprintf(outString, "Mover World Pos = (%.4f, %.4f, %.4f)\n",
			mover->getPosition().x,
			mover->getPosition().y,
			mover->getPosition().z);
		strcat(ChunkDebugMsg, outString);

		int cellPos[2];
		mover->getCellPosition(cellPos[0], cellPos[1]);
		sprintf(outString, "Mover Obj Cell Pos = [%d, %d]\n", cellPos[0], cellPos[1]);
		strcat(ChunkDebugMsg, outString);
		if (mover->getPilot() == nullptr)
			strcat(ChunkDebugMsg, "NULL pilot!\n");
		if ((mover->getObjectClass() == BATTLEMECH) && ((BattleMechPtr)mover)->inJump) {
			int jumpDest[2];
			land->worldToCell(((BattleMechPtr)mover)->jumpGoal, jumpDest[0], jumpDest[1]);
			sprintf(outString, "Jumping to [%d, %d]\n", jumpDest[0], jumpDest[1]);
			strcat(ChunkDebugMsg, outString);
		}
		strcat(ChunkDebugMsg, "\n");
	}

	if (chunk1) {
		strcat(ChunkDebugMsg, "CHUNK1\n");

		for (int i = 0; i < 4; i++) {
			sprintf(outString, "stepPos[%d] = (%d, %d)\n",
				i,
				chunk1->stepPos[i][0],
				chunk1->stepPos[i][1]);
			strcat(ChunkDebugMsg, outString);
		}

		sprintf(outString, "stepRelPos = %d, %d, %d\n",
			chunk1->stepRelPos[0],
			chunk1->stepRelPos[1],
			chunk1->stepRelPos[2]);
		strcat(ChunkDebugMsg, outString);

		sprintf(outString, "numSteps = %d\n", chunk1->numSteps);
		strcat(ChunkDebugMsg, outString);

		sprintf(outString, "run = %c\n", chunk1->run ? 'T' : 'F');
		strcat(ChunkDebugMsg, outString);

		sprintf(outString, "moving = %c\n", chunk1->moving ? 'T' : 'F');
		strcat(ChunkDebugMsg, outString);
	}

	if (chunk2) {
		strcat(ChunkDebugMsg, "\nCHUNK2\n");

		for (int i = 0; i < 4; i++) {
			sprintf(outString, "stepPos[%d] = (%d, %d)\n",
				i,
				chunk2->stepPos[i][0],
				chunk2->stepPos[i][1]);
			strcat(ChunkDebugMsg, outString);
		}

		sprintf(outString, "stepRelPos = %d, %d, %d\n",
			chunk2->stepRelPos[0],
			chunk2->stepRelPos[1],
			chunk2->stepRelPos[2]);
		strcat(ChunkDebugMsg, outString);

		sprintf(outString, "numSteps = %d\n", chunk2->numSteps);
		strcat(ChunkDebugMsg, outString);

		sprintf(outString, "moving = %c\n", chunk2->moving ? 'T' : 'F');
		strcat(ChunkDebugMsg, outString);

		sprintf(outString, "run = %c\n", chunk2->run ? 'T' : 'F');
		strcat(ChunkDebugMsg, outString);
	}

	File* debugFile = new File;
	debugFile->create("mvchunk.dbg");
	debugFile->writeString(ChunkDebugMsg);
	debugFile->close();
	delete debugFile;
	debugFile = nullptr;

	ExceptionGameMsg = ChunkDebugMsg;
}


void DebugStatusChunk (MoverPtr mover, StatusChunkPtr chunk1, StatusChunkPtr chunk2) {

	ChunkDebugMsg[0] = '\0';

	char outString[512];

	if (mover) {
		sprintf(outString, "\nmover = %s (%d)\n", mover->name, mover->getPartId());
		strcat(ChunkDebugMsg, outString);
		}
	else
		strcat(ChunkDebugMsg, "\nmover = ???\n");

	if (chunk1) {
		strcat(ChunkDebugMsg, "\nCHUNK1\n");

		GameObjectPtr target = nullptr;
		Stuff::Vector3D targetPoint;
		targetPoint.Zero();
		bool isTargetPoint = false;
		if (chunk1->targetType == STATUSCHUNK_TARGET_MOVER)
			target = (GameObjectPtr)MPlayer->moverRoster[chunk1->targetId];
		else if (chunk1->targetType == STATUSCHUNK_TARGET_TERRAIN)
			target = ObjectManager->findByPartId(chunk1->targetId);
		else if (chunk1->targetType == STATUSCHUNK_TARGET_SPECIAL)
			target = ObjectManager->findByPartId(chunk1->targetId);
		else if (chunk1->targetType == STATUSCHUNK_TARGET_LOCATION) {
			targetPoint.x = (float)chunk1->targetCellRC[1] * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
			targetPoint.y = (Terrain::worldUnitsMapSide / 2) - ((float)chunk1->targetCellRC[0] * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
			targetPoint.z = land->getTerrainElevation(targetPoint);
			isTargetPoint = true;
		}

		if (target) {
			if (target->isMover()) {
				sprintf(outString, "target = %s (%d)\n", ((MoverPtr)target)->name, target->getPartId());
				strcat(ChunkDebugMsg, outString);
				}
			else {
				sprintf(outString, "target = objClass %d (%d)\n", target->getObjectClass(), target->getPartId());
				strcat(ChunkDebugMsg, outString);
			}
			}
		else if (isTargetPoint) {
			sprintf(outString, "target point = (%f, %f, %f)\n", targetPoint.x, targetPoint.y, targetPoint.z);
			strcat(ChunkDebugMsg, outString);
			}
		else if (chunk1->targetType == STATUSCHUNK_TARGET_NONE) {
			sprintf(outString, "target = NONE\n");
			strcat(ChunkDebugMsg, outString);
			}
		else {
			strcat(ChunkDebugMsg, "target = ???\n");
			if (chunk1->targetType == STATUSCHUNK_TARGET_TERRAIN) {
				long numObjsInTile = 0;
				long partId = chunk1->targetId - chunk1->targetItemNumber;
				for (long i = 0; i < MAX_TERRAIN_TILE_ITEMS; i++) {
					GameObjectPtr curTarget = ObjectManager->findByPartId(partId + i);
					if (curTarget) {
						numObjsInTile++;
						sprintf(outString, "    %d: objClass %d (%d)\n", i, curTarget->getObjectClass(), curTarget->getPartId());
						strcat(ChunkDebugMsg, outString);
					}
				}
				if (numObjsInTile > 0) {
					sprintf(outString, "    There are %d terrain objects in this tile.\n", numObjsInTile);
					strcat(ChunkDebugMsg, outString);
				}
			}
		}

		sprintf(outString, "bodyState = %d\n", chunk1->bodyState);
		strcat(ChunkDebugMsg, outString);
			
		sprintf(outString, "targetType = %d\n", chunk1->targetType);
		strcat(ChunkDebugMsg, outString);

		sprintf(outString, "targetId = %d\n", chunk1->targetId);
		strcat(ChunkDebugMsg, outString);
			
		sprintf(outString, "targetBlockOrTrainNumber = %d\n", chunk1->targetBlockOrTrainNumber);
		strcat(ChunkDebugMsg, outString);
			
		sprintf(outString, "targetVertexOrCarNumber = %d\n", chunk1->targetVertexOrCarNumber);
		strcat(ChunkDebugMsg, outString);
			
		sprintf(outString, "targetItemNumber = %d\n", chunk1->targetItemNumber);
		strcat(ChunkDebugMsg, outString);
			
		sprintf(outString, "targetCellRC = (%d, %d)\n", chunk1->targetCellRC[0], chunk1->targetCellRC[1]);
		strcat(ChunkDebugMsg, outString);
			
		sprintf(outString, "ejectOrderGiven = %c\n", chunk1->ejectOrderGiven ? 'T' : 'F');
		strcat(ChunkDebugMsg, outString);
			
		sprintf(outString, "jumpOrder = %c\n", chunk1->jumpOrder ? 'T' : 'F');
		strcat(ChunkDebugMsg, outString);

		sprintf(outString, "data = %x\n", chunk1->data);
		strcat(ChunkDebugMsg, outString);
	}

	if (chunk2) {
		strcat(ChunkDebugMsg, "\nCHUNK2\n");

		GameObjectPtr target = nullptr;
		Stuff::Vector3D targetPoint;
		targetPoint.Zero();
		bool isTargetPoint = false;
		if (chunk1->targetType == STATUSCHUNK_TARGET_MOVER)
			target = (GameObjectPtr)MPlayer->moverRoster[chunk2->targetId];
		else if (chunk1->targetType == STATUSCHUNK_TARGET_TERRAIN)
			target = ObjectManager->findByPartId(chunk2->targetId);
		else if (chunk1->targetType == STATUSCHUNK_TARGET_SPECIAL)
			target = ObjectManager->findByPartId(chunk2->targetId);
		else if (chunk1->targetType == STATUSCHUNK_TARGET_LOCATION) {
			targetPoint.x = (float)chunk2->targetCellRC[1] * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
			targetPoint.y = (Terrain::worldUnitsMapSide / 2) - ((float)chunk2->targetCellRC[0] * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
			targetPoint.z = land->getTerrainElevation(targetPoint);
			isTargetPoint = true;
		}

		if (target) {
			if (target->isMover()) {
				sprintf(outString, "target = %s (%d)\n", ((MoverPtr)target)->name, target->getPartId());
				strcat(ChunkDebugMsg, outString);
				}
			else {
				sprintf(outString, "target = objClass %d (%d)\n", target->getObjectClass(), target->getPartId());
				strcat(ChunkDebugMsg, outString);
			}
			}
		else if (isTargetPoint) {
			sprintf(outString, "target point = (%f, %f, %f)\n", targetPoint.x, targetPoint.y, targetPoint.z);
			strcat(ChunkDebugMsg, outString);
			}
		else if (chunk1->targetType == STATUSCHUNK_TARGET_NONE) {
			sprintf(outString, "target = NONE\n");
			strcat(ChunkDebugMsg, outString);
			}
		else {
			strcat(ChunkDebugMsg, "target = ???\n");
			if (chunk2->targetType == STATUSCHUNK_TARGET_TERRAIN) {
				long numObjsInTile = 0;
				long partId = chunk1->targetId - chunk2->targetItemNumber;
				for (long i = 0; i < MAX_TERRAIN_TILE_ITEMS; i++) {
					GameObjectPtr curTarget = ObjectManager->findByPartId(partId + i);
					if (curTarget) {
						numObjsInTile++;
						sprintf(outString, "    %d: objClass %d (%d)\n", i, curTarget->getObjectClass(), curTarget->getPartId());
						strcat(ChunkDebugMsg, outString);
					}
				}
				if (numObjsInTile > 0) {
					sprintf(outString, "    There are %d terrain objects in this tile.\n", numObjsInTile);
					strcat(ChunkDebugMsg, outString);
				}
			}
		}

		sprintf(outString, "bodyState = %d\n", chunk2->bodyState);
		strcat(ChunkDebugMsg, outString);
			
		sprintf(outString, "targetType = %d\n", chunk2->targetType);
		strcat(ChunkDebugMsg, outString);

		sprintf(outString, "targetId = %d\n", chunk2->targetId);
		strcat(ChunkDebugMsg, outString);
			
		sprintf(outString, "targetBlockOrTrainNumber = %d\n", chunk2->targetBlockOrTrainNumber);
		strcat(ChunkDebugMsg, outString);
			
		sprintf(outString, "targetVertexOrCarNumber = %d\n", chunk2->targetVertexOrCarNumber);
		strcat(ChunkDebugMsg, outString);
			
		sprintf(outString, "targetItemNumber = %d\n", chunk2->targetItemNumber);
		strcat(ChunkDebugMsg, outString);
			
		sprintf(outString, "targetCellRC = (%d, %d)\n", chunk2->targetCellRC[0], chunk2->targetCellRC[1]);
		strcat(ChunkDebugMsg, outString);
			
		sprintf(outString, "ejectOrderGiven = %c\n", chunk2->ejectOrderGiven ? 'T' : 'F');
		strcat(ChunkDebugMsg, outString);
			
		sprintf(outString, "jumpOrder = %c\n", chunk2->jumpOrder ? 'T' : 'F');
		strcat(ChunkDebugMsg, outString);

		sprintf(outString, "data = %x\n", chunk2->data);
		strcat(ChunkDebugMsg, outString);
	}

	File* debugFile = new File;
	debugFile->create("stchunk.dbg");
	debugFile->writeString(ChunkDebugMsg);
	debugFile->close();
	delete debugFile;
	debugFile = nullptr;

	ExceptionGameMsg = ChunkDebugMsg;
}

extern GameObjectPtr DebugGameObject[3];

void Mover::updateDebugWindow (GameDebugWindow* debugWindow) {

	debugPage %= 3;

	debugWindow->clear();
	static char s[128];
	MovePathPtr path = getPilot()->getMovePath();
	long distToObj1 = -1;
	if (DebugGameObject[2])
		distToObj1 = (long)distanceFrom(DebugGameObject[2]->getPosition());
	long sensorRange = 0;
	if (sensorSystem)
	sensorRange = (long)sensorSystem->getEffectiveRange();
	long contStat = contactInfo->getContactStatus(0, true);
	static const char* contactStr[NUM_CONTACT_STATUSES] = {
		"        ",
		"SENSOR_1",
		"SENSOR_2",
		"SENSOR_3",
		"SENSOR_4",
		"VISUAL  "
	};

	if (debugPage == 0) {
		sprintf(s, "%s (%s %02d)", getName(), getPilot()->getName(), getPilot()->getIndex());
		debugWindow->print(s);
		long gestID = -1;
		if (getObjectClass() == BATTLEMECH)
			gestID = ((Mech3DAppearance*)appearance)->getCurrentGestureId();
		sprintf(s, "cmdr: %d, team: %d, gestID: %d, partID: %d", getCommanderId(), getTeamId(), gestID, getPartId());
		debugWindow->print(s);
		if (path->curStep < path->numStepsWhenNotPaused) {
			long curDir = path->stepList[path->curStep].direction;
			if (curDir > 7)
				sprintf(s, "pos: [%d, %d](%d) snsr: %d[%s] JMP", cellPositionRow, cellPositionCol, distToObj1, sensorRange, contactStr[contStat]);
			else
				sprintf(s, "pos: [%d, %d](%d) snsr: %d[%s] MOV", cellPositionRow, cellPositionCol, distToObj1, sensorRange, contactStr[contStat]);
			}
		else
			sprintf(s, "pos: [%d, %d](%d) snsr: %d[%s]", cellPositionRow, cellPositionCol, distToObj1, sensorRange, contactStr[contStat]);
		debugWindow->print(s);
		sprintf(s, "range: [%.f/%.f/%.f] %.f",
			getMinFireRange(),
			getOptimalFireRange(),
			getMaxFireRange(),
			getPilot()->getCurrentTarget() ? distanceFrom(getPilot()->getCurrentTarget()->getPosition()) : 0.0);
		debugWindow->print(s);
		strcpy(s, "order: ");
		if (getPilot()->getNumTacOrdersQueued()) {
			if (getPilot()->getExecutingTacOrderQueue()) {
				char ts[40];
				sprintf(ts, "[%d%c] ", getPilot()->getNumTacOrdersQueued(), getPilot()->getTacOrderQueueLooping() ? '@' : '.');
				strcat(s, ts);
			}
			else {
				char ts[40];
				sprintf(ts, "[%d] ", getPilot()->getNumTacOrdersQueued());
				strcat(s, ts);
			}
		}
		getPilot()->getCurTacOrder()->debugString(getPilot(), &s[7]);
		debugWindow->print(s);

		if (getPilot()->getBrain())
		{
			ModuleInfo brainInfo;
			getPilot()->getBrain()->getInfo(&brainInfo);
			debugWindow->print(" ");
			sprintf(s, "brain: %s", brainInfo.name);
			debugWindow->print(s);
			// Skip the "data\missions\warriors" text when printing filename...
			sprintf(s, "file: %s", &brainInfo.fileName[23]);
			debugWindow->print(s);
		}
		
		sprintf(s, "cont:");
		if (sensorSystem) {
			long numConts = 4;
			if (sensorSystem->numContacts < 4)
				numConts = sensorSystem->numContacts;
			for (long i = 0; i < numConts; i++) {
				MoverPtr contact = (MoverPtr)ObjectManager->get(sensorSystem->contacts[i] & 0x7FFF);
				Assert(contact != nullptr, sensorSystem->contacts[i] & 0x7FFF, " null contact ");
				char s2[10];
				sprintf(s2, " %d.%d", sensorSystem->contacts[i] & 0x7FFF, contact->getContactStatus(teamId, true));
				strcat(s, s2);
			}
			if (sensorSystem->numContacts > 4)
				strcat(s, " +++");
		}
		debugWindow->print(s);

		if (getPilot()->getCurrentTarget()) {
			char ts[50];
			getPilot()->printWeaponsStatus(ts);
			sprintf(s, "%s", ts);
			}
		else
			sprintf(s, "no current target");
		debugWindow->print(s);
		for (long i = 0; i < 5; i++) {
			sprintf(s, "%s", getPilot()->getDebugString(i));
			debugWindow->print(s);
		}
		}
	else if (debugPage == 1) {
		sprintf(s, "%s (%s %02d)", getName(), getPilot()->getName(), getPilot()->getIndex());
		debugWindow->print(s);
		sprintf(s, "team: %d, handle: %d, partID: %d, rank: %d", getTeamId(), getHandle(), getPartId(), getPilot()->getRank());
		debugWindow->print(s);
		if (path->curStep < path->numStepsWhenNotPaused) {
			long curDir = path->stepList[path->curStep].direction;
			if (curDir > 7)
				sprintf(s, "pos: [%d, %d](%d) snsr: %d[%s] JMP", cellPositionRow, cellPositionCol, distToObj1, sensorRange, contactStr[contStat]);
			else
				sprintf(s, "pos: [%d, %d](%d) snsr: %d[%s] MOV", cellPositionRow, cellPositionCol, distToObj1, sensorRange, contactStr[contStat]);
			}
		else
			sprintf(s, "pos: [%d, %d](%d) snsr: %d[%s]", cellPositionRow, cellPositionCol, distToObj1, sensorRange, contactStr[contStat]);
		debugWindow->print(s);
		sprintf(s, "cur step = [%d, %d], area = %d", 0, 0, GlobalMoveMap[0]->calcArea(cellPositionRow, cellPositionCol));
		debugWindow->print(s);
		MoveOrders* moveOrders = getPilot()->getMoveOrders();
		if (moveOrders->numGlobalSteps > 0) {
			sprintf(s, "LR:");
			long lastIndex = moveOrders->curGlobalStep + 8;
			if (lastIndex > moveOrders->numGlobalSteps)
				lastIndex = moveOrders->numGlobalSteps;
			for (long i = 0; i < lastIndex; i++) {
				char ss[15];
				if (i == moveOrders->curGlobalStep)
					sprintf(ss, " [%d]", moveOrders->globalPath[i].thruArea);
				else
					sprintf(ss, " %d", moveOrders->globalPath[i].thruArea);
				strcat(s, ss);
			}
			if (lastIndex < moveOrders->numGlobalSteps)
				strcat(s, " +");
			debugWindow->print(s);
		}
		static const char* moveStateStr[NUM_MOVESTATES] = {
			"SS",
			"FF",
			"RR",
			"*F",
			"*R",
			"*T"
		};
		sprintf(s, "MoveState Cur/Goal = %s[%s]",
			moveStateStr[pilot->getMoveState()],
			moveStateStr[pilot->getMoveStateGoal()]);
		debugWindow->print(s);
		GameObjectPtr target = pilot->getCurrentTarget();
		float torsoRelFacing = -1.0;
		if (objectClass == BATTLEMECH) {
			sprintf(s, "torsoRotation = %.2f", ((BattleMechPtr)this)->torsoRotation);
			debugWindow->print(s);
			if (target)
				torsoRelFacing = relFacingTo(target->getPosition()) + ((BattleMechPtr)this)->torsoRotation;
			else if (pilot->getCurTacOrder()->code == TACTICAL_ORDER_ATTACK_POINT)
				torsoRelFacing = relFacingTo(pilot->getAttackTargetPoint()) + ((BattleMechPtr)this)->torsoRotation;
			else
				torsoRelFacing = ((BattleMechPtr)this)->torsoRotation;
			sprintf(s, "C-Bills = %d", ((BattleMechPtr)this)->cBills);
			debugWindow->print(s);
			}
		else if (objectClass == GROUNDVEHICLE) {
			sprintf(s, "turretRotation = %.2f", ((GroundVehiclePtr)this)->turretRotation);
			debugWindow->print(s);
			if (target)
				torsoRelFacing = relFacingTo(target->getPosition()) + ((GroundVehiclePtr)this)->turretRotation;
			else if (pilot->getCurTacOrder()->code == TACTICAL_ORDER_ATTACK_POINT)
				torsoRelFacing = relFacingTo(pilot->getAttackTargetPoint()) + ((GroundVehiclePtr)this)->turretRotation;
			else
				torsoRelFacing = ((GroundVehiclePtr)this)->turretRotation;
		}
		sprintf(s, "torsoRelFacing = %.2f", torsoRelFacing);
		debugWindow->print(s);
		}
	else if (debugPage == 2) {
		static const char* locationStrings [] = {
			"head:     ",
			"c torso:  ",
			"l torso:  ",
			"r torso:  ",
			"l arm:    ",
			"r arm:    ",
			"l leg:    ",
			"r leg:    ",
			"rc torso: ",
			"rl torso: ",
			"rr torso: ",
			"front:    ",
			"left:     ",
			"right:    ",
			"rear:     ",
			"turret:   "
		};
		static const char* bodyState[] = {
			"normal",
			"DAM",
			"DEST"
		};
		sprintf(s, "%s (%s %02d)", getName(), getPilot()->getName(), getPilot()->getIndex());
		debugWindow->print(s);
		sprintf(s, "team: %d, partID: %d, threat: %d", getTeamId(), getPartId(), getThreatRating());
		debugWindow->print(s);
		debugWindow->print(" ");
		if (getObjectClass() == BATTLEMECH) {
			for (long i = 0; i < numBodyLocations; i++) {
				sprintf(s, "%s IS:%3.1f(%02d) AR:%3.1f(%02d) %s",
					locationStrings[i],
					body[i].curInternalStructure,
					body[i].maxInternalStructure,
					armor[i].curArmor,
					armor[i].maxArmor,
					bodyState[body[i].damageState]);
				debugWindow->print(s);
			}
			for (int i = numArmorLocations; i < numArmorLocations; i++) {
				sprintf(s, "%s AR:%.3f(%02d)",
					locationStrings[i],
					armor[i].curArmor,
					armor[i].maxArmor);
				debugWindow->print(s);
			}
		}
		else if (getObjectClass() == GROUNDVEHICLE) {
			for (long i = 0; i < numBodyLocations; i++) {
				sprintf(s, "%s IS:%3.1f(%02d) AR:%3.1f(%02d) %s",
					locationStrings[i + 11],
					body[i].curInternalStructure,
					body[i].maxInternalStructure,
					armor[i].curArmor,
					armor[i].maxArmor,
					bodyState[body[i].damageState]);
				debugWindow->print(s);
			}
		}
	}
}

void Mover::printFireWeaponDebugInfo (GameObjectPtr target, Stuff::Vector3D* targetPoint, long chance, long aimLocation, long roll, WeaponShotInfo* shotInfo) {

	if (!CombatLog)
		return;

	static const char* locationStrings [] = {
		"head",
		"center torso",
		"left torso",
		"right torso",
		"left arm",
		"right arm",
		"left leg",
		"right leg",
		"rear center torso",
		"rear left torso",
		"rear right torso"
	};

	if (roll < chance) {
		if (target) {
			char* targetName = target->getName();
			char s[1024];
			if (getObjectClass() == BATTLEMECH)
				sprintf(s, "[%.2f] mech.fireWeapon HIT: (%05d)%s @ (%05d)%s", scenarioTime, getPartId(), name, target->getPartId(), targetName ? targetName : "unknown");
			else
				sprintf(s, "[%.2f] vehicle.fireWeapon HIT: (%05d)%s @ (%05d)%s", scenarioTime, getPartId(), name, target->getPartId(), targetName ? targetName : "unknown");
			CombatLog->write(s);
			sprintf(s, "     chance = %03d, roll = %03d", chance, roll);
			CombatLog->write(s);
			sprintf(s, "     weapon = (%03d)%s, hitLocation = (%d)%s, damage = %.2f, angle = %.2f",
				shotInfo->masterId, MasterComponent::masterList[shotInfo->masterId].getName(),
				shotInfo->hitLocation, (shotInfo->hitLocation > -1) ? locationStrings[shotInfo->hitLocation] : "none",
				shotInfo->damage,
				shotInfo->entryAngle);
			CombatLog->write(s);
			if (aimLocation != -1) {
				sprintf(s, "     aimed shot = (%d)%s", aimLocation, locationStrings[aimLocation]);
				CombatLog->write(s);
			}
			sprintf(s, "     attacker pilot = %s (team id = %d)", getPilot()->getName(), getTeamId());
			CombatLog->write(s);
			CombatLog->write(" ");
			}
		else if (targetPoint) {
		}
		}
	else {
		if (target) {
			char* targetName = target->getName();
			char s[1024];
			if (getObjectClass() == BATTLEMECH)
				sprintf(s, "[%.2f] mech.fireWeapon MISS: (%05d)%s @ (%05d)%s", scenarioTime, getPartId(), name, target->getPartId(), targetName ? targetName : "unknown");
			else
				sprintf(s, "[%.2f] vehicle.fireWeapon HIT: (%05d)%s @ (%05d)%s", scenarioTime, getPartId(), name, target->getPartId(), targetName ? targetName : "unknown");
			CombatLog->write(s);
			sprintf(s, "     chance = %03d, roll = %03d", chance, roll);
			CombatLog->write(s);
			sprintf(s, "     weapon = (%03d)%s, hitLocation = (%d)%s, damage = %.2f, angle = %.2f",
				shotInfo->masterId, MasterComponent::masterList[shotInfo->masterId].getName(),
				shotInfo->hitLocation, (shotInfo->hitLocation > -1) ? locationStrings[shotInfo->hitLocation] : "none",
				shotInfo->damage,
				shotInfo->entryAngle);
			CombatLog->write(s);
			if (aimLocation != -1) {
				sprintf(s, "     aimed shot = (%d)%s", aimLocation, locationStrings[aimLocation]);
				CombatLog->write(s);
			}
			sprintf(s, "     attacker pilot = %s (team id = %d)", getPilot()->getName(), getTeamId());
			CombatLog->write(s);
			CombatLog->write(" ");
			}
		else if (targetPoint) {
		}
	}
}

bool FromMP = false;

void Mover::printHandleWeaponHitDebugInfo (WeaponShotInfo* shotInfo) {

	if (!CombatLog)
		return;

	static const char* locationStrings [] = {
		"head",
		"center torso",
		"left torso",
		"right torso",
		"left arm",
		"right arm",
		"left leg",
		"right leg",
		"rear center torso",
		"rear left torso",
		"rear right torso"
	};

	char s[1024];
	char statusStr[15];
	if (FromMP) {
		if (isDestroyed())
			sprintf(statusStr, " DESTROYED:");
		else if (getTeam() && Team::noPain[getTeamId()])
			sprintf(statusStr, " NO PAIN:");
		else
			sprintf(statusStr, ":");
	}
	else {
		if (isDestroyed())
			sprintf(statusStr, " *** DESTROYED:");
		else if (getTeam() && Team::noPain[getTeamId()])
			sprintf(statusStr, " *** NO PAIN:");
		else
			sprintf(statusStr, " ***:");
	}
	if (getObjectClass() == BATTLEMECH)
		sprintf(s, "[%.2f] mech.handleWeaponHit%s (%05d)%s ",
			scenarioTime,
			statusStr,
			getPartId(),
			name);
	else
		sprintf(s, "[%.2f] vehicle.handleWeaponHit%s (%05d)%s ",
			scenarioTime,
			statusStr,
			getPartId(),
			name);
	CombatLog->write(s);
	GameObjectPtr attacker = ObjectManager->getByWatchID(shotInfo->attackerWID);
	if (attacker)
		sprintf(s, "     attacker = (%05d)%s", attacker->getPartId(), attacker->getName());
	else
		sprintf(s, "     attacker = (%05d)<unknown WID>", shotInfo->attackerWID);
	CombatLog->write(s);
	sprintf(s, "     weapon = (%03d)%s, hitLocation = (%d)%s, damage = %.2f, angle = %.2f",
		shotInfo->masterId, MasterComponent::masterList[shotInfo->masterId].getName(),
		shotInfo->hitLocation, (shotInfo->hitLocation > -1) ? locationStrings[shotInfo->hitLocation] : "none",
		shotInfo->damage,
		shotInfo->entryAngle);
	CombatLog->write(s);
	sprintf(s, "     num weaponhits = %d", numWeaponHitsHandled);
	CombatLog->write(s);
	CombatLog->write(" ");
}
