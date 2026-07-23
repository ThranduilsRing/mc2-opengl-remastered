//---------------------------------------------------------------------------
//
// weaponbolt.cpp -- File contains the WeaponBolt Object code
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include Files
#ifndef MCLIB_H
#include"mclib.h"
#endif

#ifndef WEAPONBOLT_H
#include"weaponbolt.h"
#endif

#ifndef GAMESOUND_H
#include"gamesound.h"
#endif

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef WEAPONFX_H
#include"weaponfx.h"
#endif

#ifndef MULTPLYR_H
#include"multplyr.h"
#endif

#ifndef MISSION_H
#include"mission.h"
#endif

#include <cstdlib>  // getenv/atof for MC2_PROJECTILE_SPEED_MULT

#ifndef GOS_PROFILER_H
#include"gos_profiler.h"
#endif

#include<gameos.hpp>
#include "../mclib/particles/batcher.h"
#include "vk_shared_gbuffer_gate.h"  // VK-FIELD-VFX-2: umbrella-route immediate-draw for bolts

//---------------------------------------------------------------------------
extern bool hasGuardBand;
extern MidLevelRenderer::MLRClipper * theClipper;
extern bool drawOldWay;
extern float BaseHeadShotElevation;
extern bool useNonWeaponEffects;
extern bool MLRVertexLimitReached;

// VK-FIELD-VFX-2 (2026-07-11): under the field VK umbrella the DEFAULT bolt path
// (drawOldWay==false -> mcTextureManager->addVertices) DEFERS the bolt geometry into
// the texture-manager render list, which renderLists() flushes into the (discarded)
// GL scene backbuffer under VK-only present -> weapon fire VANISHES. The round-1
// objmgr IsHUD wrap is INERT for this path because addVertices only memcpys verts
// (no IsHUD capture) and the flush runs OUTSIDE the IsHUD scope. Fix: when the VK VFX
// route is armed, take the IMMEDIATE gos_DrawTriangles path instead (the exact
// mechanism the selection brackets / health bars use, PROVEN visible) so the bolt
// verts are issued while objmgr has gos_State_IsHUD=1 -> composited into the VK HUD
// tail. Umbrella OFF -> subGate false -> drawOldWay path unchanged -> byte-identical GL.
static bool vkVfxImmediateDraw()
{
	const bool s_on = mc2vkgate::subGate("MC2_VK_VFX");   // per-call: VK-FAILSOFT-DISARM-1
	return s_on;
}
static bool vkVfxTraceOn()
{
	static const bool s_on = []{
		const char* v = std::getenv("MC2_VK_VFX_TRACE");
		return v && v[0] && v[0] != '0';
	}();
	return s_on;
}

#define BASE_EFFECT_SCALAR		1.0f
#define EFFECT_LEFT_TIME		1.0f
#define WEAPON_ARC_FACTOR		-20.0f
#define WEAPON_ARC_HEIGHT		150.0f
#define ARC_THRESHOLD			0.0f
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
// PPC de-curve + direct-fire speed (env-gated, default-off). "Direct-fire" =
// non-arc, non-beam bolts (PPC / autocannon / gauss). MC2_DIRECT_FIRE_STRAIGHT=1
// makes them fly STRAIGHT (no per-frame target re-track) to a one-time aim that
// is LED by the target's velocity over the bolt's flight time, so a pre-rolled
// hit on a moving target still lands. LRM (arcEffect) + beams keep tracking.
// MC2_PROJECTILE_SPEED_MULT scales direct-fire travel speed (default 1.0).
static bool directFireStraightEnabled (void)
{
	static const bool s = []{
		const char* v = getenv("MC2_DIRECT_FIRE_STRAIGHT");
		return v && v[0] == '1' && v[1] == '\0';
	}();
	return s;
}
static float directFireSpeedMult (void)
{
	static const float s = []{
		const char* v = getenv("MC2_PROJECTILE_SPEED_MULT");
		float m = v ? (float)atof(v) : 1.0f;
		return (m > 0.0f && m <= 10.0f) ? m : 1.0f;
	}();
	return s;
}

//---------------------------------------------------------------------------
long WeaponBolt::update (void)
{
	ZoneScopedN("GameLogic.Projectile.Update");

	Stuff::Vector3D laserDirection;
	
	bool moveThisFrame = true;
	if (getFlag(OBJECT_FLAG_JUSTCREATED)) 
	{
		setFlag(OBJECT_FLAG_JUSTCREATED, false);
		setFlag(OBJECT_FLAG_TANGIBLE, false);
		
		hitTarget = false;
		moveThisFrame = false;
		//-------------------------------------------------------------
		// Update position relative to my owner
		GameObjectPtr myOwner = ObjectManager->getByWatchID(ownerWID);
		if (myOwner)
		{
			position = myOwner->getPositionFromHS(hotSpotNumber);
		}

		//-------------------------------------------------------------------
		// If we were just Created, play our sound Effect.
		if (((WeaponBoltTypePtr)getObjectType())->fireSoundFX != 0)
			soundSystem->playDigitalSample(((WeaponBoltTypePtr)getObjectType())->fireSoundFX, position, true);
			
		laserPosition = position;
		
		Stuff::Vector3D ownerPosition = laserPosition;
		Stuff::Point3D actualPosition;
		actualPosition.x = -laserPosition.x;
		actualPosition.y = laserPosition.z;
		actualPosition.z = laserPosition.y;

		GameObjectPtr target = ObjectManager->getByWatchID(targetWID);

		Stuff::Vector3D targetPos;
		targetPos.Zero();
		if (target)
		{
			Stuff::Vector3D aim;
			if (!target->isMech())
				aim = target->getPositionFromHS(targetHotSpot);
			else if (targetHotSpot)
				aim = target->appearance->getHitNodeLeft();
			else
				aim = target->appearance->getHitNodeRight();

			// PPC de-curve: direct-fire bolts do NOT re-track (see flight loop),
			// so lead the aim ONCE here by the target's velocity over the bolt's
			// estimated flight time. getVelocity()==0 for non-movers -> no lead
			// (straight shot at the fire-time position). Env-gated, default-off.
			WeaponBoltTypePtr bt = (WeaponBoltTypePtr)getObjectType();
			if (directFireStraightEnabled() && !bt->arcEffect && !bt->isBeam)
			{
				Stuff::Vector3D toTgt;
				toTgt.Subtract(aim, ownerPosition);
				float dist = toTgt.GetLength();
				float spd  = bt->velocity * directFireSpeedMult();
				float tof  = (spd > 0.01f) ? (dist / spd) : 0.0f;
				Stuff::Vector3D lead;
				lead.Multiply(target->getVelocity(), tof);
				aim.Add(aim, lead);
			}
			setTargetPosition(aim);
		}

		if (targetPosition)
		{
			targetPos = *targetPosition;
		}
		else
		{
			//Somehow, we are updating a fresh weaponbolt which has not been connected.
			// Do not allow it to go on or we will crash for sure.
			return false;
		}
					
		laserDirection.Subtract(targetPos,ownerPosition);

		if (laserDirection.GetLength() != 0.0)
			laserDirection.Normalize(laserDirection);

		laserDirection *= ((WeaponBoltTypePtr)getObjectType())->projLength;
		//laserPosition.Add(laserPosition,laserDirection);
		
		if (((WeaponBoltTypePtr)getObjectType())->isBeam)
		{
			timeLeft = ((WeaponBoltTypePtr)getObjectType())->beamDuration;
			actualPosition.x = -(*targetPosition).x;	//Play the beam hitting effect at the hit location
			actualPosition.y = (*targetPosition).z;
			actualPosition.z = (*targetPosition).y;
		}
		else
			timeLeft = 0.0f;
			
		if (gosEffect && gosEffect->IsExecuted())
		{
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;
			Stuff::LinearMatrix4D	localResult;
			
			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(actualPosition);
			
			rotation = -world_angle_between(position, *targetPosition);

			Stuff::UnitQuaternion effectRot;
			effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
			localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
			localResult.Multiply(localToWorld,shapeOrigin);
			
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,NULL);
			gosEffect->Start(&info);
		}
		
		if (muzzleEffect && muzzleEffect->IsExecuted())
		{
			Stuff::Point3D			mPosition;
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;
			Stuff::LinearMatrix4D	localResult;
			
			mPosition.x = -position.x;
			mPosition.y = position.z;
			mPosition.z = position.y;
			
 			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(mPosition);
			
			Stuff::UnitQuaternion effectRot;
			if (myOwner)
				effectRot = Stuff::EulerAngles(0.0f, myOwner->getRotation() * DEGREES_TO_RADS,0.0f);
			else
				effectRot = Stuff::EulerAngles(0.0f, 0.0f ,0.0f);

			localToWorld.Multiply(gosFX::Effect_Into_Motion,effectRot);
			localResult.Multiply(localToWorld,shapeOrigin);
			
 			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,NULL);
			muzzleEffect->Start(&info);
		}
		
		Stuff::Vector3D laserVelocity;
		laserVelocity.Subtract(targetPos,ownerPosition);
		float distance = laserVelocity.x * laserVelocity.x + laserVelocity.y * laserVelocity.y;
		distanceToTarget = distance;
		halfDistanceToTarget = distance / 2.0f;
		
		goalHeight = ((WeaponBoltTypePtr)getObjectType())->arcHeight;
		goalHeight *= 10.0f;
	}

	// B2 P2: prev_position snapshot — no longer used after ring-buffer fix.
	// Field kept for future velocity-based shader fades; assignment removed.
	// prev_position = position;

	//-------------------------------------------------------------
	// Update position relative to my owner
	GameObjectPtr myOwner = ObjectManager->getByWatchID(ownerWID);
	if (myOwner)
		position = myOwner->getPositionFromHS(hotSpotNumber);
	
	bool inView = TRUE;

	//---------------------
	//Update beamDuration
	if (((WeaponBoltTypePtr)getObjectType())->isBeam)
	{
		timeLeft -= frameLength;
		if (timeLeft >= 0.0f)
		{
			gosTextureHandle = mcTextureManager->get_gosTextureHandle(mcTextureHandle);
   			mcTextureManager->addTriangle(mcTextureHandle,MC2_ISEFFECTS|MC2_DRAWONEIN);
   			mcTextureManager->addTriangle(mcTextureHandle,MC2_ISEFFECTS|MC2_DRAWONEIN); 
   			mcTextureManager->addTriangle(mcTextureHandle,MC2_ISEFFECTS|MC2_DRAWONEIN);
   			mcTextureManager->addTriangle(mcTextureHandle,MC2_ISEFFECTS|MC2_DRAWONEIN); 
		}

		startUV += ((WeaponBoltTypePtr)getObjectType())->uvAnimRate * frameLength;
	}
	else if (!gosEffect && !((WeaponBoltTypePtr)getObjectType())->isBeam)
	{
		mcTextureManager->addTriangle(0xffffffff,MC2_DRAWALPHA);
		mcTextureManager->addTriangle(0xffffffff,MC2_DRAWALPHA);
		mcTextureManager->addTriangle(0xffffffff,MC2_DRAWALPHA);
		mcTextureManager->addTriangle(0xffffffff,MC2_DRAWALPHA);
	}
		
	//---------------------------------------------------------------
	// Keep projectile flying toward target.  Even if Target Moves!!
	Stuff::Vector3D ownerPosition = laserPosition;
	GameObjectPtr target = ObjectManager->getByWatchID(targetWID);
		
	Stuff::Vector3D targetPos;
	targetPos.Zero();
	// PPC de-curve: direct-fire (non-arc, non-beam) bolts do NOT re-track -- they
	// fly straight to the lead-aim locked at spawn. LRM/arc + beams keep tracking.
	WeaponBoltTypePtr boltType = (WeaponBoltTypePtr)getObjectType();
	const bool directStraight =
		directFireStraightEnabled() && !boltType->arcEffect && !boltType->isBeam;
	if (target && (!hitTarget || boltType->isBeam) && !directStraight)
	{
		if (!target->isMech())
		{
			setTargetPosition(target->getPositionFromHS(targetHotSpot));
		}
		else
		{
			if (targetHotSpot)
				setTargetPosition(target->appearance->getHitNodeLeft());
			else
				setTargetPosition(target->appearance->getHitNodeRight());
		}
	}
		
	if (targetPosition)
	{
		targetPos = *targetPosition;
	}
				
	Stuff::Vector3D laserVelocity;
	float velMag = ((WeaponBoltTypePtr)getObjectType())->velocity;
	velMag *= frameLength;

	// PPC-SPEED: scale DIRECT-FIRE (non-arc) bolt travel speed (PPC/AC/gauss).
	// MC2_PROJECTILE_SPEED_MULT default 1.0 (=stock); clamped (0,10]. Hit/miss is
	// pre-rolled and damage applies on proximity, so speed doesn't alter outcomes.
	if (!((WeaponBoltTypePtr)getObjectType())->arcEffect)
		velMag *= directFireSpeedMult();

	laserVelocity.Subtract(targetPos,ownerPosition);
	float distance = laserVelocity.x * laserVelocity.x + laserVelocity.y * laserVelocity.y;
	
	//----------------------------------------------------------------------
	// For the first half of the distance, bring weapons up if NOT beam!!!
	bool isArcing = false;
	if (((WeaponBoltTypePtr)getObjectType())->arcEffect)
	{
		//GoalHeight is actually the velocity
		if ( goalHeight > 0.0f )
		{
			float accel = -((WeaponBoltTypePtr)getObjectType())->arcHeight * 10.0f;
			goalHeight += accel * frameLength;
			if (goalHeight >= 0.0f)
			{
				laserVelocity.z = goalHeight;
				isArcing = true;
			}
		}
	}

	//-------------------------------------------------------------------
	// Make the bullets go away when they get close enough to the target.
	// This is determined by checking the distanceToTarget.  A soon as it
	// starts to get bigger, we have passed the target and "hit" it.
	if (!hitTarget)
	{
		if (!((WeaponBoltTypePtr)getObjectType())->isBeam)
		{
			float lastDistanceMoved = distanceToTarget - distance;
			distanceToTarget = distance;
			
			if (lastDistanceMoved >= distance)
			{
				hitTarget = TRUE;
				hitLeft = ((WeaponBoltTypePtr)getObjectType())->afterHitTime;

				// VFX-WEAPON-FX-RESTORE-OPUS-1: kill trail gosEffect immediately on impact.
				// The oracle renders gosEffect particles at the hit position; without this
				// kill the lozenge/bolt-head card floats at the target for several seconds.
				// hitEffect provides the actual impact visual.
				if (gosEffect) {
					gosEffect->Kill();
					delete gosEffect;
					gosEffect = NULL;
				}

				if (target)
				{
					Stuff::Vector3D hotSpot = target->getPositionFromHS(targetHotSpot);

					if (target->isMech())
					{
						if (targetHotSpot)
							hotSpot = target->appearance->getHitNodeLeft();
						else
							hotSpot = target->appearance->getHitNodeRight(); 

					}

					if (hitEffect && hitEffect->IsExecuted())
					{
						Stuff::Point3D			tPosition;
						Stuff::LinearMatrix4D 	shapeOrigin;
						Stuff::LinearMatrix4D	localToWorld;
						
						tPosition.x = -hotSpot.x;
						tPosition.y = hotSpot.z;
						tPosition.z = hotSpot.y;
						
						shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
						shapeOrigin.BuildTranslation(tPosition);
						
						gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,NULL);
						hitEffect->Start(&info);
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
					
 					//Put effect ON the spot!!
					laserPosition = hotSpot;
				}
				else
				{
					Stuff::Vector3D hotSpot = *targetPosition;
					int cellR, cellC;
					land->worldToCell(hotSpot,cellR, cellC);
					if (GameMap->getDeepWater(cellR, cellC) || GameMap->getShallowWater(cellR, cellC))
					{
						if (waterMissEffect && waterMissEffect->IsExecuted())
						{
							Stuff::Point3D			tPosition;
							Stuff::LinearMatrix4D 	shapeOrigin;
							Stuff::LinearMatrix4D	localToWorld;
							
							tPosition.x = -hotSpot.x;
							tPosition.y = Terrain::waterElevation;
							tPosition.z = hotSpot.y;
							
							shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
							shapeOrigin.BuildTranslation(tPosition);
							
							gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,NULL);
							waterMissEffect->Start(&info);
						}
							
						if (missEffect)
						{
							missEffect->Kill();
							delete missEffect;
							missEffect = NULL;
						}
					}
					else
					{
						if (missEffect && missEffect->IsExecuted())
						{
							Stuff::Point3D			tPosition;
							Stuff::LinearMatrix4D 	shapeOrigin;
							Stuff::LinearMatrix4D	localToWorld;
							
							tPosition.x = -hotSpot.x;
							tPosition.y = hotSpot.z;
							tPosition.z = hotSpot.y;
							
							shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
							shapeOrigin.BuildTranslation(tPosition);
							
							gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,NULL);
							missEffect->Start(&info);
						}
						
						if (waterMissEffect)
						{
							waterMissEffect->Kill();
							delete waterMissEffect;
							waterMissEffect = NULL;
						}
					}
					
					if (hitEffect)
					{
						hitEffect->Kill();
						delete hitEffect;
						hitEffect = NULL;
					}
					
					//Put effect ON the spot!!
					laserPosition = hotSpot;
				}
				
				//--------------------------------
				// Laser has hit target.
				if (target)
				{
					//Figure out angle between target and Shooter if target is BELOW shooter.
					// If no shooter, no bonus!!
					GameObjectPtr owner = ObjectManager->getByWatchID(ownerWID);
					if (owner)
					{
						if (target->getPosition().z < owner->getPosition().z)
						{
							Stuff::Vector3D targetPos = target->getPosition();
							Stuff::Vector3D ownerPos = owner->getPosition();
							
							float zDistance = ownerPos.z - targetPos.z;
							targetPos.z = ownerPos.z = 0.0f;
							
							Stuff::Vector3D distance;
							distance.Subtract(ownerPos,targetPos);
							
							float yDistance = distance.GetApproximateLength();
							
							float angle = mc2_atan2(zDistance,yDistance) * RADS_TO_DEGREES;
							
							if (RollDice(angle * BaseHeadShotElevation))
							{
								weaponShot.hitLocation = 0;		//Make it a head shot!
							}
							
						}
					}
					
					if (((WeaponBoltTypePtr)getObjectType())->hitEffectObjNum &&
						(((WeaponBoltTypePtr)getObjectType())->areaEffectDmg > 0.0f) &&
						(((WeaponBoltTypePtr)getObjectType())->areaEffectRad > 0.0f))
					{
						//This is an area effect weapon. Hit or MISS, its the same!
						if (hitEffect)
						{
							hitEffect->Kill();
							delete hitEffect;
							hitEffect = NULL;
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
						
						//Create the explosion here.  HIT or MISS, its the same one!!
						// Damage is done by explosion NOT by hit!
						ObjectManager->createExplosion(((WeaponBoltTypePtr)getObjectType())->hitEffectObjNum,
														NULL,*targetPosition,
														((WeaponBoltTypePtr)getObjectType())->areaEffectDmg,
														((WeaponBoltTypePtr)getObjectType())->areaEffectRad);
					}
					else
					{
						if (MPlayer)
						{
							if (MPlayer->isServer())
								target->handleWeaponHit(&weaponShot, true);
						}
						else
							target->handleWeaponHit(&weaponShot);
					}
				}
				else if (targetPosition)
				{
					long randomCrater = 0;
					if (weaponShot.damage > 9)
						randomCrater = 3;
					else if (weaponShot.damage > 6)
						randomCrater = 2;
					else if (weaponShot.damage > 3)
						randomCrater = 1;
						
					if (missEffect)		//We missed and hit land.  Otherwise, we hit water, do NOT add crater
						craterManager->addCrater(CRATER_1+randomCrater,*targetPosition,RandomNumber(180));
		
					//if (MasterComponentList[weaponShot.masterId].getWeaponAmmoType() != WEAPON_AMMO_NONE)	
					// energy weapons don't set off mines
					// Yes, they should DT, 5/30/98
					{
						int cellRow, cellCol;
		
						land->worldToCell(*targetPosition, cellRow, cellCol);
						if (GameMap->getMine(cellRow, cellCol) == 1)
						{
							ObjectManager->createExplosion(MINE_EXPLOSION_ID, NULL, *targetPosition, MineSplashDamage, MineSplashRange * worldUnitsPerMeter);
							GameMap->setMine(cellRow, cellCol, 2);
						}
					}

					if (((WeaponBoltTypePtr)getObjectType())->hitEffectObjNum &&
						(((WeaponBoltTypePtr)getObjectType())->areaEffectDmg > 0.0f) &&
						(((WeaponBoltTypePtr)getObjectType())->areaEffectRad > 0.0f))
					{
						//This is an area effect weapon. Hit or MISS, its the same!
						if (hitEffect)
						{
							hitEffect->Kill();
							delete hitEffect;
							hitEffect = NULL;
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

						// ISSUE-9 S2 (MC2_CALLED_SHOT_FIX): successful aimed shot —
						// deliver located damage to the aimed target and keep the
						// explosion cosmetic (0 damage). Stock path otherwise.
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
				}
			}
		}
		else
		{
			if (timeLeft < 0.0)
			{
				hitTarget = TRUE;

					// VFX-WEAPON-FX-RESTORE-OPUS-1: kill gosEffect here too.
					// The primary hit path (line 622) kills gosEffect, but this
					// timeout/secondary path did not — leaving gosEffect alive with
					// hitTarget=TRUE causes oracle to keep emitting cards at laserPosition
					// for the full afterHitTime duration.
					if (gosEffect) {
						gosEffect->Kill();
						delete gosEffect;
						gosEffect = NULL;
					}

				if (target)
				{
					Stuff::Vector3D hotSpot = target->getPositionFromHS(targetHotSpot);
					if (target->isMech())
					{
						if (targetHotSpot)
							hotSpot = target->appearance->getHitNodeLeft();
						else
							hotSpot = target->appearance->getHitNodeRight();
					}

					if (hitEffect && hitEffect->IsExecuted())
					{
						Stuff::Point3D			tPosition;
						Stuff::LinearMatrix4D 	shapeOrigin;
						Stuff::LinearMatrix4D	localToWorld;
						
						tPosition.x = -hotSpot.x;
						tPosition.y = hotSpot.z;
						tPosition.z = hotSpot.y;
						
						shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
						shapeOrigin.BuildTranslation(tPosition);
						
						gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,NULL);
						hitEffect->Start(&info);
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
				else
				{
					Stuff::Vector3D hotSpot = *targetPosition;
					int cellR, cellC;
					land->worldToCell(hotSpot,cellR, cellC);
					if (GameMap->getDeepWater(cellR, cellC) || GameMap->getShallowWater(cellR, cellC))
					{
						if (waterMissEffect && waterMissEffect->IsExecuted())
						{
							Stuff::Point3D			tPosition;
							Stuff::LinearMatrix4D 	shapeOrigin;
							Stuff::LinearMatrix4D	localToWorld;
							
							tPosition.x = -hotSpot.x;
							tPosition.y = Terrain::waterElevation;
							tPosition.z = hotSpot.y;
							
							shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
							shapeOrigin.BuildTranslation(tPosition);
							
							gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,NULL);
							waterMissEffect->Start(&info);
						}
						
						if (missEffect)
						{
							missEffect->Kill();
							delete missEffect;
							missEffect = NULL;
						}
					}
					else
					{
						if (missEffect && missEffect->IsExecuted())
						{
							Stuff::Point3D			tPosition;
							Stuff::LinearMatrix4D 	shapeOrigin;
							Stuff::LinearMatrix4D	localToWorld;
							
							tPosition.x = -hotSpot.x;
							tPosition.y = hotSpot.z;
							tPosition.z = hotSpot.y;
							
							shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
							shapeOrigin.BuildTranslation(tPosition);
							
							gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,NULL);
							missEffect->Start(&info);
						}
						
						if (waterMissEffect)
						{
							waterMissEffect->Kill();
							delete waterMissEffect;
							waterMissEffect = NULL;
						}
					}
					
					if (hitEffect)
					{
						hitEffect->Kill();
						delete hitEffect;
						hitEffect = NULL;
					}
				}
				
				//--------------------------------
				// Laser has hit target.
				if (target)
				{
					//Figure out angle between target and Shooter if target is BELOW shooter.
					// If no shooter, no bonus!!
					GameObjectPtr owner = ObjectManager->getByWatchID(ownerWID);
					if (owner)
					{
						if (target->getPosition().z < owner->getPosition().z)
						{
							Stuff::Vector3D targetPos = target->getPosition();
							Stuff::Vector3D ownerPos = owner->getPosition();

							float zDistance = ownerPos.z - targetPos.z;
							targetPos.z = ownerPos.z = 0.0f;

							Stuff::Vector3D distance;
							distance.Subtract(ownerPos,targetPos);

							float yDistance = distance.GetApproximateLength();

							float angle = mc2_atan2(zDistance,yDistance) * RADS_TO_DEGREES;

							if (RollDice(angle * BaseHeadShotElevation))
							{
								weaponShot.hitLocation = 0;		//Make it a head shot!
							}
						}
					}
					
					if (((WeaponBoltTypePtr)getObjectType())->hitEffectObjNum &&
						(((WeaponBoltTypePtr)getObjectType())->areaEffectDmg > 0.0f) &&
						(((WeaponBoltTypePtr)getObjectType())->areaEffectRad > 0.0f))
					{
						//This is an area effect weapon. Hit or MISS, its the same!
						if (hitEffect)
						{
							hitEffect->Kill();
							delete hitEffect;
							hitEffect = NULL;
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
						
 						//Create the explosion here.  HIT or MISS, its the same one!!
						// Damage is done by explosion NOT by hit!
						ObjectManager->createExplosion(((WeaponBoltTypePtr)getObjectType())->hitEffectObjNum,
														NULL,*targetPosition,
														((WeaponBoltTypePtr)getObjectType())->areaEffectDmg,
														((WeaponBoltTypePtr)getObjectType())->areaEffectRad);
					}
					else
					{
						if (MPlayer) 
						{
							if (MPlayer->isServer()) 
								target->handleWeaponHit(&weaponShot, true);
						}
						else
							target->handleWeaponHit(&weaponShot);
					}
				}
				else if (targetPosition) 
				{
					long randomCrater = 0;
					if (weaponShot.damage > 9)
						randomCrater = 3;
					else if (weaponShot.damage > 6)
						randomCrater = 2;
					else if (weaponShot.damage > 3)
						randomCrater = 1;
						
 					if (missEffect)
						craterManager->addCrater(CRATER_1+randomCrater,*targetPosition,RandomNumber(180));
		
					//if (MasterComponentList[weaponShot.masterId].getWeaponAmmoType() != WEAPON_AMMO_NONE)	
					// energy weapons don't set off mines
					// Yes, they should DT, 5/30/98
					{
						int cellRow, cellCol;
		
						land->worldToCell(*targetPosition, cellRow, cellCol);
						if (GameMap->getMine(cellRow, cellCol) == 1)
						{
							ObjectManager->createExplosion(MINE_EXPLOSION_ID, NULL, *targetPosition, MineSplashDamage, MineSplashRange * worldUnitsPerMeter);
							GameMap->setMine(cellRow, cellCol, 2);
						}
					}
					
					if (((WeaponBoltTypePtr)getObjectType())->hitEffectObjNum &&
						(((WeaponBoltTypePtr)getObjectType())->areaEffectDmg > 0.0f) &&
						(((WeaponBoltTypePtr)getObjectType())->areaEffectRad > 0.0f))
					{
						//This is an area effect weapon. Hit or MISS, its the same!
						if (hitEffect)
						{
							hitEffect->Kill();
							delete hitEffect;
							hitEffect = NULL;
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
						
 						//Create the explosion here.  HIT or MISS, its the same one!!
						// Damage is done by explosion NOT by hit!
						ObjectManager->createExplosion(((WeaponBoltTypePtr)getObjectType())->hitEffectObjNum,
														NULL,*targetPosition,
														((WeaponBoltTypePtr)getObjectType())->areaEffectDmg,
														((WeaponBoltTypePtr)getObjectType())->areaEffectRad);
					}
				}
			}
		}
	}
	
	//Actually move the effect toward the target
	// But only AFTER the first Start for the effect
	// Otherwise, weapon moves one time of flight distant
	if (!hitTarget)
	{
		float tmpZ = laserVelocity.z;
		if (isArcing)
		{
			laserVelocity.z = 0.0f;
			tmpZ *= frameLength;
		}
		
		if (laserVelocity.GetLength() != 0.0)
			laserVelocity.Normalize(laserVelocity);
	
		if (moveThisFrame)
		{
			laserVelocity *= velMag;
			if (isArcing)
				laserVelocity.z = tmpZ;
		}
		
		laserPosition += laserVelocity;
	
		laserDirection.Negate(laserVelocity);
		if (laserDirection.GetLength() != 0.0)
			laserDirection.Normalize(laserDirection);
	}

	//------------------------------------------------
	// Update GOSFX
   	if (gosEffect && gosEffect->IsExecuted())
   	{
		Stuff::Point3D actualPosition;
		if (((WeaponBoltTypePtr)getObjectType())->isBeam)
		{
			actualPosition.x = -(*targetPosition).x;	//Play the beam hitting effect at the hit location
			actualPosition.y = (*targetPosition).z;
			actualPosition.z = (*targetPosition).y;
		}
		else
		{
			actualPosition.x = -laserPosition.x;
			actualPosition.y = laserPosition.z;
			actualPosition.z = laserPosition.y;
		}
	
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D 	localToWorld;
		Stuff::LinearMatrix4D 	localResult;
			
		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(actualPosition);
		
		rotation = -world_angle_between(laserPosition, *targetPosition);
		
		Stuff::UnitQuaternion effectRot;
		effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
		localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
 
		Stuff::OBB boundingBox;
   		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,&boundingBox);

		bool result = gosEffect->Execute(&info);
		if (!result)
		{
			gosEffect->Kill();		//Effect is over.  Otherwise, wait until hit!
			delete gosEffect;
			gosEffect = NULL;
		}
   	}
	
   	if (muzzleEffect && muzzleEffect->IsExecuted())
   	{
		Stuff::Point3D mPosition;
		mPosition.x = -position.x;
		mPosition.y = position.z;
		mPosition.z = position.y;
	
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D 	localToWorld;
		Stuff::LinearMatrix4D 	localResult;
			
		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(mPosition);

		Stuff::UnitQuaternion effectRot;
		if (myOwner)
			effectRot = Stuff::EulerAngles(0.0f, myOwner->getRotation() * DEGREES_TO_RADS,0.0f);
		else
			effectRot = Stuff::EulerAngles(0.0f, 0.0f ,0.0f); 

		localToWorld.Multiply(gosFX::Effect_Into_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
			
 		Stuff::OBB boundingBox;
   		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,&boundingBox);

		bool result = muzzleEffect->Execute(&info);
		if (!result)
		{
			muzzleEffect->Kill();		//Effect is over.  Otherwise, wait until hit!
			delete muzzleEffect;
			muzzleEffect = NULL;
		}

   	}

	if (!hitTarget && !((WeaponBoltTypePtr)getObjectType())->isBeam)
	{
		//--------------------------------------------------------------------------------------
		// LaserPosition is front of projectile, laserVertices[0] is top vertex of bulge.
		// LaserVertices[1] is bottom vertex of bulge, laservertices[2] is back of projectile,
		// and laservertices[3] is the middle of the bulge.	
		Stuff::Vector3D lDir(laserDirection);
		lDir *= ((WeaponBoltTypePtr)getObjectType())->bulgeLength;
		laserVertices[0].Add(laserPosition,lDir);
		laserVertices[1] = laserVertices[0];
		laserVertices[3] = laserVertices[0];
		
		laserVertices[1].x += laserDirection.y * ((WeaponBoltTypePtr)getObjectType())->bulgeWidth;
		laserVertices[1].y -= laserDirection.x * ((WeaponBoltTypePtr)getObjectType())->bulgeWidth;
		
		laserVertices[0].x -= laserDirection.y * ((WeaponBoltTypePtr)getObjectType())->bulgeWidth;
		laserVertices[0].y += laserDirection.x * ((WeaponBoltTypePtr)getObjectType())->bulgeWidth;
	
		laserDirection *= ((WeaponBoltTypePtr)getObjectType())->projLength;
		laserVertices[2].Add(laserPosition,laserDirection);
	
		if (((WeaponBoltTypePtr)getObjectType())->lightSource && !pointLight && (lightId != -1))
		{
			pointLight = ((WeaponBoltTypePtr)getObjectType())->getLight();
			lightId = eye->addWorldLight(pointLight);
			if (lightId == -1)
			{
				//NO LIGHT ALLOWED!  TOO Many in World!
				// LightId is now -1 which will cause this to NEVER make a light!
				systemHeap->Free(pointLight);
				pointLight = NULL;
			}
		}
	}
	else if (!hitTarget && ((WeaponBoltTypePtr)getObjectType())->isBeam)
	{
		//------------------------------------------------
		// For beam we need 4 vertices for the two faces.
		laserVertices[0] = position;
		laserVertices[1] = position;
		laserSide[0] = position;
		laserSide[1] = position;
		
		laserVertices[0].x -= laserDirection.y * ((WeaponBoltTypePtr)getObjectType())->bulgeWidth;
		laserVertices[0].y += laserDirection.x * ((WeaponBoltTypePtr)getObjectType())->bulgeWidth;
 		
		laserVertices[1].x += laserDirection.y * ((WeaponBoltTypePtr)getObjectType())->bulgeWidth;
		laserVertices[1].y -= laserDirection.x * ((WeaponBoltTypePtr)getObjectType())->bulgeWidth;

		laserSide[0].z += ((WeaponBoltTypePtr)getObjectType())->bulgeWidth;
		laserSide[1].z -= ((WeaponBoltTypePtr)getObjectType())->bulgeWidth;

   		hsPos = *targetPosition;
   		if (target)
		{
			if (!target->isMech())
			{
				hsPos = target->getPositionFromHS(targetHotSpot);
			}
			else
			{
				if (targetHotSpot)
					hsPos = target->appearance->getHitNodeLeft();
				else
					hsPos = target->appearance->getHitNodeRight(); 
			}
		}

		//-------------------------------------------
		// Wiggle the hotspot.
		float beamWiggle = ((WeaponBoltTypePtr)getObjectType())->beamWiggle;
		Stuff::Vector3D offset(beamWiggle,beamWiggle,beamWiggle);
		offset.x = RandomNumber(offset.x * 2) - offset.x;
		offset.y = RandomNumber(offset.y * 2) - offset.y;
		offset.z = RandomNumber(offset.z * 2) - offset.z;
		hsPos += offset;

		laserVertices[2] = hsPos;
		laserVertices[3] = hsPos;
		
		laserSide[2] = hsPos;
		laserSide[3] = hsPos;
		
		laserVertices[2].x -= laserDirection.y * ((WeaponBoltTypePtr)getObjectType())->bulgeWidth;
		laserVertices[2].y += laserDirection.x * ((WeaponBoltTypePtr)getObjectType())->bulgeWidth;
 		
		laserVertices[3].x += laserDirection.y * ((WeaponBoltTypePtr)getObjectType())->bulgeWidth;
		laserVertices[3].y -= laserDirection.x * ((WeaponBoltTypePtr)getObjectType())->bulgeWidth;
		
		laserSide[2].z += ((WeaponBoltTypePtr)getObjectType())->bulgeWidth;
		laserSide[3].z -= ((WeaponBoltTypePtr)getObjectType())->bulgeWidth;
 	}

	if (pointLight)
	{
		Stuff::Point3D ourPosition;
		ourPosition.x = -laserPosition.x;
		ourPosition.y = laserPosition.z;
		ourPosition.z = laserPosition.y;

		pointLight->direction = ourPosition;

		Stuff::LinearMatrix4D lightToWorldMatrix;
		lightToWorldMatrix.BuildTranslation(ourPosition);
		lightToWorldMatrix.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		pointLight->SetLightToWorld(&lightToWorldMatrix);
		pointLight->SetPosition(&laserPosition);
	}
		
 	if (hitTarget)
	{
		hitLeft -= frameLength;
		
		if (gosEffect && (hitLeft < 0.0f))
		{
			gosEffect->Kill();
			delete gosEffect;
			gosEffect = NULL;
			
			//Need to kill the light source here too!
			if (pointLight)
			{
				eye->removeWorldLight(lightId,pointLight);
				systemHeap->Free(pointLight);
				pointLight = NULL;
			}
 		}
		
		if (hitEffect && hitEffect->IsExecuted())
		{
			Stuff::Vector3D hotSpot = *targetPosition;
			if (target)
			{
				if (!target->isMech())
				{
					hotSpot = target->getPositionFromHS(targetHotSpot);
				}
				else
				{
					if (targetHotSpot)
						hotSpot = target->appearance->getHitNodeLeft();
					else
						hotSpot = target->appearance->getHitNodeRight(); 
				}
			}
				
			Stuff::Point3D tPosition;
			tPosition.x = -hotSpot.x;
			tPosition.y = hotSpot.z;
			tPosition.z = hotSpot.y;
		
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D 	localToWorld;
				
			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(tPosition);
	
			Stuff::OBB boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
	
			bool result = hitEffect->Execute(&info);
			if (!result)
			{
				if (hitEffect)
				{
					hitEffect->Kill();		//Effect is over.  Otherwise, wait until hit!
					delete hitEffect;
					hitEffect = NULL;
				}
					
				inView = FALSE;
				
				if (pointLight)
				{
					eye->removeWorldLight(lightId,pointLight);
					systemHeap->Free(pointLight);
					pointLight = NULL;
				}
				
				//Kill later to make Steve Happy!!
				if (muzzleEffect)
				{
					muzzleEffect->Kill();
					delete muzzleEffect;
					muzzleEffect = NULL;
				}
			}
		}
		else
		{
			if (hitEffect)
			{
				hitEffect->Kill();		//Effect is over.  Otherwise, wait until hit!
				delete hitEffect;
				hitEffect = NULL;
			}
				
			if (pointLight)
			{
				eye->removeWorldLight(lightId,pointLight);
				systemHeap->Free(pointLight);
				pointLight = NULL;
			}
		}
		
		if (missEffect && missEffect->IsExecuted())
		{
			Stuff::Vector3D hotSpot = *targetPosition;
			if (target)
			{
				if (!target->isMech())
				{
					hotSpot = target->getPositionFromHS(targetHotSpot);
				}
				else
				{
					if (targetHotSpot)
						hotSpot = target->appearance->getHitNodeLeft();
					else
						hotSpot = target->appearance->getHitNodeRight(); 
				}
			}
				
			Stuff::Point3D tPosition;
			tPosition.x = -hotSpot.x;
			tPosition.y = hotSpot.z;
			tPosition.z = hotSpot.y;
		
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D 	localToWorld;
				
			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(tPosition);
	
			Stuff::OBB boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
	
			bool result = missEffect->Execute(&info);
			if (!result)
			{
				if (missEffect)
				{
					missEffect->Kill();		//Effect is over.  Otherwise, wait until hit!
					delete missEffect;
					missEffect = NULL;
				}
					
				//Kill later to make Steve Happy!!
				if (muzzleEffect)
				{
					muzzleEffect->Kill();
					delete muzzleEffect;
					muzzleEffect = NULL;
				}
				
				inView = FALSE;
				
				if (pointLight)
				{
					eye->removeWorldLight(lightId,pointLight);
					systemHeap->Free(pointLight);
					pointLight = NULL;
				}
			}
		}
		else
		{
			if (missEffect)
			{
				missEffect->Kill();		//Effect is over.  Otherwise, wait until hit!
				delete missEffect;
				missEffect = NULL;
			}
				
			if (pointLight)
			{
				eye->removeWorldLight(lightId,pointLight);
				systemHeap->Free(pointLight);
				pointLight = NULL;
			}
		}
		
		if (waterMissEffect && waterMissEffect->IsExecuted())
		{
			Stuff::Vector3D hotSpot = *targetPosition;
			if (target)
			{
				if (!target->isMech())
				{
					hotSpot = target->getPositionFromHS(targetHotSpot);
				}
				else
				{
					if (targetHotSpot)
						hotSpot = target->appearance->getHitNodeLeft();
					else
						hotSpot = target->appearance->getHitNodeRight(); 
				}
			}
				
			Stuff::Point3D tPosition;
			tPosition.x = -hotSpot.x;
			tPosition.y = Terrain::waterElevation; 
			tPosition.z = hotSpot.y;
		
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D 	localToWorld;
				
			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(tPosition);
	
			Stuff::OBB boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
	
			bool result = waterMissEffect->Execute(&info);
			if (!result)
			{
				if (waterMissEffect)
				{
					waterMissEffect->Kill();		//Effect is over.  Otherwise, wait until hit!
					delete waterMissEffect;
					waterMissEffect = NULL;
				}
					
				//Kill later to make Steve Happy!!
				if (muzzleEffect)
				{
					muzzleEffect->Kill();
					delete muzzleEffect;
					muzzleEffect = NULL;
				}
				
				inView = FALSE;
				
				if (pointLight)
				{
					eye->removeWorldLight(lightId,pointLight);
					systemHeap->Free(pointLight);
					pointLight = NULL;
				}
			}
		}
		else
		{
			if (waterMissEffect)
			{
				waterMissEffect->Kill();		//Effect is over.  Otherwise, wait until hit!
				delete waterMissEffect;
				waterMissEffect = NULL;
			}
				
			if (pointLight)
			{
				eye->removeWorldLight(lightId,pointLight);
				systemHeap->Free(pointLight);
				pointLight = NULL;
			}
		}

		if (!waterMissEffect && !hitEffect && !missEffect && !gosEffect)
			inView = false;
	}
	
	// B2 P2 fix: ring-buffer trail. Push current position into the ring, then
	// re-stamp the entire trail between every consecutive pair. Required because
	// Batcher::Flush() clears each frame — per-frame segments alone are invisible.
	// VFX-WEAPON-FX-RESTORE-OPUS-1: skip GPU trail when gosEffect is active —
	// gosEffect oracle provides the original animation; dual-trail suppressed here.
	// VFX-WEAPON-FX-RESTORE-OPUS-1: also gate on !hitTarget — after impact gosEffect
	// is killed (NULL) so the !gosEffect check passes, causing the GPU trail to emit
	// a static particle at laserPosition (target mech) every frame for the bolt's
	// afterHitTime duration, producing the persistent blue card at the impact site.
	if (gpu_trail_kind != mc2::particles::GpuTrailKind::None && !gosEffect && !hitTarget) {
		// Push current position into ring buffer.
		// B2 P2 fix: use laserPosition (the integrated in-flight world position),
		// NOT position (which tracks the launcher hotspot via getPositionFromHS).
		// Apply the same Stuff->render axis swap that gosFX uses at lines ~411-413,
		// so the trail particles land in the render-space coordinate system the
		// shader expects (matches the swap baked into parentToWorld for spawn_card).
		Stuff::Vector3D swapped;
		swapped.x = -laserPosition.x;
		swapped.y =  laserPosition.z;
		swapped.z =  laserPosition.y;
		trail_history[trail_head] = swapped;
		trail_head = static_cast<uint8_t>((trail_head + 1) % kTrailHistoryMax);
		if (trail_count < kTrailHistoryMax) ++trail_count;

		// Walk the ring oldest → newest, stamping segments between consecutive
		// positions. Oldest index = (trail_head - trail_count + kTrailHistoryMax)
		// mod kTrailHistoryMax. Stop at trail_head - 1 (most recent stored).
		if (trail_count >= 2) {
			int oldest = (static_cast<int>(trail_head) - static_cast<int>(trail_count)
			              + kTrailHistoryMax) % kTrailHistoryMax;
			int prev_i = oldest;
			for (int i = 1; i < trail_count; ++i) {
				int cur_i = (oldest + i) % kTrailHistoryMax;
				mc2::particles::GpuTrailEmitter::Spawn(
					gpu_trail_kind,
					trail_history[prev_i],
					trail_history[cur_i],
					/*deltaT*/ frameLength);
				prev_i = cur_i;
			}
		}
	}

	return (inView);
}

//---------------------------------------------------------------------------
void WeaponBolt::render (void)
{
	if (!getFlag(OBJECT_FLAG_JUSTCREATED) && !isTargeted())	//can we be seen by the enemy?
	{
		gosFX::Effect::DrawInfo drawInfo;
		drawInfo.m_clipper = theClipper;
		
		MidLevelRenderer::MLRState mlrState;
		//mlrState.SetBackFaceOn();
		mlrState.SetDitherOn();
		mlrState.SetTextureCorrectionOn();
		mlrState.SetZBufferCompareOn();
		mlrState.SetZBufferWriteOn();

		//mlrState.SetFilterMode(MidLevelRenderer::MLRState::BiLinearFilterMode);
		//mlrState.SetAlphaMode(MidLevelRenderer::MLRState::AlphaInvAlphaMode);

		drawInfo.m_state = mlrState;
		drawInfo.m_clippingFlags = 0x0;

		// VK-FIELD-VFX-2: force the immediate gos_DrawTriangles path under the VK VFX
		// umbrella route so the bolt lands in the composited HUD tail (see file-top note).
		const bool useImmediate = drawOldWay || vkVfxImmediateDraw();
		const bool vfxTrace = vkVfxTraceOn();

		if (!gosEffect && !hitTarget && !((WeaponBoltTypePtr)getObjectType())->isBeam)
		{
			//---------------------------
			// Render the polygons here.
			gos_VERTEX	lq1Vertices[3], lq2Vertices[3], lq3Vertices[3], lq4Vertices[3];
			Stuff::Vector4D screenPos;
			
			DWORD alphaMode = ((WeaponBoltTypePtr)getObjectType())->boltAlpha;
			alphaMode <<= 24;
			
			DWORD edgeMode = ((WeaponBoltTypePtr)getObjectType())->edgeAlpha;
			edgeMode <<= 24;
			
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_laser_tip]
			eye->projectForScreenXY(laserPosition,screenPos);
			lq1Vertices[0].x		= screenPos.x;
			lq1Vertices[0].y		= screenPos.y;
			lq1Vertices[0].z		= 0.1f;
			lq1Vertices[0].rhw		= screenPos.w;
			lq1Vertices[0].u		= 0.0f;
			lq1Vertices[0].v		= 0.0f;
			lq1Vertices[0].argb		= edgeMode + ((WeaponBoltTypePtr)getObjectType())->frontRGB;
	
			lq2Vertices[0].x		= screenPos.x;
			lq2Vertices[0].y		= screenPos.y;
			lq2Vertices[0].z		= 0.1f;
			lq2Vertices[0].rhw		= screenPos.w;
			lq2Vertices[0].u		= 0.0f;
			lq2Vertices[0].v		= 0.0f;
			lq2Vertices[0].argb		= edgeMode + ((WeaponBoltTypePtr)getObjectType())->frontRGB;
	
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_laser_v0]
			eye->projectForScreenXY(laserVertices[0],screenPos);
			lq1Vertices[1].x		= screenPos.x;
			lq1Vertices[1].y		= screenPos.y;
			lq1Vertices[1].z		= 0.1f;
			lq1Vertices[1].rhw		= screenPos.w;
			lq1Vertices[1].u		= 0.0f;
			lq1Vertices[1].v		= 0.0f;
			lq1Vertices[1].argb		= edgeMode + ((WeaponBoltTypePtr)getObjectType())->midEdgeRGB;
			
			lq3Vertices[1].x		= screenPos.x;
			lq3Vertices[1].y		= screenPos.y;
			lq3Vertices[1].z		= 0.1f;
			lq3Vertices[1].rhw		= screenPos.w;
			lq3Vertices[1].u		= 0.0f;
			lq3Vertices[1].v		= 0.0f;
			lq3Vertices[1].argb		= edgeMode + ((WeaponBoltTypePtr)getObjectType())->midEdgeRGB;
			
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_laser_v3]
			eye->projectForScreenXY(laserVertices[3],screenPos);
			lq1Vertices[2].x		= screenPos.x;
			lq1Vertices[2].y		= screenPos.y;
			lq1Vertices[2].z		= 0.1f;
			lq1Vertices[2].rhw		= screenPos.w;
			lq1Vertices[2].u		= 0.0f;
			lq1Vertices[2].v		= 0.0f;
			lq1Vertices[2].argb		= alphaMode + ((WeaponBoltTypePtr)getObjectType())->middleRGB;
			
			lq2Vertices[2].x		= screenPos.x;
			lq2Vertices[2].y		= screenPos.y;
			lq2Vertices[2].z		= 0.1f;
			lq2Vertices[2].rhw		= screenPos.w;
			lq2Vertices[2].u		= 0.0f;
			lq2Vertices[2].v		= 0.0f;
			lq2Vertices[2].argb		= alphaMode + ((WeaponBoltTypePtr)getObjectType())->middleRGB;
			
			lq3Vertices[0].x		= screenPos.x;
			lq3Vertices[0].y		= screenPos.y;
			lq3Vertices[0].z		= 0.1f;
			lq3Vertices[0].rhw		= screenPos.w;
			lq3Vertices[0].u		= 0.0f;
			lq3Vertices[0].v		= 0.0f;
			lq3Vertices[0].argb		= alphaMode + ((WeaponBoltTypePtr)getObjectType())->middleRGB;
			
			lq4Vertices[0].x		= screenPos.x;
			lq4Vertices[0].y		= screenPos.y;
			lq4Vertices[0].z		= 0.1f;
			lq4Vertices[0].rhw		= screenPos.w;
			lq4Vertices[0].u		= 0.0f;
			lq4Vertices[0].v		= 0.0f;
			lq4Vertices[0].argb		= alphaMode + ((WeaponBoltTypePtr)getObjectType())->middleRGB;
			
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_laser_v2]
			eye->projectForScreenXY(laserVertices[2],screenPos);
			lq3Vertices[2].x		= screenPos.x;
			lq3Vertices[2].y		= screenPos.y;
			lq3Vertices[2].z		= 0.1f;
			lq3Vertices[2].rhw		= screenPos.w;
			lq3Vertices[2].u		= 0.0f;
			lq3Vertices[2].v		= 0.0f;
			lq3Vertices[2].argb		= edgeMode + ((WeaponBoltTypePtr)getObjectType())->backRGB;
			
			lq4Vertices[2].x		= screenPos.x;
			lq4Vertices[2].y		= screenPos.y;
			lq4Vertices[2].z		= 0.1f;
			lq4Vertices[2].rhw		= screenPos.w;
			lq4Vertices[2].u		= 0.0f;
			lq4Vertices[2].v		= 0.0f;
			lq4Vertices[2].argb		= edgeMode + ((WeaponBoltTypePtr)getObjectType())->backRGB;
			
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_laser_v1]
			eye->projectForScreenXY(laserVertices[1],screenPos);
			lq2Vertices[1].x		= screenPos.x;
			lq2Vertices[1].y		= screenPos.y;
			lq2Vertices[1].z		= 0.1f;
			lq2Vertices[1].rhw		= screenPos.w;
			lq2Vertices[1].u		= 0.0f;
			lq2Vertices[1].v		= 0.0f;
			lq2Vertices[1].argb		= edgeMode + ((WeaponBoltTypePtr)getObjectType())->midEdgeRGB;
		
			lq4Vertices[1].x		= screenPos.x;
			lq4Vertices[1].y		= screenPos.y;
			lq4Vertices[1].z		= 0.1f;
			lq4Vertices[1].rhw		= screenPos.w;
			lq4Vertices[1].u		= 0.0f;
			lq4Vertices[1].v		= 0.0f;
			lq4Vertices[1].argb		= edgeMode + ((WeaponBoltTypePtr)getObjectType())->midEdgeRGB;

			if (vfxTrace) {
				const bool culled =
					(lq1Vertices[0].x > eye->viewMulX) || (lq1Vertices[0].y > eye->viewMulY) ||
					(lq1Vertices[0].x < eye->viewAddX) || (lq1Vertices[0].y < eye->viewAddY) ||
					(lq1Vertices[1].x > eye->viewMulX) || (lq1Vertices[1].y > eye->viewMulY) ||
					(lq1Vertices[1].x < eye->viewAddX) || (lq1Vertices[1].y < eye->viewAddY) ||
					(lq1Vertices[2].x > eye->viewMulX) || (lq1Vertices[2].y > eye->viewMulY) ||
					(lq1Vertices[2].x < eye->viewAddX) || (lq1Vertices[2].y < eye->viewAddY);
				printf("[VK_VFX_TRACE] bolt(laser) path=%s culled=%d view=[%.0f..%.0f,%.0f..%.0f] "
					"v0=(%.0f,%.0f) v1=(%.0f,%.0f) v2=(%.0f,%.0f) argb0=%08X\n",
					useImmediate ? "IMMEDIATE" : "addVertices", (int)culled,
					eye->viewAddX, eye->viewMulX, eye->viewAddY, eye->viewMulY,
					lq1Vertices[0].x, lq1Vertices[0].y, lq1Vertices[1].x, lq1Vertices[1].y,
					lq1Vertices[2].x, lq1Vertices[2].y, (unsigned)lq1Vertices[0].argb);
				fflush(stdout);
			}

			if (((lq1Vertices[0].x > eye->viewMulX) ||
				(lq1Vertices[0].y > eye->viewMulY) ||
				(lq1Vertices[0].x < eye->viewAddX) || 
				(lq1Vertices[0].y < eye->viewAddY) || 
				(lq1Vertices[1].x > eye->viewMulX) ||
				(lq1Vertices[1].y > eye->viewMulY) ||
				(lq1Vertices[1].x < eye->viewAddX) ||
				(lq1Vertices[1].y < eye->viewAddY) ||
				(lq1Vertices[2].x > eye->viewMulX) ||
				(lq1Vertices[2].y > eye->viewMulY) ||
				(lq1Vertices[2].x < eye->viewAddX) ||
				(lq1Vertices[2].y < eye->viewAddY)))
			{
			}
			else
			{
				if (useImmediate)
				{
					//------------------------
					// Draw em!
					gos_SetRenderState( gos_State_Texture, 0 );
			
					//--------------------------------
					//Set States for Software Renderer
					if (Environment.Renderer == 3)
					{
						gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
						gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
						gos_SetRenderState( gos_State_MonoEnable, 1);
						gos_SetRenderState( gos_State_Perspective, 0);
						gos_SetRenderState( gos_State_Clipping, 0);
						gos_SetRenderState( gos_State_AlphaTest, 0);
						gos_SetRenderState( gos_State_Specular, 0);
						gos_SetRenderState( gos_State_Dither, 0);
						gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulateAlpha);
						gos_SetRenderState( gos_State_Filter, gos_FilterNone);
						gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
					}
					//--------------------------------
					//Set States for Hardware Renderer	
					else
					{
						gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
						gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
						gos_SetRenderState( gos_State_MonoEnable, 0);
						gos_SetRenderState( gos_State_Perspective, 1);
						gos_SetRenderState( gos_State_Clipping, 2);
						gos_SetRenderState( gos_State_AlphaTest, 0);
						gos_SetRenderState( gos_State_Specular, 0);
						gos_SetRenderState( gos_State_Dither, 1);
						gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulateAlpha);
						gos_SetRenderState( gos_State_Filter, gos_FilterBiLinear);
						gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
					}

					gos_DrawTriangles(lq1Vertices,3);
				}
				else
				{
					mcTextureManager->addVertices(0xffffffff,lq1Vertices,MC2_DRAWALPHA);
				}
			}
	
			if (((lq2Vertices[0].x > eye->viewMulX) ||
				(lq2Vertices[0].y > eye->viewMulY) || 
				(lq2Vertices[0].x < eye->viewAddX) || 
				(lq2Vertices[0].y < eye->viewAddY) || 
				(lq2Vertices[1].x > eye->viewMulX) ||
				(lq2Vertices[1].y > eye->viewMulY) ||
				(lq2Vertices[1].x < eye->viewAddX) ||
				(lq2Vertices[1].y < eye->viewAddY) ||
				(lq2Vertices[2].x > eye->viewMulX) ||
				(lq2Vertices[2].y > eye->viewMulY) ||
				(lq2Vertices[2].x < eye->viewAddX) ||
				(lq2Vertices[2].y < eye->viewAddY)))
			{
			}
			else
			{
				if (useImmediate)
				{
					gos_DrawTriangles(lq2Vertices,3);
				}
				else
				{
					mcTextureManager->addVertices(0xffffffff,lq2Vertices,MC2_DRAWALPHA);
				}
			}
				
			if (((lq3Vertices[0].x > eye->viewMulX) ||
				(lq3Vertices[0].y > eye->viewMulY) || 
				(lq3Vertices[0].x < eye->viewAddX) || 
				(lq3Vertices[0].y < eye->viewAddY) || 
				(lq3Vertices[1].x > eye->viewMulX) ||
				(lq3Vertices[1].y > eye->viewMulY) ||
				(lq3Vertices[1].x < eye->viewAddX) ||
				(lq3Vertices[1].y < eye->viewAddY) ||
				(lq3Vertices[2].x > eye->viewMulX) ||
				(lq3Vertices[2].y > eye->viewMulY) ||
				(lq3Vertices[2].x < eye->viewAddX) ||
				(lq3Vertices[2].y < eye->viewAddY)))
			{
			}
			else
			{
				if (useImmediate)
				{
					gos_DrawTriangles(lq3Vertices,3);
				}
				else
				{
					mcTextureManager->addVertices(0xffffffff,lq3Vertices,MC2_DRAWALPHA);
				}
			}
	
			if (((lq4Vertices[0].x > eye->viewMulX) ||
				(lq4Vertices[0].y > eye->viewMulY) || 
				(lq4Vertices[0].x < eye->viewAddX) || 
				(lq4Vertices[0].y < eye->viewAddY) || 
				(lq4Vertices[1].x > eye->viewMulX) ||
				(lq4Vertices[1].y > eye->viewMulY) ||
				(lq4Vertices[1].x < eye->viewAddX) ||
				(lq4Vertices[1].y < eye->viewAddY) ||
				(lq4Vertices[2].x > eye->viewMulX) ||
				(lq4Vertices[2].y > eye->viewMulY) ||
				(lq4Vertices[2].x < eye->viewAddX) ||
				(lq4Vertices[2].y < eye->viewAddY)))
			{
			}
			else
			{
				if (useImmediate)
				{
					gos_DrawTriangles(lq4Vertices,3);
				}
				else
				{
					mcTextureManager->addVertices(0xffffffff,lq4Vertices,MC2_DRAWALPHA);
				}
			}
		}
		else if (!hitTarget && ((WeaponBoltTypePtr)getObjectType())->isBeam)
		{
			//---------------------------
			// Render the polygons here.
			gos_VERTEX	lq1Vertices[3], lq2Vertices[3], lq3Vertices[3], lq4Vertices[3];
			Stuff::Vector4D screenPos;
			
			DWORD alphaMode = ((WeaponBoltTypePtr)getObjectType())->boltAlpha;
			alphaMode <<= 24;
			
			DWORD edgeMode = ((WeaponBoltTypePtr)getObjectType())->edgeAlpha;
			edgeMode <<= 24;
			
			Stuff::Point3D actualPosition;
			Stuff::Vector3D hotSpot = *targetPosition;
			GameObjectPtr target = ObjectManager->getByWatchID(targetWID);
			if (target)
			{
				if (!target->isMech())
				{
					hotSpot = target->getPositionFromHS(targetHotSpot);
				}
				else
				{
					if (targetHotSpot)
						hotSpot = target->appearance->getHitNodeLeft();
					else
						hotSpot = target->appearance->getHitNodeRight(); 
				}
			}
				
			Stuff::Vector3D length;
			length.Subtract(hotSpot,position);
			float dist = length.GetApproximateLength();
			
 			float realUVRepeat = ((WeaponBoltTypePtr)getObjectType())->uvRepeat * dist / ((WeaponBoltTypePtr)getObjectType())->unitLength;
			
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_beam_v0]
			eye->projectForScreenXY(laserVertices[0],screenPos);
			lq1Vertices[0].x		= screenPos.x;
			lq1Vertices[0].y		= screenPos.y;
			lq1Vertices[0].z		= screenPos.z;
			lq1Vertices[0].rhw		= screenPos.w;
			lq1Vertices[0].u		= startUV;
			lq1Vertices[0].v		= 0.0f;
			lq1Vertices[0].argb		= edgeMode + ((WeaponBoltTypePtr)getObjectType())->midEdgeRGB;
	
			lq2Vertices[0].x		= screenPos.x;
			lq2Vertices[0].y		= screenPos.y;
			lq2Vertices[0].z		= screenPos.z;
			lq2Vertices[0].rhw		= screenPos.w;
			lq2Vertices[0].u		= startUV;
			lq2Vertices[0].v		= 0.0f;
			lq2Vertices[0].argb		= edgeMode + ((WeaponBoltTypePtr)getObjectType())->midEdgeRGB;
			
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_beam_v1]
 			eye->projectForScreenXY(laserVertices[1],screenPos);
			lq2Vertices[1].x		= screenPos.x;
			lq2Vertices[1].y		= screenPos.y;
			lq2Vertices[1].z		= screenPos.z;
			lq2Vertices[1].rhw		= screenPos.w;
			lq2Vertices[1].u		= startUV;
			lq2Vertices[1].v		= 0.999999f;
			lq2Vertices[1].argb		= alphaMode + ((WeaponBoltTypePtr)getObjectType())->midEdgeRGB;
	
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_beam_v2]
			eye->projectForScreenXY(laserVertices[2],screenPos);
			lq1Vertices[1].x		= screenPos.x;
			lq1Vertices[1].y		= screenPos.y;
			lq1Vertices[1].z		= screenPos.z;
			lq1Vertices[1].rhw		= screenPos.w;
			lq1Vertices[1].u		= startUV + realUVRepeat;
			lq1Vertices[1].v		= 0.0f;
			lq1Vertices[1].argb		= edgeMode + ((WeaponBoltTypePtr)getObjectType())->middleRGB;
			
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_beam_v3]
			eye->projectForScreenXY(laserVertices[3],screenPos);
			lq1Vertices[2].x		= screenPos.x;
			lq1Vertices[2].y		= screenPos.y;
			lq1Vertices[2].z		= screenPos.z;
			lq1Vertices[2].rhw		= screenPos.w;
			lq1Vertices[2].u		= startUV + realUVRepeat; 
			lq1Vertices[2].v		= 0.999999f;
			lq1Vertices[2].argb		= alphaMode + ((WeaponBoltTypePtr)getObjectType())->middleRGB;
			
			lq2Vertices[2].x		= screenPos.x;
			lq2Vertices[2].y		= screenPos.y;
			lq2Vertices[2].z		= screenPos.z;
			lq2Vertices[2].rhw		= screenPos.w;
			lq2Vertices[2].u		= startUV + realUVRepeat; 
			lq2Vertices[2].v		= 0.999999f;
			lq2Vertices[2].argb		= alphaMode + ((WeaponBoltTypePtr)getObjectType())->middleRGB;
			
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_beam_side0]
			eye->projectForScreenXY(laserSide[0],screenPos);
			lq3Vertices[0].x		= screenPos.x;
			lq3Vertices[0].y		= screenPos.y;
			lq3Vertices[0].z		= screenPos.z;
			lq3Vertices[0].rhw		= screenPos.w;
			lq3Vertices[0].u		= startUV;
			lq3Vertices[0].v		= 0.0f;
			lq3Vertices[0].argb		= edgeMode + ((WeaponBoltTypePtr)getObjectType())->midEdgeRGB;
	
			lq4Vertices[0].x		= screenPos.x;
			lq4Vertices[0].y		= screenPos.y;
			lq4Vertices[0].z		= screenPos.z;
			lq4Vertices[0].rhw		= screenPos.w;
			lq4Vertices[0].u		= startUV;
			lq4Vertices[0].v		= 0.0f;
			lq4Vertices[0].argb		= edgeMode + ((WeaponBoltTypePtr)getObjectType())->midEdgeRGB;
			
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_beam_side1]
 			eye->projectForScreenXY(laserSide[1],screenPos);
			lq4Vertices[1].x		= screenPos.x;
			lq4Vertices[1].y		= screenPos.y;
			lq4Vertices[1].z		= screenPos.z;
			lq4Vertices[1].rhw		= screenPos.w;
			lq4Vertices[1].u		= startUV;
			lq4Vertices[1].v		= 0.999999f;
			lq4Vertices[1].argb		= alphaMode + ((WeaponBoltTypePtr)getObjectType())->midEdgeRGB;
	
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_beam_side2]
			eye->projectForScreenXY(laserSide[2],screenPos);
			lq3Vertices[1].x		= screenPos.x;
			lq3Vertices[1].y		= screenPos.y;
			lq3Vertices[1].z		= screenPos.z;
			lq3Vertices[1].rhw		= screenPos.w;
			lq3Vertices[1].u		= startUV + realUVRepeat;
			lq3Vertices[1].v		= 0.0f;
			lq3Vertices[1].argb		= edgeMode + ((WeaponBoltTypePtr)getObjectType())->middleRGB;
			
			// [PROJECTZ:ScreenXYOracle id=weaponbolt_beam_side3]
			eye->projectForScreenXY(laserSide[3],screenPos);
			lq3Vertices[2].x		= screenPos.x;
			lq3Vertices[2].y		= screenPos.y;
			lq3Vertices[2].z		= screenPos.z;
			lq3Vertices[2].rhw		= screenPos.w;
			lq3Vertices[2].u		= startUV + realUVRepeat; 
			lq3Vertices[2].v		= 0.999999f;
			lq3Vertices[2].argb		= alphaMode + ((WeaponBoltTypePtr)getObjectType())->middleRGB;
			
			lq4Vertices[2].x		= screenPos.x;
			lq4Vertices[2].y		= screenPos.y;
			lq4Vertices[2].z		= screenPos.z;
			lq4Vertices[2].rhw		= screenPos.w;
			lq4Vertices[2].u		= startUV + realUVRepeat; 
			lq4Vertices[2].v		= 0.999999f;
			lq4Vertices[2].argb		= alphaMode + ((WeaponBoltTypePtr)getObjectType())->middleRGB;

			if (vfxTrace) {
				printf("[VK_VFX_TRACE] beam path=%s view=[%.0f..%.0f,%.0f..%.0f] "
					"v0=(%.0f,%.0f,z%.3f) v1=(%.0f,%.0f,z%.3f) v2=(%.0f,%.0f,z%.3f) argb0=%08X tex=%u\n",
					useImmediate ? "IMMEDIATE" : "addVertices",
					eye->viewAddX, eye->viewMulX, eye->viewAddY, eye->viewMulY,
					lq1Vertices[0].x, lq1Vertices[0].y, lq1Vertices[0].z,
					lq1Vertices[1].x, lq1Vertices[1].y, lq1Vertices[1].z,
					lq1Vertices[2].x, lq1Vertices[2].y, lq1Vertices[2].z,
					(unsigned)lq1Vertices[0].argb, (unsigned)gosTextureHandle);
				fflush(stdout);
			}

			if (useImmediate)
			{
 				//------------------------
				// Draw em!
				gos_SetRenderState( gos_State_Texture, 0 );
		
				//--------------------------------
				//Set States for Software Renderer
				if (Environment.Renderer == 3)
				{
					gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneInvAlpha);
					gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
					gos_SetRenderState( gos_State_MonoEnable, 1);
					gos_SetRenderState( gos_State_Perspective, 0);
					gos_SetRenderState( gos_State_Clipping, 1);
					gos_SetRenderState( gos_State_AlphaTest, 0);
					gos_SetRenderState( gos_State_Specular, 0);
					gos_SetRenderState( gos_State_Dither, 0);
					gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulateAlpha);
					gos_SetRenderState( gos_State_Filter, gos_FilterNone);
					gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
					gos_SetRenderState( gos_State_ZCompare, 1);
					gos_SetRenderState(	gos_State_ZWrite, 0);
				}
				//--------------------------------
				//Set States for Hardware Renderer
				else
				{
					// VK-FIELD-VFX-3 (look-restore): the VK HUD tail now composites the
					// native effect blend AlphaMode OneInvAlpha(4) as a textured tri-batch
					// (vkHudDrawRepresentable accepts 4 + effect sprites carry an rgba mirror),
					// so the beam renders with its real additive-style blend just like GL.
					// Round-2's AlphaInvAlpha(3) remap is retired — it was the "weird laser".
					gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneInvAlpha);
					gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
					gos_SetRenderState( gos_State_MonoEnable, 0);
					gos_SetRenderState( gos_State_Perspective, 1);
					gos_SetRenderState( gos_State_Clipping, 1);
					gos_SetRenderState( gos_State_AlphaTest, 0);
					gos_SetRenderState( gos_State_Specular, 0);
					gos_SetRenderState( gos_State_Dither, 1);
					gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulateAlpha);
					gos_SetRenderState( gos_State_Filter, gos_FilterBiLinear);
					gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
					gos_SetRenderState( gos_State_ZCompare, 1);
					gos_SetRenderState(	gos_State_ZWrite, 0);
				}
			
				// VK-FIELD-VFX-4: the beam renders TEXTURED under the VK route too.
				// Round-3 forced it untextured because the beam sprite (realized via
				// the BC7 .ktx2 sidecar path in txmmgr, so no CPU rgba mirror) was
				// per-draw-declined by the tail. flushHUDBatch now builds the mirror
				// lazily by GPU readback (gosTexture::ensureRgbaMirrorFromGpu), and
				// if that ever fails the tail drops an AlphaMode-4 tri-batch to
				// untextured vertex-coloured (round-3's proven look) instead of
				// declining — the beam can no longer vanish. GL / umbrella-off
				// behaviour unchanged (this is the stock textured state-set).
				if (gosTextureHandle != 0xffffffff)
					gos_SetRenderState( gos_State_Texture,gosTextureHandle);
				else
					gos_SetRenderState( gos_State_Texture,0);

				gos_DrawTriangles(lq1Vertices,3);
				gos_DrawTriangles(lq2Vertices,3);
				gos_DrawTriangles(lq3Vertices,3);
				gos_DrawTriangles(lq4Vertices,3);
				// VK-FIELD-VFX-3 (look-restore): the round-2 beam-axis LINE fallback
				// (3 opaque horizontal lines) is retired — it was a visibility hack that
				// produced the flat "weird laser". The textured ribbon quads above are the
				// real beam and match GL. (If a far-zoom sub-pixel ribbon coverage delta
				// resurfaces it will be handled by the ribbon geometry, not a line proxy.)
			}
			else
			{
				if ((lq1Vertices[0].z >= 0.0f) &&
					(lq1Vertices[0].z < 1.0f) &&
					(lq1Vertices[1].z >= 0.0f) &&  
					(lq1Vertices[1].z < 1.0f) && 
					(lq1Vertices[2].z >= 0.0f) &&  
					(lq1Vertices[2].z < 1.0f))
					{
						mcTextureManager->addVertices(mcTextureHandle,lq1Vertices,MC2_ISEFFECTS|MC2_DRAWONEIN);
					}
					
				if ((lq2Vertices[0].z >= 0.0f) &&
					(lq2Vertices[0].z < 1.0f) &&
					(lq2Vertices[1].z >= 0.0f) &&  
					(lq2Vertices[1].z < 1.0f) && 
					(lq2Vertices[2].z >= 0.0f) &&  
					(lq2Vertices[2].z < 1.0f))
					{
						mcTextureManager->addVertices(mcTextureHandle,lq2Vertices,MC2_ISEFFECTS|MC2_DRAWONEIN);
					}
					
				if ((lq3Vertices[0].z >= 0.0f) &&
					(lq3Vertices[0].z < 1.0f) &&
					(lq3Vertices[1].z >= 0.0f) &&  
					(lq3Vertices[1].z < 1.0f) && 
					(lq3Vertices[2].z >= 0.0f) &&  
					(lq3Vertices[2].z < 1.0f))
					{
						mcTextureManager->addVertices(mcTextureHandle,lq3Vertices,MC2_ISEFFECTS|MC2_DRAWONEIN);
					}
					
				if ((lq4Vertices[0].z >= 0.0f) &&
					(lq4Vertices[0].z < 1.0f) &&
					(lq4Vertices[1].z >= 0.0f) &&  
					(lq4Vertices[1].z < 1.0f) && 
					(lq4Vertices[2].z >= 0.0f) &&  
					(lq4Vertices[2].z < 1.0f))
					{
						mcTextureManager->addVertices(mcTextureHandle,lq4Vertices,MC2_ISEFFECTS|MC2_DRAWONEIN);
					}
			}
				
			if (gosEffect)
			{
				Stuff::Point3D actualPosition;
				Stuff::Vector3D hotSpot = *targetPosition;
				GameObjectPtr target = ObjectManager->getByWatchID(targetWID);
				if (target)
				{
					if (!target->isMech())
					{
						hotSpot = target->getPositionFromHS(targetHotSpot);
					}
					else
					{
						if (targetHotSpot)
							hotSpot = target->appearance->getHitNodeLeft();
						else
							hotSpot = target->appearance->getHitNodeRight(); 
					}
				}
	
				actualPosition.x = -hotSpot.x;	//Play the beam hitting effect at the hit location
				actualPosition.y = hotSpot.z;
				actualPosition.z = hotSpot.y;
				
				Stuff::LinearMatrix4D 	shapeOrigin;
				Stuff::LinearMatrix4D	localToWorld;
				Stuff::LinearMatrix4D	localResult;
				
				shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
				shapeOrigin.BuildTranslation(actualPosition);
				
				Stuff::UnitQuaternion effectRot;
				effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
				localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
				localResult.Multiply(localToWorld,shapeOrigin);
				
				drawInfo.m_parentToWorld = &localResult;
		 
				if (!MLRVertexLimitReached)
					gosEffect->Draw(&drawInfo);
			}
		}
		else if (gosEffect)
		{
			Stuff::Point3D actualPosition;
			actualPosition.x = -laserPosition.x;
			actualPosition.y = laserPosition.z;
			actualPosition.z = laserPosition.y;
			
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;
			Stuff::LinearMatrix4D	localResult;
			
			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(actualPosition);
			
			Stuff::UnitQuaternion effectRot;
			effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
			localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
			localResult.Multiply(localToWorld,shapeOrigin);
			
 			drawInfo.m_parentToWorld = &localResult;
	 
			if (!MLRVertexLimitReached)
				gosEffect->Draw(&drawInfo);
		}
		
		if (muzzleEffect)
		{
			Stuff::Point3D mPosition;
			mPosition.x = -position.x;
			mPosition.y = position.z;
			mPosition.z = position.y;
			
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;
			Stuff::LinearMatrix4D	localResult;
			
			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(mPosition);
			
			GameObjectPtr myOwner = ObjectManager->getByWatchID(ownerWID);
			Stuff::UnitQuaternion effectRot;
		
			if (myOwner)
				effectRot = Stuff::EulerAngles(0.0f, myOwner->getRotation() * DEGREES_TO_RADS,0.0f);
			else
				effectRot = Stuff::EulerAngles(0.0f,0.0f,0.0f); 

			localToWorld.Multiply(gosFX::Effect_Into_Motion,effectRot);
			localResult.Multiply(localToWorld,shapeOrigin);
		
 			drawInfo.m_parentToWorld = &localResult;
	 
			// VFX-WEAPON-FX-RESTORE-OPUS-1: suppress muzzleEffect Draw when oracle ON + hitTarget.
			// muzzleEffect renders at bolt position (= target after impact); with oracle ON
			// it creates persistent bright cards at the impact site for the full afterHitTime.
			if (!MLRVertexLimitReached && !(hitTarget && mc2::particles::Batcher::is_oracle_render_enabled()))
				muzzleEffect->Draw(&drawInfo);
		}

		if (hitEffect && hitTarget && hitEffect->IsExecuted())
		{
			Stuff::Vector3D hotSpot = *targetPosition;
			GameObjectPtr target = ObjectManager->getByWatchID(targetWID);
			if (target)
			{
				if (!target->isMech())
				{
					hotSpot = target->getPositionFromHS(targetHotSpot);
				}
				else
				{
					if (targetHotSpot)
						hotSpot = target->appearance->getHitNodeLeft();
					else
						hotSpot = target->appearance->getHitNodeRight(); 
				}
			}

			Stuff::Point3D tPosition;
			tPosition.x = -hotSpot.x;
			tPosition.y = hotSpot.z;
			tPosition.z = hotSpot.y;
			
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;
			
			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(tPosition);
			
			drawInfo.m_parentToWorld = &shapeOrigin;
	 
			// VFX-WEAPON-FX-RESTORE-OPUS-1: suppress hitEffect Draw when oracle is ON.
			// hitEffect was always invisible in original MC2 (MLR-gated). Oracle renders
			// it as oversized bright cards. The game's createExplosion() provides impact
			// visual. Re-enable when hitEffect visuals are tuned for GPU billboard path.
			if (!MLRVertexLimitReached && !mc2::particles::Batcher::is_oracle_render_enabled())
				hitEffect->Draw(&drawInfo);
		}

		if (missEffect && hitTarget && missEffect->IsExecuted())
		{
			Stuff::Vector3D hotSpot = *targetPosition;
			
			Stuff::Point3D tPosition;
			tPosition.x = -hotSpot.x;
			tPosition.y = hotSpot.z;
			tPosition.z = hotSpot.y;
			
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;
			
			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(tPosition);
			
			drawInfo.m_parentToWorld = &shapeOrigin;
	 
			// VFX-WEAPON-FX-RESTORE-OPUS-1: suppress missEffect Draw when oracle ON.
			// Was invisible pre-MLR-restore (gated); now renders as persistent card.
			// Re-enable when tuned for GPU billboard path.
			if (!MLRVertexLimitReached && !mc2::particles::Batcher::is_oracle_render_enabled())
				missEffect->Draw(&drawInfo);
		}

		if (waterMissEffect && hitTarget && waterMissEffect->IsExecuted())
		{
			Stuff::Vector3D hotSpot = *targetPosition;
			
			Stuff::Point3D tPosition;
			tPosition.x = -hotSpot.x;
			tPosition.y = Terrain::waterElevation; 
			tPosition.z = hotSpot.y;
			
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;
			
			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(tPosition);
			
			drawInfo.m_parentToWorld = &shapeOrigin;
	 
			// VFX-WEAPON-FX-RESTORE-OPUS-1: suppress waterMissEffect Draw when oracle ON.
			if (!MLRVertexLimitReached && !mc2::particles::Batcher::is_oracle_render_enabled())
				waterMissEffect->Draw(&drawInfo);
		}
	}
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

//---------------------------------------------------------------------------
