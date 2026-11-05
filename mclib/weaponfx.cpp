//---------------------------------------------------------------------
// Weapon FX equivalency Table for Mechs/Vehicles/Whatever
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------------
// Include Files
#ifndef MCLIB_H
#include"mclib.h"
#endif

#include "mc2_czstring.h"
#include"weaponfx.h"
//---------------------------------------------------------------------------------
// weapon FX Equivalancy.  Stores names now for GOS FX
WeaponEffects *weaponEffects = nullptr;
char mc2_word_none[5] = "NONE";

//---------------------------------------------------------------------------------
void WeaponEffects::destroy (void)
{
	std::free(effects);
	effects = nullptr;
	numEffects = 0;
}
		
//---------------------------------------------------------------------------------
void WeaponEffects::init (mc2::czstring effectCSVFileName)
{
	FullPathFileName effectsName;
	effectsName.init(objectPath,effectCSVFileName,".csv");
	
	CSVFile effectFile;
	long result = effectFile.open(effectsName);
	if (result != NO_ERR)
		STOP(("Unable to open Effects File %s", static_cast<const char*>(effectsName)));
		
	numEffects = effectFile.getNumLines() - 1;	//Always subtract one for the column headers

	effects = static_cast<EffectData *>(std::malloc(sizeof(EffectData) * numEffects));
	gosASSERT(effects != nullptr);
	
	for (long i=0;i<numEffects;i++)
	{
		effectFile.readString(i+2,2,effects[i].effectName,49);
		effectFile.readString(i+2,3,effects[i].muzzleFlashName,49);
		effectFile.readString(i+2,4,effects[i].hitEffectName,49);
		effectFile.readString(i+2,6,effects[i].missEffectName,49);
		effectFile.readLong(i+2,5,effects[i].effectObjNum);
	}
}

//---------------------------------------------------------------------------------
