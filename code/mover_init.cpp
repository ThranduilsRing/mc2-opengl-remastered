// Mover: Mover::loadGameSystem, set, init and release.
// Declared in mover.h; shared names in mover_internal.h.
#include "mover_internal.h"



long Mover::loadGameSystem (FitIniFilePtr mechFile, float visualRange) {

	long result = 0;

	result = mechFile->seekBlock("Pathfinding");
	if (result != NO_ERR)
		return(result);


	result = mechFile->readIdLong("SimplePathTileRange", SimpleMovePathRange);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("DelayedOrderTime", DelayedOrderTime);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("MoveTimeOut", MoveTimeOut);
	if (result != NO_ERR)
		MoveTimeOut = 30.0;

	result = mechFile->readIdFloat("MoveYieldTime", MoveYieldTime);
	if (result != NO_ERR)
		MoveYieldTime = 1.5;

	result = mechFile->readIdLongArray("GroupMoveTrailLength", GroupMoveTrailLen, 2);
	if (result != NO_ERR) {
		GroupMoveTrailLen[0] = 0;
		GroupMoveTrailLen[1] = 1;
	}

	result = mechFile->readIdLongArray("GroupMoveTrailLength", GroupMoveTrailLen, 2);
	if (result != NO_ERR) {
		GroupMoveTrailLen[0] = 0;
		GroupMoveTrailLen[1] = 1;
	}

	result = mechFile->readIdFloat("GroupOrderGoalOffset", GroupOrderGoalOffset);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloatArray("MoveMarginOfError", Mover::marginOfError, 2);
	if (result != NO_ERR)
		return(result);


	result = mechFile->seekBlock("Mover:General");
	Assert(result == NO_ERR, 0, "Couldn't find Mover:General block in gamesys.fit");

	result = mechFile->readIdFloat("NewThreatMultiplier", newThreatMultiplier);
	if (result != NO_ERR)
		newThreatMultiplier = 3.0;

	result = mechFile->readIdFloat("BlockCaptureRange", GameObject::blockCaptureRange);
	Assert(result == NO_ERR, 0, "Couldn't find BlockCaptureRange in Mover:General block in gamesys.fit");
	
	result = mechFile->readIdFloat("RefitTime", Mover::refitTime);
	Assert(result == NO_ERR, 0, "Couldn't find RefitTime in Mover:General block in gamesys.fit");
	
	result = mechFile->readIdFloat("RefitRange", Mover::refitRange);
	Assert(result == NO_ERR, 0, "Couldn't find RefitRange in Mover:General block in gamesys.fit");
	
	result = mechFile->readIdFloat("RefitAmount", Mover::refitAmount);
	Assert(result == NO_ERR, 0, "Couldn't find RefitAmount in Mover:General block in gamesys.fit");

	result = mechFile->readIdFloat("RefitVehicleArmorCost", Mover::refitCostArray[ARMOR_REFIT_COST][REFIT_VEHICLE]);
	Assert(result == NO_ERR, 0, "Couldn't find RefitVehicleArmorCost in Mover:General block in gamesys.fit");

	result = mechFile->readIdFloat("RecoverTime", Mover::recoverTime);
	Assert(result == NO_ERR, 0, "Couldn't find RecoverTime in Mover:General block in gamesys.fit");
	
	result = mechFile->readIdFloat("RecoverRange", Mover::recoverRange);
	Assert(result == NO_ERR, 0, "Couldn't find RecoverRange in Mover:General block in gamesys.fit");

	result = mechFile->readIdFloat("RecoverAmount", Mover::recoverAmount);
	Assert(result == NO_ERR, 0, "Couldn't find RecoverAmount in Mover:General block in gamesys.fit");

	result = mechFile->readIdFloat("RecoverCost", Mover::recoverCost);
	Assert(result == NO_ERR, 0, "Couldn't find RecoverCost in Mover:General block in gamesys.fit");
	
	result = mechFile->readIdFloat("RefitVehicleInternalCost", Mover::refitCostArray[INTERNAL_REFIT_COST][REFIT_VEHICLE]);
	Assert(result == NO_ERR, 0, "Couldn't find RefitVehicleInternalCost in Mover:General block in gamesys.fit");
	
	result = mechFile->readIdFloat("RefitVehiclePointsToAmmo", Mover::refitCostArray[AMMO_REFIT_COST][REFIT_VEHICLE]);
	Assert(result == NO_ERR, 0, "Couldn't find RefitVehiclePointsToAmmo in Mover:General block in gamesys.fit");
	
	result = mechFile->readIdFloat("RefitBayArmorCost", Mover::refitCostArray[ARMOR_REFIT_COST][REFIT_BAY]);
	Assert(result == NO_ERR, 0, "Couldn't find RefitBayArmorCost in Mover:General block in gamesys.fit");
	
	result = mechFile->readIdFloat("RefitBayInternalCost", Mover::refitCostArray[INTERNAL_REFIT_COST][REFIT_BAY]);
	Assert(result == NO_ERR, 0, "Couldn't find RefitBayInternalCost in Mover:General block in gamesys.fit");
	
	result = mechFile->readIdFloat("RefitBayAmmoCost", Mover::refitCostArray[AMMO_REFIT_COST][REFIT_BAY]);
	Assert(result == NO_ERR, 0, "Couldn't find RefitBayAmmoCost in Mover:General block in gamesys.fit");
	
	result = mechFile->readIdLongArray("IndirectFireWeaponComponentIndex",Mover::IndirectFireWeapons,20);
	Assert(result == NO_ERR, 0, "Couldn't find IndirectFireWeaponComponentIndex in Mover:General block in gamesys.fit");
	
	result = mechFile->readIdLongArray("AreaEffectWeaponComponentIndex",Mover::AreaEffectWeapons,20);
	Assert(result == NO_ERR, 0, "Couldn't find AreaEffectWeaponComponentIndex in Mover:General block in gamesys.fit");
	
 	result = mechFile->seekBlock("Mover:FireWeapon");
	if (result != NO_ERR)
		return(result);


	result = mechFile->readIdFloatArray("FireArc", FireArc, 3);
	if (result != NO_ERR)
		return(result);
	for (long arc = 0; arc < 3; arc++)
		FireArc[arc] /= 2.0;

	result = mechFile->readIdLong("AimedFireAbort", AimedFireAbort);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdLongArray("AimedFireHitTable", AimedFireHitTable, 3);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("DisableAttackModifier", DisableAttackModifier);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("DisableGunneryModifier", DisableGunneryModifier);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("SalvageAttackModifier", SalvageAttackModifier);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("MaxStationaryTime", MaxStationaryTime);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("MaxTimeRevealed",MaxTimeRevealed);
	if (result != NO_ERR)
		return(result);
		
	result = mechFile->readIdFloat("HeadShotModifier",HeadShotModifier);
	if (result != NO_ERR)
		HeadShotModifier = 0.08f;
		
	result = mechFile->readIdFloat("ArmShotModifier",ArmShotModifier);
	if (result != NO_ERR)
		ArmShotModifier = 0.15f;
		
	result = mechFile->readIdFloat("LegShotModifier",LegShotModifier);
	if (result != NO_ERR)
		LegShotModifier = 0.20f;
		
	result = mechFile->seekBlock("Mover:Damage");
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdLongArray("HitLevel", hitLevel, 2);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("PilotingCheckFactor", PilotingCheckFactor);
	if (result != NO_ERR)
		return(result);

	result = mechFile->seekBlock("Components");
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdLong("ClusterSizeSRM", ClusterSizeSRM);
	if (result != NO_ERR)
		return(result);
	// Hard code--ultimately, we kill clusters in game code!
	ClusterSizeSRM = 2;

	result = mechFile->readIdLong("ClusterSizeLRM", ClusterSizeLRM);
	if (result != NO_ERR)
		return(result);
	// Hard code--ultimately, we kill clusters in game code!
	ClusterSizeLRM = 5;

	result = mechFile->readIdLongArray("InnerSphereAntiMissile", AntiMissileSystemStats[0], 2);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdLongArray("ClanAntiMissile", AntiMissileSystemStats[1], 2);
	if (result != NO_ERR)
		return(result);

	result = mechFile->seekBlock("Warrior");
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("DefaultAttackRadius", DefaultAttackRadius);
	if (result != NO_ERR)
		DefaultAttackRadius = 275.0;

	result = mechFile->readIdFloatArray("WarriorRankScale", WarriorRankScale, NUM_WARRIOR_RANKS);
	if (result != NO_ERR)
		return(result);

    /*
	char profData[NUM_OFFSET_RANGES * 2];
	result = mechFile->readIdCharArray("ProfessionalismTable", profData, NUM_OFFSET_RANGES * 2);
	if (result != NO_ERR)
		return(result);
	for (long i = 0; i < NUM_OFFSET_RANGES; i++)
		memcpy(&ProfessionalismOffsetTable[i], &profData[i * 2], 2);

	result = mechFile->readIdCharArray("DecorumTable", profData, NUM_OFFSET_RANGES * 2);
	if (result != NO_ERR)
		return(result);
	for (int i = 0; i < NUM_OFFSET_RANGES; i++)
		memcpy(&DecorumOffsetTable[i], &profData[i * 2], 2);

	result = mechFile->readIdCharArray("AmmoTable", profData, 2 * 2);
	if (result != NO_ERR)
		return(result);
	for (int i = 0; i < 2; i++)

		memcpy(&AmmoConservationModifiers[i], &profData[i * 2], 2);
    */

#ifdef USEHEAT
	result = mechFile->readIdFloat("WeaponHeatCheckDelay", WeaponHeatCheckDelay);
	if (result != NO_ERR)
		return(result);
#endif

	result = mechFile->readIdFloat("PilotCheckHalfRate", PilotCheckHalfRate);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdLongArray("PilotCheckModifiers", PilotCheckModifierTable, 2);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("DamageRateFrequency", DamageRateFrequency);
	if (result != NO_ERR)
		return(result);

	char charData[NUM_ATTITUDES * 6];
	result = mechFile->readIdCharArray("AttitudeEffect", charData, NUM_ATTITUDES * 6);
	if (result != NO_ERR)
		return(result);

	for (int i = 0; i < NUM_ATTITUDES; i++)
		memcpy(&AttitudeEffect[i], &charData[i * 6], 6);

	result = mechFile->readIdFloat("MovementUpdateFrequency", MovementUpdateFrequency);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("CombatUpdateFrequency", CombatUpdateFrequency);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("CommandUpdateFrequency", CommandUpdateFrequency);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("PilotCheckUpdateFrequency", PilotCheckUpdateFrequency);
	if (result != NO_ERR)
		return(result);


	result = mechFile->readIdInt("SkillIncreaseCap", MechWarrior::increaseCap);
	Assert(result == NO_ERR, result, " Couldn't find SkillCap variable in Warrior block of gamesys.fit ");

	result = mechFile->readIdFloat("SkillMax", MechWarrior::maxSkill);
	Assert(result == NO_ERR, result, " Couldn't find SkillMax variable in Warrior block of gamesys.fit ");
	
	result = mechFile->readIdFloat("SkillMin", MechWarrior::minSkill);
	Assert(result == NO_ERR, result, " Couldn't find SkillMin variable in Warrior block of gamesys.fit ");

		
	result = mechFile->seekBlock("Sensors");
	if (result != NO_ERR)
		return(result);


	result = mechFile->readIdFloat("BaseSensorRollTarget", SensorBaseChance);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("SensorSkillFactor", SensorSkillFactor);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("BlockingObjectModifier", SensorBlockingObjectModifier);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("ShutdownMech", SensorShutDownMechModifier);
	if (result != NO_ERR)
		return(result);

#ifdef USEHEAT
	result = mechFile->readIdFloat("HeatMultiplier", SensorHeatMultiplier);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("WeaponHeatMultiplier", SensorWeaponHeatMultiplier);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloatArray("HeatSourceModifiers", SensorHeatSourceModifiers, 2);
	if (result != NO_ERR)
		return(result);
#endif

	float rangeTable[8];
	result = mechFile->readIdFloatArray("SensorRangeModifier", rangeTable, 8);
	if (result != NO_ERR)
		return(result);
	for (int i = 0; i < 4; i++) {
		SensorRangeModifier[i][0] = rangeTable[i * 2];
		SensorRangeModifier[i][1] = rangeTable[i * 2 + 1];
	}

	float sizeTable[6];
	result = mechFile->readIdFloatArray("SizeModifier", sizeTable, 6);
	if (result != NO_ERR)
		return(result);
	for (int i = 0; i < 3; i++) {
		SensorSizeModifier[i][0] = sizeTable[i * 2];
		SensorSizeModifier[i][1] = sizeTable[i * 2 + 1];
	}

	long masterIDs[9];
	result = mechFile->readIdLongArray("SensorMasterIDs", masterIDs, 9);
	if (result != NO_ERR)
		return(result);
	
/*
	float rangeMultipliers[9];
	result = mechFile->readIdFloatArray("SensorRangeMultipliers", rangeMultipliers, 9);
	if (result != NO_ERR)
		return(result);
	// The following relies upon knowing the MasterComponentID for all of the sensor
	// components...
	for (i = 0; i < 9; i++)
		if (masterIDs[i] > -1)
			MasterComponentList[masterIDs[i]].setSensorRange(visualRange * rangeMultipliers[i]);
*/
	result = mechFile->readIdFloatArray("BlockingTerrainModifiers", SensorBlockingTerrain, 2);
	if (result != NO_ERR)
		return(result);

	result = mechFile->seekBlock("Skills");
	if (result != NO_ERR)
		Fatal(result, "Couldn't find skill block in gamesys.fit");

	result = mechFile->readIdFloatArray("Skill Attempt", SkillTry, 4);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloatArray("Skill Success", SkillSuccess, 4);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdFloat("WeaponHit", WeaponHit);
	if (result != NO_ERR)
		return(result);
	result = mechFile->readIdFloatArray("KillSkillValues", KillSkill, 6);
	if (result != NO_ERR)
		return(result);
	result = mechFile->readIdFloat("Sensor Contact Skill", SensorSkill);
	if (result != NO_ERR)
		return(result);

	return(NO_ERR);
}

void Mover::set (Mover copy) {

	GameObject::set(copy);

	positionNormal = copy.positionNormal;
	velocity = copy.velocity;
	rotation = copy.rotation;
	strncpy(name, copy.name, MAXLEN_MOVER_NAME - 1);
	name[MAXLEN_MOVER_NAME - 1] = '\0'; 
	chassis = copy.chassis;
	damageRateTally = copy.damageRateTally;
	damageRateCheckTime = copy.damageRateCheckTime;
	pilotCheckDamageTally = copy.pilotCheckDamageTally;

	numBodyLocations = copy.numBodyLocations;
	for (long i = 0; i < numBodyLocations; i++)
		body[i] = copy.body[i];
	fieldedCV = copy.fieldedCV;
		
	numArmorLocations = copy.numArmorLocations;
	for (int i = 0; i < numArmorLocations; i++)	
		armor[i] = copy.armor[i];
}

void Mover::init (bool create) {
		
	GameObject::init(create);
	if (initialize) {
		killed = false;
		lost = false;
		salvaged = false;
		appearance = NULL;
		lowestWeaponNodeID = -2;
		sensorSystem = NULL;
		lastMapCell[0] = -1;
		lastMapCell[1] = -1;
		contactInfo = new ContactInfo;
		pilot = NULL;
		pilotHandle = 0;
		numWeaponHitsHandled = 0;
		prevTeamId = -1;
		numMovers++;
	}
	objectClass = MOVER;
	unitGroup = 2;

	causeOfDeath = -1;

	iconPictureIndex = 0;

	lowestWeaponNodeZ = -9999.9f;

	positionNormal.Zero();
	velocity.Zero();
	name[0] = '\0';
	chassis = 0;
	startDisabled = false;

	pathLocks = true;
	
	damageRateTally = 0;
	damageRateCheckTime = 1.0;
	pilotCheckDamageTally = 0;
	numBodyLocations = 0;
	fieldedCV = 0;
	numArmorLocations = 0;
	attackRange = FIRERANGE_OPTIMAL;
	suppressionFire = 0;

	numArmorLocations = 0;
	numOther = 0;
	numWeapons = 0;
	numAmmos = 0;
	numAmmoTypes = 0;

	cockpit = 255;
	engine = 255;
	lifeSupport = 255;
	sensor = 255;
	ecm = 255;
	probe = 255;
	jumpJets = 255;
	nullSignature = 255;
	maxWeaponEffectiveness = 0.0;
	weaponEffectiveness = 0.0;

	minRange = 0.0;
	maxRange = 0.0;
	optimalRange = 0.0;
	numFunctionalWeapons = 0;

	numAntiMissileSystems = 0;
	engineBlowTime = -1.0;
	maxMoveSpeed = 0.0;
	shutDownThisFrame = false;
	startUpThisFrame = false;
	disableThisFrame = false;

	teamId = -1;
	groupId = -1;
	squadId = -1;
	selectionIndex = -1;
	teamRosterIndex = -1;
	commanderId = -1;
	unitGroup = 2;						// the thing the user sets by hitting ctrl and a number

	pilotCheckModifier = -1;
	prevPilotCheckUpdate = 0.0;
	prevPilotCheckModifier = 0;
	prevPilotCheckDelta = 0;
	failedPilotingCheck = false;
	collisionFreeFromWID = 0;
	collisionFreeTime = -1.0;
	lastWeaponEffectivenessCalc = 0.0;
	lastOptimalRangeCalc = 0.0;
	challengerWID = 0;

	lastGesture = -1;
	control.init();
	dynamics.init();

	if (MPlayer) {
#ifdef USE_STRING_RESOURCES
		cLoadString(thisInstance,IDS_LOSTPLAYER+languageOffset,netPlayerName,254);
#else
		sprintf(netPlayerName, "%s", "NEED STRING RESOURCES");
#endif
		}
	else
		netPlayerName[0] = '\0';
	localMoverId = -1;
	netRosterIndex = -1;
	statusChunk.init();
	newMoveChunk = false;
	moveChunk.init();
	numWeaponFireChunks[CHUNK_RECEIVE] = 0;
	numWeaponFireChunks[CHUNK_SEND] = 0;
	numCriticalHitChunks[CHUNK_RECEIVE] = 0;
	numCriticalHitChunks[CHUNK_SEND] = 0;
	numRadioChunks[CHUNK_RECEIVE] = 0;
	numRadioChunks[CHUNK_SEND] = 0;
	ejectOrderGiven = false;

	timeLeft = 1.0;
	exploding = false;
	withdrawing = false;

	yieldTimeLeft = 0.0;
	lastValidPosition.Zero();
	pivotDirection = 0;
	lastHustleTime = -999.0;

	salvageVehicle = false;
	markDistanceMoved = 0.0;
	refitBuddyWID = 0;
	recoverBuddyWID = 0;

	crashAvoidSelf = 1;
	crashAvoidPath = 1;
	crashBlockSelf = 1;
	crashBlockPath = 1;
	crashYieldTime = 1.5;
	pathLockLength = 0;

	moveType = MOVETYPE_GROUND;
	moveLevel = 0;
	moveCenter.Zero();
	moveRadius = -1.0;
	followRoads = false;

	overlayWeightClass = 0;
	timeToClearSelection = 0.0;
	timeSinceMoving = 0.0;
	timeSinceFiredLast = MaxTimeRevealed;

	lastMovingTargetWID = 0;
	mechSalvage = true;

	setTangible(true);
	setFlag(OBJECT_FLAG_SENSOR, true);

	if (!sortList) {
		sortList = new SortList;
		if (!sortList)
			Fatal(0, " Unable to create Mover::sortList ");
		sortList->init(4000);
	}

	teleportPosition.x = -999999.0;
	teleportPosition.y = -999999.0;
	teleportPosition.z = -999999.0;
	debugPage = 0;
	
	conStat = 0;
	fadeTime = 0.0f;
	alphaValue = 0x0;
	isOnGui = false;

	if ( !holdFireIconHandle )
	{
		FullPathFileName path;
		path.init( artPath, "blip", ".tga" );
		
		holdFireIconHandle = mcTextureManager->loadTexture( path, gos_Texture_Alpha, gosHint_DisableMipmap | gosHint_DontShrink );
	}
}

void Mover::release (void) {

	setTeamId(-1, false);
	if (getGroup())
		getGroup()->remove(this);
	setCommanderId(-1);
	MechWarrior::freeWarrior(pilot);
	pilot = NULL;
}
