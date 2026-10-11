//******************************************************************************************
//	gvehicl.cpp - This file contains the GroundVehicle Class Code
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "gvehicl_internal.h"


constexpr double TURN_THRESHOLD = 20.0;
#define BLIP_FRAME_RATE		0.067

long DefaultPilotId	= 653;
char marineProfileName[80] = "PEM00001";

//**********************************************************************************

float vehicleTurnRate[110] = 
{
	360.0f,360.0f,360.0f,360.0f,360.0f,360.0f,360.0f,360.0f,360.0f,360.0f, //0 through 9 tons
	240.0f,240.0f,240.0f,240.0f,240.0f,240.0f,240.0f,240.0f,240.0f,240.0f, //10 through 19 tons
	240.0f,240.0f,240.0f,240.0f,240.0f,240.0f,240.0f,240.0f,240.0f,240.0f, //20 through 29 tons
	240.0f,240.0f,240.0f,240.0f,240.0f,240.0f,240.0f,240.0f,240.0f,240.0f, //30 through 39 tons
	240.0f,240.0f,240.0f,240.0f,240.0f,240.0f,240.0f,240.0f,240.0f,240.0f, //40 through 49 tons
	240.0f,240.0f,240.0f,240.0f,240.0f,240.0f,240.0f,240.0f,240.0f,240.0f, //50 through 59 tons
	210.0f,210.0f,210.0f,210.0f,210.0f,210.0f,210.0f,210.0f,210.0f,210.0f, //60 through 69 tons
	140.0f,140.0f,140.0f,140.0f,140.0f,140.0f,140.0f,140.0f,140.0f,140.0f, //70 through 79 tons
	120.0f,120.0f,120.0f,120.0f,120.0f,120.0f,120.0f,120.0f,120.0f,120.0f, //80 through 89 tons
	 90.0f, 90.0f, 90.0f, 90.0f, 90.0f, 90.0f, 90.0f, 90.0f, 90.0f, 90.0f, //90 through 99 tons
	 90.0f, 90.0f, 90.0f, 90.0f, 90.0f, 90.0f, 90.0f, 90.0f, 90.0f, 90.0f //100 through 109 tons
};

long GroundVehicleAttackerMoveModifier[MAX_GVACTOR_STATES] = {
	0,		// Stopped
	10,		// Moving
	100		// Destroyed
};

long GroundVehicleCriticalHitTable[NUM_GROUNDVEHICLE_CRITICAL_HIT_TYPES] = {
	63,		// No Effect
	1,		// Ammo or Power Unit hit
	1,		// Fuel Tank Hit
	1,		// Crew Killed
	1,		// Engine Hit
	1,		// Main Weapon Jammed
	1,		// Crew Stunned
	6,		// Movement Disabled
	8,		// Movement Hindered
	11,		// Hovercraft -ONLY- Movement Hindered
	6		// Turret Jammed
};

float gvCollisionThreshold = 0.0;
float gvObjectCollisionThreshold = 0.0;
float gvTonnageCollisionThreshold = 0.0;
float gvTreeDeflection = 0.0;
float gvSweepTime = 0.0;
float gvWalkSpeed = 0.0;
float gvHillSpeedFactor = 0.0;

float InclineThrottleMultiplier[NUM_GROUNDVEHICLE_CHASSIS][2] = {
	{0.2f/*0.8*/, 1.0},		// WHEELED
	{0.2f/*1.0*/, 1.0},		// TRACKED
	{0.2f/*0.75*/, 1.25}	// HOVER
};

long DefaultGroundVehicleCrashAvoidSelf = 1;
long DefaultGroundVehicleCrashAvoidPath = 1;
long DefaultGroundVehicleCrashBlockSelf = 1;
long DefaultGroundVehicleCrashBlockPath = 1;
float DefaultGroundVehicleCrashYieldTime = 2.0;

long NumMarines = 0;

//**********************************************************************************
// GROUNDVEHICLE TYPE class
//**********************************************************************************

void GroundVehicleType::init (void)  {

	objectTypeClass = VEHICLE_TYPE;
	objectClass = GROUNDVEHICLE;

	mineSweeper = false;
	refitPoints = 0;
	recoverPoints = 0;
	crashAvoidSelf = DefaultGroundVehicleCrashAvoidSelf;
	crashAvoidPath = DefaultGroundVehicleCrashAvoidPath;
	crashBlockSelf = DefaultGroundVehicleCrashBlockSelf;
	crashBlockPath = DefaultGroundVehicleCrashBlockPath;
	crashYieldTime = DefaultGroundVehicleCrashYieldTime;
	explDmg = 0.0;
	explRad = 0.0;
	mineLayer = 0;
	hoverCraft = false;
	aerospaceSpotter = false;
	moveType = MOVETYPE_GROUND;
	LOSFactor = 1.0f;
}

//---------------------------------------------------------------------------

void GroundVehicleType::destroy (void) 
{
	ObjectType::destroy();
}

//----------------------------------------------------------------------------------

long GroundVehicleType::init (FilePtr objFile, unsigned long fileSize) {

	const char* bodyLocationString[NUM_GROUNDVEHICLE_LOCATIONS] = {
		"Front",
		"Left",
		"Right",
		"Rear",
		"Turret"
	};

	long result = 0;

	FitIniFile vehicleFile;
	result = vehicleFile.open(objFile, fileSize);
	if (result != NO_ERR)
		Fatal(result, " GroundVehicle:Init - Unable to open file ");

	result = vehicleFile.seekBlock("General");
	if (result != NO_ERR)
		Fatal(result, " GroundVehicle:Init - Unable to find Block General ");

	result = vehicleFile.readIdLong("MoveType", moveType);
	if (result != NO_ERR)
		moveType = MOVETYPE_GROUND;

	result = vehicleFile.readIdFloat("LOSFactor",LOSFactor);
	if (result != NO_ERR)
		LOSFactor = 1.0f;
		
 	result = vehicleFile.readIdUChar("Chassis", chassis);
	if (result != NO_ERR)
		Fatal(result, " GroundVehicle:Init - Unable to find Chassis ");

	//------------------------------
	// Check Special Vehicle Info

	result = vehicleFile.readIdLong("RefitPoints", refitPoints);
	if (result != NO_ERR)
		refitPoints = 0;

	result = vehicleFile.readIdLong("ResourcePoints", resourcePoints);
	if (result != NO_ERR)
		resourcePoints = 0;

	result = vehicleFile.readIdLong("RecoverPoints", recoverPoints);
	if (result != NO_ERR)
		recoverPoints = 0;
	
	result = vehicleFile.readIdBoolean("MineSweeper", mineSweeper);
	if (result != NO_ERR)
		mineSweeper = false;

	result = vehicleFile.readIdLong("MineLayer", mineLayer);
	if (result != NO_ERR)
		mineLayer = 0;

	result = vehicleFile.readIdBoolean("HoverCraft", hoverCraft);
	if (result != NO_ERR)
		hoverCraft = false;
	
	result = vehicleFile.readIdBoolean("AeroSpaceSpotter", aerospaceSpotter);
	if (result != NO_ERR)
		aerospaceSpotter = false;
		
 	result = vehicleFile.readIdBoolean("IsSensorContact", isSensorContact);
	if (result != NO_ERR)
		isSensorContact = true;

	//------------------------------
	// Splash Damage.
	result = vehicleFile.readIdFloat("ExplosionRadius",explRad);
	if (result != NO_ERR)
		explRad = 0.0;
		
	result = vehicleFile.readIdFloat("ExplosionDamage",explDmg);
	if (result != NO_ERR)
		explDmg = 0.0;

	result = vehicleFile.readIdBoolean("PathLocks",pathLocks);
	if (result != NO_ERR)
		pathLocks = true;
		
	//------------------------------------------------------------------
	// Now, read in the max internal structure for each body location...
	result = vehicleFile.seekBlock("InternalStructure");
	if (result != NO_ERR)
		Fatal(result, " GroundVehicle:Init - Unable to find InternalStructure Block ");

	for (long curLocation = 0; curLocation < NUM_GROUNDVEHICLE_LOCATIONS; curLocation++) 
	{
		result = vehicleFile.readIdUChar(bodyLocationString[curLocation], maxInternalStructure[curLocation]);
		if (result != NO_ERR)
			Fatal(curLocation, " GroundVehicle:Init - Unable to find Internal Structure Piece");
	}

	//------------------------------------------------------------------
	// Read in the data needed to move the vehicle and create base classes
	dynamics.setType(DYNAMICS_GROUNDVEHICLE);
	dynamics.init(&vehicleFile);

	// UNIT-PROFILE-SEAM-1: optional .fit [UnitProfile] data read — SOLE
	// jump-capability populator for vehicles (no jump component to scan).
	populateProfileFromFit(vehicleFile, profile());

	// UNITPROFILE-VALIDATOR-1 (slice 6): report-only construction-rules check,
	// gated behind MC2_UNIT_PROFILE_DATA. Runs AFTER facets are set. A stock
	// vehicle grants no CAP_JUMP -> coherent -> logs nothing. A vehicle authored
	// with [UnitProfile] Jump=TRUE on a non-walker locomotion trips the one live
	// finding (no compatible jump executor) — reported, NOT rejected.
	validateAndReportUnitProfile(profile(), vehicleFile.getFilename());

	//------------------------------------------------------------------
	// Initialize the base object Type from the current file.
	result = ObjectType::init(&vehicleFile);
	return(result);
}

//-----------------------------------------------------------------------------------

bool GroundVehicleType::handleCollision (GameObjectPtr collidee, GameObjectPtr collider) {

	//---------------------------------------------
	// The default reaction of any object in the world
	// is to simply explode.  This just returns true
	// to facilitate this behaviour.
	//
	// Don't blow us up unless we hit another mech\vehicle.
	if (MPlayer && !MPlayer->isServer())
		return(false);
	
	bool sameTeam = false;
	bool collide = true;
	switch (collider->getObjectClass())
	{
		case BATTLEMECH:
		case GROUNDVEHICLE:
		case ELEMENTAL:
		{
			if (collidee->isFriendly(collider) && (collidee->getCommanderId() == collider->getCommanderId()))
				sameTeam = true;

			if (sameTeam)
				collide = false;
			else {
				GameObjectPtr collideeRamTarget = ((MoverPtr)collidee)->getPilot()->getCurTacOrder()->getRamTarget();
				GameObjectPtr colliderRamTarget = ((MoverPtr)collider)->getPilot()->getCurTacOrder()->getRamTarget();
				if ((collideeRamTarget == collider) || (colliderRamTarget == collidee))
					collide = true;
				else
					collide = false; //(((MoverPtr)collidee)->getPilot()->checkSkill(MWS_PILOTING) >= 0);
			}
					
			if (!((MoverPtr)collidee)->pathLocks && !sameTeam)
			{
				collide = true;			//Something steppable
				
			}

			if (collide) {
				if ((collidee->getCollisionFreeFromWID() != collider->getWatchID()) || (collidee->getCollisionFreeTime() < scenarioTime))
					collide = true;
				else
					collide = false;
			}

			if (collide) {
				//----------------------------------------------------------------------
				// Don't allow collision between these two again for a period of time...
				collidee->setCollisionFreeFromWID(collider->getWatchID());
				collidee->setCollisionFreeTime(scenarioTime + 2.0);

				if (dynamics.max.groundVehicle.speed != 0.0) {
					//------------------------------------
					// Adjust my velocity and direction...
					collidee->rotate(90.0);
					((MoverPtr)collidee)->bounceToAdjCell();

				}

				//---------------------------------------------
				// Administer the damage from this collision...
				long hitLocation = collidee->calcHitLocation(collider, -1, ATTACKSOURCE_COLLISION, 0);
				WeaponShotInfo shotInfo;
// UGLY HACK HERE...
				if (sameTeam)
					shotInfo.init(collider->getWatchID(), -1, 0 /*(long)(collider->getTonnage() / 100.0 + 0.5)*/, hitLocation, collidee->relFacingTo(collider->getPosition()));
//
				else
					shotInfo.init(collider->getWatchID(), -1, (long)(collider->getTonnage() / 10.0 + 0.5), hitLocation, collidee->relFacingTo(collider->getPosition()));
				collidee->handleWeaponHit(&shotInfo, (MPlayer != nullptr));
				((GroundVehiclePtr)collidee)->deadByCrushed = true;

				//---------------------------------------------
				if (!sameTeam && (collidee->getWindowsVisible() == turn))
					soundSystem->playDigitalSample(MECH_COLLIDE,collider->getPosition());

				return(false);
			}
		}
		break;

		case BUILDING:
		case TREEBUILDING:
		{
			//Should Never collide with Buildings.  Only Steppables which ARE not this king of building!!
		}
		break;

		case TERRAINOBJECT:
		case TREE:
		{
			if ((collidee->getCollisionFreeFromWID() != collider->getWatchID()) || (collidee->getCollisionFreeTime() < scenarioTime))
			{
				//----------------------------------------------------------------------
				// Don't allow collision between these two again for a period of time...
				collidee->setCollisionFreeFromWID(collider->getWatchID());
				collidee->setCollisionFreeTime(scenarioTime + 2.0);

				//------------------------------------
				// Adjust my velocity and direction...
				float turnAround = 0.0;
				float tonnageClass = collidee->getTonnage();
				if (tonnageClass < gvTonnageCollisionThreshold)
					turnAround = (gvTonnageCollisionThreshold / tonnageClass) * gvTreeDeflection;
				if (turnAround > 0.0)
					collidee->rotate(turnAround);

				//---------------------------------------------
				if (collidee->getWindowsVisible() == turn)
					soundSystem->playDigitalSample(MECH_COLLIDE,collidee->getPosition());
				return(false);
			}
		}
		break;
	}

	return(false);
}

//-----------------------------------------------------------------------------------
bool GroundVehicleType::handleDestruction (GameObjectPtr collidee, GameObjectPtr collider) {

	//-------------------------------------------------------
	// For now, a BattleMech will play a default explosion
	// and disappear after the explosion is half way through
	MoverPtr vehicle = (MoverPtr)collidee;

	if (!vehicle->getPilot())
		Fatal(0, " No Pilot in this vehicle! ");

	//--------------------------------------------------------
	// Let's let the unit know we're dying if we're a point...
	// No, Don't!!!
	if (vehicle->getPoint() == vehicle) {
		vehicle->getGroup()->setPoint(nullptr);
		//--------------------------------------------------------
		// If there is no new point, all units must be blown away.
		// How do we want to handle this?
	}

	//-----------------------------
	// Immediately lose contacts...
	if (vehicle->sensorSystem)
	{
		vehicle->sensorSystem->disable();
		vehicle->sensorSystem->broken = true;
	}

	if (vehicle->withdrawing) 
	{
		vehicle->timeLeft = 0.0;
		//-----------------------------------------------
		// Let the pilot know we are about to withdraw...
		vehicle->getPilot()->triggerAlarm(PILOT_ALARM_VEHICLE_WITHDRAWN, 0);
	}
	else 
	{
		vehicle->timeLeft = 0.0;
		//------------------------------------------
		// Let the pilot know we are about to die...
		vehicle->getPilot()->triggerAlarm(PILOT_ALARM_VEHICLE_DESTROYED, collider ? collider->getWatchID() : 0);
		vehicle->setStatus(OBJECT_STATUS_DESTROYED);
		vehicle->appearance->setObjStatus(OBJECT_STATUS_DESTROYED);
		vehicle->appearance->setVisibility(true,true);
		vehicle->appearance->update();
		vehicle->exploding = false;
		if (MPlayer)
			vehicle->setFlag(OBJECT_FLAG_REMOVED, true);
		
		//------------------------------------
		// What heroic music should be played?
		if (CombatLog) {
			char s[1024];
			sprintf(s, "[%.2f] vehicle.destroyed: (%05d)%s", scenarioTime, vehicle->getPartId(), vehicle->getName());
			CombatLog->write(s);
			CombatLog->write(" ");
		}
	}

	return(true);
}

//----------------------------------------------------------------------------------

long GroundVehicleType::loadHotSpots(FitIniFilePtr vehicleFile) {


	return(NO_ERR);
}

//----------------------------------------------------------------------------------

GameObjectPtr GroundVehicleType::createInstance (void) {

	GroundVehiclePtr newVehicle = new GroundVehicle;
	if (!newVehicle)
		return(nullptr);

	newVehicle->init(true, this);
	//newVehicle->setIdNumber(NextIdNumber++);

	return(newVehicle);
}

//**********************************************************************************
// GROUNDVEHICLE class
//**********************************************************************************

long GroundVehicle::loadGameSystem (FitIniFilePtr sysFile) {

	long result = 0;

	result = sysFile->seekBlock("GroundVehicle:FireWeapon");
	if (result != NO_ERR)
		return(result);

	result = sysFile->readIdLongArray("AttackerMoveModifier", GroundVehicleAttackerMoveModifier, MAX_GVACTOR_STATES);
	if (result != NO_ERR)
		return(result);

	result = sysFile->seekBlock("GroundVehicle:Damage");
	if (result != NO_ERR)
		return(result);

	result = sysFile->readIdLongArray("CriticalHitTable", GroundVehicleCriticalHitTable, NUM_GROUNDVEHICLE_CRITICAL_HIT_TYPES);
	if (result != NO_ERR)
		return(result);

	result = sysFile->seekBlock("GroundVehicle:Collision");
	if (result != NO_ERR)
		return(result);

	result = sysFile->readIdFloat("collisionThreshold", gvCollisionThreshold);
	if (result != NO_ERR)
		return(result);

	result = sysFile->readIdFloat("objectThreshold", gvObjectCollisionThreshold);
	if (result != NO_ERR)
		return(result);

	result = sysFile->readIdFloat("tonnageThreshold", gvTonnageCollisionThreshold);
	if (result != NO_ERR)
		return(result);

	result = sysFile->readIdFloat("treeDeflection", gvTreeDeflection);
	if (result != NO_ERR)
		return(result);

	result = sysFile->seekBlock("GroundVehicle:Movement");
	if (result != NO_ERR)
		return(result);

	long crashSize = 0;
	result = sysFile->readIdLong("CrashAvoidSelf", crashSize);
	if (result == NO_ERR)
		DefaultGroundVehicleCrashAvoidSelf = crashSize;

	result = sysFile->readIdLong("CrashAvoidPath", crashSize);
	if (result == NO_ERR)
		DefaultGroundVehicleCrashAvoidPath = crashSize;

	result = sysFile->readIdLong("CrashBlockSelf", crashSize);
	if (result == NO_ERR)
		DefaultGroundVehicleCrashBlockSelf = crashSize;

	result = sysFile->readIdLong("CrashBlockPath", crashSize);
	if (result == NO_ERR)
		DefaultGroundVehicleCrashBlockPath = crashSize;

	float crashYield = 0.0;
	result = sysFile->readIdFloat("CrashYieldTime", crashYield);
	if (result == NO_ERR)
		DefaultGroundVehicleCrashYieldTime = crashYield;

	result = sysFile->readIdFloat("SweeperSlowTime", gvSweepTime);
	if (result != NO_ERR)
		return(result);

	result = sysFile->readIdFloat("WalkSpeed", gvWalkSpeed);
	if (result != NO_ERR)
		return(result);

	result = sysFile->readIdFloat("HillSpeedFactor", gvHillSpeedFactor);
	if (result != NO_ERR)
		return(result);

	return(NO_ERR);
}

//---------------------------------------------------------------------------

void GroundVehicle::handleStaticCollision (void)
{
	if (getTangible() && pathLocks) 
	{
		//-----------------------------------------------------
		// What is our block and vertex number?
		int blockNumber = 0;
		int vertexNumber = 0;
		
		getBlockAndVertexNumber(blockNumber,vertexNumber);

		int numCollidables = ObjectManager->getObjBlockNumCollidables(blockNumber);
		int terrainObjHandle = ObjectManager->getObjBlockFirstHandle(blockNumber);
		int colliderBlockNumber = -1;
		int colliderVertexNumber = -1;
		for (int i = 0; i < numCollidables; i++) 
		{
			GameObjectPtr terrainObj = ObjectManager->get(terrainObjHandle + i);
			bool isTangible = false;
			switch (terrainObj->getObjectClass()) 
			{
				case TREE:
				case TREEBUILDING:
				case TERRAINOBJECT:
				case BUILDING:
					terrainObj->getBlockAndVertexNumber(colliderBlockNumber, colliderVertexNumber);
					isTangible = terrainObj->getTangible();
					break;
					
				case CAMERADRONE:
				case MINE:
					break;
			}
			
			if (isTangible)
				ObjectManager->detectStaticCollision(this, terrainObj);
		}
	}
}	

//---------------------------------------------------------------------------

void GroundVehicle::init (bool create) {

	longName[0] = 0;

	//Mover::init();
	objectClass = GROUNDVEHICLE;

	numBodyLocations = NUM_GROUNDVEHICLE_LOCATIONS;
	numArmorLocations = NUM_GROUNDVEHICLE_LOCATIONS;

	accel = 0.0;
	velocityMag = 0.0;

	movementEnabled = true;

	turretRotation = 0;
	turretEnabled = true;
	turretBlownThisFrame = false;

	deadByCrushed = false;
	

	pathLocks = true;
	
	captureable = false;
	salvageVehicle = false;
	refitBuddyWID = 0;
	recoverBuddyWID = 0;
	canRefit = false;
	refitting = false;
	canRecover = false;
	recovering = false;
	sweepTime = -1;
	mineLayer = 0;
	aerospaceSpotter = false;
	blipFrame = 0;
	cellColToMine = cellRowToMine = -1;
	timeInCurrent = 0.0;
	
	sensorSystem = nullptr;			//Make sure they start WITHOUT one or badness will insue
	
	isVehiclePilot = false;

	lowestWeaponNodeID = -2;

	dVel.Zero();
	dRot.Zero();
	dAcc.Zero();
};

//------------------------------------------------------------------------------------------

void GroundVehicle::init (bool create, ObjectTypePtr objType) {

	//-------------------------------------------------------------
	// Call down the chain to init everybody else.
	GameObject::init(create, objType);
	creationTime = scenarioTime;
	
	//-------------------------------------------------------------
	// In here will reside the code which initializes all of the
	// variables for a ground vehicle based upon the type.
	// For starters, these are tangible objects
	setTangible(true);

	//----------------------------------------------------
	// Set some mech traits based upon the vehicle Type...
	GroundVehicleTypePtr vehicleT = (GroundVehicleTypePtr)objType;
	for (long curLocation = 0; curLocation < NUM_GROUNDVEHICLE_LOCATIONS; curLocation++) {
		body[curLocation].maxInternalStructure = vehicleT->maxInternalStructure[curLocation];
		body[curLocation].CASE = false;
		body[curLocation].damageState = IS_DAMAGE_NONE;
		body[curLocation].totalSpaces = 0;
	}

	chassis = vehicleT->chassis;

	crashAvoidSelf = vehicleT->crashAvoidSelf;
	crashAvoidPath = vehicleT->crashAvoidPath;
	crashBlockSelf = vehicleT->crashBlockSelf;
	crashBlockPath = vehicleT->crashBlockPath;
	crashYieldTime = vehicleT->crashYieldTime;
	setMoveType(vehicleT->moveType);
	if (vehicleT->hoverCraft)
		moveLevel = 1;
	setFollowRoads(true);

	canRefit = (vehicleT->refitPoints > 0);
	canRecover = (vehicleT->recoverPoints > 0);
	captureable = captureable || (vehicleT->resourcePoints > 0);
	if (captureable)
		setFlag(OBJECT_FLAG_CAPTURABLE,true);

	mineSweeper = vehicleT->mineSweeper;
	mineLayer = vehicleT->mineLayer;
	aerospaceSpotter = vehicleT->aerospaceSpotter;

	nullSignature = (vehicleT->isSensorContact ? 255 : 64);
	
	//-------------------------------------------------------------
	// The control Class starts up here.
	control.init();

	//-------------------------------------------------------------
	// The dynamics class starts up here.
	dynamics = ((GroundVehicleTypePtr)objType)->dynamics;
	accel = dynamics.max.groundVehicle.accel;

	//-------------------------------------------------------------
	// The appearance is initialized here using data from the type
	char *appearanceName = objType->getAppearanceTypeName();

	//--------------------------------------------------------------
	// New code!!!
	// We need to append the sprite type to the appearance num now.
	// The MechEdit tool does not assume a sprite type, nor should it.
	// MechCmdr2 features much simpler objects which only use 1 type of sprite!
	long appearanceType = (GV_TYPE << 24);

	AppearanceTypePtr vehicleAppearanceType = appearanceTypeList->getAppearance(appearanceType,appearanceName);
	if (!vehicleAppearanceType)
	{
		//---------------------------------------------------------
		// Load a dummy appearance until all vehicles ready.
		// For now, the APC!
		vehicleAppearanceType = appearanceTypeList->getAppearance(appearanceType,"APC");
	}

	if ((appearanceType>>24) == GV_TYPE)
	{
		appearance = new GVAppearance;
		if (!appearance)
			Fatal(0, " GroundVehicle.init: unable to create appearance ");

		//-----------------------------------------------------------------
		// The only kind of appearanceType the vehicless currently know how
		// to work with is a spriteTree.  Anything else is wrong.
	   	appearance->init((GVAppearanceType*)vehicleAppearanceType, (GameObjectPtr)this);
		appearance->setAlphaValue(alphaValue);
	}

	objectClass = GROUNDVEHICLE;
	markDistanceMoved = 1000.0;

	pathLocks = ((GroundVehicleTypePtr)objType)->pathLocks;
	
	setObscured(true);
}

//----------------------------------------------------------------------------------

void GroundVehicle::setControl (ControlType ctrlType) {

	control.init(ctrlType, CONTROL_DATA_GROUNDVEHICLE);
}

//----------------------------------------------------------------------------------
long GroundVehicle::init (FitIniFile* vehicleFile) 
{
	const char* BodyLocationBlockString[NUM_GROUNDVEHICLE_LOCATIONS] = {
		"Front",
		"Left",
		"Right",
		"Rear",
		"Turret"
	};

	//--------------------------
	// Read in the vehicle data.
	long result = vehicleFile->seekBlock("ObjectType");
	if (result != NO_ERR)
		return(result);

	name[0] = '\0';

	result = vehicleFile->seekBlock("General");
	if (result != NO_ERR)
		return(result);

	result = vehicleFile->readIdInt( "IconIndex", iconPictureIndex);
	if ( result != NO_ERR )
		iconPictureIndex = 0;


	result = vehicleFile->readIdFloat("CurTonnage", tonnage);
	if (result != NO_ERR)
		return(result);

	result = vehicleFile->readIdInt("DescIndex", descID);
	if (result != NO_ERR)
		descID = -1;
	else
	{
		cLoadString( descID, name, MAXLEN_MOVER_NAME );
		char buffer[256];

		cLoadString( IDS_MFDVEHICLE_PRINTSTRING, buffer, 256 );

		sprintf( longName, buffer, name, tonnage );

	}

	
	
	char cStatus = 0;
	result = vehicleFile->readIdChar("Status", cStatus);
	if (result != NO_ERR)
		return(result);
	status = cStatus;

	result = vehicleFile->readIdInt("BattleRating", battleRating);
	if (result != NO_ERR)
		battleRating = -1;

	maxMoveSpeed = dynamics.max.groundVehicle.speed;

	//------------------------------------------------------------------------------
	// Build the vehicle's inventory (all components, and where they are located)...
	result = vehicleFile->seekBlock("InventoryInfo");
	if (result != NO_ERR)
		return(result);

	result = vehicleFile->readIdUChar("NumOther", numOther);
	if (result != NO_ERR)
		return(result);

	result = vehicleFile->readIdUChar("NumWeapons", numWeapons);
	if (result != NO_ERR)
		return(result);

	result = vehicleFile->readIdUChar("NumAmmo", numAmmos);
	if (result != NO_ERR)
		return(result);

	//--------------------------------------------------------
	// Read in the vehicle's non-weapon/non-ammo components...
	long curItem = 0;
	while (curItem < numOther) 
	{
		char itemString[128];
		sprintf(itemString, "Item:%d", curItem);
		result = vehicleFile->seekBlock(itemString);
		if (result != NO_ERR)
			return(result);

		result = vehicleFile->readIdUChar("MasterID", inventory[curItem].masterID);
		if (result != NO_ERR)
			return(result);
		inventory[curItem].health = MasterComponent::masterList[inventory[curItem].masterID].getHealth();
		inventory[curItem].disabled = false;
		inventory[curItem].amount = 1;
		inventory[curItem].ammoIndex = -1;
		inventory[curItem].readyTime = 0.0;
		inventory[curItem].bodyLocation = 255;

		//------------------------------------------------------------
		// Let's keep track of a few special components we may need...
		switch (MasterComponent::masterList[inventory[curItem].masterID].getForm()) 
		{
			case COMPONENT_FORM_COCKPIT:
				cockpit = curItem;
				break;
			case COMPONENT_FORM_SENSOR:
				sensor = curItem;
				sensorSystem = SensorManager->newSensor();
				sensorSystem->setOwner(this);
				sensorSystem->setRange(MasterComponent::masterList[inventory[sensor].masterID].getSensorRange());
				break;
			case COMPONENT_FORM_ENGINE:
				engine = curItem;
				break;
			case COMPONENT_FORM_LIFESUPPORT:
				lifeSupport = curItem;
				break;
			case COMPONENT_FORM_ECM:
				ecm = curItem;
				//if (sensor != 255)
				//	SensorManager->addEcm(this, MasterComponent::masterList[inventory[sensor].masterID].getSensorRange());
				break;
			case COMPONENT_FORM_PROBE:
				probe = curItem;
				break;
		}

		curItem++;
	}

	//---------------------------------
	// Read in the vehicle's weapons...
	while (curItem < (numOther + numWeapons)) 
	{
		char itemString[128];
		sprintf(itemString, "Item:%d", curItem);
		result = vehicleFile->seekBlock(itemString);
		if (result != NO_ERR)
			return(result);

		result = vehicleFile->readIdUChar("MasterID", inventory[curItem].masterID);
		if (result != NO_ERR)
			return(result);

		inventory[curItem].health = MasterComponent::masterList[inventory[curItem].masterID].getHealth();
		inventory[curItem].disabled = false;
		inventory[curItem].amount = 1;
		inventory[curItem].facing = 1;
		inventory[curItem].ammoIndex = -1;
		inventory[curItem].readyTime = 0.0;
		inventory[curItem].bodyLocation = 255;
		inventory[curItem].effectiveness = (short)(MasterComponent::masterList[inventory[curItem].masterID].getWeaponDamage() * 10.0 /	// damage over 10 seconds
										   MasterComponent::masterList[inventory[curItem].masterID].getWeaponRecycleTime());
		inventory[curItem].effectiveness *= WeaponRanges[MasterComponent::masterList[inventory[curItem].masterID].getWeaponRange()][1] / 24;

		curItem++;
	}

	//------------------------------
	// Read in the vehicle's ammo...
	while (curItem < (numOther + numWeapons + numAmmos)) 
	{
		char itemString[128];
		sprintf(itemString, "Item:%d", curItem);
		result = vehicleFile->seekBlock(itemString);
		if (result != NO_ERR)
			return(result);

		result = vehicleFile->readIdUChar("MasterID", inventory[curItem].masterID);
		if (result != NO_ERR)
			return(result);

		//-----------------------------------------------------------------------
		// Ammo is always full.
		inventory[curItem].amount = MasterComponent::masterList[inventory[curItem].masterID].getAmmoPerTon();

		inventory[curItem].startAmount = inventory[curItem].amount;
		inventory[curItem].ammoIndex = -1;
		inventory[curItem].health = MasterComponent::masterList[inventory[curItem].masterID].getHealth();
		inventory[curItem].disabled = false;
		inventory[curItem].readyTime = 0.0;
		inventory[curItem].bodyLocation = 255;

		curItem++;
	}

	//------------------------------------------------------------
	// Now, read in the component layout for each body location...
	for (long curLocation = 0; curLocation < NUM_GROUNDVEHICLE_LOCATIONS; curLocation++) 
	{

		result = vehicleFile->seekBlock(BodyLocationBlockString[curLocation]);
		if (result != NO_ERR)
			return(result);

		body[curLocation].CASE = false;

		unsigned char internalStructure;
		result = vehicleFile->readIdUChar("CurInternalStructure", internalStructure);
		if (result != NO_ERR)
			return(result);

		body[curLocation].curInternalStructure = internalStructure;

		//---------------------------------------------------------
		// Now, determine the damage state for the body location...
		float damageLevel = body[curLocation].curInternalStructure / body[curLocation].maxInternalStructure;
		if (damageLevel == 0.0)
			body[curLocation].damageState = IS_DAMAGE_DESTROYED;
		else if (damageLevel <= 0.5)
			body[curLocation].damageState = IS_DAMAGE_PARTIAL;
		else
			body[curLocation].damageState = IS_DAMAGE_NONE;

		result = vehicleFile->readIdUChar("MaxArmorPoints", armor[curLocation].maxArmor);
		if (result != NO_ERR)
			return(result);

		unsigned char armorPts;

		result = vehicleFile->readIdUChar("CurArmorPoints", armorPts);
		if (result != NO_ERR)
			return(result);

		armor[curLocation].curArmor = armorPts;

		body[curLocation].totalSpaces = 0;
	}

	calcAmmoTotals();

	for (long item = numOther; item < (numOther + numWeapons); item++) 
	{
		//----------------------------------------------------------
		// Each weapon should point to its appropriate ammo total in
		// the ammo type total list...
		for (long ammoIndex = 0; ammoIndex < numAmmoTypes; ammoIndex++)
		{
			if ((long)MasterComponent::masterList[inventory[item].masterID].getWeaponAmmoMasterId() == ammoTypeTotal[ammoIndex].masterId) 
			{
				inventory[item].ammoIndex = ammoIndex;
				break;
			}
		}
	}

	//------------------------------------------------------------------------------
	// Now that we've loaded inventory, let's set aside which weapon has the longest
	// range...
	calcWeaponEffectiveness(true);
	calcWeaponEffectiveness(false);
	calcFireRanges();

	maxCV = calcCV(true);
	curCV = calcCV(false);
	setThreatRating(-1);

	if (canRefit) 
	{
		GroundVehicleTypePtr objType = (GroundVehicleTypePtr)ObjectManager->getObjectType(typeHandle);
		armor[GROUNDVEHICLE_LOCATION_TURRET].curArmor = armor[GROUNDVEHICLE_LOCATION_TURRET].maxArmor =
		(objType)->refitPoints;
	}
	if (canRecover) 
	{
		GroundVehicleTypePtr objType = (GroundVehicleTypePtr)ObjectManager->getObjectType(typeHandle);
		armor[GROUNDVEHICLE_LOCATION_TURRET].curArmor = armor[GROUNDVEHICLE_LOCATION_TURRET].maxArmor =
		(objType)->recoverPoints;
	}
	if ( mineLayer )
	{
		GroundVehicleTypePtr objType = (GroundVehicleTypePtr)ObjectManager->getObjectType(typeHandle);
		armor[GROUNDVEHICLE_LOCATION_TURRET].curArmor = armor[GROUNDVEHICLE_LOCATION_TURRET].maxArmor =
		(objType)->mineLayer;
	}

	return(NO_ERR);
}

//---------------------------------------------------------------------------

long GroundVehicle::calcCV (bool calcMax) {

	if (battleRating != -1)
		return battleRating;

	//----------------------------------------
	// First, total the CV's of all weapons...
	float weaponBR = 0.0;
	for (long weaponIndex = numOther; weaponIndex < (numOther + numWeapons); weaponIndex++)
		if (calcMax || !inventory[weaponIndex].disabled)
			weaponBR += MasterComponent::masterList[inventory[weaponIndex].masterID].getCV();

	//----------------------------------------------------------
	// Now, multiply weapon BR by the speed factor. Speed Factor
	// is based upon a standard rate of 18m/s...
	weaponBR *= (1.0 + (maxMoveSpeed - 18.0) / 18.0);

	//------------------------------
	// Now, calc the defensive BR...
	float defensiveBR = 0.0;

	//-------------
	// IS Factor...
	for (long bodyLocation = 0; bodyLocation < NUM_GROUNDVEHICLE_LOCATIONS; bodyLocation++)
		defensiveBR += (calcMax ? body[bodyLocation].maxInternalStructure : body[bodyLocation].curInternalStructure);

	//----------------
	// Armor Factor...
	for (long armorLocation = 0; armorLocation < NUM_GROUNDVEHICLE_LOCATIONS; armorLocation++)
		defensiveBR += (calcMax ? armor[armorLocation].maxArmor : armor[armorLocation].curArmor);

	//----------------------
	// Add Tonnage Factor...
	defensiveBR += getTonnage();

	//-------------------
	// Movement Factor...
    int i = 0;
	for (; i < 5; i++)
		if (maxMoveSpeed <= TargetMoveModifierTable[i][0])
			break;
	defensiveBR += (TargetMoveModifierTable[i][1] * 10);


	//---------------
	// Heat Factor...
//	float heatFactor = 0;
//	for (weaponIndex = numOther; weaponIndex < (numOther + numWeapons); weaponIndex++)
//		if (calcMax || !inventory[weaponIndex].disabled)
//			heatFactor -= inventory[weaponIndex].heatPerSec;
//	heatFactor *= 10.0;
//	if (heatFactor > 0.0)
//		heatFactor = 0.0;
//	defensiveBR += heatFactor;

	//--------------------------------
	// Any other special Components..?
	for (long itemIndex = 0; itemIndex < numOther; itemIndex++)
		if (calcMax || !inventory[itemIndex].disabled)
			defensiveBR += MasterComponent::masterList[inventory[itemIndex].masterID].getCV();

	return((long)(weaponBR + defensiveBR));
}

//-------------------------------------------------------------------------------------------

void GroundVehicle::pilotingCheck (void) {

	return;
}

//-------------------------------------------------------------------------------------------

void GroundVehicle::destroy (void) 
{
	if (appearance)
	{
		delete appearance;
		appearance = nullptr;
	}
}

//------------------------------------------------------------------------------------------

void GroundVehicle::mineCheck (void) {

	if (MPlayer && !MPlayer->isServer())
		return;

	unsigned long mine = 0;
	mine = GameMap->getMine(cellPositionRow, cellPositionCol);

	if (mine == 1)
	{
		//----------------------------------------------------
		// Mark the tiles around as potentially mine occupied.
		// NOT NEEDED ANYMORE.  EVERYONE CAN SEE MINES!!!!!!!!!!!

		if (mineSweeper)
		{
			sweepTime = 0;
			Stuff::Vector3D curPos = getPosition();
			ObjectManager->createExplosion(MINE_EXPLOSION_ID, nullptr, curPos);
			armor[GROUNDVEHICLE_LOCATION_FRONT].curArmor--;
			if (MPlayer) 
			{
				WeaponShotInfo shot;
				shot.init(0, -2, 1.0, GROUNDVEHICLE_LOCATION_FRONT, 0);
				MPlayer->addWeaponHitChunk(this, &shot);
			}
			
			if (armor[GROUNDVEHICLE_LOCATION_FRONT].curArmor == 0)
			{
				mineSweeper = false;
				sweepTime = -1;
				pilot->clearCurTacOrder();	// stop!!!
			}

			//---------------------------------------------------------
			// Mark this tile as empty.  (we just set the mine off!!)
			GameMap->setMine(cellPositionRow, cellPositionCol, 2);
			
			if (MPlayer) 
			{
				gosASSERT("Fix more multiplyer--make sure position of mine is right!");
				//long tilePos[4];
				//getTileCellPosition(tilePos[0], tilePos[1], tilePos[2], tilePos[3]);
				//MPlayer->addMineChunk(tilePos[2],
				//					  tilePos[3],
				//					  1,
				//					  2,
				//					  1);
			}
		}
		else if (mineLayer == 0)
		{
			//---------------------------
			// Mine here, deal with it...
			if (tonnage > 35.0f)
			{
				Stuff::Vector3D curPos;
				curPos = getPosition();
				ObjectManager->createExplosion(MINE_EXPLOSION_ID, nullptr, curPos, MineSplashDamage, MineSplashRange * worldUnitsPerMeter);
				WeaponShotInfo shot;
				shot.init(0, -2, MineDamage, calcHitLocation(nullptr,-1,ATTACKSOURCE_MINE,0), 0);
				handleWeaponHit(&shot, (MPlayer != nullptr));
					
				pilot->pausePath();		//Force the pilot to recalc based on new data.
	
				//---------------------------------------------------------
				//  Mark this tile as empty.  (we just set the mine off!!)			
				GameMap->setMine(cellPositionRow, cellPositionCol, 2);
				
				if (MPlayer) 
				{
					gosASSERT("Fix position!");
					//long tilePos[4];
					//getTileCellPosition(tilePos[0], tilePos[1], tilePos[2], tilePos[3]);
					//MPlayer->addMineChunk(tilePos[2],
					//					  tilePos[3],
					//					  1,
					//					  2,
					//					  2);
				}
			}
		}
	}
}

//---------------------------------------------------------------------------

long GroundVehicle::setTeamId (long _teamId, bool setup) 
{
	long result = Mover::setTeamId(_teamId,setup);

	//If we are a resource truck, add resources to our side.
	if ((turn > 10) && ((GroundVehicleTypePtr)getObjectType())->resourcePoints && (_teamId == Team::home->getId()))
	{
		LogisticsData::instance->addResourcePoints(((GroundVehicleTypePtr)getObjectType())->resourcePoints);
		soundSystem->playBettySample(BETTY_RESOURCES);
	}

	return result;	
}
