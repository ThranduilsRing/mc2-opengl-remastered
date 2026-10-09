//	mover.cpp - This file contains the Mover Class code
//	MechCommander 2
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef MCLIB_h
#include"mclib.h"
#endif
#include"terrain_runtime.h"   // TERRAIN-RUNTIME-CONSUMER-WATER-1

#ifndef GAMEOBJ_H
#include"gameobj.h"
#endif

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef MOVER_H
#include"mover.h"
#endif

#ifndef MOVE_H
#include"move.h"
#endif

#ifndef MECH_H
#include"mech.h"
#endif

#ifndef GVEHICL_H
#include"gvehicl.h"
#endif

#ifndef MECH3D_H
#include"mech3d.h"
#endif

#ifndef CMPONENT_H
#include"cmponent.h"
#endif

#ifndef WARRIOR_H
#include"warrior.h"
#endif

#ifndef GOS_PROFILER_H
#include"gos_profiler.h"
#endif

#ifdef USE_DEBRIS
#ifndef DEBRIS_H
#include"debris.h"
#endif
#endif

#ifndef GAMELOG_H
#include"gamelog.h"
#endif

#ifndef TACORDR_H
#include"tacordr.h"
#endif

#ifndef GAMESOUND_H
#include"gamesound.h"
#endif

//#ifndef SOUNDSYS_H
#include"soundsys.h"
//#endif

//#ifndef SOUNDS_H
#include"sounds.h"
//#endif

#ifndef COLLSN_H
#include"collsn.h"
#endif

#ifndef MECHCLASS_H
#include"mechclass.h"
#endif

#ifndef UNITDESG_H
#include"unitdesg.h"
#endif

#ifndef MULTPLYR_H
#include"multplyr.h"
#endif

#ifndef TEAM_H
#include"team.h"
#endif

#ifndef COMNDR_H
#include"comndr.h"
#endif

#ifndef GROUP_H
#include"group.h"
#endif

#ifndef CONTACT_H
#include"contact.h"
#endif

#ifndef TRIGGER_H
#include"trigger.h"
#endif

#ifndef MISSION_H
#include"mission.h"
#endif

#ifndef LOGISTICSPILOT_H
#include"logisticspilot.h"
#endif

#include "move_recon.h"  // MC2_MOVE_RECON per-frame pathfinding cost instrumentation
#include "../GameOS/gameos/diagnostic_trace.h"  // TARGETING-GUARD-1 chokepoint trace
#include <cstdlib>  // TARGETING-GUARD-1: std::getenv
#include "unitprofile.h"             // LOADOUT-HARDPOINT-JOIN-1: mc2UnitProfileDataEnabled()
#include "unit_profile_validator.h"  // LOADOUT-HARDPOINT-JOIN-1: validateLoadoutHardpointJoins()
#include "hardpoint.h"               // LOADOUT-HARDPOINT-JOIN-1: HardpointProfile / WeaponTag
#include <cmponent.h>                // LOADOUT-HARDPOINT-JOIN-1: ComponentFormType (weapon form)

// DEFINES
#define	GOALMAP_CELL_DIM	61

// GLOBALS
extern long					SimpleMovePathRange;
extern long					GroupMoveTrailLen[2];
extern long		tileMulMAPCELL_DIM[MAX_MAP_CELL_WIDTH];

extern long					RamObjectWID;

long						MaxMoveGoalChecks = 60;
float						DelayedOrderTime = 1.0;
float						GroupOrderGoalOffset = 127.0;

//BaseObjectPtr				MoverRoster[MAX_MOVER_PART_ID - MIN_MOVER_PART_ID + 1];

extern MoveMapPtr			PathFindMap[2/*NUM_PATHMAPS*/];
long						goalMap[GOALMAP_CELL_DIM * GOALMAP_CELL_DIM];
long						goalMapRowStart[GOALMAP_CELL_DIM] = {
								-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
								-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
								-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
								-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
							};
char						goalMapRowCol[GOALMAP_CELL_DIM * GOALMAP_CELL_DIM][2];
long						Mover::numMovers = 0;
SortListPtr					Mover::sortList = NULL;

extern float				DefaultAttackRadius;

extern float WeaponFireModifiers[NUM_WEAPONFIRE_MODIFIERS];
extern void DebugWeaponFireChunk (WeaponFireChunkPtr chunk1, WeaponFireChunkPtr chunk2, GameObjectPtr attacker);

// EXTERNS

//extern ObjectMapPtr			GameObjectMap;
extern unsigned long		NextIdNumber;

#ifdef LAB_ONLY
extern __int64 MCTimeCalcGoal1Update;
#endif

#ifdef USE_SOUNDS
extern bool					friendlyDestroyed;
extern bool					enemyDestroyed;
#endif

extern TeamPtr				homeTeam;
extern bool					JumpOnBlocked;
extern bool					FindingEscapePath;

//extern float				FireOddsTable[NUM_FIREODDS];
extern float				RankVersusChassisCombatModifier[NUM_WARRIOR_RANKS][NUM_MECH_CLASSES];
float WeaponFireModifiers[NUM_WEAPONFIRE_MODIFIERS] = {
	0.0,		// Short Range To Target
	-10.0,		// Medium Range To Target
	-20.0,		// Long Range To Target
	-98.0,		// Aimed Shot To Head
	-85.0,		// Aimed Shot To Torso
	-88.0,		// Aimed Shot To Arm/Leg
	50.0,		// Target is Stationary
	0.0,		// Green/Light
	-5.0,		// Green/Medium
	-15.0,		// Green/Heavy
	-25.0,		// Green/Assault
	5.0,		// Regular/Light
	0.0,		// Regular/Medium
	-5.0,		// Regular/Heavy
	-15.0,		// Regular/Assault
	10.0,		// Veteran/Light
	5.0,		// Veteran/Medium
	0.0,		// Veteran/Heavy
	-5.0,		// Veteran/Assault
	15.0,		// Elite/Light
	10.0,		// Elite/Medium
	5.0,		// Elite/Heavy
	0.0,		// Elite/Assault
	0.0,
	0.0,
	0.0,
	0.0,
	0.0,
	0.0,
	0.0
};

extern float				WarriorRankScale[NUM_WARRIOR_RANKS];
extern const char *			SpecialtySkillsTable[NUM_SPECIALTY_SKILLS];

float						WeaponSpecialistModifier = 20.0f;

extern float				MovementUpdateFrequency;
extern float				CombatUpdateFrequency;
extern float				CommandUpdateFrequency;
extern float				PilotCheckUpdateFrequency;
extern long					PilotCheckModifierTable[2];
//extern float				ElementalTargetNoJumpDistance;
extern long					MechSalvageChance;
extern float				MapCellDiagonal;

//extern long OverlayWeightIndex[NUM_OVERLAY_TYPES];

extern long NumLocationCriticalSpaces[NUM_BODY_LOCATIONS];

extern GlobalMapPtr			GlobalMoveMap[3];

extern long adjCellTable[MAPCELL_DIM * MAPCELL_DIM][8][4];

extern bool useUnlimitedAmmo;

extern bool CantBlowSalvage;
long TargetRolo = -1;
WeaponFireChunk CurMoverWeaponFireChunk;

extern char* ExceptionGameMsg;
extern char ChunkDebugMsg[5120];

extern char OverlayIsBridge[NUM_OVERLAY_TYPES];

float GroundVehiclePivotYawMultiplier = 0.25;

extern bool friendlyDestroyed;
extern bool enemyDestroyed;

extern GameLog* CombatLog;

float Mover::newThreatMultiplier = 3.0;
float Mover::marginOfError[2] = {5.0, 10.0};
float Mover::refitRange;
float Mover::refitTime;
float Mover::refitCostArray[NUM_COSTS][NUM_FIXERS];
float Mover::refitAmount;
float Mover::recoverRange;
float Mover::recoverTime;
float Mover::recoverCost;
float Mover::recoverAmount;
long Mover::IndirectFireWeapons[20] = {-1,-1,-1,-1,-1,
									   -1,-1,-1,-1,-1,  
									   -1,-1,-1,-1,-1, 
									   -1,-1,-1,-1,-1};

long Mover::AreaEffectWeapons[20] 	= {-1,-1,-1,-1,-1,
									   -1,-1,-1,-1,-1,  
									   -1,-1,-1,-1,-1, 
									   -1,-1,-1,-1,-1};

bool Mover::inRecoverUpdate = false;

long StatusChunkUnpackErr = 0;

char AttackParameters[14][3] = {
	{ATTACK_TO_DESTROY, FIRERANGE_OPTIMAL, 1},		// DESTROY
	{ATTACK_TO_DESTROY, FIRERANGE_SHORT, 0},		// STOP AND HOLD FIRE
	{ATTACK_TO_DESTROY, FIRERANGE_SHORT, 1},		// DESTROY - SR
	{ATTACK_TO_DESTROY, FIRERANGE_MEDIUM, 1},		// DESTROY - MR
	{ATTACK_TO_DESTROY, FIRERANGE_LONG, 1},			// DESTROY - LR
	{ATTACK_TO_DESTROY, FIRERANGE_MEDIUM, 0},		// DESTROY FROM HERE
	{ATTACK_TO_DESTROY, FIRERANGE_MEDIUM, 1},		// DESTROY ALL OUT
	{ATTACK_TO_DISABLE, FIRERANGE_MEDIUM, 1},		// DISABLE
	{ATTACK_TO_DISABLE, FIRERANGE_SHORT, 0},		// STOP AND HOLD FIRE
	{ATTACK_TO_DISABLE, FIRERANGE_SHORT, 1},		// DISABLE - SR
	{ATTACK_TO_DISABLE, FIRERANGE_MEDIUM, 1},		// DISABLE - MR
	{ATTACK_TO_DISABLE, FIRERANGE_LONG, 1},			// DISABLE - LR
	{ATTACK_TO_DISABLE, FIRERANGE_MEDIUM, 0},		// DISABLE FROM HERE
	{ATTACK_TO_DISABLE, FIRERANGE_MEDIUM, 1}		// DISABLE MAX SALVAGE
};

long rearAttackTable[NUM_DIRECTIONS][3][2] = {
	{{1, 0}, {1, -1}, {0, 1}},
	{{1, -1}, {0, -1}, {1, 1}},
	{{0, -1}, {-1, -1}, {1, 0}},
	{{-1, -1}, {-1, 0}, {1, -1}},
	{{-1, 0}, {-1, 1}, {0, -1}},
	{{-1, 1}, {0, 1}, {-1, -1}},
	{{1, 0}, {1, 1}, {-1, 0}},
	{{1, 1}, {1, 0}, {-1, 1}}
};

// Game System Tweakable Data

float WeaponRange[NUM_FIRERANGES] = {
	250,
	500,
	1000
};

float WeaponRanges[NUM_WEAPON_RANGE_TYPES][2] = {
	{0, 100},
	{50, 150},
	{100, 225},
	{0, 0},
	{0, 0}
};

float OptimalRangePoints[NUM_WEAPON_RANGE_TYPES] = {
	25,		//Short
	125,	//Med
	200,	//Long
	75,		//Short-Med
	125		//Med-Long
};

bool OptimalRangePointInRange[NUM_WEAPON_RANGE_TYPES][3];

unsigned char OptimalRangeTieTable[32] = {
	255, 0, 1, 1,
	2, 2, 2, 2,
	3, 3, 3, 3,
	3, 3, 3, 3,
	4, 4, 4, 4,
	4, 4, 4, 4,
	4, 4, 4, 4,
	4, 4, 4, 4
};

float DefaultAttackRange = 75.0;

float DisableAttackModifier = 10.0;
float DisableGunneryModifier = 5.0;
float SalvageAttackModifier = 30.0;

float PilotingCheckFactor = 1.0;

long hitLevel[2] = {
	10,		// weak hit is damage of <10
	20		// moderate hit is damage <20, else heavy hit
};

long ClusterSizeSRM = 2;	// eventually, let's just kill clusters!
long ClusterSizeLRM = 5;	// eventually, let's just kill clusters!

long AntiMissileSystemStats[2][2] = {
	{1, 2},		// IS
	{2, 2}		// CLAN
};

float PilotCheckHalfRate = 5.0;

float DamageRateFrequency = 10.0;

char AttitudeEffect[NUM_ATTITUDES][6] = {
	{50, 75, 5, -2, 0, 3},
	{40, 60, 10, -1, 1, 5},
	{30, 50, 15, 0, 2, 10},
	{20, 40, 20, 1, 3, 15},
	{10, 25, 25, 1, 4, 20},
	{0, 0, 32, 2, 5, 127}
};

//extern bool SensorAutomaticSuccess;
//extern float SensorModifier[8];
float SensorBaseChance = 50.0;
float SensorSkillFactor = 10.0;

extern long DebugMovePathType;

#ifdef USEHEAT
float SensorHeatMultiplier = 5.0;
float SensorWeaponHeatMultiplier = 5.0;
float SensorHeatSourceModifiers[2] = {10.0, -10.0};
extern float WeaponHeatCheckDelay;
#endif

float SensorBlockingObjectModifier = -5.0;
float SensorShutDownMechModifier = -50.0;
float SensorRangeModifier[4][2] = {
	{0.25f, 50.0},
	{0.15f, 20.0},
	{0.35f, 0.0},
	{0.25f, -25.0}
};
float SensorSizeModifier[3][2] = {
	{25.0, -10.0},
	{60.0, 0.0},
	{999.0, 15.0}
};
float SensorBlockingTerrain[2] = {-25.0, -5.0};

long AimedFireAbort;
long AimedFireHitTable[3];
float FireArc[3];

//bool LongRangeMovementEnabled[MAX_TEAMS];

extern const char ProfessionalismOffsetTable[NUM_OFFSET_RANGES][2];
extern const char DecorumOffsetTable[NUM_OFFSET_RANGES][2];
extern const char AmmoConservationModifiers[2][2];
//extern long OverlayWeightTable[NUM_OVERLAY_WEIGHT_CLASSES * NUM_OVERLAY_TYPES * MAPCELL_DIM * MAPCELL_DIM];
//extern long OverlayWeightIndex[NUM_OVERLAY_TYPES];
//extern float MoveMarginOfError[2];
extern float MoveTimeOut;
extern float MoveYieldTime;
extern long DefaultMechJumpCost;

extern char SensorSkillMoveRange[4];
extern float SensorSkillMoveFactor[8];

float SkillTry[4];
float SkillSuccess[4];
float KillSkill[6];
float WeaponHit;
float SensorSkill;

long MoveChunk::err = 0;

float MaxStationaryTime = 0.0;
float MaxTimeRevealed = 0.0f;

#define CLAN_SEEN_FLAG		2
#define IS_SEEN_FLAG		1

extern float worldUnitsPerMeter;
char Mover::optimalCells[MAX_ATTACK_CELLRANGE][MAX_ATTACK_INCREMENTS][2];
long Mover::numOptimalIncrements = 0;
short Mover::rangedCellsIndices[MAX_ATTACK_CELLRANGE][2];
char Mover::rangedCells[RANGED_CELLS_DIM][2];
TriggerAreaManager* Mover::triggerAreaMgr = NULL;

unsigned long Mover::holdFireIconHandle = 0;

float HeadShotModifier = 0.0f;
float ArmShotModifier = 0.0f;
float LegShotModifier = 0.0f;

bool CalcValidAreaTable = true;

void DEBUGWINS_print (char* s, long window);

void Mover::setPartId (long newPartId) {

	partId = newPartId;
	//MoverRoster[newPartId - MIN_MOVER_PART_ID] = (BaseObjectPtr)this;
}

long Mover::setTeamId (long _teamId, bool setup) {

	if (teamId > -1) {
		// Remove me from my current team...
		prevTeamId = teamId;
		TeamPtr team = Team::teams[teamId];
		if (sensorSystem)
			SensorManager->removeTeamSensor(teamId, sensorSystem);
		//if (ecm != 255)
		//	SensorManager->removeEcm(this,);
		if (pilot)
			pilot->setTeam(NULL);
		team->removeFromRoster(this);
	}

	teamId = _teamId;
	Assert(teamId > -2, teamId, " Mover.setTeamId: bad teamId ");

	if (setup) {
		if (teamId > -1) {
			TeamPtr team = Team::teams[teamId];
			Assert(team != NULL, 0, " Mover.setTeamId: NULL team ");
			if (sensorSystem)
				SensorManager->addTeamSensor(teamId, sensorSystem);
			if (pilot)
				pilot->setTeam(team);
		}
	}
	return(NO_ERR);
}

TeamPtr Mover::getTeam (void) {

	if (teamId == -1)
		return(NULL);
	return(Team::teams[teamId]);
}

CommanderPtr Mover::getCommander (void) {

	if (commanderId == -1)
		return(NULL);
	return(Commander::commanders[commanderId]);
}

bool Mover::isFriendly (TeamPtr team) {

	if (teamId > -1 && team)
		return(Team::relations[teamId][team->getId()] == RELATION_FRIENDLY);
	return(false);
}

bool Mover::isEnemy (TeamPtr team) {

	if (teamId > -1 && team)
		return(Team::relations[teamId][team->getId()] == RELATION_ENEMY);
	return(false);
}

bool Mover::isNeutral (TeamPtr team) {

	if (teamId > -1 && team)
		return(Team::relations[teamId][team->getId()] == RELATION_NEUTRAL);
	return(true);
}

long Mover::setGroupId (long _groupId, bool setup) {

	groupId = _groupId;
	if (setup && (groupId > -1) && pilot) {
		pilot->clearCurTacOrder(false);
		pilot->setOrderState(ORDERSTATE_GENERAL);
	}

	return(NO_ERR);
}

MoverGroupPtr Mover::getGroup (void) {

	if (groupId == -1)
		return(NULL);
	return(Commander::commanders[commanderId]->getGroup(groupId));
}

void Mover::setPosition (Stuff::Vector3D& newPosition) {

	// Confirm position as on map and move it
	// back on map if necessary
	float maxMap = Terrain::worldUnitsMapSide / 2.0;
	bool offMap = false;

	if (newPosition.x < -maxMap)
	{
		newPosition.x = -maxMap;
		offMap = true;
	}

	if (newPosition.x > maxMap)
	{
		newPosition.x = maxMap;
		offMap = true;
	}

	if (newPosition.y < -maxMap)
	{
		newPosition.y = -maxMap;
		offMap = true;
	}

	if (newPosition.y > maxMap)
	{
		newPosition.y = maxMap;
		offMap = true;
	}

	if (!offMap)
	{
		// New map clip code here.  Actually easy once thought out!
		float maxVisual = Terrain::worldUnitsPerVertex * Terrain::blocksMapSide * Terrain::verticesBlockSide / 2;
		maxVisual -= 1300.0;
		float clipChk1 = newPosition.y - newPosition.x;
		float clipChk2 = newPosition.y + newPosition.x;
		
		bool clipA = (clipChk1 > maxVisual);
		bool clipB = (clipChk1 < -maxVisual);
		bool clipC = (clipChk2 > maxVisual);
		bool clipD = (clipChk2 < -maxVisual);
		
		if (clipA || clipB || clipC || clipD)
			offMap = true;
	}
	
	//-- Code to whack Object if it moves off of map
	//-- I.e. Withdrawing or a camera Drone.  hehehe.
	if (offMap && withdrawing)
	{
		ObjectManager->getObjectType(typeHandle)->handleDestruction(this,NULL);
	}

	GameObject::setPosition(newPosition);
	triggerAreaMgr->setHit(this);

	// If we've marked our cell as occupied, and our position has changed, then clear it...
	if (lastMapCell[0] != -1)
		if ((lastMapCell[0] != cellPositionRow) || (lastMapCell[1] != cellPositionCol)) {
			GameMap->setMover(lastMapCell[0], lastMapCell[1], false);
			lastMapCell[0] = -1;
			lastMapCell[1] = -1;
		}
	
	// Now, mark our current cell if we're NOT moving...
	if (pilot->getMovePath()->numSteps == 0) {
		lastMapCell[0] = cellPositionRow;
		lastMapCell[1] = cellPositionCol;
		GameMap->setMover(cellPositionRow, cellPositionCol, true);
	}

	//if (objPosition)
	//	GameObjectMap->updateObject(this);
}

void Mover::rotate (float angle) 
{
	//Old function.  If only one is passed in, facing and rotation are equal!
	rotate(angle,angle);
}

void Mover::rotate (float angle, float facingAngle)
{
	rotation += angle;
	if (rotation < -180.0)
		rotation += 360.0;
	else if (rotation > 180.0)
		rotation -= 360.0;
}

void Mover::setAwake (bool set) {

	GameObject::setAwake(set);
	if (set && pilot && (getStatus() == OBJECT_STATUS_SHUTDOWN))
		pilot->orderPowerUp(false, ORDER_ORIGIN_SELF);
}

float Mover::relFacingDelta (Stuff::Vector3D goalPos, Stuff::Vector3D targetPos) {

	float relFacingToGoal = relFacingTo(goalPos);
	float relFacingToTarget = relFacingTo(targetPos);
	float facingDelta = 0.0;
	if (relFacingToGoal >= 0) {
		if (relFacingToTarget >= 0)
			if (relFacingToGoal > relFacingToTarget)
				facingDelta = relFacingToGoal - relFacingToTarget;
			else
				facingDelta = relFacingToTarget - relFacingToGoal;
		else {
			facingDelta = relFacingToGoal - relFacingToTarget;
			if (facingDelta > 180)
				facingDelta = 360.0 - facingDelta;
		}
		}
	else {
		if (relFacingToTarget >= 0) {
			facingDelta = -relFacingToGoal + relFacingToTarget;
			if (facingDelta > 180)
				facingDelta = 360.0 - facingDelta;
			}
		else
			if (relFacingToGoal > relFacingToTarget)
				facingDelta = relFacingToGoal - relFacingToTarget;
			else
				facingDelta = relFacingToTarget - relFacingToGoal;
	}
	return(facingDelta);
}

float Mover::relFacingTo (Stuff::Vector3D goal, long bodyLocation) {

#if 1
	Stuff::Vector3D facingVec = getRotationVector();

	Stuff::Vector3D goalVec;
	goalVec.Subtract(goal, position);
	goalVec.z = 0.0;

	float angle = angle_from(facingVec, goalVec);
	Stuff::Vector3D angleSign;
	angleSign.Cross(facingVec, goalVec); 
	if (angleSign.z >= 0.0)
	{
		angle = -angle;
	}
	return(angle);
#else
	Stuff::Vector3D curPos = position;
	curPos.z = 0;
	goal.z = 0;

	// Create vector for facing
	frame_of_ref workFrame = frame;
	//float angle45 = 45.0;
	//workFrame.rotate_about_k(angle45);

 	Stuff::Vector3D velVect = -(workFrame.j);

	Stuff::Vector3D goalVec;
	goalVec = goal - curPos;
	goalVec.normalize();
	
	float resultAngle = velVect.angle_from(goalVec);

	Stuff::Vector3D angleSign = velVect CROSS goalVec; 
	if (angleSign.z >= 0.0)
	{
		resultAngle = -resultAngle;
	}

	/*
	if (angle < 180.0) {
		// Now, let's determine if it's to the right or left.
		// If to the right, return a positive angle.
		// If to the left, return a negative angle.
		// There has to be a cleaner way to get this, right?!
		float one = 1.0;
		workFrame.rotate_about_k(one);
 		velVect = -(workFrame.j);
		float angle2 = velVect.angle_from(goal - curPos);

		if (angle2 < angle)
			return(-angle);
	}
	*/
	return(resultAngle);
#endif
}

float Mover::getTerrainAngle (void) {

	Stuff::Vector3D worldK(0.0, 0.0, 1.0);
	float result = positionNormal * worldK;
	result = my_acos(result) * RADS_TO_DEGREES;

	return(result);
}

float Mover::getVelocityTilt (void) {

	// Create vector for velocity
	Stuff::Vector3D facingVec = getRotationVector();

	float result = positionNormal * facingVec;
	result = my_acos(result) * RADS_TO_DEGREES;

	return(result);
}

float Mover::getFireArc (void) {

	switch (getObjectClass()) {
		case BATTLEMECH:
			return(FireArc[0]);
			break;
		case GROUNDVEHICLE:
			return(FireArc[1]);
			break;
		case ELEMENTAL:
			return(FireArc[2]);
			break;
	}
	return(45.0);
}

void Mover::destroy (void) 
{
	//We only destroy on game end.  Most of this is caught in the heap frees!!

	// This will free any memory the mover is using.
	if (appearance)
	{
		delete appearance;
		appearance = NULL;
	}

	if (contactInfo)
	{
		delete contactInfo;
		contactInfo = NULL;
	}
}

Stuff::Vector3D Mover::relativePosition (float angle, float distance, unsigned long flags) {

	// Note that the angle should be -180 <= angle <= 180, and
	// the distance is in meters...

	distance *= -worldUnitsPerMeter;
	
	// Absolute facing, based upon north facing...
	Stuff::Vector3D shiftVect(0.0, 1.0, 0.0);
	if ((flags & RELPOS_FLAG_ABS) == 0) {
		// Create vector for facing, rotated to adjust for the
		// relative angle...
		shiftVect = getRotationVector();

	}
	Rotate(shiftVect, angle);
	shiftVect *= distance;

	Stuff::Vector3D curPos(position.x, position.y, 0.0);
	Stuff::Vector3D relPos(curPos.x + shiftVect.x, curPos.y + shiftVect.y, 0.0);

	Stuff::Vector3D start;
	Stuff::Vector3D goal;
	Stuff::Vector3D deltaVector;
	if (flags & RELPOS_FLAG_PASSABLE_START) {
		start  = curPos;
		goal = relPos;
		}
	else {
		start = relPos;
		goal = curPos;
	}
	deltaVector.Subtract(goal, start);

	// First, we need to calc the delta vector--how much we extend
	// the ray everytime we check the map cell for clear placement.
	deltaVector.Normalize(deltaVector);
	float cellLength = Terrain::worldUnitsPerCell;
	cellLength *= 0.5;
	deltaVector *= cellLength;
	if (deltaVector.GetLength() == 0.0)
		return(curPos);

	// Determine the max length the ray must be cast...
	float maxLength = distance_from(goal, start);

	// We'll start at the target, and if it's blocked, we'll move
	// toward our start location, looking for the first valid/open
	// cell...
	Stuff::Vector3D curPoint = start;
	Stuff::Vector3D curRay;
	curRay.Zero();
	float rayLength = 0.0;

	int cellR, cellC;
	land->worldToCell(curPoint, cellR, cellC);
	bool cellClear = GameMap->getPassable(cellR, cellC);

	Stuff::Vector3D lastGoodPoint = curPoint;
	if (flags & RELPOS_FLAG_PASSABLE_START)
		while (cellClear && (rayLength < maxLength)) {
			lastGoodPoint = curPoint;
			curPoint += deltaVector;
			curRay.Subtract(curPoint, start);
			rayLength = curRay.GetLength();

			land->worldToCell(curPoint, cellR, cellC);
			cellClear = GameMap->getPassable(cellR, cellC);
		}
	else
		while (!cellClear && (rayLength < maxLength)) {
			lastGoodPoint = curPoint;
			curPoint += deltaVector;
			curRay.Subtract(curPoint, start);
			rayLength = curRay.GetLength();

			land->worldToCell(curPoint, cellR, cellC);
			cellClear = GameMap->getPassable(cellR, cellC);
		}

	curPoint.x = lastGoodPoint.x;
	curPoint.y = lastGoodPoint.y;
	curPoint.z = land->getTerrainElevation(curPoint);
	return(curPoint);
}

void Mover::setSensorRange (float range) {

	if (sensorSystem)
		sensorSystem->setRange(range);
}

bool Mover::hasActiveProbe (void) {

	return(probe != 255);
}

void Mover::onTypeBound (void) {
	// UNIT-PROFILE-SEAM-1: current capability state starts from type baseline.
	if (getObjectType())
		runtime.currentCapabilities = getObjectType()->profile().baselineCapabilities;
}

// UNIT-ACTION-QUERY-RESULT-1: gameplay intent resolved through the pure
// unit_action.h contract, which reports a *reason* rather than a bare bool.
// canPerform() (mover.h) is now a thin adapter: queryAction(a) == Available.
// Slice-2 input mapping — wire the facts that EXIST TODAY, leave the rest at
// their non-blocking defaults so future-slice facets never spuriously deny:
//   validForSchema     = hasCapability(CAP_JUMP)  (no capability == not the
//                        kind of thing that can ever jump -> InvalidForUnitSchema)
//   hasCapability      = hasCapability(CAP_JUMP)
//   hasExecutor        = registry.isCompatible(Jump, locomotionModel())
//                        (ACTION-EXECUTOR-REGISTRY-1: registry-resolved, NOT a
//                        class virtual — the jump executor is bound compatible
//                        with LOCO_MECH_WALKER only, so a BattleMech resolves
//                        true and a vehicle's non-jump locomotion model false)
//   locomotionSupports = true   (LOCOMOTION-FACET-1, slice 4 — not yet a fact)
//   damageDisabled     = false  (no cheap knocked-out signal at this seam yet)
//   hasAnimation       = true   (presentation adapter facet — future slice)
//   hasResource        = true   (fuel/heat/ammo facet — future slice)
// Net reduction, identical to the old bool for real units: cap+compatible-executor
// -> Available; cap-but-no-compatible-executor -> MissingExecutor; no-cap ->
// InvalidForUnitSchema.
// AUDIT NOTE (ACTION-EXECUTOR-REGISTRY-1): this is where a "capability-granted
// vehicle" now correctly reports MissingExecutor instead of silently-Available.
// The old world coupled the executor to the BattleMech class; now the executor
// is resolved from the registry against the unit's locomotionModel(), so a
// vehicle that is data-granted CAP_JUMP but whose locomotion model isn't
// jump-compatible is honestly reported as MissingExecutor.
ActionAvailability Mover::queryAction(UnitAction a) const {
	switch (a) {
		case UnitAction::Jump: {
			// Executor resolved from the ONE engine-side registry (slice 3),
			// against this unit's transitional locomotion model, instead of the
			// old hasJumpExecutor() class virtual.
			const bool hasExecutor =
				theActionExecutorRegistry().isCompatible(UnitAction::Jump,
				                                         locomotionModel());
			// Pure inputs-builder (unit_action.h) keeps engine + tests in lockstep.
			return resolveAction(jumpQueryInputs(hasCapability(CAP_JUMP),
			                                     hasExecutor));
		}
	}
	return ActionAvailability::InvalidForUnitSchema;
}

// LOCOMOTION-FACET-1 (slice 4): locomotion model resolved from the DATA facet on
// the unit's type profile, not the C++ class. BattleMechType::init sets the
// facet to LOCO_MECH_WALKER at type-load; the base/vehicle default is LOCO_WHEELED
// (UnitProfile's field initializer); an optional `.fit [UnitProfile] Locomotion=`
// token can override either. This is the isolation-seam swap the audit tracked:
// the value now comes from data, so the old virtual + BattleMech override are
// gone and this single read replaces both class sites. A null type falls back to
// the base default so early-init paths never crash.
int Mover::locomotionModel (void) const {
	// getObjectType() is a non-const legacy accessor but does not mutate; the
	// read is logically const. const_cast keeps locomotionModel()/queryAction()
	// const (their callers query availability without a mutable Mover).
	ObjectTypePtr type = const_cast<Mover*>(this)->getObjectType();
	if (type)
		return type->profile().locomotion;
	return LOCO_WHEELED;
}

// DAMAGE-SCHEMA-FACET-1 (slice 5): hit-location model resolved from the DATA
// facet on the unit's type profile, not the C++ class. BattleMechType::init sets
// the facet to DMG_MECH_8PART at type-load; the base/vehicle default is
// DMG_VEHICLE_FACINGS (UnitProfile's field initializer); an optional `.fit
// [UnitProfile] DamageSchema=` token can override either. PURE ACCESSOR — nobody
// reads it yet; it is the source a future resolveHit(HitRequest) and the Phase-2
// HardpointId validator will consult. A null type falls back to the base default
// so early-init paths never crash. Mirrors locomotionModel()'s const_cast: the
// getObjectType() legacy accessor is non-const but does not mutate.
DamageSchemaId Mover::damageSchema (void) const {
	ObjectTypePtr type = const_cast<Mover*>(this)->getObjectType();
	if (type)
		return type->profile().damageSchema;
	return DMG_VEHICLE_FACINGS;
}

// LOADOUT-HARDPOINT-JOIN-1 (Phase-2 slice 3). Map a weapon's coarse component
// FORM (the legacy Energy/Ballistic/Missile bucket getWeaponNode round-robins
// over) to the typed WeaponTag the hardpoint accept-set speaks. Returns false for
// non-weapon / unmapped forms so a bad slot is skipped rather than mis-tagged.
static bool weaponFormToTag (ComponentFormType form, WeaponTag& out) {
	switch (form) {
		case COMPONENT_FORM_WEAPON_ENERGY:    out = WTAG_ENERGY;    return true;
		case COMPONENT_FORM_WEAPON_BALLISTIC: out = WTAG_BALLISTIC; return true;
		case COMPONENT_FORM_WEAPON_MISSILE:   out = WTAG_MISSILE;   return true;
		default: return false;
	}
}

// Gated report-only loadout->hardpoint join validation. Walks inventory[],
// gathers only DECLARED joins (hardpointId != 0 AND resolvable to a profile),
// runs the pure validator, and logs findings. NO runtime/visual-mount change.
// Stock loadouts declare no hardpointId -> `joins` empty -> silent (parity).
void Mover::validateLoadoutHardpointJoins (const char* unitLabel) const {
	if (!mc2UnitProfileDataEnabled())
		return;

	const DamageSchemaId schema = damageSchema();
	std::vector<LoadoutHardpointJoin> joins;
	for (long i = 0; i < MAX_MOVER_INVENTORY_ITEMS; i++) {
		const InventoryItem& it = inventory[i];
		if (it.masterID == 0xff)
			continue;                       // empty slot
		if (it.hardpointId == 0)
			continue;                       // no declared hardpoint -> legacy CSV path
		HardpointProfile hp;
		if (!resolveHardpoint(it.hardpointId, hp))
			continue;                       // declared id has no profile (base: none)
		WeaponTag tag;
		if (!weaponFormToTag(MasterComponent::masterList[it.masterID].getForm(), tag))
			continue;                       // non-weapon slot carrying a hardpoint: skip
		LoadoutHardpointJoin j;
		j.hardpoint     = hp;
		j.weaponTag     = tag;
		j.unitSchema    = schema;
		// Socket presence is a model-node lookup this slice does not perform (that is
		// the getWeaponNode adapter's territory, slice 4). Report the OTHER two joins
		// now; treat the socket as present so we do not emit a spurious SOCKET_MISSING
		// here (the authoritative socket check lives in Mech3DAppearanceType::init).
		j.socketPresent = true;
		joins.push_back(j);
	}

	// UNIT-DEFINITION-VALIDATOR-1 (Phase-2 slice 8): route the loadout-join facet
	// through the ONE consolidated validateUnitDefinition() pass rather than the
	// standalone validateLoadoutHardpointJoins() entry. Only the loadoutJoins list
	// is populated at this instance-loadout site; the profile/socket/anim/UI inputs
	// stay empty -> zero extra findings, identical output to the old per-facet call.
	// The registry is passed for the (unexercised-here) jump facet so the single
	// entry never diverges from the runtime executor resolution.
	UnitDefinitionValidationInputs in;
	in.loadoutJoins = joins;
	const std::vector<UnitProfileFinding> findings =
		::validateUnitDefinition(in, theActionExecutorRegistry());
	for (const UnitProfileFinding& f : findings) {
		printf("[UnitDefinition:VALIDATE] %s: %s (reason=%d)\n",
		       unitLabel ? unitLabel : "<unknown>",
		       f.message ? f.message : "",
		       static_cast<int>(f.code));
	}
}

float Mover::getEcmRange (void) {

	if ((ecm != 255))
		return 10.0f;		//ANY ECM means I am invisible!!

	return(0.0);
}

bool Mover::hasNullSignature (void) {

	return(nullSignature != 255);
}

extern bool InitWayPath;
