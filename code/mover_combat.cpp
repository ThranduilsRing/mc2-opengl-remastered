// Mover: tactical orders, weapon helpers, weapon-fire/critical-hit/radio chunk queues, contacts and weapon queries.
// Declared in mover.h; shared names in mover_internal.h.
#include "mover_internal.h"
#include "Gates/gate.h"
#include "mc2fmt.h"
#include "score_sort.h"
#include "../GameOS/gameos/utils/mc2log.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>


long Mover::handleTacticalOrder (TacticalOrder tacOrder, long priority, bool queuePlayerOrder) {

	// TARGETING-CURRENTNESS-GUARD-1 (chokepoint, non-fatal recon — FRAME-CURRENTNESS-GUARDS-1).
	// EVERY player attack order from every GUI handler funnels through here (the GUI-side
	// doAttack guard missed orders that route via other handlers). Log EVERY player
	// attack order — not just anomalies — plus running totals, so a clean run (orders>0,
	// stale=0) is distinguishable from a no-order run. Signals: stale_wid (the order's
	// targetWID no longer resolves to a live object = stale/dead target accepted) and
	// range (target distance from the ordered mover — an absurd range is the "eligible
	// across the whole map" symptom). Gate MC2_TARGETING_GUARD + MC2_DIAG_TAGS=TARGETING.
	{
		static const bool s_tgtGuard = mc2::gate::get<"MC2_TARGETING_GUARD">();
		if (s_tgtGuard &&
		    tacOrder.origin == ORDER_ORIGIN_PLAYER &&
		    tacOrder.code == TACTICAL_ORDER_ATTACK_OBJECT &&
		    mc2_diag::tagEnabled("TARGETING"))
		{
			static uint64_t s_tg_orders = 0, s_tg_stale = 0, s_tg_far = 0;
			++s_tg_orders;
			GameObjectPtr _tg_tgt = ObjectManager->getByWatchID(tacOrder.targetWID);
			const bool _tg_stale = (_tg_tgt == nullptr);
			const float _tg_range = _tg_tgt ? distanceFrom(_tg_tgt->getPosition()) : -1.0f;
			const bool _tg_far = (_tg_range > 3000.0f);   // far-field hint (whole-map symptom)
			if (_tg_stale) ++s_tg_stale;
			if (_tg_far)   ++s_tg_far;
			char _tg_buf[320];
			mc2::fmt::to_buf(_tg_buf, "{{\"site\":\"handleTacticalOrder\",\"order\":{:d},\"targetWID\":{:d},\"stale_wid\":{:d},\"range\":{:.1f},\"far\":{:d},\"totals\":{{\"orders\":{:d},\"stale\":{:d},\"far\":{:d}}}}}", mc2::fmt::as_u64(static_cast<unsigned long long>(s_tg_orders)), mc2::fmt::as_i32(static_cast<int>(tacOrder.targetWID)), mc2::fmt::as_i32(_tg_stale?1:0), _tg_range, mc2::fmt::as_i32(_tg_far?1:0), mc2::fmt::as_u64(static_cast<unsigned long long>(s_tg_orders)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_tg_stale)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_tg_far)));
			mc2_diag::writeEvent("TARGETING", 1, 0, _tg_buf);
		}
	}

if (queuePlayerOrder)
	tacOrder.pack(nullptr, nullptr);

#if 1
	if (MPlayer && !MPlayer->isServer()) {
		tacOrder.pack(nullptr, nullptr);
		TacticalOrder tacOrder2;
		tacOrder2.data[0] = tacOrder.data[0];
		tacOrder2.data[1] = tacOrder.data[1];
		tacOrder2.unpack();
	}
#endif

	// Any tacorder that gets here IS NOT a lance order...
	bool processOrder = true;
	long message = -1;
	switch (tacOrder.code) {
		case TACTICAL_ORDER_WAIT:
			suppressionFire = false;
			break;
		case TACTICAL_ORDER_MOVETO_POINT:
			if (selectionIndex != -1) 
			{
				tacOrder.delayedTime = scenarioTime + (selectionIndex * DelayedOrderTime);
			}

			if (!canMove())
			{
				//DO NOT send readio message.  Players will know artillery cannot move.
				// BUT do not allow order through.  Will cancel out a forceFire command!
				processOrder = false;
			}
			break;
		case TACTICAL_ORDER_JUMPTO_POINT:
		case TACTICAL_ORDER_JUMPTO_OBJECT: {
			bool canJump = mc2UnitProfileDataEnabled()
				? canPerform(UnitAction::Jump)
				: (getObjectClass() == BATTLEMECH);
			GameObjectPtr target = tacOrder.getTarget();
			if (target && target->isMover() && (target->getTeam() == getTeam()))
				canJump = false;
			if (getJumpRange() < distanceFrom(tacOrder.getWayPoint(0)))
				canJump = false;
			if (getObjectClass() == BATTLEMECH) {
				int cellR, cellC;
				land->worldToCell(tacOrder.getWayPoint(0), cellR, cellC);
			if (!GameMap->getPassable(cellR, cellC))
					canJump = false;
			}
			if (!canJump) {
				message = RADIO_ILLEGAL_ORDER;
				processOrder = false;
			}

			}
			// No break here--should fall down below...
		case TACTICAL_ORDER_MOVETO_OBJECT:
		case TACTICAL_ORDER_TRAVERSE_PATH:
		case TACTICAL_ORDER_PATROL_PATH:
			if (isDisabled() || !canMove()) {
				processOrder = false;
			}
			break;
		case TACTICAL_ORDER_ATTACK_OBJECT:
			if (tacOrder.attackParams.method == ATTACKMETHOD_DFA) {
				tacOrder.code = TACTICAL_ORDER_JUMPTO_OBJECT;
				tacOrder.moveParams.wait = false;
				tacOrder.moveParams.wayPath.mode[0] = TRAVEL_MODE_SLOW;
				GameObjectPtr target = tacOrder.getTarget();
				if (target)
					tacOrder.setWayPoint(0, target->getPosition());
			}
			if (tacOrder.attackParams.aimLocation != -1)
			{
				message = RADIO_CALLED_SHOT;
			}
			break;
		case TACTICAL_ORDER_POWERUP:
		case TACTICAL_ORDER_POWERDOWN:
		case TACTICAL_ORDER_ESCORT:
		case TACTICAL_ORDER_FOLLOW:
		case TACTICAL_ORDER_GUARD:
		case TACTICAL_ORDER_STOP:
		case TACTICAL_ORDER_WAYPOINTS_DONE:
		case TACTICAL_ORDER_EJECT:
		case TACTICAL_ORDER_HOLD_FIRE:
		case TACTICAL_ORDER_WITHDRAW:
		case TACTICAL_ORDER_ATTACK_POINT:
		case TACTICAL_ORDER_REFIT:
		case TACTICAL_ORDER_GETFIXED:
		case TACTICAL_ORDER_CAPTURE:
		case TACTICAL_ORDER_RECOVER:
		case TACTICAL_ORDER_LOAD_INTO_CARRIER:
		case TACTICAL_ORDER_DEPLOY_ELEMENTALS:
			break;
		default: {
			char s[256];
			mc2::fmt::to_buf(s, "Mover::handleTacticalOrder->Bad TacOrder Code ({:d})", mc2::fmt::as_i32(tacOrder.code));
			Assert(false, tacOrder.code, s);
			return(1);
			}
	}

	if (pilot)
		pilot->radioMessage(message, true);

	// Acknowledgement of a good order is done when the order
	// is executed...
	if (processOrder)
		switch (tacOrder.origin) {
			case ORDER_ORIGIN_PLAYER:
				if (queuePlayerOrder)
					pilot->addQueuedTacOrder(tacOrder);
				else {
//					if (pilot->getNumTacOrdersQueued())
						//This is a hack to simply trigger the execution of
						//the queued orders. The current order is ignored (and
						//is simply used for this trigger)...
//						pilot->executeTacOrderQueue();
//					else
						pilot->setPlayerTacOrder(tacOrder);
				}
				break;
			case ORDER_ORIGIN_COMMANDER:
				pilot->setGeneralTacOrder(tacOrder);
				break;
			case ORDER_ORIGIN_SELF:
				pilot->setAlarmTacOrder(tacOrder, priority);
				break;
		}
	return(NO_ERR);
}

void Mover::reduceAntiMissileAmmo (long numAntiMissiles) {

	if (numAntiMissiles > 0)
		reduceAmmo(MasterComponent::masterList[inventory[antiMissileSystem[0]].masterID].getWeaponAmmoMasterId(), numAntiMissiles);
}

void Mover::pilotingCheck (unsigned long situation, float modifier) {

	failedPilotingCheck = false;
}

void Mover::updateDamageTakenRate (void) {

	if (damageRateCheckTime < scenarioTime) {
		long damageRate = (damageRateTally / DamageRateFrequency);
		if (damageRate > 10 /*AttitudeEffect[pilot->getAttitude(ORDER_CURRENT)][5]*/)
			pilot->triggerAlarm(PILOT_ALARM_DAMAGE_TAKEN_RATE, damageRate);
		damageRateTally = 0;
		damageRateCheckTime += DamageRateFrequency;
	}
}

void Mover::setPilotHandle (long _pilotHandle) {

	if (pilot) {
		pilot->setVehicle(nullptr);
		pilot = nullptr;
	}
	pilotHandle = _pilotHandle;
	if (pilotHandle > 0) {
		pilot = MechWarrior::warriorList[pilotHandle];
		pilot->setVehicle(static_cast<GameObjectPtr>(this));
	}
}

void Mover::loadPilot (const char* pilotFileName, const char* brainFileName, LogisticsPilot *lPilot) {

	if (pilot) {
		MechWarrior::freeWarrior(pilot);
		pilot = nullptr;
	}

	// Loads a new pilot into the pilot manager and then puts it into
	// this mover. If a pilot already exists in this mover, it is
	// replaced...
	MechWarriorPtr pilot = MechWarrior::newWarrior();
	if (!pilot)
		STOP(("Too many pilots in this mission!"));

	FullPathFileName pilotFullFileName;
	pilotFullFileName.init(warriorPath, pilotFileName, ".fit");
			
	FitIniFile pilotFile;
		
	long result = pilotFile.open(pilotFullFileName);
	gosASSERT(result == NO_ERR);
	result = pilot->init(&pilotFile);
	gosASSERT(result == NO_ERR);
			
	pilotFile.close();

	//Copy logistics data to pilot AFTER loading old data.
	// ONLY if we overrode the data in logistics!!
	if (lPilot)
	{
		pilot->skills[MWS_GUNNERY] = pilot->skillRank[MWS_GUNNERY] = lPilot->getGunnery();
		pilot->skills[MWS_PILOTING] = pilot->skillRank[MWS_PILOTING] = lPilot->getPiloting();
		memcpy(pilot->specialtySkills,lPilot->getSpecialtySkills(),sizeof(bool) * NUM_SPECIALTY_SKILLS);
		pilot->calcRank();
	}

	// NOTE: Need to send packet to other players in MP with new pilot and
	// mover data!

	long numErrors, numLinesProcessed;
	FullPathFileName brainFullFileName;
	if (MPlayer) {
		pilot->setBrainName("pbrain");
		brainFullFileName.init(warriorPath, "pbrain", ".abl");
		}
	else {
		pilot->setBrainName(brainFileName);
		brainFullFileName.init(warriorPath, brainFileName, ".abl");
	}
		
	long moduleHandle = ABLi_preProcess(brainFullFileName, &numErrors, &numLinesProcessed);
	pilot->setBrain(moduleHandle);
	setPilotHandle(pilot->getIndex());
}

void Mover::setCommanderId (long _commanderId) {

	if (commanderId > -1)
		prevCommanderId = commanderId;
	commanderId = _commanderId;
}

MoverPtr Mover::getPoint (void) {

#ifdef USE_GROUPS
	if (group)
		return(group->getPoint());
#endif
	return(nullptr);
}

bool Mover::hasWeaponNode (void)
{

	if ((lowestWeaponNodeID == -1) || (lowestWeaponNodeID == -2) || (turn < 6))
	{
		lowestWeaponNodeID = appearance->getLowestWeaponNode();
		glm::vec3 nodePos = appearance->getWeaponNodePosition(lowestWeaponNodeID);
		lowestWeaponNodeZ = nodePos.z - land->getTerrainElevation(nodePos);
	}

	return (lowestWeaponNodeID != -1);
}

glm::vec3 Mover::getLOSPosition (void)
{
	float jumpLowestNode = 0.0f;
	if (appearance)
	{
		bool oldInView = appearance->canBeSeen();
		appearance->setInView(true);

		if ((getStatus() == OBJECT_STATUS_NORMAL) &&
			((appearance->getCurrentGestureId() == 2) || 
			 (appearance->getCurrentGestureId() == 4) || 
			 (appearance->getCurrentGestureId() == 7) || !isMech()) && 
			((lowestWeaponNodeID == -1) || (lowestWeaponNodeID == -2) || (turn < 6)) )
		{
			lowestWeaponNodeID = appearance->getLowestWeaponNode();
			if (lowestWeaponNodeID == -1)
			{
				//We must not have ever rendered this guy.  Probably multiplayer.
				// Update his Geometry.
				appearance->update();

				lowestWeaponNodeID = appearance->getLowestWeaponNode();
			}

			glm::vec3 nodePos = appearance->getWeaponNodePosition(lowestWeaponNodeID);
			lowestWeaponNodeZ = nodePos.z - land->getTerrainElevation(nodePos);
		}

		if (appearance->getCurrentGestureId() == 20)
		{
			// Gesture 20 is jump; lowestWeaponNodeZ is only refreshed for
			// gestures 2/4/7, so we recompute every frame here. Pose
			// oscillation can drop the raw value to/below 0, which then
			// fails the `> 0.0f` override below and drops the LOS ray to
			// the stale cached Z (often below terrain) -- every weapon's
			// lineOfSight test fails that frame. Clamp to the same
			// shutdown-mech fudge used as the fallback below.
			glm::vec3 nodePos = appearance->getWeaponNodePosition(lowestWeaponNodeID);
			float rawZ = nodePos.z - land->getTerrainElevation(nodePos);
			jumpLowestNode = (rawZ > 15.0f) ? rawZ : 15.0f;
		}

		if (lowestWeaponNodeZ <= 0.0f)
		{
			glm::vec3 nodePos = appearance->getWeaponNodePosition(lowestWeaponNodeID);
			lowestWeaponNodeZ = nodePos.z - land->getTerrainElevation(nodePos);
		}

		appearance->setInView(oldInView);
	}

	glm::vec3 losPos = position;
	if (lowestWeaponNodeZ != -9999.9f)
		losPos.z += lowestWeaponNodeZ;
	else
		losPos.z += 15.0f;		  //Fudge in some higher value so shutdown mechs don't just disappear!!

	if (jumpLowestNode > 0.0f)
	{
		losPos = position;
		losPos.z += jumpLowestNode;
	}

	return (losPos);
}

long Mover::clearWeaponFireChunks (long which) {

	long numChunks = numWeaponFireChunks[which];
	numWeaponFireChunks[which] = 0;
	return(numChunks);
}

long Mover::addWeaponFireChunk (long which, WeaponFireChunkPtr chunk) {

	if (numWeaponFireChunks[which] == MAX_WEAPONFIRE_CHUNKS)
		Fatal(0, " Mover::addWeaponFireChunk--Too many weaponfire chunks ");

	chunk->pack(this);
	WeaponFireChunk testChunk;
	testChunk.init();
	testChunk.data = chunk->data;
	testChunk.unpack(this);
	if (!chunk->equalTo(&testChunk)) {
		DebugWeaponFireChunk(chunk, &testChunk, this);
		Assert(false, 0, " Mover.addWeaponFireChunk: bad weaponfire chunk (save wfchunk.dbg file) ");
	}

	weaponFireChunks[which][numWeaponFireChunks[which]++] = chunk->data;
	return(numWeaponFireChunks[which]);
}

long Mover::addWeaponFireChunks (long which, unsigned long* packedChunkBuffer, long numChunks) {

	if ((numWeaponFireChunks[which] + numChunks) >= MAX_WEAPONFIRE_CHUNKS)
		Fatal(0, " Mover::addWeaponFireChunks--Too many weaponfire chunks ");

	for (long i = 0; i < numChunks; i++) {
		weaponFireChunks[which][numWeaponFireChunks[which]++] = packedChunkBuffer[i];
		WeaponFireChunk chunk;
		chunk.init();
		chunk.data = packedChunkBuffer[i];
		chunk.unpack(this);
	}
	return(numWeaponFireChunks[which]);
}

long Mover::grabWeaponFireChunks (long which, unsigned long* packedChunkBuffer, long maxChunks) {

	long numChunks = numWeaponFireChunks[which];
	if (numChunks > maxChunks)
		numChunks = maxChunks;
	if (numChunks > 0) {
		for (long i = 0; i < numChunks; i++)
			packedChunkBuffer[i] = weaponFireChunks[which][i];
	}
	numWeaponFireChunks[which] -= numChunks;
	return(numChunks);
}

long Mover::updateWeaponFireChunks (long which) {

	for (long i = 0; i < numWeaponFireChunks[which]; i++) {
		WeaponFireChunk chunk;
		chunk.init();
		chunk.data = weaponFireChunks[which][i];
		chunk.unpack(this);

		CurMoverWeaponFireChunk = chunk;

		static float entryQuadTable[4] = {0.0, 180.0, -90.0, 90.0};

		long weaponIndex = chunk.weaponIndex + numOther;
		if (!isWeaponIndex(weaponIndex))
			continue;

		TargetRolo = chunk.targetType;
		if (chunk.targetType == 0 /*WEAPONFIRECHUNK_TARGET_MOVER*/) {
			GameObjectPtr target = static_cast<GameObjectPtr>(MPlayer->moverRoster[chunk.targetId]);
			// Mover targets could be NULL now, since we free them when they're destroyed.
			if (target)
				handleWeaponFire(weaponIndex,
							 target,
							 nullptr,
							 chunk.hit,
							 entryQuadTable[chunk.entryAngle],
							 chunk.numMissiles,
							 chunk.hitLocation);
			}
		else if (chunk.targetType == 1 /*WEAPONFIRECHUNK_TARGET_TERRAIN*/) {
			GameObjectPtr target = ObjectManager->findByPartId(chunk.targetId);
			if (target == nullptr) {
				DebugWeaponFireChunk (&chunk, nullptr, this);
				Assert(false, 0, " Mover.updateWeaponFireChunks: NULL Terrain Target (save wfchunk.dbg file) ");
			}
			handleWeaponFire(weaponIndex,
							 target,
							 nullptr,
							 chunk.hit,
							 entryQuadTable[chunk.entryAngle],
							 chunk.numMissiles,
							 chunk.hitLocation);
			}
		else if (chunk.targetType == 2 /*WEAPONFIRECHUNK_TARGET_TRAIN*/) {
			GameObjectPtr target = ObjectManager->findByPartId(chunk.targetId);
			if (target == nullptr) {
				DebugWeaponFireChunk (&chunk, nullptr, this);
				Assert(false, 0, " Mover.updateWeaponFireChunks: NULL Special Target (save wfchunk.dbg file) ");
			}
			handleWeaponFire(weaponIndex,
							 target,
							 nullptr,
							 chunk.hit,
							 entryQuadTable[chunk.entryAngle],
							 chunk.numMissiles,
							 chunk.hitLocation);
			}
		else if (chunk.targetType == 3 /*WEAPONFIRECHUNK_TARGET_LOCATION*/) {
			glm::vec3 targetPoint;
			targetPoint.x = static_cast<float>(chunk.targetCell[1]) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
			targetPoint.y = (Terrain::worldUnitsMapSide / 2) - (static_cast<float>(chunk.targetCell[0]) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
			targetPoint.z = land->getTerrainElevation(targetPoint);
			handleWeaponFire(weaponIndex, nullptr, &targetPoint, chunk.hit, 0.0, chunk.numMissiles, 0);
			}
		else
			Fatal(0, " Mover.updateWeaponFireChunks: bad targetType ");
	}

	numWeaponFireChunks[which] = 0;

	return(NO_ERR);
}

long Mover::clearCriticalHitChunks (long which) {

	long numChunks = numCriticalHitChunks[which];
	numCriticalHitChunks[which] = 0;
	return(numChunks);
}

long Mover::addCriticalHitChunk (long which, long bodyLocation, long criticalSpace) {

	if (numCriticalHitChunks[which] == MAX_CRITICALHIT_CHUNKS)
		Fatal(0, " Mover::addCriticalHitChunk--Too many criticalhit chunks ");

	unsigned char chunkData = static_cast<unsigned char>(((bodyLocation << 4) + criticalSpace));
	criticalHitChunks[which][numCriticalHitChunks[which]++] = chunkData;
	return(numCriticalHitChunks[which]);
}

long Mover::addCriticalHitChunks (long which, unsigned char* packedChunkBuffer, long numChunks) {

	if ((numCriticalHitChunks[which] + numChunks) >= MAX_CRITICALHIT_CHUNKS)
		Fatal(0, " Mover::addCriticalHitChunks--Too many criticalhit chunks ");

	memcpy(&criticalHitChunks[which][numCriticalHitChunks[which]], packedChunkBuffer, numChunks);
	numCriticalHitChunks[which] += numChunks;
	return(numCriticalHitChunks[which]);
}

long Mover::grabCriticalHitChunks (long which, unsigned char* packedChunkBuffer) {

	if (numCriticalHitChunks[which] > 0)
		memcpy(packedChunkBuffer, criticalHitChunks[which], numCriticalHitChunks[which]);
	return(numCriticalHitChunks[which]);
}

long Mover::updateCriticalHitChunks (long which) {

	numCriticalHitChunks[which] = 0;

	return(NO_ERR);
}

long Mover::clearRadioChunks (long which) {

	long numChunks = numRadioChunks[which];
	numRadioChunks[which] = 0;
	return(numChunks);
}

long Mover::addRadioChunk (long which, unsigned char msg) {

	if (numRadioChunks[which] == MAX_RADIO_CHUNKS)
		return(numRadioChunks[which]);

	radioChunks[which][numRadioChunks[which]++] = msg;
	return(numRadioChunks[which]);
}

long Mover::addRadioChunks (long which, unsigned char* packedChunkBuffer, long numChunks) {

	for (long i = 0; i < numChunks; i++)
		addRadioChunk(which, packedChunkBuffer[i]);
	return(numRadioChunks[which]);
}

long Mover::grabRadioChunks (long which, unsigned char* packedChunkBuffer) {

	if (numRadioChunks[which] > 0)
		memcpy(packedChunkBuffer, radioChunks[which], numRadioChunks[which]);
	return(numRadioChunks[which]);
}

long Mover::updateRadioChunks (long which) {

	if (getCommander() == Commander::home)
		for (long i = 0; i < numRadioChunks[which]; i++)
			playMessage(static_cast<RadioMessageType>(radioChunks[which][i]));

	numRadioChunks[which] = 0;

	return(NO_ERR);
}

void Mover::playMessage (RadioMessageType messageId, bool propogateIfMultiplayer) {

	if (pilot)
		pilot->radioMessage(messageId, propogateIfMultiplayer);
}

void Mover::setThreatRating (short rating) {

	threatRating = rating;
	if (threatRating == -1) {
		threatRating = calcCV();
	}
}

long Mover::getThreatRating (void) {

	if ((scenarioTime - creationTime) < 5.0)
		return(static_cast<long>((static_cast<float>(threatRating) * newThreatMultiplier)));
	return(threatRating);
}

bool Mover::enemyRevealed (void)
{

	//NOTE: This function not used Anymore!!

	return(false);
}

void Mover::setTeleportPosition (glm::vec3& newPos) {

	teleportPosition = newPos;
}

long Mover::getInventoryDamage (long itemIndex) {

	if (itemIndex < (numOther + numWeapons + numAmmos))
		return (MasterComponent::masterList[inventory[itemIndex].masterID].getHealth() - inventory[itemIndex].health);
	return(0);
}

float Mover::getVisualRange (void) {

	return(MechWarrior::maxVisualRadius);
}

void Mover::setChallenger (GameObjectPtr challenger) {

	challengerWID = challenger->getWatchID();
}

GameObjectPtr Mover::getChallenger (void) {

	// We ASSUME a disabled challenger is as good as none...
	GameObjectPtr obj = ObjectManager->get(challengerWID);
	if (obj && obj->isDisabled()) {
		challengerWID = 0;
		return(nullptr);
	}
	return(obj);
}

long Mover::getContacts (int* contactList, int contactCriteria, int sortType) {

	if (sensorSystem)
		return(sensorSystem->getTeamContacts(contactList, contactCriteria, sortType));
	return(0);
}

long Mover::getContactStatus (long scanningTeamID, bool includingAllies) {

	if (getFlag(OBJECT_FLAG_REMOVED))
		return(CONTACT_NONE);
	return(getContactInfo()->getContactStatus(scanningTeamID, true));
}

float Mover::weaponLocked (long weaponIndex, glm::vec3 targetPosition) {

	long bodyLocation = inventory[weaponIndex].bodyLocation;
	return(relFacingTo(targetPosition, bodyLocation));
}

bool Mover::weaponInRange (long weaponIndex, float metersToTarget, float buffer) {

	MasterComponentPtr weapon = &MasterComponent::masterList[inventory[weaponIndex].masterID];

	float minRange = WeaponRanges[weapon->getWeaponRange()][0] - buffer;
	float maxRange = WeaponRanges[weapon->getWeaponRange()][1] + buffer;
	if (metersToTarget < minRange)
		return(false);
	if (metersToTarget > maxRange)
		return(false);
	return(true);
}

long Mover::getWeaponsReady (int* list, int listSize) {

	// Make sure list is >= the number of ready
	// weapons!

	long numReady = 0;
	if (listSize == -1)
		for (long item = numOther; item < (numOther + numWeapons); item++) {
			if (isWeaponReady(item)) {
				if (list)
					list[numReady++] = item;
				else
					numReady++;
			}
		}
	else
		for (long item = 0; item < listSize; item++) {
			if (isWeaponReady(list[item])) {
				if (list)
					list[numReady++] = list[item];
				else
					numReady++;
			}
		}

	return(numReady);
}

long Mover::getWeaponsLocked (int* list, int listSize) {


	GameObjectPtr target = pilot->getCurrentTarget();
	if (!target)
		return(-2);

	glm::vec3 targetPosition = target->getPosition();

	long numLocked = 0;
	float fireArc = getFireArc();
	if (listSize == -1)
		for (long item = numOther; item < (numOther + numWeapons); item++) {
			float relAngle = weaponLocked(item, targetPosition);
			if ((relAngle >= -fireArc) && (relAngle <= fireArc))
				list[numLocked++] = item;
		}
	else
		for (long item = 0; item < listSize; item++) {
			float relAngle = weaponLocked(list[item], targetPosition);
			if ((relAngle >= -fireArc) && (relAngle <= fireArc))
				list[numLocked++] = list[item];
		}
	return(numLocked);
}

long Mover::getWeaponsInRange (int* list, int listSize, float fireRangeOrder) {

	GameObjectPtr target = pilot->getCurrentTarget();
	if (!target)
		return(-2);

	float rangeToTarget = distanceFrom(target->getPosition());

	#ifdef STUPID_RULES
	if (rangeToTarget > fireRangeOrder)
		return(-3);
	#endif	

	long numInRange = 0;
	if (listSize == -1) {
		for (long item = numOther; item < (numOther + numWeapons); item++)
			if (weaponInRange(item, rangeToTarget, MapCellDiagonal))
				list[numInRange++] = item;
		}
	else {
		for (long item = 0; item < listSize; item++)
			if (weaponInRange(list[item], rangeToTarget, MapCellDiagonal))
				list[numInRange++] = list[item];
	}
	return(numInRange);
}

long Mover::getWeaponShots (long weaponIndex) {

	if (!isWeaponIndex(weaponIndex))
		return(-1);

	// All ammo, as listed in the Master Component Table, is a fixed index
	// greater than the weapon itself.
	if (MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponAmmoType())
		return(ammoTypeTotal[inventory[weaponIndex].ammoIndex].curAmount);

	return(UNLIMITED_SHOTS);
}

bool Mover::getWeaponIndirectFire (long weaponIndex)
{
	if (!isWeaponIndex(weaponIndex))
		return(false);

	for (long i=0;i<20;i++)
	{
		if (IndirectFireWeapons[i] == inventory[weaponIndex].masterID)
			return true;
	}
			
	// If no ammotype, then it has unlimited ammo (energy weapons, for example).
	return(false);
}

bool Mover::getWeaponAreaEffect (long weaponIndex)
{
	if (!isWeaponIndex(weaponIndex))
		return(false);

	for (long i=0;i<20;i++)
	{
		if (AreaEffectWeapons[i] == inventory[weaponIndex].masterID)
			return true;
	}
			
	// If no ammotype, then it has unlimited ammo (energy weapons, for example).
	return(false);
}

float Mover::getWeaponAmmoLevel (long weaponIndex) {

	if (!isWeaponIndex(weaponIndex))
		return(-1.0);

	return(static_cast<float>((ammoTypeTotal[inventory[weaponIndex].ammoIndex].curAmount)) / static_cast<float>((ammoTypeTotal[inventory[weaponIndex].ammoIndex].maxAmount)));
}

bool Mover::getWeaponIsEnergy( long weaponIndex )
{
	return MasterComponent::masterList[inventory[weaponIndex].masterID].getForm() == COMPONENT_FORM_WEAPON_ENERGY;

}

void Mover::calcWeaponEffectiveness (bool setMax) {

	long effectiveness = 0;
	float avgSkill;

	lastWeaponEffectivenessCalc = scenarioTime;
		
	if (pilot)
		avgSkill = static_cast<float>(pilot->getSkill(MWS_GUNNERY)) / 50.0;
	else
		avgSkill = 1.0;

	//Ammo is NOT part of weapon effectiveness anymore
	// Per Mike Lee
	// -fs
	for (long curWeapon = numOther; curWeapon < (numOther + numWeapons); curWeapon++) {
		if (setMax || (!inventory[curWeapon].disabled ))
			effectiveness += inventory[curWeapon].effectiveness * avgSkill;
	}

	if (setMax)
		maxWeaponEffectiveness = effectiveness;
	else 
	{
		weaponEffectiveness = effectiveness;

		if (weaponEffectiveness == 0)
			playMessage(RADIO_WEAPONS_OUT);
	}
}

void Mover::calcAmmoTotals (void) 
{
	AmmoTally totalList[100];
	numAmmoTypes = 0;
	if (numWeapons > 0) 
	{
		for (long i = numOther; i < (numOther + numWeapons); i++) 
		{
			long weaponMasterId = inventory[i].masterID;
			bool newAmmoType = true;
			for (long curAmmo = 0; curAmmo < static_cast<long>(numAmmoTypes); curAmmo++)
			{
				if (totalList[curAmmo].masterId == static_cast<long>(MasterComponent::masterList[weaponMasterId].getWeaponAmmoMasterId()))
				{
					newAmmoType = false;
					break;
				}
			}
			
			if (newAmmoType) 
			{
				totalList[numAmmoTypes].masterId = MasterComponent::masterList[weaponMasterId].getWeaponAmmoMasterId();
				if (MasterComponent::masterList[weaponMasterId].getWeaponAmmoType()) 
				{
					totalList[numAmmoTypes].curAmount = 0;
					totalList[numAmmoTypes].maxAmount = 0;
				}
				else 
				{
					totalList[numAmmoTypes].curAmount = UNLIMITED_SHOTS;
					totalList[numAmmoTypes].maxAmount = UNLIMITED_SHOTS;
				}
				numAmmoTypes++;
			}
		}
		
		for (int i = numOther + numWeapons; i < (numOther + numWeapons + numAmmos); i++) 
		{
			long ammoMasterId = inventory[i].masterID;
			bool foundAmmoWeapon = false;
			for (long curAmmo = 0; curAmmo < numAmmoTypes; curAmmo++)
			{
				if (totalList[curAmmo].masterId == ammoMasterId) 
				{
					foundAmmoWeapon = true;
					if (useUnlimitedAmmo)
					{
						totalList[curAmmo].curAmount = UNLIMITED_SHOTS;
						totalList[curAmmo].maxAmount = UNLIMITED_SHOTS;
						break;
					}
					else
					{
						totalList[curAmmo].curAmount += inventory[i].amount;
						totalList[curAmmo].maxAmount += inventory[i].amount;
						break;
					}
				}
			}

			if (!foundAmmoWeapon)
				STOP(("Mech %s has ammo for a weapon it is not carrying!  Ammo Index is %d",name,ammoMasterId));
		}

		memcpy(ammoTypeTotal, totalList, sizeof(AmmoTally) * numAmmoTypes);
	}
}

long Mover::calcFireRanges (void) {

	lastOptimalRangeCalc = scenarioTime;

	maxRange = 0;
	minRange = 1000000.0;
	numFunctionalWeapons = 0;
	for (long curWeapon = numOther; curWeapon < (numOther + numWeapons); curWeapon++) {
		if (!inventory[curWeapon].disabled && (getWeaponShots(curWeapon) > 0)) {
			long range = MasterComponent::masterList[inventory[curWeapon].masterID].getWeaponRange();
			float minWeaponRange = WeaponRanges[range][0];
			float maxWeaponRange = WeaponRanges[range][1];
			if (maxWeaponRange > maxRange)
				maxRange = maxWeaponRange;
			if (minWeaponRange < minRange)
				minRange = minWeaponRange;
			numFunctionalWeapons++;
		}
	}

	float rangeTotals[NUM_WEAPON_RANGE_TYPES] = {0, 0, 0, 0, 0};
	mc2::Scored ranked[NUM_WEAPON_RANGE_TYPES];
	for (int curWeapon = numOther; curWeapon < (numOther + numWeapons); curWeapon++) {
		MasterComponent* weapon = &MasterComponent::masterList[inventory[curWeapon].masterID];
		if (!inventory[curWeapon].disabled && (getWeaponShots(curWeapon) > 0)) {
			float damageTimeRating = weapon->getWeaponDamage() / weapon->getWeaponRecycleTime();
			for (long i = 0; i < 5; i++)
				if (OptimalRangePointInRange[i][weapon->getWeaponRange()])
					rangeTotals[i] += damageTimeRating;
		}
	}
	float lastOptimalRange = optimalRange;
	if (numFunctionalWeapons == 0) {
		optimalRange = 0.0;
		return(optimalRange != lastOptimalRange);
		}
	else {
		for (long i = 0; i < NUM_WEAPON_RANGE_TYPES; i++) {
			ranked[i].id = static_cast<std::int32_t>(i);
			ranked[i].score = rangeTotals[i];
		}
		mc2::sort_scored(ranked, mc2::SortOrder::Descending);
		if (ranked[0].score <= 0.0) {
			optimalRange = 0.0;
			return(optimalRange != lastOptimalRange);
		}
	}

	long optimalRangeType = ranked[0].id;
	if (ranked[0].score == ranked[1].score) {
		// A tie, so pick one based upon the pre-set range priorities. The tie set is read whole, so the order inside it does not matter.
		unsigned char tieFlags = 0;
		for (long i = 0; i < 5; i++)
			if (ranked[i].score == ranked[0].score)
				tieFlags |= (1 << ranked[i].id);

		optimalRangeType = OptimalRangeTieTable[tieFlags];
		if (optimalRangeType == 255) {
			optimalRange = 0.0;
			return(optimalRange != lastOptimalRange);
		}

	}
	optimalRange = OptimalRangePoints[optimalRangeType];
	
	return(optimalRange != lastOptimalRange);
}

float Mover::getOrderedFireRange (long* attackRange) {

	long fireRange = pilot->getCurTacOrder()->attackParams.range;
	if (attackRange)
		fireRange = *attackRange;
	float orderedFireRange = -1.0;
	switch (fireRange) {
		case FIRERANGE_DEFAULT:
			orderedFireRange = getOptimalFireRange();
			break;
		case FIRERANGE_RAMMING:
			orderedFireRange = 0.0;
			break;
		case FIRERANGE_LONGEST:
			orderedFireRange = getMaxFireRange();
			break;
		case FIRERANGE_OPTIMAL:
			orderedFireRange = getOptimalFireRange();
			break;
		case FIRERANGE_SHORT:
			orderedFireRange = (WeaponRanges[WEAPON_RANGE_SHORT][1] - WeaponRanges[WEAPON_RANGE_SHORT][0])/2.0;
			break;
		case FIRERANGE_MEDIUM:
			orderedFireRange = (WeaponRanges[WEAPON_RANGE_MEDIUM][1] - WeaponRanges[WEAPON_RANGE_MEDIUM][0])/2.0;
			break;
		case FIRERANGE_LONG:
			orderedFireRange = (WeaponRanges[WEAPON_RANGE_LONG][1] - WeaponRanges[WEAPON_RANGE_LONG][0])/2.0;
			break;
	}
	return(orderedFireRange);
}

float Mover::getMinFireRange (void) {

	return(minRange);
}

float Mover::getMaxFireRange (void) {

	return(maxRange);
}

float Mover::getOptimalFireRange (void) {

	return(optimalRange);
}


bool Mover::isWeaponIndex (long itemIndex) {

	return((itemIndex >= numOther) && (itemIndex < (numOther + numWeapons)));
}

bool Mover::isWeaponMissile (long weaponIndex) {

	return(MasterComponent::masterList[inventory[weaponIndex].masterID].getForm() == COMPONENT_FORM_WEAPON_MISSILE);
}

bool Mover::isWeaponReady (long weaponIndex) {

	if (inventory[weaponIndex].disabled)
		return(false);
	else if (inventory[weaponIndex].readyTime > scenarioTime)
		return(false);
	return(true);
}

void Mover::startWeaponRecycle (long weaponIndex) {

	inventory[weaponIndex].readyTime = scenarioTime +
									   MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponRecycleTime();
}

long Mover::tallyAmmo (long ammoMasterId) {

	long tally = 0;
	long firstAmmo = numOther + numWeapons;
	for (long item = firstAmmo; item < firstAmmo + numAmmos; item++) {
		if (inventory[item].masterID == ammoMasterId)
			tally += inventory[item].amount;
	}

	return(tally);
}

bool Mover::needsRefit(void) {

	bool result = false;

	if (!refitBuddyWID && getObjectClass() == BATTLEMECH)
	{
		for (long loc=0; loc<numArmorLocations; loc++)
		{
			if (loc<numBodyLocations && (loc == MECH_ARMOR_LOCATION_LARM || loc == MECH_ARMOR_LOCATION_RARM) &&
					body[loc].damageState == IS_DAMAGE_DESTROYED)
				continue;
			if ((loc<numBodyLocations && body[loc].curInternalStructure < body[loc].maxInternalStructure) ||
				(armor[loc].curArmor < armor[loc].maxArmor))
			{
				result = true;
				break;
			}
		}
		if (!result)
			for (long ammo = 0; ammo < numAmmoTypes; ammo++)
				if (ammoTypeTotal[ammo].curAmount < ammoTypeTotal[ammo].maxAmount)
				{
					result = true;
					break;
				}
	}
	return result;
}

long Mover::reduceAmmo (long ammoMasterId, long amount) {

	long amountUsed = amount;

	long firstAmmo = numOther + numWeapons;
	for (long item = firstAmmo; item < firstAmmo + numAmmos; item++) {
		if (inventory[item].masterID == ammoMasterId) {
			if (inventory[item].amount > amount) {
				inventory[item].amount -= amount;
				break;
				}
			else {
				amount -= inventory[item].amount;
				inventory[item].amount = 0;
			}
        }
	}

	for (long ammo = 0; ammo < numAmmoTypes; ammo++) {
		if (ammoTypeTotal[ammo].masterId == ammoMasterId) {
			ammoTypeTotal[ammo].curAmount -= amountUsed;
			if (ammoTypeTotal[ammo].curAmount <= 0) {
				// Ammo for this weapon type is spent, which may effect
				// our weapon effectiveness and optimal range...
				ammoTypeTotal[ammo].curAmount = 0;
				calcWeaponEffectiveness(false);
				calcFireRanges();
				pilot->radioMessage(RADIO_AMMO_OUT);
			}
			break;
		}
    }

	return(amountUsed);
}

/*
long Mover::refillAmmo (long ammoMasterId, long amount) {

	long amountUsed = amount;

	// Deduct it from the first available ammo critical space...
	long firstAmmo = numOther + numWeapons;
	for (long item = firstAmmo; item < firstAmmo + numAmmos; item++) {
		if (inventory[item].masterID == ammoMasterId)
			if (inventory[item].amount > amount) {
				inventory[item].amount -= amount;
				break;
				}
			else {
				amount -= inventory[item].amount;
				inventory[item].amount = 0;
			}
	}

	// Make sure the ammoTypeTotal list, in which we track total ammo for
	// all of the ammo types this mover uses, is adjusted...
	for (long ammo = 0; ammo < numAmmoTypes; ammo++)
		if (ammoTypeTotal[ammo].masterId == ammoMasterId) {
			ammoTypeTotal[ammo].curAmount -= amountUsed;
			if (ammoTypeTotal[ammo].curAmount <= 0) {
				// Ammo for this weapon type is spent, which may effect
				// our weapon effectiveness and optimal range...
				ammoTypeTotal[ammo].curAmount = 0;
				calcLongestRangeWeapon();
				calcWeaponEffectiveness(false);
				calcOptimalRange(NULL);
			}
			break;
		}

	return(amountUsed);
}
*/

void Mover::deductWeaponShot (long weaponIndex, long ammoAmount) {

	if (ammoAmount > 0)
		reduceAmmo(MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponAmmoMasterId(), ammoAmount);
}

// The caller's value list holds whole numbers. A NaN or infinite score has no whole-number value (the plain cast is undefined), so it reads as 0.
static long scoreToValue (float score) {
	return std::isfinite(score) ? static_cast<long>(score) : 0L;
}

long Mover::sortWeapons (int* weaponList, long* valueList, long listSize, long sortType, bool skillCheck) {

	GameObjectPtr target = pilot->getCurrentTarget();
	if (!target)
		return(-2);

	long aimLocation = -1;
	if (pilot && pilot->getCurTacOrder()->isCombatOrder())
		aimLocation = pilot->getCurTacOrder()->attackParams.aimLocation;
	// Highest value first; weapons of equal value keep the order they were listed in (mc2::sort_scored). A mover carries at most MAX_WEAPONS_PER_MOVER weapons,
	// and the caller's lists hold no more than that; a larger count is cut to it.
	mc2::Scored scored[MAX_WEAPONS_PER_MOVER];
	const long requested = (listSize == -1) ? static_cast<long>(numWeapons) : listSize;
	const long count = std::clamp<long>(requested, 0, MAX_WEAPONS_PER_MOVER);
	if (requested < 0)
		MC2_LOG_ONCE(Sim, Warn, "sortWeapons: negative weapon count {}, nothing is sorted", requested);
	else if (count < requested)
		MC2_LOG_ONCE(Sim, Warn, "sortWeapons: {} weapons requested, only the first {} are sorted", requested, MAX_WEAPONS_PER_MOVER);
	if (listSize == -1) {
		for (long item = numOther; item < (numOther + count); item++) {
			scored[item - numOther].id = static_cast<std::int32_t>(item);
			switch (sortType) {
				case WEAPONSORT_ATTACKCHANCE:
					scored[item - numOther].score = calcAttackChance(target, aimLocation, scenarioTime, item, 0.0, nullptr);
					break;
				default:
					return(-3);
			}
		}
		mc2::sort_scored(std::span<mc2::Scored>(scored, count), mc2::SortOrder::Descending);
		for (int item = 0; item < count; item++) {
			weaponList[item] = scored[item].id;
			valueList[item] = scoreToValue(scored[item].score);
		}
		}
	else {
		for (long item = 0; item < count; item++) {
			scored[item].id = weaponList[item];
			float sortValue = 0.0;
			if (weaponList[item] == -1)
				sortValue = -999.0;
			else
				switch (sortType) {
					case WEAPONSORT_ATTACKCHANCE:
						sortValue = calcAttackChance(target, aimLocation, scenarioTime, weaponList[item], 0.0, nullptr);
						break;
					default:
						return(-3);
				}
			scored[item].score = sortValue;
		}
		mc2::sort_scored(std::span<mc2::Scored>(scored, count), mc2::SortOrder::Descending);
		for (int item = 0; item < count; item++) {
			weaponList[item] = scored[item].id;
			valueList[item] = scoreToValue(scored[item].score);
		}
	}

	return(NO_ERR);
}

bool Mover::hasNonAreaWeapon (void)
{
	for (long i=numOther;i<numOther+numWeapons;i++)
	{
		if (!getWeaponAreaEffect(i))
			return true;
	}
	
	return false;
}
