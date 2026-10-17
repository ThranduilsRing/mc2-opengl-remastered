// BattleMech: BattleMech::init (bool and FitIni variants), clear, status rating, setControl, weapon mount lookup, write.
// Declared in the class headers; shared names in mech_internal.h.
#include "mech_internal.h"
#include "mc2fmt.h"
#include <algorithm>


void BattleMech::init (bool create) {

	objectClass = BATTLEMECH;
	overlayWeightClass = OVERLAY_WEIGHT_MECH;
	cBills = 0;
	playedHeloDestruct = false;

    control.reset();
    control.settings.mech.throttle = 100; // sebiL init to not cause garbage

	numBodyLocations = NUM_MECH_BODY_LOCATIONS;
	for (long i = 0; i < numBodyLocations; i++) {
		body[i].CASE = false;
		body[i].totalSpaces = 0;
		body[i].curInternalStructure = 0.0;
		body[i].hotSpotNumber = 0;
		body[i].maxInternalStructure = 0;
		body[i].damageState = IS_DAMAGE_NONE;
	}

	numArmorLocations = NUM_MECH_ARMOR_LOCATIONS;

#ifdef USEHEAT
	heatDissipation = 0.0;
	heatCheckTime = 0.0;
#endif

	chassisClass = MECH_CLASS_LIGHT;
	chassisBR = 0;

	damageThisFrame = 0.0f;
	
	numArmorComponents = 0;
	playedCriticalHit = false;
	
	legStatus = LEG_STATUS_NORMAL;
	torsoStatus = TORSO_STATUS_NORMAL;

	sentCrippledMsg = false;

	for (int i = 0; i < NUM_ACTUATORS; i++)
		actuator[i] = 255;
	gyro = 255;

	numJumpJets = 0;
	lastJumpTime = -100.0;
	inJump = false;
	jumpGoal.Zero();
	centerTorsoBlowTime = -1.0;
	hitFromBehindThisFrame = false;
	hitFromFrontThisFrame = false;

	leftArmBlownThisFrame = false;
	rightArmBlownThisFrame = false;

	torsoRotation = 0;
	leftArmRotation = 0;
	rightArmRotation = 0;

	fallen = false;
	mechRemoved = false;

	pivotTurnRate = 0.0;

#ifdef USE_JETS
	jumpJets[0] = nullptr;
	jumpJets[1] = nullptr;
#endif

	maxWeaponDamage = 0.0;
	longName[0] = '\0';
	pilotNum = 0;

	captureable = false;
	notMineYet = true;
	descID = -1;

	exploding = false;
	blipFrame = 0;
	drawFlags = 0;

	lowestWeaponNodeID = -2;
	
	damageAfterDisabled = 0.0f;
	
	for (long l = 0; l < 6; l++)
		rotateValues[l] = 0;

#ifdef USE_MECHDEBUGFILE
	if (!MechDebugFile) {
		MechDebugFile = UserFile::getNewFile();
		if (!MechDebugFile)
			Fatal(0, " Couldn't create Mech Debug File ");
		long err = MechDebugFile->open("mech.dbg");
		if (err)
			Fatal(0, " Couldn't open Mech Debug File ");
		MechDebugFile->write("Name           Target Turn Rot   RelFacing torsoRot newRot torsoRelFacing turnRate");
	}
#endif
}

void BattleMech::clear (void) {
}

void BattleMech::init (bool create, ObjectTypePtr _type) {

	GameObject::init(create, _type);
	creationTime = scenarioTime;

	setTangible(true);

	BattleMechTypePtr mechT = (BattleMechTypePtr)_type;
	for (long curLocation = 0; curLocation < NUM_BODY_LOCATIONS; curLocation++)
		body[curLocation].maxInternalStructure = mechT->maxInternalStructure[curLocation];

	crashAvoidSelf = mechT->crashAvoidSelf;
	crashAvoidPath = mechT->crashAvoidPath;
	crashBlockSelf = mechT->crashBlockSelf;
	crashBlockPath = mechT->crashBlockPath;
	crashYieldTime = mechT->crashYieldTime;

	setMoveType(mechT->moveType);

	control.init();

	dynamics = ((BattleMechTypePtr)_type)->dynamics;

	char *appearanceName = _type->getAppearanceTypeName();

	// New code!!!
	// We need to append the sprite type to the appearance num now.
	// The MechEdit tool does not assume a sprite type, nor should it.
	// MechCmdr2 features much simpler objects which only use 1 type of sprite!
	long appearanceType = (MECH_TYPE << 24);

	AppearanceTypePtr mechAppearanceType = appearanceTypeList->getAppearance(appearanceType,appearanceName);
	if (!mechAppearanceType)
	{
		// This mech probably is not done.  Use the MADCAT
		mechAppearanceType = appearanceTypeList->getAppearance(appearanceType,"MADCAT");
	}

	if (appearance) {
	    // M2: MANDATORY pre-init destroyMech. If BattleMech::init() is called
	    // on an object that already has a live appearance (re-init scenario),
	    // the previous appearance is deleted without BattleMech::destroy() being
	    // called. Retire the handle before the old appearance is deleted so the
	    // adapter can access it via getRenderWorldHandle().
	    // No-op if getRenderWorldHandle().isValid() == false (first-time init
	    // before any prior spawn, or already retired).
	    {
	        Mech3DAppearance* m3d = static_cast<Mech3DAppearance*>(appearance);
	        GameAdapters::Mech::destroyMech(*m3d);
	    }
	    releaseAppearance();
	}
	adoptAppearance(new Mech3DAppearance);
	gosASSERT(appearance != nullptr);

	// The only kind of appearanceType the mechs currently know how
	// to work with is a spriteTree.  Anything else is wrong.
	appearance->init((Mech3DAppearanceTypePtr)mechAppearanceType, this);
	appearance->initFX();
	// M2: RenderWorld spawn route. Called AFTER initFX() so the appearance
	// is fully initialized before the adapter records the handle.
	// static_cast safe: appearance is always Mech3DAppearance* here
	// (verified: only this overload assigns new Mech3DAppearance; one hit
	// in code/mech.cpp for "appearance = new").
	// gameObjectId=0 in M2; M2.5 refines when object-ID writes need correlation.
	{
	    Mech3DAppearance* m3d = static_cast<Mech3DAppearance*>(appearance);
	    GameAdapters::Mech::syncSpawn(*m3d, 0u);
	}
	appearance->setAlphaValue(alphaValue);

	objectClass = BATTLEMECH;

	setObscured(true);
	markDistanceMoved = 1000.0;
}

float BattleMech::getStatusRating (void) {

	float weaponEffect = weaponEffectiveness / maxWeaponEffectiveness;

	float armorHead = armor[MECH_ARMOR_LOCATION_HEAD].curArmor / armor[MECH_ARMOR_LOCATION_HEAD].maxArmor * 0.6 + 0.4;

	float armorCTorso = armor[MECH_ARMOR_LOCATION_CTORSO].curArmor / armor[MECH_ARMOR_LOCATION_CTORSO].maxArmor * 0.5 + 0.5;
	if (armor[MECH_ARMOR_LOCATION_CTORSO].curArmor > armor[MECH_ARMOR_LOCATION_RCTORSO].curArmor)
		armorCTorso = armor[MECH_ARMOR_LOCATION_RCTORSO].curArmor / armor[MECH_ARMOR_LOCATION_RCTORSO].maxArmor * 0.5 + 0.5;
				
	float curTorso = armor[MECH_ARMOR_LOCATION_RLTORSO].curArmor +
					 armor[MECH_ARMOR_LOCATION_LTORSO].curArmor +
					 armor[MECH_ARMOR_LOCATION_RRTORSO].curArmor +
					 armor[MECH_ARMOR_LOCATION_RTORSO].curArmor;
	float maxTorso = armor[MECH_ARMOR_LOCATION_RLTORSO].maxArmor +
					 armor[MECH_ARMOR_LOCATION_LTORSO].maxArmor+
					 armor[MECH_ARMOR_LOCATION_RRTORSO].maxArmor +
					 armor[MECH_ARMOR_LOCATION_RTORSO].maxArmor;
	float armorTorso = curTorso / maxTorso * 0.25 + 0.75;

	float curArm = armor[MECH_ARMOR_LOCATION_LARM].curArmor + armor[MECH_ARMOR_LOCATION_RARM].curArmor;
	float maxArm = armor[MECH_ARMOR_LOCATION_LARM].maxArmor + armor[MECH_ARMOR_LOCATION_RARM].maxArmor;
	float armorArm = curArm / maxArm * 0.25 + 0.75;

	float curLeg = armor[MECH_ARMOR_LOCATION_LLEG].curArmor + armor[MECH_ARMOR_LOCATION_RLEG].curArmor;
	float maxLeg = armor[MECH_ARMOR_LOCATION_LLEG].maxArmor + armor[MECH_ARMOR_LOCATION_RLEG].maxArmor;
	float armorLeg = curLeg / maxLeg * 0.25 + 0.75;

	float armorEffect = armorHead * armorCTorso * armorTorso * armorArm * armorLeg;
				
	float pilotWoundTable[7] = {1.00f, 0.95f, 0.85f, 0.75f, 0.50f, 0.30f, 0.00f};
	// Wounds accumulate past 6 (a dead pilot) and the table has 7 entries: clamp so a dead pilot reads the last one.
	const long woundIndex = std::clamp((long)getPilot()->getWounds(), 0L, 6L);
	float pilotEffect = pilotWoundTable[woundIndex];
	if (isDestroyed() || isDisabled())
		pilotEffect = 0.0;
					
	float rating = weaponEffect * armorEffect * pilotEffect;

	return(rating);
}

void BattleMech::setControl (ControlType ctrlType) {

	control.init(ctrlType, CONTROL_DATA_MECH);
}

// GETWEAPONNODE-HARDPOINT-ADAPTER-1 (Phase-2 slice 4): authoritative-with-fallback
// weapon-node mount resolution. The pure verdict (socket vs round-robin) lives in
// chooseWeaponNode() (hardpoint.h); this glue supplies the two node indices.
long BattleMech::resolveWeaponMountNode (const InventoryItem& item, long sourceWeaponType)
{
	// Legacy least-used round-robin -- ALWAYS computed so its nodeUsed[] side
	// effect is preserved exactly (parity), whichever node we ultimately mount to.
	const long roundRobinNode = appearance->getWeaponNode(sourceWeaponType);

	// Fast path / stock parity: gate OFF or no declared hardpoint -> fallback.
	if (!mc2UnitProfileDataEnabled() || item.hardpointId == 0)
		return roundRobinNode;

	// Declared hardpoint: resolve id -> profile -> socket name -> node index.
	HardpointProfile hp;
	long socketNode = -1;
	if (resolveHardpoint(item.hardpointId, hp) && !hp.socket.empty())
		socketNode = static_cast<Mech3DAppearance*>(appearance)
		                 ->resolveWeaponNodeBySocketName(hp.socket.c_str());

	return chooseWeaponNode(/*hasHardpoint*/ true, socketNode, roundRobinNode);
}

long BattleMech::init (FitIniFile* mechFile) {

	const char* BodyLocationBlockString[NUM_BODY_LOCATIONS] = {
		"Head",
		"CenterTorso",
		"LeftTorso",
		"RightTorso",
		"LeftArm",
		"RightArm",
		"LeftLeg",
		"RightLeg"
	};

	long result = mechFile->seekBlock("Header");
	if (result != NO_ERR)
		return(result);

	char fileType[128];
	result = mechFile->readIdString ("FileType", fileType, 127);
	if (result != NO_ERR)
		return(result);
	if (strcmp(fileType, "MechProfile"))
		return(-1);

	result = mechFile->seekBlock("General");
	if (result != NO_ERR)
		return(result);

	char thisMechName[128];
	result = mechFile->readIdString ("Name", thisMechName, 127);
	strncpy(name, thisMechName, MAXLEN_MOVER_NAME - 1);
	name[MAXLEN_MOVER_NAME-1] = '\0'; 

	result = mechFile->readIdInt("ChassisBR", chassisBR);
	if (result != NO_ERR)
		chassisBR = 100;

	result = mechFile->readIdFloat("CurTonnage", tonnage);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdInt("DescIndex", descID);
	if (result != NO_ERR)
		descID = -1;

	longName[0] = '\0';



	result = mechFile->readIdInt("Pilot",pilotNum);
	if (result != NO_ERR)
		pilotNum = -1;
		
	char cStatus = 0;

	status = cStatus;

	result = mechFile->readIdBoolean("NotMineYet",notMineYet);
	if (result != NO_ERR)
		notMineYet = true;		//We weren't written by logistics.  I.E. Not mine Yet.
		
#ifdef USEHEAT
	result = mechFile->seekBlock("HeatSinks");
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdUChar("HeatSinkType", heatSinkType);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdUChar("ExtraHeatSinks", extraHeatSinks);
	if (result != NO_ERR)
		return(result);

	unsigned char heat;
	result = mechFile->readIdUChar("HeatBuildUp", heat);
	if (result != NO_ERR)
		return(result);
	heat = (float)heat;

	disabledHeatSinks = 0;
	curLegHeatSinks = 0;
	heatDissipation = calcHeatDissipation();
#endif

	result = mechFile->seekBlock("Engine");
	if (result != NO_ERR)
		return(result);

	unsigned char speed;	
	result = mechFile->readIdUChar("MaxRunSpeed", speed);
	if (result != NO_ERR)
		return(result);
	maxMoveSpeed = (float)speed;

	result = mechFile->seekBlock("MovementSystem");
	if (result == NO_ERR) {
		long crashSize = 0;
		result = mechFile->readIdLong("CrashAvoidSelf", crashSize);
		if (result == NO_ERR)
			crashAvoidSelf = crashSize;

		result = mechFile->readIdLong("CrashAvoidPath", crashSize);
		if (result == NO_ERR)
			crashAvoidPath = crashSize;

		result = mechFile->readIdLong("CrashBlockSelf", crashSize);
		if (result == NO_ERR)
			crashBlockSelf = crashSize;

		result = mechFile->readIdLong("CrashBlockPath", crashSize);
		if (result == NO_ERR)
			crashBlockPath = crashSize;

		float crashYield = 0.0;
		result = mechFile->readIdFloat("CrashYieldTime", crashYield);
		if (result == NO_ERR)
			crashYieldTime = crashSize;
	}

	result = mechFile->seekBlock("MaxArmorPoints");
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdUChar("Head", armor[MECH_ARMOR_LOCATION_HEAD].maxArmor);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdUChar("CenterTorso", armor[MECH_ARMOR_LOCATION_CTORSO].maxArmor);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdUChar("LeftTorso", armor[MECH_ARMOR_LOCATION_LTORSO].maxArmor);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdUChar("RightTorso", armor[MECH_ARMOR_LOCATION_RTORSO].maxArmor);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdUChar("LeftArm", armor[MECH_ARMOR_LOCATION_LARM].maxArmor);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdUChar("RightArm", armor[MECH_ARMOR_LOCATION_RARM].maxArmor);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdUChar("LeftLeg", armor[MECH_ARMOR_LOCATION_LLEG].maxArmor);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdUChar("RightLeg", armor[MECH_ARMOR_LOCATION_RLEG].maxArmor);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdUChar("RearCenterTorso", armor[MECH_ARMOR_LOCATION_RCTORSO].maxArmor);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdUChar("RearLeftTorso", armor[MECH_ARMOR_LOCATION_RLTORSO].maxArmor);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdUChar("RearRightTorso", armor[MECH_ARMOR_LOCATION_RRTORSO].maxArmor);
	if (result != NO_ERR)
		return(result);

	result = mechFile->seekBlock("CurArmorPoints");
	if (result != NO_ERR)
		return(result);

	unsigned char currentArmor;
	result = mechFile->readIdUChar("Head", currentArmor);
	if (result != NO_ERR)
		return(result);

	armor[MECH_ARMOR_LOCATION_HEAD].curArmor = currentArmor;

	result = mechFile->readIdUChar("CenterTorso", currentArmor);
	if (result != NO_ERR)
		return(result);

	armor[MECH_ARMOR_LOCATION_CTORSO].curArmor = currentArmor;

	result = mechFile->readIdUChar("LeftTorso", currentArmor);
	if (result != NO_ERR)
		return(result);

	armor[MECH_ARMOR_LOCATION_LTORSO].curArmor = currentArmor;

	result = mechFile->readIdUChar("RightTorso", currentArmor);
	if (result != NO_ERR)
		return(result);

	armor[MECH_ARMOR_LOCATION_RTORSO].curArmor = currentArmor;

	result = mechFile->readIdUChar("LeftArm", currentArmor);
	if (result != NO_ERR)
		return(result);

	armor[MECH_ARMOR_LOCATION_LARM].curArmor = currentArmor;

	result = mechFile->readIdUChar("RightArm", currentArmor);
	if (result != NO_ERR)
		return(result);

	armor[MECH_ARMOR_LOCATION_RARM].curArmor = currentArmor;

	result = mechFile->readIdUChar("LeftLeg", currentArmor);
	if (result != NO_ERR)
		return(result);

	armor[MECH_ARMOR_LOCATION_LLEG].curArmor = currentArmor;

	result = mechFile->readIdUChar("RightLeg", currentArmor);
	if (result != NO_ERR)
		return(result);

	armor[MECH_ARMOR_LOCATION_RLEG].curArmor = currentArmor;

	result = mechFile->readIdUChar("RearCenterTorso", currentArmor);
	if (result != NO_ERR)
		return(result);

	armor[MECH_ARMOR_LOCATION_RCTORSO].curArmor = currentArmor;

	result = mechFile->readIdUChar("RearLeftTorso", currentArmor);
	if (result != NO_ERR)
		return(result);

	armor[MECH_ARMOR_LOCATION_RLTORSO].curArmor = currentArmor;

	result = mechFile->readIdUChar("RearRightTorso", currentArmor);
	if (result != NO_ERR)
		return(result);

	armor[MECH_ARMOR_LOCATION_RRTORSO].curArmor = currentArmor;

	result = mechFile->seekBlock("InventoryInfo");
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdUChar("NumOther", numOther);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdUChar("NumWeapons", numWeapons);
	if (result != NO_ERR)
		return(result);

	result = mechFile->readIdUChar("NumAmmo", numAmmos);
	if (result != NO_ERR)
		return(result);

	if ((numOther + numWeapons + numAmmos) > MAX_MOVER_INVENTORY_ITEMS)
		Fatal(numOther + numWeapons + numAmmos, " Battlemech.init: too many inventory items ");

	numAntiMissileSystems = 0;

	long curItem = 0;
	while (curItem < numOther) {
		char itemString[128];
		mc2::fmt::to_buf(itemString, "Item:{:d}", mc2::fmt::as_i32(curItem));
		result = mechFile->seekBlock(itemString);
		if (result != NO_ERR)
			return(result);

		result = mechFile->readIdUChar("MasterID", inventory[curItem].masterID);
		if (result != NO_ERR)
			return(result);
		inventory[curItem].health = MasterComponent::masterList[inventory[curItem].masterID].getHealth();
		inventory[curItem].disabled = false;
		inventory[curItem].amount = 1;
		inventory[curItem].ammoIndex = -1;
		inventory[curItem].readyTime = 0.0;
		inventory[curItem].bodyLocation = 255;
#ifdef USEHEAT
		inventory[curItem].heatPerSec = 0.0;
#endif

		switch (MasterComponent::masterList[inventory[curItem].masterID].getForm()) {
			case COMPONENT_FORM_JUMPJET:
				numJumpJets = 5;
				break;
			case COMPONENT_FORM_HEATSINK:
				break;
		}
		curItem++;
	}

	while (curItem < (numOther + numWeapons)) {
		char itemString[128];
		mc2::fmt::to_buf(itemString, "Item:{:d}", mc2::fmt::as_i32(curItem));
		result = mechFile->seekBlock(itemString);
		if (result != NO_ERR)
			return(result);

		result = mechFile->readIdUChar("MasterID", inventory[curItem].masterID);
		if (result != NO_ERR)
			return(result);

		result = mechFile->readIdUChar("FacesForward", inventory[curItem].facing);
		if (result != NO_ERR)
			return(result);

		inventory[curItem].health = MasterComponent::masterList[inventory[curItem].masterID].getHealth();
		inventory[curItem].disabled = false;
		inventory[curItem].amount = 1;
		inventory[curItem].ammoIndex = -1;
		inventory[curItem].readyTime = 0.0;
		inventory[curItem].bodyLocation = 255;
		inventory[curItem].effectiveness = (short)(MasterComponent::masterList[inventory[curItem].masterID].getWeaponDamage() * 10.0 /	// damage over 10 seconds
										   MasterComponent::masterList[inventory[curItem].masterID].getWeaponRecycleTime());
		inventory[curItem].effectiveness *= WeaponRanges[MasterComponent::masterList[inventory[curItem].masterID].getWeaponRange()][1] / 24;

		
		curItem++;
	}

	while (curItem < (numOther + numWeapons + numAmmos)) {
		char itemString[128];
		mc2::fmt::to_buf(itemString, "Item:{:d}", mc2::fmt::as_i32(curItem));
		result = mechFile->seekBlock(itemString);
		if (result != NO_ERR)
			return(result);

		result = mechFile->readIdUChar("MasterID", inventory[curItem].masterID);
		if (result != NO_ERR)
			return(result);

		long itemAmount;
		result = mechFile->readIdLong("Amount", itemAmount);
		if (result != NO_ERR) {
			unsigned char itemAmount2;
			result = mechFile->readIdUChar("Amount", itemAmount2);
			if (result != NO_ERR)
				return(result);
			else
				itemAmount = itemAmount2;
		}

		// Since the ammo amount in the profile is amount per ton, let's make the
		// amount equal to the number of missiles/bullets/whatever. If it's set
		// to 255, use the default amount per ton as defined in the component
		// table...
		if (itemAmount == -1)
			inventory[curItem].amount = MasterComponent::masterList[inventory[curItem].masterID].getAmmoPerTon();
		else
			inventory[curItem].amount = itemAmount;
		inventory[curItem].ammoIndex = -1;
		inventory[curItem].startAmount = inventory[curItem].amount;
		inventory[curItem].health = MasterComponent::masterList[inventory[curItem].masterID].getHealth();
		inventory[curItem].disabled = false;
		inventory[curItem].readyTime = 0.0;
		inventory[curItem].bodyLocation = 255;
#ifdef USEHEAT
		inventory[curItem].heatPerSec = 0.0;
#endif

		curItem++;
	}

	for (long curLocation = 0; curLocation < NUM_MECH_BODY_LOCATIONS; curLocation++) {

		result = mechFile->seekBlock(BodyLocationBlockString[curLocation]);
		if (result != NO_ERR)
			return(result);

		unsigned char caseHere;
		result = mechFile->readIdUChar("CASE", caseHere);
		if (result != NO_ERR)
			return(result);
		body[curLocation].CASE = caseHere ? true : false;

		unsigned char internalStructure;
		result = mechFile->readIdUChar("CurInternalStructure", internalStructure);
		if (result != NO_ERR)
			return(result);

		body[curLocation].curInternalStructure = internalStructure;

		result = mechFile->readIdUChar("HotSpotNumber", body[curLocation].hotSpotNumber);
		if (result != NO_ERR)
			return(result);

		float damageLevel = body[curLocation].curInternalStructure / body[curLocation].maxInternalStructure;

		if (damageLevel <= 0.0)
			body[curLocation].damageState = IS_DAMAGE_DESTROYED;
		else if (damageLevel <= 0.5)
			body[curLocation].damageState = IS_DAMAGE_PARTIAL;
		else
			body[curLocation].damageState = IS_DAMAGE_NONE;

		long numSpaces = NumLocationCriticalSpaces[curLocation];

		body[curLocation].totalSpaces = 0;

		for (long curSpace = 0; curSpace < numSpaces; curSpace++)
		{
			char componentString[128];
			mc2::fmt::to_buf(componentString, "Component:{:d}", mc2::fmt::as_i32(curSpace));
			unsigned char spaceData[2];
			result = mechFile->readIdUCharArray(componentString, spaceData, 2);

			if (result != NO_ERR)
				return(result);

			body[curLocation].criticalSpaces[curSpace].inventoryID = spaceData[0];
			body[curLocation].criticalSpaces[curSpace].hit = spaceData[1] ? true : false;

			if (spaceData[0] < 255)
			{
				// MECH-INV-IDX-GUARD-1: spaceData[0] is a raw .mdf byte used to
				// index inventory[MAX_MOVER_INVENTORY_ITEMS]. The bound check below
				// was debug-only (#ifdef _DEBUG Assert) and compiled out of release,
				// leaving an OOB write/read on a corrupt profile. Add the runtime
				// guard the sibling binary-format path uses: skip out-of-range
				// spaces (valid populated range is numOther+numWeapons+numAmmos).
				if (spaceData[0] >= numOther+numWeapons+numAmmos)
					continue;
#ifdef _DEBUG
			char msg[256];
			mc2::fmt::to_buf(msg, " Bad Mech Profile : {} ", mc2::fmt::safe(mechFile->getFilename()));
			Assert(spaceData[0] < numOther+numWeapons+numAmmos,spaceData[0],msg);
#endif
				inventory[spaceData[0]].bodyLocation = curLocation;

				// The following line assumes the "new" crit hit system...
				body[curLocation].totalSpaces += MasterComponent::masterList[inventory[spaceData[0]].masterID].getSize();
				switch (MasterComponent::masterList[inventory[spaceData[0]].masterID].getForm())
				{
					case COMPONENT_FORM_COCKPIT:
						cockpit = spaceData[0];
						break;

					case COMPONENT_FORM_JUMPJET:
						jumpJets = spaceData[0];
						break;

					case COMPONENT_FORM_SENSOR:
						sensor = spaceData[0];
						sensorSystem = SensorManager->newSensor();
						sensorSystem->setOwner(this);
						sensorSystem->setRange(MasterComponent::masterList[inventory[sensor].masterID].getSensorRange());
						break;

					case COMPONENT_FORM_HEATSINK:
						inventory[spaceData[0]].bodyLocation = curLocation;
						break;

					case COMPONENT_FORM_AMMO:
						inventory[spaceData[0]].bodyLocation = curLocation;
						break;

					case COMPONENT_FORM_WEAPON:
					case COMPONENT_FORM_WEAPON_ENERGY:
						inventory[spaceData[0]].bodyLocation = curLocation;
						break;

					case COMPONENT_FORM_WEAPON_BALLISTIC:
						inventory[spaceData[0]].bodyLocation = curLocation;
						if ((inventory[spaceData[0]].masterID == MasterComponent::clanAntiMissileSystemID) ||
							(inventory[spaceData[0]].masterID == MasterComponent::innerSphereAntiMissileSystemID))
						{
							if (numAntiMissileSystems == MAX_ANTI_MISSILE_SYSTEMS)
								Fatal(0, "Too many Anti-Missile Systems");
							antiMissileSystem[numAntiMissileSystems++] = spaceData[0];
						}
						break;

					case COMPONENT_FORM_WEAPON_MISSILE:
						inventory[spaceData[0]].bodyLocation = curLocation;
						break;

					case COMPONENT_FORM_ACTUATOR:
						if (inventory[spaceData[0]].masterID == MasterComponent::armActuatorID)
						{
							if (curLocation == MECH_BODY_LOCATION_LARM)
								actuator[ACTUATOR_LSHOULDER] = spaceData[0];
							else if (curLocation == MECH_BODY_LOCATION_RARM)
								actuator[ACTUATOR_RSHOULDER] = spaceData[0];
						}
						else if (inventory[spaceData[0]].masterID == MasterComponent::legActuatorID)
						{
							if (curLocation == MECH_BODY_LOCATION_LLEG)
								actuator[ACTUATOR_LHIP] = spaceData[0];
							else if (curLocation == MECH_BODY_LOCATION_RLEG)
								actuator[ACTUATOR_RHIP] = spaceData[0];
						}
						break;

					case COMPONENT_FORM_ENGINE:
						engine = spaceData[0];
						break;

					case COMPONENT_FORM_LIFESUPPORT:
						lifeSupport = spaceData[0];
						break;

					case COMPONENT_FORM_GYROSCOPE:
						gyro = spaceData[0];
						break;

					case COMPONENT_FORM_ECM:
						ecm = spaceData[0];
						break;
						
					case COMPONENT_FORM_JAMMER:
						break;
						
					case COMPONENT_FORM_PROBE:
						probe = spaceData[0];
						break;
				}
			}
		}
	}

	calcAmmoTotals();

	for (long item = numOther; item < (numOther + numWeapons); item++) {
		for (long ammoIndex = 0; ammoIndex < numAmmoTypes; ammoIndex++) {
			if ((long)MasterComponent::masterList[inventory[item].masterID].getWeaponAmmoMasterId() == ammoTypeTotal[ammoIndex].masterId) {
				inventory[item].ammoIndex = ammoIndex;
				break;
			}
		}
	}

	for (int item = numOther+numWeapons;item<(numOther+numWeapons+numAmmos);item++) {
		// Each weapon should point to its appropriate ammo total in
		// the ammo type total list...
		for (long ammoIndex = 0; ammoIndex < numAmmoTypes; ammoIndex++) {
			if (inventory[item].masterID == ammoTypeTotal[ammoIndex].masterId) {
				inventory[item].ammoIndex = ammoIndex;
				break;
			}
		}
	}

	for (int item = 0; item < numOther; item++) {
		if ((inventory[item].masterID == MasterComponent::clanAntiMissileSystemID) || (inventory[item].masterID == MasterComponent::innerSphereAntiMissileSystemID)) {
			for (long ammoIndex = 0; ammoIndex < numAmmoTypes; ammoIndex++) {
				if ((long)MasterComponent::masterList[inventory[item].masterID].getWeaponAmmoMasterId() == ammoTypeTotal[ammoIndex].masterId) {
					inventory[item].ammoIndex = ammoIndex;
					break;
				}
			}
		}
	}

#ifdef USEHEAT
	curLegHeatSinks = calcHeatSinks(MECH_BODY_LOCATION_RLEG, true) + calcHeatSinks(MECH_BODY_LOCATION_LLEG, true);
#endif


	calcLegStatus();
	calcTorsoStatus();
	calcFireRanges();

	maxCV = calcCV(true);
	curCV = calcCV(false);
	maxWeaponDamage = calcMaxTargetDamage();

	ObjectTypePtr type = ObjectManager->getObjectType(typeHandle);
	if (type->getExplosionObject() > 0)
		ObjectManager->objTypeManager->load(type->getExplosionObject(), true);
	chassisClass = getMechClass();

	// Mod-tolerance: mod mechs from stub-substituted BattleMechType paths
	// may not contain a COMPONENT_FORM_SENSOR in their inventory, so the
	// sensor-allocating branch in the inventory loop above never fires
	// and sensorSystem stays NULL. Every downstream caller
	// (setShutdown/broken/numContacts/etc) then crashes with a READ at 0x29.
	// Fallback: allocate a default sensor here for any mech still missing one.
	if (!sensorSystem && SensorManager) {
		sensorSystem = SensorManager->newSensor();
		if (sensorSystem) {
			sensorSystem->setOwner(this);
			sensorSystem->setRange(100.0f);
			if (teamId >= 0 && teamId < MAX_TEAMS)
				SensorManager->addTeamSensor(teamId, sensorSystem);
		}
	}

	// UNITPROFILE-TYPELOAD-SOURCE-1: numJumpJets is authoritative here; fold it
	// into the type baseline + this instance's runtime via the single owner.
	refreshDerivedCapabilities();

	return(NO_ERR);
}

long BattleMech::init (FilePtr mechFile) {

	return(NO_ERR);
}

long BattleMech::write (FilePtr mechFile) {

	return(NO_ERR);
}
