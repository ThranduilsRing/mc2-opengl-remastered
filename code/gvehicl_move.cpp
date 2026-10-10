// GroundVehicle: movement: pivoting, throttle, move-path following (local and network), crash avoidance.
// Declared in the class headers; shared names in gvehicl_internal.h.
#include "gvehicl_internal.h"



bool GroundVehicle::pivotTo (void) {

	//If we have no maxVelocity, we are an artillery piece.  DO NOT PIVOT!!
	if (dynamics.max.groundVehicle.speed == 0.0f)
		return false;

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

				control.settings.groundVehicle.throttle = 0;
				float relFacingToWayPt = relFacingTo(wayPt);
				if ((relFacingToWayPt < -45.0) || (relFacingToWayPt > 45.0)) 
				{
					float turnRate = -relFacingToWayPt;
					if (hasTarget && !isRunning) 
					{
						if (pivotDirection == -1) 
						{
							if (relFacingToTarget < 0)
								pivotDirection = 0;
							else
								pivotDirection = 1;
						}
						if (pivotDirection == 0) 
						{
							if (relFacingToWayPt < 0.0)
								turnRate = -relFacingToWayPt;
							else
								turnRate = 360.0 - relFacingToWayPt;
						}
						else 
						{
							if (relFacingToWayPt < 0.0)
								turnRate = -360.0 - relFacingToWayPt;
							else
								turnRate = -relFacingToWayPt;
						}
					}
					float maxRate = vehicleTurnRate[long(tonnage)] * frameLength;
					if (fabs(turnRate) > maxRate) 
					{
						if (turnRate > 0.0)
							turnRate = maxRate;
						else
							turnRate = -maxRate;
					}
					
					control.settings.groundVehicle.rotate = turnRate;
					control.settings.groundVehicle.pivot = true;

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
		if ((moveStateGoal == MOVESTATE_PIVOT_REVERSE) || (moveStateGoal == MOVESTATE_REVERSE)) {
			if ((path->numStepsWhenNotPaused > 0) && (path->curStep < path->numStepsWhenNotPaused)) {
				Stuff::Vector3D wayPt = path->stepList[path->curStep].destination;

				control.settings.groundVehicle.throttle = 0;
				float relFacingToWayPt = relFacingTo(wayPt);
				if ((relFacingToWayPt > -175.0) && (relFacingToWayPt < 175.0)) 
				{
					float turnRate = 0.0;
					if (hasTarget && !isRunning) 
					{
						if (pivotDirection == -1) 
						{
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
					else 
					{
						if (relFacingToWayPt < 0)
							turnRate = -180.0 - relFacingToWayPt;
						else
							turnRate = 180.0 - relFacingToWayPt;
					}
					float maxRate = vehicleTurnRate[long(tonnage)] * frameLength;
					if (fabs(turnRate) > maxRate) 
					{
						if (turnRate > 0.0)
							turnRate = maxRate;
						else
							turnRate = -maxRate;
					}
					control.settings.groundVehicle.rotate = turnRate;
					control.settings.groundVehicle.pivot = true;

					NewRotation = turnRate;
					return(true);
				}
				else 
				{
					if (pilot->getMoveTwisting())
						pilot->setMoveTwisting(false);
					if (moveStateGoal == MOVESTATE_REVERSE) 
					{
						pilot->setMoveState(MOVESTATE_REVERSE);
					}
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
	else if (moveState == MOVESTATE_PIVOT_TARGET) 
	{
		if (moveStateGoal == MOVESTATE_PIVOT_TARGET) 
		{
			if (isRunning || !hasTarget) 
			{
				pilot->setMoveStateGoal(MOVESTATE_FORWARD /*MOVESTATE_STAND*/);
				if (!pilot->isYielding() && !pilot->isWaitingForPoint())
					pilot->resumePath();
				pivotDirection = -1;
				return(false);
			}
			control.settings.groundVehicle.throttle = 0;
			float relFacingToTarget = relFacingTo(targetPosition);
			float fireArc = getFireArc();
			if ((relFacingToTarget < -fireArc) || (relFacingToTarget > fireArc)) 
			{
				float turnRate = -relFacingToTarget;
				float maxRate = vehicleTurnRate[long(tonnage)] * frameLength;
				if (fabs(turnRate) > maxRate) 
				{
					if (turnRate > 0.0)
						turnRate = maxRate;
					else
						turnRate = -maxRate;
				}
				control.settings.groundVehicle.rotate = turnRate;
				control.settings.groundVehicle.pivot = true;

				NewRotation = turnRate;
				return(true);
			}
			else 
			{
				pilot->setMoveStateGoal(MOVESTATE_FORWARD /*MOVESTATE_STAND*/);
			}
		}
		else
			pilot->setMoveState(MOVESTATE_FORWARD /*MOVESTATE_STAND*/);
	}
	else 
	{
		if ((moveStateGoal == MOVESTATE_PIVOT_TARGET) || (moveStateGoal == MOVESTATE_PIVOT_FORWARD) || (moveStateGoal == MOVESTATE_PIVOT_REVERSE))
			pilot->setMoveState(moveStateGoal);
	}

	if (!pilot->isYielding() && !pilot->isWaitingForPoint())
		pilot->resumePath();
		
	pivotDirection = -1;
	return(false);
}


void GroundVehicle::calcThrottleLimits (long& minThrottle, long& maxThrottle) {

	// NOT WORKING RIGHT...
	float incline = 90.0; //getVelocityTilt();
	if (incline < 90.0) {
		maxThrottle = float2long(maxThrottle*InclineThrottleMultiplier[chassis][0]);
		minThrottle = float2long(minThrottle*InclineThrottleMultiplier[chassis][0]);
		}
	else if (incline > 90.0) {
		maxThrottle = float2long(maxThrottle*InclineThrottleMultiplier[chassis][1]);
		minThrottle = float2long(minThrottle*InclineThrottleMultiplier[chassis][1]);
	}
	
}


long GroundVehicle::getSpeedState (void) {

	if (velocityMag != 0.0)
		return(SPEED_STATE_MOVING_FAST);
	return(SPEED_STATE_STATIONARY);
}


void GroundVehicle::updateMoveStateGoal (void) {

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

	MovePathPtr path = pilot->getMovePath();
	long moveStateGoal = pilot->getMoveStateGoal();
	if (path->numSteps > 0) {
		if (MPlayer && !MPlayer->isServer()) {
			if (moveChunk.run) {
				pilot->setMoveStateGoal(MOVESTATE_FORWARD);
				return;
			}
			}
		else {
			if (pilot->getMoveRun()) {
				pilot->setMoveStateGoal(MOVESTATE_FORWARD);
				return;
			}
		}
		if (hasTarget) {
			if ((path->numStepsWhenNotPaused > 0) && (path->curStep < path->numStepsWhenNotPaused)) {
				Stuff::Vector3D wayPt = path->stepList[path->curStep].destination;
				float facingDelta = relFacingDelta(wayPt, targetPosition);
				float totalFireArc = getFireArc() + dynamics.max.groundVehicle.turretYaw;
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


bool GroundVehicle::updateMovePath (float& newRotate, char& newThrottleSetting, long& newMoveState, long& minThrottle, long& maxThrottle) 
{
	
	DistanceToWaypoint = 9999.0;

	MovePathPtr path = pilot->getMovePath();
	TacticalOrderPtr curOrder = pilot->getCurTacOrder();

	long curThrottleSetting = control.settings.groundVehicle.throttle;
	newThrottleSetting = curThrottleSetting;
	newRotate = 0.0;

	bool allowedToRun = pilot->getMoveRun();

	updateHustleTime();
	bool hustle = (lastHustleTime + 2.0) > scenarioTime;

	//-----------------------------------------------
	// Am I ahead of my point vehicle, if I have one?
	bool aheadOfPointVehicle = false;
	bool stopForPointVehicle = false;
	MoverPtr pointVehicle = (MoverPtr)pilot->getPoint();
	bool hasGroupMoveOrder = (curOrder->isGroupOrder() && curOrder->isMoveOrder());
	if (!allowedToRun && !hustle && pointVehicle && !pointVehicle->isDisabled() && (pointVehicle != this) && hasGroupMoveOrder) 
	{
		MechWarriorPtr pointPilot = pointVehicle->getPilot();
		float pointDistanceFromGoal = pointPilot->getMoveDistanceLeft();
		float myDistanceFromGoal = pilot->getMoveDistanceLeft();
		aheadOfPointVehicle = (myDistanceFromGoal < pointDistanceFromGoal);
		if (aheadOfPointVehicle) {
			if (getSpeedState() == SPEED_STATE_MOVING_FAST) 
			{
				//-------------------------------------------------
				// Running. So, slow to a walk for a few seconds...
				if (!pilot->isWaitingForPoint())
					pilot->setMoveWaitForPointTime(scenarioTime + 5.0);
			}
			else 
			{
				//--------------------------------------------------------------
				// Already walking, so let's just stop until point catches up...
				if (pilot->getMoveWaitForPointTime() < scenarioTime) 
				{
					stopForPointVehicle = true;
					pilot->pausePath(); //path->numSteps = 0;
					pilot->setMoveWaitForPointTime(999999.0);
				}
			}
		}
		else 
		{
			pilot->setMoveWaitForPointTime(-1.0);
			if (!pilot->isYielding()) 
			{
				pilot->resumePath();
			}
		}
	}
	else 
	{
		pilot->setMoveWaitForPointTime(-1.0);
	}

	bool goalReached = false;
	if (path->numSteps > 0) 
	{
		if (path->curStep == path->numSteps)
			goalReached = true;
		else 
		{
			Stuff::Vector3D wayPt = path->stepList[path->curStep].destination;

			//---------------------------------------------------------
			// Let's record our last valid position, in case we need to
			// crawl back from impassable terrain we get knocked onto.
			// Ultimately, this SHOULD NOT be necessary once we agree
			// on a new gesture/movement system... gd 6/2/97
			lastValidPosition = wayPt;

			float distanceFromWayPt = distanceFrom(wayPt);
			DistanceToWaypoint = distanceFromWayPt;

			float vel = velocityMag;
			float distanceThisFrame = vel * frameLength;

			float cushion = Mover::marginOfError[0];
			if (path->curStep == (path->numSteps - 1))
				cushion = Mover::marginOfError[1];
				
			if (cushion < distanceThisFrame)
			{
				//------------------------------------------------------------------------
				//We are going to move farther then the current cushion would allow for.
				// Make the cushion larger.
				// Maybe we should make cushion this value all of the time?
				cushion = distanceThisFrame;
			}

			if (distanceFromWayPt < cushion) 
			{
				path->curStep++;
				pilot->setMoveTimeOfLastStep(scenarioTime);
				if (path->curStep < path->numSteps) 
				{
					wayPt = path->stepList[path->curStep].destination;
				}
				else 
				{
					goalReached = true;
				}
			}

			if (!goalReached) 
			{

				float relFacingToWayPt = relFacingTo(wayPt);
				long moveState = pilot->getMoveState();
				long moveStateGoal = pilot->getMoveStateGoal();

				if (moveState == MOVESTATE_FORWARD) 
				{
					if (moveStateGoal == MOVESTATE_FORWARD) 
					{

						newThrottleSetting = 100;
						
						if ((relFacingToWayPt < -5.0) || (relFacingToWayPt > 5.0)) 
						{
							newRotate = -relFacingToWayPt;
							float maxRate = vehicleTurnRate[long(tonnage)] * frameLength;
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
						}
					}
					else if (moveStateGoal == MOVESTATE_REVERSE) 
					{
						pilot->pausePath();
						newMoveState = MOVESTATE_PIVOT_REVERSE;
					}
					else if (moveStateGoal == MOVESTATE_PIVOT_FORWARD) 
					{
						pilot->pausePath();
						newMoveState = MOVESTATE_PIVOT_FORWARD;
					}
					else if (moveStateGoal == MOVESTATE_PIVOT_REVERSE) 
					{
						pilot->pausePath();
						newMoveState = MOVESTATE_PIVOT_REVERSE;
					}
					else 
					{
						pilot->pausePath();
						newMoveState = MOVESTATE_FORWARD /*MOVESTATE_STAND*/;
					}
				}
				else if (moveState == MOVESTATE_REVERSE) 
				{
					if (moveStateGoal == MOVESTATE_FORWARD) 
					{
						pilot->pausePath();
						newMoveState = MOVESTATE_PIVOT_FORWARD;
					}
					else if (moveStateGoal == MOVESTATE_REVERSE) 
					{
						newThrottleSetting = -100;
						if (relFacingToWayPt < 0)
							newRotate = -(relFacingToWayPt + 180.0);
						else
							newRotate = -(relFacingToWayPt - 180.0);
							
						float maxRate = vehicleTurnRate[long(tonnage)] * frameLength;
						if (fabs(newRotate) > maxRate) 
						{
							newThrottleSetting = -50.0f;
							
							if (fabs(newRotate) < 50.0f)
								newThrottleSetting = -50.0f + fabs(newRotate);
							else
								newThrottleSetting = 0.0f;
									
 							if (newRotate > 0.0) 
							{
								newRotate = maxRate;
							}
							else 
							{
								newRotate = -maxRate;
							}
						}
						else 
						{
							newThrottleSetting = -100;
						}
					}
					else if (moveStateGoal == MOVESTATE_PIVOT_FORWARD) 
					{
						pilot->pausePath();
						newMoveState = MOVESTATE_PIVOT_FORWARD;
					}
					else if (moveStateGoal == MOVESTATE_PIVOT_REVERSE) 
					{
						pilot->pausePath();
						newMoveState = MOVESTATE_PIVOT_REVERSE;
					}
					else 
					{
						pilot->pausePath();
						newMoveState = MOVESTATE_FORWARD /*MOVESTATE_STAND*/;
					}
				}
				else 
				{
					if ((moveStateGoal == MOVESTATE_FORWARD) || (moveStateGoal == MOVESTATE_PIVOT_FORWARD)) 
					{
						pilot->pausePath();
						newMoveState = MOVESTATE_PIVOT_FORWARD;
					}
					else if ((moveStateGoal == MOVESTATE_REVERSE) || (moveStateGoal == MOVESTATE_PIVOT_REVERSE)) 
					{
						pilot->pausePath();
						newMoveState = MOVESTATE_PIVOT_REVERSE;
					}
				}
			}
		}
	}
	else 
	{
		newThrottleSetting = 0;
	}

	// if we're a sweeping minesweeper or a laying minelayer, take it slow
	if (mineSweeper && sweepTime > 0 && sweepTime < gvSweepTime)
		maxThrottle = MineSweepThrottle;
	if ((mineLayer != 0) && pilot->getCurTacOrder()->moveParams.mode == MOVE_MODE_MINELAYING)
		maxThrottle = MineLayThrottle;

	if (goalReached) 
	{
		if (pilot->getMovePathType() == MOVEPATH_COMPLEX) 
		{
			if (pilot->getMovePathGlobalStep() < (pilot->getMoveNumGlobalSteps() - 1)) 
			{
				//-----------------------------------------------------------
				// We've reached the end of our current short-range path, but
				// we have more to go before we've reached our long-range
				// goal, so calc our next short-range path...
				goalReached = false;
			}
		}
		pilot->clearMovePath(0);
		newThrottleSetting = 0;
	}

	return(goalReached);
}


void GroundVehicle::setNextMovePath (char& newThrottleSetting) {

	//----------------------------------------
	// If this is only an intermediate path,
	// let's check where we need to go next...
	//pilot->clearMovePath(ORDER_CURRENT);

	Stuff::Vector3D nextWayPoint;
	bool haveWayPoint = pilot->getNextWayPoint(nextWayPoint, true);
	if (haveWayPoint) {
		pilot->setMoveGoal(MOVEGOAL_LOCATION, &nextWayPoint);
		TacticalOrderPtr curTacOrder = pilot->getCurTacOrder();
		pilot->requestMovePath(curTacOrder->selectionIndex, MOVEPARAM_FACE_TARGET + MOVEPARAM_FOLLOW_ROADS, 0);
		}
	else {
		pilot->clearMoveOrders();
		newThrottleSetting = 0;
	}
}


void GroundVehicle::setControlSettings (float& newRotate, char& newThrottleSetting, long& minThrottle, long& maxThrottle) {

	//-----------------------------------------------------------------------------
	// If we have no move path, then we shouldn't be moving. Let's set our throttle
	// setting to zero, just to be safe...
	long result = NO_ERR;
	MovePathPtr curPath = pilot->getMovePath();
	bool allowedToRun = false;
	if (MPlayer && !MPlayer->isServer())
		allowedToRun = moveChunk.run;
	else
		allowedToRun = pilot->getMoveRun();
	
	if (curPath->numSteps == 0)
		newThrottleSetting = 0;

	result = NO_ERR;
	if (newThrottleSetting != -1) {
		if (newThrottleSetting < minThrottle)
			newThrottleSetting = minThrottle;
		else if (newThrottleSetting > maxThrottle)
			newThrottleSetting = maxThrottle;
		control.settings.groundVehicle.throttle = newThrottleSetting;
	}
	
	if (newRotate != 0.0)
		control.settings.groundVehicle.rotate = newRotate;
		
	control.settings.groundVehicle.isWalking = !allowedToRun;
}


void GroundVehicle::updateTurret (float newRotatePerSec) {

	float turretRelFacing = 0.0;
	GameObjectPtr target = pilot->getCurrentTarget();
	if (target)
		turretRelFacing = relFacingTo(target->getPosition()) + turretRotation + newRotatePerSec;
	else if (pilot->getCurTacOrder()->code == TACTICAL_ORDER_ATTACK_POINT)
		turretRelFacing = relFacingTo(pilot->getAttackTargetPoint()) + turretRotation + newRotatePerSec;
	else
		turretRelFacing = turretRotation;

	float fireArc = getFireArc();
	if ((turretRelFacing < -fireArc) || (turretRelFacing > fireArc)) {
		float newRotateTurret = -turretRelFacing;
		float maxRate = (float)dynamics.max.groundVehicle.turretYawRate * frameLength;
		if (fabs(newRotateTurret) > maxRate) {
			if (newRotateTurret < 0.0)
				newRotateTurret = -maxRate;
			else
				newRotateTurret = maxRate;
		}
		control.settings.groundVehicle.rotateTurret = newRotateTurret;
	}
}


void GroundVehicle::updateMovement (void) {

	NewRotation = 0.0;

	if (disableThisFrame) {
		disableThisFrame = false;
		shutDownThisFrame = false;
		startUpThisFrame = false;
		setStatus(OBJECT_STATUS_DISABLED);
		control.settings.groundVehicle.throttle = 0;
		return;
	}

	if (shutDownThisFrame) {
		shutDownThisFrame = false;
		startUpThisFrame = false;
		setStatus(OBJECT_STATUS_SHUTDOWN);
		control.settings.groundVehicle.throttle = 0;
		soundSystem->playDigitalSample(VEHICLE_POWERUP,getPosition());
		return;
	}

	if (startUpThisFrame) {
		startUpThisFrame = false;
		setStatus(OBJECT_STATUS_NORMAL);
		soundSystem->playDigitalSample(VEHICLE_POWERDN,getPosition());
		control.settings.groundVehicle.throttle = 100;
		return;
	}

	if (isCaptured() || isDisabled()) {
		control.settings.groundVehicle.throttle = 0;
		return;
	}

	if (engineBlowTime > -1.0)
		return;

	if (pivotTo())
		return;

	long minThrottle = -100;
	long maxThrottle = 100;
	float newRotate = 0.0;
	char newThrottleSetting = 0;
	long newMoveState = -1;
	bool goalReached = false;

	calcThrottleLimits(minThrottle, maxThrottle);
	
		goalReached = updateMovePath(newRotate, newThrottleSetting, newMoveState, minThrottle, maxThrottle);

	if (goalReached)
		setNextMovePath(newThrottleSetting);

	if (newMoveState != -1)
		pilot->setMoveState(newMoveState);

	setControlSettings(newRotate, newThrottleSetting, minThrottle, maxThrottle);

	updateMoveStateGoal();

	NewRotation = newRotate;
}


bool GroundVehicle::netUpdateMovePath (float& newRotate, char& newThrottleSetting, long& newMoveState, long& minThrottle, long& maxThrottle) {

	DistanceToWaypoint = 9999.0;

	MovePathPtr path = pilot->getMovePath();

	long curThrottleSetting = control.settings.groundVehicle.throttle;
	newThrottleSetting = curThrottleSetting;
	newRotate = 0.0;

	bool goalReached = false;
	if (path->numSteps > 0) {
		if (path->curStep == path->numSteps)
			goalReached = true;
		else {
			Stuff::Vector3D wayPt = path->stepList[path->curStep].destination;

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
					wayPt = path->stepList[path->curStep].destination;
					}
				else {
					goalReached = true;
				}
			}

			if (!goalReached) {

				float relFacingToWayPt = relFacingTo(wayPt);
				long moveState = pilot->getMoveState();
				long moveStateGoal = pilot->getMoveStateGoal();

				if (moveState == MOVESTATE_FORWARD) {
					if (moveStateGoal == MOVESTATE_FORWARD) {

						newThrottleSetting = 100;
						
						if ((relFacingToWayPt < -5.0) || (relFacingToWayPt > 5.0)) {
							newRotate = -relFacingToWayPt;
							float maxRate = dynamics.max.groundVehicle.yawRate * frameLength;
							if (fabs(newRotate) > maxRate) {
								if (newRotate > 0.0)
									newRotate = maxRate;
								else
									newRotate = -maxRate;
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
						newThrottleSetting = -100;
						if (relFacingToWayPt < 0)
							newRotate = -(relFacingToWayPt + 180.0);
						else
							newRotate = -(relFacingToWayPt - 180.0);
						float maxRate = dynamics.max.groundVehicle.yawRate * frameLength;
						if (fabs(newRotate) > maxRate) {
							if (newRotate > 0.0) {
								newRotate = maxRate;
								newThrottleSetting = -50;
								}
							else {
								newRotate = -maxRate;
								newThrottleSetting = -50;
							}
							}
						else {
							newThrottleSetting = -100;
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
		newThrottleSetting = 0;
	}

	// if we're a sweeping minesweeper or a laying minelayer, take it slow
	if (mineSweeper && sweepTime > 0 && sweepTime < gvSweepTime)
		maxThrottle = MineSweepThrottle;
	if ((mineLayer != 0) && pilot->getCurTacOrder()->moveParams.mode == MOVE_MODE_MINELAYING)
		maxThrottle = MineLayThrottle;

	return(goalReached);
}


void GroundVehicle::netUpdateMovement (void) {

	MovePathPtr path = pilot->getMovePath();
	bool atEndOfPath = (path->curStep == (path->numSteps - 1));
	
	Stuff::Vector3D wayPt;
	float distanceFromWayPt = 1000000.0;
	if ((path->curStep > -1) && (path->curStep < path->numSteps)) {
		wayPt = path->stepList[path->curStep].destination;
		distanceFromWayPt = distanceFrom(wayPt);
	}
	if (atEndOfPath && (distanceFromWayPt < Mover::marginOfError[1])) 
	{
		//----------------------------------------------
		// Movement states no longer exist.  Just stop!
		pilot->clearMoveOrders();
		control.settings.groundVehicle.throttle = 0;
	}

	if (disableThisFrame) {
		disableThisFrame = false;
		shutDownThisFrame = false;
		startUpThisFrame = false;
		setStatus(OBJECT_STATUS_DISABLED);
		control.settings.groundVehicle.throttle = 0;
		return;
	}

	if (shutDownThisFrame) {
		shutDownThisFrame = false;
		startUpThisFrame = false;
		setStatus(OBJECT_STATUS_SHUTDOWN);
		control.settings.groundVehicle.throttle = 0;
		return;
	}

	if (startUpThisFrame) {
		startUpThisFrame = false;
		setStatus(OBJECT_STATUS_NORMAL);
		control.settings.groundVehicle.throttle = 100;
		return;
	}

	if (isCaptured() || isDisabled()) {
		control.settings.groundVehicle.throttle = 0;
		return;
	}

	if (engineBlowTime > -1.0)
		return;

	if (pivotTo())
		return;

	long minThrottle = -100;
	long maxThrottle = 100;
	float newRotate = 0;
	char newThrottleSetting = 0;
	long newMoveState = -1;
	bool goalReached = false;

	calcThrottleLimits(minThrottle, maxThrottle);
	
	goalReached = netUpdateMovePath(newRotate, newThrottleSetting, newMoveState, minThrottle, maxThrottle);

	if (newMoveState != -1)
		pilot->setMoveState(newMoveState);

	setControlSettings(newRotate, newThrottleSetting, minThrottle, maxThrottle);

	updateMoveStateGoal();
	NewRotation = newRotate;
}


bool GroundVehicle::crashAvoidanceSystem (void) {

	if (MPlayer && !MPlayer->isServer())
		return(false);

	MovePathPtr path = pilot->getMovePath();

	if (path->numStepsWhenNotPaused == 0)
		return(false);

	if (pilot->getMoveWaitForPointTime() > 999990.0)
		return(false);

	Stuff::Vector3D vel = getRotationVector();
	vel *= -velocityMag;
	vel *= frameLength;
	vel *= worldUnitsPerMeter;

	Stuff::Vector3D newPosition;
	newPosition.Add(position, vel);

	//---------------------------------------------------------------------
	// Is this new position in a pathlocked area? If so, put on the brakes!
	int cellR, cellC;
	land->worldToCell(newPosition, cellR, cellC);

	//-------------------------
	// To avoid corner stops...
	bool clippingCorner = false;
	int dir = path->getDirection(path->curStep);
	if ((dir == 1) || (dir == 3) || (dir == 5) || (dir == 7)) {
		bool firstCornerClipped = getAdjacentCellPathLocked((moveLevel == 2), cellPositionRow, cellPositionCol, adjClippedCell[dir][0]);
		bool secondCornerClipped = getAdjacentCellPathLocked((moveLevel == 2), cellPositionRow, cellPositionCol, adjClippedCell[dir][1]);
		clippingCorner = (firstCornerClipped && secondCornerClipped);
	}

	bool reachedEnd;
	bool blockReachedEnd;
	bool nearingMover = getPathRangeLock(crashAvoidPath, &reachedEnd);
	bool pathLocked = /*stepOnMover ||*/ nearingMover;
	bool pathBlocked = getPathRangeBlocked(crashAvoidPath, &blockReachedEnd);
	long stepIntoGate = (path->crossesClosedGate(-1, 2) > 1);

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
			//-------------------------------------------------------------------
			// What exactly is wrong here? Is our goal currently occupied? If so,
			// and it's our final goal, then stop!
			if (reachedEnd || blockReachedEnd) {
				//-----------------------------------------------------------------------
				// Since we're practically there, let's just kill the current move orders
				// and let the pilot's movementDecision update decide whether a new
				// one should be set...
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
