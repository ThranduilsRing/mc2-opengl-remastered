// GroundVehicle: vehicle pilot, AI/network/player control, dynamics, animations, status rating and rendering.
// Declared in the class headers; shared names in gvehicl_internal.h.
#include "gvehicl_internal.h"


//----------------------------------------------------------------------------------
// END OF MOVEMENT UPDATE ROUTINES
//----------------------------------------------------------------------------------

Stuff::Vector3D GroundVehicle::getPositionFromHS (long nodeId) 
{

	// NEED TO IMPLEMENT PROPER HOTSPOTS FOR VEHICLES...
	// How about now?
	// -fs

	//-----------------------------------------
	Stuff::Vector3D hsPos = position;
	if (appearance)
	{
		hsPos = appearance->getHitNode();
		if ((nodeId != -1) || (hsPos == position))
		{
			hsPos = appearance->getWeaponNodePosition(nodeId);
		}
	}

	return(hsPos);
}


//extern L_INTEGER startCk;
//extern L_INTEGER endCk;

//----------------------------------------------------------------------------------
void GroundVehicle::disable (unsigned long cause)
{
	Mover::disable(cause);
	timeLeft = 0.0;
	
}	

//----------------------------------------------------------------------------------

float GroundVehicle::getStatusRating (void) {

	//-------------------------------
	// calculate Weapon effectiveness
	float weaponEffect = 1.0;
	if (maxWeaponEffectiveness)
		weaponEffect = (float)weaponEffectiveness / (float)maxWeaponEffectiveness;

	//----------------
	// Calculate armor
	float armorFront = armor[GROUNDVEHICLE_LOCATION_FRONT].curArmor / armor[GROUNDVEHICLE_LOCATION_FRONT].maxArmor * 0.6 + 0.4;
	float armorLeft = armor[GROUNDVEHICLE_LOCATION_LEFT].curArmor / armor[GROUNDVEHICLE_LOCATION_LEFT].maxArmor * 0.6 + 0.4;
	float armorRight = armor[GROUNDVEHICLE_LOCATION_RIGHT].curArmor / armor[GROUNDVEHICLE_LOCATION_RIGHT].maxArmor * 0.6 + 0.4;
	float armorRear = armor[GROUNDVEHICLE_LOCATION_REAR].curArmor / armor[GROUNDVEHICLE_LOCATION_REAR].maxArmor * 0.6 + 0.4;
	float armorEffect = armorFront * armorRear * armorLeft * armorRight;
				
	//-----------------------
	// Calculate pilot Wounds
	float pilotWoundTable[7] = {1.00f,0.95f,0.85f,0.75f,0.50f,0.30f,0.00f};
	float pilotEffect = pilotWoundTable[(long)getPilot()->getWounds()];
	if (isDestroyed() || isDisabled())
		pilotEffect = 0.0;
					
	float rating = weaponEffect * armorEffect * pilotEffect;
	return(rating);
}

//Infantry Guy
#define VEHICLEPILOT_ID		638
//----------------------------------------------------------------------------------
void GroundVehicle::createVehiclePilot (void)
{
	//We are ALREADY a vehicle pilot, stop breeding.
	if (isVehiclePilot)
		return;

	//Don't let powered armor or infantry create a vehicle pilot.
	if (!pathLocks)
		return;

	GroundVehiclePtr vehiclePilot = ObjectManager->getOpenVehicle();
	if (!vehiclePilot)
		return;

	ObjectTypePtr objType = ObjectManager->getObjectType(VEHICLEPILOT_ID);
	if (!objType)
		return;

	//No guys allowed to come out in the water!!
	int cellR, cellC;
	land->worldToCell(position,cellR, cellC);
	if (GameMap->getDeepWater(cellR, cellC) || GameMap->getShallowWater(cellR, cellC))
		return;

	vehiclePilot->init(true, objType);

	//----------------------------------------------
	// Load the profile data into the game object...
	if (ObjectTypeManager::objectFile->seekPacket(VEHICLEPILOT_ID) == NO_ERR) 
	{
		//--------------------------------------------------------
		FitIniFile profileFile;
		long result = profileFile.open(ObjectTypeManager::objectFile,ObjectTypeManager::objectFile->getPacketSize());
		if (result == NO_ERR) 
		{
			//-------------------------------------------
			result = vehiclePilot->init(&profileFile);
			if (result != NO_ERR)
				return;
		}
		else
		{
			return;
		}

		profileFile.close();
	}
				
	vehiclePilot->setExists(true);
	vehiclePilot->setAwake(true);

	if (vehiclePilot->sensorSystem)
		SensorManager->removeTeamSensor(teamId, vehiclePilot->sensorSystem);

	vehiclePilot->sensorSystem = NULL;
	vehiclePilot->isVehiclePilot = true;

	//-------------------------------------------
	// <deleted really obnoxious Glenn Slam>
	// Thanks!

	vehiclePilot->pilot = pilot;		//Gets Current Vehicle Pilot.
	vehiclePilot->pilotHandle = pilotHandle;
	pilot->setVehicle(vehiclePilot);
	pilot->lobotomy();

	vehiclePilot->setControl(CONTROL_AI);	//AI Control
	
	//They get added to the home team so they are always visible.
	vehiclePilot->teamId = Team::home->getId();

	//BUT they are enemies on the GUI!!
	vehiclePilot->commanderId = 1;

	//------------------------------------------------------------------
	// Set the object's position, initial gesture and rotation.
	vehiclePilot->setPosition(position);
	vehiclePilot->setLastValidPosition(position);
				
	//--------------------------------------------------------------------
	// Add the object to the object list, if it exists
	// Otherwise, add it to the holder list which is a member of scenario
	ObjectManager->buildMoverLists();					

	//---------------------------------------------------------------
	// We need to override the TacOrder of the pilot so that the
	// little marine goes running about like he is trying to escape.
	pilot->clearAttackOrders();
	pilot->clearMoveOrders();
	
	Stuff::Vector3D location;
	location.Zero();
	
	pilot->orderMoveToPoint (false, true, ORDER_ORIGIN_PLAYER, location, -1, TACORDER_PARAM_RUN);

	vehiclePilot->getAppearance()->resetPaintScheme(0x00ffffff,
													0x00ffffff,
													0x00ff8000);
	vehiclePilot->setTangible(true);
	ObjectManager->rebuildCollidableList = true;
}	

//----------------------------------------------------------------------------------

void GroundVehicle::updateAIControl (void) {

	control.reset();

	if (getAwake())	{
		if (turretBlownThisFrame)
			turretBlownThisFrame = false;

		updateDamageTakenRate();

		if (!isDisabled() && pilot->alive()) {
			if (getTeamId() > -1)
				pilot->mainDecisionTree();
			updateMovement();
			}
		else if (shutDownThisFrame || disableThisFrame)
			updateMovement();
	}
}

//---------------------------------------------------------------------------

void GroundVehicle::updateNetworkControl (void) {

	control.reset();

	if (getAwake())	{
		if (turretBlownThisFrame)
			turretBlownThisFrame = false;

		//-----------------------------------------
		// Update any weaponfire chunks received...
		updateWeaponFireChunks(CHUNK_RECEIVE);

		updateCriticalHitChunks(CHUNK_RECEIVE);

		updateRadioChunks(CHUNK_RECEIVE);

		if (!isDisabled() && pilot->alive()) {
			pilot->checkAlarms();
			netUpdateMovement();
			}
		else if (shutDownThisFrame || disableThisFrame) {
			netUpdateMovement();
		}
	}
}

//---------------------------------------------------------------------------

void GroundVehicle::updatePlayerControl (void) {

	control.reset();

}

//---------------------------------------------------------------------------

void GroundVehicle::updateDynamics (void) {

	if (!appearance)
		Fatal(0, " Groundvehicle.updateDynamics: no appearance ");

	if (isDestroyed())
		return;

	float yawRate = control.settings.groundVehicle.rotate;
	
	float turretYawRate = control.settings.groundVehicle.rotateTurret;

	//--------------------------------------------------------------------
	// Rotate turret by turret yaw rate, making sure we don't turn too far.
	if (turretYawRate != 0.0) {
		float newTurretRotation = turretRotation;

		if (newTurretRotation > dynamics.max.groundVehicle.turretYaw) {
			newTurretRotation = dynamics.max.groundVehicle.turretYaw;
			turretYawRate = 0.0;
		}

		if (newTurretRotation < -dynamics.max.groundVehicle.turretYaw) {
			newTurretRotation = -dynamics.max.groundVehicle.turretYaw;
			turretYawRate = 0.0;
		}

		if ((newTurretRotation + turretYawRate) > dynamics.max.groundVehicle.turretYaw)
			turretYawRate = dynamics.max.groundVehicle.turretYaw - newTurretRotation;

		if ((newTurretRotation + turretYawRate) < -dynamics.max.groundVehicle.turretYaw)
			turretYawRate = -dynamics.max.groundVehicle.turretYaw - newTurretRotation;

		newTurretRotation += turretYawRate;
		turretRotation = newTurretRotation;
	}

	rotate(yawRate);

	//------------------------------------------------------------------------
	// The gesture is how we control speed.  So, at this point, check
	// the vehicleControlData->gestureGoal and see if we need to change goals.
	// See if throttle settings work.
	float throttle = control.settings.groundVehicle.throttle;
	throttle /= 100.0;

	//--------------------------------
	// Ok, real vehicle dynamics.
	// Throttle controls maxVelocity.  Rate of velocity change is constant.
	// 50% throttle means vehicle accelerates at 50% of maxAccel upto 50% of maxVelocity
	float realMaxVelocity = dynamics.max.groundVehicle.speed;
	if (control.settings.groundVehicle.isWalking)
		realMaxVelocity = gvWalkSpeed;
		
	float maxThrottledVel = throttle * realMaxVelocity;
	float velDiff = maxThrottledVel - velocityMag;
	
	//-----------------------------------------------------
	// If we're slowing down, slow us down!
	if (((velDiff < 0.0) && (accel > 0.0)) || ((velDiff > 0.0) && (accel < 0.0)))
		accel = -accel;
	
	float velChangeThisFrame = accel * frameLength;
	if (fabs(velChangeThisFrame) > fabs(velDiff))
		velChangeThisFrame = velDiff;
	
	velocityMag += velChangeThisFrame;
}

//---------------------------------------------------------------------------
long GroundVehicle::updateAnimations (void)
{
	//-------------------------------------------------
	// Always Animate ground vehicles completely.
	// If the animations do not exist, it ain't no thing.
	// Destroyed/disabled ground vehicles do nothing!
	// Ground vehicle gestures are:
	//		0 - Stand/Idle
	// 		1 - Walk
	//		2 - Run
	//		3 - Repair
	//
	if (!MPlayer)		//ONLY allowed to animate outside of Multiplayer.  Do not want to pass anim info around.  TOO Late!
	{
		long animState = appearance->getCurrentGestureId();
		if (!isDisabled() && !isDestroyed())
		{
			switch (animState)
			{
				case -1:		//NOT UPDATED YET. SWITCH TO STAND
					if (!appearance->getInTransition())
					{
						appearance->setGesture(0);
					}
					break;
					
				case 2:			//We are running.  If we have stopped or are walking, change.
					if (velocityMag == 0.0f)
					{
						//we've stopped.
						//INSTANTLY change to stand!
						appearance->setGesture(0);
					}
					else if ((velocityMag != 0.0f) && (velocityMag <= (dynamics.max.groundVehicle.speed * 0.5f)))
					{
						//We are now walking.
						appearance->setGesture(1);
					}
					
					if (refitBuddyWID)
					{
						GameObjectPtr refitMech = ObjectManager->getByWatchID(refitBuddyWID);
						Stuff::Vector3D distance;
						distance.Subtract(refitMech->getPosition(),getPosition());
						float dist = distance.GetApproximateLength();
						if (dist <= 128.0f)
							appearance->setGesture(3);
					}
					break;
					
				case 0:			//Standing, if moving now, change.
					if (velocityMag > (dynamics.max.groundVehicle.speed * 0.5f))
					{
						//We are now running.
						appearance->setGesture(2);
					}
					else if ((velocityMag != 0.0f) && (velocityMag <= (dynamics.max.groundVehicle.speed * 0.5f)))
					{
						//We are now Walking.
						appearance->setGesture(1);
					}
					
					if (refitBuddyWID)
					{
						GameObjectPtr refitMech = ObjectManager->getByWatchID(refitBuddyWID);
						Stuff::Vector3D distance;
						distance.Subtract(refitMech->getPosition(),getPosition());
						float dist = distance.GetApproximateLength();
						if (dist <= 128.0f)
							appearance->setGesture(3);
					}
					break;
					
				case 1:			//We are walking.  If we have stopped moving or are running, change.
					if (velocityMag == 0.0f)
					{
						//we've stopped.
						//INSTANTLY change to stand!
						appearance->setGesture(0);
					}
					else if (velocityMag > (dynamics.max.groundVehicle.speed * 0.5f))
					{
						//We are now running.
						appearance->setGesture(2);
					}
					
					if (refitBuddyWID)
					{
						GameObjectPtr refitMech = ObjectManager->getByWatchID(refitBuddyWID);
						Stuff::Vector3D distance;
						distance.Subtract(refitMech->getPosition(),getPosition());
						float dist = distance.GetApproximateLength();
						if (dist <= 128.0f)
							appearance->setGesture(3);
					}
					break;
					
				case 3:			//Repairing.  Check if done repairing.  If so, switch to stand.
					if (!appearance->getInTransition())
					{
						appearance->setGesture(4);
					}
					break;
	
				case 4:
					if (!refitBuddyWID)
					{
						appearance->setGesture(5);	//Start closing up.
					}
					break;
	
				case 5:
					if (!appearance->getInTransition())
					{
						if (!refitBuddyWID)
						{
							appearance->setGesture(0);
						}
						else
						{
							appearance->setGesture(3);
						}
					}
			}
		}
		else
		{
			//He's dead.
			appearance->setGesture(0);
		}
	}

	return 0;
}
