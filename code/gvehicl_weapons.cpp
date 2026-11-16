// GroundVehicle: hit and attack helpers, status/move chunks, handleWeaponHit, fireWeapon and handleWeaponFire.
// Declared in the class headers; shared names in gvehicl_internal.h.
#include <algorithm>

#include "gvehicl_internal.h"
#include "Gates/gate.h"
#include "mc2fmt.h"
#include "net/runtime.h"



float GroundVehicle::relFacingTo (glm::vec3 goal, long bodyLocation) {

	float relFacing = Mover::relFacingTo(goal);

	switch (bodyLocation) {
		case GROUNDVEHICLE_LOCATION_TURRET:
			relFacing += turretRotation;
			break;
	}
	if (relFacing < -180.0)
		relFacing += 360.0;
	else if (relFacing > 180)
		relFacing -= 360.0;
	return(relFacing);
}


float GroundVehicle::calcAttackChance (GameObjectPtr target, long aimLocation, float targetTime, long weaponIndex, float modifiers, long* range, glm::vec3* targetPoint) {

	if ((weaponIndex < numOther) || (weaponIndex >= numOther + numWeapons))
		return(-1000.0);

	float attackChance = Mover::calcAttackChance(target, aimLocation, targetTime, weaponIndex, modifiers, range, targetPoint);

	return(attackChance);
}


long GroundVehicle::calcHitLocation (GameObjectPtr attacker, long weaponIndex, long attackSource, long attackType) {

	if (attackSource == MECH_HIT_SECTION_BOTTOM) // if this came from a mine...
		return GROUNDVEHICLE_LOCATION_FRONT;
	
	//---------------------------------------------------------------------
	// Need to use the Line-of-fire params to calc section and arc of hit.
	// For now, just use the arc of entry to the target, and select section
	// without using obstructions...

	long zAxisPos = (RandomNumber(100) + RandomNumber(100)) / 2;

	//-------------------------------------------------------------
	// For now, use the zAxisOffset to determine whether we hit the
	// top, middle or bottom. Ignore the xAxisPos...
	long hitLocation;
	if (zAxisPos >= 70)
		hitLocation = GROUNDVEHICLE_LOCATION_TURRET;
	else {
		float entryAngle = 0.0;
		if (attacker)
			entryAngle = relFacingTo(attacker->getPosition());
		if ((entryAngle >= -45.0) && (entryAngle <= 45.0))
			hitLocation = GROUNDVEHICLE_LOCATION_FRONT;
		else if ((entryAngle  > -135.0) && (entryAngle < -45.0))
			hitLocation = GROUNDVEHICLE_LOCATION_LEFT;
		else if ((entryAngle > 45.0) && (entryAngle < 135))
			hitLocation = GROUNDVEHICLE_LOCATION_RIGHT;
		else
			hitLocation = GROUNDVEHICLE_LOCATION_REAR;
	}

	return(hitLocation);
}


bool GroundVehicle::hitInventoryItem (long itemIndex, bool setupOnly) {

	//----------------------------------------------------------------------
	// This should never be called for a ground vehicle, as a CRITICAL SPACE
	// hit is handled differently from mechs...
	Fatal(0, " Vehicles should never suffer inventory item hit ");
	return(false);
}


void GroundVehicle::destroyBodyLocation (long location) {
	armor[location].curArmor = 0;
}


bool GroundVehicle::injureBodyLocation (long bodyLocation, float damage) {

	BodyLocationPtr location = &body[bodyLocation];
	if (damage >= location->curInternalStructure) {
		location->curInternalStructure = 0;
		location->damageState = IS_DAMAGE_DESTROYED;
		return(true);
	}

	location->curInternalStructure -= damage;
	float damageLevel = body[bodyLocation].curInternalStructure / body[bodyLocation].maxInternalStructure;
	if (damageLevel == 0.0)
		body[bodyLocation].damageState = IS_DAMAGE_DESTROYED;
	else if (damageLevel <= 0.5)
		body[bodyLocation].damageState = IS_DAMAGE_PARTIAL;
	else
		body[bodyLocation].damageState = IS_DAMAGE_NONE;
	return(false);
}


long GroundVehicle::buildStatusChunk (void) {

	//-----------------------------------------------------------------------
	// For now, we'll just track a failed piloting check for 3 status updates.
	// If all three updates are never received, then the clients won't know
	// to fall! This should be adequate. If it proves otherwise, we can
	// change this...
	statusChunk.init();

	statusChunk.bodyState = 0;	//BODY STATES NOT USED IN GV MLR ANYMORE!!!!!!
								//THESE BITS CAN BE RECOVERED!!!

	if (pilot) {
		GameObjectPtr curTarget = pilot->getLastTarget();
		if (curTarget) {
			if (curTarget->isMover()) {
				statusChunk.targetType = STATUSCHUNK_TARGET_MOVER;
				statusChunk.targetId = (static_cast<MoverPtr>(curTarget))->getNetRosterIndex();
				}
			else if (curTarget->isTerrainObject()) {
				statusChunk.targetType = STATUSCHUNK_TARGET_TERRAIN;
				statusChunk.targetId = curTarget->getPartId();
				}
			else
				Fatal(curTarget->getObjectClass(), " GroundVehicle.buildStatusChunk: bad target object class ");
		}
	}

	statusChunk.ejectOrderGiven = ejectOrderGiven;

	statusChunk.pack(this);


	return(NO_ERR);
}


long GroundVehicle::handleStatusChunk (long updateAge, unsigned long chunk) {

	statusChunk.init();
	statusChunk.data = chunk;
	statusChunk.unpack(this);
	//------------------------------------------------------------
	// HACK!!!!!!!!!!!!!! If bad packet, no statuschunk for you...
	if (StatusChunkUnpackErr != 0)
		return(NO_ERR); //Assert(0, " BAD PACKET! ");

	long lastTargetId = 0;
	if (!statusChunk.jumpOrder) {
		if (statusChunk.targetType > STATUSCHUNK_TARGET_NONE) {
			switch (statusChunk.targetType) {
				case STATUSCHUNK_TARGET_MOVER:
					if (MPlayer->moverRoster[statusChunk.targetId])
						lastTargetId = MPlayer->moverRoster[statusChunk.targetId]->getPartId();
					break;
				case STATUSCHUNK_TARGET_TERRAIN:
				case STATUSCHUNK_TARGET_SPECIAL:
					lastTargetId = statusChunk.targetId;
					break;
			}
		}
	}

	if (pilot) {
		if (lastTargetId == 0)
			pilot->setLastTarget(nullptr);
		else {
			GameObjectPtr curTarget = pilot->getLastTarget();
			if (!curTarget || (curTarget->getPartId() != lastTargetId)) {
				curTarget = ObjectManager->findByPartId(lastTargetId);
				pilot->setLastTarget(curTarget);
			}
		}

		if (!ejectOrderGiven && statusChunk.ejectOrderGiven) {
			ejectOrderGiven = true;
			handleEjection();
		}
	}

	return(NO_ERR);
}


long GroundVehicle::buildMoveChunk (void) {

	moveChunk.init();
	if (pilot) {
		pilot->getMovePath();
		moveChunk.build(this, pilot->getMovePath(0), pilot->getMovePath(1));
	}
	moveChunk.pack(this);


	return(NO_ERR);
}

//---------------------------------------------------------------------------
#pragma optimize("",off)

long GroundVehicle::handleMoveChunk (unsigned long chunk) {

	moveChunk.init();
	moveChunk.data = chunk;
	moveChunk.unpack(this);

	//----------------------------------------------------------
	// HACK!!!!!!!!!!!!!! If bad packet, no movechunk for you...
	if (MoveChunk::err != 0)
		return(NO_ERR); //Assert(0, " BAD PACKET! ");
	
	MovePathPtr path = getPilot()->getMovePath();
	setMoveChunk(path, &moveChunk);

	//-------------------------------------------------------------
	// Now, let's determine which opening step we're closest to so
	// we don't loop back to the first step everytime we get a new
	// chunk of the path...
	if (path->numStepsWhenNotPaused > 1) {
		long i = 0;
		for (i = path->numStepsWhenNotPaused - 1; i > 0; i--) {
			float distanceFromPt = distanceFrom(path->stepList[i].destination);
			if (distanceFromPt <= MapCellDiagonal)
				break;
		}
		path->setCurStep(i);
	}

	newMoveChunk = true;

	return(NO_ERR);
}

#pragma optimize("",on)


float GroundVehicle::weaponLocked (long weaponIndex, glm::vec3 targetPosition) {

	return(relFacingTo(targetPosition, GROUNDVEHICLE_LOCATION_TURRET));
}


long GroundVehicle::handleWeaponHit (WeaponShotInfoPtr shotInfo, bool addMultiplayChunk) {

	if (!shotInfo)
		return(NO_ERR);

	printHandleWeaponHitDebugInfo(shotInfo);

	// COMBAT-TRACE-1: see BattleMech::handleWeaponHit. Same universal hit chokepoint for vehicles.
	if (mc2_diag::tagEnabled("COMBAT")) {
		GameObjectPtr _cbAtk = ObjectManager->getByWatchID(shotInfo->attackerWID);
		char _cbf[192];
		// ISSUE-9 S3: "loc" = resolved hit location (vehicle location index) —
		// keeps the hit-event schema uniform with BattleMech::handleWeaponHit.
		mc2::fmt::to_buf(_cbf, "{{\"ev\":\"hit\",\"a_wid\":{:d},\"a_team\":{:d},\"t_wid\":{:d},\"t_team\":{:d},\"dmg\":{:.1f},\"loc\":{:d}}}", mc2::fmt::as_u32(static_cast<unsigned>(shotInfo->attackerWID)), mc2::fmt::as_i32(_cbAtk ? _cbAtk->getTeamId() : -1L), mc2::fmt::as_u32(getWatchID(false)), mc2::fmt::as_i32(getTeamId()), static_cast<double>(shotInfo->damage), mc2::fmt::as_i32(static_cast<int>(shotInfo->hitLocation)));
		mc2_diag::writeEvent("COMBAT", 1, 0, _cbf);
	}

	if (getTeam() && Team::noPain[getTeamId()])
		return(NO_ERR);

	if (!MPlayer && CantTouchThis && pilot->onHomeTeam())
		return(NO_ERR);

	if (addMultiplayChunk)
		MPlayer->addWeaponHitChunk(this, shotInfo);

	if (shotInfo->damage <= 0)
		return(NO_ERR);

	if (isDestroyed())
		return(NO_ERR);

	numWeaponHitsHandled++;

	//------------------------------------------------------------
	// Since Multiplayer still needs this, preserve it and restore
	// at the end of this function!
	WeaponShotInfo startShotInfo = *shotInfo;
	GameObjectPtr attacker = ObjectManager->getByWatchID(shotInfo->attackerWID);

	if ((shotInfo->hitLocation < 0) || (shotInfo->hitLocation >= NUM_GROUNDVEHICLE_LOCATIONS)) {
		char attackerStr[30];
		if (attacker) {
			if (attacker->isMover())
				strcpy(attackerStr, attacker->getName());
			else
				mc2::fmt::to_buf(attackerStr, "ID:{:d}", mc2::fmt::as_i32(attacker->getPartId()));
			}
		else
			strcpy(attackerStr, "attacker?");
		char err[128];
		mc2::fmt::to_buf(err, "GVehicle.handleWeaponHit: [{}]{:d} for {:.2f} at {:d}", mc2::fmt::safe(attackerStr), mc2::fmt::as_i32(shotInfo->masterId), shotInfo->damage, mc2::fmt::as_i32(shotInfo->hitLocation));
		Fatal(0, err);
	}

	if (armor[shotInfo->hitLocation].curArmor > 0) {
		if (shotInfo->damage > armor[shotInfo->hitLocation].curArmor) {
			shotInfo->setDamage(shotInfo->damage - armor[shotInfo->hitLocation].curArmor);
			armor[shotInfo->hitLocation].curArmor = 0;
			}
		else {
			armor[shotInfo->hitLocation].curArmor -= shotInfo->damage;
			shotInfo->setDamage(0);
		}
		//-----------------------------------------------------------
		// Make sure, in case it's a minesweeper, it ain't no more :)
		if (shotInfo->hitLocation == GROUNDVEHICLE_LOCATION_FRONT)
			mineSweeper = false;
	}

	bool vehicleAlreadyDisabled = isDisabled();
	bool vehicleDestroyed = false;
	if (shotInfo->damage > 0)
		vehicleDestroyed = injureBodyLocation(shotInfo->hitLocation, shotInfo->damage);

	if (vehicleDestroyed) 
	{
		pilot->handleOwnVehicleIncapacitation(0);
		getObjectType()->handleDestruction(this, nullptr);
		setFlag(OBJECT_FLAG_TANGIBLE,false);
	}

	curCV = calcCV();
	if (isDisabled())
		setThreatRating(1);
	else
		setThreatRating(-1);

	if (!vehicleAlreadyDisabled && isDisabled()) {
		if (attacker && attacker->isMover()) {
			attacker->getPilot()->triggerAlarm(PILOT_ALARM_KILLED_TARGET, getWatchID());
			if (!killed && MPlayer && MPlayer->isServer()) 
			{
				long attackerCID = attacker->getCommanderId();
				if (attackerCID == -1)
					attackerCID = (static_cast<MoverPtr>(attacker))->prevCommanderId;
				if ((attackerCID == -1) || (attackerCID == getCommanderId()))
					attackerCID = MAX_MC_PLAYERS;
				MPlayer->addKillLossChunk(attackerCID, !lost ? getCommanderId() : MAX_MC_PLAYERS);
				lost = true;
				killed = true;
			}
		}
		else if (attacker && (attacker->getObjectClass() == ARTILLERY))
		{
			if (!killed && MPlayer && MPlayer->isServer()) 
			{
				long attackerCID = attacker->getCommanderId();
				if (attackerCID == -1)
					attackerCID = (static_cast<MoverPtr>(attacker))->prevCommanderId;
				if ((attackerCID == -1) || (attackerCID == getCommanderId()))
					attackerCID = MAX_MC_PLAYERS;
				MPlayer->addKillLossChunk(attackerCID, !lost ? getCommanderId() : MAX_MC_PLAYERS);
				lost = true;
				killed = true;
			}
		}
		}
	else {
		if (attacker) {
			if (shotInfo->masterId > -1)
				pilot->triggerAlarm(PILOT_ALARM_HIT_BY_WEAPONFIRE, attacker->getWatchID());
			else if (shotInfo->masterId == -4)
				pilot->triggerAlarm(PILOT_ALARM_HIT_BY_WEAPONFIRE, attacker->getWatchID());
			else
				pilot->triggerAlarm(PILOT_ALARM_COLLISION, attacker->getWatchID());
			}
		else 
		{
			//If it was -5, it was a repairvehicle burning refit points.  Do not say you are under fire.
			if (shotInfo->masterId != -5)
			{
				if (shotInfo->masterId == -4)
					pilot->triggerAlarm(PILOT_ALARM_HIT_BY_WEAPONFIRE, 0);
				else if (shotInfo->masterId < 0) {
					//------------------------------------------------------------
					// If no attacker and a negative weapon masterId, then it must
					// be a mine or artillery...
					// EXCEPT if it was -5.  Then it was a repair vehicle chunk in multiplayer!!
					pilot->triggerAlarm(PILOT_ALARM_HIT_BY_WEAPONFIRE, shotInfo->masterId);
					}
				else
					pilot->triggerAlarm(PILOT_ALARM_HIT_BY_WEAPONFIRE, 0);
			}
		}
	}

	if ((shotInfo->masterId == -2) && (shotInfo->hitLocation == GROUNDVEHICLE_LOCATION_TURRET)) {
		mineLayer--;
		if ((mineLayer == 0) && (getCommanderId() == Commander::home->getId())) 
		{
			soundSystem->playBettySample( BETTY_MINES_GONE );
		}
	}

	if (shotInfo->masterId == -5) {
		if (!getRefitPoints())
			disable(POWER_USED_UP);
	}

	*shotInfo = startShotInfo;

	mc2::net::combatEvent("damage", shotInfo->attackerWID, getWatchID(false), shotInfo->damage);
	return(NO_ERR);
}


long GroundVehicle::fireWeapon (GameObjectPtr target, float targetTime, long weaponIndex, long attackType, long aimLocation, glm::vec3* targetPoint, float &dmgDone) {

	if (status != OBJECT_STATUS_NORMAL)
		return(1);

	if (!isWeaponIndex(weaponIndex))
		return(2);

	if (!isWeaponReady(weaponIndex))
		return(3);

	if (!Terrain::IsGameSelectTerrainPosition(getPosition()))
		return (1);
		
 	if (aerospaceSpotter)
		return 4;
		
	float distanceToTarget = 0.0;
	if (target) 
	{
		if (target->isDestroyed())
			return(4);
			
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
		return(4);
		
	bool inRange = weaponInRange(weaponIndex, distanceToTarget, MapCellDiagonal);

	if (MPlayer) 
	{
		if (MPlayer->isServer() && !inRange)
			return(4);
	}
	else if (!inRange)
		return(4);

	long numShots = getWeaponShots(weaponIndex);
	if (numShots == 0)
		return(4);


	MechWarriorPtr targetPilot = nullptr;
	if (target && target->isMover()) {
		targetPilot = (static_cast<MoverPtr>(target))->getPilot();
		targetPilot->updateAttackerStatus(getWatchID(), scenarioTime);
	}

	float entryAngle = 0.0;
	if (target)
		entryAngle = target->relFacingTo(position);

	bool isStreakMissile = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponStreak(); 

	long range;
	long attackChance = static_cast<long>(calcAttackChance(target, aimLocation, targetTime, weaponIndex, 0.0, &range, targetPoint));
	long hitRoll = RandomNumber(100);
	if (target && (target->getTeamId() == TEAM2))
		pilot->incNumSkillUses(COMBAT_STAT_MISSION, MWS_GUNNERY);

	//---------------------------------------------------------------
	// HACK: If aiming a shot and moving, make chance to hit equal to
	// zero, yet still take shot (denny request: 4/22/98)
	//if ((aimLocation != -1) && (getVelocity().GetLength() > 0.0))
	//	attackChance = 0;

	long hitLocation = -1;
	if (target && (hitRoll < attackChance)) {
		//---------------------------------------------------------------------
		// This really shouldn't be hardcoded to work for just IS players! --gd
		if (target->getTeamId() == TEAM2)
			pilot->incNumSkillSuccesses(COMBAT_STAT_MISSION, MWS_GUNNERY);
		if (aimLocation != -1) {
			hitLocation = aimLocation;
		}
	}

	long sourceHotSpot;
	if (MasterComponent::masterList[inventory[weaponIndex].masterID].getForm() == COMPONENT_FORM_WEAPON_MISSILE)
		sourceHotSpot = MECH3D_WEAPONTYPE_MISSILE;
	else if (MasterComponent::masterList[inventory[weaponIndex].masterID].getForm() == COMPONENT_FORM_WEAPON_BALLISTIC)
		sourceHotSpot = MECH3D_WEAPONTYPE_BALLISTIC;
	else
		sourceHotSpot = MECH3D_WEAPONTYPE_ENERGY;

	sourceHotSpot = appearance->getWeaponNode(sourceHotSpot);

	//Check if we even found a weapon node to use.
	// If so, it will be in the range from 0 to 3.
	// If not, return (3).  No weapon node available!!!
	// DO NOT ALLOW fire to come from unit base position.
	// UNLESS WE ARE IN MULTIPLAYER!!!  Vehicle may not be on OUR screen!!
	float recycleTime = appearance->getWeaponNodeRecycle(sourceHotSpot);
	if (!MPlayer)
	{
		if (sourceHotSpot < 0 || sourceHotSpot > 3)
			return (3);

		if (recycleTime > 0.0f)
			return (3);
	}
		
	appearance->setWeaponNodeUsed(sourceHotSpot);
	
	long targetHotSpot = 0;
	if (target && target->getAppearance())
	{
		targetHotSpot = target->getAppearance()->getWeaponNode(MECH3D_WEAPONTYPE_ANY);
	}
	
	//-----------------------
	// Weapon must recycle...
	startWeaponRecycle(weaponIndex);

 	WeaponBoltPtr weaponFX = nullptr;	
	if (hitRoll < attackChance) {

		if (numShots != UNLIMITED_SHOTS) {
			deductWeaponShot(weaponIndex);
		}
		
		if (MasterComponent::masterList[inventory[weaponIndex].masterID].getForm() == COMPONENT_FORM_WEAPON_MISSILE) {
			long missileAmount = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponAmmoAmount();
			long numMissiles;
			if (isStreakMissile)
				numMissiles = missileAmount;
			else {
				numMissiles = (static_cast<float>(missileAmount) / 2.0) + 0.5;
				numMissiles = std::max<long>(numMissiles, 1);
				numMissiles = std::min(numMissiles, missileAmount);
			}

			//-----------------------------------------------
			// a MissileGen Object is ALL of the clusters.
			// Don't make a thousand of them or the game implodes!
			//numClusters = 1;
	
			if (target) {
				if (aimLocation == -1)
					hitLocation = target->calcHitLocation(this, weaponIndex, ATTACKSOURCE_WEAPONFIRE, attackType);
				}
			else
				hitLocation = -1;
			Assert(hitLocation != -2, 0, " GroundVehicle.FireWeapon: Bad Hit Location ");
			WeaponShotInfo curShotInfo;
			curShotInfo.init(this->getWatchID(),
							 inventory[weaponIndex].masterID,
							 numMissiles * MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponDamage(),
							 hitLocation,
							 entryAngle);
	
			dmgDone = curShotInfo.damage;

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
					Fatal(0, " GVehicle.fireWeapon: Bad WeaponFireChunk (save wfchunk.dbg file now) ");
				addWeaponFireChunk(CHUNK_SEND, &chunk);
				LogWeaponFireChunk(&chunk, this, target);
			}
			unsigned char effectType = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponSpecialEffect();
			weaponFX = ObjectManager->createWeaponBolt(effectType);
			if (!weaponFX) 
			{
				if (target)
					target->handleWeaponHit(&curShotInfo, MPlayer != nullptr);
				else if (targetPoint) 
				{
					int cellRow, cellCol;
					land->worldToCell(*targetPoint, cellRow, cellCol);
					if (GameMap->getMine(cellRow, cellCol) == 1) 
					{
						ObjectManager->createExplosion(MINE_EXPLOSION_ID, nullptr, *targetPoint, MineSplashDamage, MineSplashRange * worldUnitsPerMeter);
						GameMap->setMine(cellRow, cellCol, 2);	//Mark Exploded
					}
				}
			}
			else {
				if (target)
				{
					weaponFX->connect(this, target, &curShotInfo, sourceHotSpot, targetHotSpot);
                    // ISSUE-9 S2 (MC2_CALLED_SHOT_FIX, default ON; =0 restores stock): successful AIMED
					// missile shot — flag the bolt so an area-effect (LRM) arrival
					// honors the called location instead of a randomly-located
					// explosion. Mirrors BattleMech::fireWeapon.
					static const bool s_calledShotFix =
						mc2::gate::get<"MC2_CALLED_SHOT_FIX">();
					if (s_calledShotFix && (aimLocation != -1))
						weaponFX->setAimedTarget(target->getWatchID());
				}
				else {
					weaponFX->connect(this, *targetPoint, &curShotInfo, sourceHotSpot);
					pilot->clearCurTacOrder();
				}
				printFireWeaponDebugInfo(target, targetPoint, attackChance, aimLocation, hitRoll, &curShotInfo);
			}
			}
		else {
			if (target) {
				if (aimLocation == -1)
					hitLocation = target->calcHitLocation(this, weaponIndex, ATTACKSOURCE_WEAPONFIRE, attackType);
				}
			else
				hitLocation = -1;
			Assert(hitLocation != -2, 0, " GroundVehicle.FireWeapon: Bad Hit Location ");

			WeaponShotInfo shotInfo;
			shotInfo.init(this->getWatchID(),
				inventory[weaponIndex].masterID,
				MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponDamage(),
				hitLocation,
				entryAngle);

			dmgDone = shotInfo.damage;

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
					else if (target->getObjectClass() == CAMERADRONE)
						chunk.buildCameraDroneTarget(target,
													 weaponIndex - numOther,
													 true,
													 entryAngle,
													 0);
					else
						chunk.buildTerrainTarget(target,
												 weaponIndex - numOther,
												 true,
												 0);
					}
				else
					chunk.buildLocationTarget(*targetPoint, weaponIndex - numOther, true, 0);
				chunk.pack(this);
				WeaponFireChunk chunk2;
				chunk2.init();
				chunk2.data = chunk.data;
				chunk2.unpack(this);
				if (!chunk.equalTo(&chunk2))
					Fatal(0, " GVehicle.fireWeapon: Bad WeaponFireChunk (save wfchunk.dbg file now) ");
				addWeaponFireChunk(CHUNK_SEND, &chunk);
				LogWeaponFireChunk(&chunk, this, target);
			}

			unsigned char effectType = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponSpecialEffect();
			weaponFX = ObjectManager->createWeaponBolt(effectType);
			if (!weaponFX) {
				if (target)
					target->handleWeaponHit(&shotInfo, MPlayer != nullptr);
				else if (targetPoint) 
				{
					int cellRow, cellCol;
					land->worldToCell(*targetPoint, cellRow, cellCol);
					if (GameMap->getMine(cellRow, cellCol) == 1)
					{
						ObjectManager->createExplosion(MINE_EXPLOSION_ID, nullptr, *targetPoint, MineSplashDamage, MineSplashRange * worldUnitsPerMeter);
						GameMap->setMine(cellRow, cellCol, 2);	//Mark Exploded
					}
				}
				}
			else {
				if (target) {
					weaponFX->connect(this, target, &shotInfo, sourceHotSpot, targetHotSpot);
					}
				else {
					weaponFX->connect(this, *targetPoint, &shotInfo, sourceHotSpot);
					if ( !suppressionFire )
						pilot->clearCurTacOrder();
				}
				printFireWeaponDebugInfo(target, targetPoint, attackChance, aimLocation, hitRoll, &shotInfo);
			}
		}
		}
	else {
		if (!isStreakMissile) { 

			if (numShots != UNLIMITED_SHOTS) {
				deductWeaponShot(weaponIndex);
			}

			if (MasterComponent::masterList[inventory[weaponIndex].masterID].getForm() == COMPONENT_FORM_WEAPON_MISSILE)
			{
				long missileAmount = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponAmmoAmount();
				long numMissiles = (static_cast<float>(missileAmount) / 2.0) + 0.5;
				numMissiles = std::max<long>(numMissiles, 1);
				numMissiles = std::min(numMissiles, missileAmount);

				if (numMissiles) {
					WeaponShotInfo curShotInfo;
					curShotInfo.init(this->getWatchID(),
									 inventory[weaponIndex].masterID,
									 numMissiles * MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponDamage(),
									 -1,
									 entryAngle);
	
					//-------------------------------------------------------------------------------------------
					// If we missed, pick sights away from the target and check LOS to each one.  If all are
					// invisible, just hit the target with zero points of damage.
					float missRadius = target ? 25.0 : 5.0;
	
					glm::vec3 positionOffset;
					if (target)
						positionOffset = target->getPosition();
					else if (targetPoint)
						positionOffset = *targetPoint;
	
					positionOffset.x += missRadius;
					positionOffset.z = TerrainRuntime::groundElevation(positionOffset);

					bool canSeeHit = lineOfSight(positionOffset);

					if (!canSeeHit)
					{
						positionOffset.x -= (missRadius * 2.0f);
						positionOffset.z = TerrainRuntime::groundElevation(positionOffset);

						canSeeHit = lineOfSight(positionOffset); 

						if (!canSeeHit)
						{
							positionOffset.x += missRadius;
							positionOffset.y += missRadius;

							positionOffset.z = TerrainRuntime::groundElevation(positionOffset);

							canSeeHit = lineOfSight(positionOffset);

							if (!canSeeHit)
							{
								positionOffset.y -= (missRadius * 2.0f);
								positionOffset.z = TerrainRuntime::groundElevation(positionOffset);

								canSeeHit = lineOfSight(positionOffset);

								if (!canSeeHit)
								{
									//OK, no miss location is visible.  Hit the target with ZERO damage!!
									curShotInfo.init(this->getWatchID(),
													 inventory[weaponIndex].masterID,
													 0.0f,
													 -1,
													 entryAngle);
								}
							}
						}
					}

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
							Fatal(0, " GVehicl.fireWeapon: Bad WeaponFireChunk (save wfchunk.dbg file now) ");
						addWeaponFireChunk(CHUNK_SEND, &chunk);
						LogWeaponFireChunk(&chunk, this, target);
					}
					unsigned char effectType = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponSpecialEffect();
					weaponFX = ObjectManager->createWeaponBolt(effectType);
					if (!weaponFX) {
						int cellRow, cellCol;
						land->worldToCell(positionOffset, cellRow, cellCol);
						if (GameMap->getMine(cellRow, cellCol) == 1) 
						{
							ObjectManager->createExplosion(MINE_EXPLOSION_ID, nullptr, positionOffset, MineSplashDamage, MineSplashRange * worldUnitsPerMeter);
							GameMap->setMine(cellRow, cellCol, 2); // Mark Exploded
						}
						}
					else
					{
						if (canSeeHit)		//miss location is in LOS.  Hit the ground
							weaponFX->connect(this,positionOffset,&curShotInfo,sourceHotSpot);
						else if (target)	//Miss location is NOT in LOS.  Hit Target with ZERO damage!!!
							weaponFX->connect(this,target,&curShotInfo,sourceHotSpot);
					}

					printFireWeaponDebugInfo(target, &positionOffset, attackChance, aimLocation, hitRoll, &curShotInfo);
				}
				}
			else {
				WeaponShotInfo shotInfo;
				shotInfo.init(this->getWatchID(),
					inventory[weaponIndex].masterID,
					MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponDamage(),
					-1,
					entryAngle);

				//-------------------------------------------------------------------------------------------
				// If we missed, pick sights away from the target and check LOS to each one.  If all are
				// invisible, just hit the target with zero points of damage.
				float missRadius = target ? 25.0 : 5.0;

				glm::vec3 positionOffset;
				if (target)
					positionOffset = target->getPosition();
				else if (targetPoint)
					positionOffset = *targetPoint;

				positionOffset.x += missRadius;
				positionOffset.z = TerrainRuntime::groundElevation(positionOffset);

				bool canSeeHit = lineOfSight(positionOffset);

				if (!canSeeHit)
				{
					positionOffset.x -= (missRadius * 2.0f);
					positionOffset.z = TerrainRuntime::groundElevation(positionOffset);

					canSeeHit = lineOfSight(positionOffset); 

					if (!canSeeHit)
					{
						positionOffset.x += missRadius;
						positionOffset.y += missRadius;

						positionOffset.z = TerrainRuntime::groundElevation(positionOffset);

						canSeeHit = lineOfSight(positionOffset);

						if (!canSeeHit)
						{
							positionOffset.y -= (missRadius * 2.0f);
							positionOffset.z = TerrainRuntime::groundElevation(positionOffset);

							canSeeHit = lineOfSight(positionOffset);

							if (!canSeeHit)
							{
								//OK, no miss location is visible.  Hit the target with ZERO damage!!
								shotInfo.init(this->getWatchID(),
											 inventory[weaponIndex].masterID,
											 0.0f,
											 -1,
											 entryAngle);
							}
						}
					}
				}

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
						Fatal(0, " GVehicl.fireWeapon: Bad WeaponFireChunk (save wfchunk.dbg file now) ");
					addWeaponFireChunk(CHUNK_SEND, &chunk);
					LogWeaponFireChunk(&chunk, this, target);
				}
				unsigned char effectType = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponSpecialEffect();
				weaponFX = ObjectManager->createWeaponBolt(effectType);
				if (!weaponFX) {
					int cellRow, cellCol;
					land->worldToCell(positionOffset, cellRow, cellCol);
					if (GameMap->getMine(cellRow, cellCol) == 1) 
					{
						ObjectManager->createExplosion(MINE_EXPLOSION_ID, nullptr, positionOffset, MineSplashDamage, MineSplashRange * worldUnitsPerMeter);
						GameMap->setMine(cellRow, cellCol, 2);	//Mark Exploded
					}
					}
				else
				{
					if (canSeeHit)		//miss location is in LOS.  Hit the ground
						weaponFX->connect(this,positionOffset,&shotInfo,sourceHotSpot);
					else if (target)	//Miss location is NOT in LOS.  Hit Target with ZERO damage!!!
						weaponFX->connect(this,target,&shotInfo,sourceHotSpot);
				}
				printFireWeaponDebugInfo(target, &positionOffset, attackChance, aimLocation, hitRoll, &shotInfo);
			}
		}
	}

	pilot->triggerAlarm(PILOT_ALARM_FIRED_WEAPON, target ? target->getWatchID() : 0);
	if (targetPilot)
		targetPilot->triggerAlarm(PILOT_ALARM_TARGET_OF_WEAPONFIRE, getWatchID());

	timeSinceFiredLast = 0.0f;

	if (getGroup())
		getGroup()->handleMateFiredWeapon(getWatchID());

	mc2::net::combatEvent("fire", getWatchID(false), target ? target->getWatchID(false) : 0, 0);
	return(NO_ERR);
}


long GroundVehicle::handleWeaponFire (long weaponIndex,
								   GameObjectPtr target,
								   glm::vec3* targetPoint,
								   bool hit,
								   float entryAngle,
								   long numMissiles,
								   long hitLocation) {

	long numShots = getWeaponShots(weaponIndex);

	//-----------------------
	// Weapon must recycle...
	startWeaponRecycle(weaponIndex);

	bool isStreakMissile = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponStreak(); 

	WeaponBoltPtr weaponFX = nullptr;
	
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
	
	long targetHotSpot = 0;
	if (target && target->getAppearance())
	{
		targetHotSpot = target->getAppearance()->getWeaponNode(MECH3D_WEAPONTYPE_ANY);
	}
	
 	if (hit) {
		if (numShots != UNLIMITED_SHOTS) {
			deductWeaponShot(weaponIndex);
		}

		if (MasterComponent::masterList[inventory[weaponIndex].masterID].getForm() == COMPONENT_FORM_WEAPON_MISSILE) {
			WeaponShotInfo curShotInfo;

			if (numMissiles) {
				//-------------------------------------------------------------------
				// This code will mess up if the object is not a BULLET!!!!!!!!!!!
				Assert(hitLocation != -2, TargetRolo, " GroundVehicle.handleWeaponFire: Bad Hit Location ");
	
				curShotInfo.init(this->getWatchID(),
								 inventory[weaponIndex].masterID,
								 numMissiles * MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponDamage(),
								 hitLocation,
								 entryAngle);
	
				unsigned char effectType = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponSpecialEffect();
				weaponFX = ObjectManager->createWeaponBolt(effectType);
				if (!weaponFX) {
					if (target) {
						if (MPlayer) {
							if (MPlayer->isServer())
								target->handleWeaponHit(&curShotInfo, true);
							}
						else
							target->handleWeaponHit(&curShotInfo);
					}
					}
				else {
					if (target) {
						weaponFX->connect(this, target, &curShotInfo, sourceHotSpot, targetHotSpot);
						}
					else {
						weaponFX->connect(this, *targetPoint, &curShotInfo, sourceHotSpot);
						if (!suppressionFire)
							pilot->clearCurTacOrder();
					}
				}
			}
		}
		else
		{
			
			WeaponShotInfo shotInfo;
			shotInfo.init(this->getWatchID(),
				inventory[weaponIndex].masterID,
				MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponDamage(),
				hitLocation,
				entryAngle);

			unsigned char effectType = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponSpecialEffect();
			weaponFX = ObjectManager->createWeaponBolt(effectType);
			if (!weaponFX) {
				if (target) {
					if (MPlayer) {
						if (MPlayer->isServer())
							target->handleWeaponHit(&shotInfo, true);
						}
					else
						target->handleWeaponHit(&shotInfo);
				}
				}
			else {
				if (target) {
					weaponFX->connect(this, target, &shotInfo, sourceHotSpot, targetHotSpot);
					}
				else {
					weaponFX->connect(this, *targetPoint, &shotInfo, sourceHotSpot);
					if (!suppressionFire)
						pilot->clearCurTacOrder();
				}
			}
		}
		}
	else {
		Assert(target == nullptr, 0, " GVehicl.handleWeaponFire: target should be NULL with network miss! ");
		Assert(targetPoint != nullptr, 0, " GVehicl.handleWeaponFire: MUST have targetpoint with network miss! ");
		if (isStreakMissile) {
			CurMoverWeaponFireChunk.unpack(this);
			DebugWeaponFireChunk(&CurMoverWeaponFireChunk, nullptr, this);
			Assert(false, 0, " GVehicl.handleWeaponFire: streaks shouldn't miss! ");
		}

		if (numShots != UNLIMITED_SHOTS) {
			deductWeaponShot(weaponIndex);
		}

		if (MasterComponent::masterList[inventory[weaponIndex].masterID].getForm() == COMPONENT_FORM_WEAPON_MISSILE) {

			if (numMissiles > 0) {
				//-----------------------------------------------
				// a MissileGen Object is ALL of the clusters.
				// Don't make a thousand of them or the game implodes!
				//numClusters = 1;
				unsigned char effectType = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponSpecialEffect();
				weaponFX = ObjectManager->createWeaponBolt(effectType);
				if (!weaponFX)
					Fatal(-1," couldnt create weapon FX ");
				else {
					WeaponShotInfo curShotInfo;
					curShotInfo.init(this->getWatchID(),
								 inventory[weaponIndex].masterID,
								 numMissiles * MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponDamage(),
								 -1,
								 entryAngle);
					weaponFX->connect(this, *targetPoint, &curShotInfo, sourceHotSpot);
				}
			}
			}
		else {
			WeaponShotInfo shotInfo;
			shotInfo.init(this->getWatchID(),
				inventory[weaponIndex].masterID,
				MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponDamage(),
				-1,
				entryAngle);
			unsigned char effectType = MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponSpecialEffect();
			weaponFX = ObjectManager->createWeaponBolt(effectType);
			if (!weaponFX)
				Fatal(-1," couldnt create weapon FX ");
			else
				weaponFX->connect(this, *targetPoint, &shotInfo, sourceHotSpot);
		}
	}

	MechWarriorPtr targetPilot = nullptr;
	if (target && target->isMover()) {
		targetPilot = (static_cast<MoverPtr>(target))->getPilot();
		targetPilot->updateAttackerStatus(getWatchID(), scenarioTime);
		targetPilot->triggerAlarm(PILOT_ALARM_TARGET_OF_WEAPONFIRE, getWatchID());
	}

	if (getGroup())
		getGroup()->handleMateFiredWeapon(getWatchID());

	return(NO_ERR);
}




float GroundVehicle::getTotalEffectiveness(void)
{
float weaponEffect, maxFront, curFront, armorFront, maxLeft, curLeft, armorLeft, maxRight, curRight, armorRight;
float maxRear, curRear, armorRear, maxTurret, curTurret, armorTurret, armorEffect, pilotEffect, result;

		if (isDestroyed() || isDisabled())
			return 0.0;

		if (maxWeaponEffectiveness)
			weaponEffect = weaponEffectiveness / maxWeaponEffectiveness;
		else
			weaponEffect = 1.0;

		float extraEffect = 1.0f;
		if (isRefit() || mineLayer )
		{
			//---------TURRET -- Which is Refit Points -- Factor in Please!
			curTurret = armor[GROUNDVEHICLE_LOCATION_TURRET].curArmor;
			maxTurret = armor[GROUNDVEHICLE_LOCATION_TURRET].maxArmor;
			armorTurret = 1.0;
			
			if (maxTurret != 0.0)
				armorTurret = curTurret / maxTurret;
				
			extraEffect = armorTurret;
 		}
		
		maxFront = armor[GROUNDVEHICLE_LOCATION_FRONT].maxArmor;
		curFront = armor[GROUNDVEHICLE_LOCATION_FRONT].curArmor;
		armorFront = curFront / maxFront * 0.6 + (0.4);
		
		curLeft = armor[GROUNDVEHICLE_LOCATION_LEFT].curArmor;
		maxLeft = armor[GROUNDVEHICLE_LOCATION_LEFT].maxArmor;
		armorLeft = curLeft / maxLeft * 0.6 + (0.4);
		
		curRight = armor[GROUNDVEHICLE_LOCATION_RIGHT].curArmor;
		maxRight = armor[GROUNDVEHICLE_LOCATION_RIGHT].maxArmor;
		armorRight = curRight / maxRight * 0.6 + (0.4);
		
		curRear = armor[GROUNDVEHICLE_LOCATION_REAR].curArmor;
		maxRear = armor[GROUNDVEHICLE_LOCATION_REAR].maxArmor;
		armorRear = curRear / maxRear * 0.6 + (0.4);
		
		curTurret = armor[GROUNDVEHICLE_LOCATION_TURRET].curArmor;
		maxTurret = armor[GROUNDVEHICLE_LOCATION_TURRET].maxArmor;
		armorTurret = 1.0;
		
		if (maxTurret != 0.0)
			armorTurret = curTurret / maxTurret * 0.6 + (0.4);
		
		armorEffect = armorFront * armorRear * armorLeft * armorRight * armorTurret;
		
		float pilotWoundTable[7] = 
		{
			1.0,0.95f,0.85f,0.75f,0.50f,0.30f,0.00
		};
		
		pilotEffect = pilotWoundTable[float2long(getPilot()->getWounds())];
		

		result = weaponEffect * armorEffect * pilotEffect * extraEffect;
		return result;
}
