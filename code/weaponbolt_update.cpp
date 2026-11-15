// WeaponBolt: the per-frame bolt update (flight, impact, direct-fire modes) and its speed/straight-fire gates.
// Declared in the class headers; shared names in weaponbolt_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "../mclib/texture_assets.h"
#include "weaponbolt_internal.h"
#include "appearance_rng.h"
#include "sim_trace.h"
#include "mc2fmt.h"
#include "../RenderCore/axis_convert.h"
#include "Gates/gate.h"
#include "impact_presentation.h"
#include "tick_commit.h"

//---------------------------------------------------------------------------
// PPC de-curve + direct-fire speed (env-gated, default-off). "Direct-fire" =
// non-arc, non-beam bolts (PPC / autocannon / gauss). MC2_DIRECT_FIRE_STRAIGHT=1
// makes them fly STRAIGHT (no per-frame target re-track) to a one-time aim that
// is LED by the target's velocity over the bolt's flight time, so a pre-rolled
// hit on a moving target still lands. LRM (arcEffect) + beams keep tracking.
// MC2_PROJECTILE_SPEED_MULT scales direct-fire travel speed (default 1.0).
// Presentation only: the impact decal library reads this event; the simulation never reads it back and draws no RNG for it.
static void emitImpact(const WeaponBoltType* t, const WeaponShotInfo& shot, const glm::vec3& from, const glm::vec3& at,
                       unsigned long targetWID)
{
	impact_presentation::onWeaponImpact(shot.masterId, t->isBeam, t->areaEffectDmg > 0.0f && t->areaEffectRad > 0.0f,
	                                    shot.damage, from, at, targetWID);
}

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

		if ((static_cast<WeaponBoltTypePtr>(getObjectType()))->fireSoundFX != 0)
			soundSystem->playDigitalSample((static_cast<WeaponBoltTypePtr>(getObjectType()))->fireSoundFX, position, true);
			
		if (weaponShot.masterId >= 0 && MasterComponent::masterList[weaponShot.masterId].getForm() == COMPONENT_FORM_WEAPON_MISSILE) {
			position.x += (static_cast<int>(getWatchID(false) % 5) - 2) * 1.5f;
			position.y += (static_cast<int>((getWatchID(false) / 5) % 4) - 1) * 1.5f;
		}
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
			WeaponBoltTypePtr bt = static_cast<WeaponBoltTypePtr>(getObjectType());
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

		laserDirection *= (static_cast<WeaponBoltTypePtr>(getObjectType()))->projLength;
		
		if ((static_cast<WeaponBoltTypePtr>(getObjectType()))->isBeam)
		{
			timeLeft = (static_cast<WeaponBoltTypePtr>(getObjectType()))->beamDuration;
			actualPosition.x = -(*targetPosition).x;	//Play the beam hitting effect at the hit location
			actualPosition.y = (*targetPosition).z;
			actualPosition.z = (*targetPosition).y;
		}
		else
			timeLeft = 0.0f;

		glm::vec3 laserVelocity;
		laserVelocity = targetPos - ownerPosition;
		float distance = laserVelocity.x * laserVelocity.x + laserVelocity.y * laserVelocity.y;
		distanceToTarget = distance;
		halfDistanceToTarget = distance / 2.0f;
		
		goalHeight = (static_cast<WeaponBoltTypePtr>(getObjectType()))->arcHeight;
		goalHeight *= 10.0f;
	}

	GameObjectPtr myOwner = ObjectManager->getByWatchID(ownerWID);
	if (myOwner)
		position = myOwner->getPositionFromHS(hotSpotNumber);
	
	if ((static_cast<WeaponBoltTypePtr>(getObjectType()))->isBeam)
	{
		timeLeft -= frameLength;
	}
		
	glm::vec3 ownerPosition = laserPosition;
	GameObjectPtr target = ObjectManager->getByWatchID(targetWID);
		
	glm::vec3 targetPos;
	targetPos = glm::vec3(0.0f);
	// PPC de-curve: direct-fire (non-arc, non-beam) bolts do NOT re-track -- they
	// fly straight to the lead-aim locked at spawn. LRM/arc + beams keep tracking.
	WeaponBoltTypePtr boltType = static_cast<WeaponBoltTypePtr>(getObjectType());
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
	float velMag = (static_cast<WeaponBoltTypePtr>(getObjectType()))->velocity;
	velMag *= frameLength;

	// Direct-fire travel speed preserves the pre-rolled hit/miss outcome.
	if (!(static_cast<WeaponBoltTypePtr>(getObjectType()))->arcEffect)
		velMag *= directFireSpeedMult();

	laserVelocity = targetPos - ownerPosition;
	float distance = laserVelocity.x * laserVelocity.x + laserVelocity.y * laserVelocity.y;
	
	bool isArcing = false;
	if ((static_cast<WeaponBoltTypePtr>(getObjectType()))->arcEffect)
	{
		//GoalHeight is actually the velocity
		if ( goalHeight > 0.0f )
		{
			float accel = -(static_cast<WeaponBoltTypePtr>(getObjectType()))->arcHeight * 10.0f;
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
		if (!(static_cast<WeaponBoltTypePtr>(getObjectType()))->isBeam)
		{
			float lastDistanceMoved = distanceToTarget - distance;
#if MC2_SIM_TRACE
			if (target && target->isMech()) {
				const glm::vec3 tp = target->getPosition();
				MC2_SIM_TRACE_LINE("BOLT id={:d} tgt={:d} dist={:.4f} last={:.4f} tpos={:.4f},{:.4f},{:.4f} mpos={:.4f},{:.4f},{:.4f}", static_cast<int32_t>(getPartId()), static_cast<int32_t>(target->getPartId()), static_cast<double>(distance), static_cast<double>(lastDistanceMoved),
				                   static_cast<double>(targetPos.x), static_cast<double>(targetPos.y), static_cast<double>(targetPos.z), static_cast<double>(tp.x), static_cast<double>(tp.y), static_cast<double>(tp.z));
			}
#endif
			distanceToTarget = distance;
			
			if (lastDistanceMoved >= distance)
			{
				if (target && target->isMover() && weaponShot.masterId >= 0 &&
					MasterComponent::masterList[weaponShot.masterId].getForm() == COMPONENT_FORM_WEAPON_MISSILE &&
					static_cast<MoverPtr>(target)->interceptMissile()) {
					traceCombat("intercept", "ams", target, &weaponShot);
					hitTarget = true;
					weaponShot.damage = 0.0f;
					targetWID = 0;
					armRetireDeadline();
					return false;
				}
				hitTarget = TRUE;
				if (target) traceCombat("impact", "hit");
				hitLeft = (static_cast<WeaponBoltTypePtr>(getObjectType()))->afterHitTime;

				// Arrival is authoritative; visual impact starts from the completed snapshot.

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

					emitImpact(boltType, weaponShot, ownerPosition, hotSpot, targetWID);   // projectile arrival
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

					}
					else
					{
						traceCombat("impact", "miss");
						emitImpact(boltType, weaponShot, ownerPosition, hotSpot, 0);   // projectile ground miss
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
					
					if ((static_cast<WeaponBoltTypePtr>(getObjectType()))->hitEffectObjNum &&
						((static_cast<WeaponBoltTypePtr>(getObjectType()))->areaEffectDmg > 0.0f) &&
						((static_cast<WeaponBoltTypePtr>(getObjectType()))->areaEffectRad > 0.0f))
					{

						// The explosion owns area damage for both hits and misses.
						ObjectManager->createExplosion((static_cast<WeaponBoltTypePtr>(getObjectType()))->hitEffectObjNum,
														nullptr,*targetPosition,
														weaponShot.masterId >= 0 && MasterComponent::masterList[weaponShot.masterId].getForm() == COMPONENT_FORM_WEAPON_MISSILE
														? weaponShot.damage : (static_cast<WeaponBoltTypePtr>(getObjectType()))->areaEffectDmg,
														(static_cast<WeaponBoltTypePtr>(getObjectType()))->areaEffectRad);
					}
					else
					{
						submitDamage(target);
					}
					applySoftSplash(laserPosition, targetWID);
				}
				else if (targetPosition)
				{

					{
						int cellRow, cellCol;
		
						land->worldToCell(*targetPosition, cellRow, cellCol);
						if (GameMap->getMine(cellRow, cellCol) == 1)
						{
							ObjectManager->createExplosion(MINE_EXPLOSION_ID, nullptr, *targetPosition, MineSplashDamage, MineSplashRange * worldUnitsPerMeter);
							GameMap->setMine(cellRow, cellCol, 2);
						}
					}

					if ((static_cast<WeaponBoltTypePtr>(getObjectType()))->hitEffectObjNum &&
						((static_cast<WeaponBoltTypePtr>(getObjectType()))->areaEffectDmg > 0.0f) &&
						((static_cast<WeaponBoltTypePtr>(getObjectType()))->areaEffectRad > 0.0f))
					{

						// Exclude the located target, rather than suppressing the entire blast.
						float areaDmg = weaponShot.masterId >= 0 && MasterComponent::masterList[weaponShot.masterId].getForm() == COMPONENT_FORM_WEAPON_MISSILE
							? weaponShot.damage : (static_cast<WeaponBoltTypePtr>(getObjectType()))->areaEffectDmg;

						// The explosion owns area damage for both hits and misses.
						auto* explosion = ObjectManager->createExplosion((static_cast<WeaponBoltTypePtr>(getObjectType()))->hitEffectObjNum,
														nullptr,*targetPosition,
														areaDmg,
														(static_cast<WeaponBoltTypePtr>(getObjectType()))->areaEffectRad);
						deliverAimedAreaHit(explosion);
					}
					applySoftSplash(*targetPosition, 0);
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

					emitImpact(boltType, weaponShot, position, hotSpot, targetWID);   // beam end on the target
				}
				else
				{
					glm::vec3 hotSpot = *targetPosition;
					int cellR, cellC;
					land->worldToCell(hotSpot,cellR, cellC);
					if (GameMap->getDeepWater(cellR, cellC) || GameMap->getShallowWater(cellR, cellC))
					{
						traceCombat("impact", "water_miss");

					}
					else
					{
						traceCombat("impact", "miss");
						emitImpact(boltType, weaponShot, position, hotSpot, 0);   // beam end on the ground
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
					
					if ((static_cast<WeaponBoltTypePtr>(getObjectType()))->hitEffectObjNum &&
						((static_cast<WeaponBoltTypePtr>(getObjectType()))->areaEffectDmg > 0.0f) &&
						((static_cast<WeaponBoltTypePtr>(getObjectType()))->areaEffectRad > 0.0f))
					{

						// The explosion owns area damage for both hits and misses.
						ObjectManager->createExplosion((static_cast<WeaponBoltTypePtr>(getObjectType()))->hitEffectObjNum,
														nullptr,*targetPosition,
														weaponShot.masterId >= 0 && MasterComponent::masterList[weaponShot.masterId].getForm() == COMPONENT_FORM_WEAPON_MISSILE
														? weaponShot.damage : (static_cast<WeaponBoltTypePtr>(getObjectType()))->areaEffectDmg,
														(static_cast<WeaponBoltTypePtr>(getObjectType()))->areaEffectRad);
					}
					else
					{
						submitDamage(target);
					}
				}
				else if (targetPosition) 
				{

					{
						int cellRow, cellCol;
		
						land->worldToCell(*targetPosition, cellRow, cellCol);
						if (GameMap->getMine(cellRow, cellCol) == 1)
						{
							ObjectManager->createExplosion(MINE_EXPLOSION_ID, nullptr, *targetPosition, MineSplashDamage, MineSplashRange * worldUnitsPerMeter);
							GameMap->setMine(cellRow, cellCol, 2);
						}
					}
					
					if ((static_cast<WeaponBoltTypePtr>(getObjectType()))->hitEffectObjNum &&
						((static_cast<WeaponBoltTypePtr>(getObjectType()))->areaEffectDmg > 0.0f) &&
						((static_cast<WeaponBoltTypePtr>(getObjectType()))->areaEffectRad > 0.0f))
					{

						// The explosion owns area damage for both hits and misses.
						ObjectManager->createExplosion((static_cast<WeaponBoltTypePtr>(getObjectType()))->hitEffectObjNum,
														nullptr,*targetPosition,
														weaponShot.masterId >= 0 && MasterComponent::masterList[weaponShot.masterId].getForm() == COMPONENT_FORM_WEAPON_MISSILE
														? weaponShot.damage : (static_cast<WeaponBoltTypePtr>(getObjectType()))->areaEffectDmg,
														(static_cast<WeaponBoltTypePtr>(getObjectType()))->areaEffectRad);
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

	if (hitTarget) hitLeft -= frameLength;
	// Gameplay retirement never reads particle completion or visibility.
	return !(hitTarget && retireDeadline.armed &&
	         mission->getCombatClock().ticks >= retireDeadline.tick);
}

void WeaponBolt::updatePresentation()
{
	if (!targetPosition || getFlag(OBJECT_FLAG_JUSTCREATED)) return;
	auto* myOwner = ObjectManager->getByWatchID(ownerWID);
	auto* target = ObjectManager->getByWatchID(targetWID);
	glm::vec3 laserDirection = *targetPosition - (hitTarget ? position : laserPosition);
	if (glm::length(laserDirection) > 0.0f) laserDirection = glm::normalize(laserDirection);
	bool inView = true;
	const auto start = [&](gosFX::EffectPtr& effect, const glm::vec3& at) {
		if (!effect) return;
		glm::vec3 world; mc2ToRender(world, at);
		glm::mat4 origin(1.0f); mc2::math::buildTranslation(origin, world);
		gosFX::Effect::ExecuteInfo info(static_cast<Stuff::Time>(scenarioTime), &origin, nullptr);
		effect->Start(&info);
	};
	if (!presentationStarted) {
		presentationStarted = true;
		createPresentationResources();
		start(gosEffect, laserPosition);
		start(muzzleEffect, position);
	}
	if (hitTarget && !presentationImpactStarted) {
		presentationImpactStarted = true;
		mc2::tick::releaseEffect(gosEffect);
		int row, col; land->worldToCell(*targetPosition, row, col);
		const bool water = GameMap->getDeepWater(row, col) || GameMap->getShallowWater(row, col);
		if (target) {
			mc2::tick::releaseEffect(missEffect); mc2::tick::releaseEffect(waterMissEffect); start(hitEffect, laserPosition);
		} else if (water) {
			mc2::tick::releaseEffect(hitEffect); mc2::tick::releaseEffect(missEffect);
			auto at = *targetPosition; at.z = Terrain::waterElevation; start(waterMissEffect, at);
		} else {
			mc2::tick::releaseEffect(hitEffect); mc2::tick::releaseEffect(waterMissEffect); start(missEffect, *targetPosition);
		}
	}
	if (static_cast<WeaponBoltTypePtr>(getObjectType())->isBeam) {
		startUV += static_cast<WeaponBoltTypePtr>(getObjectType())->uvAnimRate * frameLength;
		gosTextureHandle = mc2::textures::sourceId(mcTextureHandle);
	}
   	if (gosEffect && gosEffect->IsExecuted())
   	{
		glm::vec3 actualPosition;
		if ((static_cast<WeaponBoltTypePtr>(getObjectType()))->isBeam)
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
		
		const float rotation = -world_angle_between(laserPosition, *targetPosition);
		
		glm::quat effectRot;
		effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,rotation * DEGREES_TO_RADS,0.0f));
		localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
		localResult = localToWorld * shapeOrigin;
 
		mc2::math::Obb boundingBox;
   		gosFX::Effect::ExecuteInfo info(static_cast<Stuff::Time>(scenarioTime),&localResult,&boundingBox);

		bool result = gosEffect->Execute(&info);
		if (!result)
		{
			mc2::tick::releaseEffect(gosEffect);		//Effect is over.  Otherwise, wait until hit!
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
   		gosFX::Effect::ExecuteInfo info(static_cast<Stuff::Time>(scenarioTime),&localResult,&boundingBox);

		bool result = muzzleEffect->Execute(&info);
		if (!result)
		{
			mc2::tick::releaseEffect(muzzleEffect);		//Effect is over.  Otherwise, wait until hit!
		}

   	}

	if (!hitTarget && !(static_cast<WeaponBoltTypePtr>(getObjectType()))->isBeam)
	{
		//--------------------------------------------------------------------------------------
		// LaserPosition is front of projectile, laserVertices[0] is top vertex of bulge.
		// LaserVertices[1] is bottom vertex of bulge, laservertices[2] is back of projectile,
		// and laservertices[3] is the middle of the bulge.	
		glm::vec3 lDir(laserDirection);
		lDir *= (static_cast<WeaponBoltTypePtr>(getObjectType()))->bulgeLength;
		laserVertices[0] = laserPosition + lDir;
		laserVertices[1] = laserVertices[0];
		laserVertices[3] = laserVertices[0];
		
		laserVertices[1].x += laserDirection.y * (static_cast<WeaponBoltTypePtr>(getObjectType()))->bulgeWidth;
		laserVertices[1].y -= laserDirection.x * (static_cast<WeaponBoltTypePtr>(getObjectType()))->bulgeWidth;
		
		laserVertices[0].x -= laserDirection.y * (static_cast<WeaponBoltTypePtr>(getObjectType()))->bulgeWidth;
		laserVertices[0].y += laserDirection.x * (static_cast<WeaponBoltTypePtr>(getObjectType()))->bulgeWidth;
	
		laserDirection *= (static_cast<WeaponBoltTypePtr>(getObjectType()))->projLength;
		laserVertices[2] = laserPosition + laserDirection;
	
		if ((static_cast<WeaponBoltTypePtr>(getObjectType()))->lightSource && !pointLight && (lightId != -1))
		{
			pointLight = (static_cast<WeaponBoltTypePtr>(getObjectType()))->getLight();
			lightId = eye->addWorldLight(pointLight);
			if (lightId == -1)
			{
				//NO LIGHT ALLOWED!  TOO Many in World!
				// LightId is now -1 which will cause this to NEVER make a light!
				std::free(pointLight);
				pointLight = nullptr;
			}
		}
	}
	else if (!hitTarget && (static_cast<WeaponBoltTypePtr>(getObjectType()))->isBeam)
	{
		laserVertices[0] = position;
		laserVertices[1] = position;
		laserSide[0] = position;
		laserSide[1] = position;
		
		laserVertices[0].x -= laserDirection.y * (static_cast<WeaponBoltTypePtr>(getObjectType()))->bulgeWidth;
		laserVertices[0].y += laserDirection.x * (static_cast<WeaponBoltTypePtr>(getObjectType()))->bulgeWidth;
 		
		laserVertices[1].x += laserDirection.y * (static_cast<WeaponBoltTypePtr>(getObjectType()))->bulgeWidth;
		laserVertices[1].y -= laserDirection.x * (static_cast<WeaponBoltTypePtr>(getObjectType()))->bulgeWidth;

		laserSide[0].z += (static_cast<WeaponBoltTypePtr>(getObjectType()))->bulgeWidth;
		laserSide[1].z -= (static_cast<WeaponBoltTypePtr>(getObjectType()))->bulgeWidth;

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

		float beamWiggle = (static_cast<WeaponBoltTypePtr>(getObjectType()))->beamWiggle;
		glm::vec3 offset(beamWiggle,beamWiggle,beamWiggle);
		offset.x = mc2_appearance_rng::randomNumber(offset.x * 2) - offset.x;
		offset.y = mc2_appearance_rng::randomNumber(offset.y * 2) - offset.y;
		offset.z = mc2_appearance_rng::randomNumber(offset.z * 2) - offset.z;
		hsPos += offset;

		laserVertices[2] = hsPos;
		laserVertices[3] = hsPos;
		
		laserSide[2] = hsPos;
		laserSide[3] = hsPos;
		
		laserVertices[2].x -= laserDirection.y * (static_cast<WeaponBoltTypePtr>(getObjectType()))->bulgeWidth;
		laserVertices[2].y += laserDirection.x * (static_cast<WeaponBoltTypePtr>(getObjectType()))->bulgeWidth;
 		
		laserVertices[3].x += laserDirection.y * (static_cast<WeaponBoltTypePtr>(getObjectType()))->bulgeWidth;
		laserVertices[3].y -= laserDirection.x * (static_cast<WeaponBoltTypePtr>(getObjectType()))->bulgeWidth;
		
		laserSide[2].z += (static_cast<WeaponBoltTypePtr>(getObjectType()))->bulgeWidth;
		laserSide[3].z -= (static_cast<WeaponBoltTypePtr>(getObjectType()))->bulgeWidth;
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

		if (gosEffect && (hitLeft < 0.0f))
		{
			mc2::tick::releaseEffect(gosEffect);
			
			if (pointLight)
			{
				eye->removeWorldLight(lightId,pointLight);
				std::free(pointLight);
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
			gosFX::Effect::ExecuteInfo info(static_cast<Stuff::Time>(scenarioTime),&shapeOrigin,&boundingBox);
	
			bool result = hitEffect->Execute(&info);
			if (!result)
			{
				if (hitEffect)
				{
					mc2::tick::releaseEffect(hitEffect);		//Effect is over.  Otherwise, wait until hit!
				}
					
				inView = FALSE;
				
				if (pointLight)
				{
					eye->removeWorldLight(lightId,pointLight);
					std::free(pointLight);
					pointLight = nullptr;
				}
				
				if (muzzleEffect)
				{
					mc2::tick::releaseEffect(muzzleEffect);
				}
			}
		}
		else
		{
			if (hitEffect)
			{
				mc2::tick::releaseEffect(hitEffect);		//Effect is over.  Otherwise, wait until hit!
			}
				
			if (pointLight)
			{
				eye->removeWorldLight(lightId,pointLight);
				std::free(pointLight);
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
			gosFX::Effect::ExecuteInfo info(static_cast<Stuff::Time>(scenarioTime),&shapeOrigin,&boundingBox);
	
			bool result = missEffect->Execute(&info);
			if (!result)
			{
				if (missEffect)
				{
					mc2::tick::releaseEffect(missEffect);		//Effect is over.  Otherwise, wait until hit!
				}
					
				if (muzzleEffect)
				{
					mc2::tick::releaseEffect(muzzleEffect);
				}
				
				inView = FALSE;
				
				if (pointLight)
				{
					eye->removeWorldLight(lightId,pointLight);
					std::free(pointLight);
					pointLight = nullptr;
				}
			}
		}
		else
		{
			if (missEffect)
			{
				mc2::tick::releaseEffect(missEffect);		//Effect is over.  Otherwise, wait until hit!
			}
				
			if (pointLight)
			{
				eye->removeWorldLight(lightId,pointLight);
				std::free(pointLight);
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
			gosFX::Effect::ExecuteInfo info(static_cast<Stuff::Time>(scenarioTime),&shapeOrigin,&boundingBox);
	
			bool result = waterMissEffect->Execute(&info);
			if (!result)
			{
				if (waterMissEffect)
				{
					mc2::tick::releaseEffect(waterMissEffect);		//Effect is over.  Otherwise, wait until hit!
				}
					
				if (muzzleEffect)
				{
					mc2::tick::releaseEffect(muzzleEffect);
				}
				
				inView = FALSE;
				
				if (pointLight)
				{
					eye->removeWorldLight(lightId,pointLight);
					std::free(pointLight);
					pointLight = nullptr;
				}
			}
		}
		else
		{
			if (waterMissEffect)
			{
				mc2::tick::releaseEffect(waterMissEffect);		//Effect is over.  Otherwise, wait until hit!
			}
				
			if (pointLight)
			{
				eye->removeWorldLight(lightId,pointLight);
				std::free(pointLight);
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

	if (!inView && !getExists()) destroy();
}
