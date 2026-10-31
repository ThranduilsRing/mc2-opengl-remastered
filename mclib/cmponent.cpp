//******************************************************************************************
//
//	cmponent.cp - This file contains the Component Class
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//******************************************************************************************
#ifndef CMPONENT_H
#include"cmponent.h"
#endif

#include <new>

#ifndef HEAP_H
#include"heap.h"
#endif

#include "platform_str.h"

#include"utilities.h"
#include "mc2format.h"
//******************************************************************************************

const char* ComponentFormString [] = {
	"Simple",
	"Cockpit",
	"Sensors",
	"Actuator",
	"Engine",
	"HeatSink",
	"Weapon",
	"EnergyWeapon",
	"BallisticWeapon",
	"MissileWeapon",
	"Ammo",
	"JumpJet",
	"Case",
	"LifeSupport",
	"Gyroscope",
	"PowerAmplifier",
	"ECM",
	"Probe",
	"Jammer",
	"Bulk",
    nullptr
};

const char* WeaponRangeString[] = {
	"short",
	"medium",
	"long"
};

MasterComponentPtr		MasterComponent::masterList = nullptr;
long					MasterComponent::numComponents = 0;
long					MasterComponent::armActuatorID = 4;
long					MasterComponent::legActuatorID = 5;
long					MasterComponent::clanAntiMissileSystemID = 115;
long					MasterComponent::innerSphereAntiMissileSystemID = 106;

constexpr int COMPONENT_NAME_START = 32000;
constexpr int COMPONENT_ABBR_START = 33000;
//******************************************************************************************
// MASTER COMPONENT routines
//******************************************************************************************



//******************************************************************************************



//******************************************************************************************

void MasterComponent::destroy (void) {
}

//******************************************************************************************
long MasterComponent::initEXCEL (char* dataLine, float baseSensorRange) {

	//----------------------------------------------------------
	// Component data was read in, so parse it. First, parse the
	// fields common to all components...
	char* field = strtok(dataLine, ",");

	int ammoAmount = 1;

	health = 1;

	masterID = atoi(field);

	cLoadString(COMPONENT_NAME_START+masterID,name,MAXLEN_COMPONENT_NAME);
	cLoadString(COMPONENT_ABBR_START+masterID,abbreviation,MAXLEN_COMPONENT_ABBREV);
	
	field = strtok(nullptr, ",");
	long formIndex;
	for (formIndex = 0; ComponentFormString[formIndex] != nullptr; formIndex++)
		if (strcmp(field, ComponentFormString[formIndex]) == 0)
			break;
	if (ComponentFormString[formIndex] == nullptr)
		return(-1);

	if (strcmp(field, "undefined") == 0) {
		masterID = -1;
		return(NO_ERR);
	}

	form = static_cast<ComponentFormType>(formIndex);
	
	field = strtok(nullptr, ","); // name, ignore

	field = strtok(nullptr, ",");
	size = atof(field);

	field = strtok(nullptr, ",");
	float recycleTime = atof(field);
	
	field = strtok(nullptr, ",");
	float heat = static_cast<unsigned long>(atof(field));

	field = strtok(nullptr, ","); 
	tonnage = atof( field );

	field = strtok(nullptr, ",");
	float damage = atof(field);

	field = strtok(nullptr, ",");
	CV = atof(field);
	
	field = strtok(nullptr, ",");
	resourcePoints = atoi(field);
	
	field = strtok(nullptr, ",");
	unsigned char rangeType = 255;
	S_strlwr(field);
	if (strcmp(field, "0") != 0) {
		if (strcmp(field, WeaponRangeString[WEAPON_RANGE_SHORT]) == 0)
			rangeType = WEAPON_RANGE_SHORT;
		else if (strcmp(field, WeaponRangeString[WEAPON_RANGE_MEDIUM]) == 0)
			rangeType = WEAPON_RANGE_MEDIUM;
		else if (strcmp(field, WeaponRangeString[WEAPON_RANGE_LONG]) == 0)
			rangeType = WEAPON_RANGE_LONG;
		else
			Fatal(0, " MasterComponent.initEXCEL: bad weapon range type in compbase ");
	}

	field = strtok(nullptr, ",");
	for (long location = 0; location < NUM_BODY_LOCATIONS; location++) 
	{
		if ( field )
		{
			if (strcmp(field, "No") == 0)
				criticalSpacesReq[location] = -1;
			else if (strcmp(field, "Yes") == 0)
				criticalSpacesReq[location] = 0;
			else
				criticalSpacesReq[location] = atoi(field);
		}
			field = strtok(nullptr, ",");

	}

	int ammoType = WEAPON_AMMO_NONE;
	if ( field )
	{
		if (strcmp(field, "1") == 0)
			ammoType = WEAPON_AMMO_LIMITED;
		else if (strcmp(field, "SRM") == 0)
			ammoType = WEAPON_AMMO_SRM;
		else if (strcmp(field, "LRM") == 0)
			ammoType = WEAPON_AMMO_LRM;
		else if (strcmp(field, "ST") == 0)
			ammoType = WEAPON_AMMO_ST;
	}

	field = strtok( nullptr, "," );
	int flags = 0;
	if ( field )
		flags = atoi(field);
	
	field = strtok(nullptr, ",");
	int specialEffect = 0;
	if ( field )
		specialEffect = static_cast<char>(atoi(field));
	
	field = strtok(nullptr, ",");
	int ammoMasterId = 0;
	if ( field )
		ammoMasterId = static_cast<char>(atoi(field));
	
	switch (form) {
		case COMPONENT_FORM_SIMPLE:
			//----------------------
			// No additional data...
			break;
		case COMPONENT_FORM_COCKPIT:
			break;
		case COMPONENT_FORM_SENSOR:
			stats.sensor.range = damage * baseSensorRange;
			break;
		case COMPONENT_FORM_ECM:
			stats.ecm.effect = damage;
			field = strtok(nullptr, ",");
			stats.ecm.range = recycleTime;
			break;
		case COMPONENT_FORM_JAMMER:
			stats.jammer.effect = damage;
			break;
		case COMPONENT_FORM_PROBE:
			stats.probe.effect = damage;
			break;
		case COMPONENT_FORM_ACTUATOR:
			break;
		case COMPONENT_FORM_ENGINE:
			stats.engine.rating = 0;
			break;
		case COMPONENT_FORM_HEATSINK:
			stats.heatsink.dissipation = damage;
			break;
		case COMPONENT_FORM_WEAPON_ENERGY:
		case COMPONENT_FORM_WEAPON_MISSILE:
		case COMPONENT_FORM_WEAPON_BALLISTIC:
			stats.weapon.damage = damage;
			stats.weapon.recycleTime = recycleTime;
			stats.weapon.heat = heat;
			stats.weapon.ammoAmount = 1;
			stats.weapon.ammoType = ammoType;
			stats.weapon.range = rangeType;
			stats.weapon.specialEffect = specialEffect;
			stats.weapon.ammoMasterId = ammoMasterId;
			stats.weapon.flags = ammoType == WEAPON_AMMO_ST ? WEAPON_FLAG_STREAK : 0;
			stats.weapon.ammoAmount = ammoAmount;
			break;
		case COMPONENT_FORM_AMMO:
			stats.ammo.ammoPerTon = recycleTime;
			stats.ammo.explosiveDamage = damage;
			break;
		case COMPONENT_FORM_JUMPJET:
			stats.jumpjet.rangeMod = damage;
			break;
		case COMPONENT_FORM_CASE:
			break;
		case COMPONENT_FORM_LIFESUPPORT:
			break;
		case COMPONENT_FORM_GYROSCOPE:
			break;
		case COMPONENT_FORM_POWER_AMPLIFIER:
			break;
		case COMPONENT_FORM_BULK:
			break;
		default:
			return(-2);
	}

	return(NO_ERR);
}

//******************************************************************************************
long MasterComponent::saveEXCEL (FilePtr componentFile, unsigned char masterId, float baseSensorRange) 
{
	char dataLine[512];
	char piece[512];
	char comma[2] = ",";

	if (masterID == -1)
	{
		//---------------------------------------
		// Blank undefined line in compbas here.
		mc2::fmt::to_buf(dataLine,"0,undefined,undefined,0,0,0,0,0,0,0,0,0,0,0,0,No,No,0,No,0,0,0,0,na,na,na,na,na,na,na,na,na,,,,,");
		componentFile->writeLine(dataLine);
		return(0);
	}

	//----------------------------------------------------------
	// Build the dataline piece by piece
	mc2::fmt::to_buf(piece,"{:d}",mc2::fmt::as_i32(masterId));
	strcpy(dataLine,piece);
	strcat(dataLine,comma);

	cLoadString(COMPONENT_NAME_START+masterID,name,MAXLEN_COMPONENT_NAME);
	strcat(dataLine,name);
	strcat(dataLine,comma);

	cLoadString(COMPONENT_ABBR_START+masterID,abbreviation,MAXLEN_COMPONENT_ABBREV);
	strcat(dataLine,abbreviation);
	strcat(dataLine,comma);

	strcat(dataLine,ComponentFormString[form]);
	strcat(dataLine,comma);

	mc2::fmt::to_buf(piece,"{:d}",mc2::fmt::as_i32(size));
	strcat(dataLine,piece);
	strcat(dataLine,comma);

	mc2::fmt::to_buf(piece,"1");
	strcat(dataLine,piece);
	strcat(dataLine,comma);

	mc2::fmt::to_buf(piece,"{:3.1f}",tonnage);
	strcat(dataLine,piece);
	strcat(dataLine,comma);

	mc2::fmt::to_buf(piece,"{:d}",mc2::fmt::as_i32(resourcePoints));
	strcat(dataLine,piece);
	strcat(dataLine,comma);

	for (long location = 0; location < NUM_BODY_LOCATIONS; location++) 
	{
		if (criticalSpacesReq[location] == -1)
			mc2::fmt::to_buf(piece,"No");
		else if (criticalSpacesReq[location] == 0)
			mc2::fmt::to_buf(piece,"Yes");
		else
			Fatal(criticalSpacesReq[location],"Bad Data");

		strcat(dataLine,piece);
		strcat(dataLine,comma);
	}

	if (getCanVehicleUse())
		mc2::fmt::to_buf(piece,"Yes");
	else
		mc2::fmt::to_buf(piece,"No");

	strcat(dataLine,piece);
	strcat(dataLine,comma);

	if (getCanMechUse())
		mc2::fmt::to_buf(piece,"Yes");
	else
		mc2::fmt::to_buf(piece,"No");

	strcat(dataLine,piece);
	strcat(dataLine,comma);

	if (getClanTechBase() && getISTechBase())
	{
		mc2::fmt::to_buf(piece,"Both");
	}
	else if (getClanTechBase())
	{
		mc2::fmt::to_buf(piece,"Clan");
	}
	else if (getISTechBase())
	{
		mc2::fmt::to_buf(piece,"IS");
	}
	else
	{
		mc2::fmt::to_buf(piece,"0");
	}

	strcat(dataLine,piece);
	strcat(dataLine,comma);

	if (getCanISUse())
		mc2::fmt::to_buf(piece,"Yes");
	else
		mc2::fmt::to_buf(piece,"No");

	strcat(dataLine,piece);
	strcat(dataLine,comma);

	mc2::fmt::to_buf(piece,"0");
	strcat(dataLine,piece);
	strcat(dataLine,comma);

	mc2::fmt::to_buf(piece,"1");
	strcat(dataLine,piece);
	strcat(dataLine,comma);

	mc2::fmt::to_buf(piece,"{:8.1f}",CV);
	strcat(dataLine,piece);
	strcat(dataLine,comma);

	switch (form) 
	{
		case COMPONENT_FORM_COCKPIT:
		case COMPONENT_FORM_ACTUATOR:
		case COMPONENT_FORM_SIMPLE:
		case COMPONENT_FORM_CASE:
		case COMPONENT_FORM_POWER_AMPLIFIER:
		case COMPONENT_FORM_GYROSCOPE:
		case COMPONENT_FORM_LIFESUPPORT:
		case COMPONENT_FORM_BULK:
		default:
			//----------------------
			// No additional data...
			mc2::fmt::to_buf(piece,"na,na,na,na,na,na,na,na,na,,,,,");
			strcat(dataLine,piece);
			strcat(dataLine,comma);
			break;

		case COMPONENT_FORM_SENSOR:
			mc2::fmt::to_buf(piece,"{:4.1f}",(stats.sensor.range / baseSensorRange));
			strcat(dataLine,piece);
			strcat(dataLine,comma);

			mc2::fmt::to_buf(piece,"na,na,na,na,na,na,na,na,,,,,");
			strcat(dataLine,piece);
			strcat(dataLine,comma);
			break;

		case COMPONENT_FORM_ECM:
			mc2::fmt::to_buf(piece,"{:4.1f}",stats.ecm.effect);
			strcat(dataLine,piece);
			strcat(dataLine,comma);
			mc2::fmt::to_buf(piece,"{:4.1f}",stats.ecm.range);
			strcat(dataLine,piece);
			strcat(dataLine,comma);
			mc2::fmt::to_buf(piece,"na,na,na,na,na,na,na,,,,,");
			strcat(dataLine,piece);
			strcat(dataLine,comma);
			break;

		case COMPONENT_FORM_JAMMER:
			mc2::fmt::to_buf(piece,"{:4.1f}",stats.jammer.effect);
			strcat(dataLine,piece);
			strcat(dataLine,comma);
			mc2::fmt::to_buf(piece,"na,na,na,na,na,na,na,na,,,,,");
			strcat(dataLine,piece);
			strcat(dataLine,comma);
			break;

		case COMPONENT_FORM_PROBE:
			mc2::fmt::to_buf(piece,"{:4.1f}",stats.probe.effect);
			strcat(dataLine,piece);
			strcat(dataLine,comma);
			mc2::fmt::to_buf(piece,"na,na,na,na,na,na,na,na,,,,,");
			strcat(dataLine,piece);
			strcat(dataLine,comma);
			break;

		case COMPONENT_FORM_ENGINE:
			mc2::fmt::to_buf(piece,"{:d}",mc2::fmt::as_i32(0));
			strcat(dataLine,piece);
			strcat(dataLine,comma);
			mc2::fmt::to_buf(piece,"na,na,na,na,na,na,na,na,,,,,");
			strcat(dataLine,piece);
			strcat(dataLine,comma);
			break;

		case COMPONENT_FORM_HEATSINK:
			mc2::fmt::to_buf(piece,"{:d}",mc2::fmt::as_i32(stats.heatsink.dissipation));
			strcat(dataLine,piece);
			strcat(dataLine,comma);
			mc2::fmt::to_buf(piece,"na,na,na,na,na,na,na,na,,,,,");
			strcat(dataLine,piece);
			strcat(dataLine,comma);
			break;

		case COMPONENT_FORM_WEAPON_ENERGY:
			mc2::fmt::to_buf(piece,"{:4.1f}",stats.weapon.damage);
			strcat(dataLine,piece);
			strcat(dataLine,comma);

			mc2::fmt::to_buf(piece,"{:4.2f}",stats.weapon.recycleTime);
			strcat(dataLine,piece);
			strcat(dataLine,comma);

			mc2::fmt::to_buf(piece,"{:4.1f}",stats.weapon.heat);
			strcat(dataLine,piece);
			strcat(dataLine,comma);

			mc2::fmt::to_buf(piece,"{:d}",mc2::fmt::as_i32(0));
			strcat(dataLine,piece);
			strcat(dataLine,comma);

			mc2::fmt::to_buf(piece,"{:d}",mc2::fmt::as_i32(WEAPON_AMMO_NONE));
			strcat(dataLine,piece);
			strcat(dataLine,comma);

			mc2::fmt::to_buf(piece,"{}",mc2::fmt::safe(WeaponRangeString[stats.weapon.range]));
			strcat(dataLine,piece);
			strcat(dataLine,comma);
			
			//sprintf(piece,"%d",stats.weapon.type);
			//strcat(dataLine,piece);
			//strcat(dataLine,comma);

			mc2::fmt::to_buf(piece,"{:d}",mc2::fmt::as_i32(stats.weapon.specialEffect));
			strcat(dataLine,piece);
			strcat(dataLine,comma);

			mc2::fmt::to_buf(piece,"{:d}",mc2::fmt::as_i32(0));
			strcat(dataLine,piece);
			strcat(dataLine,comma);

			mc2::fmt::to_buf(piece,"{:d}",mc2::fmt::as_i32(stats.weapon.flags));
			strcat(dataLine,piece);
			strcat(dataLine,comma);
			break;

		case COMPONENT_FORM_WEAPON_BALLISTIC:
			mc2::fmt::to_buf(piece,"{:4.1f}",stats.weapon.damage);
			strcat(dataLine,piece);
			strcat(dataLine,comma);

			mc2::fmt::to_buf(piece,"{:4.2f}",stats.weapon.recycleTime);
			strcat(dataLine,piece);
			strcat(dataLine,comma);

			mc2::fmt::to_buf(piece,"{:4.1f}",stats.weapon.heat);
			strcat(dataLine,piece);
			strcat(dataLine,comma);

			mc2::fmt::to_buf(piece,"{:d}",mc2::fmt::as_i32(stats.weapon.ammoAmount));
			strcat(dataLine,piece);
			strcat(dataLine,comma);

			mc2::fmt::to_buf(piece,"{:d}",mc2::fmt::as_i32(stats.weapon.ammoType));
			strcat(dataLine,piece);
			strcat(dataLine,comma);

			mc2::fmt::to_buf(piece,"{}",mc2::fmt::safe(WeaponRangeString[stats.weapon.range]));
			strcat(dataLine,piece);
			strcat(dataLine,comma);
			
//			sprintf(piece,"%d",stats.weapon.type);
//			strcat(dataLine,piece);
//			strcat(dataLine,comma);

			mc2::fmt::to_buf(piece,"{:d}",mc2::fmt::as_i32(stats.weapon.specialEffect));
			strcat(dataLine,piece);
			strcat(dataLine,comma);

			mc2::fmt::to_buf(piece,"{:d}",mc2::fmt::as_i32(stats.weapon.flags));
			strcat(dataLine,piece);
			strcat(dataLine,comma);
			break;

		case COMPONENT_FORM_WEAPON_MISSILE:
			mc2::fmt::to_buf(piece,"{:4.1f}",stats.weapon.damage);
			strcat(dataLine,piece);
			strcat(dataLine,comma);

			mc2::fmt::to_buf(piece,"{:4.2f}",stats.weapon.recycleTime);
			strcat(dataLine,piece);
			strcat(dataLine,comma);

			mc2::fmt::to_buf(piece,"{:4.1f}",stats.weapon.heat);
			strcat(dataLine,piece);
			strcat(dataLine,comma);

			mc2::fmt::to_buf(piece,"{:d}",mc2::fmt::as_i32(stats.weapon.ammoAmount));
			strcat(dataLine,piece);
			strcat(dataLine,comma);

			mc2::fmt::to_buf(piece,"{:d}",mc2::fmt::as_i32(stats.weapon.ammoType));
			strcat(dataLine,piece);
			strcat(dataLine,comma);

			mc2::fmt::to_buf(piece,"{}",mc2::fmt::safe(WeaponRangeString[stats.weapon.range]));
			strcat(dataLine,piece);
			strcat(dataLine,comma);
			
//			sprintf(piece,"%d",stats.weapon.type);
//			strcat(dataLine,piece);
//			strcat(dataLine,comma);

			mc2::fmt::to_buf(piece,"{:d}",mc2::fmt::as_i32(stats.weapon.specialEffect));
			strcat(dataLine,piece);
			strcat(dataLine,comma);

			mc2::fmt::to_buf(piece,"{:d}",mc2::fmt::as_i32(stats.weapon.flags));
			strcat(dataLine,piece);
			strcat(dataLine,comma);
			break;

		case COMPONENT_FORM_AMMO:
			mc2::fmt::to_buf(piece,"{:d}",mc2::fmt::as_i32(stats.ammo.ammoPerTon));
			strcat(dataLine,piece);
			strcat(dataLine,comma);

			mc2::fmt::to_buf(piece,"{:6.1f}",stats.ammo.explosiveDamage);
			strcat(dataLine,piece);
			strcat(dataLine,comma);
			mc2::fmt::to_buf(piece,"na,na,na,na,na,na,na,,,,,");
			strcat(dataLine,piece);
			strcat(dataLine,comma);
			break;

		case COMPONENT_FORM_JUMPJET:
			mc2::fmt::to_buf(piece,"{:d}",mc2::fmt::as_i32(stats.jumpjet.rangeMod));
			strcat(dataLine,piece);
			strcat(dataLine,comma);
			mc2::fmt::to_buf(piece,"na,na,na,na,na,na,na,na,,,,,");
			strcat(dataLine,piece);
			strcat(dataLine,comma);
			break;
	}

	return(NO_ERR);
}

//---------------------------------------------------------------------------

bool MasterComponent::isOffensiveWeapon (void) {

	//----------------------------------------------------------------------
	// For now, we have just one defensive weapon--the anti-missile system.
	// So, we can pretty much special-case this routine. If we add others,
	// we will want to specify DEFENSIVE/OFFENSIVE for each weapon, and then
	// check against this field...
	return((masterID != clanAntiMissileSystemID) && (masterID != innerSphereAntiMissileSystemID));
}

//---------------------------------------------------------------------------

bool MasterComponent::isDefensiveWeapon (void) {

	//----------------------------------------------------------------------
	// For now, we have just one defensive weapon--the anti-missile system.
	// So, we can pretty much special-case this routine. If we add others,
	// we will want to specify DEFENSIVE/OFFENSIVE for each weapon, and then
	// check against this field...
	return((masterID == clanAntiMissileSystemID) || (masterID == innerSphereAntiMissileSystemID));
}

//---------------------------------------------------------------------------

long MasterComponent::loadMasterList (const char* fileName, long listSize, float baseSensorRange) {

	freeMasterList();

	numComponents = listSize;
	masterList = static_cast<MasterComponentPtr>(std::malloc(sizeof(MasterComponent) * numComponents));
	// The table loaders set only the fields the data names; the rest (disableLevel, which getInventoryDamage is compared with on every component hit,
	// among them) must not be whatever the heap block held: the same hit disabled a component in one run and not in the next (mc2_24 frame 3193).
	memset(masterList, 0, sizeof(MasterComponent) * numComponents);

	for (long curComponent = 0; curComponent < numComponents; curComponent++)
		::new (static_cast<void*>(&masterList[curComponent])) MasterComponent;

	armActuatorID = -1;
	legActuatorID = -1;
	clanAntiMissileSystemID = -1;
	innerSphereAntiMissileSystemID = -1;

	//-----------------------------------------------------------------
	// All components are in one data file. Open it up, and read in the
	// comma-delimited data...
	File componentFile;
	long result = componentFile.open(fileName);
	if (result != NO_ERR)
		return(result);

	//-----------------------
	// Special Component Data
	char dataLine[512];
	long lineLength;
	

	lineLength = componentFile.readLine(reinterpret_cast<MemoryPtr>(dataLine), 511);

	for (long componentNum = 0; componentNum < numComponents; componentNum++) {
		//----------------------------------------
		// Read in the line from the table file...
		lineLength = componentFile.readLine(reinterpret_cast<MemoryPtr>(dataLine), 511);
		if (!lineLength)
			return(-1);

		// EQUIP-ADD-FILL-1 landmine fix: initEXCEL early-returns -1 on an
		// unknown Type token ("undefined" is NOT in ComponentFormString, so the
		// masterID=-1 reset inside initEXCEL is unreachable dead code) WITHOUT
		// resetting masterID/name -- undefined rows kept masterID==ordinal, a
		// string-table name (non-empty on installs with a full string table),
		// and a GARBAGE uninitialized form. Restore the sentinel state
		// (masterID=-1, empty name) on failure so "undefined row" is
		// deterministic for every consumer (registry, ADD fill, debug-state
		// witness, saveEXCEL's blank-line branch). Populated rows untouched.
		if (masterList[componentNum].initEXCEL(dataLine, baseSensorRange) != NO_ERR)
			masterList[componentNum].init();
	}

	componentFile.close();

	return(NO_ERR);
}


//---------------------------------------------------------------------------
long MasterComponent::saveMasterList (const char* fileName, long listSize, float baseSensorRange) 
{
	//-----------------------------------------------------------------
	// All components are in one data file. Save it in CSV format!
	File componentFile;
	long result = componentFile.create(fileName);
	if (result != NO_ERR)
		return(result);

	//----------------------------------------------------
	// Comment header so we know who screwed the file up!
	char dataLine[512];

	mc2::fmt::to_buf(dataLine,"// <Built by Editor v3.0> Special Component Data,,,,,******REVISED FOR NEW MECH STATS,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,");

	componentFile.writeLine(dataLine);

	//-----------------------
	// Special Component Data
	mc2::fmt::to_buf(dataLine,"MasterArmActuatorID = {:d},,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,",mc2::fmt::as_i32(armActuatorID));
	componentFile.writeLine(dataLine);

	mc2::fmt::to_buf(dataLine,"MasterLegActuatorID = {:d},,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,",mc2::fmt::as_i32(legActuatorID));
	componentFile.writeLine(dataLine);

	//sebi: what is more correct here? always pass155 or actual clanAntiMissileSystemID ?
	//sprintf(dataLine,"MasterClanAntiMissileSystemID = 115,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,",clanAntiMissileSystemID);
	mc2::fmt::to_buf(dataLine,"MasterClanAntiMissileSystemID = 115,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,");
	componentFile.writeLine(dataLine);

	//sebi: same here
	//sprintf(dataLine,"MasterInnerSphereAntiMissileSystemID = 106,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,",innerSphereAntiMissileSystemID);
	mc2::fmt::to_buf(dataLine,"MasterInnerSphereAntiMissileSystemID = 106,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,");
	componentFile.writeLine(dataLine);
	
	mc2::fmt::to_buf(dataLine,"//,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,Weapon,Effect,Ammo,Flag,1 = streak 2 = inferno");
	componentFile.writeLine(dataLine);

	mc2::fmt::to_buf(dataLine,"// Component Table,,abbr,type,crits,?,tons,RP,head,CT,LT,RT,LA,RA,LL,RL,Vehicle?,Fit both?,Side,Fit IS?,art,disable,BR,Damage,Recycle,heat,#miss,miss type,min,short,med,long,Type,Field,Master ID,Fields,4 = LBX 8 = artillery");
	componentFile.writeLine(dataLine);

	for (long componentNum = 0; componentNum < listSize; componentNum++) 
	{
		masterList[componentNum].saveEXCEL(&componentFile,componentNum, baseSensorRange);
	}

	componentFile.close();

	return(NO_ERR);
}

//---------------------------------------------------------------------------

long MasterComponent::freeMasterList (void) {

	if (masterList) 
	{
		for (long i = 0; i < numComponents; ++i)
			masterList[i].~MasterComponent();
		std::free(masterList);
		masterList = nullptr;
		numComponents = 0;
	}

	return(NO_ERR);
}

//***************************************************************************

