// BattleMech: inventory hits, critical hits, status/move chunks and handleWeaponHit.
// Declared in the class headers; shared names in mech_internal.h.
#include "mech_internal.h"
#include "mc2fmt.h"
#include "sim_trace.h"


bool BattleMech::hitInventoryItem (long itemIndex, bool setupOnly) {

	if ((itemIndex < 0) || (itemIndex >= (numOther + numWeapons + numAmmos)))
		return false;

	(inventory[itemIndex].health)--;
	bool itemDisabled = false;
	long masterID = inventory[itemIndex].masterID;
	long bodyLocation = inventory[itemIndex].bodyLocation;

	switch (MasterComponent::masterList[masterID].getForm()) {
		case COMPONENT_FORM_SIMPLE:
			break;
		case COMPONENT_FORM_COCKPIT:
			break;
		case COMPONENT_FORM_SENSOR:
			break;
		case COMPONENT_FORM_ACTUATOR:
			if ((bodyLocation == MECH_BODY_LOCATION_LLEG) || (bodyLocation == MECH_BODY_LOCATION_RLEG))
				pilotingCheck(0); //pilotCheckModifier += PilotCheckModifierTable[0];
			break;
		case COMPONENT_FORM_ENGINE:
			break;
		case COMPONENT_FORM_HEATSINK:
		case COMPONENT_FORM_WEAPON:
		case COMPONENT_FORM_WEAPON_ENERGY:
		case COMPONENT_FORM_WEAPON_BALLISTIC:
		case COMPONENT_FORM_WEAPON_MISSILE:
			break;
		case COMPONENT_FORM_AMMO:
			break;
		case COMPONENT_FORM_JUMPJET:
			break;
		case COMPONENT_FORM_CASE:
			break;
		case COMPONENT_FORM_LIFESUPPORT:
			break;
		case COMPONENT_FORM_GYROSCOPE:
			pilotingCheck(0); //pilotCheckModifier += PilotCheckModifierTable[1];
			break;
		case COMPONENT_FORM_POWER_AMPLIFIER:
		case COMPONENT_FORM_BULK:
			break;
	}

	if (getInventoryDamage(itemIndex) >= MasterComponent::masterList[masterID].getDisableLevel()) {
		inventory[itemIndex].disabled = true;
		itemDisabled = true;
		long damageHotSpot = 1;

		switch (MasterComponent::masterList[masterID].getForm()) {
			case COMPONENT_FORM_SIMPLE:
			case COMPONENT_FORM_COCKPIT:
				damageHotSpot = 2;
				break;
			case COMPONENT_FORM_SENSOR:
			//Sensors CANNOT be destroyed anymore.  Non-Sensor mechs lose all line of sight then.
			// If you want to destroy the sensor mech, blow it up!
//				if (sensorSystem)
//					sensorSystem->broken = true;
				break;
			case COMPONENT_FORM_ECM:
				break;
			case COMPONENT_FORM_JAMMER:
				break;
			case COMPONENT_FORM_ACTUATOR:
				damageHotSpot = 0;
				break;
			case COMPONENT_FORM_ENGINE:
				engineBlowTime = scenarioTime + 5.0;
				damageHotSpot = 1;
				disable(ENGINE_DEATH);
				break;
			case COMPONENT_FORM_HEATSINK:
#ifdef USEHEAT
				if ((inventory[itemIndex].bodyLocation == MECH_BODY_LOCATION_LLEG) || (inventory[itemIndex].bodyLocation == MECH_BODY_LOCATION_RLEG))
					curLegHeatSinks--;
				disabledHeatSinks++;
				heatDissipation = calcHeatDissipation();
				damageHotSpot = 1;
#endif
				break;
			case COMPONENT_FORM_WEAPON:
			case COMPONENT_FORM_WEAPON_ENERGY:
			case COMPONENT_FORM_WEAPON_BALLISTIC:
			case COMPONENT_FORM_WEAPON_MISSILE:
				calcWeaponEffectiveness(false);
				calcFireRanges();
				damageHotSpot = 0;
				pilot->radioMessage(RADIO_WEAPON_DOWN);
				break;
			case COMPONENT_FORM_AMMO:
				break;
			case COMPONENT_FORM_JUMPJET:
				break;
			case COMPONENT_FORM_CASE:
				body[inventory[itemIndex].bodyLocation].CASE = false;
				break;
			case COMPONENT_FORM_LIFESUPPORT:
			case COMPONENT_FORM_GYROSCOPE:
				break;
			case COMPONENT_FORM_POWER_AMPLIFIER:
			case COMPONENT_FORM_BULK:
				damageHotSpot = 1;
				break;
		}
	}

	if (inventory[itemIndex].health == 0) 
	{
		switch (MasterComponent::masterList[masterID].getForm()) 
		{
			case COMPONENT_FORM_SIMPLE:
				break;
			case COMPONENT_FORM_COCKPIT:
				pilot->injure(6, false);
				break;
			case COMPONENT_FORM_SENSOR:
				break;
			case COMPONENT_FORM_ACTUATOR:
				if ((bodyLocation == MECH_BODY_LOCATION_LLEG) || (bodyLocation == MECH_BODY_LOCATION_RLEG))
					pilotingCheck(PILOTCHECK_SITUATION_NONE, 100.0); //pilotCheckModifier += 1000;
				break;
			case COMPONENT_FORM_ENGINE:
				break;
			case COMPONENT_FORM_HEATSINK:
			case COMPONENT_FORM_WEAPON:
			case COMPONENT_FORM_WEAPON_ENERGY:
			case COMPONENT_FORM_WEAPON_BALLISTIC:
			case COMPONENT_FORM_WEAPON_MISSILE:
				break;
			case COMPONENT_FORM_AMMO:
				ammoExplosion(itemIndex);
				break;
			case COMPONENT_FORM_JUMPJET:
			case COMPONENT_FORM_CASE:
			case COMPONENT_FORM_LIFESUPPORT:
				break;
			case COMPONENT_FORM_GYROSCOPE:
				break;
			case COMPONENT_FORM_POWER_AMPLIFIER:
			case COMPONENT_FORM_BULK:
				break;
		}

		if (!setupOnly && !playedCriticalHit)
		{
			Stuff::Vector3D smokePos = appearance->getNodePosition(0);
			ObjectManager->createExplosion(MECH_CRITICAL_HIT_ID,nullptr,smokePos);

			appearance->startSmoking(1);
			playedCriticalHit = true;
		}
	}

	return(false);
}

void BattleMech::destroyBodyLocation (long bodyLocation) {
	MC2_SIM_TRACE_LINE("DESTROYLOC mech=%ld loc=%ld", static_cast<long>(getPartId()), static_cast<long>(bodyLocation));

	if (body[bodyLocation].damageState == IS_DAMAGE_DESTROYED)
		return;


	body[bodyLocation].curInternalStructure = 0;
	body[bodyLocation].damageState = IS_DAMAGE_DESTROYED;
	armor[bodyLocation].curArmor = 0;
	if ((bodyLocation == MECH_BODY_LOCATION_LLEG) || (bodyLocation == MECH_BODY_LOCATION_RLEG)) {
		calcLegStatus();
		pilotingCheck(PILOTCHECK_SITUATION_NONE, 100.0); //pilotCheckModifier += 1000;
		}
	else if (bodyLocation == MECH_BODY_LOCATION_CTORSO)
		calcTorsoStatus();

	for (long curSpace = 0; curSpace < NumLocationCriticalSpaces[bodyLocation]; curSpace++) {
		if (!body[bodyLocation].criticalSpaces[curSpace].hit)
			if (body[bodyLocation].criticalSpaces[curSpace].inventoryID != 255) {
					body[bodyLocation].criticalSpaces[curSpace].hit = true;
					hitInventoryItem(body[bodyLocation].criticalSpaces[curSpace].inventoryID);
			}
	}

	if (bodyLocation == MECH_BODY_LOCATION_CTORSO) {
		// Mech essentially destroyed. Does it explode or simply shutdown?
		// For now, it's always exploding...
		MC2_SIM_TRACE_LINE("CTBRANCH mech=%ld ctbt=%.6f ebt=%.6f status=%d", static_cast<long>(getPartId()), static_cast<double>(centerTorsoBlowTime), static_cast<double>(engineBlowTime), static_cast<int>(getStatus()));
		if ((scenarioTime <= centerTorsoBlowTime) || ((engineBlowTime > -1.0) && (scenarioTime <= engineBlowTime))) {
			pilot->handleAlarm(PILOT_ALARM_VEHICLE_INCAPACITATED, 0);
			ObjectManager->getObjectType(typeHandle)->handleDestruction(this, nullptr);
			}
		else {
			disable(ENGINE_DEATH);
		}
		}
	else if (bodyLocation == MECH_BODY_LOCATION_RTORSO)
		destroyBodyLocation(MECH_BODY_LOCATION_RARM);
	else if (bodyLocation == MECH_BODY_LOCATION_LTORSO)
		destroyBodyLocation(MECH_BODY_LOCATION_LARM);
	else if (bodyLocation == MECH_BODY_LOCATION_LARM)
		leftArmBlownThisFrame = true;
	else if (bodyLocation == MECH_BODY_LOCATION_RARM)
		rightArmBlownThisFrame = true;
}

void BattleMech::calcCriticalHit (long hitLocation) {

	// If we're playing multiplayer and we're not the server,
	// exit--wait for the critical hit chunks from the server...
	if (MPlayer && !MPlayer->isServer())
		return;

	if (body[hitLocation].totalSpaces == 0)
		return;

	long numCriticalHits = 0;
	long bodyLocation = MechArmorToBodyLocation[hitLocation];
	long numBodySpaces = NumLocationCriticalSpaces[bodyLocation];

	long criticalRoll = RandomNumber(100);
	if (criticalRoll < CriticalHitTable[0])
		return;
	else if (criticalRoll < CriticalHitTable[1])
		numCriticalHits = 1;
	else if (criticalRoll < CriticalHitTable[2])
		numCriticalHits = 2;
	else if (isTorso(bodyLocation))
		numCriticalHits = 3;

	if (numCriticalHits > 0)
	{
		for (long curHit = 0; curHit < numCriticalHits; curHit++) 
		{
			long critHitRoll = RandomNumber(body[bodyLocation].totalSpaces);
			long hitSpace = 0;
			long numSpaces = 0;

			for (hitSpace = 0; hitSpace < numBodySpaces; hitSpace++) 
			{
				numSpaces = 0;
				unsigned char inv_id = body[bodyLocation].criticalSpaces[hitSpace].inventoryID;
				if (inv_id < 255)
				{
					gosASSERT(inv_id < MAX_MOVER_INVENTORY_ITEMS);
					numSpaces = MasterComponent::masterList[inventory[inv_id].masterID].getSize();
				}

				if (critHitRoll < numSpaces)
					break;
				else
					critHitRoll -= numSpaces;
			}

			if (bodyLocation < 0 || bodyLocation > 7)
				return;

			if (hitSpace < 0 || hitSpace >= NumLocationCriticalSpaces[bodyLocation])
				return;

			body[bodyLocation].criticalSpaces[hitSpace].hit = 1;
			hitInventoryItem(body[bodyLocation].criticalSpaces[hitSpace].inventoryID);

			if (MPlayer)
				addCriticalHitChunk(CHUNK_SEND, bodyLocation, hitSpace);
		}
	}
	else
	{
		destroyBodyLocation(bodyLocation);
		if (MPlayer)
			addCriticalHitChunk(CHUNK_SEND, bodyLocation, 15);
	}
}

void BattleMech::handleCriticalHit (long bodyLocation, long criticalSpace) {

	if (criticalSpace == 15)
		destroyBodyLocation(bodyLocation);
	else
		hitInventoryItem(body[bodyLocation].criticalSpaces[criticalSpace].inventoryID);
}

long BattleMech::updateCriticalHitChunks (long which) {

	for (long i = 0; i < numCriticalHitChunks[which]; i++) {
		unsigned char chunkData = criticalHitChunks[which][i];
		long criticalSpace = chunkData & 0x0F;
		long bodyLocation = (chunkData >> 4);
		handleCriticalHit(bodyLocation, criticalSpace);
	}

	numCriticalHitChunks[which] = 0;

	return(NO_ERR);
}

long BattleMech::buildStatusChunk (void) {

	// For now, we'll just track a failed piloting check for 3 status updates.
	// If all three updates are never received, then the clients won't know
	// to fall! This should be adequate. If it proves otherwise, we can
	// change this...
	statusChunk.init();

	if (isDestroyed())
		statusChunk.bodyState = MECH_STATUSCHUNK_BODYSTATE_DESTROYED;
	else
		statusChunk.bodyState = getBodyState();

	if (pilot) {
		if (inJump) {
			statusChunk.jumpOrder = true;
			int cellRC[2];
			land->worldToCell(jumpGoal, cellRC[0], cellRC[1]);
			statusChunk.targetCellRC[0] = cellRC[0];
			statusChunk.targetCellRC[1] = cellRC[1];
			}
		else {
			GameObjectPtr curTarget = pilot->getLastTarget();
			if (curTarget) {
				if (curTarget->isMover()) {
					statusChunk.targetType = STATUSCHUNK_TARGET_MOVER;
					statusChunk.targetId = ((MoverPtr)curTarget)->getNetRosterIndex();
					}
				else if (curTarget->isTerrainObject()) {
					statusChunk.targetType = STATUSCHUNK_TARGET_TERRAIN;
					statusChunk.targetId = curTarget->getPartId();
					}
				else
					Fatal(curTarget->getObjectClass(), " BattleMech.buildStatusChunk: bad target type ");
			}
		}
	}

	statusChunk.ejectOrderGiven = ejectOrderGiven;

	statusChunk.pack(this);

#ifdef DEBUG_CHUNKS
	StatusChunk chunk2;
	chunk2.data = statusChunk.data;
	chunk2.unpack(this);
	if (!statusChunk.equalTo(&chunk2))
		Fatal(0, " BAD status chunk in mech: save stchunk.dbg file! ");
#endif

	return(NO_ERR);
}

long BattleMech::handleStatusChunk (long updateAge, unsigned long chunk) {

	statusChunk.init();
	statusChunk.data = chunk;
	statusChunk.unpack(this);
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

	if (statusChunk.bodyState == MECH_STATUSCHUNK_BODYSTATE_DESTROYED)
		setStatus(OBJECT_STATUS_DESTROYED);

	return(NO_ERR);
}

#pragma optimize("",off)

long BattleMech::buildMoveChunk (void) {

	moveChunk.init();
	if (pilot) {
		// First, make sure we have the latest path...
		pilot->getMovePath();
		moveChunk.build(this, pilot->getMovePath(0), pilot->getMovePath(1));
	}
	moveChunk.pack(this);

#ifdef DEBUG_CHUNKS
	MoveChunk chunk2;
	chunk2.data = moveChunk.data;
	chunk2.unpack(this);
	// HACK!!!!!!!!!!!!!! If bad packet, no movechunk for you...
	if (MoveChunk::err != 0) {
		moveChunk.init();
		moveChunk.build(this, nullptr, nullptr);
		moveChunk.pack(this);
		return(NO_ERR); //Assert(0, " BAD PACKET! ");
	}
	if (!moveChunk.equalTo(this, &chunk2))
		Fatal(0, " Bad mech movechunk: save mvchunk.dbg file! ");
#endif

	return(NO_ERR);
}

long BattleMech::handleMoveChunk (unsigned long chunk) {

	moveChunk.init();
	moveChunk.data = chunk;
	moveChunk.unpack(this);

	// HACK!!!!!!!!!!!!!! If bad packet, no movechunk for you...
	if (MoveChunk::err != 0)
		return(NO_ERR); //Assert(0, " BAD PACKET! ");

	MovePathPtr path = getPilot()->getMovePath();
	setMoveChunk(path, &moveChunk);

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

bool BattleMech::injureBodyLocation (long bodyLocation, float damage) {
	MC2_SIM_TRACE_LINE("INJURE mech=%ld loc=%ld dmg=%.3f cur=%.3f", static_cast<long>(getPartId()), static_cast<long>(bodyLocation), static_cast<double>(damage), static_cast<double>(body[bodyLocation].curInternalStructure));

	BodyLocationPtr location = &body[bodyLocation];

	if ((bodyLocation == MECH_BODY_LOCATION_CTORSO) && (centerTorsoBlowTime < 0.0))
		centerTorsoBlowTime = scenarioTime;

	if (damage > location->curInternalStructure)
		location->curInternalStructure = 0;
	else
		location->curInternalStructure -= damage;
	if (location->curInternalStructure > 0) {
		float damageLevel = body[bodyLocation].curInternalStructure / body[bodyLocation].maxInternalStructure;
		if (damageLevel <= 0.5)
			body[bodyLocation].damageState = IS_DAMAGE_PARTIAL;
		else
			body[bodyLocation].damageState = IS_DAMAGE_NONE;
		if ((bodyLocation == MECH_BODY_LOCATION_LLEG) || (bodyLocation == MECH_BODY_LOCATION_RLEG))
			calcLegStatus();
		else if (bodyLocation == MECH_BODY_LOCATION_CTORSO)
			calcTorsoStatus();
		calcCriticalHit(bodyLocation);
		}
	else {
		destroyBodyLocation(bodyLocation);
		return(true);
	}
	return(false);
}

float BattleMech::weaponLocked (long weaponIndex, Stuff::Vector3D targetPosition) {

	return(relFacingTo(targetPosition, inventory[weaponIndex].bodyLocation));
}

long BattleMech::handleWeaponHit (WeaponShotInfoPtr shotInfo, bool addMultiplayChunk) 
{
	bool localInvulnerable = false;
	if (invulnerableON && Team::home == getTeam())
		localInvulnerable = true;

	if (!shotInfo)
		return(NO_ERR);

	MC2_SIM_TRACE_LINE("HIT mech=%ld dmg=%.3f loc=%ld master=%ld atk=%ld", static_cast<long>(getPartId()), static_cast<double>(shotInfo->damage), static_cast<long>(shotInfo->hitLocation), static_cast<long>(shotInfo->masterId), static_cast<long>(shotInfo->attackerWID));
	printHandleWeaponHitDebugInfo(shotInfo);

	// COMBAT-TRACE-1: weapon HIT resolution is the universal combat chokepoint — scripted ABL
	// attacks (ablmc2.cpp) AND normal weapon fire both land here (unlike LogWeaponFireChunk,
	// which is the multiplayer chunk-send path that never runs in single-player). Emitting here
	// captures all combat headlessly: attacker/target wid+team + damage. Opt-in via
	// MC2_DIAG_TAGS=...,COMBAT (tagEnabled short-circuits to zero cost otherwise).
	if (mc2_diag::tagEnabled("COMBAT")) {
		GameObjectPtr _cbAtk = ObjectManager->getByWatchID(shotInfo->attackerWID);
		char _cbf[192];
		// ISSUE-9 S3: "loc" = resolved hit location (mech body index; 0 = head)
		// so called-shot location distribution is measurable headlessly.
		mc2::fmt::to_buf(_cbf, "{{\"ev\":\"hit\",\"a_wid\":{:d},\"a_team\":{:d},\"t_wid\":{:d},\"t_team\":{:d},\"dmg\":{:.1f},\"loc\":{:d}}}", mc2::fmt::as_u32((unsigned)shotInfo->attackerWID), mc2::fmt::as_i32(_cbAtk ? _cbAtk->getTeamId() : -1L), mc2::fmt::as_u32(getWatchID(false)), mc2::fmt::as_i32(getTeamId()), (double)shotInfo->damage, mc2::fmt::as_i32((int)shotInfo->hitLocation));
		mc2_diag::writeEvent("COMBAT", 1, 0, _cbf);
	}

	if ((getTeam() && Team::noPain[getTeamId()]) || localInvulnerable)
		return(NO_ERR);

	if (!MPlayer && CantTouchThis && pilot->onHomeTeam())
		return(NO_ERR);

	GameObjectPtr attacker = ObjectManager->getByWatchID(shotInfo->attackerWID);
	BadGuy = attacker;

	if ((shotInfo->damage <= 0) || (shotInfo->hitLocation == -1))
		return(NO_ERR);

	// This may not be possible, but just in case. Possibly someone nails
	// a mech after it's been destroyed, but before it's deleted from
	// the object list...
	if (isDestroyed())
		return(NO_ERR);

	numWeaponHitsHandled++;
	if (addMultiplayChunk)
		MPlayer->addWeaponHitChunk(this, shotInfo);

	if (isDisabled() && fallen)
	{
		if (getMoveType() == MOVETYPE_GROUND)
		{
			damageAfterDisabled += shotInfo->damage;
			if (damageAfterDisabled > ((BattleMechTypePtr)getObjectType())->destructDamage)
			{
				//Once the server does this, the status chunk will update the clients!!
				if (!MPlayer || (MPlayer && MPlayer->isServer()))
				{
					timeLeft = 1.0f;
					setStatus(OBJECT_STATUS_DESTROYED);
					exploding = false;
				
					for (long i=0;i<numBodyLocations;i++)		//Blow all of the components.  NO SALVAGE!!
						destroyBodyLocation(i);
		
					if (CombatLog) {
						char s[1024];
						mc2::fmt::to_buf(s, "[{:.2f}] mech.destroyed HWH: [{:05d}]{}", scenarioTime, mc2::fmt::as_i32(getPartId()), mc2::fmt::safe(getName()));
						CombatLog->write(s);
						CombatLog->write(" ");
					}
				}
			}
		}
		else
		{
			setStatus(OBJECT_STATUS_DESTROYED);
			exploding = false;
			
			for (long i=0;i<numBodyLocations;i++)		//Blow all of the components.  NO SALVAGE!!
				destroyBodyLocation(i);
	
	#ifdef USE_MOODMUSIC
			if (collidee->getAlignment() == Team::home->getAlignment())
				friendlyDestroyed = true;
			else
				enemyDestroyed = true;
	#endif
			if (CombatLog) {
				char s[1024];
				mc2::fmt::to_buf(s, "[{:.2f}] helicopter.destroyed: [{:05d}]{}", scenarioTime, mc2::fmt::as_i32(getPartId()), mc2::fmt::safe(getName()));
				CombatLog->write(s);
				CombatLog->write(" ");
			}
		}

		return(NO_ERR);
	}
	
	// Since Multiplayer still needs this, preserve it and restore
	// at the end of this function!
	WeaponShotInfo startShotInfo = *shotInfo;

	bool alreadyDisabled = isDisabled();

	bool isAmmoExplosion = false;
	isAmmoExplosion = (shotInfo->masterId == -4);
	if (shotInfo->masterId > 0)
		isAmmoExplosion = (MasterComponent::masterList[shotInfo->masterId].getForm() == COMPONENT_FORM_AMMO);


	damageRateTally += shotInfo->damage;
	pilotCheckDamageTally += shotInfo->damage;

	float torsoEntryAngle = shotInfo->entryAngle + torsoRotation;
	if ((torsoEntryAngle >= -90.0/*-45.0*/) && (torsoEntryAngle <= 90.0/*45.0*/))
		hitFromFrontThisFrame = true;
	else if ((torsoEntryAngle <= -91.0/*-135.0*/) || (torsoEntryAngle >= 91.0/*135.0*/))
		hitFromBehindThisFrame = true;

	damageThisFrame += shotInfo->damage;
	long totalHitType = 0;
	if (damageThisFrame < hitLevel[0])
		totalHitType = HIT_TYPE_WEAK;
	else if (damageThisFrame < hitLevel[1])
		totalHitType = HIT_TYPE_MODERATE;
	else
		totalHitType = HIT_TYPE_HEAVY;

	long hitType = 0;
	if (shotInfo->damage < hitLevel[0])
		hitType = HIT_TYPE_WEAK;
	else if (shotInfo->damage < hitLevel[1])
		hitType = HIT_TYPE_MODERATE;
	else
		hitType = HIT_TYPE_HEAVY;

	if (!MPlayer && (totalHitType == HIT_TYPE_MODERATE) && (getCommanderId() != Commander::home->getId()))
	{
		if ((torsoEntryAngle >= -45.0) && (torsoEntryAngle <= 45.0))
			appearance->hitFront();
		else if ((torsoEntryAngle <= -45.0) && (torsoEntryAngle >= -135.0))
			appearance->hitLeft();
		else if ((torsoEntryAngle >= 45.0) && (torsoEntryAngle <= 135.0))
			appearance->hitRight();
		else if ((torsoEntryAngle <= -135.0) || (torsoEntryAngle >= 135.0))
			appearance->hitBack();
	}

	// Note that the hitLocation is the ARMOR hit location...
	long bodyLocation = MechArmorToBodyLocation[shotInfo->hitLocation];

	if ((bodyLocation == MECH_BODY_LOCATION_HEAD) && (shotInfo->damage >= 2))
		pilot->injure(1);
	
	bool armorHoled = false;
	if ((armor[shotInfo->hitLocation].curArmor > 0) && !isAmmoExplosion) 
	{
		if (shotInfo->damage > armor[shotInfo->hitLocation].curArmor) 
		{
			shotInfo->setDamage(shotInfo->damage - armor[shotInfo->hitLocation].curArmor);
			armor[shotInfo->hitLocation].curArmor = 0;

			if (!armorHoled)
				pilot->radioMessage(RADIO_ARMORHOLE);

			armorHoled = true;

			//Check for Tutorials here.
			// IN the tutorial missions, your mechs can only have armor damaged.  NO INTERNALS ALLOWED!!
			// ONLY the tutorials should set this flag through ABL!!!
			if (!localInvulnerable)
			{
				if (shotInfo->damage >= body[bodyLocation].curInternalStructure) 
				{
					shotInfo->setDamage(shotInfo->damage - body[bodyLocation].curInternalStructure);
					injureBodyLocation(bodyLocation, body[bodyLocation].curInternalStructure);
					if (shotInfo->damage > 0) 
					{
						if ((bodyLocation == MECH_BODY_LOCATION_CTORSO) || (bodyLocation == MECH_BODY_LOCATION_HEAD)) 
						{
							// If hit in the center torso, no where else to go...
							// If hit in the head, it is disabled. What that means at this point...?
							// We'll probably want to put a disabled check in there, somewhere :)
							// Most likely, the mech will have been blown up and/or destroyed before
							// it's "disabled"...
						}
						else 
						{
							WeaponShotInfo newShotInfo = *shotInfo;
							newShotInfo.hitLocation = transferHitLocation(bodyLocation/*shotInfo->hitLocation*/);
							if (MPlayer) 
							{
								if (MPlayer->isServer()) 
								{
									if (CombatLog)
										CombatLog->write("     HIT TRANSFER 1...");
									handleWeaponHit(&newShotInfo, true);
								}
							}
							else 
							{
								if (CombatLog)
									CombatLog->write("     HIT TRANSFER 1...");
								handleWeaponHit(&newShotInfo);
							}
						}
					}
				}
				else
				{
					injureBodyLocation(bodyLocation, shotInfo->damage);
				}
			}
		}
		else
		{
			armor[shotInfo->hitLocation].curArmor -= shotInfo->damage;
		}
	}
	else if (!localInvulnerable)	//Again, for tutorials.  See above check.
	{
		if (body[bodyLocation].curInternalStructure > 0)
		{
			if (shotInfo->damage >= body[bodyLocation].curInternalStructure) 
			{
				shotInfo->setDamage(shotInfo->damage - body[bodyLocation].curInternalStructure);
				injureBodyLocation(bodyLocation, body[bodyLocation].curInternalStructure);
				if (shotInfo->damage > 0) 
				{
					if (isAmmoExplosion && body[bodyLocation].CASE) 
					{
						long armorLocation = MECH_ARMOR_LOCATION_RCTORSO;
						switch (bodyLocation) 
						{
							case MECH_BODY_LOCATION_LTORSO:
							case MECH_BODY_LOCATION_LARM:
							case MECH_BODY_LOCATION_LLEG:
								armorLocation = MECH_ARMOR_LOCATION_RLTORSO;
								break;
							case MECH_BODY_LOCATION_RTORSO:
							case MECH_BODY_LOCATION_RARM:
							case MECH_BODY_LOCATION_RLEG:
								armorLocation = MECH_ARMOR_LOCATION_RRTORSO;
								break;
						}

						if (shotInfo->damage > armor[armorLocation].curArmor) 
						{
							armor[armorLocation].curArmor = 0;
							armorHoled = true;
						}
						else
							armor[armorLocation].curArmor -= shotInfo->damage;

						shotInfo->setDamage(0);
					}
					else 
					{
						if ((bodyLocation == MECH_BODY_LOCATION_CTORSO) || (bodyLocation == MECH_BODY_LOCATION_HEAD)) 
						{
							// Hit center torso, already, or
							// Head wiped out... what to do?
						}
						else 
						{
							WeaponShotInfo newShotInfo = *shotInfo;
							newShotInfo.hitLocation = transferHitLocation(bodyLocation/*shotInfo->hitLocation*/);
							if (MPlayer) 
							{
								if (MPlayer->isServer()) 
								{
									if (CombatLog)
										CombatLog->write("     HIT TRANSFER 2...");
									handleWeaponHit(&newShotInfo, true);
								}
							}
							else 
							{
								if (CombatLog)
									CombatLog->write("     HIT TRANSFER 2...");
								handleWeaponHit(&newShotInfo);
							}
						}
					}
				}
			}
			else
			{
				injureBodyLocation(bodyLocation, shotInfo->damage);
			}
		}
		else 
		{
			if (isAmmoExplosion && body[bodyLocation].CASE) 
			{
				long armorLocation = MECH_ARMOR_LOCATION_RCTORSO;
				switch (bodyLocation) 
				{
					case MECH_BODY_LOCATION_LTORSO:
					case MECH_BODY_LOCATION_LARM:
					case MECH_BODY_LOCATION_LLEG:
						armorLocation = MECH_ARMOR_LOCATION_RLTORSO;
						break;
					case MECH_BODY_LOCATION_RTORSO:
					case MECH_BODY_LOCATION_RARM:
					case MECH_BODY_LOCATION_RLEG:
						armorLocation = MECH_ARMOR_LOCATION_RRTORSO;
						break;
				}
				if (shotInfo->damage > armor[armorLocation].curArmor) 
				{
					armor[armorLocation].curArmor = 0;
					armorHoled = true;
				}
				else
					armor[armorLocation].curArmor -= shotInfo->damage;

				shotInfo->setDamage(0);
			}
			else 
			{
				if ((bodyLocation == MECH_BODY_LOCATION_CTORSO) || (bodyLocation == MECH_BODY_LOCATION_HEAD)) 
				{
					// Already in center torso, or
					// Head wiped out... what to do?
				}
				else 
				{
					WeaponShotInfo newShotInfo = *shotInfo;
					newShotInfo.hitLocation = transferHitLocation(bodyLocation/*shotInfo->hitLocation*/);
					if (MPlayer) 
					{
						if (MPlayer->isServer()) 
						{
							if (CombatLog)
								CombatLog->write("     HIT TRANSFER 3...");
							handleWeaponHit(&newShotInfo, true);
						}
					}
					else 
					{
						if (CombatLog)
							CombatLog->write("     HIT TRANSFER 3...");
						handleWeaponHit(&newShotInfo);
					}
				}
			}
		}
	}

	if (inventory[gyro].disabled)
	{
		switch (hitType)
		{
			case HIT_TYPE_WEAK:
				break;
			case HIT_TYPE_MODERATE:
				break;
		}
	}


	if (attacker) 
	{
		if (shotInfo->masterId > -1)
			pilot->triggerAlarm(PILOT_ALARM_HIT_BY_WEAPONFIRE, attacker->getWatchID());
		else if (shotInfo->masterId == -4)
			pilot->triggerAlarm(PILOT_ALARM_HIT_BY_WEAPONFIRE, attacker->getWatchID());
		else
			pilot->triggerAlarm(PILOT_ALARM_COLLISION, attacker->getWatchID());
	}
	else
	{
		if (shotInfo->masterId == -4)
			pilot->triggerAlarm(PILOT_ALARM_HIT_BY_WEAPONFIRE, 0);
		else if (shotInfo->masterId < 0) {
			// If no attacker and a negative weapon masterId, then it must
			// be a mine or artillery...
			pilot->triggerAlarm(PILOT_ALARM_HIT_BY_WEAPONFIRE, shotInfo->masterId);
			}
		else
			pilot->triggerAlarm(PILOT_ALARM_HIT_BY_WEAPONFIRE, 0);
	}

	curCV = calcCV(false);
	if (isDisabled())
		setThreatRating(1);
	else
		setThreatRating(-1);

	if (!alreadyDisabled && isDisabled()) {
		if (attacker && attacker->isMover()) 
		{
			((MoverPtr)attacker)->getPilot()->triggerAlarm(PILOT_ALARM_KILLED_TARGET, getWatchID());
			if (!killed && MPlayer && MPlayer->isServer()) 
			{
				if (moveType != MOVETYPE_AIR) {
					if (MPlayer->missionSettings.resourceForMechs)
						if (attacker->getCommanderId() != getCommanderId())
							MPlayer->sendReinforcement(getResourcePointValue(), 0, "noname", attacker->getCommanderId(), attacker->getPosition(), 6);
					if (MPlayer->missionSettings.missionType == MISSION_TYPE_ELIMINATION) 
					{
						if (attacker->getCommanderId() != getCommanderId())
							MPlayer->addTeamScore(MPlayer->playerInfo[attacker->getCommanderId()].team, cBills / 100);
					}
				}
				long attackerCID = attacker->getCommanderId();
				if (attackerCID == -1)
					attackerCID = ((MoverPtr)attacker)->prevCommanderId;
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
				if (moveType != MOVETYPE_AIR) {
					if (MPlayer->missionSettings.resourceForMechs)
						if (attacker->getCommanderId() != getCommanderId())
							MPlayer->sendReinforcement(getResourcePointValue(), 0, "noname", attacker->getCommanderId(), attacker->getPosition(), 6);
					if (MPlayer->missionSettings.missionType == MISSION_TYPE_ELIMINATION) 
					{
						if (attacker->getCommanderId() != getCommanderId())
							MPlayer->addTeamScore(MPlayer->playerInfo[attacker->getCommanderId()].team, cBills / 100);
					}
				}

				long attackerCID = attacker->getCommanderId();
				if (attackerCID == -1)
					attackerCID = ((MoverPtr)attacker)->prevCommanderId;
				if ((attackerCID == -1) || (attackerCID == getCommanderId()))
					attackerCID = MAX_MC_PLAYERS;
				MPlayer->addKillLossChunk(attackerCID, !lost ? getCommanderId() : MAX_MC_PLAYERS);
				lost = true;
				killed = true;
			}
		}
	}

	*shotInfo = startShotInfo;

	return(NO_ERR);
}
