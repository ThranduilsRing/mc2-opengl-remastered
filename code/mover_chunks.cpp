// Mover: multiplayer MoveChunk/StatusChunk build, pack, unpack and compare, plus MoverDynamics and MoverControl.
// Declared in mover.h; shared names in mover_internal.h.
#include <algorithm>

#include "terrain_part_chunk.h"
#include "mover_internal.h"
#include "mc2fmt.h"



inline MoverPtr getMoverFromHandle (long handle) {

	return(dynamic_cast<MoverPtr>(ObjectManager->get(handle)));
}


constexpr int MOVECHUNK_CELLPOS_BITS = 9;
constexpr int MOVECHUNK_NUMSTEPS_BITS = 2;
constexpr int MOVECHUNK_RUN_BITS = 1;
constexpr int MOVECHUNK_MOVING_BITS = 1;
constexpr int MOVECHUNK_STEP_BITS = 3;

constexpr int MOVECHUNK_CELLPOS_MASK = 0x000001FF;
constexpr int MOVECHUNK_NUMSTEPS_MASK = 0x00000003;
constexpr int MOVECHUNK_RUN_MASK = 0x00000001;
constexpr int MOVECHUNK_MOVING_MASK = 0x00000001;
constexpr int MOVECHUNK_STEP_MASK = 0x00000007;

constexpr int MOVECHUNK_NUM_STEPS = 3;





inline void getAdjCell (long row,
						long col,
						long dir,
						long& adjRow,
						long& adjCol) {

	long adjMod[8][2] = {{-1, 0}, {-1, 1}, {0, 1}, {1, 1},
						 {1, 0}, {1, -1}, {0, -1}, {-1, -1}};
	adjRow = row + adjMod[dir][0];
	adjCol = col + adjMod[dir][1];
}

inline long cellDirToCell (long fromCellRow,
						   long fromCellCol,
						   long toCellRow,
						   long toCellCol) {

	// This routine assumes the two cells are adjacent!
	static long deltaDir[3][3] = {
		{7, 0, 1},
		{6, -1, 2},
		{5, 4, 3}
	};
	long rowCellDelta = toCellRow - fromCellRow + 1;
	long colCellDelta = toCellCol - fromCellCol + 1;
	if ((rowCellDelta < 0) || (rowCellDelta > 2))
		return(-2);
	if ((colCellDelta < 0) || (colCellDelta > 2))
		return(-2);
	long dir = deltaDir[rowCellDelta][colCellDelta];
	if (dir == -1)
		return(-2);
	return(dir);
}

void MoveChunk::build (MoverPtr mover, MovePathPtr path1, MovePathPtr path2) {

	int cellPos[2];
	mover->getCellPosition(cellPos[0], cellPos[1]);
	stepPos[0][0] = cellPos[0];
	stepPos[0][1] = cellPos[1];

	stepRelPos[0] = 0;
	stepRelPos[1] = 0;
	stepRelPos[2] = 0;

	numSteps = 1;

	moving = path1 && (path1->numSteps > 0);
	if (path1 && path1->numSteps > 0) {
		bool reachedEndOfPath1 = true;
		long curStep = 1;
		if (path1->curStep < path1->numSteps) {
			reachedEndOfPath1 = false;
			long curPath1Step = path1->curStep;
			bool startWithCurStep = false;
			if ((cellPos[0] == path1->stepList[curPath1Step].cell[0]) &&
				(cellPos[1] == path1->stepList[curPath1Step].cell[1]))
				startWithCurStep = true;
			else {
				// Whacky case, since current object position is not next to the
				// next step in the path...
				long relDir = cellDirToCell(cellPos[0],
											cellPos[1],
											path1->stepList[curPath1Step].cell[0],
											path1->stepList[curPath1Step].cell[1]);
				startWithCurStep = (relDir == -2);
			}
			if (startWithCurStep) {	
				stepPos[0][0] = path1->stepList[curPath1Step].cell[0];
				stepPos[0][1] = path1->stepList[curPath1Step].cell[1];

				curPath1Step++;

				numSteps = MOVECHUNK_NUM_STEPS;
				long numStepsLeft = path1->numSteps - curPath1Step;
				if (numStepsLeft < MOVECHUNK_NUM_STEPS) {
					numSteps = numStepsLeft;
					reachedEndOfPath1 = true;
				}
				for (long i = 0; i < numSteps; i++) {
					stepPos[curStep][0] = path1->stepList[curPath1Step].cell[0];
					stepPos[curStep][1] = path1->stepList[curPath1Step].cell[1];
					if (curStep == 1)
						stepRelPos[curStep - 1] = cellDirToCell(stepPos[0][0],
																stepPos[0][1],
																stepPos[1][0],
																stepPos[1][1]);
					else
						stepRelPos[curStep - 1] = path1->stepList[curPath1Step].direction;
					curPath1Step++;
					curStep++;
				}

				// Since we count the cur position as the first step...
				numSteps++;
				
				}
			else {

				numSteps = MOVECHUNK_NUM_STEPS;
				long numStepsLeft = path1->numSteps - curPath1Step;
				if (numStepsLeft < MOVECHUNK_NUM_STEPS) {
					numSteps = numStepsLeft;
					reachedEndOfPath1 = true;
				}
				for (long i = 0; i < numSteps; i++) {
					stepPos[curStep][0] = path1->stepList[curPath1Step].cell[0];
					stepPos[curStep][1] = path1->stepList[curPath1Step].cell[1];
					if (curStep == 1)
						stepRelPos[curStep - 1] = cellDirToCell(stepPos[0][0],
																stepPos[0][1],
																stepPos[1][0],
																stepPos[1][1]);
					else
						stepRelPos[curStep - 1] = path1->stepList[curPath1Step].direction;
					curPath1Step++;
					curStep++;
				}

				// Since we count the cur position as the first step...
				numSteps++;
			}
		}

		bool path2Continues = path2 && (path1->globalStep > -1) && (path2->globalStep == (path1->globalStep + 1));
		if (reachedEndOfPath1 && path2Continues && (path2->numStepsWhenNotPaused > 0)) {
			// Splice second (queued) path into the chunk.
			// One assumption: if the first path is empty, it is assumed the
			// second path is irrelevant...
//			Assert(curStep > 1, curStep, " MoveChunk.build: path2 and curStep == 1 ");
			if (numSteps < (MOVECHUNK_NUM_STEPS + 1)) {
				long numAdditionalSteps = (MOVECHUNK_NUM_STEPS + 1) - numSteps;
				numAdditionalSteps = std::min(numAdditionalSteps, path2->numSteps);
				for (long i = 0; i < numAdditionalSteps; i++) {
					Assert(curStep <= MOVECHUNK_NUM_STEPS, curStep, " MoveChunk.build: path2 and bad curStep > MOVECHUNK_NUM_STEPS ");
					stepPos[curStep][0] = path2->stepList[i].cell[0];
					stepPos[curStep][1] = path2->stepList[i].cell[1];
					if (curStep == 1)
						stepRelPos[curStep - 1] = cellDirToCell(stepPos[0][0],
																stepPos[0][1],
																stepPos[1][0],
																stepPos[1][1]);
					else
						stepRelPos[curStep - 1] = path2->stepList[i].direction;
					curStep++;
				}
				numSteps += numAdditionalSteps;
				Assert(numSteps <= (MOVECHUNK_NUM_STEPS + 1), curStep, " MoveChunk.build: path2 and bad curStep > MOVECHUNK_NUM_STEPS ");
			}
		}
	
	}

	if ((numSteps < 1)  || (numSteps > 4)) {
		// For now, a safety guard. However, it would be nice to catch this
		// sooner and avoid this HACK. EXPANSION DISK!
		stepPos[0][0] = cellPos[0];
		stepPos[0][1] = cellPos[1];

		stepRelPos[0] = 0;
		stepRelPos[1] = 0;
		stepRelPos[2] = 0;

		numSteps = 1;
	}

	run = mover->getPilot()->getMoveRun();
	if (run && (mover->getObjectClass() == BATTLEMECH))
		run = (static_cast<BattleMechPtr>(mover))->canRun();

	data = 0;
}

// DON'T USE THIS NOW, AS JUMPGOAL IS STORED IN THE STATUS CHUNK IF IT'S
// A JUMP ORDER!

void MoveChunk::build (MoverPtr mover, glm::vec3 jumpGoal) {

	int jumpCoords[2];
	land->worldToCell(jumpGoal, jumpCoords[0], jumpCoords[1]);

	stepPos[0][0] = jumpCoords[0];
	stepPos[0][1] = jumpCoords[1];

	stepRelPos[0] = 0;
	stepRelPos[1] = 0;
	stepRelPos[2] = 0;

	numSteps = 1;

	run = false;
	moving = false;
}

#define	DEBUG_MOVECHUNK

void MoveChunk::pack (MoverPtr mover) {

	data = stepPos[0][0];
	data <<= MOVECHUNK_CELLPOS_BITS;

	data |= stepPos[0][1];
	data <<= MOVECHUNK_NUMSTEPS_BITS;

	data |= (numSteps - 1);
	data <<= MOVECHUNK_RUN_BITS;

	if (run)
		data |= 1;
	data <<= MOVECHUNK_MOVING_BITS;
	
	if (moving)
		data |= 1;
	data <<= MOVECHUNK_STEP_BITS;

	data |= stepRelPos[0];
	data <<= MOVECHUNK_STEP_BITS;

	data |= stepRelPos[1];
	data <<= MOVECHUNK_STEP_BITS;

	data |= stepRelPos[2];

#ifdef DEBUG_MOVECHUNK
	if ((numSteps < 1) || (numSteps > 4)) {
		DebugMoveChunk(mover, this, nullptr);
		char errMsg[1024];
		(*std::format_to_n(errMsg, sizeof(errMsg) - 1, " MoveChunk.pack: bad numSteps {:d} (save mvchunk.dbg file) ", mc2::fmt::as_i32(numSteps)).out = '\0');
		Assert(false, numSteps, errMsg);
	}
#endif

}

void MoveChunk::unpack (MoverPtr mover) {

	err = 0;

	stepRelPos[2] = (data & MOVECHUNK_STEP_MASK);
	data >>= MOVECHUNK_STEP_BITS;

	stepRelPos[1] = (data & MOVECHUNK_STEP_MASK);
	data >>= MOVECHUNK_STEP_BITS;

	stepRelPos[0] = (data & MOVECHUNK_STEP_MASK);
	data >>= MOVECHUNK_STEP_BITS;

	moving = ((data & MOVECHUNK_MOVING_MASK) != 0);
	data >>= MOVECHUNK_MOVING_BITS;

	run = ((data & MOVECHUNK_RUN_MASK) != 0);
	data >>= MOVECHUNK_RUN_BITS;

	numSteps = (data & MOVECHUNK_NUMSTEPS_MASK) + 1;
	data >>= MOVECHUNK_NUMSTEPS_BITS;

	stepPos[0][1] = (data & MOVECHUNK_CELLPOS_MASK);
	data >>= MOVECHUNK_CELLPOS_BITS;

	stepPos[0][0] = (data & MOVECHUNK_CELLPOS_MASK);

	if ((numSteps < 1) || (numSteps > 4)) {
		// HACK!!!!!!!!!! All of this unpackerr stuff is a last minute hack...
		err = 1;
		return;
	}
	if (numSteps > 1) {
		for (long i = 0; i < (numSteps - 1); i++) {
			long nextStep = i + 1;
			if ((i < 0) || (i > 2)) {
				err = 2;
				return;
			}
			if ((stepRelPos[i] < 0) || (stepRelPos[i] > 7)) {
				err = 3;
				return;
			}
			getAdjCell(stepPos[i][0],
					   stepPos[i][1],
					   stepRelPos[i],
					   stepPos[nextStep][0],
					   stepPos[nextStep][1]);
		}
	}

#ifdef DEBUG_MOVECHUNK
	if ((numSteps < 1) || (numSteps > 4)) {
		DebugMoveChunk(mover, this, nullptr);
		char errMsg[1024];
		(*std::format_to_n(errMsg, sizeof(errMsg) - 1, " MoveChunk.unpack: bad numSteps {:d} (save mvchunk.dbg file) ", mc2::fmt::as_i32(numSteps)).out = '\0');
		Assert(false, numSteps, errMsg);
	}
#endif
}

bool MoveChunk::equalTo (MoverPtr mover, MoveChunkPtr chunk) {

	if (numSteps != chunk->numSteps) {
		DebugMoveChunk(mover, this, chunk);
		return(false);
	}

	if (run != chunk->run) {
		DebugMoveChunk(mover, this, chunk);
		return(false);
	}

	if (moving != chunk->moving) {
		DebugMoveChunk(mover, this, chunk);
		return(false);
	}

	for (long i = 0; i < numSteps; i++)
		if ((stepPos[i][0] != chunk->stepPos[i][0]) || (stepPos[i][1] != chunk->stepPos[i][1])) {
			DebugMoveChunk(mover, this, chunk);
			return(false);
		}

	for (int i = 0; i < numSteps - 1; i++) {
		if (stepRelPos[i] != chunk->stepRelPos[i]) {
			DebugMoveChunk(mover, this, chunk);
			return(false);
		}
	}

	return(true);
}

void Mover::setMoveChunk (MovePathPtr path, MoveChunkPtr chunk) {

	for (long i = 0; i < chunk->numSteps; i++) {
		PathStepPtr pathStep = &path->stepList[i];
		pathStep->cell[0] = chunk->stepPos[i][0];
		pathStep->cell[1] = chunk->stepPos[i][1];
		pathStep->distanceToGoal = 0.0;
		land->cellToWorld(pathStep->cell[0], pathStep->cell[1], pathStep->destination);
	}
	path->goal = path->stepList[chunk->numSteps - 1].destination;
	path->target = glm::vec3(0.0f);
	path->numSteps = path->numStepsWhenNotPaused = chunk->numSteps;
	path->curStep = 0;
	path->cost = -1;
	path->marked = false;
	path->globalStep = -1;
}	





void StatusChunk::build (MoverPtr mover) {

	bodyState = 0;

	data = 0;
}

#define	DEBUG_STATUSCHUNK

void StatusChunk::pack (MoverPtr mover) {

	data = 0;

	if (jumpOrder) {
		data |= targetCellRC[0];
		data <<= STATUSCHUNK_CELLPOS_BITS;

		data |= targetCellRC[1];
		data <<= STATUSCHUNK_TARGETTYPE_BITS;
		}
	else {
		switch (targetType) {
			case STATUSCHUNK_TARGET_MOVER:
				data |= targetId;
				data <<= STATUSCHUNK_TARGETTYPE_BITS;
				break;
			case STATUSCHUNK_TARGET_TERRAIN:
				data |= terrain_part_chunk::pack(targetId);
				data <<= STATUSCHUNK_TARGETTYPE_BITS;
				break;
			case STATUSCHUNK_TARGET_SPECIAL:
				data |= targetBlockOrTrainNumber;
				data <<= STATUSCHUNK_TRAINCAR_BITS;

				data |= targetVertexOrCarNumber;
				data <<= STATUSCHUNK_TARGETTYPE_BITS;
				break;
			case STATUSCHUNK_TARGET_LOCATION:
				// Must be a Location Target (or a Miss)...
				data |= targetCellRC[0];
				data <<= STATUSCHUNK_CELLPOS_BITS;

				data |= targetCellRC[1];
				data <<= STATUSCHUNK_TARGETTYPE_BITS;
				break;
		}
	}

	data |= targetType;
	data <<= STATUSCHUNK_EJECTORDER_BITS;

	if (ejectOrderGiven)
		data |= 1;
	data <<= STATUSCHUNK_JUMPORDER_BITS;

	if (jumpOrder)
		data |= 1;
	data <<= STATUSCHUNK_BODYSTATE_BITS;

	data |= bodyState;

#ifdef DEBUG_STATUSCHUNK
	switch (targetType) {
		case STATUSCHUNK_TARGET_NONE:
			break;
		case STATUSCHUNK_TARGET_MOVER: {
			if ((targetId < 0) || (targetId >= MAX_MULTIPLAYER_MOVERS)) {
				#ifdef ASSERT_STATUSCHUNK
					DebugStatusChunk(mover, this, nullptr);
					char errMsg[1024];
					(*std::format_to_n(errMsg, sizeof(errMsg) - 1, " StatusChunk.pack: bad targetId {:d} (save stchunk.dbg file) ", mc2::fmt::as_i32(targetId)).out = '\0');
					Assert(false, targetId, errMsg);
				#else
					StatusChunkUnpackErr = 1;
				#endif
			}
			GameObjectPtr target = static_cast<GameObjectPtr>(MPlayer->moverRoster[targetId]);
			}
			break;
		case STATUSCHUNK_TARGET_TERRAIN: {
			GameObjectPtr target = ObjectManager->findByPartId(targetId);
			if (!target) {
				#ifdef ASSERT_STATUSCHUNK
					DebugStatusChunk(mover, this, nullptr);
					char errMsg[1024];
					(*std::format_to_n(errMsg, sizeof(errMsg) - 1, " StatusChunk.pack: NULL Terrain Target (save stchunk.dbg file) ").out = '\0');
					Assert(false, targetId, errMsg);
				#else
					StatusChunkUnpackErr = 3;
				#endif
			}
			}
			break;
		case STATUSCHUNK_TARGET_SPECIAL: {
			GameObjectPtr target = ObjectManager->findByPartId(targetId);
			if (!target) {
				#ifdef ASSERT_STATUSCHUNK
					DebugStatusChunk(mover, this, nullptr);
					char errMsg[1024];
					(*std::format_to_n(errMsg, sizeof(errMsg) - 1, " StatusChunk.pack: NULL Special Target (save stchunk.dbg file) ").out = '\0');
					Assert(false, targetId, errMsg);
				#else
					StatusChunkUnpackErr = 4;
				#endif
			}
			}
			break;
		case STATUSCHUNK_TARGET_LOCATION:
			break;
		default:
			#ifdef ASSERT_STATUSCHUNK
				DebugStatusChunk(mover, this, nullptr);
				char errMsg[1024];
				(*std::format_to_n(errMsg, sizeof(errMsg) - 1, " StatusChunk.pack: bad targetType {:d} (save stchunk.dbg file) ", mc2::fmt::as_i32(targetType)).out = '\0');
				Assert(false, targetType, errMsg);
			#else
				StatusChunkUnpackErr = 5;
			#endif
	}
#endif

}

void StatusChunk::unpack (MoverPtr mover) {

	StatusChunkUnpackErr = 0;

	unsigned long tempData = data;

	bodyState = (tempData & STATUSCHUNK_BODYSTATE_MASK);
	tempData >>= STATUSCHUNK_BODYSTATE_BITS;

	jumpOrder = ((tempData & STATUSCHUNK_JUMPORDER_MASK) != 0);
	tempData >>= STATUSCHUNK_JUMPORDER_BITS;


	ejectOrderGiven = ((tempData & STATUSCHUNK_EJECTORDER_MASK) != 0);
	tempData >>= STATUSCHUNK_EJECTORDER_BITS;

	targetType = (tempData & STATUSCHUNK_TARGETTYPE_MASK);
	tempData >>= STATUSCHUNK_TARGETTYPE_BITS;

	if (jumpOrder) {
		targetCellRC[1] = (tempData & STATUSCHUNK_CELLPOS_MASK);
		tempData >>= STATUSCHUNK_CELLPOS_BITS;
						
		targetCellRC[0] = (tempData & STATUSCHUNK_CELLPOS_MASK);
		}
	else {
		switch (targetType) {
			case STATUSCHUNK_TARGET_NONE:
				break;
			case STATUSCHUNK_TARGET_MOVER:
				targetId = (tempData & STATUSCHUNK_MOVERINDEX_MASK);
				break;
			case STATUSCHUNK_TARGET_TERRAIN:
				targetId = terrain_part_chunk::unpack(tempData & STATUSCHUNK_TERRAINPARTID_MASK).value_or(-1);
				break;
			case STATUSCHUNK_TARGET_SPECIAL:
				targetVertexOrCarNumber = (tempData & STATUSCHUNK_TRAIN_MASK);
				tempData >>= STATUSCHUNK_TRAIN_BITS;

				targetBlockOrTrainNumber = (tempData & STATUSCHUNK_TRAINCAR_MASK);
				tempData >>= STATUSCHUNK_TRAINCAR_BITS;

				if (targetBlockOrTrainNumber == 128)
					targetId = MIN_CAMERA_DRONE_ID + targetVertexOrCarNumber;
				else
					targetId = MIN_TRAIN_PART_ID +
							   targetBlockOrTrainNumber * MAX_TRAIN_CARS +
							   targetVertexOrCarNumber;
				break;
			case STATUSCHUNK_TARGET_LOCATION:
				// Must be a Location Target (or a Miss)...
				targetCellRC[1] = (tempData & STATUSCHUNK_CELLPOS_MASK);
				tempData >>= STATUSCHUNK_CELLPOS_BITS;
						
				targetCellRC[0] = (tempData & STATUSCHUNK_CELLPOS_MASK);
				break;
		}
	}

#ifdef DEBUG_STATUSCHUNK
	switch (targetType) {
		case STATUSCHUNK_TARGET_NONE:
			break;
		case STATUSCHUNK_TARGET_MOVER: {
			if ((targetId < 0) || (targetId >= MAX_MULTIPLAYER_MOVERS)) {
				#ifdef ASSERT_STATUSCHUNK
					DebugStatusChunk(mover, this, nullptr);
					char errMsg[1024];
					(*std::format_to_n(errMsg, sizeof(errMsg) - 1, " StatusChunk.unpack: bad targetId {:d} (save stchunk.dbg file) ", mc2::fmt::as_i32(targetId)).out = '\0');
					Assert(false, targetId, errMsg);
				#else
					StatusChunkUnpackErr = 1;
				#endif
			}
			GameObjectPtr target = static_cast<GameObjectPtr>(MPlayer->moverRoster[targetId]);
			}
			break;
		case STATUSCHUNK_TARGET_TERRAIN: {
			GameObjectPtr target = ObjectManager->findByPartId(targetId);
			if (!target) {
				#ifdef ASSERT_STATUSCHUNK
					DebugStatusChunk(mover, this, nullptr);
					char errMsg[1024];
					(*std::format_to_n(errMsg, sizeof(errMsg) - 1, " StatusChunk.unpack: NULL Terrain Target (save stchunk.dbg file) ").out = '\0');
					Assert(false, targetId, errMsg);
				#else
					StatusChunkUnpackErr = 3;
				#endif
			}
			}
			break;
		case STATUSCHUNK_TARGET_SPECIAL: {
			GameObjectPtr target = ObjectManager->findByPartId(targetId);
			if (!target) {
				#ifdef ASSERT_STATUSCHUNK
					DebugStatusChunk(mover, this, nullptr);
					char errMsg[1024];
					(*std::format_to_n(errMsg, sizeof(errMsg) - 1, " StatusChunk.unpack: NULL Special Target (save stchunk.dbg file) ").out = '\0');
					Assert(false, targetId, errMsg);
				#else
					StatusChunkUnpackErr = 4;
				#endif
			}
			}
			break;
		case STATUSCHUNK_TARGET_LOCATION:
			break;
		default:
			#ifdef ASSERT_STATUSCHUNK
				DebugStatusChunk(mover, this, nullptr);
				char errMsg[1024];
				(*std::format_to_n(errMsg, sizeof(errMsg) - 1, " StatusChunk.unpack: bad targetType {:d} (save stchunk.dbg file) ", mc2::fmt::as_i32(targetType)).out = '\0');
				Assert(false, targetType, errMsg);
			#else
				StatusChunkUnpackErr = 5;
			#endif
	}
#endif

}

bool StatusChunk::equalTo (StatusChunkPtr chunk) {

	if (bodyState != chunk->bodyState) {
		DebugStatusChunk(nullptr, this, chunk);
		return(false);
	}

	if (ejectOrderGiven != chunk->ejectOrderGiven) {
		DebugStatusChunk(nullptr, this, chunk);
		return(false);
	}

	if (jumpOrder != chunk->jumpOrder) {
		DebugStatusChunk(nullptr, this, chunk);
		return(false);
	}

	if (targetType != chunk->targetType) {
		DebugStatusChunk(nullptr, this, chunk);
		return(false);
	}

	if (targetId != chunk->targetId) {
		DebugStatusChunk(nullptr, this, chunk);
		return(false);
	}

	if (targetCellRC[0] != chunk->targetCellRC[0]) {
		DebugStatusChunk(nullptr, this, chunk);
		return(false);
	}

	if (targetCellRC[1] != chunk->targetCellRC[1]) {
		DebugStatusChunk(nullptr, this, chunk);
		return(false);
	}

	return(true);
}


void MoverDynamics::init (void) {

	type = DYNAMICS_BASE;
}

void MoverDynamics::init (DynamicsType newType) {

	type = newType;
	switch (newType) {
		case DYNAMICS_MECH:
			max.mech.torsoYawRate = 0;
			max.mech.torsoYaw = 0;
			break;
		case DYNAMICS_GROUNDVEHICLE:
			max.groundVehicle.yawRate = 0;
			break;
		case DYNAMICS_ELEMENTAL:
			max.elemental.yawRate = 0;
			break;
		default:
			Fatal(newType, " ugh ");
	}
}

void MoverDynamics::init (CSVFilePtr dynamicsFile) {

	if (type == DYNAMICS_MECH) 
	{
		long tmp, tmp2;
		dynamicsFile->readLong(8, 5, tmp);
		dynamicsFile->readLong(9, 5, tmp2);
        max.mech.torsoYawRate = tmp;
        max.mech.torsoYaw = tmp2;
	}
	else if (type == DYNAMICS_GROUNDVEHICLE) {
	/*
		long result = 0;
		result = dynamicsFile->seekBlock("VehicleDynamics");
		Assert(result == NO_ERR, result, " MoverDynamics.init: vehicle error 0 ");

		result = dynamicsFile->readIdLong("maxTurretYawRate",max.groundVehicle.turretYawRate);
		Assert(result == NO_ERR, result, " MoverDynamics.init: vehicle error 1 ");
	
		result = dynamicsFile->readIdLong("maxTurretYaw",max.groundVehicle.turretYaw);
		Assert(result == NO_ERR, result, " MoverDynamics.init: vehicle error 2 ");
	
		result = dynamicsFile->readIdLong("maxVehicleYawRate",max.groundVehicle.yawRate);
		Assert(result == NO_ERR, result, " MoverDynamics.init: vehicle error 3 ");
		if (max.groundVehicle.yawRate < 720)
			max.groundVehicle.yawRate = 720;

		result = dynamicsFile->readIdLong("maxVehiclePivotRate",max.groundVehicle.pivotRate);
		if (result != NO_ERR)
			max.groundVehicle.pivotRate = (long)((float)max.groundVehicle.yawRate * GroundVehiclePivotYawMultiplier);

		result = dynamicsFile->readIdFloat("maxAccel",max.groundVehicle.accel);
		Assert(result == NO_ERR, result, " MoverDynamics.init: vehicle error 4 ");
			
		result = dynamicsFile->readIdFloat("maxVelocity",max.groundVehicle.speed);
		Assert(result == NO_ERR, result, " MoverDynamics.init: vehicle error 5 ");
		// Frank Hack
		max.groundVehicle.accel = max.groundVehicle.speed * 5.0;
	*/
		}
	else if (type == DYNAMICS_ELEMENTAL) {
		Fatal(type, " MoverDynamics.init: bad elemental ");
		}
	else
		Fatal(type, " MoverDynamics.init: bad type ");
}

void MoverDynamics::init (FitIniFilePtr dynamicsFile) 
{
	if (type == DYNAMICS_MECH) 
	{
	}
	else if (type == DYNAMICS_GROUNDVEHICLE) {
		long result = 0;
		result = dynamicsFile->seekBlock("VehicleDynamics");
		Assert(result == NO_ERR, result, " MoverDynamics.init: vehicle error 0 ");

        long tmp;
		result = dynamicsFile->readIdLong("maxTurretYawRate", tmp);
        max.groundVehicle.turretYawRate = tmp;
		Assert(result == NO_ERR, result, " MoverDynamics.init: vehicle error 1 ");
	
		result = dynamicsFile->readIdLong("maxTurretYaw", tmp);
        max.groundVehicle.turretYaw = tmp;
		Assert(result == NO_ERR, result, " MoverDynamics.init: vehicle error 2 ");
	
		result = dynamicsFile->readIdLong("maxVehicleYawRate", tmp);
        max.groundVehicle.yawRate = tmp;
		Assert(result == NO_ERR, result, " MoverDynamics.init: vehicle error 3 ");
		max.groundVehicle.yawRate = std::max(max.groundVehicle.yawRate, 720);

		result = dynamicsFile->readIdLong("maxVehiclePivotRate", tmp);
        max.groundVehicle.pivotRate = tmp;
		if (result != NO_ERR)
			max.groundVehicle.pivotRate = static_cast<long>((static_cast<float>(max.groundVehicle.yawRate) * GroundVehiclePivotYawMultiplier));

		result = dynamicsFile->readIdFloat("maxAccel",max.groundVehicle.accel);
		Assert(result == NO_ERR, result, " MoverDynamics.init: vehicle error 4 ");
			
		result = dynamicsFile->readIdFloat("maxVelocity",max.groundVehicle.speed);
		Assert(result == NO_ERR, result, " MoverDynamics.init: vehicle error 5 ");
		max.groundVehicle.accel = max.groundVehicle.speed * 5.0;
		}
	else if (type == DYNAMICS_ELEMENTAL) {
		Fatal(type, " MoverDynamics.init: bad elemental ");
		}
	else
		Fatal(type, " MoverDynamics.init: bad type ");
}


void MoverControl::reset (void) {

    //PAUSE(("sebi: see where it gets called from to see why throttle should not be reset\n"));

	switch (dataType) {
		case CONTROL_DATA_BASE:
			break;
		case CONTROL_DATA_MECH:
            // sebi: thi s icommented, but causes valgrind complain because it is not initialized...
            // for now just initialize it in init() but need to check why it is commented and do something about it
			//settings.mech.throttle = 100;
			settings.mech.rotate = 0.0;
			settings.mech.facingRotate = 0.0f;
			settings.mech.rotateTorso = 0;
			settings.mech.rotateLeftArm = 0;
			settings.mech.rotateRightArm = 0;
			settings.mech.gestureGoal = -1;
			settings.mech.blowLeftArm = false;
			settings.mech.blowRightArm = false;
			settings.mech.pivot = false;
			break;
		case CONTROL_DATA_GROUNDVEHICLE:
			settings.groundVehicle.rotate = 0.0;
			settings.groundVehicle.rotateTurret = 0.0;
			settings.groundVehicle.gestureGoal = -1;
			settings.groundVehicle.pivot = false;
			settings.groundVehicle.isWalking = false;
			settings.groundVehicle.throttle = 0.0;
			break;
		case CONTROL_DATA_ELEMENTAL:
			break;
	}
}

void MoverControl::brake (void) {

	switch (dataType) {
		case CONTROL_DATA_BASE:
			break;
		case CONTROL_DATA_MECH:
			settings.mech.throttle = 100;
			settings.mech.gestureGoal = MECH_STATE_STANDING;
			break;
		case CONTROL_DATA_GROUNDVEHICLE:
			settings.mech.throttle = 0;
			break;
		case CONTROL_DATA_ELEMENTAL:
			break;
	}

}

void MoverControl::update (MoverPtr mover) {

	ZoneScopedN("GameLogic.Mover.Update");

	switch (type) {
		case CONTROL_AI:
			mover->updateAIControl();
			break;
		case CONTROL_NET:
			mover->updateNetworkControl();
			break;
		case CONTROL_PLAYER:
			mover->updatePlayerControl();
			break;
		default:
			Fatal(type, " MoverControl.update: bad control type ");
	}
}

long MoverControl::init (FitIniFilePtr controlFile) {

	return(NO_ERR);
}
