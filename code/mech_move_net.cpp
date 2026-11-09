// BattleMech: network movement updates, crash avoidance, AI/network/player control and dynamics.
// Declared in the class headers; shared names in mech_internal.h.
#include "mech_internal.h"



bool BattleMech::netUpdateMovePath (float& newRotate, char& newThrottleSetting, long& newGestureStateGoal, long& newMoveState, long& minThrottle, long& maxThrottle) {

	DistanceToWaypoint = 9999.0;

	MovePathPtr path = pilot->getMovePath();

	bool allowedToRun = canRun() && moveChunk.run;
	long curThrottleSetting = control.settings.mech.throttle;
	newThrottleSetting = curThrottleSetting;
	newRotate = 0.0;

	bool goalReached = false;

	if ((legStatus == LEG_STATUS_NORMAL) || (legStatus == LEG_STATUS_HURTING) || (legStatus == LEG_STATUS_IMPAIRED_LEFT) || (legStatus == LEG_STATUS_IMPAIRED_RIGHT)) {
		if (path->numSteps > 0) {
			if (path->curStep == path->numSteps)
				goalReached = true;
			else {
				glm::vec3 wayPt = path->stepList[path->curStep].destination;
				
				// Let's record our last valid position, in case we need to
				// crawl back from impassable terrain we get knocked onto.
				// Ultimately, this SHOULD NOT be necessary once we agree
				// on a new gesture/movement system... gd 6/2/97
				lastValidPosition = wayPt;

				float distanceFromWayPt = distanceFrom(wayPt);
				DistanceToWaypoint = distanceFromWayPt;

				float cushion = Mover::marginOfError[0];
				if (path->curStep == (path->numSteps - 1))
					cushion = Mover::marginOfError[1];
				if (distanceFromWayPt < cushion) {
					path->curStep++;
					pilot->setMoveTimeOfLastStep(scenarioTime);
					if (path->curStep < path->numSteps) {
						long curDir = path->stepList[path->curStep].direction;
						if (!MPlayer && (curDir > 7)) {
							newGestureStateGoal = 6;
							return(false);
							}
						else
							wayPt = path->stepList[path->curStep].destination;
						}
					else
						goalReached = true;
					}
				else {
					long curDir = path->stepList[path->curStep].direction;
					if (!MPlayer && (curDir > 7)) {
						newGestureStateGoal = 6;
						return(false);
					}
				}

				if (!goalReached) {
					float relFacingToWayPt = relFacingTo(wayPt, MECH_BODY_LOCATION_LLEG);

					long moveState = pilot->getMoveState();
					long moveStateGoal = pilot->getMoveStateGoal();

					if (moveState == MOVESTATE_FORWARD) {
						if (moveStateGoal == MOVESTATE_FORWARD) {
							if (legStatus == LEG_STATUS_IMPAIRED_LEFT) {
								newGestureStateGoal = MECH_STATE_LIMPING_LEFT;
								newThrottleSetting = 100;
								}
							else if (legStatus == LEG_STATUS_IMPAIRED_RIGHT) {
								newGestureStateGoal = MECH_STATE_LIMPING_RIGHT;
								newThrottleSetting = 100;
								}
							else if (allowedToRun) {
								newThrottleSetting = 100;
								newGestureStateGoal = MECH_STATE_RUNNING;
								}
							else
								newGestureStateGoal = MECH_STATE_WALKING;
							
							if ((relFacingToWayPt < -5.0) || (relFacingToWayPt > 5.0)) {
								// We can and will shift facing to destination...
								// Don't know that we need to force walk to turn!!
								// -fs
								newRotate = -relFacingToWayPt;
								float maxRate = tonnageTurnRate[static_cast<int32_t>(tonnage)] * frameLength;
								if (fabs(newRotate) > maxRate) {
									if (newRotate > 0.0)
										newRotate = maxRate;
									else
										newRotate = -maxRate;
									}
								else {
									if (newGestureStateGoal == MECH_STATE_WALKING) {
										newThrottleSetting = pilot->calcMoveSpeedThrottle(getBodyState(), control.settings.mech.throttle);
									}
								}
							}

							}
						else if (moveStateGoal == MOVESTATE_REVERSE) {
							pilot->pausePath(); //pilot->getMovePath()->numSteps = 0;
							newMoveState = MOVESTATE_PIVOT_REVERSE;
							}
						else if (moveStateGoal == MOVESTATE_PIVOT_FORWARD) {
							pilot->pausePath(); //pilot->getMovePath()->numSteps = 0;
							newMoveState = MOVESTATE_PIVOT_FORWARD;
							}
						else if (moveStateGoal == MOVESTATE_PIVOT_REVERSE) {
							pilot->pausePath(); //pilot->getMovePath()->numSteps = 0;
							newMoveState = MOVESTATE_PIVOT_REVERSE;
							}
						else {
							pilot->pausePath(); //pilot->getMovePath()->numSteps = 0;
							newMoveState = MOVESTATE_FORWARD /*MOVESTATE_STAND*/;
						}
						}
					else if (moveState == MOVESTATE_REVERSE) {
						if (moveStateGoal == MOVESTATE_FORWARD) {
							pilot->pausePath(); //pilot->getMovePath()->numSteps = 0;
							newMoveState = MOVESTATE_PIVOT_FORWARD;
							}
						else if (moveStateGoal == MOVESTATE_REVERSE) {
							newGestureStateGoal = MECH_STATE_REVERSE;
							/*
							if ((relFacingToWayPt < -5.0) || (relFacingToWayPt > 5.0)) 
							{
								// We can and will shift facing to destination...
								float maxRate = dyn->maxMechYawRate;
								if (relFacingToWayPt < 0.0)
									newTurnRate = maxRate*frameLength;
								else
									newTurnRate = -maxRate*frameLength;
								
								relFacingToWayPt = -(relFacingToWayPt + 180.0);
								if (relFacingToWayPt > 0.0)
									relFacingToWayPt = -(relFacingToWayPt - 180.0);
								
								if ((newTurnRate > 0.0) && (newTurnRate > relFacingToWayPt))
									newTurnRate = relFacingToWayPt;
								else if ((newTurnRate > 0.0) && (newTurnRate < relFacingToWayPt))
									newTurnRate = relFacingToWayPt;
								else
								{
									if (newGestureStateGoal == MECH_STATE_WALKING) {
										// Try to attain current goal speed...
										newThrottleSetting = pilot->calcMoveSpeedThrottle(getBodyState(), data->throttleSetting);
									}
								}
									
								newMechYaw = newTurnRate;
							}
							*/
							
							if (relFacingToWayPt < 0)
								newRotate = -(relFacingToWayPt + 180.0);
							else
								newRotate = -(relFacingToWayPt - 180.0);
							float maxRate = tonnageTurnRate[static_cast<int32_t>(tonnage)] * frameLength;
							if (fabs(newRotate) > maxRate) {
								if (newRotate > 0.0)
									newRotate = maxRate;
								else
									newRotate = -maxRate;
								newThrottleSetting = control.settings.mech.throttle - 10;
								}
							else {
								newThrottleSetting = pilot->calcMoveSpeedThrottle(getBodyState(), control.settings.mech.throttle);
							}

							}
						else if (moveStateGoal == MOVESTATE_PIVOT_FORWARD) {
							pilot->pausePath(); //pilot->getMovePath()->numSteps = 0;
							newMoveState = MOVESTATE_PIVOT_FORWARD;
							}
						else if (moveStateGoal == MOVESTATE_PIVOT_REVERSE) {
							pilot->pausePath(); //pilot->getMovePath()->numSteps = 0;
							newMoveState = MOVESTATE_PIVOT_REVERSE;
							}
						else {
							pilot->pausePath(); //pilot->getMovePath()->numSteps = 0;
							newMoveState = MOVESTATE_FORWARD /*MOVESTATE_STAND*/;
						}
						}
					else {
						if ((moveStateGoal == MOVESTATE_FORWARD) || (moveStateGoal == MOVESTATE_PIVOT_FORWARD)) {
							pilot->pausePath(); //pilot->getMovePath()->numSteps = 0;
							newMoveState = MOVESTATE_PIVOT_FORWARD;
							}
						else if ((moveStateGoal == MOVESTATE_REVERSE) || (moveStateGoal == MOVESTATE_PIVOT_REVERSE)) {
							pilot->pausePath(); //pilot->getMovePath()->numSteps = 0;
							newMoveState = MOVESTATE_PIVOT_REVERSE;
						}
					}
				}
			}
			}
		else {
			newGestureStateGoal = MECH_STATE_STANDING;
		}
		}
	else {
		newGestureStateGoal = MECH_STATE_STANDING;
	}

	return(goalReached);
}

void BattleMech::netUpdateMovement (void) {

	long minThrottle = 35;
	long maxThrottle = 100;

	long bodyState = getBodyState();

	MovePathPtr path = pilot->getMovePath();
	bool atEndOfPath = (path->curStep == (path->numSteps - 1)); //was >=

	glm::vec3 wayPt;
	float distanceFromWayPt = 1000000.0;
	if ((path->curStep > -1) && (path->curStep < path->numSteps)) {
		wayPt = path->stepList[path->curStep].destination;
		distanceFromWayPt = distanceFrom(wayPt);
	}
	if (moveChunk.moving && /*(path->numStepsWhenNotPaused > 0) &&*/ (bodyState == MECH_STATUSCHUNK_BODYSTATE_PARKED))
		startUpThisFrame = true;

	if (atEndOfPath && (distanceFromWayPt < Mover::marginOfError[1])) {
		startUpThisFrame = false;
		switch (statusChunk.bodyState) {
			case MECH_STATUSCHUNK_BODYSTATE_FALLEN_FORWARD:
				if (!isDisabled()) {
					setStatus(OBJECT_STATUS_NORMAL);
					sensorSystem->setShutdown(false);
				}
				if (bodyState != MECH_STATUSCHUNK_BODYSTATE_FALLEN_FORWARD) {
					pilot->clearMoveOrders();
					appearance->setGestureGoal(MECH_STATE_FALLEN_FORWARD);
					control.settings.mech.throttle = maxThrottle;
					return;
				}
				break;
			case MECH_STATUSCHUNK_BODYSTATE_FALLEN_BACKWARD:
				if (!isDisabled()) {
					setStatus(OBJECT_STATUS_NORMAL);
					sensorSystem->setShutdown(false);
				}
				if (bodyState != MECH_STATUSCHUNK_BODYSTATE_FALLEN_BACKWARD) {
					pilot->clearMoveOrders();
					appearance->setGestureGoal(MECH_STATE_FALLEN_BACKWARD);
					control.settings.mech.throttle = maxThrottle;
					return;
				}
				break;
			case MECH_STATUSCHUNK_BODYSTATE_PARKED:
				if (!isDisabled()) {
					setStatus(OBJECT_STATUS_SHUTDOWN);
					sensorSystem->setShutdown(true);
				}
				if (bodyState != MECH_STATUSCHUNK_BODYSTATE_PARKED) {
					soundSystem->playDigitalSample(POWERDOWN_SFX,getPosition());
					pilot->clearMoveOrders();
					appearance->setGestureGoal(MECH_STATE_PARKED);
					control.settings.mech.throttle = maxThrottle;
					return;
				}
				break;
			case MECH_STATUSCHUNK_BODYSTATE_STANDING:
				if (!isDisabled()) {
					setStatus(OBJECT_STATUS_NORMAL);
					sensorSystem->setShutdown(false);
				}
				if (bodyState != MECH_STATUSCHUNK_BODYSTATE_STANDING) {
				
					if (bodyState == MECH_STATUSCHUNK_BODYSTATE_PARKED)
						soundSystem->playDigitalSample(POWERUP_SFX,getPosition());
						
					pilot->clearMoveOrders();
					appearance->setGestureGoal(MECH_STATE_STANDING);
					control.settings.mech.throttle = maxThrottle;
					return;
				}
				break;
		}
	}

	if (disableThisFrame) {
		long fallGesture = RandomNumber(2) ? MECH_STATE_FALLEN_FORWARD : MECH_STATE_FALLEN_BACKWARD;
		if (hitFromBehindThisFrame)
			fallGesture = MECH_STATE_FALLEN_FORWARD;
		else if (hitFromFrontThisFrame)
			fallGesture = MECH_STATE_FALLEN_BACKWARD;
		appearance->setGestureGoal(fallGesture);
		disableThisFrame = false;
		shutDownThisFrame = false;
		startUpThisFrame = false;
		hitFromFrontThisFrame = false;
		hitFromBehindThisFrame = false;
		control.settings.mech.throttle = maxThrottle;
		return;
	}

	if (shutDownThisFrame) {
		appearance->setGestureGoal(MECH_STATE_PARKED);
		shutDownThisFrame = false;
		startUpThisFrame = false;
		setStatus(OBJECT_STATUS_SHUTDOWN);
		control.settings.mech.throttle = maxThrottle;
		return;
	}

	if (startUpThisFrame) {
		appearance->setGestureGoal(MECH_STATE_STANDING);
		startUpThisFrame = false;
		shutDownThisFrame = false;
		setStatus(OBJECT_STATUS_NORMAL);
		control.settings.mech.throttle = maxThrottle;
		return;
	}

	if ((status == OBJECT_STATUS_SHUTTING_DOWN) || 
		(status == OBJECT_STATUS_SHUTDOWN) || 
		(status == OBJECT_STATUS_DISABLED))
		return;

	if (isCaptured())
		return;

	if (engineBlowTime > -1.0)
		return;

	if (updateJump())
		return;

	if (pivotTo())
		return;

	float newRotate = 0.0;
	long newGestureStateGoal = -1;
	long newMoveState = -1;
	char newThrottleSetting = -1;
	bool goalReached = false;

	goalReached = netUpdateMovePath(newRotate, newThrottleSetting, newGestureStateGoal, newMoveState, minThrottle, maxThrottle);

	if (newMoveState != -1)
		pilot->setMoveState(newMoveState);

	setControlSettings(newRotate, newThrottleSetting, newGestureStateGoal, minThrottle, maxThrottle,newRotate);
		
	updateMoveStateGoal();

	NewRotation = newRotate;
}
		
bool BattleMech::crashAvoidanceSystem (void) {

	if (MPlayer && !MPlayer->isServer())
		return(false);

	MovePathPtr path = pilot->getMovePath();

	if (path->numStepsWhenNotPaused == 0)
		return(false);

	if (pilot->getMoveWaitForPointTime() > 999990.0)
		return(false);

	glm::vec3 velocity = getVelocity();
	velocity.z = 0.0;

	glm::vec3 relVelocity = velocity;
	relVelocity *= frameLength;
	relVelocity *= worldUnitsPerMeter;

	glm::vec3 newPosition;
	newPosition = position + relVelocity;

	int cellR, cellC;
	land->worldToCell(newPosition, cellR, cellC);
	bool reachedEnd;
	bool blockReachedEnd;
	// To avoid corner stops...
	bool clippingCorner = false;
	int dir = path->getDirection(path->curStep);
	if ((dir == 1) || (dir == 3) || (dir == 5) || (dir == 7)) {
		bool firstCornerClipped = getAdjacentCellPathLocked((moveLevel == 2), cellPositionRow, cellPositionCol, adjClippedCell[dir][0]);
		bool secondCornerClipped = getAdjacentCellPathLocked((moveLevel == 2), cellPositionRow, cellPositionCol, adjClippedCell[dir][1]);
		clippingCorner = (firstCornerClipped && secondCornerClipped);
	}

	bool nearingMover = getPathRangeLock(crashAvoidPath, &reachedEnd);
	bool pathBlocked = getPathRangeBlocked(crashAvoidPath, &blockReachedEnd);
	bool pathLocked = /*stepOnMover ||*/ nearingMover;
	long stepIntoGate = (path->crossesClosedGate(-1, 2) > 0);

	if (pilot->isYielding()) {
		if (pathLocked || pathBlocked || clippingCorner || stepIntoGate) {
			pilot->pausePath(); //path->numSteps = 0; //in theory, this should already be zero, yet is sometimes not. WHY?!
			return(true);
			}
		else {
			pilot->resumePath();
			pilot->setMoveYieldTime(-1.0);
		}
		}
	else {

		if (pathLocked || pathBlocked || clippingCorner || stepIntoGate) {
			if (reachedEnd || blockReachedEnd) {
				/*
				pilot->setMoveWayPath(ORDER_CURRENT, NULL, 0);
				for (long i = 0; i < 2; i++)
					pilot->clearMovePath(ORDER_CURRENT, i);
				pilot->setMoveGlobalPath(ORDER_CURRENT, NULL, 0);
				*/
				pilot->rethinkPath(0);
				control.brake();
				}
			else {
				pilot->pausePath();
				pilot->setMoveYieldTime(scenarioTime + crashYieldTime);
				control.brake();
			}
			return(true);
		}
	}
	
	return(false);
}

void BattleMech::updateAIControl (void) {

	control.reset();

	if (getAwake())	{

		updateDamageTakenRate();

		if (!isDisabled() && pilot->alive() && !pilot->hasEjected()) {
			if (getTeamId() > -1)
				pilot->mainDecisionTree();
			updateMovement();
			}
		else if (shutDownThisFrame || disableThisFrame)
			updateMovement();
	}
}

void BattleMech::updateNetworkControl (void) {

	control.reset();

	if (getAwake())	{

		updateWeaponFireChunks(CHUNK_RECEIVE);

		updateCriticalHitChunks(CHUNK_RECEIVE);

		updateRadioChunks(CHUNK_RECEIVE);

		if (!isDisabled() && pilot->alive() && !pilot->hasEjected()) {
			pilot->checkAlarms();
			netUpdateMovement();
			}
		else if (shutDownThisFrame || disableThisFrame) {
			netUpdateMovement();
		}
	}
}

void BattleMech::updatePlayerControl (void) {

	control.reset();
		
	if (userInput->getKeyDown(KEY_T))
		control.settings.mech.rotate = tonnageTurnRate[static_cast<int32_t>(tonnage)] / 4.0 * frameLength;

	if (userInput->getKeyDown(KEY_Y))
		control.settings.mech.rotate = -tonnageTurnRate[static_cast<int32_t>(tonnage)] / 4.0 * frameLength;

	if (userInput->getKeyDown(KEY_DIVIDE))
		control.settings.mech.rotateTorso = dynamics.max.mech.torsoYawRate / 4.0 * frameLength;

	if (userInput->getKeyDown(KEY_MULTIPLY))
		control.settings.mech.rotateTorso = -dynamics.max.mech.torsoYawRate / 4.0 * frameLength;

	if (userInput->getKeyDown(KEY_NEXT)) 
	{
		appearance->setNextFrame();
	}
		
	if (userInput->getKeyDown(KEY_PRIOR)) 
	{
		appearance->setPrevFrame();
	}

	if (userInput->getKeyDown(KEY_END))		
	{
		appearance->setSingleStepMode();
	}

	if (userInput->getKeyDown(KEY_U))
		control.settings.mech.rotateLeftArm = tonnageTurnRate[static_cast<int32_t>(tonnage)] / 4.0 * frameLength;

	if (userInput->getKeyDown(KEY_I))
		control.settings.mech.rotateLeftArm = -tonnageTurnRate[static_cast<int32_t>(tonnage)] / 4.0 * frameLength;

	if (userInput->getKeyDown(KEY_O))
		control.settings.mech.rotateRightArm = tonnageTurnRate[static_cast<int32_t>(tonnage)] / 4.0 * frameLength;

	if (userInput->getKeyDown(KEY_P))
		control.settings.mech.rotateRightArm = -tonnageTurnRate[static_cast<int32_t>(tonnage)] / 4.0 * frameLength;

	if (userInput->getKeyDown(KEY_1))
		appearance->setGestureGoal(0);

	if (userInput->getKeyDown(KEY_2))
		appearance->setGestureGoal(1);		//Stand

	if (userInput->getKeyDown(KEY_3))
		appearance->setGestureGoal(2);		//Walk

	if (userInput->getKeyDown(KEY_4))
		appearance->setGestureGoal(3);		//Run

	if (userInput->getKeyDown(KEY_R))
		appearance->setGestureGoal(4);		//Reverse

	if (userInput->getKeyDown(KEY_5))	
		appearance->setGestureGoal(5);		//Limp LEFT!
			
	if (userInput->getKeyDown(KEY_9))	
		appearance->setGestureGoal(9);		//Limp RIGHT!

	if (userInput->getKeyDown(KEY_6))
		appearance->setGestureGoal(7);		//Fall Forward 
		
	if (userInput->getKeyDown(KEY_7))
		appearance->setGestureGoal(8);		//Fall Backward 

	if (userInput->getKeyDown(KEY_8))
		appearance->hitFront();
			
	if (userInput->getKeyDown(KEY_B))
		appearance->hitBack();
		
	if (userInput->getKeyDown(KEY_N))
		appearance->hitRight();
 
 	if (userInput->getKeyDown(KEY_M))
		appearance->hitLeft();
		
 	if (userInput->getKeyDown(KEY_Z))
	{
		causeOfDeath = SELF_DESTRUCT_DEATH;
		ObjectManager->getObjectType(typeHandle)->handleDestruction(this, nullptr);		//Blow self up!!
	}

	if (userInput->getKeyDown(KEY_X))
		control.settings.mech.blowLeftArm = true;

	if (userInput->getKeyDown(KEY_J)) 
	{
		glm::vec3 jumpDest = getRotationVector();
		jumpDest *= 150.0;
		jumpDest += position;
		appearance->setJumpParameters(jumpDest);
		appearance->setGestureGoal(6);		//Jump
	}
		
	if (userInput->getKeyDown(KEY_G))
		appearance->setCombatMode(1);

	if (userInput->getKeyDown(KEY_F))
		appearance->setCombatMode(0);

	if (userInput->getKeyDown(KEY_C))
		control.settings.mech.blowRightArm = true;
}

void BattleMech::updateDynamics (void) {

	float yawRate = control.settings.mech.rotate;
	float facingYawRate = control.settings.mech.facingRotate;
	float torsoYawRate = control.settings.mech.rotateTorso;
	float rightArmYawRate = control.settings.mech.rotateRightArm;
	float leftArmYawRate = control.settings.mech.rotateLeftArm;

	// The gesture is how we control speed.  So, at this point, check
	// the mechControlData->gestureGoal and see if we need to change goals
	bool bottomNotAllowedToYaw = false;
	bool topNotAllowedToYaw = false;
	
	if (appearance) 
	{
		if ((control.settings.mech.gestureGoal != -1) ||
			((control.settings.mech.gestureGoal == 7) && (getMoveType() != MOVETYPE_AIR)) ||
			((control.settings.mech.gestureGoal == 8) && (getMoveType() != MOVETYPE_AIR)))
			appearance->setGestureGoal(control.settings.mech.gestureGoal);
	}
	
	// Rotate torso by torso yaw rate, making sure we don't turn too far.
	if ((torsoYawRate != 0.0) && !topNotAllowedToYaw) {
		float newTorsoRotation = torsoRotation;

		if (newTorsoRotation > dynamics.max.mech.torsoYaw) {
			newTorsoRotation = dynamics.max.mech.torsoYaw;
			torsoYawRate = 0.0;
		}

		if (newTorsoRotation < -dynamics.max.mech.torsoYaw) {
			newTorsoRotation = -dynamics.max.mech.torsoYaw;
			torsoYawRate = 0.0;
		}

		if ((newTorsoRotation + torsoYawRate) > dynamics.max.mech.torsoYaw)
			torsoYawRate = dynamics.max.mech.torsoYaw - newTorsoRotation;

		if ((newTorsoRotation + torsoYawRate) < -dynamics.max.mech.torsoYaw)
			torsoYawRate = -dynamics.max.mech.torsoYaw - newTorsoRotation;

		newTorsoRotation += torsoYawRate;
		torsoRotation = newTorsoRotation;
	}

	// Rotate rightArm by rightArm yaw rate, making sure we don't turn too far.
	if ((rightArmYawRate != 0.0) && !topNotAllowedToYaw) {
		float newRightArmRotation = rightArmRotation;

		if (newRightArmRotation > tonnageTurnRate[static_cast<int32_t>(tonnage)]) {
			newRightArmRotation = tonnageTurnRate[static_cast<int32_t>(tonnage)];
			rightArmYawRate = 0.0;
		}
	
		if (newRightArmRotation < -tonnageTurnRate[static_cast<int32_t>(tonnage)]) {
			newRightArmRotation = -tonnageTurnRate[static_cast<int32_t>(tonnage)];
			rightArmYawRate = 0.0;
		}
	
		if ((newRightArmRotation + rightArmYawRate) > tonnageTurnRate[static_cast<int32_t>(tonnage)]) {
			newRightArmRotation = tonnageTurnRate[static_cast<int32_t>(tonnage)];
			rightArmYawRate = 0.0;
		}
	
		if ((newRightArmRotation + rightArmYawRate) < -tonnageTurnRate[static_cast<int32_t>(tonnage)]) {
			newRightArmRotation = -tonnageTurnRate[static_cast<int32_t>(tonnage)];
			rightArmYawRate = 0.0;
		}
	
		newRightArmRotation += rightArmYawRate;
		rightArmRotation = newRightArmRotation;
	}

	// Rotate rightArm by rightArm yaw rate, making sure we don't turn too far.
	if ((leftArmYawRate != 0.0)  && !topNotAllowedToYaw) {
		float newLeftArmRotation = leftArmRotation;

		if (newLeftArmRotation < -tonnageTurnRate[static_cast<int32_t>(tonnage)]) {
			newLeftArmRotation = -tonnageTurnRate[static_cast<int32_t>(tonnage)];
			leftArmYawRate = 0.0;
		}
	
		if (newLeftArmRotation > tonnageTurnRate[static_cast<int32_t>(tonnage)]) {
			newLeftArmRotation = tonnageTurnRate[static_cast<int32_t>(tonnage)];
			leftArmYawRate = 0.0;
		}
	
		if ((newLeftArmRotation + leftArmYawRate) < -tonnageTurnRate[static_cast<int32_t>(tonnage)]) {
			newLeftArmRotation = -tonnageTurnRate[static_cast<int32_t>(tonnage)];
			leftArmYawRate = 0.0;
		}
	
		if ((newLeftArmRotation + leftArmYawRate) > tonnageTurnRate[static_cast<int32_t>(tonnage)]) {
			newLeftArmRotation = tonnageTurnRate[static_cast<int32_t>(tonnage)];
			leftArmYawRate = 0.0;
		}
	
		newLeftArmRotation += leftArmYawRate;
		leftArmRotation = newLeftArmRotation;
	}

	if (!bottomNotAllowedToYaw)
		rotate(yawRate,facingYawRate);
}

extern GameObjectPtr DebugGameObject[3];
