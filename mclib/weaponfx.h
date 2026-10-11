//---------------------------------------------------------------------
// Weapon FX equivalency Table for Mechs/Vehicles/Whatever
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef WEAPONFX_H
#define WEAPONFX_H
//---------------------------------------------------------------------------------
// weapon FX Equivalancy.  Now reads in data from CSV for GOS Effects
inline constexpr int BUILDING_EXPLOSION_ID = 45;
inline constexpr int VEHICLE_EXPLOSION_ID = 40;
inline constexpr int SPLASH_VEHICLE_EXPLOSION_ID = 46;
inline constexpr int DFA_BOOM_EFFECT = 47;
inline constexpr int MECH_EXPLOSION_ID = 39;
inline constexpr int MECH_CRITICAL_HIT_ID = 35;
inline constexpr int MECH_SMOKE_ID = 48;
inline constexpr int TURRET_EXPLOSION_ID = 44;
inline constexpr int MECH_DISABLE_EXPLOSION_ID = 49;
inline constexpr int LARGE_AIRSTRIKE_ID = 50;
inline constexpr int SMALL_AIRSTRIKE_ID = 51;
inline constexpr int SENSOR_AIRSTRIKE_ID = 72;
inline constexpr int MINE_EXPLOSION_ID = 53;
inline constexpr int EMPTY_EXPLOSION_ID = 54;
inline constexpr int JET_CONTRAIL_ID = 58;

inline constexpr int ARM_FLYING_ID = 61;
inline constexpr int SHOULDER_POP_ID = 62;
inline constexpr int CRITICAL_SMOKE_ID = 65;
inline constexpr int MECH_WATER_WAKE = 34;
inline constexpr int VEHICLE_WATER_WAKE = 66;
inline constexpr int VEHICLE_DUST_CLOUD = 70;
inline constexpr int VTOL_DUST_CLOUD = 56;
inline constexpr int KARNOV_DUST_CLOUD = 73;
inline constexpr int KARNOV_RECOVERY_BEAM = 69;
inline constexpr int MINELAYER_EFFECT_ID = 68;
inline constexpr int REPAIR_VEHICLE_EFFECT_ID = 57;
inline constexpr int HELICOPTER_DUST_CLOUD = 67;
inline constexpr int TURRET_POWER_DOWN_EFFECT = 75;
inline constexpr int WATER_MISS_FX = 76;
inline constexpr int INFANTRY_BOOM_EFFECT = 55;
inline constexpr int INFANTRY_BLOOD_EFFECT = 82;

extern char mc2_word_none[];

typedef struct _EffectData
{
	char effectName[50];
	char muzzleFlashName[50];
	char hitEffectName[50];
	char missEffectName[50];
	long effectObjNum;
} EffectData;

class WeaponEffects
{
	//Data Members
	//-------------
	protected:
		EffectData			*effects;
		long				numEffects;
	
	//Member Functions
	//----------------	
	public:
	
		WeaponEffects (void)
		{
			init();
		}
		
		~WeaponEffects (void)
		{
			destroy();
		}
		
		void init (void)
		{
			effects = NULL;
			numEffects = 0;
		}
		
		void destroy (void);
		
		void init (const char *effectCSVFileName);
		
		char *GetEffectName (long effectId)
		{
			if ((effectId >= 0) && (effectId < numEffects))
			{
				return effects[effectId].effectName;
			}
			
			return mc2_word_none;
		}
		
		char *GetEffectMuzzleFlashName (long effectId)
		{
			if ((effectId >= 0) && (effectId < numEffects))
			{
				return effects[effectId].muzzleFlashName;
			}
			
			return mc2_word_none;
		}
		
		char *GetEffectHitName (long effectId)
		{
			if ((effectId >= 0) && (effectId < numEffects))
			{
				return effects[effectId].hitEffectName;
			}
			
			return mc2_word_none;
		}
		
		char *GetEffectMissName (long effectId)
		{
			if ((effectId >= 0) && (effectId < numEffects))
			{
				return effects[effectId].missEffectName;
			}
			
			return mc2_word_none;
		}
		
 		long GetEffectObjNum (long effectId)
		{
			if ((effectId >= 0) && (effectId < numEffects))
			{
				return effects[effectId].effectObjNum;
			}
			
			return -1;
		}
	
};

extern WeaponEffects *weaponEffects;
//---------------------------------------------------------------------------------
#endif
