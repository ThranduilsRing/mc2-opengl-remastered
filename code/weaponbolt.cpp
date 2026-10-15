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
#include "mc2fmt.h"
#include <bit>
#include <cstring>
#include "diagnostic_trace.h"


//---------------------------------------------------------------------------
// class WeaponBoltType
//---------------------------------------------------------------------------
GameObjectPtr WeaponBoltType::createInstance (void)
{
	WeaponBoltPtr result = new WeaponBolt;
	if (!result)
		return nullptr;

	result->init(true, this);
		
	return(result);
}

//---------------------------------------------------------------------------
void WeaponBoltType::destroy (void)
{
	if (textureName)
		ObjectTypeManager::objectCache->Free(textureName); 
		
	textureName = nullptr;

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
		// Editor colors use BGR byte order.
		frontRGB = bgrTorgb(frontRGB);
		backRGB = bgrTorgb(backRGB);
		middleRGB = bgrTorgb(middleRGB);
		midEdgeRGB = bgrTorgb(midEdgeRGB);
		lightRGB = bgrTorgb(lightRGB);
	}
	
	//------------------------------------------------------------------
	result = ObjectType::init(&bullFile);
	
	return(result);
}
	
//---------------------------------------------------------------------------
bool WeaponBoltType::handleCollision (GameObjectPtr collidee, GameObjectPtr collider)
{
	//-------------------------------------------------------
	// Effect completion owns retirement; collision must not remove the bolt.
	
	return(FALSE);
}

//---------------------------------------------------------------------------
bool WeaponBoltType::handleDestruction (GameObjectPtr collidee, GameObjectPtr collider)
{
	//-------------------------------------------------------
	// Effect completion owns retirement; collision must not remove the bolt.
	
	return(FALSE);
}

//---------------------------------------------------------------------------
TG_LightPtr WeaponBoltType::getLight (void)
{
	TG_LightPtr pointLight = nullptr;

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
	bool isVisible = true;
	if (isVisible)
	{
		windowsVisible = turn;
	}
		
	return(true);
}

//---------------------------------------------------------------------------

// Combat diagnostics retain object identity and event order without owning gameplay state.
void WeaponBolt::traceCombat(std::string_view event, std::string_view reason,
                              GameObjectPtr target, const WeaponShotInfo* shot)
{
    combatTrace.write("bolt", getHandle(), turn, event, reason, position,
        [this] { return getFlag(OBJECT_FLAG_JUSTCREATED) ? nullptr : &laserPosition; }, target, shot);
}

void WeaponBolt::traceCombatRetire(std::string_view reason)
{
    traceCombat("retire", reason);
    combatTrace.live = false;
}

// Only the authoritative side submits damage; its trace must precede the target's response.
void WeaponBolt::submitDamage(GameObjectPtr target)
{
    if (MPlayer) {
        if (MPlayer->isServer()) {
            traceCombat("damage", "server", target, &weaponShot);
            target->handleWeaponHit(&weaponShot, true);
        }
    } else {
        traceCombat("damage", "direct", target, &weaponShot);
        target->handleWeaponHit(&weaponShot);
    }
}

void WeaponBolt::destroy (void)
{
	traceCombatRetire("teardown");
	if (targetPosition)
	{
		delete targetPosition;
		targetPosition = nullptr;
	}

	// Effects can outlive the mission.
	if (pointLight)
	{
		if (eye)
			eye->removeWorldLight(lightId,pointLight);
		systemHeap->Free(pointLight);
		pointLight = nullptr;
	}

	if (hitEffect)
	{
		hitEffect->Kill();
		delete hitEffect;
		hitEffect = nullptr;
	}

	if (muzzleEffect)
	{
		muzzleEffect->Kill();
		delete muzzleEffect;
		muzzleEffect = nullptr;
	}

	if (gosEffect)
	{
		gosEffect->Kill();	 
		delete gosEffect;
		gosEffect = nullptr;
	}

	if (missEffect)
	{
		missEffect->Kill();	 
		delete missEffect;
		missEffect = nullptr;
	}

	if (waterMissEffect)
	{
		waterMissEffect->Kill();
		delete waterMissEffect;
		waterMissEffect = nullptr;
	}
}

// ---------------------------------------------------------------------------
// Trail kinds follow effects.csv names; unmapped effects retain CPU gosFX.
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
    // Flamer retains CPU gosFX until GPU visual parity is established.
    return GpuTrailKind::None;
}

// PPC and missile trails retain CPU MLR: GPU billboards produce white square artifacts.
// Promotion requires swept-mesh Tube parity in active missile/PPC combat; passive smoke
// misses weapon-hit effects. Keep docs/vfx-originals-lockdown.md consistent with this policy.
static bool gpuTrailKindProven(mc2::particles::GpuTrailKind k)
{
    (void)k;
    return false;
}

//---------------------------------------------------------------------------
void WeaponBolt::init (bool create, ObjectTypePtr _type)
{
	traceCombatRetire("reinit");
	GameObject::init(create, _type);

	setFlag(OBJECT_FLAG_JUSTCREATED, true);

	objectClass = WEAPONBOLT;
	
	lightId = -9;	//Tell it to make one!
	

	destroy();
	//-----------------------------------------
	if (effectId >= 0)
	{
		// A GPU-owned trail must not also emit CPU gosFX.
		mc2::particles::GpuTrailKind init_gpu_kind = gpuTrailKindFromEffectId(effectId);
		bool gpu_owns_trail = false;
		if (gpuTrailKindProven(init_gpu_kind)) {
			// Batcher owns the default-ON policy; raw environment reads can cause double trails.
			gpu_owns_trail = mc2::particles::Batcher::is_enabled();
		}
		if (!gpu_owns_trail && strcmp(weaponEffects->GetEffectName(effectId),"NONE") != 0)
		{
			//--------------------------------------------
			unsigned flags = gosFX::Effect::ExecuteFlag|gosFX::Effect::LoopFlag;

			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(effectId));

			if (gosEffectSpec)
			{
				gosEffect = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(gosEffect != nullptr);

				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();

				//gosEffect->SetScalar(BASE_EFFECT_SCALAR);
			}
		}
		
		if ((strcmp(weaponEffects->GetEffectMuzzleFlashName(effectId),"NONE") != 0) && useNonWeaponEffects)
		{
			//--------------------------------------------
			unsigned flags = gosFX::Effect::ExecuteFlag;
	
			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectMuzzleFlashName(effectId));
			
			if (gosEffectSpec)
			{
				muzzleEffect = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(muzzleEffect != nullptr);
				
				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			
				//muzzleEffect->SetScalar(BASE_EFFECT_SCALAR);
			}
		}
		
		if (strcmp(weaponEffects->GetEffectHitName(effectId),"NONE") != 0)
		{
			//--------------------------------------------
			unsigned flags = gosFX::Effect::ExecuteFlag;
	
			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectHitName(effectId));
			
			if (gosEffectSpec)
			{
				hitEffect = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(hitEffect != nullptr);
			
				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			
				//hitEffect->SetScalar(BASE_EFFECT_SCALAR);
			}
		}
		
		if ((strcmp(weaponEffects->GetEffectMissName(effectId),"NONE") != 0) && useNonWeaponEffects)
		{
			//--------------------------------------------
			unsigned flags = gosFX::Effect::ExecuteFlag;
	
			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectMissName(effectId));
			
			if (gosEffectSpec)
			{
				missEffect = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(missEffect != nullptr);
			
				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			
				//hitEffect->SetScalar(BASE_EFFECT_SCALAR);
			}
		}
		
		//No water miss for machine guns
		if (useNonWeaponEffects && (effectId != 11) && (strcmp(weaponEffects->GetEffectName(WATER_MISS_FX),"NONE") != 0))
		{
			//--------------------------------------------
			unsigned flags = gosFX::Effect::ExecuteFlag;
	
			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(WATER_MISS_FX));
			
			if (gosEffectSpec)
			{
				waterMissEffect = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(waterMissEffect != nullptr);
			
				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			
				//hitEffect->SetScalar(BASE_EFFECT_SCALAR);
			}
		}
		else
		{
			waterMissEffect = nullptr;
		}
	}
	
	//----------------------------------------------
	// Effect textures require alpha.
	if (((WeaponBoltTypePtr)_type)->textureName && S_stricmp(((WeaponBoltTypePtr)_type)->textureName,"NONE") != 0)
	{
		char tPath[1024];
		mc2::fmt::to_buf(tPath, "{}128" PATH_SEPARATOR "", mc2::fmt::safe(tglPath));

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

	gpu_trail_kind = gpuTrailKindFromEffectId(effectId);
    combatTrace.begin(_type->whatAmI(), effectId);
    traceCombat("birth");
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
#include "save_struct_zero.h"
void WeaponBolt::Save (PacketFilePtr file, long packetNum)
{
	MC2_SAVE_STRUCT(WeaponBoltData, data);
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
		targetPosition = nullptr;
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
	combatTrace.effect = effectId;
	traceCombat("restore");
	if (!getExists()) traceCombatRetire("loaded_inactive");
}

//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
// Successful gated area-effect aimed shots retain weaponShot.hitLocation in single-player;
// ordinary position-bolt explosions randomize the location. Multiplayer keeps stock splash.
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

	traceCombat("damage", "aimed_area", aimedTarget, &weaponShot);
	aimedTarget->handleWeaponHit(&weaponShot);
	return true;
}

//---------------------------------------------------------------------------

void WeaponBolt::finishNow (void)
{
	if (getExists())
	{

		if (hitEffect)
		{
			hitEffect->Kill();
			delete hitEffect;
			hitEffect = nullptr;
		}

		if (muzzleEffect)
		{
			muzzleEffect->Kill();
			delete muzzleEffect;
			muzzleEffect = nullptr;
		}

		if (missEffect)
		{
			missEffect->Kill();
			delete missEffect;
			missEffect = nullptr;
		}

		if (waterMissEffect)
		{
			waterMissEffect->Kill();
			delete waterMissEffect;
			waterMissEffect = nullptr;
		}

		if (gosEffect)
		{
			gosEffect->Kill();		//Effect is over.  Otherwise, wait until hit!
			delete gosEffect;
			gosEffect = nullptr;
		}

		if (pointLight)
		{
			if (eye)
				eye->removeWorldLight(lightId,pointLight);
			systemHeap->Free(pointLight);
			pointLight = nullptr;
		}

		GameObjectPtr target = ObjectManager->getByWatchID(targetWID);
		if (target)
		{
			submitDamage(target);
		}

		if (((WeaponBoltTypePtr)getObjectType())->hitEffectObjNum &&
			(((WeaponBoltTypePtr)getObjectType())->areaEffectDmg > 0.0f) &&
			(((WeaponBoltTypePtr)getObjectType())->areaEffectRad > 0.0f))
		{
			// Aimed-hit explosions stay cosmetic to avoid duplicate damage.
			float areaDmg = ((WeaponBoltTypePtr)getObjectType())->areaEffectDmg;
			if (deliverAimedAreaHit())
				areaDmg = 0.0f;

			//Create the explosion here.  HIT or MISS, its the same one!!
			// Damage is done by explosion NOT by hit!
			ObjectManager->createExplosion(((WeaponBoltTypePtr)getObjectType())->hitEffectObjNum,
											nullptr,*targetPosition,
											areaDmg,
											((WeaponBoltTypePtr)getObjectType())->areaEffectRad);
		}

		traceCombatRetire("forced_finish");
		MC2_DESTROY(this, "bolt_impact");
	}
}
