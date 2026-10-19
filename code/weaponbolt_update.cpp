// WeaponBolt: the per-frame bolt update (flight, impact, direct-fire modes) and its speed/straight-fire gates.
// Declared in the class headers; shared names in weaponbolt_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "../mclib/texture_assets.h"
#include "weaponbolt_internal.h"
#include "appearance_rng.h"
#include "sim_trace.h"
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

	glm::vec3 laserDirection;
	
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
		
		glm::vec3 ownerPosition = laserPosition;
		glm::vec3 actualPosition;
		mc2ToRender(actualPosition, laserPosition);

		GameObjectPtr target = ObjectManager->getByWatchID(targetWID);

		glm::vec3 targetPos;
		targetPos = glm::vec3(0.0f);
		if (target)
		{
			glm::vec3 aim;
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
				glm::vec3 toTgt;
				toTgt = aim - ownerPosition;
				float dist = glm::length(toTgt);
				float spd  = bt->velocity * directFireSpeedMult();
				float tof  = (spd > 0.01f) ? (dist / spd) : 0.0f;
				glm::vec3 lead;
				lead = target->getVelocity() * tof;
				aim = aim + lead;
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
					
		laserDirection = targetPos - ownerPosition;

		if (glm::length(laserDirection) != 0.0)
			laserDirection = glm::normalize(laserDirection);

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
			glm::mat4 	shapeOrigin;
			glm::mat4	localToWorld;
			glm::mat4	localResult;
			
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, actualPosition);
			
			rotation = -world_angle_between(position, *targetPosition);

			glm::quat effectRot;
			effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,rotation * DEGREES_TO_RADS,0.0f));
			localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
			localResult = localToWorld * shapeOrigin;
			
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,nullptr);
			gosEffect->Start(&info);
		}
		
		if (muzzleEffect && muzzleEffect->IsExecuted())
		{
			glm::vec3			mPosition;
			glm::mat4 	shapeOrigin;
			glm::mat4	localToWorld;
			glm::mat4	localResult;
			
			mc2ToRender(mPosition, position);
			
 			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, mPosition);
			
			glm::quat effectRot;
			if (myOwner)
				effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, myOwner->getRotation() * DEGREES_TO_RADS,0.0f));
			else
				effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, 0.0f ,0.0f));

			localToWorld = mc2::math::multiply(gosFX::Effect_Into_Motion, effectRot);
			localResult = localToWorld * shapeOrigin;
			
 			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,nullptr);
			muzzleEffect->Start(&info);
		}
		
		glm::vec3 laserVelocity;
		laserVelocity = targetPos - ownerPosition;
		float distance = laserVelocity.x * laserVelocity.x + laserVelocity.y * laserVelocity.y;
		distanceToTarget = distance;
		halfDistanceToTarget = distance / 2.0f;
		
		goalHeight = ((WeaponBoltTypePtr)getObjectType())->arcHeight;
		goalHeight *= 10.0f;
	}


	GameObjectPtr myOwner = ObjectManager->getByWatchID(ownerWID);
	if (myOwner)
		position = myOwner->getPositionFromHS(hotSpotNumber);
	
	bool inView = TRUE;

	if (((WeaponBoltTypePtr)getObjectType())->isBeam)
	{
		timeLeft -= frameLength;
		if (timeLeft >= 0.0f)
		{
			gosTextureHandle = mc2::textures::sourceId(mcTextureHandle);
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
		
	glm::vec3 ownerPosition = laserPosition;
	GameObjectPtr target = ObjectManager->getByWatchID(targetWID);
		
	glm::vec3 targetPos;
	targetPos = glm::vec3(0.0f);
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
				
	glm::vec3 laserVelocity;
	float velMag = ((WeaponBoltTypePtr)getObjectType())->velocity;
	velMag *= frameLength;

	// Direct-fire travel speed preserves the pre-rolled hit/miss outcome.
	if (!((WeaponBoltTypePtr)getObjectType())->arcEffect)
		velMag *= directFireSpeedMult();

	laserVelocity = targetPos - ownerPosition;
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

	if (!hitTarget)
	{
		if (!((WeaponBoltTypePtr)getObjectType())->isBeam)
		{
			float lastDistanceMoved = distanceToTarget - distance;
#if MC2_SIM_TRACE
			if (target && target->isMech()) {
				const glm::vec3 tp = target->getPosition();
				MC2_SIM_TRACE_LINE("BOLT id=%ld tgt=%ld dist=%.4f last=%.4f tpos=%.4f,%.4f,%.4f mpos=%.4f,%.4f,%.4f", static_cast<long>(getPartId()), static_cast<long>(target->getPartId()), static_cast<double>(distance), static_cast<double>(lastDistanceMoved),
				                   (double)targetPos.x, (double)targetPos.y, (double)targetPos.z, (double)tp.x, (double)tp.y, (double)tp.z);
			}
#endif
			distanceToTarget = distance;
			
			if (lastDistanceMoved >= distance)
			{
				hitTarget = TRUE;
				if (target) traceCombat("impact", "hit");
				hitLeft = ((WeaponBoltTypePtr)getObjectType())->afterHitTime;

				// Only the impact effect remains visible after arrival.
				if (gosEffect) {
					gosEffect->Kill();
					gosEffect.reset();
				}

				if (target)
				{
					glm::vec3 hotSpot = target->getPositionFromHS(targetHotSpot);

					if (target->isMech())
					{
						if (targetHotSpot)
							hotSpot = target->appearance->getHitNodeLeft();
						else
							hotSpot = target->appearance->getHitNodeRight(); 

					}

					if (hitEffect && hitEffect->IsExecuted())
					{
						glm::vec3			tPosition;
						glm::mat4 	shapeOrigin;
						glm::mat4	localToWorld;
						
						mc2ToRender(tPosition, hotSpot);
						
						mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
						mc2::math::buildTranslation(shapeOrigin, tPosition);
						
						gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);
						hitEffect->Start(&info);
					}
					
					if (missEffect)
					{
						missEffect->Kill();
						missEffect.reset();
					}
					
					if (waterMissEffect)
					{
						waterMissEffect->Kill();
						waterMissEffect.reset();
					}
					
					laserPosition = hotSpot;
				}
				else
				{
					glm::vec3 hotSpot = *targetPosition;
					int cellR, cellC;
					land->worldToCell(hotSpot,cellR, cellC);
					if (GameMap->getDeepWater(cellR, cellC) || GameMap->getShallowWater(cellR, cellC))
					{
						traceCombat("impact", "water_miss");
						if (waterMissEffect && waterMissEffect->IsExecuted())
						{
							glm::vec3			tPosition;
							glm::mat4 	shapeOrigin;
							glm::mat4	localToWorld;
							
							tPosition.x = -hotSpot.x;
							tPosition.y = Terrain::waterElevation;
							tPosition.z = hotSpot.y;
							
							mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
							mc2::math::buildTranslation(shapeOrigin, tPosition);
							
							gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);
							waterMissEffect->Start(&info);
						}
							
						if (missEffect)
						{
							missEffect->Kill();
							missEffect.reset();
						}
					}
					else
					{
						traceCombat("impact", "miss");
						if (missEffect && missEffect->IsExecuted())
						{
							glm::vec3			tPosition;
							glm::mat4 	shapeOrigin;
							glm::mat4	localToWorld;
							
							mc2ToRender(tPosition, hotSpot);
							
							mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
							mc2::math::buildTranslation(shapeOrigin, tPosition);
							
							gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);
							missEffect->Start(&info);
						}
						
						if (waterMissEffect)
						{
							waterMissEffect->Kill();
							waterMissEffect.reset();
						}
					}
					
					if (hitEffect)
					{
						hitEffect->Kill();
						hitEffect.reset();
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
							glm::vec3 targetPos = target->getPosition();
							glm::vec3 ownerPos = owner->getPosition();
							
							float zDistance = ownerPos.z - targetPos.z;
							targetPos.z = ownerPos.z = 0.0f;
							
							glm::vec3 distance;
							distance = ownerPos - targetPos;
							
							float yDistance = glm::length(distance);
							
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
							hitEffect.reset();
						}
						
						if (missEffect)
						{
							missEffect->Kill();
							missEffect.reset();
						}
						
						if (waterMissEffect)
						{
							waterMissEffect->Kill();
							waterMissEffect.reset();
						}
						
						// The explosion owns area damage for both hits and misses.
						ObjectManager->createExplosion(((WeaponBoltTypePtr)getObjectType())->hitEffectObjNum,
														nullptr,*targetPosition,
														((WeaponBoltTypePtr)getObjectType())->areaEffectDmg,
														((WeaponBoltTypePtr)getObjectType())->areaEffectRad);
					}
					else
					{
						submitDamage(target);
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
							hitEffect.reset();
						}

						if (missEffect)
						{
							missEffect->Kill();
							missEffect.reset();
						}

						if (waterMissEffect)
						{
							waterMissEffect->Kill();
							waterMissEffect.reset();
						}

						// Located aimed damage leaves the explosion cosmetic to avoid duplicate damage.
						float areaDmg = ((WeaponBoltTypePtr)getObjectType())->areaEffectDmg;
						if (deliverAimedAreaHit())
							areaDmg = 0.0f;

						// The explosion owns area damage for both hits and misses.
						ObjectManager->createExplosion(((WeaponBoltTypePtr)getObjectType())->hitEffectObjNum,
														nullptr,*targetPosition,
														areaDmg,
														((WeaponBoltTypePtr)getObjectType())->areaEffectRad);
					}
				}
				armRetireDeadline();
			}
		}
		else
		{
			if (timeLeft < 0.0)
			{
				hitTarget = TRUE;
				if (target) traceCombat("impact", "hit");

					// The beam head must not remain visible through the impact tail.
					if (gosEffect) {
						gosEffect->Kill();
						gosEffect.reset();
					}

				if (target)
				{
					glm::vec3 hotSpot = target->getPositionFromHS(targetHotSpot);
					if (target->isMech())
					{
						if (targetHotSpot)
							hotSpot = target->appearance->getHitNodeLeft();
						else
							hotSpot = target->appearance->getHitNodeRight();
					}

					if (hitEffect && hitEffect->IsExecuted())
					{
						glm::vec3			tPosition;
						glm::mat4 	shapeOrigin;
						glm::mat4	localToWorld;
						
						mc2ToRender(tPosition, hotSpot);
						
						mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
						mc2::math::buildTranslation(shapeOrigin, tPosition);
						
						gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);
						hitEffect->Start(&info);
					}
					
					if (missEffect)
					{
						missEffect->Kill();
						missEffect.reset();
					}
					
					if (waterMissEffect)
					{
						waterMissEffect->Kill();
						waterMissEffect.reset();
					}
				}
				else
				{
					glm::vec3 hotSpot = *targetPosition;
					int cellR, cellC;
					land->worldToCell(hotSpot,cellR, cellC);
					if (GameMap->getDeepWater(cellR, cellC) || GameMap->getShallowWater(cellR, cellC))
					{
						traceCombat("impact", "water_miss");
						if (waterMissEffect && waterMissEffect->IsExecuted())
						{
							glm::vec3			tPosition;
							glm::mat4 	shapeOrigin;
							glm::mat4	localToWorld;
							
							tPosition.x = -hotSpot.x;
							tPosition.y = Terrain::waterElevation;
							tPosition.z = hotSpot.y;
							
							mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
							mc2::math::buildTranslation(shapeOrigin, tPosition);
							
							gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);
							waterMissEffect->Start(&info);
						}
						
						if (missEffect)
						{
							missEffect->Kill();
							missEffect.reset();
						}
					}
					else
					{
						traceCombat("impact", "miss");
						if (missEffect && missEffect->IsExecuted())
						{
							glm::vec3			tPosition;
							glm::mat4 	shapeOrigin;
							glm::mat4	localToWorld;
							
							mc2ToRender(tPosition, hotSpot);
							
							mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
							mc2::math::buildTranslation(shapeOrigin, tPosition);
							
							gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);
							missEffect->Start(&info);
						}
						
						if (waterMissEffect)
						{
							waterMissEffect->Kill();
							waterMissEffect.reset();
						}
					}
					
					if (hitEffect)
					{
						hitEffect->Kill();
						hitEffect.reset();
					}
				}
				
				if (target)
				{
					GameObjectPtr owner = ObjectManager->getByWatchID(ownerWID);
					if (owner)
					{
						if (target->getPosition().z < owner->getPosition().z)
						{
							glm::vec3 targetPos = target->getPosition();
							glm::vec3 ownerPos = owner->getPosition();

							float zDistance = ownerPos.z - targetPos.z;
							targetPos.z = ownerPos.z = 0.0f;

							glm::vec3 distance;
							distance = ownerPos - targetPos;

							float yDistance = glm::length(distance);

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
							hitEffect.reset();
						}
						
						if (missEffect)
						{
							missEffect->Kill();
							missEffect.reset();
						}
						
						if (waterMissEffect)
						{
							waterMissEffect->Kill();
							waterMissEffect.reset();
						}
						
						// The explosion owns area damage for both hits and misses.
						ObjectManager->createExplosion(((WeaponBoltTypePtr)getObjectType())->hitEffectObjNum,
														nullptr,*targetPosition,
														((WeaponBoltTypePtr)getObjectType())->areaEffectDmg,
														((WeaponBoltTypePtr)getObjectType())->areaEffectRad);
					}
					else
					{
						submitDamage(target);
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
							hitEffect.reset();
						}
						
						if (missEffect)
						{
							missEffect->Kill();
							missEffect.reset();
						}
						
						if (waterMissEffect)
						{
							waterMissEffect->Kill();
							waterMissEffect.reset();
						}
						
						// The explosion owns area damage for both hits and misses.
						ObjectManager->createExplosion(((WeaponBoltTypePtr)getObjectType())->hitEffectObjNum,
														nullptr,*targetPosition,
														((WeaponBoltTypePtr)getObjectType())->areaEffectDmg,
														((WeaponBoltTypePtr)getObjectType())->areaEffectRad);
					}
				}
				armRetireDeadline();
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
		
		if (glm::length(laserVelocity) != 0.0)
			laserVelocity = glm::normalize(laserVelocity);
	
		if (moveThisFrame)
		{
			laserVelocity *= velMag;
			if (isArcing)
				laserVelocity.z = tmpZ;
		}
		
		laserPosition += laserVelocity;
	
		laserDirection = -laserVelocity;
		if (glm::length(laserDirection) != 0.0)
			laserDirection = glm::normalize(laserDirection);
	}

   	if (gosEffect && gosEffect->IsExecuted())
   	{
		glm::vec3 actualPosition;
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
	
		glm::mat4 	shapeOrigin;
		glm::mat4 	localToWorld;
		glm::mat4 	localResult;
			
		mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
		mc2::math::buildTranslation(shapeOrigin, actualPosition);
		
		rotation = -world_angle_between(laserPosition, *targetPosition);
		
		glm::quat effectRot;
		effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,rotation * DEGREES_TO_RADS,0.0f));
		localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
		localResult = localToWorld * shapeOrigin;
 
		mc2::math::Obb boundingBox;
   		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,&boundingBox);

		bool result = gosEffect->Execute(&info);
		if (!result)
		{
			gosEffect->Kill();		//Effect is over.  Otherwise, wait until hit!
			gosEffect.reset();
		}
   	}
	
   	if (muzzleEffect && muzzleEffect->IsExecuted())
   	{
		glm::vec3 mPosition;
		mc2ToRender(mPosition, position);
	
		glm::mat4 	shapeOrigin;
		glm::mat4 	localToWorld;
		glm::mat4 	localResult;
			
		mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
		mc2::math::buildTranslation(shapeOrigin, mPosition);

		glm::quat effectRot;
		if (myOwner)
			effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, myOwner->getRotation() * DEGREES_TO_RADS,0.0f));
		else
			effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, 0.0f ,0.0f)); 

		localToWorld = mc2::math::multiply(gosFX::Effect_Into_Motion, effectRot);
		localResult = localToWorld * shapeOrigin;
			
 		mc2::math::Obb boundingBox;
   		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,&boundingBox);

		bool result = muzzleEffect->Execute(&info);
		if (!result)
		{
			muzzleEffect->Kill();		//Effect is over.  Otherwise, wait until hit!
			muzzleEffect.reset();
		}

   	}

	if (!hitTarget && !((WeaponBoltTypePtr)getObjectType())->isBeam)
	{
		//--------------------------------------------------------------------------------------
		// LaserPosition is front of projectile, laserVertices[0] is top vertex of bulge.
		// LaserVertices[1] is bottom vertex of bulge, laservertices[2] is back of projectile,
		// and laservertices[3] is the middle of the bulge.	
		glm::vec3 lDir(laserDirection);
		lDir *= ((WeaponBoltTypePtr)getObjectType())->bulgeLength;
		laserVertices[0] = laserPosition + lDir;
		laserVertices[1] = laserVertices[0];
		laserVertices[3] = laserVertices[0];
		
		laserVertices[1].x += laserDirection.y * ((WeaponBoltTypePtr)getObjectType())->bulgeWidth;
		laserVertices[1].y -= laserDirection.x * ((WeaponBoltTypePtr)getObjectType())->bulgeWidth;
		
		laserVertices[0].x -= laserDirection.y * ((WeaponBoltTypePtr)getObjectType())->bulgeWidth;
		laserVertices[0].y += laserDirection.x * ((WeaponBoltTypePtr)getObjectType())->bulgeWidth;
	
		laserDirection *= ((WeaponBoltTypePtr)getObjectType())->projLength;
		laserVertices[2] = laserPosition + laserDirection;
	
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
		glm::vec3 offset(beamWiggle,beamWiggle,beamWiggle);
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
		glm::vec3 ourPosition;
		mc2ToRender(ourPosition, laserPosition);

		pointLight->direction = ourPosition;

		glm::mat4 lightToWorldMatrix;
		mc2::math::buildTranslation(lightToWorldMatrix, ourPosition);
		mc2::math::buildRotation(lightToWorldMatrix, mc2::math::Euler(0.0f,0.0f,0.0f));
		pointLight->SetLightToWorld(&lightToWorldMatrix);
		pointLight->SetPosition(&laserPosition);
	}
		
 	if (hitTarget)
	{
		hitLeft -= frameLength;
		
		if (gosEffect && (hitLeft < 0.0f))
		{
			gosEffect->Kill();
			gosEffect.reset();
			
			if (pointLight)
			{
				eye->removeWorldLight(lightId,pointLight);
				systemHeap->Free(pointLight);
				pointLight = nullptr;
			}
 		}
		
		if (hitEffect && hitEffect->IsExecuted())
		{
			glm::vec3 hotSpot = *targetPosition;
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
				
			glm::vec3 tPosition;
			mc2ToRender(tPosition, hotSpot);
		
			glm::mat4 	shapeOrigin;
			glm::mat4 	localToWorld;
				
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, tPosition);
	
			mc2::math::Obb boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
	
			bool result = hitEffect->Execute(&info);
			if (!result)
			{
				if (hitEffect)
				{
					hitEffect->Kill();		//Effect is over.  Otherwise, wait until hit!
					hitEffect.reset();
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
					muzzleEffect.reset();
				}
			}
		}
		else
		{
			if (hitEffect)
			{
				hitEffect->Kill();		//Effect is over.  Otherwise, wait until hit!
				hitEffect.reset();
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
			glm::vec3 hotSpot = *targetPosition;
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
				
			glm::vec3 tPosition;
			mc2ToRender(tPosition, hotSpot);
		
			glm::mat4 	shapeOrigin;
			glm::mat4 	localToWorld;
				
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, tPosition);
	
			mc2::math::Obb boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
	
			bool result = missEffect->Execute(&info);
			if (!result)
			{
				if (missEffect)
				{
					missEffect->Kill();		//Effect is over.  Otherwise, wait until hit!
					missEffect.reset();
				}
					
				if (muzzleEffect)
				{
					muzzleEffect->Kill();
					muzzleEffect.reset();
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
				missEffect.reset();
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
			glm::vec3 hotSpot = *targetPosition;
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
				
			glm::vec3 tPosition;
			tPosition.x = -hotSpot.x;
			tPosition.y = Terrain::waterElevation; 
			tPosition.z = hotSpot.y;
		
			glm::mat4 	shapeOrigin;
			glm::mat4 	localToWorld;
				
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, tPosition);
	
			mc2::math::Obb boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
	
			bool result = waterMissEffect->Execute(&info);
			if (!result)
			{
				if (waterMissEffect)
				{
					waterMissEffect->Kill();		//Effect is over.  Otherwise, wait until hit!
					waterMissEffect.reset();
				}
					
				if (muzzleEffect)
				{
					muzzleEffect->Kill();
					muzzleEffect.reset();
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
				waterMissEffect.reset();
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
	
	// Trail history survives the batcher's per-frame flush. CPU effects and impact tails
	// own their presentation, so GPU trails are limited to otherwise unrepresented flight.
	if (gpu_trail_kind != mc2::particles::GpuTrailKind::None && !gosEffect && !hitTarget) {
		// Trail positions belong to the projectile in render space, not the launcher hotspot.
		glm::vec3 swapped;
		mc2ToRender(swapped, laserPosition);
		trail_history[trail_head] = swapped;
		trail_head = static_cast<uint8_t>((trail_head + 1) % kTrailHistoryMax);
		if (trail_count < kTrailHistoryMax) ++trail_count;

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
