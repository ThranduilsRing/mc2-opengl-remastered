// Mover: diagnostic dumps: chunk dump helpers, updateDebugWindow and the combat-log weapon fire/hit printers.
// Declared in mover.h; shared names in mover_internal.h.
#include <algorithm>

#include "mover_internal.h"
#include "mc2fmt.h"


void DebugMoveChunk (MoverPtr mover, MoveChunkPtr chunk1, MoveChunkPtr chunk2) {

	ChunkDebugMsg[0] = '\0';
	char outString[512];

	if (mover) {
		mc2::fmt::to_buf(outString, "Mover = {} ({:d})\n", mc2::fmt::safe(mover->getName()), mc2::fmt::as_i32(mover->getPartId()));
		strcat(ChunkDebugMsg, outString);

		mc2::fmt::to_buf(outString, "Mover World Pos = ({:.4f}, {:.4f}, {:.4f})\n", mover->getPosition().x, mover->getPosition().y, mover->getPosition().z);
		strcat(ChunkDebugMsg, outString);

		int cellPos[2];
		mover->getCellPosition(cellPos[0], cellPos[1]);
		mc2::fmt::to_buf(outString, "Mover Obj Cell Pos = [{:d}, {:d}]\n", mc2::fmt::as_i32(cellPos[0]), mc2::fmt::as_i32(cellPos[1]));
		strcat(ChunkDebugMsg, outString);
		if (mover->getPilot() == nullptr)
			strcat(ChunkDebugMsg, "NULL pilot!\n");
		if ((mover->getObjectClass() == BATTLEMECH) && (static_cast<BattleMechPtr>(mover))->inJump) {
			int jumpDest[2];
			land->worldToCell((static_cast<BattleMechPtr>(mover))->jumpGoal, jumpDest[0], jumpDest[1]);
			mc2::fmt::to_buf(outString, "Jumping to [{:d}, {:d}]\n", mc2::fmt::as_i32(jumpDest[0]), mc2::fmt::as_i32(jumpDest[1]));
			strcat(ChunkDebugMsg, outString);
		}
		strcat(ChunkDebugMsg, "\n");
	}

	if (chunk1) {
		strcat(ChunkDebugMsg, "CHUNK1\n");

		for (int i = 0; i < 4; i++) {
			mc2::fmt::to_buf(outString, "stepPos[{:d}] = ({:d}, {:d})\n", mc2::fmt::as_i32(i), mc2::fmt::as_i32(chunk1->stepPos[i][0]), mc2::fmt::as_i32(chunk1->stepPos[i][1]));
			strcat(ChunkDebugMsg, outString);
		}

		mc2::fmt::to_buf(outString, "stepRelPos = {:d}, {:d}, {:d}\n", mc2::fmt::as_i32(chunk1->stepRelPos[0]), mc2::fmt::as_i32(chunk1->stepRelPos[1]), mc2::fmt::as_i32(chunk1->stepRelPos[2]));
		strcat(ChunkDebugMsg, outString);

		mc2::fmt::to_buf(outString, "numSteps = {:d}\n", mc2::fmt::as_i32(chunk1->numSteps));
		strcat(ChunkDebugMsg, outString);

		mc2::fmt::to_buf(outString, "run = {:c}\n", mc2::fmt::as_char(chunk1->run ? 'T' : 'F'));
		strcat(ChunkDebugMsg, outString);

		mc2::fmt::to_buf(outString, "moving = {:c}\n", mc2::fmt::as_char(chunk1->moving ? 'T' : 'F'));
		strcat(ChunkDebugMsg, outString);
	}

	if (chunk2) {
		strcat(ChunkDebugMsg, "\nCHUNK2\n");

		for (int i = 0; i < 4; i++) {
			mc2::fmt::to_buf(outString, "stepPos[{:d}] = ({:d}, {:d})\n", mc2::fmt::as_i32(i), mc2::fmt::as_i32(chunk2->stepPos[i][0]), mc2::fmt::as_i32(chunk2->stepPos[i][1]));
			strcat(ChunkDebugMsg, outString);
		}

		mc2::fmt::to_buf(outString, "stepRelPos = {:d}, {:d}, {:d}\n", mc2::fmt::as_i32(chunk2->stepRelPos[0]), mc2::fmt::as_i32(chunk2->stepRelPos[1]), mc2::fmt::as_i32(chunk2->stepRelPos[2]));
		strcat(ChunkDebugMsg, outString);

		mc2::fmt::to_buf(outString, "numSteps = {:d}\n", mc2::fmt::as_i32(chunk2->numSteps));
		strcat(ChunkDebugMsg, outString);

		mc2::fmt::to_buf(outString, "moving = {:c}\n", mc2::fmt::as_char(chunk2->moving ? 'T' : 'F'));
		strcat(ChunkDebugMsg, outString);

		mc2::fmt::to_buf(outString, "run = {:c}\n", mc2::fmt::as_char(chunk2->run ? 'T' : 'F'));
		strcat(ChunkDebugMsg, outString);
	}

	File debugFile;
	debugFile.create("mvchunk.dbg");
	debugFile.writeString(ChunkDebugMsg);
	debugFile.close();

	ExceptionGameMsg = ChunkDebugMsg;
}


void DebugStatusChunk (MoverPtr mover, StatusChunkPtr chunk1, StatusChunkPtr chunk2) {

	ChunkDebugMsg[0] = '\0';

	char outString[512];

	if (mover) {
		mc2::fmt::to_buf(outString, "\nmover = {} ({:d})\n", mc2::fmt::safe(mover->name), mc2::fmt::as_i32(mover->getPartId()));
		strcat(ChunkDebugMsg, outString);
		}
	else
		strcat(ChunkDebugMsg, "\nmover = ???\n");

	if (chunk1) {
		strcat(ChunkDebugMsg, "\nCHUNK1\n");

		GameObjectPtr target = nullptr;
		glm::vec3 targetPoint;
		targetPoint = glm::vec3(0.0f);
		bool isTargetPoint = false;
		if (chunk1->targetType == STATUSCHUNK_TARGET_MOVER)
			target = static_cast<GameObjectPtr>(MPlayer->moverRoster[chunk1->targetId]);
		else if (chunk1->targetType == STATUSCHUNK_TARGET_TERRAIN)
			target = ObjectManager->findByPartId(chunk1->targetId);
		else if (chunk1->targetType == STATUSCHUNK_TARGET_SPECIAL)
			target = ObjectManager->findByPartId(chunk1->targetId);
		else if (chunk1->targetType == STATUSCHUNK_TARGET_LOCATION) {
			targetPoint.x = static_cast<float>(chunk1->targetCellRC[1]) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
			targetPoint.y = (Terrain::worldUnitsMapSide / 2) - (static_cast<float>(chunk1->targetCellRC[0]) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
			targetPoint.z = land->getTerrainElevation(targetPoint);
			isTargetPoint = true;
		}

		if (target) {
			if (target->isMover()) {
				mc2::fmt::to_buf(outString, "target = {} ({:d})\n", mc2::fmt::safe((static_cast<MoverPtr>(target))->name), mc2::fmt::as_i32(target->getPartId()));
				strcat(ChunkDebugMsg, outString);
				}
			else {
				mc2::fmt::to_buf(outString, "target = objClass {:d} ({:d})\n", mc2::fmt::as_i32(target->getObjectClass()), mc2::fmt::as_i32(target->getPartId()));
				strcat(ChunkDebugMsg, outString);
			}
			}
		else if (isTargetPoint) {
			mc2::fmt::to_buf(outString, "target point = ({:f}, {:f}, {:f})\n", targetPoint.x, targetPoint.y, targetPoint.z);
			strcat(ChunkDebugMsg, outString);
			}
		else if (chunk1->targetType == STATUSCHUNK_TARGET_NONE) {
			mc2::fmt::to_buf(outString, "target = NONE\n");
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
						mc2::fmt::to_buf(outString, "    {:d}: objClass {:d} ({:d})\n", mc2::fmt::as_i32(i), mc2::fmt::as_i32(curTarget->getObjectClass()), mc2::fmt::as_i32(curTarget->getPartId()));
						strcat(ChunkDebugMsg, outString);
					}
				}
				if (numObjsInTile > 0) {
					mc2::fmt::to_buf(outString, "    There are {:d} terrain objects in this tile.\n", mc2::fmt::as_i32(numObjsInTile));
					strcat(ChunkDebugMsg, outString);
				}
			}
		}

		mc2::fmt::to_buf(outString, "bodyState = {:d}\n", mc2::fmt::as_i32(chunk1->bodyState));
		strcat(ChunkDebugMsg, outString);
			
		mc2::fmt::to_buf(outString, "targetType = {:d}\n", mc2::fmt::as_i32(chunk1->targetType));
		strcat(ChunkDebugMsg, outString);

		mc2::fmt::to_buf(outString, "targetId = {:d}\n", mc2::fmt::as_i32(chunk1->targetId));
		strcat(ChunkDebugMsg, outString);
			
		mc2::fmt::to_buf(outString, "targetBlockOrTrainNumber = {:d}\n", mc2::fmt::as_i32(chunk1->targetBlockOrTrainNumber));
		strcat(ChunkDebugMsg, outString);
			
		mc2::fmt::to_buf(outString, "targetVertexOrCarNumber = {:d}\n", mc2::fmt::as_i32(chunk1->targetVertexOrCarNumber));
		strcat(ChunkDebugMsg, outString);
			
		mc2::fmt::to_buf(outString, "targetItemNumber = {:d}\n", mc2::fmt::as_i32(chunk1->targetItemNumber));
		strcat(ChunkDebugMsg, outString);
			
		mc2::fmt::to_buf(outString, "targetCellRC = ({:d}, {:d})\n", mc2::fmt::as_i32(chunk1->targetCellRC[0]), mc2::fmt::as_i32(chunk1->targetCellRC[1]));
		strcat(ChunkDebugMsg, outString);
			
		mc2::fmt::to_buf(outString, "ejectOrderGiven = {:c}\n", mc2::fmt::as_char(chunk1->ejectOrderGiven ? 'T' : 'F'));
		strcat(ChunkDebugMsg, outString);
			
		mc2::fmt::to_buf(outString, "jumpOrder = {:c}\n", mc2::fmt::as_char(chunk1->jumpOrder ? 'T' : 'F'));
		strcat(ChunkDebugMsg, outString);

		mc2::fmt::to_buf(outString, "data = {:x}\n", mc2::fmt::as_u32(chunk1->data));
		strcat(ChunkDebugMsg, outString);
	}

	if (chunk2) {
		strcat(ChunkDebugMsg, "\nCHUNK2\n");

		GameObjectPtr target = nullptr;
		glm::vec3 targetPoint;
		targetPoint = glm::vec3(0.0f);
		bool isTargetPoint = false;
		if (chunk1->targetType == STATUSCHUNK_TARGET_MOVER)
			target = static_cast<GameObjectPtr>(MPlayer->moverRoster[chunk2->targetId]);
		else if (chunk1->targetType == STATUSCHUNK_TARGET_TERRAIN)
			target = ObjectManager->findByPartId(chunk2->targetId);
		else if (chunk1->targetType == STATUSCHUNK_TARGET_SPECIAL)
			target = ObjectManager->findByPartId(chunk2->targetId);
		else if (chunk1->targetType == STATUSCHUNK_TARGET_LOCATION) {
			targetPoint.x = static_cast<float>(chunk2->targetCellRC[1]) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
			targetPoint.y = (Terrain::worldUnitsMapSide / 2) - (static_cast<float>(chunk2->targetCellRC[0]) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
			targetPoint.z = land->getTerrainElevation(targetPoint);
			isTargetPoint = true;
		}

		if (target) {
			if (target->isMover()) {
				mc2::fmt::to_buf(outString, "target = {} ({:d})\n", mc2::fmt::safe((static_cast<MoverPtr>(target))->name), mc2::fmt::as_i32(target->getPartId()));
				strcat(ChunkDebugMsg, outString);
				}
			else {
				mc2::fmt::to_buf(outString, "target = objClass {:d} ({:d})\n", mc2::fmt::as_i32(target->getObjectClass()), mc2::fmt::as_i32(target->getPartId()));
				strcat(ChunkDebugMsg, outString);
			}
			}
		else if (isTargetPoint) {
			mc2::fmt::to_buf(outString, "target point = ({:f}, {:f}, {:f})\n", targetPoint.x, targetPoint.y, targetPoint.z);
			strcat(ChunkDebugMsg, outString);
			}
		else if (chunk1->targetType == STATUSCHUNK_TARGET_NONE) {
			mc2::fmt::to_buf(outString, "target = NONE\n");
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
						mc2::fmt::to_buf(outString, "    {:d}: objClass {:d} ({:d})\n", mc2::fmt::as_i32(i), mc2::fmt::as_i32(curTarget->getObjectClass()), mc2::fmt::as_i32(curTarget->getPartId()));
						strcat(ChunkDebugMsg, outString);
					}
				}
				if (numObjsInTile > 0) {
					mc2::fmt::to_buf(outString, "    There are {:d} terrain objects in this tile.\n", mc2::fmt::as_i32(numObjsInTile));
					strcat(ChunkDebugMsg, outString);
				}
			}
		}

		mc2::fmt::to_buf(outString, "bodyState = {:d}\n", mc2::fmt::as_i32(chunk2->bodyState));
		strcat(ChunkDebugMsg, outString);
			
		mc2::fmt::to_buf(outString, "targetType = {:d}\n", mc2::fmt::as_i32(chunk2->targetType));
		strcat(ChunkDebugMsg, outString);

		mc2::fmt::to_buf(outString, "targetId = {:d}\n", mc2::fmt::as_i32(chunk2->targetId));
		strcat(ChunkDebugMsg, outString);
			
		mc2::fmt::to_buf(outString, "targetBlockOrTrainNumber = {:d}\n", mc2::fmt::as_i32(chunk2->targetBlockOrTrainNumber));
		strcat(ChunkDebugMsg, outString);
			
		mc2::fmt::to_buf(outString, "targetVertexOrCarNumber = {:d}\n", mc2::fmt::as_i32(chunk2->targetVertexOrCarNumber));
		strcat(ChunkDebugMsg, outString);
			
		mc2::fmt::to_buf(outString, "targetItemNumber = {:d}\n", mc2::fmt::as_i32(chunk2->targetItemNumber));
		strcat(ChunkDebugMsg, outString);
			
		mc2::fmt::to_buf(outString, "targetCellRC = ({:d}, {:d})\n", mc2::fmt::as_i32(chunk2->targetCellRC[0]), mc2::fmt::as_i32(chunk2->targetCellRC[1]));
		strcat(ChunkDebugMsg, outString);
			
		mc2::fmt::to_buf(outString, "ejectOrderGiven = {:c}\n", mc2::fmt::as_char(chunk2->ejectOrderGiven ? 'T' : 'F'));
		strcat(ChunkDebugMsg, outString);
			
		mc2::fmt::to_buf(outString, "jumpOrder = {:c}\n", mc2::fmt::as_char(chunk2->jumpOrder ? 'T' : 'F'));
		strcat(ChunkDebugMsg, outString);

		mc2::fmt::to_buf(outString, "data = {:x}\n", mc2::fmt::as_u32(chunk2->data));
		strcat(ChunkDebugMsg, outString);
	}

	File debugFile;
	debugFile.create("stchunk.dbg");
	debugFile.writeString(ChunkDebugMsg);
	debugFile.close();

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
		distToObj1 = static_cast<long>(distanceFrom(DebugGameObject[2]->getPosition()));
	long sensorRange = 0;
	if (sensorSystem)
	sensorRange = static_cast<long>(sensorSystem->getEffectiveRange());
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
		mc2::fmt::to_buf(s, "{} ({} {:02d})", mc2::fmt::safe(getName()), mc2::fmt::safe(getPilot()->getName()), mc2::fmt::as_i32(getPilot()->getIndex()));
		debugWindow->print(s);
		long gestID = -1;
		if (getObjectClass() == BATTLEMECH)
			gestID = (static_cast<Mech3DAppearance*>(appearance))->getCurrentGestureId();
		mc2::fmt::to_buf(s, "cmdr: {:d}, team: {:d}, gestID: {:d}, partID: {:d}", mc2::fmt::as_i32(getCommanderId()), mc2::fmt::as_i32(getTeamId()), mc2::fmt::as_i32(gestID), mc2::fmt::as_i32(getPartId()));
		debugWindow->print(s);
		if (path->curStep < path->numStepsWhenNotPaused) {
			long curDir = path->stepList[path->curStep].direction;
			if (curDir > 7)
				mc2::fmt::to_buf(s, "pos: [{:d}, {:d}]({:d}) snsr: {:d}[{}] JMP", mc2::fmt::as_i32(cellPositionRow), mc2::fmt::as_i32(cellPositionCol), mc2::fmt::as_i32(distToObj1), mc2::fmt::as_i32(sensorRange), mc2::fmt::safe(contactStr[contStat]));
			else
				mc2::fmt::to_buf(s, "pos: [{:d}, {:d}]({:d}) snsr: {:d}[{}] MOV", mc2::fmt::as_i32(cellPositionRow), mc2::fmt::as_i32(cellPositionCol), mc2::fmt::as_i32(distToObj1), mc2::fmt::as_i32(sensorRange), mc2::fmt::safe(contactStr[contStat]));
			}
		else
			mc2::fmt::to_buf(s, "pos: [{:d}, {:d}]({:d}) snsr: {:d}[{}]", mc2::fmt::as_i32(cellPositionRow), mc2::fmt::as_i32(cellPositionCol), mc2::fmt::as_i32(distToObj1), mc2::fmt::as_i32(sensorRange), mc2::fmt::safe(contactStr[contStat]));
		debugWindow->print(s);
		mc2::fmt::to_buf(s, "range: [{:.0f}/{:.0f}/{:.0f}] {:.0f}", getMinFireRange(), getOptimalFireRange(), getMaxFireRange(), getPilot()->getCurrentTarget() ? distanceFrom(getPilot()->getCurrentTarget()->getPosition()) : 0.0);
		debugWindow->print(s);
		strcpy(s, "order: ");
		if (getPilot()->getNumTacOrdersQueued()) {
			if (getPilot()->getExecutingTacOrderQueue()) {
				char ts[40];
				mc2::fmt::to_buf(ts, "[{:d}{:c}] ", mc2::fmt::as_i32(getPilot()->getNumTacOrdersQueued()), mc2::fmt::as_char(getPilot()->getTacOrderQueueLooping() ? '@' : '.'));
				strcat(s, ts);
			}
			else {
				char ts[40];
				mc2::fmt::to_buf(ts, "[{:d}] ", mc2::fmt::as_i32(getPilot()->getNumTacOrdersQueued()));
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
			mc2::fmt::to_buf(s, "brain: {}", mc2::fmt::safe(brainInfo.name));
			debugWindow->print(s);
			// Skip the "data\missions\warriors" text when printing filename...
			mc2::fmt::to_buf(s, "file: {}", mc2::fmt::safe(&brainInfo.fileName[23]));
			debugWindow->print(s);
		}
		
		mc2::fmt::to_buf(s, "cont:");
		if (sensorSystem) {
			long numConts = 4;
			if (sensorSystem->numContacts < 4)
				numConts = sensorSystem->numContacts;
			for (long i = 0; i < numConts; i++) {
				MoverPtr contact = static_cast<MoverPtr>(ObjectManager->get(sensorSystem->contacts[i] & 0x7FFF));
				Assert(contact != nullptr, sensorSystem->contacts[i] & 0x7FFF, " null contact ");
				char s2[10];
				mc2::fmt::to_buf(s2, " {:d}.{:d}", mc2::fmt::as_i32(sensorSystem->contacts[i] & 0x7FFF), mc2::fmt::as_i32(contact->getContactStatus(teamId, true)));
				strcat(s, s2);
			}
			if (sensorSystem->numContacts > 4)
				strcat(s, " +++");
		}
		debugWindow->print(s);

		if (getPilot()->getCurrentTarget()) {
			char ts[50];
			getPilot()->printWeaponsStatus(ts);
			mc2::fmt::to_buf(s, "{}", mc2::fmt::safe(ts));
			}
		else
			mc2::fmt::to_buf(s, "no current target");
		debugWindow->print(s);
		for (long i = 0; i < 5; i++) {
			mc2::fmt::to_buf(s, "{}", mc2::fmt::safe(getPilot()->getDebugString(i)));
			debugWindow->print(s);
		}
		}
	else if (debugPage == 1) {
		mc2::fmt::to_buf(s, "{} ({} {:02d})", mc2::fmt::safe(getName()), mc2::fmt::safe(getPilot()->getName()), mc2::fmt::as_i32(getPilot()->getIndex()));
		debugWindow->print(s);
		mc2::fmt::to_buf(s, "team: {:d}, handle: {:d}, partID: {:d}, rank: {:d}", mc2::fmt::as_i32(getTeamId()), mc2::fmt::as_i32(getHandle()), mc2::fmt::as_i32(getPartId()), mc2::fmt::as_i32(getPilot()->getRank()));
		debugWindow->print(s);
		if (path->curStep < path->numStepsWhenNotPaused) {
			long curDir = path->stepList[path->curStep].direction;
			if (curDir > 7)
				mc2::fmt::to_buf(s, "pos: [{:d}, {:d}]({:d}) snsr: {:d}[{}] JMP", mc2::fmt::as_i32(cellPositionRow), mc2::fmt::as_i32(cellPositionCol), mc2::fmt::as_i32(distToObj1), mc2::fmt::as_i32(sensorRange), mc2::fmt::safe(contactStr[contStat]));
			else
				mc2::fmt::to_buf(s, "pos: [{:d}, {:d}]({:d}) snsr: {:d}[{}] MOV", mc2::fmt::as_i32(cellPositionRow), mc2::fmt::as_i32(cellPositionCol), mc2::fmt::as_i32(distToObj1), mc2::fmt::as_i32(sensorRange), mc2::fmt::safe(contactStr[contStat]));
			}
		else
			mc2::fmt::to_buf(s, "pos: [{:d}, {:d}]({:d}) snsr: {:d}[{}]", mc2::fmt::as_i32(cellPositionRow), mc2::fmt::as_i32(cellPositionCol), mc2::fmt::as_i32(distToObj1), mc2::fmt::as_i32(sensorRange), mc2::fmt::safe(contactStr[contStat]));
		debugWindow->print(s);
		mc2::fmt::to_buf(s, "cur step = [{:d}, {:d}], area = {:d}", mc2::fmt::as_i32(0), mc2::fmt::as_i32(0), mc2::fmt::as_i32(GlobalMoveMap[0]->calcArea(cellPositionRow, cellPositionCol)));
		debugWindow->print(s);
		MoveOrders* moveOrders = getPilot()->getMoveOrders();
		if (moveOrders->numGlobalSteps > 0) {
			mc2::fmt::to_buf(s, "LR:");
			long lastIndex = moveOrders->curGlobalStep + 8;
			lastIndex = std::min<long>(lastIndex, moveOrders->numGlobalSteps);
			for (long i = 0; i < lastIndex; i++) {
				char ss[15];
				if (i == moveOrders->curGlobalStep)
					mc2::fmt::to_buf(ss, " [{:d}]", mc2::fmt::as_i32(moveOrders->globalPath[i].thruArea));
				else
					mc2::fmt::to_buf(ss, " {:d}", mc2::fmt::as_i32(moveOrders->globalPath[i].thruArea));
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
		mc2::fmt::to_buf(s, "MoveState Cur/Goal = {}[{}]", mc2::fmt::safe(moveStateStr[pilot->getMoveState()]), mc2::fmt::safe(moveStateStr[pilot->getMoveStateGoal()]));
		debugWindow->print(s);
		GameObjectPtr target = pilot->getCurrentTarget();
		float torsoRelFacing = -1.0;
		if (objectClass == BATTLEMECH) {
			mc2::fmt::to_buf(s, "torsoRotation = {:.2f}", (static_cast<BattleMechPtr>(this))->torsoRotation);
			debugWindow->print(s);
			if (target)
				torsoRelFacing = relFacingTo(target->getPosition()) + (static_cast<BattleMechPtr>(this))->torsoRotation;
			else if (pilot->getCurTacOrder()->code == TACTICAL_ORDER_ATTACK_POINT)
				torsoRelFacing = relFacingTo(pilot->getAttackTargetPoint()) + (static_cast<BattleMechPtr>(this))->torsoRotation;
			else
				torsoRelFacing = (static_cast<BattleMechPtr>(this))->torsoRotation;
			mc2::fmt::to_buf(s, "C-Bills = {:d}", mc2::fmt::as_i32((static_cast<BattleMechPtr>(this))->cBills));
			debugWindow->print(s);
			}
		else if (objectClass == GROUNDVEHICLE) {
			mc2::fmt::to_buf(s, "turretRotation = {:.2f}", (static_cast<GroundVehiclePtr>(this))->turretRotation);
			debugWindow->print(s);
			if (target)
				torsoRelFacing = relFacingTo(target->getPosition()) + (static_cast<GroundVehiclePtr>(this))->turretRotation;
			else if (pilot->getCurTacOrder()->code == TACTICAL_ORDER_ATTACK_POINT)
				torsoRelFacing = relFacingTo(pilot->getAttackTargetPoint()) + (static_cast<GroundVehiclePtr>(this))->turretRotation;
			else
				torsoRelFacing = (static_cast<GroundVehiclePtr>(this))->turretRotation;
		}
		mc2::fmt::to_buf(s, "torsoRelFacing = {:.2f}", torsoRelFacing);
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
		mc2::fmt::to_buf(s, "{} ({} {:02d})", mc2::fmt::safe(getName()), mc2::fmt::safe(getPilot()->getName()), mc2::fmt::as_i32(getPilot()->getIndex()));
		debugWindow->print(s);
		mc2::fmt::to_buf(s, "team: {:d}, partID: {:d}, threat: {:d}", mc2::fmt::as_i32(getTeamId()), mc2::fmt::as_i32(getPartId()), mc2::fmt::as_i32(getThreatRating()));
		debugWindow->print(s);
		debugWindow->print(" ");
		if (getObjectClass() == BATTLEMECH) {
			for (long i = 0; i < numBodyLocations; i++) {
				mc2::fmt::to_buf(s, "{} IS:{:3.1f}({:02d}) AR:{:3.1f}({:02d}) {}", mc2::fmt::safe(locationStrings[i]), body[i].curInternalStructure, mc2::fmt::as_i32(body[i].maxInternalStructure), armor[i].curArmor, mc2::fmt::as_i32(armor[i].maxArmor), mc2::fmt::safe(bodyState[body[i].damageState]));
				debugWindow->print(s);
			}
			for (int i = numArmorLocations; i < numArmorLocations; i++) {
				mc2::fmt::to_buf(s, "{} AR:{:.3f}({:02d})", mc2::fmt::safe(locationStrings[i]), armor[i].curArmor, mc2::fmt::as_i32(armor[i].maxArmor));
				debugWindow->print(s);
			}
		}
		else if (getObjectClass() == GROUNDVEHICLE) {
			for (long i = 0; i < numBodyLocations; i++) {
				mc2::fmt::to_buf(s, "{} IS:{:3.1f}({:02d}) AR:{:3.1f}({:02d}) {}", mc2::fmt::safe(locationStrings[i + 11]), body[i].curInternalStructure, mc2::fmt::as_i32(body[i].maxInternalStructure), armor[i].curArmor, mc2::fmt::as_i32(armor[i].maxArmor), mc2::fmt::safe(bodyState[body[i].damageState]));
				debugWindow->print(s);
			}
		}
	}
}

void Mover::printFireWeaponDebugInfo (GameObjectPtr target, glm::vec3* targetPoint, long chance, long aimLocation, long roll, WeaponShotInfo* shotInfo) {

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
				mc2::fmt::to_buf(s, "[{:.2f}] mech.fireWeapon HIT: ({:05d}){} @ ({:05d}){}", scenarioTime, mc2::fmt::as_i32(getPartId()), mc2::fmt::safe(name), mc2::fmt::as_i32(target->getPartId()), mc2::fmt::safe(targetName ? targetName : "unknown"));
			else
				mc2::fmt::to_buf(s, "[{:.2f}] vehicle.fireWeapon HIT: ({:05d}){} @ ({:05d}){}", scenarioTime, mc2::fmt::as_i32(getPartId()), mc2::fmt::safe(name), mc2::fmt::as_i32(target->getPartId()), mc2::fmt::safe(targetName ? targetName : "unknown"));
			CombatLog->write(s);
			mc2::fmt::to_buf(s, "     chance = {:03d}, roll = {:03d}", mc2::fmt::as_i32(chance), mc2::fmt::as_i32(roll));
			CombatLog->write(s);
			mc2::fmt::to_buf(s, "     weapon = ({:03d}){}, hitLocation = ({:d}){}, damage = {:.2f}, angle = {:.2f}", mc2::fmt::as_i32(shotInfo->masterId), mc2::fmt::safe(MasterComponent::masterList[shotInfo->masterId].getName()), mc2::fmt::as_i32(shotInfo->hitLocation), mc2::fmt::safe((shotInfo->hitLocation > -1) ? locationStrings[shotInfo->hitLocation] : "none"), shotInfo->damage, shotInfo->entryAngle);
			CombatLog->write(s);
			if (aimLocation != -1) {
				mc2::fmt::to_buf(s, "     aimed shot = ({:d}){}", mc2::fmt::as_i32(aimLocation), mc2::fmt::safe(locationStrings[aimLocation]));
				CombatLog->write(s);
			}
			mc2::fmt::to_buf(s, "     attacker pilot = {} (team id = {:d})", mc2::fmt::safe(getPilot()->getName()), mc2::fmt::as_i32(getTeamId()));
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
				mc2::fmt::to_buf(s, "[{:.2f}] mech.fireWeapon MISS: ({:05d}){} @ ({:05d}){}", scenarioTime, mc2::fmt::as_i32(getPartId()), mc2::fmt::safe(name), mc2::fmt::as_i32(target->getPartId()), mc2::fmt::safe(targetName ? targetName : "unknown"));
			else
				mc2::fmt::to_buf(s, "[{:.2f}] vehicle.fireWeapon HIT: ({:05d}){} @ ({:05d}){}", scenarioTime, mc2::fmt::as_i32(getPartId()), mc2::fmt::safe(name), mc2::fmt::as_i32(target->getPartId()), mc2::fmt::safe(targetName ? targetName : "unknown"));
			CombatLog->write(s);
			mc2::fmt::to_buf(s, "     chance = {:03d}, roll = {:03d}", mc2::fmt::as_i32(chance), mc2::fmt::as_i32(roll));
			CombatLog->write(s);
			mc2::fmt::to_buf(s, "     weapon = ({:03d}){}, hitLocation = ({:d}){}, damage = {:.2f}, angle = {:.2f}", mc2::fmt::as_i32(shotInfo->masterId), mc2::fmt::safe(MasterComponent::masterList[shotInfo->masterId].getName()), mc2::fmt::as_i32(shotInfo->hitLocation), mc2::fmt::safe((shotInfo->hitLocation > -1) ? locationStrings[shotInfo->hitLocation] : "none"), shotInfo->damage, shotInfo->entryAngle);
			CombatLog->write(s);
			if (aimLocation != -1) {
				mc2::fmt::to_buf(s, "     aimed shot = ({:d}){}", mc2::fmt::as_i32(aimLocation), mc2::fmt::safe(locationStrings[aimLocation]));
				CombatLog->write(s);
			}
			mc2::fmt::to_buf(s, "     attacker pilot = {} (team id = {:d})", mc2::fmt::safe(getPilot()->getName()), mc2::fmt::as_i32(getTeamId()));
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
			mc2::fmt::to_buf(statusStr, " DESTROYED:");
		else if (getTeam() && Team::noPain[getTeamId()])
			mc2::fmt::to_buf(statusStr, " NO PAIN:");
		else
			mc2::fmt::to_buf(statusStr, ":");
	}
	else {
		if (isDestroyed())
			mc2::fmt::to_buf(statusStr, " *** DESTROYED:");
		else if (getTeam() && Team::noPain[getTeamId()])
			mc2::fmt::to_buf(statusStr, " *** NO PAIN:");
		else
			mc2::fmt::to_buf(statusStr, " ***:");
	}
	if (getObjectClass() == BATTLEMECH)
		mc2::fmt::to_buf(s, "[{:.2f}] mech.handleWeaponHit{} ({:05d}){} ", scenarioTime, mc2::fmt::safe(statusStr), mc2::fmt::as_i32(getPartId()), mc2::fmt::safe(name));
	else
		mc2::fmt::to_buf(s, "[{:.2f}] vehicle.handleWeaponHit{} ({:05d}){} ", scenarioTime, mc2::fmt::safe(statusStr), mc2::fmt::as_i32(getPartId()), mc2::fmt::safe(name));
	CombatLog->write(s);
	GameObjectPtr attacker = ObjectManager->getByWatchID(shotInfo->attackerWID);
	if (attacker)
		mc2::fmt::to_buf(s, "     attacker = ({:05d}){}", mc2::fmt::as_i32(attacker->getPartId()), mc2::fmt::safe(attacker->getName()));
	else
		mc2::fmt::to_buf(s, "     attacker = ({:05d})<unknown WID>", mc2::fmt::as_i32(shotInfo->attackerWID));
	CombatLog->write(s);
	mc2::fmt::to_buf(s, "     weapon = ({:03d}){}, hitLocation = ({:d}){}, damage = {:.2f}, angle = {:.2f}", mc2::fmt::as_i32(shotInfo->masterId), mc2::fmt::safe(MasterComponent::masterList[shotInfo->masterId].getName()), mc2::fmt::as_i32(shotInfo->hitLocation), mc2::fmt::safe((shotInfo->hitLocation > -1) ? locationStrings[shotInfo->hitLocation] : "none"), shotInfo->damage, shotInfo->entryAngle);
	CombatLog->write(s);
	mc2::fmt::to_buf(s, "     num weaponhits = {:d}", mc2::fmt::as_i32(numWeaponHitsHandled));
	CombatLog->write(s);
	CombatLog->write(" ");
}
