// BattleMech: loadout build: BattleMech::init(variant) and resetComponents.
// Declared in the class headers; shared names in mech_internal.h.
#include "mech_internal.h"


#define ARMOR_POINTS_PER_NEWARMOR				32.0f
#define ARMOR_HEAD_PERCENT						0.0f
#define ARMOR_CTORSO_PERCENT					0.17f
#define ARMOR_RTORSO_PERCENT					0.125f
#define ARMOR_LTORSO_PERCENT					0.125f
#define ARMOR_LARM_PERCENT						0.12f
#define ARMOR_RARM_PERCENT						0.12f
#define ARMOR_LLEG_PERCENT						0.13f
#define ARMOR_RLEG_PERCENT						0.13f
#define ARMOR_RCTORSO_PERCENT					0.03f
#define ARMOR_RLTORSO_PERCENT					0.02f
#define ARMOR_RRTORSO_PERCENT					0.02f

// LOADOUT-EQUIPMENT-ID-ALIAS-1 (equipment arc, engine slice E5).
// ADDITIVE string-id branch beside the legacy numeric MasterID column. The stock
// loadout parse fills inventory[].masterID from a positional numeric CSV column
// (readUChar(row, 5, masterID)). This helper adds an OPTIONAL string-id token:
// gate-ON, it reads an `EquipmentId` string from an out-of-band column of the
// SAME row; if present and it resolves through the EquipmentRegistry to a legacy
// ordinal in the existing 0..254 space, that becomes the masterID. Otherwise the
// caller keeps the already-read legacy numeric masterID UNCHANGED.
// This is NOT a parser rewrite: the legacy readUChar path runs first and is the
// default; this only OVERRIDES masterID when a string-id token is present AND the
// gate is ON. Stock CSVs carry no such column -> readString errors -> no override
// -> byte-identical. No widen (rejects ids >=255), no firing change.
// The optional token lives in a high, stock-unused column so a stock loadout row
// never accidentally supplies one.
enum { kEquipmentIdCsvColumn = 40 };

// If gate-ON and the row carries an optional EquipmentId string token that
// resolves to a legacy ordinal in the existing 8-bit space, return that ordinal
// (0..254). Otherwise return -1 (no override -> caller keeps legacy masterID).
static int resolveOptionalEquipmentIdColumn(CSVFile* mechFile, long row)
{
	if (!mc2UnitProfileDataEnabled())
		return -1;                    // gate OFF -> legacy path only (parity)

	char refBuf[64] = {0};
	// Out-of-band column: stock rows have no data here -> readString != NO_ERR.
	if (mechFile->readString(row, kEquipmentIdCsvColumn, refBuf, sizeof(refBuf) - 1) != NO_ERR)
		return -1;                    // no token -> legacy path
	if (refBuf[0] == '\0')
		return -1;

	// MODDER-FIRST-ERROR-SURFACE-1: context for both warnings — WHICH csv file,
	// WHICH row, WHICH field, and the fallback taken. getFilename() is the loadout
	// CSV being parsed (mods override it whole-file, so the winning path IS the
	// modder's file).
	auto idAliasContext = [&](const char* msg, const char* expected) -> std::string {
		char fieldBuf[48];
		std::snprintf(fieldBuf, sizeof(fieldBuf), "EquipmentId (csv col %d, row %ld)",
		              (int)kEquipmentIdCsvColumn, row);
		ModderFinding f;
		f.message  = msg;
		f.file     = mechFile->getFilename() ? mechFile->getFilename() : "";
		f.id       = refBuf;
		f.field    = fieldBuf;
		f.expected = expected;
		f.fallback = "legacy numeric masterID for this row";
		return formatModderFinding(f);
	};

	int resolved = resolveLoadoutEquipmentRef(getEquipmentRegistry(), refBuf);
	if (resolved == kEquipmentRefNeedsWiden) {
		printf("[EQUIP-IDALIAS] WARNING: %s\n",
		       idAliasContext("equipment ref resolves to an id >=255 (future-widen, E4) -- keeping legacy masterID",
		                      "an equipment id resolving to a legacy ordinal 0..254").c_str());
		return -1;
	}
	if (resolved == kEquipmentRefNotFound) {
		printf("[EQUIP-IDALIAS] WARNING: %s\n",
		       idAliasContext("unknown equipment ref -- keeping legacy masterID",
		                      "a qualified id present in the equipment registry (is the owning mod loaded?)").c_str());
		return -1;
	}
	return resolved;                  // 0..254: override the legacy masterID
}

long BattleMech::init (DWORD variantNum)
{
	// Open the mech's CSV file.
	CSVFile *mechFile = new CSVFile;
	
	FullPathFileName csvName;
	csvName.init(objectPath,getObjectType()->getAppearanceTypeName(),".csv");

	variantID = variantNum;
	
	long result = mechFile->open(csvName);
	if (result != NO_ERR)
	{
		delete mechFile;
		mechFile = NULL;
		return result;
	}

	// CSV file open.  Variant Num controls offsets to data in the file.	
	// Same rules as before,  Rows start at one.  Columns start at zero.
	char thisMechName[128];
	mechFile->readString(3,2,thisMechName,127);
	strncpy(name, thisMechName, MAXLEN_MOVER_NAME - 1);
	name[MAXLEN_MOVER_NAME-1] = '\0';

	mechFile->readInt(10,5,chassisBR);

	mechFile->readFloat(3,5,tonnage);

	mechFile->readInt(5,2,descID);

	cLoadString( descID, thisMechName, 127 );

	char tmp[256];
	cLoadString( IDS_MFDMCH_PRINTSTRING, tmp, 256 );
	sprintf( longName, tmp, thisMechName, tonnage );

	mechFile->readInt( 12, 2, iconPictureIndex );

	unsigned char speed;	
	mechFile->readUChar(7,5,speed);
	maxMoveSpeed = (float)speed;

	result = mechFile->readString( 23 + ( 97 * variantNum ), 2, variantName, 63 );
	if (result == 1)
	{
		PAUSE(("Variant Number %d does not exist for mech %s.  Please set variant in editor!",variantNum,(const char*)csvName));
		variantNum = 0;
		variantID = 0;
		result = mechFile->readString( 23 + ( 97 * variantNum ), 2, variantName, 63 );
		if (result == 1)
			STOP(("Can't even find Variant 0 for mech %s",csvName));
	}

	status = 0;

	mechFile->readUChar(18,2,armor[MECH_ARMOR_LOCATION_HEAD].maxArmor);

	mechFile->readUChar(18,7,armor[MECH_ARMOR_LOCATION_CTORSO].maxArmor);

	mechFile->readUChar(18,5,armor[MECH_ARMOR_LOCATION_LTORSO].maxArmor);

	mechFile->readUChar(18,6,armor[MECH_ARMOR_LOCATION_RTORSO].maxArmor);
	
	mechFile->readUChar(18,3,armor[MECH_ARMOR_LOCATION_LARM].maxArmor);

	mechFile->readUChar(18,4,armor[MECH_ARMOR_LOCATION_RARM].maxArmor);

	mechFile->readUChar(18,8,armor[MECH_ARMOR_LOCATION_LLEG].maxArmor);

	mechFile->readUChar(18,9,armor[MECH_ARMOR_LOCATION_RLEG].maxArmor);

	mechFile->readUChar(18,12,armor[MECH_ARMOR_LOCATION_RCTORSO].maxArmor);

	mechFile->readUChar(18,10,armor[MECH_ARMOR_LOCATION_RLTORSO].maxArmor);

	mechFile->readUChar(18,11,armor[MECH_ARMOR_LOCATION_RRTORSO].maxArmor);

	unsigned char currentArmor;
	mechFile->readUChar(115 + (97 * variantNum),2,currentArmor);
	armor[MECH_ARMOR_LOCATION_HEAD].curArmor = currentArmor;

	mechFile->readUChar(115 + (97 * variantNum),7,currentArmor);
	armor[MECH_ARMOR_LOCATION_CTORSO].curArmor = currentArmor;

	mechFile->readUChar(115 + (97 * variantNum),5,currentArmor);
	armor[MECH_ARMOR_LOCATION_LTORSO].curArmor = currentArmor;

	mechFile->readUChar(115 + (97 * variantNum),6,currentArmor);
	armor[MECH_ARMOR_LOCATION_RTORSO].curArmor = currentArmor;

	mechFile->readUChar(115 + (97 * variantNum),3,currentArmor);
	armor[MECH_ARMOR_LOCATION_LARM].curArmor = currentArmor;

	mechFile->readUChar(115 + (97 * variantNum),4,currentArmor);
	armor[MECH_ARMOR_LOCATION_RARM].curArmor = currentArmor;

	mechFile->readUChar(115 + (97 * variantNum),8,currentArmor);
	armor[MECH_ARMOR_LOCATION_LLEG].curArmor = currentArmor;

	mechFile->readUChar(115 + (97 * variantNum),9,currentArmor);
	armor[MECH_ARMOR_LOCATION_RLEG].curArmor = currentArmor;

	mechFile->readUChar(115 + (97 * variantNum),12,currentArmor);
	armor[MECH_ARMOR_LOCATION_RCTORSO].curArmor = currentArmor;

	mechFile->readUChar(115 + (97 * variantNum),10,currentArmor);
	armor[MECH_ARMOR_LOCATION_RLTORSO].curArmor = currentArmor;

	mechFile->readUChar(115 + (97 * variantNum),11,currentArmor);
	armor[MECH_ARMOR_LOCATION_RRTORSO].curArmor = currentArmor;

	// Build the mech's inventory (all components, and where they are located)...
	// Check through all 30 possible.  Create the numOther, numWeapons and numAmmo from that.
	// Sanity check what the designers have input.
	numAntiMissileSystems = numOther = numWeapons = numAmmos = 0;
	numArmorComponents = 0;

	// Read in the mech's non-weapon/non-ammo components...
	long realItemNum = 0;
    MemSet(ItemLocationToInvLocation, 0xff);

	// sebi: init inventory items to uninitialized. Original mc2 memeory allocator seemed to init all by 0xff which coincidentally
	// marked all inventory items as not valid, our memory allocation does not do this and so there was garbage (0xcd), leading to
	// all items in inventory behind last loaded from file to contain garbage. I wonder how many are such things still present.
	// Before similar issue led to some in-game characters be invisible (salvage aircraft for example)
	for (long curItem = 0;curItem < MAX_MOVER_INVENTORY_ITEMS;curItem++)
	{
		inventory[curItem].masterID = 0xff;
		inventory[realItemNum].disabled = true;
		// LOADOUT-HARDPOINT-JOIN-1: default the optional logical-hardpoint join key
		// to unset. Stock CSV loadouts carry no hardpoint column, so this stays 0
		// (legacy positional-CSV mount unchanged). A data source may later set it.
		inventory[curItem].hardpointId = 0;
	}

	//Read in everything but weapons and AMMO
	for (long curItem = 0;curItem < MAX_MOVER_INVENTORY_ITEMS;curItem++)
	{
		mechFile->readUChar((26 + (97 * variantNum)) + curItem, 5, inventory[realItemNum].masterID);

		// LOADOUT-EQUIPMENT-ID-ALIAS-1 (E5): ADDITIVE string-id override. Gate-ON,
		// if this row carries an optional EquipmentId="core:<slug>" token that
		// resolves to a legacy ordinal in 0..254, use it; else keep the numeric
		// masterID just read. Stock rows have no token -> no override (parity).
		{
			int aliasId = resolveOptionalEquipmentIdColumn(mechFile, (26 + (97 * variantNum)) + curItem);
			if (aliasId >= 0)
				inventory[realItemNum].masterID = (unsigned char)aliasId;
		}

		// MECH-INV-IDX-GUARD-1: masterID is read from an untrusted .mdf and used to
		// index MasterComponent::masterList (sized numComponents). A corrupt, modded
		// or truncated file can hold an out-of-range id; normalize it to the empty
		// sentinel (0xff) so the slot is dropped like an unused one instead of
		// indexing masterList out of bounds (the existing 255 check below then skips
		// it). No-op for valid files, where every id is < numComponents.
		if (inventory[realItemNum].masterID != 0xff &&
			(long)inventory[realItemNum].masterID >= MasterComponent::numComponents)
			inventory[realItemNum].masterID = 0xff;

		if (inventory[realItemNum].masterID && (inventory[realItemNum].masterID != 255))
		{
			// If the component is a JumpJet, increment our jump jet count...
			switch (MasterComponent::masterList[inventory[realItemNum].masterID].getForm()) 
			{
				case COMPONENT_FORM_JUMPJET:
					numJumpJets = 5;
				case COMPONENT_FORM_COCKPIT:
				case COMPONENT_FORM_SENSOR:
				case COMPONENT_FORM_LIFESUPPORT:
				case COMPONENT_FORM_GYROSCOPE:
				case COMPONENT_FORM_POWER_AMPLIFIER:
				case COMPONENT_FORM_ECM:
				case COMPONENT_FORM_PROBE:
				case COMPONENT_FORM_JAMMER:
				case COMPONENT_FORM_CASE:
				case COMPONENT_FORM_ACTUATOR:
				case COMPONENT_FORM_ENGINE:
				case COMPONENT_FORM_HEATSINK:
					numOther++;
					inventory[realItemNum].health = MasterComponent::masterList[inventory[realItemNum].masterID].getHealth();
					inventory[realItemNum].disabled = false;
					inventory[realItemNum].amount = 1;
					inventory[realItemNum].ammoIndex = -1;
					inventory[realItemNum].readyTime = 0.0;
					inventory[realItemNum].bodyLocation = 255;
					ItemLocationToInvLocation[curItem] = realItemNum;
					realItemNum++;
					break;
				
				case COMPONENT_FORM_BULK:		//Special case now.
												//This is an armor component.
												//DO NOT store with other components
												//It becomes a free hit then!!
					numArmorComponents++;
				break;
				
				case COMPONENT_FORM_WEAPON:
				case COMPONENT_FORM_WEAPON_ENERGY:
				case COMPONENT_FORM_WEAPON_BALLISTIC:
				case COMPONENT_FORM_WEAPON_MISSILE:
					//Ignore weapons until second pass
					break;
					
				case COMPONENT_FORM_AMMO:
					//Ignore AMMO until third pass
					break;
			}
		}
	}

	//NOW read in the weapons.
	for (int curItem = 0;curItem < MAX_MOVER_INVENTORY_ITEMS;curItem++)
	{
		mechFile->readUChar((26 + (97 * variantNum)) + curItem, 5, inventory[realItemNum].masterID);

		// LOADOUT-EQUIPMENT-ID-ALIAS-1 (E5): ADDITIVE string-id override (weapons
		// pass). Same rule as the non-weapon pass above -- gate-ON optional
		// EquipmentId token resolves to a legacy ordinal; else legacy numeric id.
		{
			int aliasId = resolveOptionalEquipmentIdColumn(mechFile, (26 + (97 * variantNum)) + curItem);
			if (aliasId >= 0)
				inventory[realItemNum].masterID = (unsigned char)aliasId;
		}

		// MECH-INV-IDX-GUARD-1: masterID is read from an untrusted .mdf and used to
		// index MasterComponent::masterList (sized numComponents). A corrupt, modded
		// or truncated file can hold an out-of-range id; normalize it to the empty
		// sentinel (0xff) so the slot is dropped like an unused one instead of
		// indexing masterList out of bounds (the existing 255 check below then skips
		// it). No-op for valid files, where every id is < numComponents.
		if (inventory[realItemNum].masterID != 0xff &&
			(long)inventory[realItemNum].masterID >= MasterComponent::numComponents)
			inventory[realItemNum].masterID = 0xff;

		if (inventory[realItemNum].masterID && (inventory[realItemNum].masterID != 255))
		{
			// If the component is a JumpJet, increment our jump jet count...
			long sourceWeaponType;
			if (MasterComponent::masterList[inventory[realItemNum].masterID].getForm() == COMPONENT_FORM_WEAPON_MISSILE)
				sourceWeaponType = MECH3D_WEAPONTYPE_MISSILE;
			else if (MasterComponent::masterList[inventory[realItemNum].masterID].getForm() == COMPONENT_FORM_WEAPON_BALLISTIC)
				sourceWeaponType = MECH3D_WEAPONTYPE_BALLISTIC;
			else if (MasterComponent::masterList[inventory[realItemNum].masterID].getForm() == COMPONENT_FORM_WEAPON_ENERGY) 
				sourceWeaponType = MECH3D_WEAPONTYPE_ENERGY;
			else
				sourceWeaponType = MECH3D_WEAPONTYPE_ANY;
			
			switch (MasterComponent::masterList[inventory[realItemNum].masterID].getForm()) 
			{
				case COMPONENT_FORM_JUMPJET:
				case COMPONENT_FORM_COCKPIT:
				case COMPONENT_FORM_SENSOR:
				case COMPONENT_FORM_LIFESUPPORT:
				case COMPONENT_FORM_GYROSCOPE:
				case COMPONENT_FORM_POWER_AMPLIFIER:
				case COMPONENT_FORM_ECM:
				case COMPONENT_FORM_PROBE:
				case COMPONENT_FORM_JAMMER:
				case COMPONENT_FORM_BULK:
				case COMPONENT_FORM_CASE:
				case COMPONENT_FORM_ACTUATOR:
				case COMPONENT_FORM_ENGINE:
				case COMPONENT_FORM_HEATSINK:
					//We did these Above here.
					break;
				
				case COMPONENT_FORM_WEAPON:
				case COMPONENT_FORM_WEAPON_ENERGY:
				case COMPONENT_FORM_WEAPON_BALLISTIC:
				case COMPONENT_FORM_WEAPON_MISSILE:
					numWeapons++;
								
					inventory[realItemNum].health = MasterComponent::masterList[inventory[realItemNum].masterID].getHealth();
					inventory[realItemNum].disabled = false;
					inventory[realItemNum].amount = 1;
					inventory[realItemNum].ammoIndex = -1;
					// GETWEAPONNODE-HARDPOINT-ADAPTER-1 (Phase-2 slice 4): route the
					// weapon-node mount through the item's declared HardpointId when one
					// resolves to a real model socket; else the legacy least-used
					// round-robin. Stock items carry hardpointId==0 -> always fallback ->
					// byte-identical mount (parity). Returns the same node-index domain.
					inventory[realItemNum].facing = (unsigned char)resolveWeaponMountNode(inventory[realItemNum], sourceWeaponType);
					inventory[realItemNum].readyTime = 0.0;
					inventory[realItemNum].bodyLocation = 255;
					inventory[realItemNum].effectiveness = (short)(MasterComponent::masterList[inventory[realItemNum].masterID].getWeaponDamage() * 10.0 /	// damage over 10 seconds
													 MasterComponent::masterList[inventory[realItemNum].masterID].getWeaponRecycleTime());
					inventory[realItemNum].effectiveness *= WeaponRanges[MasterComponent::masterList[inventory[realItemNum].masterID].getWeaponRange()][1] / 24;
					ItemLocationToInvLocation[curItem] = realItemNum;
					realItemNum++;
					// Cache in the weapon special effect.
					
					break;
					
				case COMPONENT_FORM_AMMO:
					//Do these next.
					break;
			}
		}
	}

	//NOW read in the AMMO
	for (int curItem = 0;curItem < MAX_MOVER_INVENTORY_ITEMS;curItem++)
	{
		mechFile->readUChar((26 + (97 * variantNum)) + curItem, 5, inventory[realItemNum].masterID);

		// MECH-INV-IDX-GUARD-1: masterID is read from an untrusted .mdf and used to
		// index MasterComponent::masterList (sized numComponents). A corrupt, modded
		// or truncated file can hold an out-of-range id; normalize it to the empty
		// sentinel (0xff) so the slot is dropped like an unused one instead of
		// indexing masterList out of bounds (the existing 255 check below then skips
		// it). No-op for valid files, where every id is < numComponents.
		if (inventory[realItemNum].masterID != 0xff &&
			(long)inventory[realItemNum].masterID >= MasterComponent::numComponents)
			inventory[realItemNum].masterID = 0xff;

		if (inventory[realItemNum].masterID && (inventory[realItemNum].masterID != 255))
		{
			// If the component is a JumpJet, increment our jump jet count...
			switch (MasterComponent::masterList[inventory[realItemNum].masterID].getForm()) 
			{
				case COMPONENT_FORM_JUMPJET:
				case COMPONENT_FORM_COCKPIT:
				case COMPONENT_FORM_SENSOR:
				case COMPONENT_FORM_LIFESUPPORT:
				case COMPONENT_FORM_GYROSCOPE:
				case COMPONENT_FORM_POWER_AMPLIFIER:
				case COMPONENT_FORM_ECM:
				case COMPONENT_FORM_PROBE:
				case COMPONENT_FORM_JAMMER:
				case COMPONENT_FORM_BULK:
				case COMPONENT_FORM_CASE:
				case COMPONENT_FORM_ACTUATOR:
				case COMPONENT_FORM_ENGINE:
				case COMPONENT_FORM_HEATSINK:
					//Did these above.
					break;
				
				case COMPONENT_FORM_WEAPON:
				case COMPONENT_FORM_WEAPON_ENERGY:
				case COMPONENT_FORM_WEAPON_BALLISTIC:
				case COMPONENT_FORM_WEAPON_MISSILE:
					//Did These Above, too...
					break;
					
				case COMPONENT_FORM_AMMO:
					numAmmos++;
					inventory[realItemNum].health = MasterComponent::masterList[inventory[realItemNum].masterID].getHealth();
					inventory[realItemNum].disabled = false;
					inventory[realItemNum].amount = 1;
					inventory[realItemNum].ammoIndex = -1;
					inventory[realItemNum].readyTime = 0.0;
					inventory[realItemNum].bodyLocation = 255;
		
					// Since the ammo amount in the profile is amount per ton, let's make the
					// amount equal to the number of missiles/bullets/whatever. If it's set
					// to 255, use the default amount per ton as defined in the component
					// table...
					// Ammo per ton is always fully stocked.  I.e. always -1 or 255
					inventory[realItemNum].amount = MasterComponent::masterList[inventory[realItemNum].masterID].getAmmoPerTon();
					inventory[realItemNum].ammoIndex = -1;
					inventory[realItemNum].startAmount = inventory[realItemNum].amount;
					inventory[realItemNum].health = MasterComponent::masterList[inventory[realItemNum].masterID].getHealth();
					inventory[realItemNum].disabled = false;
					inventory[realItemNum].readyTime = 0.0;
					inventory[realItemNum].bodyLocation = 255;

					ItemLocationToInvLocation[curItem] = realItemNum;
					realItemNum++;
					break;
			}
		}
	}

	// Now, read in the component layout for each body location...
	for (long curLocation = 0; curLocation < NUM_MECH_BODY_LOCATIONS; curLocation++) 
	{
		body[curLocation].CASE = true;	//ALL MC2 Mechs have CASE everywhere.

		unsigned char internalStructure;
		mechFile->readUChar(114 + (97 * variantNum), stupidNewSageColumns[curLocation], internalStructure);
		body[curLocation].curInternalStructure = internalStructure;

		// Now, determine the damage state for the body location...
		float damageLevel = (float)body[curLocation].curInternalStructure / body[curLocation].maxInternalStructure;

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
			unsigned char spaceData;
			mechFile->readUChar((102 + (97*variantNum))+curSpace,stupidNewSageColumns[curLocation], spaceData);

			// MECH-INV-IDX-GUARD-1: spaceData is a raw .mdf byte (0-255) used to
			// index ItemLocationToInvLocation[MAX_MOVER_INVENTORY_ITEMS]. A corrupt
			// or out-of-range value (incl. the 0xff empty marker) would read past
			// the array. Mirror the guarded sibling path below ("sebi" guard at the
			// logistics read): treat out-of-range as an empty crit space.
			if (spaceData >= MAX_MOVER_INVENTORY_ITEMS)
			{
				body[curLocation].criticalSpaces[curSpace].inventoryID = 0xff;
				body[curLocation].criticalSpaces[curSpace].hit = false;
				continue;
			}
			spaceData = ItemLocationToInvLocation[spaceData];

			body[curLocation].criticalSpaces[curSpace].inventoryID = spaceData;
			body[curLocation].criticalSpaces[curSpace].hit = false;				//Everything always repaired now.
			
			if (spaceData < 255)
			{
				if (spaceData >= numOther+numWeapons+numAmmos)            
					//Just ignore this space.  Sage has filled them all in now.
					// ACTUALLY, we should stop here.
					continue;
					
				inventory[spaceData].bodyLocation = curLocation;

				// The following line assumes the "new" crit hit system...
				body[curLocation].totalSpaces += MasterComponent::masterList[inventory[spaceData].masterID].getSize();
				
				// Preserve critical component indices for quick access...
				switch (MasterComponent::masterList[inventory[spaceData].masterID].getForm())
				{
					case COMPONENT_FORM_COCKPIT:
						cockpit = spaceData;
						break;

					case COMPONENT_FORM_JUMPJET:
						jumpJets = spaceData;
						break;

					case COMPONENT_FORM_SENSOR:
						sensor = spaceData;
						sensorSystem = SensorManager->newSensor();
						sensorSystem->setOwner(this);
						sensorSystem->setRange(MasterComponent::masterList[inventory[sensor].masterID].getSensorRange());
						break;

					case COMPONENT_FORM_HEATSINK:
						inventory[spaceData].bodyLocation = curLocation;
						break;

					case COMPONENT_FORM_AMMO:
						inventory[spaceData].bodyLocation = curLocation;
						break;

					case COMPONENT_FORM_WEAPON:
					case COMPONENT_FORM_WEAPON_ENERGY:
						inventory[spaceData].bodyLocation = curLocation;
						break;

					case COMPONENT_FORM_WEAPON_BALLISTIC:
						inventory[spaceData].bodyLocation = curLocation;
						if ((inventory[spaceData].masterID == MasterComponent::clanAntiMissileSystemID) ||
							(inventory[spaceData].masterID == MasterComponent::innerSphereAntiMissileSystemID))
						{
							// Add to Anti-Missile System list for fast reference...
							if (numAntiMissileSystems == MAX_ANTI_MISSILE_SYSTEMS)
								Fatal(0, "Too many Anti-Missile Systems");
							antiMissileSystem[numAntiMissileSystems++] = spaceData;
						}
						break;

					case COMPONENT_FORM_WEAPON_MISSILE:
						inventory[spaceData].bodyLocation = curLocation;
						break;

					case COMPONENT_FORM_ACTUATOR:
						if (inventory[spaceData].masterID == MasterComponent::armActuatorID)
						{
							if (curLocation == MECH_BODY_LOCATION_LARM)
								actuator[ACTUATOR_LSHOULDER] = spaceData;
							else if (curLocation == MECH_BODY_LOCATION_RARM)
								actuator[ACTUATOR_RSHOULDER] = spaceData;
						}
						else if (inventory[spaceData].masterID == MasterComponent::legActuatorID)
						{
							if (curLocation == MECH_BODY_LOCATION_LLEG)
								actuator[ACTUATOR_LHIP] = spaceData;
							else if (curLocation == MECH_BODY_LOCATION_RLEG)
								actuator[ACTUATOR_RHIP] = spaceData;
						}
						break;

					case COMPONENT_FORM_ENGINE:
						engine = spaceData;
						break;

					case COMPONENT_FORM_LIFESUPPORT:
						lifeSupport = spaceData;
						break;

					case COMPONENT_FORM_GYROSCOPE:
						gyro = spaceData;
						break;

					case COMPONENT_FORM_ECM:
						ecm = spaceData;
						break;
						
					case COMPONENT_FORM_JAMMER:
						break;
						
					case COMPONENT_FORM_PROBE:
						probe = spaceData;
						break;
				}
			}
		}
	}

	calcAmmoTotals();

	for (long item = numOther; item < (numOther + numWeapons); item++) 
	{
		// Each weapon should point to its appropriate ammo total in
		// the ammo type total list...
		for (long ammoIndex = 0; ammoIndex < numAmmoTypes; ammoIndex++) 
		{
			if ((long)MasterComponent::masterList[inventory[item].masterID].getWeaponAmmoMasterId() == ammoTypeTotal[ammoIndex].masterId) 
			{
				inventory[item].ammoIndex = ammoIndex;
				break;
			}
		}
	}

	for (int item = numOther+numWeapons;item<(numOther+numWeapons+numAmmos);item++) 
	{
		// Each weapon should point to its appropriate ammo total in
		// the ammo type total list...
		for (long ammoIndex = 0; ammoIndex < numAmmoTypes; ammoIndex++) 
		{
			if (inventory[item].masterID == ammoTypeTotal[ammoIndex].masterId) 
			{
				inventory[item].ammoIndex = ammoIndex;
				break;
			}
		}
	}

	for (int item = 0; item < numOther; item++) 
	{
		if ((inventory[item].masterID == MasterComponent::clanAntiMissileSystemID) || (inventory[item].masterID == MasterComponent::innerSphereAntiMissileSystemID)) 
		{
			for (long ammoIndex = 0; ammoIndex < numAmmoTypes; ammoIndex++) {
				if ((long)MasterComponent::masterList[inventory[item].masterID].getWeaponAmmoMasterId() == ammoTypeTotal[ammoIndex].masterId) 
				{
					inventory[item].ammoIndex = ammoIndex;
					break;
				}
			}
		}
	}

	// If we added BULK to the mech, adjust the max and current armor settings.
	if (numArmorComponents)
	{
		float totalArmorPoints = ARMOR_POINTS_PER_NEWARMOR * numArmorComponents;
		
		float armorPercent = ARMOR_HEAD_PERCENT * totalArmorPoints;
		armor[MECH_ARMOR_LOCATION_HEAD].maxArmor += armorPercent;
		armor[MECH_ARMOR_LOCATION_HEAD].curArmor = armor[MECH_ARMOR_LOCATION_HEAD].maxArmor;
	
		armorPercent = ARMOR_CTORSO_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_CTORSO].maxArmor += armorPercent;
		armor[MECH_ARMOR_LOCATION_CTORSO].curArmor = armor[MECH_ARMOR_LOCATION_CTORSO].maxArmor;
	
		armorPercent = ARMOR_LTORSO_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_LTORSO].maxArmor += armorPercent;
		armor[MECH_ARMOR_LOCATION_LTORSO].curArmor = armor[MECH_ARMOR_LOCATION_LTORSO].maxArmor;
	
		armorPercent = ARMOR_RTORSO_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_RTORSO].maxArmor += armorPercent;
		armor[MECH_ARMOR_LOCATION_RTORSO].curArmor = armor[MECH_ARMOR_LOCATION_RTORSO].maxArmor;
		
		armorPercent = ARMOR_LARM_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_LARM].maxArmor += armorPercent;
		armor[MECH_ARMOR_LOCATION_LARM].curArmor = armor[MECH_ARMOR_LOCATION_LARM].maxArmor;
	
		armorPercent = ARMOR_RARM_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_RARM].maxArmor += armorPercent;
		armor[MECH_ARMOR_LOCATION_RARM].curArmor = armor[MECH_ARMOR_LOCATION_RARM].maxArmor;
	
		armorPercent = ARMOR_LLEG_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_LLEG].maxArmor += armorPercent;
		armor[MECH_ARMOR_LOCATION_LLEG].curArmor = armor[MECH_ARMOR_LOCATION_LLEG].maxArmor;
	
		armorPercent = ARMOR_RLEG_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_RLEG].maxArmor += armorPercent;
		armor[MECH_ARMOR_LOCATION_RLEG].curArmor = armor[MECH_ARMOR_LOCATION_RLEG].maxArmor;
	
		armorPercent = ARMOR_RCTORSO_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_RCTORSO].maxArmor += armorPercent;
		armor[MECH_ARMOR_LOCATION_RCTORSO].curArmor = armor[MECH_ARMOR_LOCATION_RCTORSO].maxArmor;
	
		armorPercent = ARMOR_RLTORSO_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_RLTORSO].maxArmor += armorPercent;
		armor[MECH_ARMOR_LOCATION_RLTORSO].curArmor = armor[MECH_ARMOR_LOCATION_RLTORSO].maxArmor;
	
		armorPercent = ARMOR_RRTORSO_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_RRTORSO].maxArmor += armorPercent;
		armor[MECH_ARMOR_LOCATION_RRTORSO].curArmor = armor[MECH_ARMOR_LOCATION_RRTORSO].maxArmor;
	}
	
	// We need to set the status states for legs and torso based upon our current
	// condition...
	calcLegStatus();
	calcTorsoStatus();
	calcFireRanges();

	maxCV = calcCV(true);
	curCV = calcCV(false);
	setThreatRating(-1);

	maxWeaponDamage = calcMaxTargetDamage();

	ObjectTypePtr type = ObjectManager->getObjectType(typeHandle);
	if (type->getExplosionObject() > 0)
		ObjectManager->objTypeManager->load(type->getExplosionObject(), true);
		
	// Calc mech's chassis class...
	chassisClass = getMechClass();

	// logistics data needs to be initialized if you're going to get values
	LogisticsData::instance->init();

	cBills = 10;
	LogisticsVariant* mechVariant = LogisticsData::instance->getVariant(variantName);
	if (mechVariant)
		cBills = mechVariant->getCost();

	mechFile->close();
	delete mechFile;
	mechFile = NULL;

	// UNITPROFILE-TYPELOAD-SOURCE-1: numJumpJets is final here (loadout scan
	// complete). This is the real gameplay mech-load path (init(variantNum)).
	refreshDerivedCapabilities();

	// LOADOUT-HARDPOINT-JOIN-1 (Phase-2 slice 3): report-only, gated
	// (MC2_UNIT_PROFILE_DATA). Inventory + body locations are final here. Gathers
	// any DECLARED loadout->hardpoint joins and logs coherence findings; stock
	// loadouts declare no hardpoint -> nothing gathered -> silent (gate-ON parity).
	// No runtime/visual-mount change: inventory[].facing / getWeaponNode still own
	// the actual mount (slice 4 routes it through the HardpointId).
	validateLoadoutHardpointJoins(variantName);

	return(NO_ERR);
}

typedef struct _LogisticsMasterLocationTableEntry
{
	bool		clearOut;
	long		bodyLocation;
	long		slotNumber;			//Starting at zero!
}	LogisticsMasterLocationTableEntry;

LogisticsMasterLocationTableEntry logisticsTable[MAX_MOVER_INVENTORY_ITEMS] = 
{
	false,MECH_BODY_LOCATION_CTORSO,	0,		// 0-Always Fusion Engine
	false,MECH_BODY_LOCATION_CTORSO,	1, 		// 1-Always Gyroscope
	false,MECH_BODY_LOCATION_LARM,		1,		// 2-Always Arm Actuator
	false,MECH_BODY_LOCATION_RARM,		1,		// 3-Always Arm Actuator
	false,MECH_BODY_LOCATION_LLEG,		1,		// 4-Always Leg Actuator
	false,MECH_BODY_LOCATION_RLEG,		1,		// 5-Always Leg Actuator 
	false,MECH_BODY_LOCATION_HEAD,		0,		// 6-Always COckpit
	false,MECH_BODY_LOCATION_HEAD,		1,		// 7-Always Life Support
	false,MECH_BODY_LOCATION_LARM,  	0,		// 8-Always Shoulder
	false,MECH_BODY_LOCATION_RARM,  	0,		// 9-Always Shoulder
	false,MECH_BODY_LOCATION_LLEG,  	0,      //10-Always Hip
	false,MECH_BODY_LOCATION_RLEG,  	0,      //11-Always Hip
	false,MECH_BODY_LOCATION_HEAD,  	2,      //12-Sensor									-Change back to true to be able to replace these.
	false,MECH_BODY_LOCATION_HEAD,  	3,      //13-Other Electronic Warfare Component
	false,MECH_BODY_LOCATION_HEAD,  	4,      //14-Other Electronic Warfare Component 
	false,MECH_BODY_LOCATION_HEAD,  	5,      //15-Other Electronic Warfare Component 
	true ,MECH_BODY_LOCATION_LLEG,		3,		//16-JumpJet
	false,MECH_BODY_LOCATION_LLEG,  	4,		//17-Always NOTHING
	false,MECH_BODY_LOCATION_LLEG,  	5,		//18-Always NOTHING 
	false,MECH_BODY_LOCATION_RLEG,  	3,      //19-Always NOTHING
	false,MECH_BODY_LOCATION_RLEG,  	4,      //20-Always NOTHING  
	false,MECH_BODY_LOCATION_RLEG,  	5,      //21-Always NOTHING  
	true ,MECH_BODY_LOCATION_LARM,		4,		//22-Weapon or Ammo	- 22
	true ,MECH_BODY_LOCATION_LTORSO,	0,		//38-Weapon or Ammo - 23
	true ,MECH_BODY_LOCATION_CTORSO,	2,      //62-Weapon or Ammo - 24
	true ,MECH_BODY_LOCATION_RTORSO,	0,		//50-Weapon or Ammo - 25
	true ,MECH_BODY_LOCATION_RARM,		4,		//30-Weapon or Ammo - 26
	true ,MECH_BODY_LOCATION_LARM,  	5,      //23-Weapon or Ammo - 27
	true ,MECH_BODY_LOCATION_LTORSO,	1,		//39-Weapon or Ammo - 28 
	true ,MECH_BODY_LOCATION_CTORSO,	3,      //63-Weapon or Ammo - 29 
	true ,MECH_BODY_LOCATION_RTORSO,	1,		//51-Weapon or Ammo - 30 
	true ,MECH_BODY_LOCATION_RARM,  	5,      //31-Weapon or Ammo - 31
	true ,MECH_BODY_LOCATION_LARM,  	6,      //24-Weapon or Ammo - 32
	true ,MECH_BODY_LOCATION_LTORSO,	2,      //40-Weapon or Ammo - 33 
	true ,MECH_BODY_LOCATION_CTORSO,	4,      //64-Weapon or Ammo - 34 
	true ,MECH_BODY_LOCATION_RTORSO,	2,      //52-Weapon or Ammo - 35 
	true ,MECH_BODY_LOCATION_RARM,  	6,      //32-Weapon or Ammo - 36
	true ,MECH_BODY_LOCATION_LARM,  	7,      //25-Weapon or Ammo - 37
	true ,MECH_BODY_LOCATION_LTORSO,	3,      //41-Weapon or Ammo - 38 
	true ,MECH_BODY_LOCATION_CTORSO,	5,      //65-Weapon or Ammo - 39
	true ,MECH_BODY_LOCATION_RTORSO,	3,      //53-Weapon or Ammo - 40 
	true ,MECH_BODY_LOCATION_RARM,  	7,      //33-Weapon or Ammo - 41
	true ,MECH_BODY_LOCATION_LARM,  	8,      //26-Weapon or Ammo - 42
	true ,MECH_BODY_LOCATION_LTORSO,	4,      //42-Weapon or Ammo - 43 
	true ,MECH_BODY_LOCATION_CTORSO,	6,      //66-Weapon or Ammo - 44 
	true ,MECH_BODY_LOCATION_RTORSO,	4,      //54-Weapon or Ammo - 45 
	true ,MECH_BODY_LOCATION_RARM,  	8,      //34-Weapon or Ammo - 46
	true ,MECH_BODY_LOCATION_LARM,  	9,      //27-Weapon or Ammo - 47
	true ,MECH_BODY_LOCATION_LTORSO,	5,      //43-Weapon or Ammo - 48 
	true ,MECH_BODY_LOCATION_CTORSO,	7,      //67-Weapon or Ammo - 49 
	true ,MECH_BODY_LOCATION_RTORSO,	5,      //55-Weapon or Ammo - 50 
	true ,MECH_BODY_LOCATION_RARM,  	9,      //35-Weapon or Ammo - 51
	true ,MECH_BODY_LOCATION_LARM, 		10,      //28-Weapon or Ammo- 52 
	true ,MECH_BODY_LOCATION_LTORSO,	6,      //44-Weapon or Ammo - 53 
	true ,MECH_BODY_LOCATION_CTORSO,	8,      //68-Weapon or Ammo - 54 
	true ,MECH_BODY_LOCATION_RTORSO,	6,      //56-Weapon or Ammo - 55 
	true ,MECH_BODY_LOCATION_RARM, 		10,      //36-Weapon or Ammo- 56 
	true ,MECH_BODY_LOCATION_LARM, 		11,      //29-Weapon or Ammo- 57 
	true ,MECH_BODY_LOCATION_LTORSO,	7,      //45-Weapon or Ammo - 58 
	true ,MECH_BODY_LOCATION_CTORSO,	9,      //69-Weapon or Ammo - 59 
	true ,MECH_BODY_LOCATION_RTORSO,	7,      //57-Weapon or Ammo - 60 
	true ,MECH_BODY_LOCATION_RARM, 		11,      //37-Weapon or Ammo- 61
	true ,MECH_BODY_LOCATION_LTORSO,	8,      //46-Weapon or Ammo - 62 
	true ,MECH_BODY_LOCATION_CTORSO,	10,     //70-Weapon or Ammo - 63 
	true ,MECH_BODY_LOCATION_RTORSO,	8,      //58-Weapon or Ammo - 64 
	true ,MECH_BODY_LOCATION_LTORSO,	9,      //47-Weapon or Ammo - 65 
	true ,MECH_BODY_LOCATION_CTORSO,	11,     //71-Weapon or Ammo  - 66
	true ,MECH_BODY_LOCATION_RTORSO,	9,      //59-Weapon or Ammo - 67 
	true ,MECH_BODY_LOCATION_LTORSO,	10,     //48-Weapon or Ammo - 68 
	true ,MECH_BODY_LOCATION_RTORSO,	10,     //60-Weapon or Ammo - 69 
	true ,MECH_BODY_LOCATION_LTORSO,	11,     //49-Weapon or Ammo - 70
	true ,MECH_BODY_LOCATION_RTORSO,	11     //61-Weapon or Ammo - 71
};

#define JUMPJET_SLOT					16
#define SENSOR_SLOT						12
#define ELECTRONIC_SLOT					13
#define WEAPONAMMO_SLOT					17
void BattleMech::resetComponents (long totalComponents, long *componentList)
{
	// Create a master component list for this mech.
	// Remove all of the components logistics can replace.
	// Add back in all of the components logistics replaced.
	// Put in the right body locations using the above table.
	int32_t localMasterComponentList[MAX_MOVER_INVENTORY_ITEMS];
    MemSet(localMasterComponentList, 0xff);

	//Copy current itemList to localLists
	//Remove all logistics replacable components
	for (long item = 0;item<MAX_MOVER_INVENTORY_ITEMS;item++)
	{
		localMasterComponentList[item] = inventory[item].masterID;
		if (inventory[item].masterID != 255)
		{
			long ItemLocation = -1;
			for (long i=0;i<MAX_MOVER_INVENTORY_ITEMS;i++)
			{
				if (ItemLocationToInvLocation[i] == item)
				{
					ItemLocation = i;
					break;			
				}
			}
			
			if (ItemLocation == -1)
				STOP(("Could Not Find Item %d on Mech",item));
				
			if (logisticsTable[ItemLocation].clearOut)
			{
				localMasterComponentList[item] = 0xff;
			}
		}
	}

	// Reset the weapon Node Data in the mech Appearance..�.
	appearance->resetWeaponNodes();
		
	//If we added Armor to the mech, remove the armor so that player's new armor
	// components are all that is added, adjust the max and current armor settings.
	if (numArmorComponents)
	{
		float totalArmorPoints = ARMOR_POINTS_PER_NEWARMOR * numArmorComponents;
		
		long armorPercent = ARMOR_HEAD_PERCENT * totalArmorPoints;
		armor[MECH_ARMOR_LOCATION_HEAD].maxArmor -= armorPercent;
		armor[MECH_ARMOR_LOCATION_HEAD].curArmor = armor[MECH_ARMOR_LOCATION_HEAD].maxArmor;
	
		armorPercent = ARMOR_CTORSO_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_CTORSO].maxArmor -= armorPercent;
		armor[MECH_ARMOR_LOCATION_CTORSO].curArmor = armor[MECH_ARMOR_LOCATION_CTORSO].maxArmor;
	
		armorPercent = ARMOR_LTORSO_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_LTORSO].maxArmor -= armorPercent;
		armor[MECH_ARMOR_LOCATION_LTORSO].curArmor = armor[MECH_ARMOR_LOCATION_LTORSO].maxArmor;
	
		armorPercent = ARMOR_RTORSO_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_RTORSO].maxArmor -= armorPercent;
		armor[MECH_ARMOR_LOCATION_RTORSO].curArmor = armor[MECH_ARMOR_LOCATION_RTORSO].maxArmor;
		
		armorPercent = ARMOR_LARM_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_LARM].maxArmor -= armorPercent;
		armor[MECH_ARMOR_LOCATION_LARM].curArmor = armor[MECH_ARMOR_LOCATION_LARM].maxArmor;
	
		armorPercent = ARMOR_RARM_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_RARM].maxArmor -= armorPercent;
		armor[MECH_ARMOR_LOCATION_RARM].curArmor = armor[MECH_ARMOR_LOCATION_RARM].maxArmor;
	
		armorPercent = ARMOR_LLEG_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_LLEG].maxArmor -= armorPercent;
		armor[MECH_ARMOR_LOCATION_LLEG].curArmor = armor[MECH_ARMOR_LOCATION_LLEG].maxArmor;
	
		armorPercent = ARMOR_RLEG_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_RLEG].maxArmor -= armorPercent;
		armor[MECH_ARMOR_LOCATION_RLEG].curArmor = armor[MECH_ARMOR_LOCATION_RLEG].maxArmor;
	
		armorPercent = ARMOR_RCTORSO_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_RCTORSO].maxArmor -= armorPercent;
		armor[MECH_ARMOR_LOCATION_RCTORSO].curArmor = armor[MECH_ARMOR_LOCATION_RCTORSO].maxArmor;
	
		armorPercent = ARMOR_RLTORSO_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_RLTORSO].maxArmor -= armorPercent;
		armor[MECH_ARMOR_LOCATION_RLTORSO].curArmor = armor[MECH_ARMOR_LOCATION_RLTORSO].maxArmor;
	
		armorPercent = ARMOR_RRTORSO_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_RRTORSO].maxArmor -= armorPercent;
		armor[MECH_ARMOR_LOCATION_RRTORSO].curArmor = armor[MECH_ARMOR_LOCATION_RRTORSO].maxArmor;
	}
	
 	long numNewArmors = 0;
	numJumpJets = 0;
	long numElectronics = 0;
	long numWeaponsAmmo = 0;
	
	//Add Heidi's new Components to open locations.
	for (long newItems = 0;newItems < totalComponents;newItems++)
	{
		switch (MasterComponent::masterList[componentList[newItems]].getForm())
		{
			case COMPONENT_FORM_HEATSINK:
				continue;
				break;

			case COMPONENT_FORM_ENGINE:
			{
				// An XL engine (master id 2 = XL Fusion, 3 = Clan XL) added in the
				// mech bay is an additive speed upgrade, not a real inventory
				// engine: always skip adding it to inventory (never crash), and
				// under MC2_ENGINE_XL bump move speed one class (~+20%).
				// resetComponents runs once per mech build, so it does not compound.
				if ( componentList[newItems] == 2 || componentList[newItems] == 3 )
				{
					static const bool s_engineXL = ( getenv( "MC2_ENGINE_XL" ) != nullptr );
					if ( s_engineXL )
						maxMoveSpeed *= 1.2f;
					continue;
				}
				STOP(("Invalid component added in Logistics %d",componentList[newItems]));
				break;
			}

			case COMPONENT_FORM_COCKPIT:
			case COMPONENT_FORM_LIFESUPPORT:
			case COMPONENT_FORM_GYROSCOPE:
			case COMPONENT_FORM_POWER_AMPLIFIER:
			case COMPONENT_FORM_CASE:
			case COMPONENT_FORM_ACTUATOR:
				//If any of these are in Heidi's list its REALLY BAD
				STOP(("Invalid component added in Logistics %d",componentList[newItems]));
				break;
				
			case COMPONENT_FORM_SENSOR:
				localMasterComponentList[SENSOR_SLOT] = componentList[newItems];
			break;
			
			case COMPONENT_FORM_ECM:
			case COMPONENT_FORM_PROBE:
			case COMPONENT_FORM_JAMMER:
				localMasterComponentList[ELECTRONIC_SLOT+numElectronics] = componentList[newItems];
				numElectronics++;
				if (numElectronics > 3)
					STOP(("TOO many electronic warfare components on this mech"));
			break;
			
			case COMPONENT_FORM_WEAPON:
			case COMPONENT_FORM_WEAPON_ENERGY:
			case COMPONENT_FORM_WEAPON_BALLISTIC:
			case COMPONENT_FORM_WEAPON_MISSILE:
			case COMPONENT_FORM_AMMO:
				localMasterComponentList[WEAPONAMMO_SLOT+numWeaponsAmmo] = componentList[newItems];
				numWeaponsAmmo++;
				
				//Heidi does not give me ammo.  Add manually here.
				if (MasterComponent::masterList[componentList[newItems]].getWeaponAmmoMasterId())
				{
					localMasterComponentList[WEAPONAMMO_SLOT+numWeaponsAmmo] = MasterComponent::masterList[componentList[newItems]].getWeaponAmmoMasterId();
					numWeaponsAmmo++;
				}
				
				if (numWeaponsAmmo > 54)
					STOP(("TOO many weapons and/or Ammo components on this mech"));
				
			break;
			
			case COMPONENT_FORM_BULK:
				numNewArmors++;
			break;			
			
			case COMPONENT_FORM_JUMPJET:
				localMasterComponentList[JUMPJET_SLOT] = componentList[newItems];
			break;			
		}
	}
	
	//Now use the above lists to re-component out the mech!
	numAntiMissileSystems = numOther = numWeapons = numAmmos = 0;

	// Read in the mech's non-weapon/non-ammo components...
	long realItemNum = 0;
	MemSet(ItemLocationToInvLocation, 0xff);

	//Read in everything but weapons and AMMO
	for (long curItem = 0;curItem < MAX_MOVER_INVENTORY_ITEMS;curItem++)
	{
		inventory[realItemNum].masterID = localMasterComponentList[curItem];
		
		if (inventory[realItemNum].masterID && (inventory[realItemNum].masterID != 255))
		{
			// If the component is a JumpJet, increment our jump jet count...
			switch (MasterComponent::masterList[inventory[realItemNum].masterID].getForm()) 
			{
				case COMPONENT_FORM_JUMPJET:
					numJumpJets = 5;
				case COMPONENT_FORM_COCKPIT:
				case COMPONENT_FORM_SENSOR:
				case COMPONENT_FORM_LIFESUPPORT:
				case COMPONENT_FORM_GYROSCOPE:
				case COMPONENT_FORM_POWER_AMPLIFIER:
				case COMPONENT_FORM_ECM:
				case COMPONENT_FORM_PROBE:
				case COMPONENT_FORM_JAMMER:
				case COMPONENT_FORM_BULK:
				case COMPONENT_FORM_CASE:
				case COMPONENT_FORM_ACTUATOR:
				case COMPONENT_FORM_ENGINE:
				case COMPONENT_FORM_HEATSINK:
					numOther++;
					inventory[realItemNum].health = MasterComponent::masterList[inventory[realItemNum].masterID].getHealth();
					inventory[realItemNum].disabled = false;
					inventory[realItemNum].amount = 1;
					inventory[realItemNum].ammoIndex = -1;
					inventory[realItemNum].readyTime = 0.0;
					inventory[realItemNum].bodyLocation = 255;
					ItemLocationToInvLocation[curItem] = realItemNum;
					realItemNum++;
					break;
				
				case COMPONENT_FORM_WEAPON:
				case COMPONENT_FORM_WEAPON_ENERGY:
				case COMPONENT_FORM_WEAPON_BALLISTIC:
				case COMPONENT_FORM_WEAPON_MISSILE:
					//Ignore weapons until second pass
					break;
					
				case COMPONENT_FORM_AMMO:
					//Ignore AMMO until third pass
					break;
			}
		}
	}

	//NOW read in the weapons.
	for (int curItem = 0;curItem < MAX_MOVER_INVENTORY_ITEMS;curItem++)
	{
		inventory[realItemNum].masterID = localMasterComponentList[curItem];
		
		if (inventory[realItemNum].masterID && (inventory[realItemNum].masterID != 255))
		{
			// If the component is a JumpJet, increment our jump jet count...
			long sourceWeaponType;
			if (MasterComponent::masterList[inventory[realItemNum].masterID].getForm() == COMPONENT_FORM_WEAPON_MISSILE)
				sourceWeaponType = MECH3D_WEAPONTYPE_MISSILE;
			else if (MasterComponent::masterList[inventory[realItemNum].masterID].getForm() == COMPONENT_FORM_WEAPON_BALLISTIC)
				sourceWeaponType = MECH3D_WEAPONTYPE_BALLISTIC;
			else if (MasterComponent::masterList[inventory[realItemNum].masterID].getForm() == COMPONENT_FORM_WEAPON_ENERGY) 
				sourceWeaponType = MECH3D_WEAPONTYPE_ENERGY;
			else
				sourceWeaponType = MECH3D_WEAPONTYPE_ANY;
			// If the component is a JumpJet, increment our jump jet count...
			switch (MasterComponent::masterList[inventory[realItemNum].masterID].getForm()) 
			{
				case COMPONENT_FORM_JUMPJET:
				case COMPONENT_FORM_COCKPIT:
				case COMPONENT_FORM_SENSOR:
				case COMPONENT_FORM_LIFESUPPORT:
				case COMPONENT_FORM_GYROSCOPE:
				case COMPONENT_FORM_POWER_AMPLIFIER:
				case COMPONENT_FORM_ECM:
				case COMPONENT_FORM_PROBE:
				case COMPONENT_FORM_JAMMER:
				case COMPONENT_FORM_BULK:
				case COMPONENT_FORM_CASE:
				case COMPONENT_FORM_ACTUATOR:
				case COMPONENT_FORM_ENGINE:
				case COMPONENT_FORM_HEATSINK:
					//We did these Above here.
					break;
				
				case COMPONENT_FORM_WEAPON:
				case COMPONENT_FORM_WEAPON_ENERGY:
				case COMPONENT_FORM_WEAPON_BALLISTIC:
				case COMPONENT_FORM_WEAPON_MISSILE:
					numWeapons++;
								
					inventory[realItemNum].health = MasterComponent::masterList[inventory[realItemNum].masterID].getHealth();
					inventory[realItemNum].disabled = false;
					inventory[realItemNum].amount = 1;
					inventory[realItemNum].ammoIndex = -1;
					// GETWEAPONNODE-HARDPOINT-ADAPTER-1 (Phase-2 slice 4): route the
					// weapon-node mount through the item's declared HardpointId when one
					// resolves to a real model socket; else the legacy least-used
					// round-robin. Stock items carry hardpointId==0 -> always fallback ->
					// byte-identical mount (parity). Returns the same node-index domain.
					inventory[realItemNum].facing = (unsigned char)resolveWeaponMountNode(inventory[realItemNum], sourceWeaponType);
					inventory[realItemNum].readyTime = 0.0;
					inventory[realItemNum].bodyLocation = 255;
					inventory[realItemNum].effectiveness = (short)(MasterComponent::masterList[inventory[realItemNum].masterID].getWeaponDamage() * 10.0 /	// damage over 10 seconds
													 MasterComponent::masterList[inventory[realItemNum].masterID].getWeaponRecycleTime());
					inventory[realItemNum].effectiveness *= WeaponRanges[MasterComponent::masterList[inventory[realItemNum].masterID].getWeaponRange()][1] / 24;
					ItemLocationToInvLocation[curItem] = realItemNum;
					realItemNum++;
					// Cache in the weapon special effect.
					
					break;
					
				case COMPONENT_FORM_AMMO:
					//Do these next.
					break;
			}
		}
	}

	//NOW read in the AMMO
	for (int curItem = 0;curItem < MAX_MOVER_INVENTORY_ITEMS;curItem++)
	{
		inventory[realItemNum].masterID = localMasterComponentList[curItem];
		
		if (inventory[realItemNum].masterID && (inventory[realItemNum].masterID != 255))
		{
			// If the component is a JumpJet, increment our jump jet count...
			switch (MasterComponent::masterList[inventory[realItemNum].masterID].getForm()) 
			{
				case COMPONENT_FORM_JUMPJET:
				case COMPONENT_FORM_COCKPIT:
				case COMPONENT_FORM_SENSOR:
				case COMPONENT_FORM_LIFESUPPORT:
				case COMPONENT_FORM_GYROSCOPE:
				case COMPONENT_FORM_POWER_AMPLIFIER:
				case COMPONENT_FORM_ECM:
				case COMPONENT_FORM_PROBE:
				case COMPONENT_FORM_JAMMER:
				case COMPONENT_FORM_BULK:
				case COMPONENT_FORM_CASE:
				case COMPONENT_FORM_ACTUATOR:
				case COMPONENT_FORM_ENGINE:
				case COMPONENT_FORM_HEATSINK:
					//Did these above.
					break;
				
				case COMPONENT_FORM_WEAPON:
				case COMPONENT_FORM_WEAPON_ENERGY:
				case COMPONENT_FORM_WEAPON_BALLISTIC:
				case COMPONENT_FORM_WEAPON_MISSILE:
					//Did These Above, too...
					break;
					
				case COMPONENT_FORM_AMMO:
					numAmmos++;
					inventory[realItemNum].health = MasterComponent::masterList[inventory[realItemNum].masterID].getHealth();
					inventory[realItemNum].disabled = false;
					inventory[realItemNum].amount = 1;
					inventory[realItemNum].ammoIndex = -1;
					inventory[realItemNum].readyTime = 0.0;
					inventory[realItemNum].bodyLocation = 255;
		
					// Since the ammo amount in the profile is amount per ton, let's make the
					// amount equal to the number of missiles/bullets/whatever. If it's set
					// to 255, use the default amount per ton as defined in the component
					// table...
					// Ammo per ton is always fully stocked.  I.e. always -1 or 255
					inventory[realItemNum].amount = MasterComponent::masterList[inventory[realItemNum].masterID].getAmmoPerTon();
					inventory[realItemNum].ammoIndex = -1;
					inventory[realItemNum].startAmount = inventory[realItemNum].amount;
					inventory[realItemNum].health = MasterComponent::masterList[inventory[realItemNum].masterID].getHealth();
					inventory[realItemNum].disabled = false;
					inventory[realItemNum].readyTime = 0.0;
					inventory[realItemNum].bodyLocation = 255;

					ItemLocationToInvLocation[curItem] = realItemNum;
					realItemNum++;
					break;
			}
		}
	}

	// If we added BULK to the mech, adjust the max and current armor settings.
	if (numNewArmors)
	{
		float totalArmorPoints = ARMOR_POINTS_PER_NEWARMOR * numNewArmors;
		
		float armorPercent = ARMOR_HEAD_PERCENT * totalArmorPoints;
		armor[MECH_ARMOR_LOCATION_HEAD].maxArmor += armorPercent;
		armor[MECH_ARMOR_LOCATION_HEAD].curArmor = armor[MECH_ARMOR_LOCATION_HEAD].maxArmor;
	
		armorPercent = ARMOR_CTORSO_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_CTORSO].maxArmor += armorPercent;
		armor[MECH_ARMOR_LOCATION_CTORSO].curArmor = armor[MECH_ARMOR_LOCATION_CTORSO].maxArmor;
	
		armorPercent = ARMOR_LTORSO_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_LTORSO].maxArmor += armorPercent;
		armor[MECH_ARMOR_LOCATION_LTORSO].curArmor = armor[MECH_ARMOR_LOCATION_LTORSO].maxArmor;
	
		armorPercent = ARMOR_RTORSO_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_RTORSO].maxArmor += armorPercent;
		armor[MECH_ARMOR_LOCATION_RTORSO].curArmor = armor[MECH_ARMOR_LOCATION_RTORSO].maxArmor;
		
		armorPercent = ARMOR_LARM_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_LARM].maxArmor += armorPercent;
		armor[MECH_ARMOR_LOCATION_LARM].curArmor = armor[MECH_ARMOR_LOCATION_LARM].maxArmor;
	
		armorPercent = ARMOR_RARM_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_RARM].maxArmor += armorPercent;
		armor[MECH_ARMOR_LOCATION_RARM].curArmor = armor[MECH_ARMOR_LOCATION_RARM].maxArmor;
	
		armorPercent = ARMOR_LLEG_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_LLEG].maxArmor += armorPercent;
		armor[MECH_ARMOR_LOCATION_LLEG].curArmor = armor[MECH_ARMOR_LOCATION_LLEG].maxArmor;
	
		armorPercent = ARMOR_RLEG_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_RLEG].maxArmor += armorPercent;
		armor[MECH_ARMOR_LOCATION_RLEG].curArmor = armor[MECH_ARMOR_LOCATION_RLEG].maxArmor;
	
		armorPercent = ARMOR_RCTORSO_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_RCTORSO].maxArmor += armorPercent;
		armor[MECH_ARMOR_LOCATION_RCTORSO].curArmor = armor[MECH_ARMOR_LOCATION_RCTORSO].maxArmor;
	
		armorPercent = ARMOR_RLTORSO_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_RLTORSO].maxArmor += armorPercent;
		armor[MECH_ARMOR_LOCATION_RLTORSO].curArmor = armor[MECH_ARMOR_LOCATION_RLTORSO].maxArmor;
	
		armorPercent = ARMOR_RRTORSO_PERCENT * totalArmorPoints; 
		armor[MECH_ARMOR_LOCATION_RRTORSO].maxArmor += armorPercent;
		armor[MECH_ARMOR_LOCATION_RRTORSO].curArmor = armor[MECH_ARMOR_LOCATION_RRTORSO].maxArmor;
	}
	
	// Reset the quick Access components in case they removed them in logistics.
	// SHould only need to do jump jets!!
	jumpJets = 255;

	// Now, read in the component layout for each body location...
	for (long curLocation = 0; curLocation < NUM_MECH_BODY_LOCATIONS; curLocation++) 
	{
		body[curLocation].CASE = true;	//ALL MC2 Mechs have CASE everywhere.

		//Internal Structure is ALREADY set!
		
		// Damage and states are already set.
		
		long numSpaces = NumLocationCriticalSpaces[curLocation];
		body[curLocation].totalSpaces = 0;

		for (long curSpace = 0; curSpace < numSpaces; curSpace++)
		{
			//Find out where in the space table this location lies.
			unsigned char spaceData = 0xff;
			for (long curTable = 0;curTable < MAX_MOVER_INVENTORY_ITEMS;curTable++)
			{
				if ((logisticsTable[curTable].bodyLocation == curLocation) &&
					(logisticsTable[curTable].slotNumber == curSpace))
				{
					spaceData = curTable;
					break;
				}
			}

			gosASSERT(spaceData < MAX_MOVER_INVENTORY_ITEMS || spaceData == 0xff);
			//sebi:
			if (spaceData >= MAX_MOVER_INVENTORY_ITEMS)
			{
				body[curLocation].criticalSpaces[curSpace].inventoryID = 0xff;
				body[curLocation].criticalSpaces[curSpace].hit = false;				//Everything always repaired now.
				continue;
			}
			
			spaceData = ItemLocationToInvLocation[spaceData];
			
			body[curLocation].criticalSpaces[curSpace].inventoryID = spaceData;
			body[curLocation].criticalSpaces[curSpace].hit = false;				//Everything always repaired now.
			
			if (spaceData < 255)
			{
				if (spaceData >= numOther+numWeapons+numAmmos)
					//Just ignore this space.  Sage has filled them all in now.
					// ACTUALLY, we should stop here.
					continue;
					
				inventory[spaceData].bodyLocation = curLocation;

				// The following line assumes the "new" crit hit system...
				body[curLocation].totalSpaces += MasterComponent::masterList[inventory[spaceData].masterID].getSize();
				
				// Preserve critical component indices for quick access...
				switch (MasterComponent::masterList[inventory[spaceData].masterID].getForm())
				{
					case COMPONENT_FORM_COCKPIT:
						cockpit = spaceData;
						break;

					case COMPONENT_FORM_JUMPJET:
						jumpJets = spaceData;
						break;

					case COMPONENT_FORM_SENSOR:
						sensor = spaceData;
						//Sensor already defined in the first component pass.
						// Not possible to replace sensors or EW components in logistics now!!
						/*
						sensorSystem = SensorManager->newSensor();
						sensorSystem->setOwner(this);
						sensorSystem->setRange(MasterComponent::masterList[inventory[sensor].masterID].getSensorRange());
						*/
						break;

					case COMPONENT_FORM_HEATSINK:
						inventory[spaceData].bodyLocation = curLocation;
						break;

					case COMPONENT_FORM_AMMO:
						inventory[spaceData].bodyLocation = curLocation;
						break;

					case COMPONENT_FORM_WEAPON:
					case COMPONENT_FORM_WEAPON_ENERGY:
						inventory[spaceData].bodyLocation = curLocation;
						break;

					case COMPONENT_FORM_WEAPON_BALLISTIC:
						inventory[spaceData].bodyLocation = curLocation;
						if ((inventory[spaceData].masterID == MasterComponent::clanAntiMissileSystemID) ||
							(inventory[spaceData].masterID == MasterComponent::innerSphereAntiMissileSystemID))
						{
							// Add to Anti-Missile System list for fast reference...
							if (numAntiMissileSystems == MAX_ANTI_MISSILE_SYSTEMS)
								Fatal(0, "Too many Anti-Missile Systems");
							antiMissileSystem[numAntiMissileSystems++] = spaceData;
						}
						break;

					case COMPONENT_FORM_WEAPON_MISSILE:
						inventory[spaceData].bodyLocation = curLocation;
						break;

					case COMPONENT_FORM_ACTUATOR:
						if (inventory[spaceData].masterID == MasterComponent::armActuatorID)
						{
							if (curLocation == MECH_BODY_LOCATION_LARM)
								actuator[ACTUATOR_LSHOULDER] = spaceData;
							else if (curLocation == MECH_BODY_LOCATION_RARM)
								actuator[ACTUATOR_RSHOULDER] = spaceData;
						}
						else if (inventory[spaceData].masterID == MasterComponent::legActuatorID)
						{
							if (curLocation == MECH_BODY_LOCATION_LLEG)
								actuator[ACTUATOR_LHIP] = spaceData;
							else if (curLocation == MECH_BODY_LOCATION_RLEG)
								actuator[ACTUATOR_RHIP] = spaceData;
						}
						break;

					case COMPONENT_FORM_ENGINE:
						engine = spaceData;
						break;

					case COMPONENT_FORM_LIFESUPPORT:
						lifeSupport = spaceData;
						break;

					case COMPONENT_FORM_GYROSCOPE:
						gyro = spaceData;
						break;

					case COMPONENT_FORM_ECM:
						ecm = spaceData;
						break;
						
					case COMPONENT_FORM_JAMMER:
						break;
						
					case COMPONENT_FORM_PROBE:
						probe = spaceData;
						break;
				}
			}
		}
	}

	calcAmmoTotals();

	for (int item = numOther; item < (numOther + numWeapons); item++) 
	{
		// Each weapon should point to its appropriate ammo total in
		// the ammo type total list...
		for (long ammoIndex = 0; ammoIndex < numAmmoTypes; ammoIndex++) 
		{
			if ((long)MasterComponent::masterList[inventory[item].masterID].getWeaponAmmoMasterId() == ammoTypeTotal[ammoIndex].masterId) 
			{
				inventory[item].ammoIndex = ammoIndex;
				break;
			}
		}
	}

	for (int item = numOther+numWeapons;item<(numOther+numWeapons+numAmmos);item++) 
	{
		// Each weapon should point to its appropriate ammo total in
		// the ammo type total list...
		for (long ammoIndex = 0; ammoIndex < numAmmoTypes; ammoIndex++) 
		{
			if (inventory[item].masterID == ammoTypeTotal[ammoIndex].masterId) 
			{
				inventory[item].ammoIndex = ammoIndex;
				break;
			}
		}
	}

	for (int item = 0; item < numOther; item++) 
	{
		if ((inventory[item].masterID == MasterComponent::clanAntiMissileSystemID) || (inventory[item].masterID == MasterComponent::innerSphereAntiMissileSystemID)) 
		{
			for (long ammoIndex = 0; ammoIndex < numAmmoTypes; ammoIndex++) {
				if ((long)MasterComponent::masterList[inventory[item].masterID].getWeaponAmmoMasterId() == ammoTypeTotal[ammoIndex].masterId) 
				{
					inventory[item].ammoIndex = ammoIndex;
					break;
				}
			}
		}
	}

	// We need to set the status states for legs and torso based upon our current
	// condition...
	calcFireRanges();

	maxCV = calcCV(true);
	curCV = calcCV(false);
	setThreatRating(-1);

	maxWeaponDamage = calcMaxTargetDamage();

	// local variable needs to be set for class
    //sebi: no really...
	// should set it to zero ?
	//numJumpJets = numJumpJets;
}
