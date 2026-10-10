// BattleMech: jump and ground movement: updateJump, pivotTo, move goals/paths, updateMovement, torso, jump FX positions.
// Declared in the class headers; shared names in mech_internal.h.
#include "mech_internal.h"



bool BattleMech::updateJump (void) {

	// We can turn after lifting off in a jump.
	if (!isJumping())
		return(false);

	if (appearance->isJumpSetup()) 
	{
		if (MPlayer && !MPlayer->isServer()) 
		{
			// Handled elsewhere, for client...
			if (distanceFrom(jumpGoal) > 8.0) 
			{
				appearance->setJumpParameters(jumpGoal);
				appearance->setGestureGoal(6);
				control.settings.mech.throttle = 100;
			}
		}
		else 
		{
			appearance->setGestureGoal(6);
			control.settings.mech.throttle = 100;
			return(true);
		}
	}
	else if (appearance->isJumpAirborne()) 
	{
		if (!playedJumpSFX)
		{
			playedJumpSFX = true;
			soundSystem->playDigitalSample(JUMPJETS,getPosition(),true);
		}
		
		/*
		float turnRate = 0.0;
		float relFacing = relFacingTo(jumpGoal);
		if ((relFacing < -2.0) || (relFacing > 2.0)) 
		{
			// We can and will shift facing to destination...
			turnRate = -relFacing * frameLength;
			float maxRate = tonnageTurnRate[long(tonnage)] * frameLength;
			if (turnRate < 0.0f)
				maxRate = -maxRate;

			if (turnRate > 0.0f)
			{
				if (maxRate > turnRate)
					maxRate = turnRate;
			}
			else if (turnRate < 0.0f)
			{
				if (maxRate < turnRate)
					maxRate = turnRate;
			}
						
		}
		*/
		
		// Do not rotate mech anymore in the jumps!
		// Mech's velocity take it right to point.
		control.settings.mech.rotate = 0.0f;
		return(true);
	}
	else if (!appearance->isInJump()) 
	{
		inJump = false;
		playedJumpSFX = false;
		lastJumpTime = scenarioTime;
		MovePathPtr path = pilot->getMovePath();
		pilot->resumePath();
		path->curStep++;

		// Let's record our last valid position, in case we need to
		// crawl back from impassable terrain we get knocked onto.
		// Ultimately, this SHOULD NOT be necessary once we agree
		// on a new gesture/movement system... gd 6/2/97
		lastValidPosition = position;

		pilotingCheck(PILOTCHECK_SITUATION_JUMPING);
		
		pilot->getCurTacOrder()->setStage(3);	//We're done.
		return(true);
	}

	return(true);
}

bool BattleMech::pivotTo (void) {

	MovePathPtr path = pilot->getMovePath();
	long moveState = pilot->getMoveState();
	long moveStateGoal = pilot->getMoveStateGoal();
	bool isRunning = false;
	if (MPlayer && !MPlayer->isServer())
		isRunning = moveChunk.run;
	else
		isRunning = (pilot->getMovePath()->numStepsWhenNotPaused > 0) && pilot->getMoveRun();

	bool hasTarget = false;
	Stuff::Vector3D targetPosition;
	GameObjectPtr target = pilot->getCurrentTarget();
	float relFacingToTarget = 0.0;
	if (target) 
	{
		targetPosition = target->getPosition();
		relFacingToTarget = relFacingTo(targetPosition);
		hasTarget = true;
	}
	else if (pilot->getCurTacOrder()->code == TACTICAL_ORDER_ATTACK_POINT) 
	{
		targetPosition = pilot->getAttackTargetPoint();
		relFacingToTarget = relFacingTo(targetPosition);
		hasTarget = true;
	}

	if (moveState == MOVESTATE_PIVOT_FORWARD) 
	{
		if ((moveStateGoal == MOVESTATE_PIVOT_FORWARD) || (moveStateGoal == MOVESTATE_FORWARD)) 
		{
			if ((path->numStepsWhenNotPaused > 0) && (path->curStep < path->numStepsWhenNotPaused)) 
			{
				Stuff::Vector3D wayPt = path->stepList[path->curStep].destination;

				appearance->setGestureGoal(MECH_STATE_STANDING);
				control.settings.mech.throttle = 100;
				float relFacingToWayPt = relFacingTo(wayPt);
				if ((relFacingToWayPt < -5.0) || (relFacingToWayPt > 5.0)) {
					float turnRate = -relFacingToWayPt;
					float maxRate = tonnageTurnRate[long(tonnage)] * frameLength;
					if (turnRate < 0.0f)
						maxRate = -maxRate;
					if (fabs(turnRate) > maxRate) {
						if (turnRate > 0)
							turnRate = maxRate;
						else
							turnRate = -maxRate;
					}
					control.settings.mech.rotate = turnRate;
					control.settings.mech.pivot = true;

					NewRotation = turnRate;
					return(true);
				}
				else 
				{
					pilot->setMoveState(MOVESTATE_FORWARD);
					if (pilot->getMoveTwisting())
						pilot->setMoveTwisting(false);
				}
			}
			else
				pilot->setMoveStateGoal(MOVESTATE_FORWARD /*MOVESTATE_STAND*/);
		}
		else
			pilot->setMoveState(MOVESTATE_FORWARD /*MOVESTATE_STAND*/);
	}
	else if (moveState == MOVESTATE_PIVOT_REVERSE) 
	{
		if ((moveStateGoal == MOVESTATE_PIVOT_REVERSE) || (moveStateGoal == MOVESTATE_REVERSE)) 
		{
			if ((path->numStepsWhenNotPaused > 0) && (path->curStep < path->numStepsWhenNotPaused)) 
			{
				Stuff::Vector3D wayPt = path->stepList[path->curStep].destination;

				appearance->setGestureGoal(MECH_STATE_STANDING);
				control.settings.mech.throttle = 100;
				float relFacingToWayPt = relFacingTo(wayPt);
				if ((relFacingToWayPt > -175.0) && (relFacingToWayPt < 175.0)) {
					float turnRate = 0.0;
					if (hasTarget && !isRunning) {
						if (pivotDirection == -1) {
							if (relFacingToTarget < 0)
								pivotDirection = 0;
							else
								pivotDirection = 1;
						}
						if (pivotDirection == 0)
							turnRate = 180.0 - relFacingToWayPt;
						else
							turnRate = -180.0 - relFacingToWayPt;
					}
					else {
						if (relFacingToWayPt < 0)
							turnRate = -180.0 - relFacingToWayPt;
						else
							turnRate = 180.0 - relFacingToWayPt;
					}
					
					float maxRate = tonnageTurnRate[long(tonnage)] * frameLength;
					if (fabs(turnRate) > maxRate) {
						if (turnRate > 0)
							turnRate = maxRate;
						else
							turnRate = -maxRate;
					}
 					control.settings.mech.rotate = turnRate;
					control.settings.mech.pivot = true;

					NewRotation = turnRate;
					return(true);
				}
				else {
					if (pilot->getMoveTwisting())
						pilot->setMoveTwisting(false);
						
					if (moveStateGoal == MOVESTATE_REVERSE) 
						pilot->setMoveState(MOVESTATE_REVERSE);
					else
						pilot->setMoveStateGoal(MOVESTATE_FORWARD /*MOVESTATE_STAND*/);
				}
			}
			else
				pilot->setMoveStateGoal(MOVESTATE_FORWARD /*MOVESTATE_STAND*/);
		}
		else
			pilot->setMoveState(MOVESTATE_FORWARD /*MOVESTATE_STAND*/);
	}
	else if (moveState == MOVESTATE_PIVOT_TARGET) {
		if (moveStateGoal == MOVESTATE_PIVOT_TARGET) {
			if (isRunning || !hasTarget) {
				pilot->setMoveStateGoal(MOVESTATE_FORWARD /*MOVESTATE_STAND*/);
				if (!pilot->isYielding() && !pilot->isWaitingForPoint())
					pilot->resumePath();
				pivotDirection = -1;
				return(false);
			}
			
			appearance->setGestureGoal(MECH_STATE_STANDING);
			control.settings.mech.throttle = 100;
			float fireArc = getFireArc();
			if ((relFacingToTarget < -fireArc) || (relFacingToTarget > fireArc)) {
				float turnRate = -relFacingToTarget;
				float maxRate = tonnageTurnRate[long(tonnage)] * frameLength;
				if (fabs(turnRate) > maxRate) {
					if (turnRate > 0)
						turnRate = maxRate;
					else
						turnRate = -maxRate;
				}
				control.settings.mech.rotate = turnRate;
				control.settings.mech.pivot = true;

				NewRotation = turnRate;
				return(true);
			}
			else 
				pilot->setMoveStateGoal(MOVESTATE_FORWARD /*MOVESTATE_STAND*/);
		}
		else
			pilot->setMoveState(MOVESTATE_FORWARD /*MOVESTATE_STAND*/);
	}
	else {
		if ((moveStateGoal == MOVESTATE_PIVOT_TARGET) || (moveStateGoal == MOVESTATE_PIVOT_FORWARD) || (moveStateGoal == MOVESTATE_PIVOT_REVERSE))
			pilot->setMoveState(moveStateGoal);
	}

	if (!pilot->isYielding() && !pilot->isWaitingForPoint())
		pilot->resumePath();
		
	pivotDirection = -1;
	return(false);
}

long BattleMech::getSpeedState (void) {

	return(mechSpeedStateArray[appearance->getCurrentGestureId()]);
}

void BattleMech::updateMoveStateGoal (void) {

	MovePathPtr path = pilot->getMovePath();

	Stuff::Vector3D targetPosition;
	targetPosition.Zero();
	GameObjectPtr target = pilot->getLastTarget();
	bool hasTarget = false;
	if (target) {
		targetPosition = target->getPosition();
		hasTarget = true;
		}
	else if (pilot->getCurTacOrder()->code == TACTICAL_ORDER_ATTACK_POINT) {
		targetPosition = pilot->getAttackTargetPoint();
		hasTarget = true;
	}

	long moveStateGoal = pilot->getMoveStateGoal();
	if (path->numSteps > 0) {
		if (MPlayer && !MPlayer->isServer()) {
			if (moveChunk.run || (legStatus == LEG_STATUS_IMPAIRED_LEFT) || (legStatus == LEG_STATUS_IMPAIRED_RIGHT)) {
				pilot->setMoveStateGoal(MOVESTATE_FORWARD);
				return;
			}
			}
		else {
			if (pilot->getMoveRun() || (legStatus == LEG_STATUS_IMPAIRED_LEFT) || (legStatus == LEG_STATUS_IMPAIRED_RIGHT)) {
				pilot->setMoveStateGoal(MOVESTATE_FORWARD);
				return;
			}
		}
		if (hasTarget) {
			if ((path->numStepsWhenNotPaused > 0) && (path->curStep < path->numStepsWhenNotPaused)) {
				Stuff::Vector3D wayPt = path->stepList[path->curStep].destination;

				float facingDelta = relFacingDelta(wayPt, targetPosition);
				float totalFireArc = getFireArc() + dynamics.max.mech.torsoYaw;
				long moveStateGoal = pilot->getMoveStateGoal();
				if (moveStateGoal == MOVESTATE_FORWARD) {
					if (facingDelta > totalFireArc) {
						if ((180.0 - facingDelta) <= totalFireArc) {
							if (!pilot->getMoveTwisting()) {
								pilot->setMoveTwisting(true);
								pilot->setMoveStateGoal(MOVESTATE_REVERSE);
							}
						}
					}
					}
				else {
					if ((180.0 - facingDelta) > totalFireArc) {
						if (facingDelta <= totalFireArc) {
							if (!pilot->getMoveTwisting()) {
								pilot->setMoveTwisting(true);
								pilot->setMoveStateGoal(MOVESTATE_FORWARD);
							}
						}
					}
				}
			}
			}
		else
			pilot->setMoveStateGoal(MOVESTATE_FORWARD);
		}
	else if ((moveStateGoal != MOVESTATE_PIVOT_TARGET) && (moveStateGoal != MOVESTATE_PIVOT_FORWARD) && (moveStateGoal != MOVESTATE_PIVOT_REVERSE)) {
		if (!hasTarget && (path->numStepsWhenNotPaused == 0))
			pilot->setMoveStateGoal(MOVESTATE_FORWARD);
	}
}

#define	DEBUG_MECH_UPDATE	0
float DistanceToWaypoint;

bool BattleMech::updateMovePath (float& newRotate, char& newThrottleSetting, long& newGestureStateGoal, long& newMoveState, long& minThrottle, long& maxThrottle, float &facingRotate) {

	DistanceToWaypoint = 9999.0;

	MovePathPtr path = pilot->getMovePath();
	TacticalOrderPtr curOrder = pilot->getCurTacOrder();

	bool allowedToRun = canRun() && pilot->getMoveRun();
	long curThrottleSetting = control.settings.mech.throttle;
	newThrottleSetting = curThrottleSetting;
	newRotate = 0.0;

	#if DEBUG_MECH_UPDATE
		if (selected) {
			long area = GlobalMoveMap->calcArea(objPosition->tileRow, objPosition->tileCol);
			char debugStr[256];
			sprintf(debugStr, "Area = %d\n", area);
			OutputDebugString(debugStr);
		}
	#endif

	updateHustleTime();
	bool hustle = (lastHustleTime + 2.0) > scenarioTime;

	bool aheadOfPointVehicle = false;
	bool stopForPointVehicle = false;
	MoverPtr pointVehicle = pilot->getPoint();
	bool hasGroupMoveOrder = (curOrder->isGroupOrder() && curOrder->isMoveOrder());

	if (!allowedToRun && !hustle && pointVehicle && !pointVehicle->isDisabled() && (pointVehicle != this) && hasGroupMoveOrder) {
		MechWarriorPtr pointPilot = pointVehicle->getPilot();
		float pointDistanceFromGoal = pointPilot->getMoveDistanceLeft();
		float myDistanceFromGoal = pilot->getMoveDistanceLeft();
		aheadOfPointVehicle = (myDistanceFromGoal < pointDistanceFromGoal);
		if (aheadOfPointVehicle) {
			allowedToRun = false;
			if (getSpeedState() == SPEED_STATE_MOVING_FAST) {
				if (!pilot->isWaitingForPoint())
					pilot->setMoveWaitForPointTime(scenarioTime + 5.0);
				}
			else {
				if (pilot->getMoveWaitForPointTime() < scenarioTime) {
					stopForPointVehicle = true;
					pilot->pausePath(); //path->numSteps = 0;
					pilot->setMoveWaitForPointTime(999999.0);
				}
			}
			}
		else {
			pilot->setMoveWaitForPointTime(-1.0);
			if (!pilot->isYielding()) {
				pilot->resumePath();
			}
		}
		}
	else {
		pilot->setMoveWaitForPointTime(-1.0);
	}

	bool goalReached = false;
	
	if ((legStatus == LEG_STATUS_NORMAL) || (legStatus == LEG_STATUS_HURTING) || (legStatus == LEG_STATUS_IMPAIRED_LEFT) || (legStatus == LEG_STATUS_IMPAIRED_RIGHT)) {
		if (path->numSteps > 0) {
			if (path->curStep == path->numSteps)
				goalReached = true;
			else {
				Stuff::Vector3D wayPt = path->stepList[path->curStep].destination;
				
				// Let's record our last valid position, in case we need to
				// crawl back from impassable terrain we get knocked onto.
				// Ultimately, this SHOULD NOT be necessary once we agree
				// on a new gesture/movement system... gd 6/2/97
				lastValidPosition = wayPt;

				float distanceFromWayPt = distanceFrom(wayPt);
				DistanceToWaypoint = distanceFromWayPt;

				//Calculate how far the mech will move this frame.
				// Vel is in m/s
				float vel = appearance->getVelocityMagnitude();
				float distanceThisFrame = vel * frameLength;

				float cushion = Mover::marginOfError[0];
				if (path->curStep == (path->numSteps - 1))
					cushion = Mover::marginOfError[1];

				if (cushion < distanceThisFrame)
				{
					cushion = distanceThisFrame;
				}

				if (distanceFromWayPt < cushion) {
					path->curStep++;
					pilot->setMoveTimeOfLastStep(scenarioTime);
					if (path->curStep < path->numSteps) {
						long curDir = path->stepList[path->curStep].direction;
						if (curDir > 7) {
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
					if (curDir > 7) {
						newGestureStateGoal = 6;
						return(false);
					}
				}

				if (!goalReached) 
				{
					float relFacingToWayPt = relFacingTo(wayPt, MECH_BODY_LOCATION_LLEG);
					long moveState = pilot->getMoveState();
					long moveStateGoal = pilot->getMoveStateGoal();

					if (moveState == MOVESTATE_FORWARD) 
					{
						if (moveStateGoal == MOVESTATE_FORWARD) 
						{
							if (legStatus == LEG_STATUS_IMPAIRED_LEFT) 
							{
								newGestureStateGoal = MECH_STATE_LIMPING_LEFT;
							}
							else if (legStatus == LEG_STATUS_IMPAIRED_RIGHT) 
							{
								newGestureStateGoal = MECH_STATE_LIMPING_RIGHT;
							}
							else if (allowedToRun) 
							{
								newGestureStateGoal = MECH_STATE_RUNNING;
							}
							else
								newGestureStateGoal = MECH_STATE_WALKING;
							
							if ((relFacingToWayPt < -5.0) || (relFacingToWayPt > 5.0)) 
							{
								// We can and will shift facing to destination...
								// Don't know that we need to force walk to turn!!
								// -fs
								
								newRotate = -relFacingToWayPt; 								
								float maxRate = tonnageTurnRate[long(tonnage)] * frameLength;
								if (fabs(newRotate) > maxRate) 
								{
									if (fabs(newRotate) < 50.0f)
										newThrottleSetting = 100.0f - fabs(newRotate);
									else
										newThrottleSetting = 0.0f;
									
									if (newRotate > 0.0)
										newRotate = maxRate;
									else
										newRotate = -maxRate;
								}
								else 
								{
									newThrottleSetting = pilot->calcMoveSpeedThrottle(getBodyState(), control.settings.mech.throttle);
								}
							}
							else
								newThrottleSetting = 100.0f;
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
					else if (moveState == MOVESTATE_REVERSE) 
					{
						if (moveStateGoal == MOVESTATE_FORWARD) 
						{
							pilot->pausePath(); //pilot->getMovePath()->numSteps = 0;
							newMoveState = MOVESTATE_PIVOT_FORWARD;
						}
						else if (moveStateGoal == MOVESTATE_REVERSE) 
						{
							newGestureStateGoal = MECH_STATE_REVERSE;
								
 							if (relFacingToWayPt < 0)
								newRotate = -(relFacingToWayPt + 180.0);
							else
								newRotate = -(relFacingToWayPt - 180.0);
								
							// We can and will shift facing to destination...
							// Don't know that we need to force walk to turn!!
							// -fs
								
							float maxRate = tonnageTurnRate[long(tonnage)] * frameLength;
							if (fabs(newRotate) > maxRate) 
							{
								if (fabs(newRotate) < 50.0f)
									newThrottleSetting = 100.0f - fabs(newRotate);
								else
									newThrottleSetting = 0.0f;
								
								if (newRotate > 0.0)
									newRotate = maxRate;
								else
									newRotate = -maxRate;
							}
							else 
							{
								newThrottleSetting = pilot->calcMoveSpeedThrottle(getBodyState(), control.settings.mech.throttle);
							}
						}
						else if (moveStateGoal == MOVESTATE_PIVOT_FORWARD) 
						{
							pilot->pausePath(); //pilot->getMovePath()->numSteps = 0;
							newMoveState = MOVESTATE_PIVOT_FORWARD;
						}
						else if (moveStateGoal == MOVESTATE_PIVOT_REVERSE) 
						{
							pilot->pausePath(); //pilot->getMovePath()->numSteps = 0;
							newMoveState = MOVESTATE_PIVOT_REVERSE;
						}
						else 
						{
							pilot->pausePath(); //pilot->getMovePath()->numSteps = 0;
							newMoveState = MOVESTATE_FORWARD /*MOVESTATE_STAND*/;
						}
					}
					else 
					{
						if ((moveStateGoal == MOVESTATE_FORWARD) || (moveStateGoal == MOVESTATE_PIVOT_FORWARD)) 
						{
							pilot->pausePath(); //pilot->getMovePath()->numSteps = 0;
							newMoveState = MOVESTATE_PIVOT_FORWARD;
						}
						else if ((moveStateGoal == MOVESTATE_REVERSE) || (moveStateGoal == MOVESTATE_PIVOT_REVERSE)) 
						{
							pilot->pausePath(); //pilot->getMovePath()->numSteps = 0;
							newMoveState = MOVESTATE_PIVOT_REVERSE;
						}
					}
				}
			}
		}
		else 
		{
			newGestureStateGoal = MECH_STATE_STANDING;
		}
	}
	else 
	{
		newGestureStateGoal = MECH_STATE_STANDING;
	}

	if (goalReached) 
	{
		// Did we finish just a local part of a global path?
		if (pilot->getMovePathType() == MOVEPATH_COMPLEX) 
		{
			if (pilot->getMovePathGlobalStep() < (pilot->getMoveNumGlobalSteps() - 1)) 
			{
				goalReached = false;
			}
		}
		pilot->clearMovePath(0);
	}

	return(goalReached);
}

void BattleMech::setNextMovePath (char& newThrottleSetting, long& newGestureStateGoal) {


	Stuff::Vector3D nextWayPoint;
	bool haveWayPoint = pilot->getNextWayPoint(nextWayPoint, true);
	if (haveWayPoint) {
		pilot->setMoveGoal(MOVEGOAL_LOCATION, &nextWayPoint);
		TacticalOrderPtr curTacOrder = pilot->getCurTacOrder();
		pilot->requestMovePath(curTacOrder->selectionIndex, MOVEPARAM_FACE_TARGET/*+MOVEPARAM_INIT*/, 1);
		}
	else {
		// This may be a hack--do we want to be able to follow moving
		// objects that we don't necessarily have tracked? We will
		// for now...
		//GameObjectPtr moveGoalObject = NULL;
		//long goalType = pilot->getMoveGoal(ORDER_CURRENT, NULL, &moveGoalObject);
		//pilot->setMoveGoal(ORDER_CURRENT, MOVEGOAL_NONE, NULL);
		pilot->clearMoveOrders();
		newGestureStateGoal = MECH_STATE_STANDING;
	}
}

void BattleMech::updateTorso (float newRotatePerSec) 
{
	float torsoRelFacing = 0.0;
	GameObjectPtr target = pilot->getCurrentTarget();
	rotateValues[0] = target ? relFacingTo(target->getPosition()) : 0.0;
	rotateValues[1] = torsoRotation;
	rotateValues[2] = newRotatePerSec;
	if (target)
		torsoRelFacing = relFacingTo(target->getPosition()) + torsoRotation + newRotatePerSec;
	else if (pilot->getCurTacOrder()->code == TACTICAL_ORDER_ATTACK_POINT)
		torsoRelFacing = relFacingTo(pilot->getAttackTargetPoint()) + torsoRotation + newRotatePerSec;
	else
		torsoRelFacing = torsoRotation;
	rotateValues[3] = torsoRelFacing;

	rotateValues[4] = torsoRelFacing;


	float fireArc = getFireArc();
	if ((torsoRelFacing < -fireArc) || (torsoRelFacing > fireArc)) 
	{
		float turnRate = -torsoRelFacing;
		rotateValues[5] = turnRate;
		float maxRate = (float)dynamics.max.mech.torsoYawRate * frameLength;
		if (fabs(turnRate) > maxRate) {
			if (turnRate < 0.0)
				turnRate = -maxRate;
			else
				turnRate = maxRate;
		}
		control.settings.mech.rotateTorso = turnRate;
	}
/*
	if (target && CombatLog) {
		static char moveStateChar[NUM_MOVESTATES] = {' ', ' ', ' ', '*', '*', '*'};
		static char s[512];
		sprintf(s, "TORSO ROTATE: %-15s %-06d %-05d %-5c, relF = %-06d    torR = %06d   nRot = %06d torRF = %06d(%06d) turnR = %06d",
			getName(),
			target->getWatchID(),
			turn,
			moveStateChar[getMoveState()],
			(long)rotateValues[0],
			(long)rotateValues[1],
			(long)rotateValues[2],
			(long)rotateValues[3],
			(long)rotateValues[4],
			(long)rotateValues[5]);
		CombatLog->write(s);
		CombatLog->write(" ");
	}
*/
}

void BattleMech::setControlSettings (float& newRotate, char& newThrottleSetting, long& newGestureStateGoal, long& minThrottle, long& maxThrottle, float &facingRotate) {

	if (inJump && !appearance->isInJump()) {
		inJump = false;
		pilot->resumePath();
	}

	if (newGestureStateGoal == 6) {
		MovePathPtr path = pilot->getMovePath();
		pilot->pausePath(); //path->numSteps = 0;
		jumpGoal = path->stepList[path->curStep].destination;
		appearance->setJumpParameters(jumpGoal);
	}

	if (MPlayer && !MPlayer->isServer()) {
		if (statusChunk.jumpOrder && !inJump) {
			land->cellToWorld(statusChunk.targetCellRC[0], statusChunk.targetCellRC[1], jumpGoal);
			if (distanceFrom(jumpGoal) > 8.0) {
				newGestureStateGoal = 6;
				appearance->setJumpParameters(jumpGoal);
			}
		}
		}
	else {
		if (pilot->getCurTacOrder()->isJumpOrder() && !inJump) {
			newGestureStateGoal = 6;
			jumpGoal.x = pilot->getCurTacOrder()->moveParams.wayPath.points[0];
			jumpGoal.y = pilot->getCurTacOrder()->moveParams.wayPath.points[1];
			jumpGoal.z = pilot->getCurTacOrder()->moveParams.wayPath.points[2];
			appearance->setJumpParameters(jumpGoal);
		}
	}

	if (newGestureStateGoal != -1) 
	{
		control.settings.mech.gestureGoal = newGestureStateGoal;
		switch (newGestureStateGoal) {
			case 6:
				control.settings.mech.throttle = 100.0f;
				inJump = true;
				break;
			case MECH_STATE_WALKING:
				if (newThrottleSetting != -1) {
					if (newThrottleSetting < minThrottle)
						newThrottleSetting = minThrottle;
					else if (newThrottleSetting > maxThrottle)
						newThrottleSetting = maxThrottle;
					control.settings.mech.throttle = newThrottleSetting;
				}
				break;
			default:
				control.settings.mech.throttle = newThrottleSetting;
		}
	}

	if (newRotate != 0.0f)
		control.settings.mech.rotate = newRotate;
		
	if (facingRotate != 0.0f)
		control.settings.mech.facingRotate = facingRotate;
}

void BattleMech::startShutDown (void)
{
	appearance->setGesture(0);		//Force us to parked
	shutDownThisFrame = false;
	startUpThisFrame = false;
	setStatus(OBJECT_STATUS_SHUTDOWN);
	sensorSystem->setShutdown(true);
}

void BattleMech::updateMovement (void) {

	long minThrottle = 35;
	long maxThrottle = 100;

	NewRotation = 0.0;

	if (disableThisFrame) 
	{
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

	if (shutDownThisFrame) 
	{
		//Confusing, but is used to display the helicopter dust cloud.
		// Could also use as a fall dust cloud?  
		if (getMoveType() == MOVETYPE_AIR)
			appearance->playEjection();
			
 		appearance->setGestureGoal(MECH_STATE_PARKED);
		if ((scenarioTime > 8.0) || (getTeam() == Team::home))
		{
			if (getMoveType() != MOVETYPE_AIR)
				soundSystem->playDigitalSample(POWERDOWN_SFX,getPosition());
			else
				soundSystem->playDigitalSample(COPTER_POWERUP,getPosition());
		}

		shutDownThisFrame = false;
		startUpThisFrame = false;
		setStatus(OBJECT_STATUS_SHUTDOWN);
		control.settings.mech.throttle = maxThrottle;
		sensorSystem->setShutdown(true);
		return;
	}

	long gID = appearance->getCurrentGestureId();
	if (pilot && pilot->getCurTacOrder()->code == TACTICAL_ORDER_POWERUP) {
		if (gID != 2)
			appearance->setGestureGoal(MECH_STATE_STANDING);
		else	//Otherwise, we were ordered to powerUp and we are.  Clear iT!!
			pilot->clearCurTacOrder();
	}

	if (startUpThisFrame) 
	{
		//Confusing, but is used to display the helicopter dust cloud.
		// Could also use as a fall dust cloud?  
		if (getMoveType() == MOVETYPE_AIR)
			appearance->playEjection();
			
		if (getMoveType() != MOVETYPE_AIR)
			soundSystem->playDigitalSample(POWERUP_SFX,getPosition());
		else
			soundSystem->playDigitalSample(COPTER_POWERDN,getPosition());

		startUpThisFrame = false;
		shutDownThisFrame = false;
		setStatus(OBJECT_STATUS_NORMAL);
		control.settings.mech.throttle = maxThrottle;
		if (useSound && !MPlayer && Team::home->isEnemy(getTeam()) && (getMoveType() != MOVETYPE_AIR))
			soundSystem->playBettySample(BETTY_POWERUP);
		sensorSystem->setShutdown(false);
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

	if (failedPilotingCheck && (getMoveType() != MOVETYPE_AIR)) {
		long fallGesture = RandomNumber(2) ? MECH_STATE_FALLEN_FORWARD : MECH_STATE_FALLEN_BACKWARD;
		if (hitFromBehindThisFrame)
			fallGesture = MECH_STATE_FALLEN_FORWARD;
		else if (hitFromFrontThisFrame)
			fallGesture = MECH_STATE_FALLEN_BACKWARD;
		appearance->setGestureGoal(fallGesture);
		failedPilotingCheck = false;
		control.settings.mech.throttle = maxThrottle;
		return;
	}

	if (updateJump())
		return;

	if (pivotTo())
		return;

	float newRotate = 0.0;
	float facingRotate = 0.0f;
	long newGestureStateGoal = -1;
	long newMoveState = -1;
	char newThrottleSetting = -1;
	bool goalReached = false;

	goalReached = updateMovePath(newRotate, newThrottleSetting, newGestureStateGoal, newMoveState, minThrottle, maxThrottle, facingRotate);

	if (goalReached)
		setNextMovePath(newThrottleSetting, newGestureStateGoal);

	if (newMoveState != -1)
		pilot->setMoveState(newMoveState);

	setControlSettings(newRotate, newThrottleSetting, newGestureStateGoal, minThrottle, maxThrottle,facingRotate);
	
	updateMoveStateGoal();
	
	NewRotation = newRotate;
}


Stuff::Vector3D BattleMech::getPositionFromHS (long nodeId)
{
	return(appearance->getWeaponNodePosition(nodeId));
}

#ifdef PROFILE
extern long srCtrlUpd;
extern long srApprUpd;
extern long srDyneUpd;
extern long srWeapUpd;
extern long srObjtUpd;
#endif


#define JUMP_FX		454

void BattleMech::createJumpFX (void) {

#ifdef USE_JETS
	if (!jumpJets[0] && !jumpJets[1])
	{
		jumpJets[0] = (JetPtr)createObject(JUMP_FX);
		jumpJets[0]->setOwner(this);
		
		jumpJets[1] = (JetPtr)createObject(JUMP_FX);
		jumpJets[1]->setOwner(this);
		
		craterManager->addCrater(SCORCHMARKS,position,0);
	}
#endif
}	

void BattleMech::endJumpFX (void) {

#ifdef USE_JETS
	if (jumpJets[0] || jumpJets[1])
	{
		delete jumpJets[0];
		jumpJets[0] = NULL;
		
		delete jumpJets[1];
		jumpJets[1] = NULL;
	}
#endif
}	

Stuff::Vector3D BattleMech::getJumpPosition (void) 
{
	Stuff::Vector3D result;
	result.x = result.y = result.z = 0.0f;

	return result;
}	
