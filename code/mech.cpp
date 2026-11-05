//	mech.cpp - BattleMech data tables plus the heat, status, speed and Save/Load methods
//	MechCommander 2
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include "mech_internal.h"


float ContactFadeTime = 2.0f;

bool invulnerableON = false;

long NumLocationCriticalSpaces[NUM_BODY_LOCATIONS] = {
	NUM_CRITICAL_SPACES_HEAD,
	NUM_CRITICAL_SPACES_CTORSO,
	NUM_CRITICAL_SPACES_LTORSO,
	NUM_CRITICAL_SPACES_RTORSO,
	NUM_CRITICAL_SPACES_LARM,
	NUM_CRITICAL_SPACES_RARM,
	NUM_CRITICAL_SPACES_LLEG,
	NUM_CRITICAL_SPACES_RLEG
};

long MechHitSectionTable[NUM_ATTACKSOURCES] = {
	MECH_HIT_SECTION_MIDDLE,	// Weapon Fire
	MECH_HIT_SECTION_MIDDLE,	// Collision
	MECH_HIT_SECTION_TOP,		// Death From Above
	MECH_HIT_SECTION_BOTTOM,	// Mine
	MECH_HIT_SECTION_MIDDLE		// Artillery
};

#ifdef USEHEAT
HeatEffectEntryPtr HeatEffectTable = nullptr;
long			   NumHeatLevels = 0;
#endif

// DEBUG HACK!!!!!
extern GameObjectPtr mech1;

float tonnageTurnRate[110] = 
{
	720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f, //0 through 9 tons
	720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f, //10 through 19 tons
	720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f, //20 through 29 tons
	720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f, //30 through 39 tons
	720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f, //40 through 49 tons
	720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f, //50 through 59 tons
	720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f, //60 through 69 tons
	720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f, //70 through 79 tons
	720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f, //80 through 89 tons
	720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f, //90 through 99 tons
	720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f,720.0f //100 through 109 tons
};

long stupidNewSageColumns[8] = 
{
   2,7,5,6,3,4,8,9
};

long adjClippedCell[8][2] = {
	{0, 0},
	{0, 2},
	{2, 2},
	{2, 4},
	{4, 4},
	{4, 6},
	{6, 6},
	{6, 0}
};

float MechClassScale[std::to_underlying(NUM_MECH_CLASSES)] = {
	0.0,
	15.0,
	40.0,
	75.0,
	200.0
};

float RankVersusChassisCombatModifier[NUM_WARRIOR_RANKS][std::to_underlying(NUM_MECH_CLASSES)] = {
	{0.0, 0.0, -5.0, -15.0, -25.0},
	{0.0, 5.0, 0.0, -5.0, -15.0},
	{0.0, 10.0, 5.0, 0.0, -5.0},
	{0.0, 15.0, 10.0, 5.0, 0.0}
};

float WeaponFireJumpTime = 3.0;

long AttackerMoveModifier[NUM_MECH_STATES] = {
	0,		// Parked
	0,		// Standing
	10,		// Walking
	20,		// Running
	10,		// Reverse
	5,		// Limping
	30,		// Jumping
	0,		// Fallen Forward
	0		// Fallen Backward
};

char CriticalHitTable[4] = {
	58,		// 0 Hits
	83,		// 1 Hits
	97,		// 2 Hits
	100		// Head\Limb Destroyed or 3 Hits if Torso
};

long TargetMoveModifierTable [5][2] = {
	{6, 0},
	{12, 1},
	{18, 2},
	{27, 3},
	{999, 4}
};

float MechClassWeights[std::to_underlying(NUM_MECH_CLASSES)] =
{
	0.0,
	35.0,
	55.0,
	75.0,
	100.0
};

#ifdef USEHEAT
float HeatCheckFrequency = 10.0;
float StandUpHeat = 15.0;
float BodyStateHeat[NUM_MECH_STATES] = {
	0.0,	// Parked
	0.0,	// Standing
	0.15,	// Walking
	0.3,	// Running
	0.15,	// Reverse
	0.1,	// Limping
	0.75,	// Jumping
	0.0,	// Fallen Forward
	0.0		// Fallen Backward
};
#endif

char MechHitLocationTable[NUM_MECH_HIT_SECTIONS * NUM_MECH_HIT_ARCS * NUM_MECH_ARMOR_LOCATIONS] = {
	30,  20,  25,  25,   0,   0,   0,   0,   0,   0,   0,	// Top Front
	30,   0,   0,   0,   0,   0,   0,   0,  20,  25,  25,	// Top Rear
	20,  15,  25,   0,   0,   0,   0,   0,  15,  25,   0,	// Top Left
	20,  15,   0,  25,   0,   0,   0,   0,  15,   0,  25,	// Top Right
	 0,  20,  20,  20,  20,  20,   0,   0,   0,   0,   0,	// Middle Front
	 0,   0,   0,   0,  20,  20,   0,   0,  20,  20,  20,	// Middle Rear
	 0,   0,  25,   0,  50,   0,   0,   0,   0,  25,   0,	// Middle Left
	 0,   0,   0,  25,   0,  50,   0,   0,   0,   0,  25,	// Middle Right
	 0,  10,   0,   0,   0,   0,  45,  45,   0,   0,   0,	// Bottom Front
	 0,   0,   0,   0,   0,   0,  45,  45,  10,   0,   0,	// Bottom Rear
	 0,   0,   0,   0,   0,   0, 100,   0,   0,   0,   0,	// Bottom Left
	 0,   0,   0,   0,   0,   0,   0, 100,   0,   0,   0	// Bottom Right
};

char MechTransferHitTable[NUM_MECH_BODY_LOCATIONS] = {
	MECH_BODY_LOCATION_CTORSO,
	-1,
	MECH_BODY_LOCATION_CTORSO,
	MECH_BODY_LOCATION_CTORSO,
	MECH_BODY_LOCATION_LTORSO,
	MECH_BODY_LOCATION_RTORSO,
	MECH_BODY_LOCATION_LTORSO,
	MECH_BODY_LOCATION_RTORSO
};

char MechArmorToBodyLocation[NUM_MECH_ARMOR_LOCATIONS] = {
	MECH_BODY_LOCATION_HEAD,
	MECH_BODY_LOCATION_CTORSO,
	MECH_BODY_LOCATION_LTORSO,
	MECH_BODY_LOCATION_RTORSO,
	MECH_BODY_LOCATION_LARM,
	MECH_BODY_LOCATION_RARM,
	MECH_BODY_LOCATION_LLEG,
	MECH_BODY_LOCATION_RLEG,
	MECH_BODY_LOCATION_CTORSO,
	MECH_BODY_LOCATION_LTORSO,
	MECH_BODY_LOCATION_RTORSO,
};

long MechPilotCheckConditions[2] = {
	25,
	25
};

long MechPilotCheckTerrainEffect[64] = {
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0
};

#ifdef USEHEAT
long HeatInjuryTable[2][2] = {
	{15, 1},
	{26, 2}
};
float heatSinkEfficiency = 1.0;
long heatShutdown = 30;
#endif

extern long AimedFireAbort;

float mechCollisionThreshold = 0.0;
float objectCollisionThreshold = 0.0;
float tonnageCollisionThreshold = 0.0;
float treeDeflection = 0.0;
float mechPivotAngle = 0.0;
float mechPivotThrottle = 0.0;

float MaxVelocityMag = 0.0;
float NewRotation = 0.0;

long DefaultMechCrashAvoidSelf = 1;
long DefaultMechCrashAvoidPath = 1;
long DefaultMechCrashBlockSelf = 1;
long DefaultMechCrashBlockPath = 1;
float DefaultMechCrashYieldTime = 2.0;
long DefaultMechJumpCost = COST_BLOCKED / 3;

GameObjectPtr BadGuy = nullptr;

//#define USE_MECHDEBUGFILE
#ifdef USE_MECHDEBUGFILE
UserFile* MechDebugFile = nullptr;
#endif

extern bool ShowMovers;

#ifdef USEHEAT
long BattleMech::calcHeatSinks (long location, bool calcCurrent) {

	// IMPORTANT: This needs to be properly implemented!
	long tally = 0;
	for (long item = 0; item < numOther; item++) {
		if (MasterComponent::masterList[inventory[item].masterID].getForm() == COMPONENT_FORM_HEATSINK)
			if (inventory[item].bodyLocation == location)
				if (!calcCurrent || !inventory[item].disabled)
					tally++;
	}
	return(tally);
}
#endif

long BattleMech::calcCV (bool calcMax) {

	// Due to quality communication, the components have float BR ratings, but
	// the mechs have integer BR ratings. Ahhhh, love those casts...
	float totalCV = static_cast<float>(chassisBR);

	// Total Component BRs...
	long numComponents = numWeapons + numAmmos + numOther;
	for (long itemIndex = 0; itemIndex < numComponents; itemIndex++)
		if (calcMax || !inventory[itemIndex].disabled)
			totalCV += MasterComponent::masterList[inventory[itemIndex].masterID].getCV();

	return(static_cast<long>(totalCV));
}

long BattleMech::calcLegStatus (void)
{
	if (body[MECH_BODY_LOCATION_RLEG].damageState == IS_DAMAGE_DESTROYED)
	{
		if (body[MECH_BODY_LOCATION_LLEG].damageState == IS_DAMAGE_DESTROYED)
		{
			legStatus = LEG_STATUS_DESTROYED;
			if (pilot && !inRecoverUpdate)
			{
				pilot->triggerAlarm(PILOT_ALARM_VEHICLE_INCAPACITATED, 66);
			}
		}
		else
		{
			if (!sentCrippledMsg && !inRecoverUpdate)
			{
				pilot->radioMessage(RADIO_CRIPPLED);
				legStatus = LEG_STATUS_IMPAIRED_RIGHT;
				sentCrippledMsg = true;
			}
		}
	}
	else if (body[MECH_BODY_LOCATION_LLEG].damageState == IS_DAMAGE_DESTROYED)
	{
		if (!sentCrippledMsg && !inRecoverUpdate)
		{
			pilot->radioMessage(RADIO_CRIPPLED);
			legStatus = LEG_STATUS_IMPAIRED_LEFT;
			sentCrippledMsg = true;
		}
	}
	else
	{
		legStatus = LEG_STATUS_NORMAL;
	}

	return(legStatus);
}

long BattleMech::calcTorsoStatus (void)
{
	if (body[MECH_BODY_LOCATION_CTORSO].damageState == IS_DAMAGE_PARTIAL)
		torsoStatus = TORSO_STATUS_IMPAIRED;
	else
		torsoStatus = TORSO_STATUS_NORMAL;

	return(torsoStatus);
}

void BattleMech::pilotingCheck (unsigned long situation, float modifier) {

	if (MPlayer && !MPlayer->isServer())
		return;

	if (failedPilotingCheck)
		return;

	float pilotRoll = RandomNumber(100);

	// Mech's situation...
//	if (situation & PILOTCHECK_SITUATION_JUMPING)
//		pilotRoll += 20.0;
	if (situation & PILOTCHECK_SITUATION_COLLISION)
		pilotRoll += 20.0;

	// If leg(s) gone, it's hopeless...
	if ((body[MECH_BODY_LOCATION_RLEG].curInternalStructure == 0) || (body[MECH_BODY_LOCATION_LLEG].curInternalStructure == 0))
		pilotRoll += 100.0;

	// Is gyro damaged?
	if (gyro >= (numOther+numWeapons+numAmmos))
		STOP(("Gyro Was not loaded correctly and would have crashed here %d",gyro));

	long gyroHealth = inventory[gyro].health;
	if (gyroHealth == 0)
		pilotRoll += 100.0;
	else if (gyroHealth < getInventoryMax(gyro))
		pilotRoll += 30.0;

	// Leg actuator(s) destroyed?
	//sebi: to not read uninitialized memory
	if(actuator[ACTUATOR_LHIP] < MAX_MOVER_INVENTORY_ITEMS)
		if (inventory[actuator[ACTUATOR_LHIP]].health == 0)
			pilotRoll += 10.0;

	//sebi: ORIG BUG FIX to not read uninitialized memory (ApplicatioVerifier)
	if(actuator[ACTUATOR_RHIP] < MAX_MOVER_INVENTORY_ITEMS)
		if (inventory[actuator[ACTUATOR_RHIP]].health == 0)
			pilotRoll += 10.0;
	
	// Terrain type...
	//long terrainType = GameMap->getTileType(tilePosition[0], tilePosition[1]);

	// So, did we pass?
	{
		failedPilotingCheck = (pilotRoll >= pilot->getSkill(MWS_PILOTING));
		pilot->skillPoints[MWS_PILOTING] += SkillTry[MWS_PILOTING];
		if (!failedPilotingCheck)
			pilot->skillPoints[MWS_PILOTING] += SkillSuccess[MWS_PILOTING];
	}
}

bool BattleMech::canPowerUp (void)
{
#ifdef USEHEAT
	long heatIndex = static_cast<long>(heat);

	if (heatIndex > 0)
	{
		if (heatIndex >= NumHeatLevels)
			heatIndex = NumHeatLevels - 1;

		// Check if we've cooled enough to restart...
		if (heatIndex >= heatShutdown)
			return(false);
	}
#endif
	return(true);
}

#ifdef USEHEAT
void BattleMech::updateHeat (void)
{
	float heatChange = 0.0;

	if (!isDisabled()) {
		if ((reinterpret_cast<MechActor*>(appearance))->currentGestureId == GestureGetUp)
			heatChange += (StandUpHeat * frameLength);
		else
			heatChange += (BodyStateHeat[getBodyState()] * frameLength);

		// Engine damage may cause heat buildup...
		long numEngineHits = getInventoryDamage(engine);
		if (numEngineHits > 2)
			numEngineHits = 2;
		heatChange += (numEngineHits * 0.6 * frameLength);
	}

	// The terrain we're in may affect our heat...
	long terrainHere = GameMap->getTerrain(position);
	switch (terrainHere) {
		case TILE_WATER/*_SHALLOW*/: {
			float heatSinkEffect = (curLegHeatSinks * -0.15);
			if (heatSinkType == HEATSINK_TYPE_DOUBLE)
				heatSinkEffect *= 2.0;
			if (heatSinkEffect < -0.9)
				heatSinkEffect = -0.9;
			heatChange += heatSinkEffect;
		}
		break;
	}

	// Apply weapon heat...
	for (long weaponIndex = numOther; weaponIndex < (numOther + numWeapons); weaponIndex++)
		if (inventory[weaponIndex].readyTime > scenarioTime) {
			// Weapon is recycling, so apply the heat...
			heatChange += (inventory[weaponIndex].heatPerSec * frameLength);
		}

	// Now, apply the dissipation amount (never allowing heat to go below
	// zero, of course)...
	heatChange -= (heatDissipation * frameLength * heatSinkEfficiency);

	heat += heatChange;
	if (heat < 0.0)
		heat = 0.0;

	long heatIndex = static_cast<long>(heat);

	if (!isDisabled()) {
		if (inventory[lifeSupport].disabled) {
			if (heatIndex >= HeatInjuryTable[1][0])
				pilot->injure(HeatInjuryTable[1][1] * frameLength, true);
			else if (heatIndex >= HeatInjuryTable[0][0])
				pilot->injure(HeatInjuryTable[0][1] * frameLength, true);
		}
	}

	// Now, let's make a HEAT CHECK.
	// NOTE: Disabled vehicles CAN still overheat and explode...
	if ((heatCheckTime < scenarioTime) && !isDisabled())
	{
		if (heatIndex >= NumHeatLevels)
			heatIndex = NumHeatLevels - 1;

		if (heatIndex == 0)
		{
			if (status == OBJECT_STATUS_SHUTDOWN)
			{
				if (pilot->getCurTacOrder()->code != TACTICAL_ORDER_POWERDOWN)
					startUp();
			}
		}
		else if (heatIndex > 0)
		{
			if (status == OBJECT_STATUS_SHUTDOWN)
			{
				if ((pilot->getCurTacOrder()->code != TACTICAL_ORDER_POWERDOWN) && (pilot->getCurTacOrder()->code != TACTICAL_ORDER_POWERUP))
				{
					// Check if we've cooled enough to restart...
					if (canPowerUp())
						startUp();
				}
			}
			else
			{
				if ((status != OBJECT_STATUS_DISABLED) && !canPowerUp())
				{
					// Shut Down...
					shutDown();
					pilot->triggerAlarm(PILOT_ALARM_OVERHEAT, heatIndex);
				}

				// This can't happen here this way.  No randoms!!!!!!!!!!!!!
				// This should set a flag a in ControlData to inform the update to blow the ammo!!
				// In this way, we can still play network MechCommander.
				if (RandomNumber(100) < HeatEffectTable[heatIndex - 1].ammoExplosionChance)
				{
					long numExplodables = 0;
					long ammoList[100];
					for (long ammoIndex = numOther + numWeapons; ammoIndex < (numOther + numWeapons + numAmmos); ammoIndex++)
					{
						if (!inventory[ammoIndex].disabled)
							ammoList[numExplodables++] = ammoIndex;
					}

					if (numExplodables > 0)
					{
						MechControlDataPtr mechCtrlData = static_cast<MechControlDataPtr>((control->controlData));
						mechCtrlData->blowAmmo = true;
						mechCtrlData->ammoLocation = ammoList[RandomNumber(numExplodables)];
					}
				}
			}
		}

		heatCheckTime += HeatCheckFrequency;
	}
}
#endif

void BattleMech::destroy (void)
{
	if (appearance)
	{
	    // M2: retire RenderWorld handle BEFORE deleting the appearance so the
	    // adapter can read the handle via getRenderWorldHandle(). No-op if
	    // already retired (valid handle check is inside destroyMech).
	    // static_cast safe: appearance is always Mech3DAppearance* in
	    // BattleMech (its only assignment site is BattleMech::init).
	    {
	        Mech3DAppearance* m3d = static_cast<Mech3DAppearance*>(appearance);
	        GameAdapters::Mech::destroyMech(*m3d);
	    }
		releaseAppearance();
	}
}

long BattleMech::getResourcePointValue (void) {

	long resourcePointsForMechs[15][2] = {
		{30, 1500},
		{35, 1750},
		{40, 2000},
		{45, 2250},
		{50, 3000},
		{55, 3300},
		{60, 3600},
		{65, 3900},
		{70, 4200},
		{75, 5250},
		{80, 5600},
		{85, 5950},
		{90, 6300},
		{95, 6650},
		{100, 7000}
	};

	for (auto & resourcePointsForMech : resourcePointsForMechs)
		if (tonnage <= resourcePointsForMech[0])
			return(resourcePointsForMech[1]);
	return(resourcePointsForMechs[14][1]);
};

void BattleMech::mineCheck (void) 
{
	if (MPlayer && !MPlayer->isServer())
		return;

	if (isJumping())	//Can't set 'em off while flying!!
		return;
	
	//Can't have those damned helicopters setting stuff off.
	if ((getMoveLevel() == 2) && (status != OBJECT_STATUS_SHUTDOWN))
		return;
		
	// Must mark the end of the path as desirable or much badness
	// NOT NEEDED ANYMORE.  Everyone can see mines all of the time.
	unsigned long mine = 0;
   	mine = GameMap->getMine(cellPositionRow, cellPositionCol);
		
	if (mine == 1)
	{
		// Mine here, deal with it...
		if ((tonnage > 35.0f))
		{
			//  Mark this tile as empty.  (we just set the mine off!!)			
			GameMap->setMine(cellPositionRow, cellPositionCol, 2);
			if (MPlayer)
				MPlayer->addMineChunk(cellPositionRow,
									  cellPositionCol,
									  1,
									  2,
									  2);
				
			pilot->clearCurTacOrder();		//Force the pilot to recalc based on new data.
			
			// Mine here, deal with it...
			glm::vec3 explosionPosition;
			explosionPosition = getPosition();
			ObjectManager->createExplosion(MINE_EXPLOSION_ID, nullptr, explosionPosition, MineSplashDamage, MineSplashRange * worldUnitsPerMeter);
			
			WeaponShotInfo shot;
			shot.init(0, -2, MineDamage, calcHitLocation(nullptr,-1,ATTACKSOURCE_MINE,0), 0);
			handleWeaponHit(&shot, (MPlayer != nullptr));
		}
	}
}

float BattleMech::calcMaxSpeed (void) {

	if (canRun())
		return(appearance->getVelocityOfGesture(GestureRun));
	else if (canWalk())
		return(appearance->getVelocityOfGesture(GestureWalk));
	else if (canLimp())
		return(appearance->getVelocityOfGesture(GestureLimpLeft));
	return(0.0);
}

float BattleMech::calcSlowSpeed (void) {

	if (canWalk())
		return(maxMoveSpeed * 0.25);
	else if (canLimp())
		return(maxMoveSpeed * 0.2);
	return(0.0);
}

float BattleMech::calcModerateSpeed (void) {

	if (canWalk())
		return(maxMoveSpeed * 0.4);
	else if (canLimp())
		return(maxMoveSpeed * 0.3);
	return(0.0);
}

long BattleMech::calcSpriteSpeed (float speed, unsigned long flags, long& state, long& throttle) {

	state = MECH_STATE_RUNNING;
	throttle = 100;

	// If the speed we want falls between the max walk speed and the run speed
	// of the mech, we use the run speed (we'd rather have them stop and wait
	// for others to catch up than always fall behind). If the speed is
	// greater than the mech's fastest speed, we select the run state, but
	// also return an error code...
	long errCode = 0;
	float maxWalkSpeed = appearance->getVelocityOfGesture(GestureWalk);
	float minWalkSpeed = maxWalkSpeed / 2.0;
	float runSpeed = appearance->getVelocityOfGesture(GestureRun);

	if (speed == 0.0)
		state = MECH_STATE_STANDING;
	else if (speed < minWalkSpeed) {
		state = MECH_STATE_WALKING;
		throttle = 50;
		errCode = 1;
		}
	else if (speed <= maxWalkSpeed) {
		state = MECH_STATE_WALKING;
		throttle = static_cast<long>((speed / maxWalkSpeed * 100.0));
		}
	else if (speed < runSpeed) {
		if (flags & MECH_SPRITESPEED_FLAG_GO_SLOW) {
			state = MECH_STATE_WALKING;
			throttle = static_cast<long>((speed / maxWalkSpeed * 100.0));
			}
		else
			state = MECH_STATE_RUNNING;
		errCode = 2;
		}
	else if (speed > runSpeed) {
		state = MECH_STATE_RUNNING;
		errCode = 3;
	}

	return(errCode);
}

bool BattleMech::isCaptureable (long capturingTeamID) {

	// The following seems wrong, but we've opted to leave it so we don't upset the
	// BETA gods. This function should never get called anyway, we think...hehe...3/20/01
	return(getFlag(OBJECT_FLAG_CAPTURABLE) && (getTeam() == Team::home) && !isDestroyed());
}

float BattleMech::calcMaxTargetDamage(void)
{
float			damage = 0;

	for (long curWeapon = numOther; curWeapon < numWeapons + numOther; curWeapon++)
	{
	long	numClusters, clusterSize;
	float	thisDamage;

		numClusters = clusterSize = 1;
		if (MasterComponent::masterList[inventory[curWeapon].masterID].getForm() == COMPONENT_FORM_WEAPON_MISSILE)
		{
			// It's a missile weapon. We need to determine how many hit
			// (and missed) the target, and in how many clusters...
			long numMissiles = MasterComponent::masterList[inventory[curWeapon].masterID].getWeaponAmmoAmount() / 2;
			switch (MasterComponent::masterList[inventory[curWeapon].masterID].getWeaponAmmoType())
			{
				case WEAPON_AMMO_SRM:
					numClusters = numMissiles / ClusterSizeSRM;
					if (numMissiles % ClusterSizeSRM)
						numClusters++;
					clusterSize = ClusterSizeSRM;
					break;
				case WEAPON_AMMO_LRM:
					numClusters = numMissiles / ClusterSizeLRM;
					if (numMissiles % ClusterSizeLRM)
						numClusters++;
					clusterSize = ClusterSizeLRM;
					break;
			};
		}
		thisDamage = clusterSize * numClusters * MasterComponent::masterList[inventory[curWeapon].masterID].getWeaponDamage() * 10;
		thisDamage /= MasterComponent::masterList[inventory[curWeapon].masterID].getWeaponRecycleTime();
		thisDamage *= 100;	// 100% chance to hit
		if (thisDamage > 0)
			damage += thisDamage;
	}
		
	maxWeaponDamage = damage;
	return damage;
}

bool BattleMech::isWeaponWorking (long weaponIndex)
{
//long bodyLocation = inventory[weaponIndex].bodyLocation;

//	if ((bodyLocation == MECH_BODY_LOCATION_LARM) && inventory[actuator[ACTUATOR_LSHOULDER]].disabled)
//		return(false);
//	else if ((bodyLocation == MECH_BODY_LOCATION_RARM) && inventory[actuator[ACTUATOR_RSHOULDER]].disabled)
//		return(false);
//	else
	if (inventory[weaponIndex].disabled)
		return(false);
	else if (getWeaponShots(weaponIndex) == 0)
		return (false);
	return(true);
}

float BattleMech::getTotalEffectiveness(void)
{
float weaponEffect, result;
float curCTorso, maxCTorso, armorCTorso;
float maxHead, curHead, armorHead, armorEffect, curArm, maxArm, armorArm, curLeg, maxLeg, armorLeg;
float curTorso, maxTorso, armorTorso, pilotEffect;

	// calculate Weapon effectiveness
	if (0.0 == maxWeaponEffectiveness)
	{
		gosASSERT(false);
		maxWeaponEffectiveness = 1.0;
	}

	if (weaponEffectiveness > maxWeaponEffectiveness)
		maxWeaponEffectiveness = weaponEffectiveness;

	weaponEffect = weaponEffectiveness / maxWeaponEffectiveness;

	// Calculate armor effectiveness.
	// DO NOT CHANGE!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	// COmpiler does funky things if we just return 0.0!
	if (isDestroyed() || isDisabled())
	{
		armorEffect = 0.0;
	}
	else
	{
		maxHead = armor[MECH_ARMOR_LOCATION_HEAD].maxArmor;
		curHead = armor[MECH_ARMOR_LOCATION_HEAD].curArmor;
		armorHead = curHead / maxHead * 0.6 + (0.4);
		
		curCTorso = armor[MECH_ARMOR_LOCATION_CTORSO].curArmor;
		maxCTorso = armor[MECH_ARMOR_LOCATION_CTORSO].maxArmor;
		if (curCTorso > armor[MECH_ARMOR_LOCATION_RCTORSO].curArmor)
		{
			curCTorso = armor[MECH_ARMOR_LOCATION_RCTORSO].curArmor;
			maxCTorso = armor[MECH_ARMOR_LOCATION_RCTORSO].maxArmor;
		}
		armorCTorso = curCTorso / maxCTorso * 0.5 + (0.5);
		
		curTorso = armor[MECH_ARMOR_LOCATION_RLTORSO].curArmor + 
							armor[MECH_ARMOR_LOCATION_LTORSO].curArmor +
							armor[MECH_ARMOR_LOCATION_RRTORSO].curArmor +
							armor[MECH_ARMOR_LOCATION_RTORSO].curArmor;
		maxTorso = armor[MECH_ARMOR_LOCATION_RLTORSO].maxArmor + 
							armor[MECH_ARMOR_LOCATION_LTORSO].maxArmor+
							armor[MECH_ARMOR_LOCATION_RRTORSO].maxArmor +
							armor[MECH_ARMOR_LOCATION_RTORSO].maxArmor;
		armorTorso = curTorso / maxTorso * 0.25 + (0.75);

		curArm = armor[MECH_ARMOR_LOCATION_LARM].curArmor + armor[MECH_ARMOR_LOCATION_RARM].curArmor;
		maxArm = armor[MECH_ARMOR_LOCATION_LARM].maxArmor + armor[MECH_ARMOR_LOCATION_RARM].maxArmor;
		armorArm = curArm / maxArm * 0.25 + (0.75);

		curLeg = armor[MECH_ARMOR_LOCATION_LLEG].curArmor + armor[MECH_ARMOR_LOCATION_RLEG].curArmor;
		maxLeg = armor[MECH_ARMOR_LOCATION_LLEG].maxArmor + armor[MECH_ARMOR_LOCATION_RLEG].maxArmor;
		armorLeg = curArm / maxArm * 0.4 + (0.6);

		armorEffect = armorHead * armorCTorso * armorTorso * armorArm * armorLeg;
	}
	
	float pilotWoundTable[7] = 
	{
		1.0,0.95f,0.85f,0.75f,0.50f,0.30f,0.00
	};
	
	unsigned long pilotWounds = float2long(getPilot()->getWounds());
	if (pilotWounds > 6)
		pilotWounds = 6;
		
	pilotEffect = pilotWoundTable[pilotWounds];
	
	result = weaponEffect * armorEffect * pilotEffect;
	return result;
}

void BattleMech::damageLoadedComponents (void)
{
	for (long curLocation = 0; curLocation < NUM_MECH_BODY_LOCATIONS; curLocation++) 
	{
		long numSpaces = NumLocationCriticalSpaces[curLocation];
		
		for (long curSpace = 0; curSpace < numSpaces; curSpace++)
		{
			if (body[curLocation].criticalSpaces[curSpace].hit)
				hitInventoryItem(body[curLocation].criticalSpaces[curSpace].inventoryID, true);
		}
	}
}	

#include "save_struct_zero.h"
#include "mech_value_codec.h"
void BattleMech::Save (PacketFilePtr file, long packetNum)
{
	MC2_SAVE_STRUCT(MechData, data);
	CopyTo(&data);

	//PacketNum incremented in ObjectManager!!
	auto bytes = mc2::save::encodeTaggedRecord(mc2::save::MechRecord{data}, mc2::save::mechTag);
	if (bytes.empty()) Fatal(0, "Invalid mech ObjectSlot");
	file->writePacket(packetNum, bytes.data(), static_cast<unsigned int>(bytes.size()), STORAGE_TYPE_ZLIB);
}

void BattleMech::CopyTo (MechData *data)
{																	   
	data->chassisClass				                            = chassisClass;                                        
	data->chassisBR 					                        = chassisBR;                                           
																													 
	data->variantID 					                        = variantID;                                           

	memcpy(data->variantName,variantName,sizeof(char) * 64);
																													 
	data->legStatus 					                        = legStatus;                                           
	data->torsoStatus 				                            = torsoStatus;                                         
																													 
	memcpy(data->actuator,actuator, sizeof(unsigned char) * NUM_ACTUATORS);

	data->gyro 						                            = gyro;                                                

	if (sensorSystem)
		data->sensorOK											= !sensorSystem->broken;
	else
		data->sensorOK											= false;

	data->numJumpJets                                           = numJumpJets;                                         
	data->lastJumpTime                                          = lastJumpTime;                                        
	data->inJump                                                = inJump;                                              
	data->jumpGoal                                              = jumpGoal;                                            
	data->centerTorsoBlowTime                                   = centerTorsoBlowTime;                                 
	data->hitFromBehindThisFrame                                = hitFromBehindThisFrame;                              
	data->hitFromFrontThisFrame                                 = hitFromFrontThisFrame;                               
																													 
	data->leftArmBlownThisFrame                                 = leftArmBlownThisFrame;                               
	data->rightArmBlownThisFrame                                = rightArmBlownThisFrame;                              
																													 
	data->torsoRotation 				                        = torsoRotation;                                       
	data->rightArmRotation			                            = rightArmRotation;                                    
	data->leftArmRotation 			                            = leftArmRotation;                                     
																													 
	data->fallen 						                        = fallen;                                              
	data->mechRemoved 				                            = mechRemoved;                                         
																													 
	data->pivotTurnRate                                         = pivotTurnRate;                                       
																													 
	data->playedJumpSFX 				                        = playedJumpSFX;                                       
	data->playedCriticalHit 			                        = playedCriticalHit;                                   
																													 
	data->maxWeaponDamage 			                            = maxWeaponDamage;                                     
																													 
	memcpy(data->longName,longName,sizeof(char) * MAXLEN_MECH_LONGNAME);

	data->pilotNum		                                        = pilotNum;                                            
																													 
	data->captureable                                           = captureable;                                         
	data->notMineYet		                                    = notMineYet;                                          
																													 
	data->descID 			                                    = descID;                                              
																													 
	// One sim turn's damage sum; a load starts a new turn, so the save carries zero.
	data->damageThisFrame                                       = 0.0f;
	data->sentCrippledMsg                                       = sentCrippledMsg;                                     
																													 
	memcpy(data->rotateValues,rotateValues, sizeof(float) * 6);
																													 
	memcpy(data->ItemLocationToInvLocation,ItemLocationToInvLocation, sizeof(int32_t) * MAX_MOVER_INVENTORY_ITEMS);  
																													 
	data->damageAfterDisabled                                   = damageAfterDisabled;                                 
																													 
	data->numArmorComponents                                    = numArmorComponents;                                  
	data->cBills                                                = cBills;                                              

	if (appearance)
		static_cast<Mech3DAppearance *>(appearance)->copyTo(&(data->apData));

	Mover::CopyTo(data);
}

void BattleMech::Load (MechData *data)
{
	Mover::Load(data);

	chassisClass				                        = data->chassisClass;                                        
	chassisBR 					                        = data->chassisBR;                                           
																												 
	variantID 					                        = data->variantID;                                           

	memcpy(variantName,data->variantName,sizeof(char) * 64);
																													 
	legStatus 					                        = data->legStatus;                                           
	torsoStatus 				                        = data->torsoStatus;                                         
																													 
	memcpy(actuator,data->actuator, sizeof(unsigned char) * NUM_ACTUATORS);

	gyro 						                        = data->gyro;                                                
																													 
	numJumpJets                                           = data->numJumpJets;                                         
	lastJumpTime                                          = data->lastJumpTime;                                        
	inJump                                                = data->inJump;                                              
	jumpGoal                                              = data->jumpGoal;                                            
	centerTorsoBlowTime                                   = data->centerTorsoBlowTime;                                 
	hitFromBehindThisFrame                                = data->hitFromBehindThisFrame;                              
	hitFromFrontThisFrame                                 = data->hitFromFrontThisFrame;                               
																												 
	leftArmBlownThisFrame                                 = data->leftArmBlownThisFrame;                               
	rightArmBlownThisFrame                                = data->rightArmBlownThisFrame;                              
																												 
	torsoRotation 				                          = data->torsoRotation;                                       
	rightArmRotation			                          = data->rightArmRotation;                                    
	leftArmRotation 			                          = data->leftArmRotation;                                     
																												 
	fallen 						                          = data->fallen;                                              
	mechRemoved 				                          = data->mechRemoved;                                         
																												 
	pivotTurnRate                                         = data->pivotTurnRate;                                       
																												 
	playedJumpSFX 				                          = data->playedJumpSFX;                                       
	playedCriticalHit 			                          = data->playedCriticalHit;                                   
																												 
	maxWeaponDamage 			                          = data->maxWeaponDamage;                                     
																													 
	memcpy(longName,data->longName,sizeof(char) * MAXLEN_MECH_LONGNAME);

	pilotNum		                                      = data->pilotNum;                                            
																												 
	captureable                                           = data->captureable;                                         
	notMineYet		                                      = data->notMineYet;                                          
																												 
	descID 			                                      = data->descID;                                              
																												 
	damageThisFrame                                       = data->damageThisFrame;                                     
	sentCrippledMsg                                       = data->sentCrippledMsg;                                     
																													 
	memcpy(rotateValues,data->rotateValues, sizeof(float) * 6);
																													 
	memcpy(ItemLocationToInvLocation,data->ItemLocationToInvLocation, sizeof(int32_t) * MAX_MOVER_INVENTORY_ITEMS);  
																													 
	damageAfterDisabled                                   = data->damageAfterDisabled;                                 
																												 
	numArmorComponents                                    = data->numArmorComponents;                                  
	cBills                                                = data->cBills;                                              

	if (appearance)
		static_cast<Mech3DAppearance *>(appearance)->copyFrom(&(data->apData));

	sensorSystem = SensorManager->newSensor();
	sensorSystem->setOwner(this);
	sensorSystem->setRange(MasterComponent::masterList[inventory[sensor].masterID].getSensorRange());
	SensorManager->addTeamSensor(teamId, sensorSystem);

	if (!data->sensorOK)
		sensorSystem->broken = true;

	// UNITPROFILE-TYPELOAD-SOURCE-1: numJumpJets is authoritative only after Load;
	// fold it into the type baseline + this instance's runtime via the single owner.
	refreshDerivedCapabilities();
}

void BattleMech::repairAll (void)
{
	armor[MECH_ARMOR_LOCATION_HEAD].curArmor = armor[MECH_ARMOR_LOCATION_HEAD].maxArmor;
	armor[MECH_ARMOR_LOCATION_CTORSO].curArmor = armor[MECH_ARMOR_LOCATION_CTORSO].maxArmor;
	armor[MECH_ARMOR_LOCATION_LTORSO].curArmor = armor[MECH_ARMOR_LOCATION_LTORSO].maxArmor;
	armor[MECH_ARMOR_LOCATION_RTORSO].curArmor = armor[MECH_ARMOR_LOCATION_RTORSO].maxArmor;
	armor[MECH_ARMOR_LOCATION_LARM].curArmor = armor[MECH_ARMOR_LOCATION_LARM].maxArmor;
	armor[MECH_ARMOR_LOCATION_RARM].curArmor = armor[MECH_ARMOR_LOCATION_RARM].maxArmor;
	armor[MECH_ARMOR_LOCATION_LLEG].curArmor = armor[MECH_ARMOR_LOCATION_LLEG].maxArmor;
	armor[MECH_ARMOR_LOCATION_RLEG].curArmor = armor[MECH_ARMOR_LOCATION_RLEG].maxArmor;
	armor[MECH_ARMOR_LOCATION_RCTORSO].curArmor = armor[MECH_ARMOR_LOCATION_RCTORSO].maxArmor;
	armor[MECH_ARMOR_LOCATION_RLTORSO].curArmor = armor[MECH_ARMOR_LOCATION_RLTORSO].maxArmor;
	armor[MECH_ARMOR_LOCATION_RRTORSO].curArmor = armor[MECH_ARMOR_LOCATION_RRTORSO].maxArmor;

	// Now, read in the component layout for each body location...
	for (long curLocation = 0; curLocation < NUM_MECH_BODY_LOCATIONS; curLocation++) 
	{
		body[curLocation].curInternalStructure = body[curLocation].maxInternalStructure;
		body[curLocation].damageState = IS_DAMAGE_NONE;

		long numSpaces = NumLocationCriticalSpaces[curLocation];
		for (long curSpace = 0; curSpace < numSpaces; curSpace++)
		{
			body[curLocation].criticalSpaces[curSpace].hit = false;
		}
	}

	for (auto & curItem : inventory)
	{
		curItem.health = 255;
		curItem.disabled = false;
	}

	calcAmmoTotals();

	// We need to set the status states for legs and torso based upon our current
	// condition...
	calcLegStatus();
	calcTorsoStatus();
	calcFireRanges();

	maxCV = calcCV(true);
	curCV = calcCV(false);
	setThreatRating(-1);

	maxWeaponDamage = calcMaxTargetDamage();

	sensorSystem->broken = false;
}
