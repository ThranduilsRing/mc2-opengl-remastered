// BattleMech: BattleMechType: loadGameSystem, init, collision and destruction handling, createInstance.
// Declared in the class headers; shared names in mech_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "mech_internal.h"
#include "world/visibility_input.h"
#include "mc2fmt.h"



long BattleMech::loadGameSystem (FitIniFilePtr mechFile) {

	long result = 0;

	result = mechFile->seekBlock("Mech:Class");
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("MaxLightMech",MechClassWeights[std::to_underlying(MECH_CLASS_LIGHT)]);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("MaxHeavyMech",MechClassWeights[std::to_underlying(MECH_CLASS_MEDIUM)]);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("MaxHeavyMech",MechClassWeights[std::to_underlying(MECH_CLASS_HEAVY)]);
	if (result != NO_ERR)
		return(result);

	result = mechFile->seekBlock("Mech:Movement");
	if (result != NO_ERR)
		return(result);

	long jumpCost = 0;
	result = mechFile->readIdLong("JumpCost", jumpCost);
	if (result == NO_ERR)
		DefaultMechJumpCost = jumpCost;

	long crashSize = 0;
	result = mechFile->readIdLong("CrashAvoidSelf", crashSize);
	if (result == NO_ERR)
		DefaultMechCrashAvoidSelf = crashSize;

	result = mechFile->readIdLong("CrashAvoidPath", crashSize);
	if (result == NO_ERR)
		DefaultMechCrashAvoidPath = crashSize;

	result = mechFile->readIdLong("CrashBlockSelf", crashSize);
	if (result == NO_ERR)
		DefaultMechCrashBlockSelf = crashSize;

	result = mechFile->readIdLong("CrashBlockPath", crashSize);
	if (result == NO_ERR)
		DefaultMechCrashBlockPath = crashSize;

	float crashYield = 0.0;
	result = mechFile->readIdFloat("CrashYieldTime", crashYield);
	if (result == NO_ERR)
		DefaultMechCrashYieldTime = crashYield;

	result = mechFile->readIdLongArray("PilotCheckConditions", MechPilotCheckConditions, NUM_MECH_PILOT_CHECK_CONDITIONS);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdLongArray("PilotCheckTerrainEffect", MechPilotCheckTerrainEffect, 64);
	if (result != NO_ERR)
		return(result);

	result = mechFile->seekBlock("Mech:FireWeapon");
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdLongArray("AttackerMoveModifier", AttackerMoveModifier, NUM_MECH_STATES);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdCharArray("HitLocationTable", MechHitLocationTable, NUM_MECH_HIT_SECTIONS * NUM_MECH_HIT_ARCS * NUM_MECH_ARMOR_LOCATIONS);
	if (result != NO_ERR)
		return(result);

	long longData[5 * 2];
	result = mechFile->readIdLongArray("TargetMoveModifierTable", longData, 5 * 2);
	if (result != NO_ERR)
		return(result);

	for (long i = 0; i < 5; i++)
	{
		TargetMoveModifierTable[i][0] = longData[2 * i];
		TargetMoveModifierTable[i][1] = longData[2 * i + 1];
	}

	result = mechFile->seekBlock("Mech:Damage");
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdCharArray("CriticalHitTable", CriticalHitTable, 4);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdCharArray("MechTransferHitTable", MechTransferHitTable, NUM_BODY_LOCATIONS);
	if (result != NO_ERR)
		return(result);
		
	result = mechFile->readIdLong("MechSalvageChance", MechSalvageChance);
	if (result != NO_ERR)
		return(result);

	result = mechFile->seekBlock("Mech:Collision");
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("collisionThreshold", mechCollisionThreshold);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("objectThreshold", objectCollisionThreshold);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("tonnageThreshold", tonnageCollisionThreshold);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("treeDeflection", treeDeflection);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("pivotAngle", mechPivotAngle);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("pivotThrottle", mechPivotThrottle);
	if (result != NO_ERR)
		return(result);

	return(NO_ERR);
}

void BattleMechType::init (void) {

	objectTypeClass = BATTLEMECH_TYPE;
	objectClass = BATTLEMECH;

	anim = nullptr;
			
	rightArmDebrisId = -1;
	leftArmDebrisId = -1;
	destroyedDebrisId = -1;
	crashAvoidSelf = DefaultMechCrashAvoidSelf;
	crashAvoidPath = DefaultMechCrashAvoidPath;
	crashBlockSelf = DefaultMechCrashBlockSelf;
	crashBlockPath = DefaultMechCrashBlockPath;
	crashYieldTime = DefaultMechCrashYieldTime;
	moveType = MOVETYPE_GROUND;
			
	explDmg = 0.0;
	explRad = 0.0;
	LOSFactor = 1.0f;
	destructDamage = 0.0f;
}

long BattleMechType::init (FilePtr objFile, unsigned long fileSize) 
{
	long result = 0;

	FitIniFile mechFile;
	result = mechFile.open(objFile,fileSize);
	if (result != NO_ERR)
		return(result);

	result = mechFile.seekBlock("MechProfile");
	if (result != NO_ERR)
		return(result);

	char fileType[512];
	result = mechFile.readIdString ("ProfileName", fileType, 511);
	if (result != NO_ERR)
		return(result);

	result = mechFile.readIdLong("MoveType", moveType);
	if (result != NO_ERR)
		moveType = MOVETYPE_GROUND;

	result = mechFile.readIdFloat("LOSFactor",LOSFactor);
	if (result != NO_ERR)
		LOSFactor = 1.0f;
		
	result = mechFile.readIdFloat("DestructDamage",destructDamage);
	if (result != NO_ERR)
		destructDamage = 20.0f;
		
	result = mechFile.readIdFloat("ExplosionDamage",explDmg);
	if (result != NO_ERR)
		explDmg = 10.0f;
		
	result = mechFile.readIdFloat("ExplosionRadius",explRad);
	if (result != NO_ERR)
		explRad = 25.0f;
		
	FullPathFileName mechCSVName;
	mechCSVName.init(objectPath,fileType,".csv");
	
	CSVFile mechCSVFile;
	result = mechCSVFile.open(mechCSVName);
	if (result != NO_ERR)
		return result;


	// Now, read in the max internal structure for each body location...
	// Reader reads rows from 1 just like spreadsheet.
	// Row 4, Cols 2,4,6,8,10,12,14,16
	for (long curLocation = 0; curLocation < NUM_BODY_LOCATIONS; curLocation++) 
	{
		result = mechCSVFile.readUChar(17,stupidNewSageColumns[curLocation],maxInternalStructure[curLocation]);
		if (result != NO_ERR)
			return(result);
	}

	rightArmDebrisId = leftArmDebrisId = destroyedDebrisId = -1;

	dynamics.setType(DYNAMICS_MECH);
	dynamics.init(&mechCSVFile);
	
	// LOCOMOTION-FACET-1 (slice 4): a mech's data-owned locomotion facet defaults
	// to the mech-walker model (was the BattleMech::locomotionModel() class
	// override). Set BEFORE the .fit read so an optional [UnitProfile] Locomotion=
	// token can override it; stock .fits omit the key -> walker preserved (parity
	// with slice 3). This is the last class-touch: a data default keyed by type,
	// which a future full UnitDefinition load will supply directly.
	profile().locomotion = LOCO_MECH_WALKER;

	// DAMAGE-SCHEMA-FACET-1 (slice 5): a mech's data-owned hit-location model
	// defaults to the 8-body-part schema. The vehicle/base default is
	// DMG_VEHICLE_FACINGS (UnitProfile's field initializer), so only the mech
	// type overrides it here. Set BEFORE the .fit read so an optional
	// [UnitProfile] DamageSchema= token can override; stock .fits omit it ->
	// mech-8part preserved (no behavior change — nobody reads the facet yet).
	profile().damageSchema = DMG_MECH_8PART;

	// UNIT-SOURCE-FLIP-SWITCH-1: when MC2_UNIT_DEFINITION_SOURCE=generated, SOURCE
	// the chassis-level green facets (locomotion + damageSchema) from the generated
	// UnitDefinition sidecar (data/defs/units/core/<stem>/chassisdef.fit) instead of
	// the legacy class defaults just set above. Gate=legacy (default) or a missing
	// generated def -> no-op, the legacy defaults stand (byte-identical). fileType
	// is the mech ProfileName == the converter's chassis stem. Runs BEFORE the .fit
	// read so a stock-declared [UnitProfile] block still overrides last.
	sourceGeneratedChassisFacets(fileType, profile(), mechFile.getFilename());

	// UNIT-PROFILE-SEAM-1: optional .fit [UnitProfile] data read (additive).
	populateProfileFromFit(mechFile, profile());

	// UNITPROFILE-VALIDATOR-1 (slice 6): report-only construction-rules check,
	// gated behind MC2_UNIT_PROFILE_DATA. Runs AFTER facets are set. A stock jet
	// mech (walker + CAP_JUMP) is coherent -> logs nothing. Never rejects.
	validateAndReportUnitProfile(profile(), mechFile.getFilename());

	result = ObjectType::init(&mechFile);
	return(result);
}

// UNITPROFILE-TYPELOAD-SOURCE-1: single owner of the instance-loadout-derived
// capability write. Previously this was open-coded (and drifted into dead code)
// in every mech load overload. There are TWO legitimate capability sources and
// this method owns exactly ONE of them:
//   (a) TYPE-LOAD declared  -> .fit [UnitProfile] block via populateProfileFromFit
//       (already single-owner; runs once at type load, grant-only, additive).
//   (b) INSTANCE-loadout-derived -> CAP_JUMP from numJumpJets, which is only
//       final AFTER the loadout scan. That is THIS method.
// Grant-only: we OR the jump fact in when jets are installed; we never clear a
// capability that (a) may have granted. Then we refresh this instance's runtime
// state from the (now-updated) shared type baseline. Call from the end of every
// real load path where numJumpJets is authoritative.
void BattleMech::refreshDerivedCapabilities (void) {
	if (!getObjectType())
		return;
	CapabilitySet& baseline = getObjectType()->profile().baselineCapabilities;
	// UNIT-SOURCE-FLIP-SWITCH-1: instance-level CAP_JUMP source selection. Under
	// MC2_UNIT_DEFINITION_SOURCE=generated, source the jump bit from the spawned
	// variant's generated unit.fit (keyed by chassis stem + VariantName). The
	// shadow-compare proved generated jump == the legacy numJumpJets derivation, so
	// this is behavior-identical. Gate=legacy (default) or a missing generated def
	// -> sourceGeneratedVariantJump returns false and we keep the legacy path.
	// Grant-only: we only OR CAP_JUMP in (never clear (a)'s type-load grants).
	bool genJump = false;
	const char* chassisStem = getObjectType()->getAppearanceTypeName();
	if (sourceGeneratedVariantJump(chassisStem, variantName, &genJump)) {
		if (genJump)
			baseline.set(CAP_JUMP, true);
	} else if (numJumpJets > 0)
		baseline.set(CAP_JUMP, true);      // legacy grant-only; never clears (a)'s grants
	runtime.currentCapabilities = baseline;
}

void BattleMechType::destroy (void) 
{
	ObjectType::destroy();
}

bool BattleMechType::handleCollision (GameObjectPtr collidee, GameObjectPtr collider) {

	// The default reaction of any object in the world
	// is to simply explode.  This just returns true
	// to facilitate this behaviour.
	// Don't blow us up unless we hit another mech.

	if (MPlayer && !MPlayer->isServer())
		return(false);

	bool sameTeam = false;
	bool collide = true;
	bool collideeJumping = (static_cast<BattleMechPtr>(collidee))->isJumping();
	bool colliderJumping = false;
	BattleMechPtr mech = static_cast<BattleMechPtr>(collidee);
	switch (collider->getObjectClass())
	{
		case TRAINCAR:
		{
			if ((collidee->getCollisionFreeFromWID() != collider->getWatchID()) || (collidee->getCollisionFreeTime() < scenarioTime))
				collide = true;
			else
				collide = false;

			if (collide)
			{
				// Don't allow collision between these two again for a period of time...
				collidee->setCollisionFreeFromWID(collider->getWatchID());
				collidee->setCollisionFreeTime(scenarioTime + 2.0);

				/*
				frame_of_ref workFrame = collidee->getFrame();
				float turnAround = 90.0;
				workFrame.rotate_about_k(turnAround);
				collidee->setFrame(workFrame);
				*/
				collidee->rotate(90.0);
				
				// Check to be sure at least one is moving.  If not, move them apart!!!
				// They must be moved at least extentRadius1 + extentRadius2 apart!!
/*
				vector_3d collideeVel = collidee->getVelocity();
				float collideeSpeed = collideeVel.magnitude();
				if (collideeSpeed <= mechCollisionThreshold)
				{
					float extentRadius1 = collider->getObjectType()->getExtentRadius();
					extentRadius1 += extentRadius;

					// Find vector to move collidee
					vector_3d cPosition = collidee->getPosition();
					vector_3d direction = cPosition - collider->getPosition();
					direction.normalize();
					direction *= extentRadius1;
					cPosition += direction;
					collidee->setPosition(cPosition);
				}
*/
				(static_cast<MoverPtr>(collidee))->bounceToAdjCell();

				// Train will take care of administering damage from this collision...
				soundSystem->playDigitalSample(MECH_COLLIDE,collidee->getPosition());
				return(false);
			}
		}
		break;

	case BATTLEMECH:
			if (collidee->getTeam() != collider->getTeam())
			{
				if ((static_cast<MoverPtr>(collider))->getPilot()->getCurTacOrder()->code == TACTICAL_ORDER_ATTACK_OBJECT)
					(static_cast<MoverPtr>(collider))->getPilot()->numPhysicalAttacks[PHYSICAL_ATTACK_RAM][COMBAT_STAT_MISSION]++;
				else if ((static_cast<MoverPtr>(collider))->getPilot()->getCurTacOrder()->code == TACTICAL_ORDER_JUMPTO_POINT)
				{
					if (mech->getPilot()->getCurTacOrder()->getJumpTarget() == collidee)
						mech->getPilot()->numPhysicalAttacks[PHYSICAL_ATTACK_DFA][COMBAT_STAT_MISSION]++;
				}
			}

			colliderJumping = (static_cast<BattleMechPtr>(collider))->isJumping();
			if (!colliderJumping && ((static_cast<BattleMechPtr>(collider))->lastJumpTime >= 0.0)) {
				float timeSinceLanding = scenarioTime - (static_cast<BattleMechPtr>(collider))->lastJumpTime;
				colliderJumping = (timeSinceLanding < 0.5);
			}
		case GROUNDVEHICLE:
		{
			if (!collideeJumping && ((static_cast<BattleMechPtr>(collidee))->lastJumpTime >= 0.0)) {
				float timeSinceLanding = scenarioTime - (static_cast<BattleMechPtr>(collidee))->lastJumpTime;
				(static_cast<BattleMechPtr>(collidee))->lastJumpTime = -1.0;
				collideeJumping = (timeSinceLanding < 0.5);
			}
			bool jumpCollision = (collideeJumping || colliderJumping);

			// Typically, same-alignment movers don't collide unless a DFA or RAM was
			// involved...
			if (collidee->isFriendly(collider))
				sameTeam = true;
			if (sameTeam)
				collide = jumpCollision;
			else if (jumpCollision)
				collide = true;
			else {
				GameObjectPtr collideeRamTarget = (static_cast<MoverPtr>(collidee))->getPilot()->getCurTacOrder()->getRamTarget();
				GameObjectPtr colliderRamTarget = (static_cast<MoverPtr>(collider))->getPilot()->getCurTacOrder()->getRamTarget();
				collide = ((collideeRamTarget == collider) || (colliderRamTarget == collidee));
			}
			if (collide)
				collide = ((collidee->getCollisionFreeFromWID() != collider->getWatchID()) || (collidee->getCollisionFreeTime() < scenarioTime));

			if (collide) {
				// Don't allow collision between these two again for a period of time...
				collidee->setCollisionFreeFromWID(collider->getWatchID());
				collidee->setCollisionFreeTime(scenarioTime + 2.0);

				collidee->rotate(90.0);

				// Check to be sure at least one is moving.  If not, move them apart!!!
				// They must be moved at least extentRadius1 + extentRadius2 apart!!
/*
				vector_3d collideeVel = collidee->getVelocity();
				float collideeSpeed = collideeVel.magnitude();
				if (collideeSpeed <= mechCollisionThreshold)
				{
					float extentRadius1 = collider->getObjectType()->getExtentRadius();
					extentRadius1 += extentRadius;

					// Find vector to move collidee
					vector_3d cPosition = collidee->getPosition();
					vector_3d direction = cPosition - collider->getPosition();
					direction.normalize();
					direction *= extentRadius1;
					cPosition += direction;
					collidee->setPosition(cPosition);
				}
*/
				if (jumpCollision) 
				{
					// Since we reset the jumping mech's lastJumpTime right here,
					// we should apply the damage to the collider here, as well.
					// If both are jumping...well then, double damage is taken!
					if (collideeJumping && !sameTeam) 
					{
						WeaponShotInfo shotInfo;
						long hitLocation = collidee->calcHitLocation(collider, -1, ATTACKSOURCE_MINE, 0);
						if (sameTeam)
							shotInfo.init(collider->getWatchID(), -1, collider->getTonnage() / 100.0 + 0.5, hitLocation, collidee->relFacingTo(collider->getPosition()));
						else
							shotInfo.init(collider->getWatchID(), -1, collider->getTonnage() / 10.0 + 0.5, hitLocation, collidee->relFacingTo(collider->getPosition()));
						collidee->handleWeaponHit(&shotInfo, (MPlayer != nullptr));
						hitLocation = collider->calcHitLocation(collidee, -1, ATTACKSOURCE_DFA, 0);
						if (sameTeam)
							shotInfo.init(collidee->getWatchID(), -1, collider->getTonnage() / 100.0 + 0.5, hitLocation, collider->relFacingTo(collidee->getPosition()));
						else
							shotInfo.init(collidee->getWatchID(), -1, collider->getTonnage() / 10.0 + 0.5, hitLocation, collider->relFacingTo(collidee->getPosition()));
						collider->handleWeaponHit(&shotInfo, (MPlayer != nullptr));
						glm::vec3 tmpPosition = collider->getPosition();
						ObjectManager->createExplosion(DFA_BOOM_EFFECT, nullptr, tmpPosition);
					}
				}
				else 
				{
					if (!sameTeam)
					{
						long hitLocation = collidee->calcHitLocation(collider, -1, ATTACKSOURCE_COLLISION, 0);
						WeaponShotInfo shotInfo;
						shotInfo.init(collider->getWatchID(), -1, collider->getTonnage() / 10.0 + 0.5, hitLocation, collidee->relFacingTo(collider->getPosition()));
						collidee->handleWeaponHit(&shotInfo, (MPlayer != nullptr));
						collidee->handleWeaponHit(&shotInfo, (MPlayer != nullptr));
					}
				}

				long bounceDir = (static_cast<MoverPtr>(collidee))->bounceToAdjCell();
				if (jumpCollision && (bounceDir == -1)) {
				}

				if (!sameTeam && (mc2::world::visibleThisTurn(collidee)))
					soundSystem->playDigitalSample(MECH_COLLIDE,collidee->getPosition());

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

		case TREE:
		case TERRAINOBJECT:
		{
			if ((collidee->getCollisionFreeFromWID() != collider->getWatchID()) || (collidee->getCollisionFreeTime() < scenarioTime))
			{
				// Don't allow collision between these two again for a period of time...
				collidee->setCollisionFreeFromWID(collider->getWatchID());
				collidee->setCollisionFreeTime(scenarioTime + 2.0);

				float turnAround = 0.0;
				float tonnageClass = collidee->getTonnage();
				if (tonnageClass < tonnageCollisionThreshold)
					turnAround = (tonnageCollisionThreshold / tonnageClass) * treeDeflection;
				if (turnAround > 0.0)
					collidee->rotate(turnAround);

				if (mc2::world::visibleThisTurn(collidee))
					soundSystem->playDigitalSample(MECH_COLLIDE,collidee->getPosition());
				return(false);
			}
		}
		break;
	}

	return(false);
}

bool BattleMechType::handleDestruction (GameObjectPtr collidee, GameObjectPtr collider)
{
	// BUG ALERT!!!!!!!!!
	// This could get called multiple times for a mech before it's
	// truly destroyed (if multiple weapon hits are taken in an update).
	// This should be prevented by checking if the mech is already
	// destroyed. BUT, for fear that this could break something this late
	// in the game (4/4/98), it would be best to address the symptoms
	// for now (e.g. # of kills in handleTargetKilled). After release
	// (or demo), fix this and seriously test it doesn't "break"
	// anything!!!!!!!!!!! Should also be fixed for other objects'
	// handleDestruction().

	BattleMechPtr mech = static_cast<BattleMechPtr>(collidee);

	if (!mech->getPilot())
		Fatal(0, " No Pilot in this mech! ");

	if (mech->getPoint() == mech) {
		mech->getGroup()->setPoint(nullptr);
		// If there is no new point, all units must be blown away.
		// How do we want to handle this?
	}

	if (mech->sensorSystem)
		mech->sensorSystem->disable();
	
	if (mech->withdrawing) {
		mech->timeLeft = 0.8f;
		mech->getPilot()->handleAlarm(PILOT_ALARM_VEHICLE_WITHDRAWN, 0);
	}
	else {
		if (!mech->startDisabled)
			mech->timeLeft = 0.8f;
		mech->getPilot()->handleAlarm(PILOT_ALARM_VEHICLE_DESTROYED, collider ? collider->getWatchID() : 0);

		//ONLY the server may DESTROY a mech.  All other clients must await the statusChunk!!
		if (!MPlayer || (MPlayer && MPlayer->isServer()))
			mech->setStatus(OBJECT_STATUS_DESTROYED);

		mech->fallen = false;
		mech->exploding = false;
		
		for (long i=0;i<mech->numBodyLocations;i++)		//Blow all of the components.  NO SALVAGE!!
			mech->destroyBodyLocation(i);

#ifdef USE_MOODMUSIC
		if (collidee->getAlignment() == Team::home->getAlignment())
			friendlyDestroyed = true;
		else
			enemyDestroyed = true;
#endif
		if (CombatLog) {
			char s[1024];
			mc2::fmt::to_buf(s, "[{:.2f}] mech.destroyed HD: [{:05d}]{}", scenarioTime, mc2::fmt::as_i32(mech->getPartId()), mc2::fmt::safe(mech->getName()));
			CombatLog->write(s);
			CombatLog->write(" ");
		}
	}

	return(true);
}

GameObjectPtr BattleMechType::createInstance (void) {

	BattleMechPtr newMech = new BattleMech;
	if (!newMech)
		return(nullptr);

	newMech->init(true, this);

	return newMech;
}


void BattleMech::operator = (BattleMech copy) {

}

void BattleMech::handleStaticCollision (void) {

	bool endOfJump = false;
	if (getTangible() || endOfJump) 
	{
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
