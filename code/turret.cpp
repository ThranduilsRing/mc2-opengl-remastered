#include "tick_commit.h"
//***************************************************************************
//
//	turret.cpp -- File contains the Turret Object code
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef MCLIB_H
#include <algorithm>
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include"mclib.h"
#endif
#include "warrior.h"

#ifndef TURRET_H
#include"turret.h"
#endif

#ifndef GAMESOUND_H
#include"gamesound.h"
#endif

#ifndef SOUNDS_H
#include"sounds.h"
#endif

#ifndef MOVE_H
#include"move.h"
#endif

#ifndef COLLSN_H
#include"collsn.h"
#endif

#ifndef MECH_H
#include"mech.h"
#endif

#ifndef GVEHICL_H
#include"gvehicl.h"
#endif

#ifndef CARNAGE_H
#include"carnage.h"
#endif

#ifndef TEAM_H
#include"team.h"
#include"tacticaloverview.h"  // maskWorldBars: hide health bars in overview
#endif

#ifndef WEAPONFX_H
#include"weaponfx.h"
#endif

#ifndef WEAPONBOLT_H
#include"weaponbolt.h"
#endif

#ifndef MULTPLYR_H
#include"multplyr.h"
#endif

#ifndef MISSION_H
#include"mission.h"
#endif

#ifndef GAMELOG_H
#include"gamelog.h"
#endif

#include "../resource.h"
#include "../RenderCore/axis_convert.h"
#include "mc2fmt.h"
//***************************************************************************

//extern float worldUnitsPerMeter;
//extern bool useSound;
//extern BOOL useOldProject;
//extern BOOL drawExtents;

extern float WeaponRange[NUM_FIRERANGES];
extern float WeaponRanges[NUM_WEAPON_RANGE_TYPES][2];
extern float WeaponFireModifiers[NUM_WEAPONFIRE_MODIFIERS];
extern float MaxVisualRadius;

//extern long ClusterSizeSRM;
//extern long ClusterSizeLRM;
//extern float				MaxVisualRadius;
//extern float				MetersPerCell;

extern GameLog* CombatLog;

extern void DebugWeaponFireChunk (WeaponFireChunkPtr chunk1, WeaponFireChunkPtr chunk2, GameObjectPtr attacker);
extern void LogWeaponFireChunk (WeaponFireChunkPtr chunk, GameObjectPtr attacker, GameObjectPtr target);

//long cLoadString (HINSTANCE hInstance,  UINT uID, LPTSTR lpBuffer, int nBufferMax );

bool Turret::turretsEnabled[MAX_TEAMS] = {true, true, true, true, true, true, true, true};

constexpr float MAX_TURRET_PITCH = -45.0f;

inline float agsqrt( float _a, float _b )
{
	return sqrt(_a*_a + _b*_b);
}

//***************************************************************************
// TURRET TYPE class
//***************************************************************************

GameObjectPtr TurretType::createInstance (void) {

	TurretPtr newTurret = new Turret;
	if (!newTurret)
		return(nullptr);

	newTurret->init(true, this);

	return(newTurret);
}

//---------------------------------------------------------------------------

void TurretType::destroy (void) 
{
	ObjectType::destroy();
}
		
//---------------------------------------------------------------------------

long TurretType::init (FilePtr objFile, unsigned long fileSize) {

	long result = 0;
	
	FitIniFile bldgFile;
	result = bldgFile.open(objFile, fileSize);
	if (result != NO_ERR)
		return(result);
	
	//------------------------------------------------------------------
	// Read in the data needed for the TreeBuilding
	result = bldgFile.seekBlock("TurretData");
	if (result != NO_ERR)
		return(result);

	result = bldgFile.readIdFloat("LOSFactor",LOSFactor);
	if (result != NO_ERR)
		LOSFactor = 1.0f;
		
 	unsigned long dmgLevel;
	result = bldgFile.readIdULong("DmgLevel",dmgLevel);
	if (result != NO_ERR)
		return(result);
	damageLevel = static_cast<float>(dmgLevel);

	bldgFile.readIdULong("BlownEffectId",blownEffectId);
	if (result != NO_ERR)
		blownEffectId = -1;
		
	bldgFile.readIdULong("NormalEffectId",normalEffectId);
	if (result != NO_ERR)
		normalEffectId = -1;
		
	bldgFile.readIdULong("DamageEffectId",damageEffectId);
	if (result != NO_ERR)
		damageEffectId = -1;

	result = bldgFile.readIdFloat("ExplosionRadius",explRad);
	if (result != NO_ERR)
		explRad = 0.0;
		
	result = bldgFile.readIdFloat("ExplosionDamage",explDmg);
	if (result != NO_ERR)
		explDmg = 0.0;

	result = bldgFile.readIdFloat("Tonnage", baseTonnage);
	if (result != NO_ERR)
		baseTonnage = 20;
	
	result = bldgFile.readIdFloat("AttackRadius", engageRadius);
	if (result != NO_ERR)
		return result;
		
	result = bldgFile.readIdFloat("MaxTurretYawRate", turretYawRate);
	if (result != NO_ERR)
		return result;
		
	result = bldgFile.readIdLong("WeaponType", weaponMasterId[0]);
	if (result != NO_ERR)
		return(result);

	//SPOTLIGHTS!!!!
	if (weaponMasterId[0] != -1)
		punch = MasterComponent::masterList[weaponMasterId[0]].getCV();
	else
		punch = 0.0f;
		
	result = bldgFile.readIdLong("WeaponType1", weaponMasterId[1]);
	if (result != NO_ERR)
		weaponMasterId[1] = -1;
	else
		punch += MasterComponent::masterList[weaponMasterId[1]].getCV();

	result = bldgFile.readIdLong("WeaponType2", weaponMasterId[2]);
	if (result != NO_ERR)
		weaponMasterId[2] = -1;
	else
		punch += MasterComponent::masterList[weaponMasterId[2]].getCV();
	
	result = bldgFile.readIdLong("WeaponType3", weaponMasterId[3]);
	if (result != NO_ERR)
		weaponMasterId[3] = -1;
	else
		punch += MasterComponent::masterList[weaponMasterId[3]].getCV();

	result = bldgFile.readIdLong("PilotSkill", pilotSkill);
	if (result != NO_ERR)
		return(result);
		
	result = bldgFile.readIdFloat("LittleExtent",littleExtent);
	if (result != NO_ERR)
		littleExtent = 20.0;

	result = bldgFile.readIdLong ("BuildingName", turretTypeName);
	if (result != NO_ERR)
		turretTypeName = IDS_TRTOBJ_NAME;

	result = bldgFile.readIdLong( "BuildingDescription", buildingDescriptionID );
		if ( result != NO_ERR )
			buildingDescriptionID = -1;

	//------------------------------------------------------------------
	// Initialize the base object Type from the current file.
	result = ObjectType::init(&bldgFile);
	return(result);
}
	
//---------------------------------------------------------------------------

bool TurretType::handleCollision (GameObjectPtr collidee, GameObjectPtr collider) {

	if (MPlayer && !MPlayer->isServer())
		return(true);

	//-------------------------------------------------------
	// The handleCollision routine checks the current target
	// against the thing collided with and compares distance.
	// The closest of the two becomes the new target.
	TurretPtr turret = static_cast<TurretPtr>(collidee);
	GameObjectPtr target = ObjectManager->getByWatchID(turret->targetWID);
	float targetRange = 10000000.0;
	if (target) {
		glm::vec3 targetRangeV3;
		targetRangeV3 = turret->getPosition() - target->getPosition();
		targetRange = targetRangeV3.x * targetRangeV3.x + targetRangeV3.y * targetRangeV3.y;								//().magnitude();
	}

	//-------------------------------------
	// IMPORTANT MULTIPLAYER NOTE!!!!!!!!!!
	//-------------------------------------
	// Turrets may only target movers and
	// camera drones. NOTHING ELSE!
	// Anything else would cause BIG
	// BADNESS!
	//-------------------------------------

	// Turrets can check for neutral.  Turrets must also check for allied team member in GAME
	// THIS IS NOT DONE YET!
	if ((turret->getFlag(OBJECT_FLAG_POP_NEUTRALS) && collidee->isNeutral(collider)) || collidee->isEnemy(collider))
	{
		//----------------------------------------------
		// Not on my side.  Shoot her!!!
		switch (collider->getObjectClass())
		{
			case BATTLEMECH:
			case GROUNDVEHICLE:
			case ELEMENTAL:
			{
				if (collider->isDisabled() || collider->isDestroyed())
				{
					//--------------------------------------
					// Don't shoot, its dead!!!
				}
				else
				{
					glm::vec3 colliderRangeV3;
					colliderRangeV3 = turret->getPosition() - collider->getPosition();
					float colliderRange = colliderRangeV3.x * colliderRangeV3.x + colliderRangeV3.y * colliderRangeV3.y;		//().magnitude();
					if (colliderRange < targetRange)
						turret->targetWID = collider->getWatchID();
				}
			}
			break;
			
			case CAMERADRONE:
			{
				if (collider->isDestroyed())
				{
					//--------------------------------------
					// Don't shoot, its dead!!!
				}
				else
				{
					glm::vec3 colliderRangeV3;
					colliderRangeV3 = turret->getPosition() - collider->getPosition();
					float colliderRange = colliderRangeV3.x * colliderRangeV3.x + colliderRangeV3.y * colliderRangeV3.y;		//().magnitude();
					if (colliderRange < targetRange)
						turret->targetWID = collider->getWatchID();
				}
			}
			break;
		}
	}
	
	
	return(true);
}

//---------------------------------------------------------------------------

bool TurretType::handleDestruction (GameObjectPtr collidee, GameObjectPtr collider) {

	return(false);
}

//---------------------------------------------------------------------------
// class Turret
//---------------------------------------------------------------------------

void Turret::init (bool create) {

	setFlag(OBJECT_FLAG_JUSTCREATED, true);
	setFlag(OBJECT_FLAG_CAN_FIRE, true);
	setFlag(OBJECT_FLAG_ONFIRE, false);
	setFlag(OBJECT_FLAG_CHECKED_ONSCREEN, false);

	adoptAppearance(nullptr);
			
	vertexNumber = 0;
	blockNumber = 0;
		
	idleWait = RandomNumber(10);

	didReveal = 0;
	
	turretRotation = 0.0;
			
	targetWID = 0;

	pointLight = nullptr;

	maxRange = 0.0f;
	
	netRosterIndex = -1;
	numWeaponFireChunks[0] = 0;
	numWeaponFireChunks[1] = 0;

	parentId = 0xffffffff;
	parent = 0;

	minRange = 0.0;
	maxRange = 0.0;
	numFunctionalWeapons = 0;
}

//---------------------------------------------------------------------------

long Turret::setTeamId (long _teamId, bool setup) {
	if (teamId != _teamId) markTerrainPresentationDirty();

	if (MPlayer)
	{
		teamId = _teamId;
	}
	else
	{
		if (_teamId == 2)		//Allies
			teamId = 0;			//Same as PlayerTeam.
		else if (_teamId > 2)
			teamId = 1;			//Not ally.  ENEMY!!
		else
			teamId = _teamId;	//Otherwise we were set to either -1, 0 or 1.
	}
	targetWID = 0;

	static unsigned long highLight[8] = {0x00007f00, 0x007f0000,
										  0x0000007f, 0x0000007f,
										  0x0000007f, 0x0000007f,
										  0x0000007f, 0x0000007f};

	if ((turn > 10) && (teamId > -1) && (teamId < 8))
		appearance->flashBuilding(5.0,0.5,highLight[teamId]);
	
	return(NO_ERR);
}

//---------------------------------------------------------------------------

TeamPtr Turret::getTeam (void) {

	if (teamId == -1)
		return(nullptr);
	return(Team::teams[teamId]);
}

//---------------------------------------------------------------------------

bool Turret::isFriendly (TeamPtr team) {

	if (teamId > -1)
		return(Team::relations[teamId][team->getId()] == RELATION_FRIENDLY);
	return(false);
}

//---------------------------------------------------------------------------

bool Turret::isEnemy (TeamPtr team) {

	if (teamId > -1)
		return(Team::relations[teamId][team->getId()] == RELATION_ENEMY);
	return(false);
}

//---------------------------------------------------------------------------

bool Turret::isNeutral (TeamPtr team) {

	if (teamId > -1)
		return(Team::relations[teamId][team->getId()] == RELATION_NEUTRAL);
	return(true);
}

//---------------------------------------------------------------------------
void Turret::setDamage (float newDamage) 
{
	damage = newDamage;

	TurretTypePtr type = static_cast<TurretTypePtr>(getObjectType());
	if (damage >= type->getDamageLevel())
	{
		setStatus(OBJECT_STATUS_DESTROYED);
		appearance->setObjStatus(OBJECT_STATUS_DESTROYED);
	}
}
 
//---------------------------------------------------------------------------
long Turret::updateAnimations (void)
{
	long canFire = 0;
	
	//---------------------------------------------
	// Animate the turret popping up!
	long animState = appearance->getCurrentGestureId();
	if (getAwake() && targetWID)
	{
		switch (animState)
		{
			case -1:		//NOT UPDATED YET. SWITCH TO ZERO
				if (!appearance->getInTransition())
				{
					appearance->setGesture(0);
				}
				break;
					
			case 2:			//Just Animating.  Do NOTHING!
					canFire = 1;
				break;
					
			case 0:			//Not triggered yet.  Switch to 1
				appearance->setGesture(1);
				break;
				
			case 1:			//triggered, when fully open switch to 2
				if (!appearance->getInTransition())
				{
					appearance->setGesture(2);
					canFire = 1;
				}
				break;
					
			case 3:			//Closing.  Wait until closed and then switch to 1.
				if (!appearance->getInTransition())
				{
					appearance->setGesture(1);
				}
				break;
			}
		}
	else if (!getAwake() || !targetWID)
	{
		switch (animState)
		{
			case -1:		//NOT UPDATED YET. SWITCH TO ZERO
				if (!appearance->getInTransition())
				{
					appearance->setGesture(0);
				}
				break;
				
			case 2:			//Just Animating.  Wait until one loop done, then trigger closing
				appearance->setGesture(3);
				break;
				
			case 0:			//Not triggered yet.  DO NOTHING!
				break;
				
			case 1:			//triggered, when fully open switch to 3 to close it.
				if (!appearance->getInTransition())
				{
					appearance->setGesture(3);
				}
				break;
				
			case 3:		//Closing to closed.  When Closed, DO NOTHING
				if (!appearance->getInTransition())
				{
					appearance->setGesture(0);
				}
				break;		
		}
	}
	
	return canFire;
}
	
//---------------------------------------------------------------------------

long Turret::update (void)
{
	if (getFlag(OBJECT_FLAG_JUSTCREATED)) 
	{
		setFlag(OBJECT_FLAG_JUSTCREATED, false);

		//Since we are idling, pick a new random position to point to.
		if (RollDice(50))
			idlePosition.x = position.x + RandomNumber(500);
		else
			idlePosition.x = position.x - RandomNumber(500);
	   
		if (RollDice(50))
			idlePosition.y = position.y + RandomNumber(500);
		else
			idlePosition.y = position.y - RandomNumber(500);
		   
		idlePosition.z = position.z;
		oldPosition = idlePosition;

		//-----------------------------------------------------
		// Check if ParentId is NOT 0xffffffff.
		// if not, find parent in ObjMgr and get its pointer.
		if ((parentId != 0xffffffff) && (parentId != 0))
		{
			parent = ObjectManager->findByCellPosition((parentId>>16),(parentId & 0x0000ffff))->getWatchID();
			ObjectManager->getByWatchID(parent)->setFlag(OBJECT_FLAG_CAPTURABLE, true);
			gosASSERT(parent != 0);
		}
		threatRating = static_cast<short>((static_cast<TurretType*>(getObjectType()))->punch);
		calcFireRanges();
	}

		
	//--------------------------------------
	// Turret is Dead.  React Appropriately
	// Just do not call any of the fire routines
	if (getFlag(OBJECT_FLAG_DESTROYED)) 
	{
		setFlag(OBJECT_FLAG_TANGIBLE, false);
			
		if (pointLight)
		{
            mc2::tick::releaseLight(pointLight, eye, lightId);
		}

		appearance->setObjectParameters(position, rotation, TacticalOverview::maskWorldBars(drawFlags), teamId,Team::getRelation(teamId, Team::home->getId()));
		appearance->setMoverParameters(turretRotation,0.0f,0.0f);
		if (appearance)
		{
			// Bounds, visibility and the visible stamp are presentation work for a simulation-owned appearance.
			if (!appearance->simulationOwned())
			{
				appearance->recalcBounds();
				appearance->setInView(true);
				windowsVisible = turn;
			}
			appearance->update();
			didReveal = true;
		}
		
		return(true);	//NEVER RETURN ANYTHING BUT TRUE!!!!!!!!!!!!!!!!
	}

	// Neutral / environmental turrets have teamId == -1 which would index
	// turretsEnabled[-1] (global-buffer-overflow on ASan; reads whatever
	// the adjacent global holds on normal builds -- observable as neutral
	// turrets occasionally failing to engage). Guard with an explicit
	// non-negative check. Caught by finding #4 in docs/asan-follow-ups.md.
	const long tid = getTeamId();
	if (tid < 0 || !turretsEnabled[tid]) {
		targetWID = 0;
	}

	//--------------------------------------
	// In case the target has been purged...
	if (targetWID && !ObjectManager->getByWatchID(targetWID))
		targetWID = 0;

	float ourExtentRadius = getExtentRadius();
	if (targetWID && (!MPlayer || MPlayer->isServer())) 
	{
		GameObjectPtr attackTarget = ObjectManager->getByWatchID(targetWID);
		if (attackTarget)
		{
			glm::vec3 targetRangeV3;
			targetRangeV3 = attackTarget->getPosition() - position;
			float targetRange = targetRangeV3.x * targetRangeV3.x + targetRangeV3.y * targetRangeV3.y;				//().magnitude();
			if (attackTarget->isDisabled() || 
				attackTarget->isDestroyed() || 
				GameObject::isFriendly(attackTarget) ||
				(targetRange > (ourExtentRadius * ourExtentRadius)))
				targetWID = 0;
		}
		else
			targetWID = 0;
	}

	bool active = getAwake();

	//-----------------------------------------------------------------------------------
	// Old linkage code set awake in ABL.  MUST still support this and new linkage code!
	if (active && parent && 
		(ObjectManager->getByWatchID(parent)->isDestroyed() || 
		 ObjectManager->getByWatchID(parent)->isDisabled() || 
		 !ObjectManager->getByWatchID(parent)->getAwake()))
	{

		ObjectType* parentClass = ObjectManager->getByWatchID(parent)->getObjectType();
		if ( active ) // play this sound regardless of wether its a turret control or generator
		{
			soundSystem->playBettySample( BETTY_GENERATOR_DESTROYED );
		}
		active = false;
		setAwake(false);
		
		appearance->startActivity(TURRET_POWER_DOWN_EFFECT,false);
	}

	//-----------------------------------------------------------------
	// Must also check if parent alive and on same side now internally
	if (active && parent && 
		!ObjectManager->getByWatchID(parent)->isDestroyed() && 
		!ObjectManager->getByWatchID(parent)->isDisabled() && 
		(ObjectManager->getByWatchID(parent)->getTeamId() != getTeamId()))
	{
		// if building recaptured play a sound
		if ((ObjectManager->getByWatchID(parent)->getTeamId() != Team::home->getId()) && (turn > 5) && (getTeamId() != -1))
			soundSystem->playBettySample(BETTY_BUILDING_RECAPTURED);

		setTeamId(ObjectManager->getByWatchID(parent)->getTeam()->getId(),false);
	}

	if ( parent && 
		 (!ObjectManager->getByWatchID(parent)->isDisabled()) && 
		 ObjectManager->getByWatchID(parent)->getTargeted() )
	{
		setTargeted( true );
	}

	float turretFacing = 0.0f;
	float turretTurnRate = 0.0;
	if (active && targetWID) 
	{
		GameObjectPtr target = ObjectManager->getByWatchID(targetWID);
		//----------------------------------
		// Rotate Turret to point at target
		if (target)
			turretFacing = relFacingTo(target->getPosition());

		//--------------------------------------------------------------------
		// Complicated, but let me 'splain.  No there is too much legacy code.
		// Let me sum up.  We need to know what direction to rotate the turret.
		// That direction is the shortest distance between where we are and 
		// where we are going.  For the record, WE HAVE NEVER DONE THIS CORRECTLY!
		if ((turretFacing < -10.0) || (turretFacing > 10.0)) {
			//-----------------------------------------------
			// We can and will shift facing to destination...
			float maxRate = (static_cast<TurretTypePtr>(getObjectType()))->turretYawRate;
			if (turretFacing < 0.0)
			{
				turretTurnRate = maxRate*frameLength;
				turretTurnRate = std::min(turretTurnRate, fabs(turretFacing));
			}
			else
			{
				turretTurnRate = -maxRate*frameLength;
				turretTurnRate = std::max(turretTurnRate, -turretFacing);
			}
			
			setFlag(OBJECT_FLAG_FACING_TARGET,false);
		}
		else
		{
			setFlag(OBJECT_FLAG_FACING_TARGET,true);
		}
		
		turretRotation += turretTurnRate;
	}
	else if (active && !targetWID) 
	{
		//---------------------------------------------------------------
		// Create an Idle animation here for the turrets and Spotlights.
		// Just slowly rotate turret for now.
		turretFacing = relFacingTo(idlePosition);
		
		if ((turretFacing < -10.0) || (turretFacing > 10.0)) {
			//-----------------------------------------------
			// We can and will shift facing to destination...
			float maxRate = (static_cast<TurretTypePtr>(getObjectType()))->turretYawRate;
			if (turretFacing < 0.0)
			{
				turretTurnRate = maxRate*frameLength;
				turretTurnRate = std::min(turretTurnRate, fabs(turretFacing));
			}
			else
			{
				turretTurnRate = -maxRate*frameLength;
				turretTurnRate = std::max(turretTurnRate, -turretFacing);
			}

			setFlag(OBJECT_FLAG_FACING_TARGET,false);
		}
		else
		{
			setFlag(OBJECT_FLAG_FACING_TARGET,true);
			
			idleWait += frameLength;
			if (idleWait > 10.0f)
			{
				//Since we are idling, pick a new random position to point to.
				if (RollDice(50))
					idlePosition.x = position.x + RandomNumber(ourExtentRadius);
				else
					idlePosition.x = position.x - RandomNumber(ourExtentRadius);
				
				if (RollDice(50))
					idlePosition.y = position.y + RandomNumber(ourExtentRadius);
				else
					idlePosition.y = position.y - RandomNumber(ourExtentRadius);
					
				idlePosition.z = position.z;
				
				idleWait = 0.0f;
			}
 		}
		
		turretRotation += turretTurnRate;
	}
	else if (!active && !getFlag(OBJECT_FLAG_VEHICLE_APPR)) 
	{
	}

	if (turretRotation > 360.0f)
		turretRotation -= 360.0f;
			
	if (turretRotation < -360.0f)
		turretRotation += 360.0f;
	
	glm::vec3 distance;
	distance = oldPosition - position;
	float spotDistance = glm::length(distance);
	
	float turretPitch = 0.0f;
	
	if ((static_cast<TurretTypePtr>(getObjectType()))->weaponMasterId[0] == -1)
	{
		turretPitch = (1.0f - (spotDistance / (ourExtentRadius))) * MAX_TURRET_PITCH;
		turretPitch = std::min(turretPitch, 0.0f);
	}

	//--------------------------------------------------------------------------
	// Make the turrets sad if their parent was destroyed, disabled or asleep.
	if (!active)
	{
		turretPitch = -35.0f;
	}
	
	//-------------------------------------------
	// Handle power out.
	// Parent is down OR we are destroyed.
	if (!active || (getStatus() == OBJECT_STATUS_DESTROYED))
		appearance->setLightsOut(true);
		
 	appearance->setObjectParameters(position, rotation, TacticalOverview::maskWorldBars(drawFlags), teamId,Team::getRelation(teamId, Team::home->getId()));
	appearance->setMoverParameters(turretRotation,turretPitch,0.0f);
	bool inView = false;
	long canFire = updateAnimations();

	if (appearance)
	{
		appearance->setInView(true);
		appearance->update();
		windowsVisible = turn;
		didReveal = true;
	}

	if ((static_cast<TurretTypePtr>(getObjectType()))->weaponMasterId[0] == -1)
	{
		//----------------------------------------------
		// Must move light if spotlight is MOVING!
		// Find Direction of movement subtracting target or idle position from oldPosition.
		// find distance from that vector.
		// move distance proportional to rotation this frame divided by rotation left to hit target.
		// Store newly derived position as oldPosition.
		if (turretTurnRate == 0.0f)
		{
			//We are on Target.  Light is at same position as thing we are illuminating.
			GameObjectPtr target = ObjectManager->getByWatchID(targetWID);
			if (target)
				oldPosition = target->getPosition();
			else if (idleWait != 0.0f)
				oldPosition = idlePosition;
				
			oldPosition.z = position.z;
		}
		else
		{
			glm::vec3 Dir;
			GameObjectPtr target = ObjectManager->getByWatchID(targetWID);
			if (target)
			{
				Dir = target->getPosition() - oldPosition;
				float dist = glm::length(Dir);
				if (dist > Stuff::SMALL)
				{
					Dir = glm::normalize(Dir);
					dist *= fabs(turretTurnRate) / fabs(turretFacing);
				
					Dir *= dist;
					oldPosition += Dir;
				}
				else
					oldPosition = target->getPosition();
			}
			else
			{
				Dir = idlePosition - oldPosition;
				float dist = glm::length(Dir);
				if (dist > Stuff::SMALL)
				{
					Dir = glm::normalize(Dir);
					dist *= fabs(turretTurnRate) / fabs(turretFacing);
					
					Dir *= dist;
					oldPosition += Dir;
				}
				else
					oldPosition = idlePosition;
			}
		}
	
	}

	setFlag(OBJECT_FLAG_CAN_FIRE, true);

	for (long i=0;i<MAX_TURRET_WEAPONS;i++)
	{
		if (!getFlag(OBJECT_FLAG_DESTROYED) && targetWID && isWeaponReady(i) && active && canFire)
		{
			GameObjectPtr target = ObjectManager->getByWatchID(targetWID);
			if (target && (!MPlayer || MPlayer->isServer()))
				fireWeapon(target,i);
		}
	}
	
	if (numWeaponFireChunks[CHUNK_RECEIVE] > 0)
		updateWeaponFireChunks(CHUNK_RECEIVE);

	return(1);
}

//---------------------------------------------------------------------------

float Turret::relFacingTo (glm::vec3 goal, long bodyLocation) 
{
	glm::vec3 facingVec;
	facingVec.x = 0.0f;
	facingVec.y = -1.0f;
	facingVec.z = 0.0f;
	Rotate(facingVec, -turretRotation);

	glm::vec3 goalVec;
	goalVec = goal - position;

	float angle = angle_from(facingVec, goalVec);

	//--------------------------------
	// Get sign of relative angle.
	float z = (facingVec.x * goalVec.y) - (facingVec.y * goalVec.x);
	if (z > 0.0f)
		angle = -angle;
	
	return(angle);
}

//---------------------------------------------------------------------------

bool Turret::isWeaponReady (long weaponId) 
{
	if ((static_cast<TurretTypePtr>(getObjectType()))->weaponMasterId[weaponId] == -1)
		return false;
 
	if ((readyTime[weaponId] > scenarioTime) || !getFlag(OBJECT_FLAG_CAN_FIRE) || !getFlag(OBJECT_FLAG_FACING_TARGET))
		return(false);
		
	return(true);
}

//---------------------------------------------------------------------------

bool Turret::isWeaponMissile (long weaponId) 
{
	if ((static_cast<TurretTypePtr>(getObjectType()))->weaponMasterId[weaponId] == -1)
		return false;
		
	return(MasterComponent::masterList[(static_cast<TurretTypePtr>(getObjectType()))->weaponMasterId[weaponId]].getForm() == COMPONENT_FORM_WEAPON_MISSILE);
}

//---------------------------------------------------------------------------

bool Turret::isWeaponStreak (long weaponId) 
{
	if ((static_cast<TurretTypePtr>(getObjectType()))->weaponMasterId[weaponId] == -1)
		return false;
		
	return(MasterComponent::masterList[(static_cast<TurretTypePtr>(getObjectType()))->weaponMasterId[weaponId]].getWeaponStreak());
}

//---------------------------------------------------------------------------

float Turret::calcAttackChance (GameObjectPtr target, long* range, long weaponId) 
{
	if ((static_cast<TurretTypePtr>(getObjectType()))->weaponMasterId[weaponId] == -1)
		return 0.0f;
		
	//-------------------------------------------------------------
	// First, let's find out what kind of object we're targeting...
	glm::vec3 targetPosition(0.0f,0.0f,0.0f);
	BattleMechPtr mech = nullptr;
	GroundVehiclePtr vehicle = nullptr;
	
 	if (target) 
	{
		long targetObjectClass = target->getObjectClass();
		switch (targetObjectClass) 
		{
			case BATTLEMECH:
				mech = static_cast<BattleMechPtr>(target);
				break;
			case GROUNDVEHICLE:
				vehicle = static_cast<GroundVehiclePtr>(target);
				break;
		}
		
		targetPosition = target->getPosition();
	}
	
	float attackChance = (static_cast<TurretTypePtr>(getObjectType()))->pilotSkill;
	//----------------------
	// General fire range...
	float distanceToTarget = distanceFrom(targetPosition);
	if (range) 
	{
		if (distanceToTarget <= WeaponRange[FIRERANGE_SHORT])
			*range = FIRERANGE_SHORT;		
		else if (distanceToTarget <= WeaponRange[FIRERANGE_MEDIUM])
			*range = FIRERANGE_MEDIUM;
		else
			*range = FIRERANGE_LONG;
	}
	
	//----------------------
	// Range (Per Weapon)...
	long weaponMasterId = (static_cast<TurretTypePtr>(getObjectType()))->weaponMasterId[weaponId];
	float weaponMinRange = WeaponRanges[MasterComponent::masterList[weaponMasterId].getWeaponRange()][0];
	float weaponMaxRange = WeaponRanges[MasterComponent::masterList[weaponMasterId].getWeaponRange()][1];

	if (distanceToTarget <= weaponMinRange)
		return(-1.0);
	else if (distanceToTarget > weaponMaxRange)
		return(-1.0);

	//-------------------
	// Target movement...
	if (target) 
	{
		glm::vec3 targetVelocity;
		targetVelocity = target->getVelocity();
		float targetSpeed = targetVelocity.x * targetVelocity.x + targetVelocity.y * targetVelocity.y;
		if (targetSpeed == 0.0)
			attackChance += 50.0;
	}
	
	//Mech specialist modifiers
	if (mech && mech->getPilot())
	{
		if ((mech->tonnage < 40) && mech->getPilot()->isLightMechSpecialist())
			attackChance -= 30.0f;
		else if ((mech->tonnage >= 40) && (mech->tonnage < 60) && mech->getPilot()->isMediumMechSpecialist())
			attackChance -= 20.0f;
		else if ((mech->tonnage >= 60) && (mech->tonnage < 80) && mech->getPilot()->isHevayMechSpecialist())
			attackChance -= 10.0f;
		else if ((mech->tonnage >= 80) && (mech->tonnage < 100) && mech->getPilot()->isAssaultMechSpecialist())
			attackChance -= 10.0f;
			
		attackChance = std::max(attackChance, 5.0f);
	}
 	
	return(attackChance);
}

//------------------------------------------------------------------------------------------

void Turret::recordWeaponFireTime (long weaponId) {

	lastFireTime[weaponId] = scenarioTime;
}

//------------------------------------------------------------------------------------------

void Turret::startWeaponRecycle (long weaponId) 
{
	readyTime[weaponId] = scenarioTime + MasterComponent::masterList[(static_cast<TurretTypePtr>(getObjectType()))->weaponMasterId[weaponId]].getWeaponRecycleTime();
}

//---------------------------------------------------------------------------

long Turret::clearWeaponFireChunks (long which) {

	long numChunks = numWeaponFireChunks[which];
	numWeaponFireChunks[which] = 0;
	return(numChunks);
}

//---------------------------------------------------------------------------

long Turret::addWeaponFireChunk (long which, WeaponFireChunkPtr chunk) {

	if (numWeaponFireChunks[which] == MAX_WEAPONFIRE_CHUNKS)
		Fatal(0, " Turret::addWeaponFireChunk--Too many weaponfire chunks ");

	chunk->pack(this);
	weaponFireChunks[which][numWeaponFireChunks[which]++] = chunk->data;
	return(numWeaponFireChunks[which]);
}

//---------------------------------------------------------------------------

long Turret::addWeaponFireChunks (long which, unsigned long* packedChunkBuffer, long numChunks) {

	if ((numWeaponFireChunks[which] + numChunks) >= MAX_WEAPONFIRE_CHUNKS)
		Fatal(0, " Turret::addWeaponFireChunks--Too many weaponfire chunks ");

#if 0
	memcpy(&weaponFireChunks[which][numWeaponFireChunks[which]], packedChunkBuffer, 4 * numChunks);
	numWeaponFireChunks[which] += numChunks;
#else
	for (long i = 0; i < numChunks; i++) {
		weaponFireChunks[which][numWeaponFireChunks[which]++] = packedChunkBuffer[i];
		//---------------
		// FOR TESTING...
		WeaponFireChunk chunk;
		chunk.init();
		chunk.data = packedChunkBuffer[i];
		chunk.unpack(this);
	}
#endif
	return(numWeaponFireChunks[which]);
}

//---------------------------------------------------------------------------

long Turret::grabWeaponFireChunks (long which, unsigned long* packedChunkBuffer) {

	if (numWeaponFireChunks[which] > 0)
		for (long i = 0; i < numWeaponFireChunks[which]; i++)
			packedChunkBuffer[i] = weaponFireChunks[which][i];
		//memcpy(packedChunkBuffer, weaponFireChunks[which], 4 * numWeaponFireChunks[which]);
	return(numWeaponFireChunks[which]);
}

//---------------------------------------------------------------------------

long Turret::updateWeaponFireChunks (long which) 
{
	for (long i = 0; i < numWeaponFireChunks[which]; i++) 
	{
		WeaponFireChunk chunk;
		chunk.init();
		chunk.data = weaponFireChunks[which][i];
		chunk.unpack(this);

		static float entryQuadTable[4] = {0.0, 180.0, -90.0, 90.0};

		if (chunk.targetType == 0) 
		{
			GameObjectPtr target = static_cast<GameObjectPtr>(MPlayer->moverRoster[chunk.targetId]);
			//----------------------------------------------------------------------------
			// Mover targets could be NULL now, since we free them when they're destroyed.
			if (target) 
				handleWeaponFire(chunk.weaponIndex,
							 target,
							 nullptr,
							 chunk.hit,
							 entryQuadTable[chunk.entryAngle],
							 chunk.numMissiles,
							 chunk.hitLocation);
		}
		else if (chunk.targetType == 1) 
		{
			GameObjectPtr target = ObjectManager->findByPartId(chunk.targetId);
			if (target == nullptr) 
			{
				DebugWeaponFireChunk (&chunk, nullptr, this);
				Assert(FALSE, 0, " Turret.updateWeaponFireChunks: NULL Terrain Target (save wfchunk.dbg file) ");
			}
			handleWeaponFire(chunk.weaponIndex,
							 target,
							 nullptr,
							 chunk.hit,
							 entryQuadTable[chunk.entryAngle],
							 chunk.numMissiles,
							 chunk.hitLocation);
		}
		else if (chunk.targetType == 2) 
		{
			GameObjectPtr target = ObjectManager->findByPartId(chunk.targetId);
			if (target == nullptr) 
			{
				DebugWeaponFireChunk (&chunk, nullptr, this);
				Assert(FALSE, 0, " Turret.updateWeaponFireChunks: NULL Special Target (save wfchunk.dbg file) ");
			}
			handleWeaponFire(chunk.weaponIndex,
							 target,
							 nullptr,
							 chunk.hit,
							 entryQuadTable[chunk.entryAngle],
							 chunk.numMissiles,
							 chunk.hitLocation);
		}
		else if (chunk.targetType == 3) 
		{
			glm::vec3 targetPoint;
			targetPoint.x = static_cast<float>(chunk.targetCell[1]) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
			targetPoint.y = (Terrain::worldUnitsMapSide / 2) - (static_cast<float>(chunk.targetCell[0]) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
			targetPoint.z = static_cast<float>(0);
			handleWeaponFire(chunk.weaponIndex, nullptr, &targetPoint, chunk.hit, 0.0, 0, 0);
		}
		else
			Fatal(0, " Mover.updateWeaponFireChunk: bad targetType ");
	}

	numWeaponFireChunks[which] = 0;

	return(NO_ERR);
}

//---------------------------------------------------------------------------

glm::vec3 Turret::getPositionFromHS (long nodeId) 
{
	//-----------------------------------------------------
	// Need to get this working with SITE code when done!
	glm::vec3 nodePos = appearance->getHitNode();
	if ((mc2::math::closeEnough(nodePos, position)) || (nodeId != -1))
		nodePos = appearance->getWeaponNodePosition(0);

	return(nodePos);
}

//---------------------------------------------------------------------------
glm::vec3 Turret::getLOSPosition (void)
{
	glm::vec3 nodePos = appearance->getWeaponNodePosition(0);
	glm::vec3 losPos = position;
	losPos.z = nodePos.z;
	return(losPos);
}

//---------------------------------------------------------------------------

void Turret::printFireWeaponDebugInfo (GameObjectPtr target, glm::vec3* targetPoint, long chance, long roll, WeaponShotInfo* shotInfo) {

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
			(*std::format_to_n(s, sizeof(s) - 1, "[{:.2f}] turret.fireWeapon HIT: ({:05d}){} @ ({:05d}){}", scenarioTime, mc2::fmt::as_i32(getPartId()), mc2::fmt::safe(getName()), mc2::fmt::as_i32(target->getPartId()), mc2::fmt::safe(targetName ? targetName : "unknown")).out = '\0');
			CombatLog->write(s);
			(*std::format_to_n(s, sizeof(s) - 1, "     chance = {:03d}, roll = {:03d}", mc2::fmt::as_i32(chance), mc2::fmt::as_i32(roll)).out = '\0');
			CombatLog->write(s);
			(*std::format_to_n(s, sizeof(s) - 1, "     weapon = ({:03d}){}, hitLocation = ({:d}){}, damage = {:.2f}, angle = {:.2f}",
				mc2::fmt::as_i32(shotInfo->masterId), mc2::fmt::safe(MasterComponent::masterList[shotInfo->masterId].getName()),
				mc2::fmt::as_i32(shotInfo->hitLocation), mc2::fmt::safe((shotInfo->hitLocation > -1) ? locationStrings[shotInfo->hitLocation] : "none"),
				shotInfo->damage,
				shotInfo->entryAngle).out = '\0');
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
			(*std::format_to_n(s, sizeof(s) - 1, "[{:.2f}] turret.fireWeapon MISS: ({:05d}){} @ ({:05d}){}", scenarioTime, mc2::fmt::as_i32(getPartId()), mc2::fmt::safe(getName()), mc2::fmt::as_i32(target->getPartId()), mc2::fmt::safe(targetName ? targetName : "unknown")).out = '\0');
			CombatLog->write(s);
			(*std::format_to_n(s, sizeof(s) - 1, "     chance = {:03d}, roll = {:03d}", mc2::fmt::as_i32(chance), mc2::fmt::as_i32(roll)).out = '\0');
			CombatLog->write(s);
			(*std::format_to_n(s, sizeof(s) - 1, "     weapon = ({:03d}){}, hitLocation = ({:d}){}, damage = {:.2f}, angle = {:.2f}",
				mc2::fmt::as_i32(shotInfo->masterId), mc2::fmt::safe(MasterComponent::masterList[shotInfo->masterId].getName()),
				mc2::fmt::as_i32(shotInfo->hitLocation), mc2::fmt::safe((shotInfo->hitLocation > -1) ? locationStrings[shotInfo->hitLocation] : "none"),
				shotInfo->damage,
				shotInfo->entryAngle).out = '\0');
			CombatLog->write(s);
			CombatLog->write(" ");
			}
		else if (targetPoint) {
		}
	}
}

//----------------------------------------------------------------------------

void Turret::printHandleWeaponHitDebugInfo (WeaponShotInfo* shotInfo) {

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
	char statusStr[50];
	if (isDestroyed())
		(*std::format_to_n(statusStr, sizeof(statusStr) - 1, " DESTROYED:").out = '\0');
	//else if (Team::noPain[getTeamId()])
	//	sprintf(statusStr, " NO PAIN:");
	else
		(*std::format_to_n(statusStr, sizeof(statusStr) - 1, ":").out = '\0');
	(*std::format_to_n(s, sizeof(s) - 1, "[{:.2f}] turret.handleWeaponHit{} ({:05d}){} ", scenarioTime, mc2::fmt::safe(statusStr), mc2::fmt::as_i32(getPartId()), mc2::fmt::safe(getName())).out = '\0');
	CombatLog->write(s);
	GameObjectPtr attacker = ObjectManager->getByWatchID(shotInfo->attackerWID);
	if (attacker)
		(*std::format_to_n(s, sizeof(s) - 1, "     attacker = ({:05d}){}", mc2::fmt::as_i32(attacker->getPartId()), mc2::fmt::safe(attacker->getName())).out = '\0');
	else
		(*std::format_to_n(s, sizeof(s) - 1, "     attacker = ({:05d})<unknown WID>", mc2::fmt::as_i32(shotInfo->attackerWID)).out = '\0');
	CombatLog->write(s);
	(*std::format_to_n(s, sizeof(s) - 1, "     weapon = ({:03d}){}, hitLocation = ({:d}){}, damage = {:.2f}, angle = {:.2f}",
		mc2::fmt::as_i32(shotInfo->masterId), mc2::fmt::safe(MasterComponent::masterList[shotInfo->masterId].getName()),
		mc2::fmt::as_i32(shotInfo->hitLocation), mc2::fmt::safe((shotInfo->hitLocation > -1) ? locationStrings[shotInfo->hitLocation] : "none"),
		shotInfo->damage,
		shotInfo->entryAngle).out = '\0');
	CombatLog->write(s);
	CombatLog->write(" ");
}

//----------------------------------------------------------------------------

void Turret::fireWeapon (GameObjectPtr target, long weaponId) {

	if (!target)
		return;

	long weaponMasterId = (static_cast<TurretTypePtr>(getObjectType()))->weaponMasterId[weaponId];

	//--------------------------------------------
	// Check and make sure we have line of fire...
	bool canSeeTarget = false;
	currentWeaponNode = appearance->getWeaponNode(0);
	if (currentWeaponNode != -1) 
	{
		glm::vec3 pos = appearance->getWeaponNodePosition(currentWeaponNode);
		appearance->setWeaponNodeUsed(currentWeaponNode);
		
		//Check if this is an indirect Fire weapon.
		bool indirectFireWeapon = false;
		
		//--------------------------------------------------------------------
		for (long i=0;i<20;i++)
		{
			if (Mover::IndirectFireWeapons[i] == (static_cast<TurretTypePtr>(getObjectType()))->weaponMasterId[weaponId])
			{
				indirectFireWeapon = true;
				break;
			}
		}
			
		
		bool LOS = false;
		LOS = lineOfSight(target,getAppearRadius());
		if (!LOS && indirectFireWeapon)
			LOS = getTeam()->teamLineOfSight(target->getPosition(),target->getAppearRadius());
		
		canSeeTarget = LOS;
	}

	if (target && !canSeeTarget)
		return;
	
	float entryAngle = 0.0;
	long aimLocation = -1;
	if (target)
		entryAngle = target->relFacingTo(position);

	bool isStreakMissile = MasterComponent::masterList[weaponMasterId].getWeaponStreak();

	long range;
	long attackChance = calcAttackChance(target,&range,weaponId);
	if (attackChance == -1.0)
		return;
		
	long hitRoll = RandomNumber(100);
	
	long hitLocation = -2;

	MechWarriorPtr targetPilot = nullptr;
	if (target && target->isMover()) {
		targetPilot = (static_cast<MoverPtr>(target))->getPilot();
		targetPilot->updateAttackerStatus(partId, scenarioTime);
	}

	//-----------------------
	// Weapon must recycle...
	startWeaponRecycle(weaponId);
	
	if (hitRoll < attackChance) {
		//------------------------------------------------
		// Weapon fires, so record current scenarioTime...
		recordWeaponFireTime(weaponId);

		//------------
		// Attack hit.
		if (MasterComponent::masterList[weaponMasterId].getForm() == COMPONENT_FORM_WEAPON_MISSILE) {
			//---------------------------------------------------------
			// It's a missile weapon. We need to determine how many hit
			// (and missed) the target, and in how many clusters...
			long missileAmount = MasterComponent::masterList[weaponMasterId].getWeaponAmmoAmount();
			long numMissiles;
			if (isStreakMissile)
				numMissiles = missileAmount;
			else {
				numMissiles = (static_cast<float>(missileAmount) / 2.0) + 0.5;
				numMissiles = std::max<long>(numMissiles, 1);
				numMissiles = std::min(numMissiles, missileAmount);
			}

			//-----------------------------------------------------------------------------
			// Is the missile short-range(SRM) or long-range(LRM)? SRMs are handled with
			// each missile spawning a separate damage calc. LRMs are clustered, 5 missiles
			// to a cluster (with the remainder a final, separate cluster), where each
			// cluster spawns a separate damage calc...

			// NO MORE CLUSTERS!

			//-----------------------------------------------
			// a MissileGen Object is ALL of the clusters.
			// Don't make a thousand of them or the game implodes!
			//numClusters = 1;
			unsigned char effectType = MasterComponent::masterList[weaponMasterId].getWeaponSpecialEffect();

			//----------------------------------------------------
			// Need to know which hotspot this comes from.
			// Also need to know which hotspot this is going to.
			unsigned long sourceHotSpot = currentWeaponNode;
			unsigned long targetHotSpot = 0;
			WeaponShotInfo curShotInfo;

			if (numMissiles > 0) {
				//-------------------------------------------------------------------
				// This code will mess up if the object is not a BULLET!!!!!!!!!!!
				WeaponBoltPtr weaponFX = ObjectManager->createWeaponBolt(effectType);
				if (!weaponFX)
					Fatal(-1," couldnt create weapon FX ");

				if (target) {
					if (aimLocation == -1)
						hitLocation = target->calcHitLocation(this, weaponMasterId, ATTACKSOURCE_WEAPONFIRE, ATTACK_TO_DESTROY);
					if (target->getObjectClass() == BATTLEMECH)
						targetHotSpot = (static_cast<BattleMechPtr>(target))->body[hitLocation].hotSpotNumber;
					}
				else
					hitLocation = -1;
						
				Assert(hitLocation != -2, 0, " Turret.FireWeapon: Bad Hit Location ");
	
				//--------------------------------------
				curShotInfo.init(this->getWatchID(),
								 weaponMasterId,
								 numMissiles * MasterComponent::masterList[weaponMasterId].getWeaponDamage(),
								 hitLocation,
								 entryAngle);

				//-------------------------------------------------------------------------
				// If I'm in a multiplayer game and I'm the server, record this turret fire
				// so it may be broadcast to all clients...
				if (MPlayer && MPlayer->isServer()) {
					WeaponFireChunk chunk;
					chunk.init();
					if (target) {
						if (target->isMover())
							chunk.buildMoverTarget(target,
												   weaponId,
												   true,
												   entryAngle,
												   numMissiles,
												   hitLocation);
						else
							chunk.buildTerrainTarget(target,
													 weaponId,
													 true,
													 numMissiles);
						//GameObjectPtr tempObj = ObjectManager->findByPartId(chunk.targetId);
						chunk.pack(this);
						WeaponFireChunk chunk2;
						chunk2.init();
						chunk2.data = chunk.data;
						chunk2.unpack(this);
						if (!chunk.equalTo(&chunk2))
							Fatal(0, " Turret.fireWeapon: Bad WeaponFireChunk (save wfchunk.dbg file now) ");
						addWeaponFireChunk(CHUNK_SEND, &chunk);
						LogWeaponFireChunk(&chunk, this, target);
					}
				}

				if (target) {
					weaponFX->connect(this, target, &curShotInfo, sourceHotSpot, targetHotSpot);
					printFireWeaponDebugInfo(target, nullptr, attackChance, hitRoll, &curShotInfo);
				}
			}
		}
		else
		{
			//----------------------------------------------------
			// Non-missile weapon, so just one weapon hit spawn...
			// For now, always use a laser effect...
			if (target) 
			{
				if (aimLocation == -1)
					hitLocation = target->calcHitLocation(this, weaponMasterId, ATTACKSOURCE_WEAPONFIRE, ATTACK_TO_DESTROY);
			}
			else
				hitLocation = -1;
			Assert(hitLocation != -2, 0, " Turret.FireWeapon: Bad Hit Location 2 ");

			WeaponShotInfo shotInfo;
			shotInfo.init(this->getWatchID(),
						  weaponMasterId,
						  MasterComponent::masterList[weaponMasterId].getWeaponDamage(),
						  hitLocation,
						  entryAngle);

			//-------------------------------------------------------------------------
			// If I'm in a multiplayer game and I'm the server, record this turret fire
			// so it may be broadcast to all clients...
			if (MPlayer && MPlayer->isServer()) {
				WeaponFireChunk chunk;
				chunk.init();
				if (target) {
					if (target->isMover())
						chunk.buildMoverTarget(target,
											   weaponId,
											   true,
											   entryAngle,
											   0,
											   hitLocation);
					else
						chunk.buildTerrainTarget(target,
												 weaponId,
												 true,
												 0);
					//GameObjectPtr tempObj = ObjectManager->findByPartId(chunk.targetId);
					chunk.pack(this);
					WeaponFireChunk chunk2;
					chunk2.init();
					chunk2.data = chunk.data;
					chunk2.unpack(this);
					if (!chunk.equalTo(&chunk2))
						Fatal(0, " Turret.fireWeapon: Bad WeaponFireChunk (save wfchunk.dbg file now) ");
					addWeaponFireChunk(CHUNK_SEND, &chunk);
					LogWeaponFireChunk(&chunk, this, target);
				}
			}

			unsigned char effectType = MasterComponent::masterList[weaponMasterId].getWeaponSpecialEffect();
			WeaponBoltPtr weaponFX = ObjectManager->createWeaponBolt(effectType);
			if (!weaponFX)
				Fatal(-1," couldnt create weapon FX ");

			unsigned long sourceHotSpot = currentWeaponNode;
			unsigned long targetHotSpot = 0;

			if (target) {
				if (target->getObjectClass() == BATTLEMECH)
					targetHotSpot = (static_cast<BattleMechPtr>(target))->body[hitLocation].hotSpotNumber;
				weaponFX->connect(this, target, &shotInfo, sourceHotSpot, targetHotSpot);
				printFireWeaponDebugInfo(target, nullptr, attackChance, hitRoll, &shotInfo);
			}
		}
	}
	else
	{
		if (!isStreakMissile) 
		{
			//------------------------------------------------
			// Weapon fires, so record current scenarioTime...
			recordWeaponFireTime(weaponId);

			//----------------------------------------------------
			// Miss, so check for possible miss resolution here...
			if (MasterComponent::masterList[weaponMasterId].getForm() == COMPONENT_FORM_WEAPON_MISSILE)
			{
				//---------------------------------------------------------
				// It's a missile weapon. We need to determine how many hit
				// (and missed) the target, and in how many clusters...
				long missileAmount = MasterComponent::masterList[weaponMasterId].getWeaponAmmoAmount();
				long numMissiles = (static_cast<float>(missileAmount) / 2.0) + 0.5;
				numMissiles = std::max<long>(numMissiles, 1);
				numMissiles = std::min(numMissiles, missileAmount);

				//-----------------------------------------------------------------------------
				// Is the missile short-range(SRM) or long-range(LRM)? SRMs are handled with
				// each missile spawning a separate damage calc. LRMs are clustered, 5 missiles
				// to a cluster (with the remainder a final, separate cluster), where each
				// cluster spawns a separate damage calc...

				if (numMissiles > 0) 
				{
					//-----------------------------------------------
					// a MissileGen Object is ALL of the clusters.
					// Don't make a thousand of them or the game implodes!
					//numClusters = 1;
					unsigned char effectType = MasterComponent::masterList[weaponMasterId].getWeaponSpecialEffect();
	
					//-------------------------------------------------------------------
					// This code will mess up if the object is not a BULLET!!!!!!!!!!!
					WeaponBoltPtr weaponFX = ObjectManager->createWeaponBolt(effectType);
					if (!weaponFX)
						Fatal(-1," couldnt create weapon FX ");
	
					unsigned long sourceHotSpot = currentWeaponNode;
					WeaponShotInfo curShotInfo;
	
					curShotInfo.init(this->getWatchID(),
									 weaponMasterId,
									 numMissiles * MasterComponent::masterList[weaponMasterId].getWeaponDamage(),
									 -1,
									 entryAngle);
	
					//-------------------------------------------------------------------------------------------
					// If we missed, pick sights away from the target and check LOS to each one.  If all are
					// invisible, just hit the target with zero points of damage.
					float missRadius = target ? 25.0 : 5.0;

					glm::vec3 positionOffset = target->getPosition();
					positionOffset.x += missRadius;
					positionOffset.z = land->getTerrainElevation(positionOffset);

					bool canSeeHit = lineOfSight(positionOffset);

					if (!canSeeHit)
					{
						positionOffset.x -= (missRadius * 2.0f);
						positionOffset.z = land->getTerrainElevation(positionOffset);

						canSeeHit = lineOfSight(positionOffset); 

						if (!canSeeHit)
						{
							positionOffset.x += missRadius;
							positionOffset.y += missRadius;

							positionOffset.z = land->getTerrainElevation(positionOffset);

							canSeeHit = lineOfSight(positionOffset);

							if (!canSeeHit)
							{
								positionOffset.y -= (missRadius * 2.0f);
								positionOffset.z = land->getTerrainElevation(positionOffset);

								canSeeHit = lineOfSight(positionOffset);

								if (!canSeeHit)
								{
									//OK, no miss location is visible.  Hit the target with ZERO damage!!
									curShotInfo.init(this->getWatchID(),
													 weaponMasterId,
													 0.0f,
													 -1,
													 entryAngle);
								}
							}
						}
					}
		
					//-------------------------------------------------------------------------
					// If I'm in a multiplayer game and I'm the server, record this weapon fire
					// so it may be broadcast to all clients...
					if (MPlayer && MPlayer->isServer()) 
					{
						WeaponFireChunk chunk;
						chunk.init();
						chunk.buildLocationTarget(positionOffset, weaponId, FALSE, numMissiles);
						chunk.pack(this);
						WeaponFireChunk chunk2;
						chunk2.init();
						chunk2.data = chunk.data;
						chunk2.unpack(this);
						if (!chunk.equalTo(&chunk2))
							Fatal(0, " Turret.fireWeapon: Bad WeaponFireChunk (save wfchunk.dbg file now) ");
						addWeaponFireChunk(CHUNK_SEND, &chunk);
						LogWeaponFireChunk(&chunk, this, target);
					}

					if (canSeeHit)		//miss location is in LOS.  Hit the ground
						weaponFX->connect(this,positionOffset,&curShotInfo,sourceHotSpot);
					else				//Miss location is NOT in LOS.  Hit Target with ZERO damage!!!
						weaponFX->connect(this,target,&curShotInfo,sourceHotSpot);

					printFireWeaponDebugInfo(target, &positionOffset, attackChance, hitRoll, &curShotInfo);
				}
			}
			else
			{
				//----------------------------------------------------
				// Non-missile weapon, so just one weapon hit spawn...
				// For now, always use a laser effect...
				WeaponShotInfo shotInfo;
				shotInfo.init(this->getWatchID(),
					weaponMasterId,
					MasterComponent::masterList[weaponMasterId].getWeaponDamage(),
					-1,
					entryAngle);

				unsigned char effectType = MasterComponent::masterList[weaponMasterId].getWeaponSpecialEffect();
				WeaponBoltPtr weaponFX = ObjectManager->createWeaponBolt(effectType);
				if (!weaponFX)
					Fatal(-1," couldnt create weapon FX ");

				//-------------------------------------------------------------------------------------------
				// If we missed, pick sights away from the target and check LOS to each one.  If all are
				// invisible, just hit the target with zero points of damage.
				float missRadius = target ? 25.0 : 5.0;

				glm::vec3 positionOffset = target->getPosition();
				positionOffset.x += missRadius;
				positionOffset.z = land->getTerrainElevation(positionOffset);

				bool canSeeHit = lineOfSight(positionOffset);

				if (!canSeeHit)
				{
					positionOffset.x -= (missRadius * 2.0f);
					positionOffset.z = land->getTerrainElevation(positionOffset);

					canSeeHit = lineOfSight(positionOffset); 

					if (!canSeeHit)
					{
						positionOffset.x += missRadius;
						positionOffset.y += missRadius;

						positionOffset.z = land->getTerrainElevation(positionOffset);

						canSeeHit = lineOfSight(positionOffset);

						if (!canSeeHit)
						{
							positionOffset.y -= (missRadius * 2.0f);
							positionOffset.z = land->getTerrainElevation(positionOffset);

							canSeeHit = lineOfSight(positionOffset);

							if (!canSeeHit)
							{
								//OK, no miss location is visible.  Hit the target with ZERO damage!!
								shotInfo.init(this->getWatchID(),
											 weaponMasterId,
											 0.0f,
											 -1,
											 entryAngle);
							}
						}
					}
				}
									
				//-------------------------------------------------------------------------
				// If I'm in a multiplayer game and I'm the server, record this weapon fire
				// so it may be broadcast to all clients...
				if (MPlayer && MPlayer->isServer()) {
					WeaponFireChunk chunk;
					chunk.init();
					chunk.buildLocationTarget(positionOffset, weaponId, FALSE, 0);
					chunk.pack(this);
					WeaponFireChunk chunk2;
					chunk2.init();
					chunk2.data = chunk.data;
					chunk2.unpack(this);
					if (!chunk.equalTo(&chunk2))
						Fatal(0, " Turret.fireWeapon: Bad WeaponFireChunk (save wfchunk.dbg file now) ");
					addWeaponFireChunk(CHUNK_SEND, &chunk);
					LogWeaponFireChunk(&chunk, this, target);
				}

				unsigned long sourceHotSpot = currentWeaponNode;

				if (canSeeHit)		//miss location is in LOS.  Hit the ground
					weaponFX->connect(this,positionOffset,&shotInfo,sourceHotSpot);
				else				//Miss location is NOT in LOS.  Hit Target with ZERO damage!!!
					weaponFX->connect(this,target,&shotInfo,sourceHotSpot);

				printFireWeaponDebugInfo(target, &positionOffset, attackChance, hitRoll, &shotInfo);
			}
		}
	}

	//-------------------------------------------------------------------
	// Trigger the WEAPON TARGET event. For now, this assumes the target
	// KNOWS we were targeting him. Of course, the target wouldn't always
	// be aware of this, would they?
	if (targetPilot)
		targetPilot->triggerAlarm(PILOT_ALARM_TARGET_OF_WEAPONFIRE, getWatchID());
}

//---------------------------------------------------------------------------

long Turret::handleWeaponFire (long weaponIndex,
							   GameObjectPtr target,
							   glm::vec3* targetPoint,
							   bool hit,
							   float entryAngle,
							   long numMissiles,
							   long hitLocation) {

	long weaponMasterId = (static_cast<TurretTypePtr>(getObjectType()))->weaponMasterId[weaponIndex];
	bool isStreakMissile = MasterComponent::masterList[weaponMasterId].getWeaponStreak();

	//-----------------------
	// Weapon must recycle...
	startWeaponRecycle(weaponIndex);

	WeaponBoltPtr weaponFX = nullptr;
	
	if (hit) {
		//------------
		// Attack hit.

		if (MasterComponent::masterList[weaponMasterId].getForm() == COMPONENT_FORM_WEAPON_MISSILE) {
			//-----------------------------------------------------------------------------
			// Is the missile short-range(SRM) or long-range(LRM)? SRMs are handled with
			// each missile spawning a separate damage calc. LRMs are clustered, 5 missiles
			// to a cluster (with the remainder a final, separate cluster), where each
			// cluster spawns a separate damage calc...

			//-----------------------------------------------
			// a MissileGen Object is ALL of the clusters.
			// Don't make a thousand of them or the game implodes!
			//numClusters = 1;
			unsigned char effectType = MasterComponent::masterList[weaponMasterId].getWeaponSpecialEffect();

			if (numMissiles > 0) {
				//-------------------------------------------------------------------
				// This code will mess up if the object is not a BULLET!!!!!!!!!!!
				weaponFX = ObjectManager->createWeaponBolt(effectType);
				if (!weaponFX)
					Fatal(-1," couldnt create weapon FX ");
	
				Assert(hitLocation != -2, 0, " Turret.handleWeaponFire: Bad Hit Location ");
				unsigned long targetHotSpot = 0;
				if (target && (target->getObjectClass() == BATTLEMECH))
					targetHotSpot = (static_cast<BattleMechPtr>(target))->body[hitLocation].hotSpotNumber;
	
				WeaponShotInfo curShotInfo;
				curShotInfo.init(this->getWatchID(),
								 weaponMasterId,
								 (numMissiles * MasterComponent::masterList[weaponMasterId].getWeaponDamage()),
								 hitLocation,
								 entryAngle);
	
				if (target)
					weaponFX->connect(this, target, &curShotInfo, 0, targetHotSpot);
				else
					weaponFX->connect(this, *targetPoint, &curShotInfo);
			}
		}
		else
		{
			//----------------------------------------------------
			// Non-missile weapon, so just one weapon hit spawn...
			// For now, always use a laser effect...
			
			WeaponShotInfo shotInfo;
			shotInfo.init(this->getWatchID(),
						  weaponMasterId,
						  MasterComponent::masterList[weaponMasterId].getWeaponDamage(),
						  hitLocation,
						  entryAngle);

			unsigned char effectType = MasterComponent::masterList[weaponMasterId].getWeaponSpecialEffect();
			weaponFX = ObjectManager->createWeaponBolt(effectType);
			if (!weaponFX)
				Fatal(-1," couldnt create weapon FX ");

			if (target) {
				unsigned long targetHotSpot = 0;
				if (target->getObjectClass() == BATTLEMECH)
					targetHotSpot = (static_cast<BattleMechPtr>(target))->body[hitLocation].hotSpotNumber;
				weaponFX->connect(this, target, &shotInfo, 0, targetHotSpot);
				}
			else {
				//--------------------------------
				// Hit the target point/terrain...
				weaponFX->connect(this, *targetPoint, &shotInfo);
			}
		}
	}
	else
	{
		if (!isStreakMissile) {

			//----------------------------------------------------
			// Miss, so check for possible miss resolution here...
			if (MasterComponent::masterList[weaponMasterId].getForm() == COMPONENT_FORM_WEAPON_MISSILE) {
				//-----------------------------------------------------------------------------
				// Is the missile short-range(SRM) or long-range(LRM)? SRMs are handled with
				// each missile spawning a separate damage calc. LRMs are clustered, 5 missiles
				// to a cluster (with the remainder a final, separate cluster), where each
				// cluster spawns a separate damage calc...

				if (numMissiles > 0) {
					//-----------------------------------------------
					// a MissileGen Object is ALL of the clusters.
					// Don't make a thousand of them or the game implodes!
					//numClusters = 1;
					unsigned char effectType = MasterComponent::masterList[weaponMasterId].getWeaponSpecialEffect();
	
					//-------------------------------------------------------------------
					// This code will mess up if the object is not a BULLET!!!!!!!!!!!
					weaponFX = ObjectManager->createWeaponBolt(effectType);
					if (!weaponFX)
						Fatal(-1," couldnt create weapon FX ");
	
					WeaponShotInfo curShotInfo;
					curShotInfo.init(this->getWatchID(),
									 weaponMasterId,
									 numMissiles * MasterComponent::masterList[weaponMasterId].getWeaponDamage(),
									 -1,
									 entryAngle);
	
					//-------------------------------------------------------------------------------------------
					// If we missed, push the weapon target position away from the target by twice extent Radius
					float missRadius = target ? 25.0 : 5.0;
					glm::vec3 positionOffset;
					positionOffset.x = missRadius;
					positionOffset.y = missRadius;
					positionOffset.z = 0.0;
		
					//------------------------------------------------------------
					// This is OK because a miss can miss to any screen location
					// during network play and it is still valid.
					// NOT true: Stray shots could trigger mines!
					positionOffset.x = RandomNumber(positionOffset.x * 2) - positionOffset.x;
					positionOffset.y = RandomNumber(positionOffset.y * 2) - positionOffset.y;
					positionOffset.z = RandomNumber(positionOffset.z * 2) - positionOffset.z;
					positionOffset += (target ? target->getPosition() : *targetPoint);
	
					weaponFX->connect(this, positionOffset, &curShotInfo);
				}
			}
			else
			{
				//----------------------------------------------------
				// Non-missile weapon, so just one weapon hit spawn...
				// For now, always use a laser effect...
				WeaponShotInfo shotInfo;
				shotInfo.init(this->getWatchID(),
					weaponMasterId,
					MasterComponent::masterList[weaponMasterId].getWeaponDamage(),
					-1,
					entryAngle);

				unsigned char effectType = MasterComponent::masterList[weaponMasterId].getWeaponSpecialEffect();
				weaponFX = ObjectManager->createWeaponBolt(effectType);
				if (!weaponFX)
					Fatal(-1," couldnt create weapon FX ");

				//-------------------------------------------------------------------------------------------
				// If we missed, push the weapon target position away from the target by twice extent Radius
				float missRadius = target ? 25.0 : 5.0;
				glm::vec3 positionOffset;
				positionOffset.x = missRadius;
				positionOffset.y = missRadius;
				positionOffset.z = 0.0;
		
				positionOffset.x = RandomNumber(positionOffset.x * 2) - positionOffset.x;
				positionOffset.y = RandomNumber(positionOffset.y * 2) - positionOffset.y;
				positionOffset.z = RandomNumber(positionOffset.z * 2) - positionOffset.z;
				positionOffset += (target ? target->getPosition() : *targetPoint);

				weaponFX->connect(this, positionOffset, &shotInfo);
			}
		}
	}

	//-------------------------------------------------------------------
	// Trigger the WEAPON TARGET event. For now, this assumes the target
	// KNOWS we were targeting him. Of course, the target wouldn't always
	// be aware of this, would they?
	MechWarriorPtr targetPilot = nullptr;
	if (target && target->isMover()) {
		targetPilot = (static_cast<MoverPtr>(target))->getPilot();
		targetPilot->updateAttackerStatus(partId, scenarioTime);
		targetPilot->triggerAlarm(PILOT_ALARM_TARGET_OF_WEAPONFIRE, getWatchID());
	}

	return(NO_ERR);
}

//---------------------------------------------------------------------------

void Turret::lightOnFire (float timeToBurn) {

}

//---------------------------------------------------------------------------
void Turret::renderShadows (void)
{
	if (appearance->hasAnimationData(0))
		return;			//No shadows on Popup Turrets.  Period.
		
	if (appearance->canBeSeen())
	{
		appearance->renderShadows();
	}
	
	setSelected(false);		//ALWAYS reset the selected flags.  GUI needs this to work!
	setTargeted( false );	//ALWAYS do it here, too!  Otherwise things may draw FUNNY!

}	

//---------------------------------------------------------------------------

// G7b K3: the old render() split at the render extract (sim reads and writes here; the draw only touches the appearance).
void Turret::extractRenderView (mc2::render_view::RenderView& view)
{
	GameObject::extractRenderView(view);
	if (appearance->canBeSeen())
	{
		view.flags |= mc2::render_view::Visible;
		if (TacticalOverview::maskWorldBars(drawFlags) & DRAW_BARS )
		{
			TurretTypePtr type = static_cast<TurretTypePtr>(getObjectType());
			float barStatus = 1.0;
			float totalDmgLvl = type->getDamageLevel();
			if (totalDmgLvl > 0.0)
				barStatus -= getDamage() / totalDmgLvl;
			
			barStatus = std::max<double>(barStatus, 0.0);
			
			DWORD color = 0xff7f7f7f;
			if ((teamId > -1) && (teamId < 8)) {
				if (getTeam()->isFriendly(Team::home))
					color = SB_GREEN;
				else if (getTeam()->isEnemy(Team::home))
					color = SB_RED;
				else
					color = SB_BLUE;
			}
				
			view.flags |= mc2::render_view::Bars;
			view.barColor = color;
			view.barStatus = barStatus;
		}

		if ((static_cast<TurretTypePtr>(getObjectType()))->turretTypeName < IDS_MC2_STRING_START)
		{
			view.nameId = (static_cast<TurretTypePtr>(getObjectType()))->turretTypeName + IDS_MC2_STRING_START;
		}
		else
		{
			view.nameId = (static_cast<TurretTypePtr>(getObjectType()))->turretTypeName;
		}


		windowsVisible = turn;
	}

	setSelected(false);		//ALWAYS reset the selected flags.  GUI needs this to work!
	setTargeted( false );	//ALWAYS do it here, too!  Otherwise things may draw FUNNY!

}

void Turret::renderView (const mc2::render_view::RenderView& view)
{
	if (!view.has(mc2::render_view::Visible))
		return;
	if (view.has(mc2::render_view::Bars))
	{
		appearance->setBarColor(view.barColor);
		appearance->setBarStatus(view.barStatus);
	}
	appearance->setObjectNameId(view.nameId);
	appearance->setVisibility(true,true);
	appearance->render();
	if (!view.has(mc2::render_view::Bars))
		if (const auto recipe = appearance->residentRenderRecipe()) ObjectManager->noteRenderResident(this, *recipe);
}

//---------------------------------------------------------------------------

void Turret::destroy (void) 
{
	mc2::tick::releaseLight(pointLight, eye, lightId);
	if (appearance)
	{
		releaseAppearance();
	}
}

//---------------------------------------------------------------------------

void Turret::init (bool create, ObjectTypePtr _type) {

	//-------------------------------------------
	// Initialize the Building Appearance here.
	GameObject::init(true, _type);

	setFlag(OBJECT_FLAG_JUSTCREATED, true);

	//-------------------------------------------------------------
	// The appearance is initialized here using data from the type
	// Need an MLR appearance class
	char *appearName = _type->getAppearanceTypeName();

	//--------------------------------------------------------------
	// New code!!!
	// We need to append the sprite type to the appearance num now.
	// The MechEdit tool does not assume a sprite type, nor should it.
	// MechCmdr2 features much simpler objects which only use 1 type of sprite!
	long appearanceType = (BLDG_TYPE << 24);

	AppearanceTypePtr buildingAppearanceType = nullptr;
	if (!appearName)
	{
		//------------------------------------------------------
		// LOAD a dummy appearance until real ones are available
		// for this building!
		appearanceType = (BLDG_TYPE << 24);
		buildingAppearanceType = appearanceTypeList->getAppearance(appearanceType,"TESTBLDG");
	}
	else
	{
		buildingAppearanceType = appearanceTypeList->getAppearance(appearanceType,appearName);
		if (!buildingAppearanceType)
		{
			char msg[1024];
			(*std::format_to_n(msg,sizeof(msg) - 1,"No Building Appearance Named {}",mc2::fmt::safe(appearName)).out = '\0');
			Fatal(0,msg);
		}
	}
	  
	adoptAppearance(std::make_unique_for_overwrite<BldgAppearance>());
	gosASSERT(appearance != nullptr);

	//--------------------------------------------------------------
	// The only appearance type for buildings is MLR_APPEARANCE.
	gosASSERT(buildingAppearanceType->getAppearanceClass() == BLDG_TYPE);
	
	appearance->init(static_cast<BldgAppearanceType*>(buildingAppearanceType), static_cast<GameObjectPtr>(this));
								
	objectClass = TURRET;
	setFlag(OBJECT_FLAG_NEVER_REVEALED, true);
	teamId = -1;
	readyTime[0] = readyTime[1] = readyTime[2] = readyTime[3] = 0.0;
	
	TurretTypePtr type = static_cast<TurretTypePtr>(getObjectType());
	if (type->engageRadius != 0.0)
		type->setExtentRadius(type->engageRadius);

	if (type->getExtentRadius() > 0.0)
		setFlag(OBJECT_FLAG_TANGIBLE, true);
	

	tonnage = type->baseTonnage;
	explDamage = type->explDmg;
	explRadius = type->explRad;

	setFlag(OBJECT_FLAG_CAPTURABLE, false);
	
	setFlag(OBJECT_FLAG_POP_NEUTRALS, false);
}	

//---------------------------------------------------------------------------

long Turret::handleWeaponHit (WeaponShotInfoPtr shotInfo, bool addMultiplayChunk) {

	if (!shotInfo)
		return(NO_ERR);

	if (addMultiplayChunk)
		MPlayer->addWeaponHitChunk(this, shotInfo);

	printHandleWeaponHitDebugInfo(shotInfo);

	damage = getDamage() + shotInfo->damage;
			
	TurretTypePtr type = static_cast<TurretTypePtr>(getObjectType());
	if (!isDestroyed() && (damage >= type->getDamageLevel()))
	{
		setFlag(OBJECT_FLAG_DESTROYED, true);
				
		//------------------------------------------------------
		// Turret is dead.  You may no longer collide with it.
		setFlag(OBJECT_FLAG_TANGIBLE, false);
		setStatus(OBJECT_STATUS_DESTROYED);
		appearance->setObjStatus(OBJECT_STATUS_DESTROYED); 

		//-----------------------------------------------------
		// Now, blow the building up using its type->explosion
		// (this won't do anything if a building type doesn't have an explosion)
		ObjectManager->createExplosion(TURRET_EXPLOSION_ID,nullptr,position,explDamage,explRadius,getExtentRadius());
		if (CombatLog) {
			char s[1024];
			(*std::format_to_n(s, sizeof(s) - 1, "[{:.2f}] turret.destroyed: ({:05d}){}", scenarioTime, mc2::fmt::as_i32(getPartId()), mc2::fmt::safe(getName())).out = '\0');
			CombatLog->write(s);
			CombatLog->write(" ");
		}
	}
		
	return(NO_ERR);
}

//---------------------------------------------------------------------------

long Turret::calcFireRanges (void) {

	//---------------------------
	// Calc min and max ranges...
	maxRange = 0;
	minRange = 1000000.0;
	numFunctionalWeapons = 0;
	for (long weaponMasterId : (static_cast<TurretTypePtr>(getObjectType()))->weaponMasterId) {
		if (weaponMasterId != -1) {
			long range = MasterComponent::masterList[weaponMasterId].getWeaponRange();
			float minWeaponRange = WeaponRanges[range][0];
			float maxWeaponRange = WeaponRanges[range][1];
			maxRange = std::max(maxRange, maxWeaponRange);
			minRange = std::min(minRange, minWeaponRange);
			numFunctionalWeapons++;
		}
	}

	return(0);
}

//---------------------------------------------------------------------------

bool Turret::isLinked (void)
{
	return (parent != 0);
}

//---------------------------------------------------------------------------
GameObjectPtr Turret::getParent (void)
{
	return (ObjectManager->getByWatchID(parent));
}

//---------------------------------------------------------------------------
void Turret::setParentId (DWORD pId)
{
	parentId = pId;
}

//---------------------------------------------------------------------------

void Turret::updateDebugWindow (GameDebugWindow* debugWindow) {

	debugWindow->clear();
	char s[128];
	//-------------------------------------------------------
	// For now, show the floating help text if we have one...
	if ((static_cast<ObjectAppearance*>(appearance))->objectNameId != -1) {
		char myName[255];
		cLoadString((static_cast<ObjectAppearance*>(appearance))->objectNameId, myName, 254);
		debugWindow->print(myName);
		}
	else
		debugWindow->print("<no name>");
	(*std::format_to_n(s, sizeof(s) - 1, "team: {:d}, partID: {:d}, objT: {:d}", mc2::fmt::as_i32(getTeamId()), mc2::fmt::as_i32(getPartId()), mc2::fmt::as_i32(getObjectType()->whatAmI())).out = '\0');
	debugWindow->print(s);
	GameObjectPtr target = ObjectManager->getByWatchID(targetWID);
	(*std::format_to_n(s, sizeof(s) - 1, "target: {:d}", mc2::fmt::as_i32(target ? target->getPartId() : 0)).out = '\0');
	debugWindow->print(s);
	(*std::format_to_n(s, sizeof(s) - 1, "damage: {:.2f}/{:.2f}", getDamage(), getDamageLevel()).out = '\0');
	debugWindow->print(s);
}

//***************************************************************************
#include "save_struct_zero.h"
#include "turret_value_codec.h"
void Turret::Save (PacketFilePtr file, long packetNum)
{
	MC2_SAVE_STRUCT(TurretData, data);
	CopyTo(&data);

	//PacketNum incremented in ObjectManager!!
	auto bytes = mc2::save::encodeTaggedRecord(mc2::save::TurretRecord{data}, mc2::save::turretTag);
	if (bytes.empty()) Fatal(0, "Invalid turret ObjectSlot");
	file->writePacket(packetNum, bytes.data(), static_cast<unsigned int>(bytes.size()), STORAGE_TYPE_ZLIB);
}

//***************************************************************************
void Turret::CopyTo (TurretData *data)
{																	   
	data->teamId = teamId;
	data->turretRotation = turretRotation;
	data->didReveal = didReveal;
	data->targetWID = targetWID;
	memcpy(data->readyTime,readyTime,sizeof(float) * MAX_TURRET_WEAPONS);
	memcpy(data->lastFireTime,lastFireTime,sizeof(float) * MAX_TURRET_WEAPONS);
	data->minRange = minRange;
	data->maxRange = maxRange;
	data->numFunctionalWeapons = numFunctionalWeapons;

	data->idleWait = idleWait;
	data->idlePosition = idlePosition;
	data->oldPosition = oldPosition;
	data->parentId = parentId;
	data->parent = parent;
	data->currentWeaponNode = currentWeaponNode;

	TerrainObject::CopyTo(data);
}

//---------------------------------------------------------------------------
void Turret::Load (TurretData *data)
{
	TerrainObject::Load(data);

	teamId = data->teamId;
	turretRotation = data->turretRotation;

	//Always FALSE on a load because we need to recalc the matrices to insure good LOS!!
	didReveal = false;

	targetWID = data->targetWID;
	memcpy(readyTime,data->readyTime,sizeof(float) * MAX_TURRET_WEAPONS);
	memcpy(lastFireTime,data->lastFireTime,sizeof(float) * MAX_TURRET_WEAPONS);
	minRange = data->minRange;
	maxRange = data->maxRange;
	numFunctionalWeapons = data->numFunctionalWeapons;

	idleWait = data->idleWait;
	idlePosition = data->idlePosition;
	oldPosition = data->oldPosition;
	parentId = data->parentId;
	parent = data->parent;
	currentWeaponNode = data->currentWeaponNode;
}

//***************************************************************************


void Turret::updatePresentationLighting()
{
	const float ourExtentRadius = getExtentRadius();
	const bool active = getAwake();
	//-------------------------------------
	// Are we a spotlight?
	if ((static_cast<TurretTypePtr>(getObjectType()))->weaponMasterId[0] == -1)
	{
		//---------------------------------
		// Yes we are.  Check if night.
		// If night and no spotlight yet, create one.
		// Move it with turret's rotation below.
		//
		// For E3, have the spotlights create the light immediately or we run out of light sources!
		if (active && eye->getIsNight() && (pointLight == nullptr) && (getStatus() != OBJECT_STATUS_DESTROYED))
		{
			pointLight = static_cast<TG_LightPtr>(malloc(sizeof(TG_Light)));
			pointLight->init(TG_LIGHT_SPOT);
			lightId = eye->addWorldLight(pointLight);

			pointLight->SetaRGB(0xffffffee);
			pointLight->SetIntensity(0.25f);
			pointLight->SetFalloffDistances(50.0f, 450.0f);
		}

		if ((!active || !eye->getIsNight()) && pointLight)
		{
			eye->removeWorldLight(lightId,pointLight);
			free(pointLight);
			pointLight = nullptr;
		}
	}

	if (pointLight) {
		glm::vec3 ourPosition = oldPosition;
		glm::vec3 xlatPos;
		mc2ToRender(xlatPos, ourPosition);

		pointLight->direction = xlatPos;

		mc2ToRender(pointLight->spotDir, position);

		pointLight->maxSpotLength = ourExtentRadius * 1.5f;

		glm::mat4 lightToWorldMatrix;
		mc2::math::buildTranslation(lightToWorldMatrix, xlatPos);
		mc2::math::buildRotation(lightToWorldMatrix, mc2::math::Euler(0.0f,0.0f,0.0f));
		pointLight->SetLightToWorld(&lightToWorldMatrix);
		pointLight->SetPosition(&ourPosition);
	}

}
