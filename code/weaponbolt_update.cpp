// WeaponBolt: the per-frame bolt update (flight, impact, direct-fire modes) and its speed/straight-fire gates.
// Declared in the class headers; shared names in weaponbolt_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "weaponbolt_internal.h"
#include "appearance_rng.h"
#include "../RenderCore/axis_convert.h"
#include "Gates/gate.h"


//---------------------------------------------------------------------------
// PPC de-curve + direct-fire speed (env-gated, default-off). "Direct-fire" =
// non-arc, non-beam bolts (PPC / autocannon / gauss). MC2_DIRECT_FIRE_STRAIGHT=1
// makes them fly STRAIGHT (no per-frame target re-track) to a one-time aim that
// is LED by the target's velocity over the bolt's flight time, so a pre-rolled
// hit on a moving target still lands. LRM (arcEffect) + beams keep tracking.
// MC2_PROJECTILE_SPEED_MULT scales direct-fire travel speed (default 1.0).
static bool directFireStraightEnabled (void)
{
	static const bool s = mc2::gate::get<"MC2_DIRECT_FIRE_STRAIGHT">();
	return s;
}
static float directFireSpeedMult (void)
{
	static const float s = mc2::gate::get<"MC2_PROJECTILE_SPEED_MULT">();   // the row reads 1 outside (0, 10]
	return s;
}

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
		GameObjectPtr myOwner = ObjectManager->getByWatchID(ownerWID);
		if (myOwner)
		{
			position = myOwner->getPositionFromHS(hotSpotNumber);
		}

		if (((WeaponBoltTypePtr)getObjectType())->fireSoundFX != 0)
			soundSystem->playDigitalSample(((WeaponBoltTypePtr)getObjectType())->fireSoundFX, position, true);
			
		laserPosition = position;
		
		Stuff::Vector3D ownerPosition = laserPosition;
		Stuff::Point3D actualPosition;
		mc2ToRender(actualPosition, laserPosition);

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
			
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,nullptr);
			gosEffect->Start(&info);
		}
		
		if (muzzleEffect && muzzleEffect->IsExecuted())
		{
			Stuff::Point3D			mPosition;
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;
			Stuff::LinearMatrix4D	localResult;
			
			mc2ToRender(mPosition, position);
			
 			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(mPosition);
			
			Stuff::UnitQuaternion effectRot;
			if (myOwner)
				effectRot = Stuff::EulerAngles(0.0f, myOwner->getRotation() * DEGREES_TO_RADS,0.0f);
			else
				effectRot = Stuff::EulerAngles(0.0f, 0.0f ,0.0f);

			localToWorld.Multiply(gosFX::Effect_Into_Motion,effectRot);
			localResult.Multiply(localToWorld,shapeOrigin);
			
 			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,nullptr);
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

	GameObjectPtr myOwner = ObjectManager->getByWatchID(ownerWID);
	if (myOwner)
		position = myOwner->getPositionFromHS(hotSpotNumber);
	
	bool inView = TRUE;

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
					gosEffect = nullptr;
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
						
						mc2ToRender(tPosition, hotSpot);
						
						shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
						shapeOrigin.BuildTranslation(tPosition);
						
						gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);
						hitEffect->Start(&info);
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
							
							gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);
							waterMissEffect->Start(&info);
						}
							
						if (missEffect)
						{
							missEffect->Kill();
							delete missEffect;
							missEffect = nullptr;
						}
					}
					else
					{
						if (missEffect && missEffect->IsExecuted())
						{
							Stuff::Point3D			tPosition;
							Stuff::LinearMatrix4D 	shapeOrigin;
							Stuff::LinearMatrix4D	localToWorld;
							
							mc2ToRender(tPosition, hotSpot);
							
							shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
							shapeOrigin.BuildTranslation(tPosition);
							
							gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);
							missEffect->Start(&info);
						}
						
						if (waterMissEffect)
						{
							waterMissEffect->Kill();
							delete waterMissEffect;
							waterMissEffect = nullptr;
						}
					}
					
					if (hitEffect)
					{
						hitEffect->Kill();
						delete hitEffect;
						hitEffect = nullptr;
					}
					
					laserPosition = hotSpot;
				}
				
				if (target)
				{
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
						if (hitEffect)
						{
							hitEffect->Kill();
							delete hitEffect;
							hitEffect = nullptr;
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
						
						//Create the explosion here.  HIT or MISS, its the same one!!
						// Damage is done by explosion NOT by hit!
						ObjectManager->createExplosion(((WeaponBoltTypePtr)getObjectType())->hitEffectObjNum,
														nullptr,*targetPosition,
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
						craterManager->addCrater(CRATER_1+randomCrater,*targetPosition,mc2_appearance_rng::randomNumber(180));
		
					//if (MasterComponentList[weaponShot.masterId].getWeaponAmmoType() != WEAPON_AMMO_NONE)	
					// energy weapons don't set off mines
					// Yes, they should DT, 5/30/98
					{
						int cellRow, cellCol;
		
						land->worldToCell(*targetPosition, cellRow, cellCol);
						if (GameMap->getMine(cellRow, cellCol) == 1)
						{
							ObjectManager->createExplosion(MINE_EXPLOSION_ID, nullptr, *targetPosition, MineSplashDamage, MineSplashRange * worldUnitsPerMeter);
							GameMap->setMine(cellRow, cellCol, 2);
						}
					}

					if (((WeaponBoltTypePtr)getObjectType())->hitEffectObjNum &&
						(((WeaponBoltTypePtr)getObjectType())->areaEffectDmg > 0.0f) &&
						(((WeaponBoltTypePtr)getObjectType())->areaEffectRad > 0.0f))
					{
						if (hitEffect)
						{
							hitEffect->Kill();
							delete hitEffect;
							hitEffect = nullptr;
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

						// ISSUE-9 S2 (MC2_CALLED_SHOT_FIX): successful aimed shot —
						// deliver located damage to the aimed target and keep the
						// explosion cosmetic (0 damage). Stock path otherwise.
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
						gosEffect = nullptr;
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
						
						mc2ToRender(tPosition, hotSpot);
						
						shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
						shapeOrigin.BuildTranslation(tPosition);
						
						gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);
						hitEffect->Start(&info);
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
							
							gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);
							waterMissEffect->Start(&info);
						}
						
						if (missEffect)
						{
							missEffect->Kill();
							delete missEffect;
							missEffect = nullptr;
						}
					}
					else
					{
						if (missEffect && missEffect->IsExecuted())
						{
							Stuff::Point3D			tPosition;
							Stuff::LinearMatrix4D 	shapeOrigin;
							Stuff::LinearMatrix4D	localToWorld;
							
							mc2ToRender(tPosition, hotSpot);
							
							shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
							shapeOrigin.BuildTranslation(tPosition);
							
							gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);
							missEffect->Start(&info);
						}
						
						if (waterMissEffect)
						{
							waterMissEffect->Kill();
							delete waterMissEffect;
							waterMissEffect = nullptr;
						}
					}
					
					if (hitEffect)
					{
						hitEffect->Kill();
						delete hitEffect;
						hitEffect = nullptr;
					}
				}
				
				if (target)
				{
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
						if (hitEffect)
						{
							hitEffect->Kill();
							delete hitEffect;
							hitEffect = nullptr;
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
						
 						//Create the explosion here.  HIT or MISS, its the same one!!
						// Damage is done by explosion NOT by hit!
						ObjectManager->createExplosion(((WeaponBoltTypePtr)getObjectType())->hitEffectObjNum,
														nullptr,*targetPosition,
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
						craterManager->addCrater(CRATER_1+randomCrater,*targetPosition,mc2_appearance_rng::randomNumber(180));
		
					//if (MasterComponentList[weaponShot.masterId].getWeaponAmmoType() != WEAPON_AMMO_NONE)	
					// energy weapons don't set off mines
					// Yes, they should DT, 5/30/98
					{
						int cellRow, cellCol;
		
						land->worldToCell(*targetPosition, cellRow, cellCol);
						if (GameMap->getMine(cellRow, cellCol) == 1)
						{
							ObjectManager->createExplosion(MINE_EXPLOSION_ID, nullptr, *targetPosition, MineSplashDamage, MineSplashRange * worldUnitsPerMeter);
							GameMap->setMine(cellRow, cellCol, 2);
						}
					}
					
					if (((WeaponBoltTypePtr)getObjectType())->hitEffectObjNum &&
						(((WeaponBoltTypePtr)getObjectType())->areaEffectDmg > 0.0f) &&
						(((WeaponBoltTypePtr)getObjectType())->areaEffectRad > 0.0f))
					{
						if (hitEffect)
						{
							hitEffect->Kill();
							delete hitEffect;
							hitEffect = nullptr;
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
						
 						//Create the explosion here.  HIT or MISS, its the same one!!
						// Damage is done by explosion NOT by hit!
						ObjectManager->createExplosion(((WeaponBoltTypePtr)getObjectType())->hitEffectObjNum,
														nullptr,*targetPosition,
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
			mc2ToRender(actualPosition, laserPosition);
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
			gosEffect = nullptr;
		}
   	}
	
   	if (muzzleEffect && muzzleEffect->IsExecuted())
   	{
		Stuff::Point3D mPosition;
		mc2ToRender(mPosition, position);
	
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
			muzzleEffect = nullptr;
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
				pointLight = nullptr;
			}
		}
	}
	else if (!hitTarget && ((WeaponBoltTypePtr)getObjectType())->isBeam)
	{
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

		float beamWiggle = ((WeaponBoltTypePtr)getObjectType())->beamWiggle;
		Stuff::Vector3D offset(beamWiggle,beamWiggle,beamWiggle);
		offset.x = mc2_appearance_rng::randomNumber(offset.x * 2) - offset.x;
		offset.y = mc2_appearance_rng::randomNumber(offset.y * 2) - offset.y;
		offset.z = mc2_appearance_rng::randomNumber(offset.z * 2) - offset.z;
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
		mc2ToRender(ourPosition, laserPosition);

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
			gosEffect = nullptr;
			
			if (pointLight)
			{
				eye->removeWorldLight(lightId,pointLight);
				systemHeap->Free(pointLight);
				pointLight = nullptr;
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
			mc2ToRender(tPosition, hotSpot);
		
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
					hitEffect = nullptr;
				}
					
				inView = FALSE;
				
				if (pointLight)
				{
					eye->removeWorldLight(lightId,pointLight);
					systemHeap->Free(pointLight);
					pointLight = nullptr;
				}
				
				if (muzzleEffect)
				{
					muzzleEffect->Kill();
					delete muzzleEffect;
					muzzleEffect = nullptr;
				}
			}
		}
		else
		{
			if (hitEffect)
			{
				hitEffect->Kill();		//Effect is over.  Otherwise, wait until hit!
				delete hitEffect;
				hitEffect = nullptr;
			}
				
			if (pointLight)
			{
				eye->removeWorldLight(lightId,pointLight);
				systemHeap->Free(pointLight);
				pointLight = nullptr;
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
			mc2ToRender(tPosition, hotSpot);
		
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
					missEffect = nullptr;
				}
					
				if (muzzleEffect)
				{
					muzzleEffect->Kill();
					delete muzzleEffect;
					muzzleEffect = nullptr;
				}
				
				inView = FALSE;
				
				if (pointLight)
				{
					eye->removeWorldLight(lightId,pointLight);
					systemHeap->Free(pointLight);
					pointLight = nullptr;
				}
			}
		}
		else
		{
			if (missEffect)
			{
				missEffect->Kill();		//Effect is over.  Otherwise, wait until hit!
				delete missEffect;
				missEffect = nullptr;
			}
				
			if (pointLight)
			{
				eye->removeWorldLight(lightId,pointLight);
				systemHeap->Free(pointLight);
				pointLight = nullptr;
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
					waterMissEffect = nullptr;
				}
					
				if (muzzleEffect)
				{
					muzzleEffect->Kill();
					delete muzzleEffect;
					muzzleEffect = nullptr;
				}
				
				inView = FALSE;
				
				if (pointLight)
				{
					eye->removeWorldLight(lightId,pointLight);
					systemHeap->Free(pointLight);
					pointLight = nullptr;
				}
			}
		}
		else
		{
			if (waterMissEffect)
			{
				waterMissEffect->Kill();		//Effect is over.  Otherwise, wait until hit!
				delete waterMissEffect;
				waterMissEffect = nullptr;
			}
				
			if (pointLight)
			{
				eye->removeWorldLight(lightId,pointLight);
				systemHeap->Free(pointLight);
				pointLight = nullptr;
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
		mc2ToRender(swapped, laserPosition);
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
