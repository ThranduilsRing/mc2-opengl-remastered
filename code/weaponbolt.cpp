//---------------------------------------------------------------------------
//
// weaponbolt.cpp -- File contains the WeaponBolt Object code
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include "weaponbolt_internal.h"


//---------------------------------------------------------------------------
// class WeaponBoltType
//---------------------------------------------------------------------------
GameObjectPtr WeaponBoltType::createInstance (void)
{
	WeaponBoltPtr result = new WeaponBolt;
	if (!result)
		return NULL;

	result->init(true, this);
		
	return(result);
}

//---------------------------------------------------------------------------
void WeaponBoltType::destroy (void)
{
	if (textureName)
		ObjectTypeManager::objectCache->Free(textureName); 
		
	textureName = NULL;

	ObjectType::destroy();
}

//---------------------------------------------------------------------------
DWORD bgrTorgb (DWORD frontRGB);
	
//---------------------------------------------------------------------------
long WeaponBoltType::init (FilePtr objFile, unsigned long fileSize)
{
	long result = 0;
	
	FitIniFile bullFile;
	result = bullFile.open(objFile,fileSize);
	if (result != NO_ERR)
		Fatal(result,"WeaponBoltType::init -- Unable to open File");
	
	//------------------------------------------------------------------
	result = bullFile.seekBlock("BoltProjectileData");
	if (result != NO_ERR)
	{
		//Check if we are a NEW beam type.
		result = bullFile.seekBlock("BeamProjectileData");
		if (result != NO_ERR)
		{
			STOP(("WeaponBoltTYpe::init -- Not a valid weaponBolt"));
		}
		
		result = bullFile.readIdBoolean("IsBeam",isBeam);
		if (result != NO_ERR)
			isBeam = false;
			
		result = bullFile.readIdFloat("BeamDuration",beamDuration);
		if (result != NO_ERR)
			beamDuration = 0.0f;
			
		result = bullFile.readIdFloat("BeamWiggle",beamWiggle);
		if (result != NO_ERR)
			beamWiggle = 0.0f;
			
		result = bullFile.readIdBoolean("PointLight",lightSource);
		if (result != NO_ERR)
			lightSource = false;
	
		char txmName[1024];
		result = bullFile.readIdString("TextureName",txmName,1023);
		if (result != NO_ERR)
			strcpy(txmName,"NONE");
		
		textureName = (char *)ObjectTypeManager::objectCache->Malloc(strlen(txmName)+1); 
		strcpy(textureName,txmName);

		result = bullFile.readIdFloat("UVAnimRate",uvAnimRate);
		if (result != NO_ERR)
			uvAnimRate = 0.0f;
			
		result = bullFile.readIdFloat("UVRepeat",uvRepeat);
		if (result != NO_ERR)
			uvRepeat = 1.0f;
			
		result = bullFile.readIdFloat("UnitLength",unitLength);
		if (result != NO_ERR)
			unitLength = 1.0f;
			
		if (unitLength < Stuff::SMALL)
			unitLength = 1.0f;

		result = bullFile.readIdBoolean("MipTexture",mipTexture);
		if (result != NO_ERR)
			mipTexture = false;
			
 	 	result = bullFile.readIdLong("FiringSoundFX",fireSoundFX);
		if (result != NO_ERR)
			fireSoundFX = 0;
			
		result = bullFile.readIdFloat("BulgeWidth",bulgeWidth);
		if (result != NO_ERR)
			bulgeWidth = 1.0f;
			
		result = bullFile.readIdULong("MidEdgeRGB",midEdgeRGB);
		if (result != NO_ERR)
			midEdgeRGB = 0x00ffffff;
		
		result = bullFile.readIdULong("MiddleRGB",middleRGB);
		if (result != NO_ERR)
			middleRGB = 0x00ffffff;
				
		result = bullFile.readIdUChar("EdgeAlpha",edgeAlpha);
		if (result != NO_ERR)
			edgeAlpha = 0xff;
			
		result = bullFile.readIdUChar("BoltAlpha",boltAlpha);
		if (result != NO_ERR)
			boltAlpha = 0xff;
		
		projLength = 1.0f;
		velocity = 1.0f;
					
   		if (lightSource)
		{
			result = bullFile.readIdULong("LightRGB",lightRGB);
			if (result != NO_ERR)
				Fatal(result,"WeaponBoltType::init -- Unable to find lightRGB");
	
			result = bullFile.readIdFloat("MaxRadius",maxRadius);
			if (result != NO_ERR)
				Fatal(result,"WeaponBoltType::init -- Unable to find maxRadius");
	
			result = bullFile.readIdFloat("OutRadius",outRadius);
			if (result != NO_ERR)
				Fatal(result,"WeaponBoltType::init -- Unable to find outRadius");
	
			result = bullFile.readIdFloat("Intensity",intensity);
			if (result != NO_ERR)
				Fatal(result,"WeaponBoltType::init -- Unable to find intensity");
		}
	}
	else
	{
		result = bullFile.readIdFloat("ProjectileLength",projLength);
		if (result != NO_ERR)
			Fatal(result,"WeaponBoltType::init -- Unable to find projectileLength");
				
		result = bullFile.readIdFloat("BulgeLength",bulgeLength);
		if (result != NO_ERR)
			Fatal(result,"WeaponBoltType::init -- Unable to find bulgeLength");
			
		result = bullFile.readIdFloat("BulgeWidth",bulgeWidth);
		if (result != NO_ERR)
			Fatal(result,"WeaponBoltType::init -- Unable to find bulgeWidth");
		
		result = bullFile.readIdFloat("Velocity",velocity);
		if (result != NO_ERR)
			Fatal(result,"WeaponBoltType::init -- Unable to find velocity");
		
		result = bullFile.readIdLong("HitEffect",hitEffectObjNum);
		if (result != NO_ERR)
			Fatal(result,"WeaponBoltType::init -- Unable to find HitEffect");
		
		result = bullFile.readIdLong("MissEffect",missEffectObjNum);
		if (result != NO_ERR)
			Fatal(result,"WeaponBoltType::init -- Unable to find missEffect");
		
		result = bullFile.readIdLong("FiringSoundFX",fireSoundFX);
		if (result != NO_ERR)
			Fatal(result,"WeaponBoltType::init -- Unable to find FiringSoundFx");
		
		result = bullFile.readIdULong("FrontRGB",frontRGB);
		if (result != NO_ERR)
			Fatal(result,"WeaponBoltType::init -- Unable to find FrontRGB");
			
		result = bullFile.readIdULong("MidEdgeRGB",midEdgeRGB);
		if (result != NO_ERR)
			Fatal(result,"WeaponBoltType::init -- Unable to find MIDEDGERGB");
		
		result = bullFile.readIdULong("MiddleRGB",middleRGB);
		if (result != NO_ERR)
			Fatal(result,"WeaponBoltType::init -- Unable to find MiddleRGB");
		
		result = bullFile.readIdULong("BackRGB",backRGB);
		if (result != NO_ERR)
			Fatal(result,"WeaponBoltType::init -- Unable to find backRGB");
			
		result = bullFile.readIdUChar("EdgeAlpha",edgeAlpha);
		if (result != NO_ERR)
			Fatal(result,"WeaponBoltType::init -- Unable to find edgeAlpha");
			
		result = bullFile.readIdUChar("BoltAlpha",boltAlpha);
		if (result != NO_ERR)
			Fatal(result,"WeaponBoltType::init -- Unable to find boltAlpha");
		
		result = bullFile.readIdBoolean("IsBeam",isBeam);
		if (result != NO_ERR)
			isBeam = false;
			
		result = bullFile.readIdFloat("BeamDuration",beamDuration);
		if (result != NO_ERR)
			beamDuration = 0.0f;
			
		result = bullFile.readIdFloat("BeamWiggle",beamWiggle);
		if (result != NO_ERR)
			beamWiggle = 0.0f;
			
		result = bullFile.readIdBoolean("PointLight",lightSource);
		if (result != NO_ERR)
			lightSource = false;

		result = bullFile.readIdBoolean("ArcEffect",arcEffect);
		if (result != NO_ERR)
			arcEffect = false;
			
		result = bullFile.readIdFloat("ArcHeight",arcHeight);
		if (result != NO_ERR)
			arcHeight = -1.0f;

		result = bullFile.readIdFloat("AfterHitTime",afterHitTime);
		if (result != NO_ERR)
			afterHitTime = 0.0f;
			
		result = bullFile.readIdFloat("AreaEffectDamage",areaEffectDmg);
		if (result != NO_ERR)
			areaEffectDmg = 0.0f;
			
		result = bullFile.readIdFloat("AreaEffectRadius",areaEffectRad);
		if (result != NO_ERR)
			areaEffectRad = 0.0f;
			
   		if (lightSource)
		{
			result = bullFile.readIdULong("LightRGB",lightRGB);
			if (result != NO_ERR)
				Fatal(result,"WeaponBoltType::init -- Unable to find lightRGB");
	
			result = bullFile.readIdFloat("MaxRadius",maxRadius);
			if (result != NO_ERR)
				Fatal(result,"WeaponBoltType::init -- Unable to find maxRadius");
	
			result = bullFile.readIdFloat("OutRadius",outRadius);
			if (result != NO_ERR)
				Fatal(result,"WeaponBoltType::init -- Unable to find outRadius");
	
			result = bullFile.readIdFloat("Intensity",intensity);
			if (result != NO_ERR)
				Fatal(result,"WeaponBoltType::init -- Unable to find intensity");
		}
	
		//------------------------------------------------------------------
		// Editor writes RGBs as BGRs.  Change over here.
		frontRGB = bgrTorgb(frontRGB);
		backRGB = bgrTorgb(backRGB);
		middleRGB = bgrTorgb(middleRGB);
		midEdgeRGB = bgrTorgb(midEdgeRGB);
		lightRGB = bgrTorgb(lightRGB);
	}
	
	//------------------------------------------------------------------
	// Initialize the base object Type from the current file.
	result = ObjectType::init(&bullFile);
	
	return(result);
}
	
//---------------------------------------------------------------------------
bool WeaponBoltType::handleCollision (GameObjectPtr collidee, GameObjectPtr collider)
{
	//-------------------------------------------------------
	// The bolt ceases to exist when its effect is done.
	// always return FALSE or the collision will make it
	// go away!
	
	return(FALSE);
}

//---------------------------------------------------------------------------
bool WeaponBoltType::handleDestruction (GameObjectPtr collidee, GameObjectPtr collider)
{
	//-------------------------------------------------------
	// The bolt ceases to exist when its effect is done.
	// always return FALSE or the collision will make it
	// go away!
	
	return(FALSE);
}

//---------------------------------------------------------------------------
TG_LightPtr WeaponBoltType::getLight (void)
{
	TG_LightPtr pointLight = NULL;

	if (lightSource)
	{
		pointLight = (TG_LightPtr)systemHeap->Malloc(sizeof(TG_Light));
		pointLight->init(TG_LIGHT_POINT);

		pointLight->SetaRGB(lightRGB);
		pointLight->SetIntensity(intensity);
		pointLight->SetFalloffDistances(maxRadius,outRadius);
	}

	return(pointLight);
}	

//---------------------------------------------------------------------------
// class WeaponBolt
//---------------------------------------------------------------------------
bool WeaponBolt::isVisible (void)
{
	//----------------------------------------------------------------------
	// This function checks to see if this weaponBolt is visible on screen
	bool isVisible = true;
	if (isVisible)
	{
		windowsVisible = turn;
	}
		
	return(true);
}

//---------------------------------------------------------------------------
void WeaponBolt::destroy (void)
{
	if (targetPosition)
	{
		delete targetPosition;
		targetPosition = NULL;
	}

	//Must toss these here as mission may have ended with effect still playing!!
	if (pointLight)
	{
		if (eye)
			eye->removeWorldLight(lightId,pointLight);
		systemHeap->Free(pointLight);
		pointLight = NULL;
	}

	if (hitEffect)
	{
		hitEffect->Kill();
		delete hitEffect;
		hitEffect = NULL;
	}

	if (muzzleEffect)
	{
		muzzleEffect->Kill();
		delete muzzleEffect;
		muzzleEffect = NULL;
	}

	if (gosEffect)
	{
		gosEffect->Kill();	 
		delete gosEffect;
		gosEffect = NULL;
	}

	if (missEffect)
	{
		missEffect->Kill();	 
		delete missEffect;
		missEffect = NULL;
	}

	if (waterMissEffect)
	{
		waterMissEffect->Kill();
		delete waterMissEffect;
		waterMissEffect = NULL;
	}
}

// ---------------------------------------------------------------------------
// B3b-3: INI survey -> gpu_trail_kind mapping.
// Driven by effects.csv GosEffectName column:
//   effectName="ppc_trail"      (ids 4=PPC, 5=ER PPC)           -> PpcBolt
//   effectName="srm_trail"      (id 8=SRM)                       -> MissileSmoke
//   effectName="lrm_trail"      (id 9=LRM)                       -> MissileSmoke
//   effectName="Swarm_lrm_trail"(id 74=Swarm LRM)                -> MissileSmoke
//   effectName="TBolt_trail"    (id 27=Thunderbolt)               -> MissileSmoke
//   effectName="flamer_trail"   (id 3=Flamer bolt)                -> None (CPU gosFX; not proven)
//   anything else (lasers, ACs, gauss, MG, NONE, ...)             -> None
// Survey saved to docs/superpowers/specs/2026-05-26-fx-gpu-b3-weapon-ini-survey.md
static mc2::particles::GpuTrailKind gpuTrailKindFromEffectId(int32_t eid)
{
    using mc2::particles::GpuTrailKind;
    if (eid < 0 || !weaponEffects)
        return GpuTrailKind::None;
    const char* name = weaponEffects->GetEffectName(eid);
    if (!name || S_stricmp(name, "NONE") == 0)
        return GpuTrailKind::None;
    if (S_stricmp(name, "ppc_trail") == 0)
        return GpuTrailKind::PpcBolt;
    if (S_stricmp(name, "srm_trail")       == 0 ||
        S_stricmp(name, "lrm_trail")       == 0 ||
        S_stricmp(name, "Swarm_lrm_trail") == 0 ||
        S_stricmp(name, "TBolt_trail")     == 0)
        return GpuTrailKind::MissileSmoke;
    // flamer_trail → None: flamer GPU visual not proven; CPU gosFX fallback preserved.
    return GpuTrailKind::None;
}

// VFX-ORIGINALS-LOCKDOWN-1: allowlist of GPU trail kinds proven to match original visuals.
// Both PpcBolt and MissileSmoke are currently demoted (return false). The GPU ring-buffer
// billboard trail produced "white square card" artifacts. Original gosFX Tube via CPU MLR
// is the active path for both.
//
// To re-promote a kind:
//   1. Implement a proper swept-mesh ribbon GPU oracle for Tube (VFX-TUBE-SWEPT-MESH-ORACLE-1).
//   2. Run an interactive combat session (mc2_10 missiles + mc2_24 PPC) and confirm visual parity.
//   3. Update this function and docs/vfx-originals-lockdown.md in the same commit.
//   NEVER re-promote based on smoke-only (passive 30s) results — weapon-hit effects are not
//   exercised in passive smoke. See docs/vfx-originals-lockdown.md for full policy.
static bool gpuTrailKindProven(mc2::particles::GpuTrailKind k)
{
    (void)k;
    return false;
}

//---------------------------------------------------------------------------
void WeaponBolt::init (bool create, ObjectTypePtr _type)
{
	GameObject::init(create, _type);

	setFlag(OBJECT_FLAG_JUSTCREATED, true);

	objectClass = WEAPONBOLT;
	
	lightId = -9;	//Tell it to make one!
	
	//Make SURE all of the old Effects are GONE!!

	destroy();
	//-----------------------------------------
	// Are we a magical new GOSFX(tm)?
	if (effectId >= 0)
	{
		// B3c-1: suppress CPU gosFX trail when GPU owns it.
		// gpu_trail_kind is set at the bottom of this function by gpuTrailKindFromEffectId.
		// We need the kind before that assignment, so derive it here directly from effectId.
		mc2::particles::GpuTrailKind init_gpu_kind = gpuTrailKindFromEffectId(effectId);
		bool gpu_owns_trail = false;
		if (gpuTrailKindProven(init_gpu_kind)) {
			// Use Batcher::is_enabled() as single source of truth.
			// Reading getenv directly misses the B3c-2 default-ON case
			// (v==nullptr -> false), causing double trails on normal runs.
			gpu_owns_trail = mc2::particles::Batcher::is_enabled();
		}
		if (!gpu_owns_trail && strcmp(weaponEffects->GetEffectName(effectId),"NONE") != 0)
		{
			//--------------------------------------------
			// Yes, load it on up.
			unsigned flags = gosFX::Effect::ExecuteFlag|gosFX::Effect::LoopFlag;

			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(effectId));

			if (gosEffectSpec)
			{
				gosEffect = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(gosEffect != NULL);

				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();

				//gosEffect->SetScalar(BASE_EFFECT_SCALAR);
			}
		}
		
		if ((strcmp(weaponEffects->GetEffectMuzzleFlashName(effectId),"NONE") != 0) && useNonWeaponEffects)
		{
			//--------------------------------------------
			// Yes, load it on up.
			unsigned flags = gosFX::Effect::ExecuteFlag;
	
			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectMuzzleFlashName(effectId));
			
			if (gosEffectSpec)
			{
				muzzleEffect = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(muzzleEffect != NULL);
				
				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			
				//muzzleEffect->SetScalar(BASE_EFFECT_SCALAR);
			}
		}
		
		if (strcmp(weaponEffects->GetEffectHitName(effectId),"NONE") != 0)
		{
			//--------------------------------------------
			// Yes, load it on up.
			unsigned flags = gosFX::Effect::ExecuteFlag;
	
			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectHitName(effectId));
			
			if (gosEffectSpec)
			{
				hitEffect = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(hitEffect != NULL);
			
				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			
				//hitEffect->SetScalar(BASE_EFFECT_SCALAR);
			}
		}
		
		if ((strcmp(weaponEffects->GetEffectMissName(effectId),"NONE") != 0) && useNonWeaponEffects)
		{
			//--------------------------------------------
			// Yes, load it on up.
			unsigned flags = gosFX::Effect::ExecuteFlag;
	
			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectMissName(effectId));
			
			if (gosEffectSpec)
			{
				missEffect = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(missEffect != NULL);
			
				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			
				//hitEffect->SetScalar(BASE_EFFECT_SCALAR);
			}
		}
		
		//No water miss for machine guns
		if (useNonWeaponEffects && (effectId != 11) && (strcmp(weaponEffects->GetEffectName(WATER_MISS_FX),"NONE") != 0))
		{
			//--------------------------------------------
			// Yes, load it on up.
			unsigned flags = gosFX::Effect::ExecuteFlag;
	
			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(WATER_MISS_FX));
			
			if (gosEffectSpec)
			{
				waterMissEffect = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(waterMissEffect != NULL);
			
				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			
				//hitEffect->SetScalar(BASE_EFFECT_SCALAR);
			}
		}
		else
		{
			waterMissEffect = NULL;
		}
	}
	
	//----------------------------------------------
	// Get a texture Handle from the textureManager
	// Assume ALPHA!
	if (((WeaponBoltTypePtr)_type)->textureName && S_stricmp(((WeaponBoltTypePtr)_type)->textureName,"NONE") != 0)
	{
		char tPath[1024];
		sprintf(tPath,"%s128" PATH_SEPARATOR, tglPath);

		FullPathFileName textureName;
		textureName.init(tPath,((WeaponBoltTypePtr)_type)->textureName,".tga");
	
		if (((WeaponBoltTypePtr)_type)->mipTexture)
			mcTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,0);
		else
			mcTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap);
	}
	else
	{
		mcTextureHandle = 0;
		gosTextureHandle = 0xffffffff;
	}

	// B3b-3: table-driven mapping from effects.csv effectName to GpuTrailKind.
	// Unmapped weapons fall back to GpuTrailKind::None (CPU gosFX unchanged).
	gpu_trail_kind = gpuTrailKindFromEffectId(effectId);
}

//---------------------------------------------------------------------------
void WeaponBolt::setOwner (GameObjectPtr who)
{
	if (who)
		ownerWID = who->getWatchID();
	else
		ownerWID = 0;
}

//---------------------------------------------------------------------------
void WeaponBolt::setTarget (GameObjectPtr who)
{
	if (who)
		targetWID = who->getWatchID();
	else
		targetWID = 0;
}

//---------------------------------------------------------------------------
void WeaponBolt::setTargetPosition (Stuff::Vector3D pos)
{
	if (!targetPosition)
		targetPosition = new Stuff::Vector3D;
		
	*targetPosition = pos;
}

//***************************************************************************
void WeaponBolt::Save (PacketFilePtr file, long packetNum)
{
	WeaponBoltData data;
	CopyTo(&data);

	//PacketNum incremented in ObjectManager!!
	file->writePacket(packetNum,(MemoryPtr)&data,sizeof(WeaponBoltData),STORAGE_TYPE_ZLIB);
}

//***************************************************************************
void WeaponBolt::CopyTo (WeaponBoltData *data)
{																	   
	data->ownerWID = ownerWID;
	data->hotSpotNumber = hotSpotNumber;

	data->targetWID = targetWID;
	data->targetHotSpot = targetHotSpot;

	Stuff::Vector3D dmy;
	dmy.Zero();

	if (targetPosition)
		data->targetPosition = *targetPosition;
	else
		data->targetPosition = dmy;

	data->distanceToTarget = distanceToTarget;
	data->halfDistanceToTarget = halfDistanceToTarget;

	data->weaponShot = weaponShot;

	data->laserPosition = laserPosition;
	memcpy(data->laserVertices,laserVertices,sizeof(Stuff::Vector3D) * 4);
	memcpy(data->laserSide,laserSide,sizeof(Stuff::Vector3D) * 4);

	data->effectId = effectId;
	data->hitTarget = hitTarget;

	data->timeLeft = timeLeft;
	data->hsPos = hsPos;

	data->hitLeft = hitLeft;
	data->mcTextureHandle = mcTextureHandle;
	data->gosTextureHandle = gosTextureHandle;
	data->startUV = startUV;
	data->goalHeight = goalHeight;

	GameObject::CopyTo(data);
}

//---------------------------------------------------------------------------
void WeaponBolt::Load (WeaponBoltData *data)
{
	GameObject::Load(data); 

	ownerWID = data->ownerWID;
	hotSpotNumber = data->hotSpotNumber;

	targetWID = data->targetWID;
	targetHotSpot = data->targetHotSpot;

	if ((data->targetPosition.x == data->targetPosition.y) &&
		(data->targetPosition.x == data->targetPosition.z))
		targetPosition = NULL;
	else
	{
		targetPosition = new Stuff::Vector3D;
		*targetPosition = data->targetPosition;
	}

	distanceToTarget = data->distanceToTarget;
	halfDistanceToTarget = data->halfDistanceToTarget;

	weaponShot = data->weaponShot;

	laserPosition = data->laserPosition;
	memcpy(laserVertices,data->laserVertices,sizeof(Stuff::Vector3D) * 4);
	memcpy(laserSide,data->laserSide,sizeof(Stuff::Vector3D) * 4);

	effectId = data->effectId;
	hitTarget = data->hitTarget;

	timeLeft = data->timeLeft;
	hsPos = data->hsPos;

	hitLeft = data->hitLeft;
	startUV = data->startUV;
	goalHeight = data->goalHeight;
}

//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
// ISSUE-9 S2 (MC2_CALLED_SHOT_FIX): deliver a successful AIMED area-effect
// shot's located damage directly to the aimed target. Stock area-effect bolts
// (LRM et al.) are converted to position bolts at connect() and deliver ALL
// damage as a randomly-located explosion ("Hit or MISS, its the same!"), so a
// called shot's location is never honored. aimedTargetWID is only ever set by
// fireWeapon under the gate on a successful aimed roll; weaponShot.hitLocation
// already carries the called location. Single-player only — in MP the flag is
// never acted on (stock splash behavior preserved).
bool WeaponBolt::deliverAimedAreaHit (void)
{
	if (!aimedTargetWID)
		return false;

	GameObjectWatchID tgtWID = aimedTargetWID;
	aimedTargetWID = 0;		// consume — arrival paths can be re-entered

	if (MPlayer)
		return false;

	GameObjectPtr aimedTarget = ObjectManager->getByWatchID(tgtWID);
	if (!aimedTarget || aimedTarget->isDestroyed())
		return false;

	aimedTarget->handleWeaponHit(&weaponShot);
	return true;
}

//---------------------------------------------------------------------------

void WeaponBolt::finishNow (void)
{
	if (getExists())
	{
		//This weapon is still enroute or in some other way is not resolved.  Resolve it.
		// Then setExists to false!!

		//Toss its effects.
		if (hitEffect)
		{
			hitEffect->Kill();
			delete hitEffect;
			hitEffect = NULL;
		}

		if (muzzleEffect)
		{
			muzzleEffect->Kill();
			delete muzzleEffect;
			muzzleEffect = NULL;
		}

		if (missEffect)
		{
			missEffect->Kill();
			delete missEffect;
			missEffect = NULL;
		}

		if (waterMissEffect)
		{
			waterMissEffect->Kill();
			delete waterMissEffect;
			waterMissEffect = NULL;
		}

		if (gosEffect)
		{
			gosEffect->Kill();		//Effect is over.  Otherwise, wait until hit!
			delete gosEffect;
			gosEffect = NULL;
		}

		//Need to kill the light source here too!
		if (pointLight)
		{
			if (eye)
				eye->removeWorldLight(lightId,pointLight);
			systemHeap->Free(pointLight);
			pointLight = NULL;
		}

		GameObjectPtr target = ObjectManager->getByWatchID(targetWID);
		if (target)
		{
			if (MPlayer)
			{
				if (MPlayer->isServer())
					target->handleWeaponHit(&weaponShot, true);
			}
			else
				target->handleWeaponHit(&weaponShot);
		}

		if (((WeaponBoltTypePtr)getObjectType())->hitEffectObjNum &&
			(((WeaponBoltTypePtr)getObjectType())->areaEffectDmg > 0.0f) &&
			(((WeaponBoltTypePtr)getObjectType())->areaEffectRad > 0.0f))
		{
			// ISSUE-9 S2 (MC2_CALLED_SHOT_FIX): same aimed-hit routing as the
			// normal arrival path — located damage to the aimed target, explosion
			// kept cosmetic. Stock path otherwise.
			float areaDmg = ((WeaponBoltTypePtr)getObjectType())->areaEffectDmg;
			if (deliverAimedAreaHit())
				areaDmg = 0.0f;

			//Create the explosion here.  HIT or MISS, its the same one!!
			// Damage is done by explosion NOT by hit!
			ObjectManager->createExplosion(((WeaponBoltTypePtr)getObjectType())->hitEffectObjNum,
											NULL,*targetPosition,
											areaDmg,
											((WeaponBoltTypePtr)getObjectType())->areaEffectRad);
		}

		MC2_DESTROY(this, "bolt_impact");
	}
}
