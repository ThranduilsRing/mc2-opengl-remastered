// BattleMech: fireGateTrace, fireWeapon and handleWeaponFire (kept together: the order-gate contract reads both).
// Declared in the class headers; shared names in mech_internal.h.
#include "mech_internal.h"


// ISSUE-17 (MC2_FIRE_DECISION_TRACE, default OFF = byte-identical): fireWeapon
// has silent non-zero early returns — the combat decision tree said FIRE
// (weaponsStatus > 0) but the weapon quietly skipped (body state, ready,
// terrain, range re-check, shots, per-hotspot ripple-recycle). Emits one COMBAT
// JSONL event per refusal so 'decision said fire but nothing came out' is
// classifiable headlessly. Pairs with the 'nofire' event in combatDecisionTree.
static void fireGateTrace (unsigned long wid, long weaponIndex, const char* site)
{
	static const bool s_fireDecisionTrace =
		(getenv("MC2_FIRE_DECISION_TRACE") != NULL && getenv("MC2_FIRE_DECISION_TRACE")[0] != '0');
	if (!s_fireDecisionTrace || !mc2_diag::tagEnabled("COMBAT"))
		return;
	char _fg[128];
	snprintf(_fg, sizeof(_fg),
		"{\"ev\":\"firegate\",\"wid\":%lu,\"widx\":%ld,\"site\":\"%s\"}",
		wid, weaponIndex, site);
	mc2_diag::writeEvent("COMBAT", 1, 0, _fg);
}

long BattleMech::fireWeapon (GameObjectPtr target, float targetTime, long weaponIndex, long attackType, long aimLocation, Stuff::Vector3D* targetPoint, float &dmgDone) {

//	if (getTeam() != Team::home)
//		return(1);

	if ((status == OBJECT_STATUS_SHUTDOWN) ||
		(status == OBJECT_STATUS_SHUTTING_DOWN) ||
		(status == OBJECT_STATUS_DISABLED) ||
		(status == OBJECT_STATUS_DESTROYED))
	{
		fireGateTrace(getWatchID(false), weaponIndex, "status");	// ISSUE-17
		return(1);
	}

	if ((getBodyState() == MECH_STATUSCHUNK_BODYSTATE_PARKED) ||
		(getBodyState() == MECH_STATUSCHUNK_BODYSTATE_FALLEN_FORWARD) ||
		(getBodyState() == MECH_STATUSCHUNK_BODYSTATE_FALLEN_BACKWARD))
	{
		fireGateTrace(getWatchID(false), weaponIndex, "body");	// ISSUE-17
		return(1);
	}

	if (!isWeaponReady(weaponIndex))
	{
		fireGateTrace(getWatchID(false), weaponIndex, "ready");	// ISSUE-17
		return(3);
	}

	if (!Terrain::IsGameSelectTerrainPosition(getPosition()))
	{
		fireGateTrace(getWatchID(false), weaponIndex, "terrain");	// ISSUE-17
		return (1);
	}

	float distanceToTarget = 0.0;
	if (target)
	{
		if (target->isDestroyed())
		{
			fireGateTrace(getWatchID(false), weaponIndex, "dead_target");	// ISSUE-17
			return(4);
		}
			
		//We did this in calcWeaponsStatus.  Do we really need to do it again??!
		// I have decided NO as of now!
		/*
		if (getWeaponIndirectFire(weaponIndex))
		{
			if (!getTeam()->teamLineOfSight(target->getPosition()))
				return(4);
		}
		else
		{
 			if (!lineOfSight(target))
				return(4);
		}
		*/
		
		distanceToTarget = distanceFrom(target->getPosition());
	}
	else if (targetPoint) 
	{
		//We did this in calcWeaponsStatus.  Do we really need to do it again??!
		// I have decided NO as of now!
		/*
		if (getWeaponIndirectFire(weaponIndex))
		{
			if (!getTeam()->teamLineOfSight(*targetPoint))
				return(4);
		}
		else
		{
			if (!lineOfSight(*targetPoint))
				return(4);
		}
		*/

 		distanceToTarget = distanceFrom(*targetPoint);
	}
	else
	{
		fireGateTrace(getWatchID(false), weaponIndex, "no_target");	// ISSUE-17
		return(4);
	}

	bool inRange = weaponInRange(weaponIndex, distanceToTarget, MapCellDiagonal);

	// Let's make sure the target's in range...
	if (MPlayer)
	{
		if (MPlayer->isServer() && !inRange)
		{
			fireGateTrace(getWatchID(false), weaponIndex, "range_mp");	// ISSUE-17 / META-FIX F: was the 10th, untraced silent gate
			return(4);
		}
	}
	else if (!inRange)
	{
		fireGateTrace(getWatchID(false), weaponIndex, "range");	// ISSUE-17
		return(4);
	}

	long numShots = getWeaponShots(weaponIndex);
	if (numShots == 0)
	{
		fireGateTrace(getWatchID(false), weaponIndex, "shots");	// ISSUE-17
		return(4);
	}

	// As of 4/9/97, missiles cannot be aimed...
	// As of 4/30/98, they can be again... -fs
	//if ((aimLocation != -1) && (MasterComponent::masterList[inventory[weaponIndex].masterID].getForm() == COMPONENT_FORM_WEAPON_MISSILE))
	//	return(4);

	// Ripple Fire
	// Figure out what weapon Node on the Mech we are going to use
	// And see if its recycle time is Green.  If not, just return, we'll shoot again!
	long sourceHotSpot;
	if (MasterComponent::masterList[inventory[weaponIndex].masterID].getForm() == COMPONENT_FORM_WEAPON_MISSILE)
		sourceHotSpot = MECH3D_WEAPONTYPE_MISSILE;
	else if (MasterComponent::masterList[inventory[weaponIndex].masterID].getForm() == COMPONENT_FORM_WEAPON_BALLISTIC)
		sourceHotSpot = MECH3D_WEAPONTYPE_BALLISTIC;
	else
		sourceHotSpot = MECH3D_WEAPONTYPE_ENERGY;

	sourceHotSpot = inventory[weaponIndex].facing;
	if (appearance->getRightArmOff() && (sourceHotSpot == 1))
		sourceHotSpot = 3;	//Force into right torso.  Arm is gone.

	if (appearance->getLeftArmOff() && (sourceHotSpot == 2))
		sourceHotSpot = 5;	//Force into left torso.  Arm is gone.

	float recycleTime = appearance->getWeaponNodeRecycle(sourceHotSpot);
	if (recycleTime > 0.0f)
	{
		fireGateTrace(getWatchID(false), weaponIndex, "node_recycle");	// ISSUE-17
		return (3);
	}

	// FIREGATE-REGION-END (checked by check-order-gate-coverage.py) — every
	// silent early-return ABOVE this sentinel must have a fireGateTrace()
	// within the 4 preceding lines OR a KNOWN_UNCOVERED entry in the checker.
	// Shot delivery begins below; do not move this line without updating the
	// checker (META-FIX design F static chokepoint).

	//Set the hotspot for the target
	long targetHotSpot = 0;
	if (target && target->getAppearance())
	{
		targetHotSpot = target->getAppearance()->getWeaponNode(MECH3D_WEAPONTYPE_ANY);
	}
	
	float entryAngle = 0.0;
	if (target)
		entryAngle = target->relFacingTo(position);

	//long weaponType = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponType();
	bool isStreakMissile = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponStreak(); 

	long range;
	long attackChance = (long)calcAttackChance(target, aimLocation, targetTime, weaponIndex, 0.0, &range, targetPoint);
	long hitRoll = RandomNumber(100);
	if (target)
	{
		if (target->getTeamId() == TEAM2) {
			pilot->incNumSkillUses(COMBAT_STAT_MISSION, MWS_GUNNERY);
			pilot->skillPoints[MWS_GUNNERY] += SkillTry[MWS_GUNNERY];
		}
		else
			pilot->skillPoints[MWS_GUNNERY] += SkillTry[MWS_GUNNERY] / 10;
	}

	// HACK: If aiming a shot and moving, make chance to hit equal to
	// zero, yet still take shot (denny request: 4/22/98)
	//if ((aimLocation != -1) && (getSpeed() > 0.0))
	//	attackChance = 0;

	long hitLocation = -2;
	if (target && (hitRoll < attackChance)) {
		if (target->getTeamId() == TEAM2)
		{
			pilot->incNumSkillSuccesses(COMBAT_STAT_MISSION, MWS_GUNNERY);
			pilot->skillPoints[MWS_GUNNERY] += SkillSuccess[MWS_GUNNERY];
		}
		else
			pilot->skillPoints[MWS_GUNNERY] += SkillSuccess[MWS_GUNNERY] / 10;
		// If it's an aimed shot, we need to calc whether we hit the desired
		// location on the target...
		if (aimLocation != -1)
			hitLocation = aimLocation;
	}

	MechWarriorPtr targetPilot = NULL;
	if (target && target->isMover()) {
		targetPilot = ((MoverPtr)target)->getPilot();
		targetPilot->updateAttackerStatus(getWatchID(), scenarioTime);
	}

	// Weapon must recycle...
	startWeaponRecycle(weaponIndex);
	
	WeaponBoltPtr weaponFX = NULL;
	if (hitRoll < attackChance) {

		if (numShots != UNLIMITED_SHOTS) {
			// We're taking the shot, so reduce our ammo inventory...
			deductWeaponShot(weaponIndex);
		}

		// Attack hit.
		if (MasterComponent::masterList[inventory[weaponIndex].masterID].getForm() == COMPONENT_FORM_WEAPON_MISSILE) {
			// It's a missile weapon. We need to determine how many hit
			// (and missed) the target, and in how many clusters...
			long missileAmount = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponAmmoAmount();
			long numMissiles = 0;
			if (isStreakMissile)
				numMissiles = missileAmount;
			else {
				numMissiles = (long)(missileAmount / 2.0 + 0.5);
				if (numMissiles < 1)
					numMissiles = 1;
				if (numMissiles > missileAmount)
					numMissiles = missileAmount;
			}

			// a MissileGen Object is ALL missiles--clusters no more:)
			unsigned char effectType = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponSpecialEffect();

			// Need to know which hotspot this comes from.
			// Also need to know which hotspot this is going to.
			if (target) 
			{
				if (aimLocation == -1)
					hitLocation = target->calcHitLocation(this, weaponIndex, ATTACKSOURCE_WEAPONFIRE, attackType);
			}
			else
				hitLocation = -1;

			Assert(hitLocation != -2, 0, " Mech.FireWeapon: Bad Hit Location ");
	
			// For now, always use a bullet effect...
			WeaponShotInfo curShotInfo;
			curShotInfo.init(getWatchID(),
							 inventory[weaponIndex].masterID,
							 numMissiles * MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponDamage(),
							 hitLocation,
							 entryAngle);
			Assert(((float)((long)(curShotInfo.damage / 0.25)) * 0.25) == curShotInfo.damage, 0, " WeaponHitChunk.build: damage round error ");
			dmgDone = curShotInfo.damage;

			// If I'm in a multiplayer game and I'm the server, record this weapon fire
			// so it may be broadcast to all clients...
			if (MPlayer && MPlayer->isServer()) {
				WeaponFireChunk chunk;
				chunk.init();
				if (target) {
					if (target->isMover())
						chunk.buildMoverTarget(target,
											   weaponIndex - numOther,
											   true,
											   entryAngle,
											   numMissiles,
											   hitLocation);
					else if (target->getObjectClass() == CAMERADRONE)
						chunk.buildCameraDroneTarget(target,
													 weaponIndex - numOther,
													 true,
													 entryAngle,
													 numMissiles);
					else
						chunk.buildTerrainTarget(target,
												 weaponIndex - numOther,
												 true,
												 numMissiles);
					}
				else
					chunk.buildLocationTarget(*targetPoint, weaponIndex - numOther, true, numMissiles);
				chunk.pack(this);
				WeaponFireChunk chunk2;
				chunk2.init();
				chunk2.data = chunk.data;
				chunk2.unpack(this);
				if (!chunk.equalTo(&chunk2))
					Fatal(0, " Mech.fireWeapon: Bad WeaponFireChunk (save wfchunk.dbg file now) ");
				addWeaponFireChunk(CHUNK_SEND, &chunk);
				LogWeaponFireChunk(&chunk, this, target);
			}

			// This code will mess up if the object is not a BULLET!!!!!!!!!!!
			weaponFX = ObjectManager->createWeaponBolt(effectType);
			if (!weaponFX) {
				if (targetPoint) 
				{
					// Check for Mine hit and MOVE ON!!!
					int cellRow, cellCol;
					land->worldToCell(*targetPoint, cellRow, cellCol);
					if (GameMap->getMine(cellRow, cellCol) == 1) 
					{
						ObjectManager->createExplosion(MINE_EXPLOSION_ID, NULL, *targetPoint, MineSplashDamage, MineSplashRange * worldUnitsPerMeter);
						GameMap->setMine(cellRow, cellCol, 2);
					}
				}
				}
			else {
				if (target)
				{
					weaponFX->connect(this, target, &curShotInfo, sourceHotSpot, targetHotSpot);
					// ISSUE-9 S2 (MC2_CALLED_SHOT_FIX, default OFF): successful AIMED
					// missile shot — flag the bolt so an area-effect (LRM) arrival
					// honors the called location (curShotInfo.hitLocation ==
					// aimLocation here) instead of a randomly-located explosion.
					static const bool s_calledShotFix =
						(getenv("MC2_CALLED_SHOT_FIX") != NULL && getenv("MC2_CALLED_SHOT_FIX")[0] != '0');
					if (s_calledShotFix && (aimLocation != -1))
						weaponFX->setAimedTarget(target->getWatchID());
				}
				else {
					weaponFX->connect(this, *targetPoint, &curShotInfo, sourceHotSpot);
					//pilot->clearCurTacOrder();
				}
				printFireWeaponDebugInfo(target, targetPoint, attackChance, aimLocation, hitRoll, &curShotInfo);
			}
			}
		else {
			// Non-missile weapon, so just one weapon hit spawn...
			// For now, always use a laser effect...
			if (target) {
				if (aimLocation == -1)
					hitLocation = target->calcHitLocation(this, weaponIndex, ATTACKSOURCE_WEAPONFIRE, attackType);
				}
			else
				hitLocation = -1;
			Assert(hitLocation != -2, 1, " Mech.FireWeapon: Bad Hit Location ");

			WeaponShotInfo shotInfo;
			shotInfo.init(getWatchID(),
				inventory[weaponIndex].masterID,
				MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponDamage(),
				hitLocation,
				entryAngle);

			dmgDone = shotInfo.damage;

			// If I'm in a multiplayer game and I'm the server, record this weapon fire
			// so it may be broadcast to all clients...
			if (MPlayer && MPlayer->isServer()) {
				WeaponFireChunk chunk;
				chunk.init();
				if (target) {
					if (target->isMover())
						chunk.buildMoverTarget(target,
											   weaponIndex - numOther,
											   true,
											   entryAngle,
											   0,
											   hitLocation);
					else
						chunk.buildTerrainTarget(target,
												 weaponIndex - numOther,
												 true,
												 0);
					}
				else
				{
					chunk.buildLocationTarget(*targetPoint, weaponIndex - numOther, true, 0);
				}
				
				chunk.pack(this);
				WeaponFireChunk chunk2;
				chunk2.init();
				chunk2.data = chunk.data;
				chunk2.unpack(this);
				if (!chunk.equalTo(&chunk2))
					Fatal(0, " Mech.fireWeapon: Bad WeaponFireChunk (save wfchunk.dbg file now) ");
				addWeaponFireChunk(CHUNK_SEND, &chunk);
				LogWeaponFireChunk(&chunk, this, target);
			}

			unsigned char effectType = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponSpecialEffect();
			weaponFX = ObjectManager->createWeaponBolt(effectType);
			if (!weaponFX)
			{
				if (targetPoint)
				{
					// Check for Mine hit and MOVE ON!!!
					int cellRow, cellCol;

					land->worldToCell(*targetPoint, cellRow, cellCol);
					if (GameMap->getMine(cellRow, cellCol) == 1)
					{
						ObjectManager->createExplosion(MINE_EXPLOSION_ID, NULL, *targetPoint, MineSplashDamage, MineSplashRange * worldUnitsPerMeter);
						GameMap->setMine(cellRow, cellCol, 2);
					}
				}
			}
			else
			{
				if (target) 
				{
					weaponFX->connect(this, target, &shotInfo, sourceHotSpot, targetHotSpot);
				}
				else 
				{
					// Hit the target point/terrain...
					weaponFX->connect(this, *targetPoint, &shotInfo, sourceHotSpot);
				}
				printFireWeaponDebugInfo(target, targetPoint, attackChance, aimLocation, hitRoll, &shotInfo);
			}

			if (!target) {
				// Now, cancel the tac order...
				//pilot->clearCurTacOrder();
			}
		}
	}
	else
	{
		//if ((aimLocation != -1) && (RandomNumber(100) < AimedFireAbort))
		//	return(NO_ERR);
		if (!isStreakMissile) { 

			if (numShots != UNLIMITED_SHOTS) {
				// We're taking the shot, so reduce our ammo inventory...
				deductWeaponShot(weaponIndex);
			}

			// Miss, so check for possible miss resolution here...
			if (MasterComponent::masterList[inventory[weaponIndex].masterID].getForm() == COMPONENT_FORM_WEAPON_MISSILE)
			{
				// It's a missile weapon. We need to determine how many hit
				// (and missed) the target, and in how many clusters...
				long missileAmount = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponAmmoAmount();
				long numMissiles = ((float)missileAmount / 2.0) + 0.5;
				if (numMissiles < 1)
					numMissiles = 1;
				if (numMissiles > missileAmount)
					numMissiles = missileAmount;

				if (numMissiles > 0) {
					// No more clusters!
						WeaponShotInfo curShotInfo;
						curShotInfo.init(getWatchID(),
										 inventory[weaponIndex].masterID,
										 numMissiles * MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponDamage(),
										 -1,
										 entryAngle);
						Assert(((float)((long)(curShotInfo.damage / 0.25)) * 0.25) == curShotInfo.damage, 0, " WeaponHitChunk.build: damage round error ");

						// If we missed, pick sights away from the target and check LOS to each one.  If all are
						// invisible, just hit the target with zero points of damage.
						float missRadius = target ? 25.0 : 5.0;

						Stuff::Vector3D positionOffset;
						if (target)
							positionOffset = target->getPosition();
						else if (targetPoint)
							positionOffset = *targetPoint;
		
						positionOffset.x += missRadius;
						positionOffset.z = TerrainRuntime::groundElevation(positionOffset);

						bool canSeeHit = lineOfSight(positionOffset,true);

						if (!canSeeHit)
						{
							positionOffset.x -= (missRadius * 2.0f);
							positionOffset.z = TerrainRuntime::groundElevation(positionOffset);

							canSeeHit = lineOfSight(positionOffset,true); 

							if (!canSeeHit)
							{
								positionOffset.x += missRadius;
								positionOffset.y += missRadius;

								positionOffset.z = TerrainRuntime::groundElevation(positionOffset);

								canSeeHit = lineOfSight(positionOffset,true);

								if (!canSeeHit)
								{
									positionOffset.y -= (missRadius * 2.0f);
									positionOffset.z = TerrainRuntime::groundElevation(positionOffset);

									canSeeHit = lineOfSight(positionOffset,true);

									if (!canSeeHit)
									{
										//OK, no miss location is visible.  Hit the target with ZERO damage!!
										curShotInfo.init(getWatchID(),
														 inventory[weaponIndex].masterID,
														 0.0f,
														 -1,
														 entryAngle);
									}
								}
							}
						}

						// If I'm in a multiplayer game and I'm the server, record this weapon fire
						// so it may be broadcast to all clients...
						if (MPlayer && MPlayer->isServer()) {
							WeaponFireChunk chunk;
							chunk.init();
							chunk.buildLocationTarget(positionOffset, weaponIndex - numOther, false, numMissiles);
							chunk.pack(this);
							WeaponFireChunk chunk2;
							chunk2.init();
							chunk2.data = chunk.data;
							chunk2.unpack(this);
							if (!chunk.equalTo(&chunk2))
								Fatal(0, " Mech.fireWeapon: Bad WeaponFireChunk (save wfchunk.dbg file now) ");
							addWeaponFireChunk(CHUNK_SEND, &chunk);
							LogWeaponFireChunk(&chunk, this, target);
						}
	
					unsigned char effectType = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponSpecialEffect();
					weaponFX = ObjectManager->createWeaponBolt(effectType);
					if (!weaponFX)
					{
						// Check for Mine hit and MOVE ON!!!
						int  cellRow, cellCol;

						land->worldToCell(positionOffset, cellRow, cellCol);
						if (GameMap->getMine(cellRow, cellCol) == 1)
						{
							ObjectManager->createExplosion(MINE_EXPLOSION_ID, NULL, positionOffset, MineSplashDamage, MineSplashRange * worldUnitsPerMeter);
							GameMap->setMine(cellRow, cellCol, 2);
						}
					}
					else
					{
						if (canSeeHit)		//miss location is in LOS.  Hit the ground
							weaponFX->connect(this,positionOffset,&curShotInfo,sourceHotSpot);
						else if (target)	//Miss location is NOT in LOS.  Hit Target with ZERO damage!!!
							weaponFX->connect(this,target,&curShotInfo,sourceHotSpot);
						//OTHERWISE, we tried to hit the ground but we can't see the location we shot at.
						// DRAW NO WEAPON EFFECT!!!!

						printFireWeaponDebugInfo(target, &positionOffset, attackChance, aimLocation, hitRoll, &curShotInfo);
					}
				}
			}
			else
			{
				// Non-missile weapon, so just one weapon hit spawn...
				// For now, always use a laser effect...
				WeaponShotInfo shotInfo;
				shotInfo.init(getWatchID(),
					inventory[weaponIndex].masterID,
					MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponDamage(),
					-1,
					entryAngle);

				// If we missed, pick sights away from the target and check LOS to each one.  If all are
				// invisible, just hit the target with zero points of damage.
				float missRadius = target ? 25.0 : 5.0;

				Stuff::Vector3D positionOffset;
				if (target)
					positionOffset = target->getPosition();
				else if (targetPoint)
					positionOffset = *targetPoint;

				positionOffset.x += missRadius;
				positionOffset.z = TerrainRuntime::groundElevation(positionOffset);

				bool canSeeHit = lineOfSight(positionOffset,true);

				if (!canSeeHit)
				{
					positionOffset.x -= (missRadius * 2.0f);
					positionOffset.z = TerrainRuntime::groundElevation(positionOffset);

					canSeeHit = lineOfSight(positionOffset,true); 

					if (!canSeeHit)
					{
						positionOffset.x += missRadius;
						positionOffset.y += missRadius;

						positionOffset.z = TerrainRuntime::groundElevation(positionOffset);

						canSeeHit = lineOfSight(positionOffset,true);

						if (!canSeeHit)
						{
							positionOffset.y -= (missRadius * 2.0f);
							positionOffset.z = TerrainRuntime::groundElevation(positionOffset);

							canSeeHit = lineOfSight(positionOffset,true);

							if (!canSeeHit)
							{
								//OK, no miss location is visible.  Hit the target with ZERO damage!!
								shotInfo.init(getWatchID(),
											 inventory[weaponIndex].masterID,
											 0.0f,
											 -1,
											 entryAngle);
							}
						}
					}
				}

				// If I'm in a multiplayer game and I'm the server, record this weapon fire
				// so it may be broadcast to all clients...
				if (MPlayer && MPlayer->isServer()) {
					WeaponFireChunk chunk;
					chunk.init();
					chunk.buildLocationTarget(positionOffset, weaponIndex - numOther, false, 0);
					chunk.pack(this);
					WeaponFireChunk chunk2;
					chunk2.init();
					chunk2.data = chunk.data;
					chunk2.unpack(this);
					if (!chunk.equalTo(&chunk2))
						Fatal(0, " Mech.fireWeapon: Bad WeaponFireChunk (save wfchunk.dbg file now) ");
					addWeaponFireChunk(CHUNK_SEND, &chunk);
					LogWeaponFireChunk(&chunk, this, target);
				}
	
				unsigned char effectType = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponSpecialEffect();
				weaponFX = ObjectManager->createWeaponBolt(effectType);
				if (!weaponFX)
				{
					// Check for Mine hit and MOVE ON!!!
					int  cellRow, cellCol;

					land->worldToCell(positionOffset, cellRow, cellCol);
					if (GameMap->getMine(cellRow, cellCol) == 1)
					{
						ObjectManager->createExplosion(MINE_EXPLOSION_ID, NULL, positionOffset, MineSplashDamage, MineSplashRange * worldUnitsPerMeter);
						GameMap->setMine(cellRow, cellCol, 2);
					}
				}
				else
				{
					if (canSeeHit)		//miss location is in LOS.  Hit the ground
						weaponFX->connect(this,positionOffset,&shotInfo,sourceHotSpot);
					else if (target)	//Miss location is NOT in LOS.  Hit Target with ZERO damage!!!
						weaponFX->connect(this,target,&shotInfo,sourceHotSpot);

					printFireWeaponDebugInfo(target, &positionOffset, attackChance, aimLocation, hitRoll, &shotInfo);
				}
			}
		}
	}

	// Trigger the WEAPON TARGET event. For now, this assumes the target
	// KNOWS we were targeting him. Of course, the target wouldn't always
	// be aware of this, would they?
	pilot->triggerAlarm(PILOT_ALARM_FIRED_WEAPON, target ? target->getWatchID() : 0);
	if (targetPilot)
		targetPilot->triggerAlarm(PILOT_ALARM_TARGET_OF_WEAPONFIRE, getWatchID());

	timeSinceFiredLast = 0.0f;

	if (getGroup())
		getGroup()->handleMateFiredWeapon(getWatchID());

	return(NO_ERR);
}

long BattleMech::handleWeaponFire (long weaponIndex,
								   GameObjectPtr target,
								   Stuff::Vector3D* targetPoint,
								   bool hit,
								   float entryAngle,
								   long numMissiles,
								   long hitLocation) {

	// If it's already been fired, assume the ammo is there...
	long numShots = getWeaponShots(weaponIndex);
	//Assert(numShots > 0, numShots, " handleWeaponFire: numShots is negative! ");

	// Weapon must recycle...
	startWeaponRecycle(weaponIndex);

	bool isStreakMissile = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponStreak(); 

	WeaponBoltPtr weaponFX = NULL;
	
	// Need to know which hotspot this comes from.
	// Also need to know which hotspot this is going to.
	long sourceHotSpot;
	if (MasterComponent::masterList[inventory[weaponIndex].masterID].getForm() == COMPONENT_FORM_WEAPON_MISSILE)
		sourceHotSpot = MECH3D_WEAPONTYPE_MISSILE;
	else if (MasterComponent::masterList[inventory[weaponIndex].masterID].getForm() == COMPONENT_FORM_WEAPON_BALLISTIC)
		sourceHotSpot = MECH3D_WEAPONTYPE_BALLISTIC;
	else
		sourceHotSpot = MECH3D_WEAPONTYPE_ENERGY;
 
	sourceHotSpot = appearance->getWeaponNode(sourceHotSpot);
	
	//No Ripple fire in MPLayer.  Must just fire the weapon when we get the packet.
	
	appearance->setWeaponNodeUsed(sourceHotSpot);
	
 	//Set the hotspot for the target
	long targetHotSpot = 0;
	if (target && target->getAppearance())
	{
		targetHotSpot = target->getAppearance()->getWeaponNode(MECH3D_WEAPONTYPE_ANY);
	}
			
 	if (hit) {
		// Attack hit.
		if (numShots != UNLIMITED_SHOTS) {
			// We're taking the shot, so reduce our ammo inventory...
			deductWeaponShot(weaponIndex);
		}

		if (MasterComponent::masterList[inventory[weaponIndex].masterID].getForm() == COMPONENT_FORM_WEAPON_MISSILE) {
			// It's a missile weapon. We need to determine how many hit
			// (and missed) the target, and in how many clusters...

			// Clusters no more!
			unsigned char effectType = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponSpecialEffect();

			if (numMissiles > 0) {
				// This code will mess up if the object is not a BULLET!!!!!!!!!!!
				weaponFX = ObjectManager->createWeaponBolt(effectType);
				
				if (!weaponFX) 
				{
					if (targetPoint) 
					{
						// Check for Mine hit and MOVE ON!!!
						int cellRow, cellCol;
						land->worldToCell(*targetPoint, cellRow, cellCol);
						if (GameMap->getMine(cellRow, cellCol) == 1)
						{
							ObjectManager->createExplosion(MINE_EXPLOSION_ID, NULL, *targetPoint, MineSplashDamage, MineSplashRange * worldUnitsPerMeter);
							GameMap->setMine(cellRow, cellCol, 2);
						}
					}
				}
				else
				{
					Assert(hitLocation != -2, TargetRolo, " Mech.handleWeaponFire: Bad Hit Location ");
		
					// For now, always use a bullet effect...
					WeaponShotInfo curShotInfo;
					curShotInfo.init(getWatchID(),
									 inventory[weaponIndex].masterID,
									 numMissiles * MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponDamage(),
									 hitLocation,
									 entryAngle);
					Assert(((float)((long)(curShotInfo.damage / 0.25)) * 0.25) == curShotInfo.damage, 0, " WeaponHitChunk.build: damage round error ");
		
					if (target) 
						weaponFX->connect(this, target, &curShotInfo, sourceHotSpot, targetHotSpot);
					else  {
						weaponFX->connect(this, *targetPoint, &curShotInfo, sourceHotSpot);

						// Now, cancel the tac order...
						//pilot->clearCurTacOrder();
					}

				}
			}
			}
		else {
			// Non-missile weapon, so just one weapon hit spawn...
			// For now, always use a laser effect...
			
			WeaponShotInfo shotInfo;
			shotInfo.init(getWatchID(),
				inventory[weaponIndex].masterID,
				MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponDamage(),
				hitLocation,
				entryAngle);

			// ONE-WEAPON-PRESENTATION-PROOF-1 (E8): the ONE weapon-fire VFX emit
			// point intercepted for the equipment arc's final slice. For the ONE
			// target weapon (core:ppc) under MC2_UNIT_PROFILE_DATA, the muzzle/bolt
			// effect id is resolved from EquipmentDefinition presentation data;
			// gate-OFF / every other weapon / stock-no-data returns the legacy
			// getWeaponSpecialEffect() value UNCHANGED. Presentation ASSET CHOICE
			// only -- the fire decision, damage, heat, ammo, recycle, and the
			// createWeaponBolt/connect behavior below are the legacy path, untouched.
			long legacyEffectType = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponSpecialEffect();
			unsigned char effectType = (unsigned char)resolveTargetWeaponFireEffectType(
				inventory[weaponIndex].masterID, (int)legacyEffectType);
			weaponFX = ObjectManager->createWeaponBolt(effectType);

			if (!weaponFX)
			{
				if (targetPoint)
				{
					// Check for Mine hit and MOVE ON!!!
					int  cellRow, cellCol;
					land->worldToCell(*targetPoint, cellRow, cellCol);
					if (GameMap->getMine(cellRow, cellCol) == 1)
					{
						ObjectManager->createExplosion(MINE_EXPLOSION_ID, NULL, *targetPoint, MineSplashDamage, MineSplashRange * worldUnitsPerMeter);
						GameMap->setMine(cellRow, cellCol, 2);
					}
				}
			}
			else {
				if (target)
					weaponFX->connect(this, target, &shotInfo, sourceHotSpot, targetHotSpot);
				else  {
					weaponFX->connect(this, *targetPoint, &shotInfo, sourceHotSpot);

					// Now, cancel the tac order...
					//pilot->clearCurTacOrder();
				}
			}
		}
		}
	else {
		Assert(target == NULL, 0, " Mech.handleWeaponFire: target should be NULL with network miss! ");
		Assert(targetPoint != NULL, 0, " Mech.handleWeaponFire: MUST have targetpoint with network miss! ");
		if (isStreakMissile) {
			CurMoverWeaponFireChunk.unpack(this);
			DebugWeaponFireChunk(&CurMoverWeaponFireChunk, NULL, this);
			Assert(false, 0, " Mech.handleWeaponFire: streaks shouldn't miss! (save wfchunk.dbg file) ");
		}

		if (numShots != UNLIMITED_SHOTS) {
			// We're taking the shot, so reduce our ammo inventory...
			deductWeaponShot(weaponIndex);
		}

		// Miss, so check for possible miss resolution here...
		if (MasterComponent::masterList[inventory[weaponIndex].masterID].getForm() == COMPONENT_FORM_WEAPON_MISSILE) {

			if (numMissiles) {
				// Clusters no more!
				unsigned char effectType = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponSpecialEffect();
				weaponFX = ObjectManager->createWeaponBolt(effectType);
				
				if (!weaponFX) 
				{
					if (targetPoint) 
					{
						// Check for Mine hit and MOVE ON!!!
						int cellRow, cellCol;
						land->worldToCell(*targetPoint, cellRow, cellCol);
						if (GameMap->getMine(cellRow, cellCol) == 1) 
						{
							ObjectManager->createExplosion(MINE_EXPLOSION_ID, NULL, *targetPoint, MineSplashDamage, MineSplashRange * worldUnitsPerMeter);
							GameMap->setMine(cellRow, cellCol, 2);
						}
					}
				}
				else {
					WeaponShotInfo curShotInfo;
					curShotInfo.init(getWatchID(),
									 inventory[weaponIndex].masterID,
									 numMissiles * MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponDamage(),
									 -1,
									 entryAngle);
					Assert(((float)((long)(curShotInfo.damage / 0.25)) * 0.25) == curShotInfo.damage, 0, " WeaponHitChunk.build: damage round error ");
					weaponFX->connect(this, *targetPoint, &curShotInfo, sourceHotSpot);
				}
			}
			}
		else {
			// Non-missile weapon, so just one weapon hit spawn...
			// For now, always use a laser effect...
			WeaponShotInfo shotInfo;
			shotInfo.init(getWatchID(),
				inventory[weaponIndex].masterID,
				MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponDamage(),
				-1,
				entryAngle);

			unsigned char effectType = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponSpecialEffect();
			weaponFX = ObjectManager->createWeaponBolt(effectType);
			
			if (!weaponFX) 
			{
				if (targetPoint) 
				{
					// Check for Mine hit and MOVE ON!!!
					int cellRow, cellCol;
					land->worldToCell(*targetPoint, cellRow, cellCol);
					if (GameMap->getMine(cellRow, cellCol) == 1) 
					{
						ObjectManager->createExplosion(MINE_EXPLOSION_ID, NULL, *targetPoint, MineSplashDamage, MineSplashRange * worldUnitsPerMeter);
						GameMap->setMine(cellRow, cellCol, 2);
					}
				}
			}
			else {
				weaponFX->connect(this, *targetPoint, &shotInfo, sourceHotSpot);
			}
		}
	}

	// Trigger the WEAPON TARGET event. For now, this assumes the target
	// KNOWS we were targeting him. Of course, the target wouldn't always
	// be aware of this, would they?
	MechWarriorPtr targetPilot = NULL;
	if (target && target->isMover()) {
		targetPilot = ((MoverPtr)target)->getPilot();
		targetPilot->updateAttackerStatus(getWatchID(), scenarioTime);
		targetPilot->triggerAlarm(PILOT_ALARM_TARGET_OF_WEAPONFIRE, getWatchID());
	}

	if (getGroup())
		getGroup()->handleMateFiredWeapon(getWatchID());

	return(NO_ERR);
}
