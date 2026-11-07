// Mover: attack chance, shutdown/startup, refit and recover.
// Declared in mover.h; shared names in mover_internal.h.
#include <algorithm>

#include "mover_internal.h"
#include "Gates/gate.h"


constexpr float MAX_SHORT_RANGE_METERS = 60.0f;
constexpr float MIN_MEDIUM_RANGE_METERS = 31.0f;
constexpr float MAX_MEDIUM_RANGE_METERS = 120.0f;
constexpr float MIN_LONG_RANGE_METERS = 61.0f;
constexpr float MAX_LONG_RANGE_METERS = 180.0f;

extern float applyDifficultySkill (float chance, bool isPlayer);
float Mover::calcAttackChance (GameObjectPtr target, long aimLocation, float targetTime, long weaponIndex, float modifiers, long* range, glm::vec3* targetPoint) 
{
	if ((weaponIndex < numOther) || (weaponIndex >= numOther + numWeapons))
		return(-9999.0);

	glm::vec3 targetPosition;
	BattleMechPtr mech = nullptr;
	GroundVehiclePtr vehicle = nullptr;
	
	if (target) 
	{
		long targetObjectClass = target->getObjectClass();
		switch (targetObjectClass) 
		{
			case BATTLEMECH:
				mech = static_cast<BattleMechPtr>(target);
				break;
			case GROUNDVEHICLE:
				vehicle = static_cast<GroundVehiclePtr>(target);
				break;
		}
		targetPosition = target->getPosition();
	}
	else if (targetPoint)
		targetPosition = *targetPoint;
	else
		return(-9999.0);

	float attackChance = pilot->getSkill(MWS_GUNNERY);
	
	if (!MPlayer && (getCommanderId() == Commander::home->getId()))		//Up the gunnery Skill by the difficulty modifier
		attackChance = applyDifficultySkill(attackChance,TRUE);
	else if (!MPlayer && (getCommanderId() == Commander::home->getId()))	//Up the gunnery Skill by the difficulty modifier
		attackChance = applyDifficultySkill(attackChance,FALSE);

	float distanceToTarget = distanceFrom(targetPosition);
	if (range) 
	{
		if (distanceToTarget <= WeaponRange[FIRERANGE_SHORT])
			*range = FIRERANGE_SHORT;		
		else if (distanceToTarget <= WeaponRange[FIRERANGE_MEDIUM])
			*range = FIRERANGE_MEDIUM;
		else
			*range = FIRERANGE_LONG;
	}
	
	float weaponMinRange = WeaponRanges[MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponRange()][0];
	float weaponLongRange = WeaponRanges[MasterComponent::masterList[inventory[weaponIndex].masterID].getWeaponRange()][1];

	if (distanceToTarget <= (weaponMinRange - MapCellDiagonal))
		return(-1.0);
		
	if (distanceToTarget > (weaponLongRange + MapCellDiagonal)) {
		return(-1.0);
	}

	//Non-movers get 100% attack chance.  Look for modifications for turrets soon!
	// Nothing is better then 100%, no need to modify.  Exit here!!
	// -fs 9/8/2000
	if (!target || (target && !target->isMover()) || (target && target->isMover() && target->isDisabled()))
	{
		attackChance = 100.0f;
		return attackChance;
	}

	if (mech && mech->getPilot())
	{
		if ((mech->tonnage < 40) && mech->getPilot()->isLightMechSpecialist())
			attackChance -= 30.0f;
		else if ((mech->tonnage >= 40) && (mech->tonnage < 60) && mech->getPilot()->isMediumMechSpecialist())
			attackChance -= 20.0f;
		else if ((mech->tonnage >= 60) && (mech->tonnage < 80) && mech->getPilot()->isHevayMechSpecialist())
			attackChance -= 10.0f;
		else if ((mech->tonnage >= 80) && (mech->tonnage < 100) && mech->getPilot()->isAssaultMechSpecialist())
			attackChance -= 10.0f;
	}
	
	if (pilot && pilot->isLaserSpecialist() && ((inventory[weaponIndex].masterID == 139) ||
												(inventory[weaponIndex].masterID == 140) ||
												(inventory[weaponIndex].masterID == 147) ||
												(inventory[weaponIndex].masterID == 156)))
	{
		attackChance += WeaponSpecialistModifier;
	}
	
	if (pilot && pilot->isPulseLaserSpecialist() && ((inventory[weaponIndex].masterID == 142) ||
													 (inventory[weaponIndex].masterID == 144) || 
													 (inventory[weaponIndex].masterID == 153) ||
													 (inventory[weaponIndex].masterID == 151)))
	{
		attackChance += WeaponSpecialistModifier;
	}
	
	if (pilot && pilot->isERLaserSpecialist() && ((inventory[weaponIndex].masterID == 141) ||
												  (inventory[weaponIndex].masterID == 143) || 
												  (inventory[weaponIndex].masterID == 150) ||
												  (inventory[weaponIndex].masterID == 152)))
	{
		attackChance += WeaponSpecialistModifier;
	}
	
	if (pilot && pilot->isLightACSpecialist() && ((inventory[weaponIndex].masterID == 100) ||
												  (inventory[weaponIndex].masterID == 103) ||
												  (inventory[weaponIndex].masterID == 110)))
	{
		attackChance += WeaponSpecialistModifier;
	}
	
	if (pilot && pilot->isMediumACSpecialist() && ((inventory[weaponIndex].masterID == 101) ||
												   (inventory[weaponIndex].masterID == 108) ||
												   (inventory[weaponIndex].masterID == 111)))
	{
		attackChance += WeaponSpecialistModifier;
	}

	if (pilot && pilot->isHeavyACSpecialist() && ((inventory[weaponIndex].masterID == 102) ||
												  (inventory[weaponIndex].masterID == 109) ||
												  (inventory[weaponIndex].masterID == 112)))
	{
		attackChance += WeaponSpecialistModifier;
	}
 		
	if (pilot && pilot->isLRMSpecialist() && ((inventory[weaponIndex].masterID == 120) ||
											  (inventory[weaponIndex].masterID == 123)))
	{
		attackChance += WeaponSpecialistModifier;
	}
 		
	if (pilot && pilot->isSRMSpecialist() && ((inventory[weaponIndex].masterID == 124) ||
											  (inventory[weaponIndex].masterID == 125)))
	{
		attackChance += WeaponSpecialistModifier;
	}
 		
	if (pilot && pilot->isPPCSpecialist() && ((inventory[weaponIndex].masterID == 145) ||
											  (inventory[weaponIndex].masterID == 146) ||
											  (inventory[weaponIndex].masterID == 154)))
	{
		attackChance += WeaponSpecialistModifier;
	}

	if (pilot && pilot->isGaussSpecialist() && (inventory[weaponIndex].masterID == 99))
	{
		attackChance += WeaponSpecialistModifier;
	}
  		
	if (pilot && pilot->isSmallArmsSpecialist() && ((inventory[weaponIndex].masterID == 114) || 
													(inventory[weaponIndex].masterID == 155)))
	{
		attackChance += WeaponSpecialistModifier;
	}

	if ((distanceToTarget <= (MAX_SHORT_RANGE_METERS * worldUnitsPerMeter)) && pilot && pilot->isShortRangeSpecialist())
	{
		attackChance += 15.0f;
	}
	
	if ((distanceToTarget >= (MIN_MEDIUM_RANGE_METERS * worldUnitsPerMeter)) && (distanceToTarget <= (MAX_MEDIUM_RANGE_METERS * worldUnitsPerMeter)) && pilot && pilot->isMediumRangeSpecialist())
	{
		attackChance += 15.0f;
	}
	
	if ((distanceToTarget >= (MIN_LONG_RANGE_METERS * worldUnitsPerMeter)) && (distanceToTarget <= (MAX_LONG_RANGE_METERS * worldUnitsPerMeter)) && pilot && pilot->isLongRangeSpecialist())
	{
		attackChance += 15.0f;
	}
		
	//Aimed shots go here.
	// Aimed or called shots are percentage of the actual attack chance
	// Therefore, must go here after all modifiers...
	if ((aimLocation > -1) && (mech))
	{
		// ISSUE-9 S1 (MC2_CALLED_SHOT_FIX, default OFF = stock): the stock
		// gamesys.fit modifiers (Head=0.08/Arm=0.15/Leg=0.20) put an aimed head
		// shot at the 5% clamp floor for most engagements, so per-shot the
		// called shot is statistically invisible (issue #9). Under the gate the
		// aimed modifiers are FLOORED at 0.30/0.45/0.50 (data value wins if a
		// mod raised it higher); Sharpshooter +20 and the 5-95 clamp unchanged.
		static const bool s_calledShotFix =
			mc2::gate::get<"MC2_CALLED_SHOT_FIX">();
		float headMod = HeadShotModifier;
		float armMod  = ArmShotModifier;
		float legMod  = LegShotModifier;
		if (s_calledShotFix) {
			headMod = std::max(headMod, 0.30f);
			armMod = std::max(armMod, 0.45f);
			legMod = std::max(legMod, 0.50f);
		}
		switch (aimLocation)
		{
			case MECH_BODY_LOCATION_HEAD:
				attackChance *= headMod;
				break;
			case MECH_BODY_LOCATION_CTORSO:
			case MECH_BODY_LOCATION_LTORSO:
			case MECH_BODY_LOCATION_RTORSO:
				break;
			case MECH_BODY_LOCATION_LARM:
			case MECH_BODY_LOCATION_RARM:
				attackChance *= armMod;
				break;

			case MECH_BODY_LOCATION_LLEG:
			case MECH_BODY_LOCATION_RLEG:
				attackChance *= legMod;
				break;
		}

		if (pilot && pilot->isSharpShooter())
			attackChance += 20.0f;
	}
	
	attackChance = std::min(attackChance, 95.0f);
	
	attackChance = std::max(attackChance, 5.0f);
		
	return(attackChance);
}

/*
void Mover::buildAttackChanceTable (void) {

	for (long i = 0; i < 
}
*/

void Mover::ammoExplosion (long ammoIndex) {

	pilot->injure(2);

	Assert(ammoIndex < (numOther + numWeapons + numAmmos),ammoIndex," Ammo Index out of range ");
	Assert(ammoIndex >= (numOther + numWeapons),ammoIndex," Ammo Index too low ");

	long ammoMasterId = inventory[ammoIndex].masterID;
	long bodyLocation = inventory[ammoIndex].bodyLocation;
	long remainingAmmo = inventory[ammoIndex].amount;
	float damage = static_cast<float>(remainingAmmo) *
				  MasterComponent::masterList[ammoMasterId].getAmmoExplosiveDamage();
	inventory[ammoIndex].amount = 0;

	if (inventory[ammoIndex].ammoIndex == -1)
		Fatal(-1," Bad Ammo Index in Ammo Explosion ");

	Assert(inventory[ammoIndex].ammoIndex < numAmmoTypes,inventory[ammoIndex].ammoIndex," Too Many Ammo Types ");
	Assert(inventory[ammoIndex].ammoIndex >= 0,inventory[ammoIndex].ammoIndex," not enough Ammo Types ");

	ammoTypeTotal[inventory[ammoIndex].ammoIndex].curAmount -= remainingAmmo;

	// We can use the bodyLocation as the hitLocation since the location codes
	// for all body locations are the same as the armor location codes.
	// MULTIPLAYER NOTE: Since this is called ONLY by Mech.cpp from hitInventoryItem,
	// there should be no need to call it from the server since it's triggered
	// by a weaponhit already.
	WeaponShotInfo shotInfo;

	// This is big ass damage for an ammo explosion!!!!
	// Happens with the Hunchback right now.
	damage = std::min(damage, 255.0f);

	shotInfo.init(0, inventory[ammoIndex].masterID, damage, bodyLocation, 0.0);

	if (MPlayer) {
		if (MPlayer->isServer())
			handleWeaponHit(&shotInfo, true);
		}
	else
		handleWeaponHit(&shotInfo);
}

void Mover::disable (unsigned long cause) 
{
	if (!isDisabled()) 
	{
		if (pilot)
			pilot->handleAlarm(PILOT_ALARM_VEHICLE_INCAPACITATED, cause);

		if (getObjectClass() == BATTLEMECH)
			if (MPlayer && MPlayer->isServer() && !(static_cast<BattleMech*>(this))->lost) {
				MPlayer->addKillLossChunk(MAX_MC_PLAYERS, getCommanderId());
				(static_cast<BattleMech*>(this))->lost = true;
			}

		setStatus(OBJECT_STATUS_DISABLED);
		disableThisFrame = true;

		if (Team::home->isFriendly(getTeam()))
			friendlyDestroyed = true;
		else 
		{
			enemyDestroyed = true;
		}

		if (sensorSystem)
			sensorSystem->disable();

		causeOfDeath = cause;

		if (isMech() && (getMoveType() != MOVETYPE_AIR))
		{
			glm::vec3 explosionLoc;
			explosionLoc = appearance->getNodeNamePosition("cockpit");
			ObjectManager->createExplosion(MECH_CRITICAL_HIT_ID,nullptr,explosionLoc,0.0f,0.0f);

			explosionLoc = appearance->getNodeNamePosition("hit_left");
			ObjectManager->createExplosion(MECH_CRITICAL_HIT_ID,nullptr,explosionLoc,0.0f,0.0f);
			
			explosionLoc = appearance->getNodeNamePosition("hit_right");
			ObjectManager->createExplosion(MECH_CRITICAL_HIT_ID,nullptr,explosionLoc,0.0f,0.0f);

			explosionLoc = appearance->getNodeNamePosition("weapon_righttorso");
			ObjectManager->createExplosion(MECH_CRITICAL_HIT_ID,nullptr,explosionLoc,0.0f,0.0f);

 			explosionLoc = appearance->getNodeNamePosition("weapon_lefttorso");
			ObjectManager->createExplosion(MECH_CRITICAL_HIT_ID,nullptr,explosionLoc,0.0f,0.0f);
		}
	}
}

void Mover::shutDown (void) {

	if (isDisabled())
		return;

	if ((status == OBJECT_STATUS_SHUTDOWN) || (status == OBJECT_STATUS_SHUTTING_DOWN))
		return;

	setStatus(OBJECT_STATUS_SHUTTING_DOWN);
	shutDownThisFrame = true;
}

void Mover::startUp (void) {

	if (isDisabled())
		return;

	if ((status == OBJECT_STATUS_STARTING_UP) || (status == OBJECT_STATUS_NORMAL))
		return;

	setStatus(OBJECT_STATUS_STARTING_UP);
	startUpThisFrame = true;
}

void Mover::tradeRefresh (void) {

	timeLeft = 15.0;
	exploding = false;
	withdrawing = false;
	engineBlowTime = -1.0;
	ejectOrderGiven = false;
	statusChunk.ejectOrderGiven = false;
}

bool Mover::isWithdrawing (void) {

	return(pilot->getCurTacOrder()->code == TACTICAL_ORDER_WITHDRAW);
}

bool Mover::refit (float pointsAvailable, float& pointsUsed, bool ammoOnly) {

	bool result = false;
	float refitPoints = pointsAvailable;

	if (refitPoints > 0.0) 	{
		float areaMax = refitAmount / 3.0;	// size of pool for each section (armor, internals, and ammo) before depletions
		long rearmCount = 0;
		long repairCount = 0;
		
		if (!ammoOnly) {
			for (long i = 0; i < numArmorLocations; i++) {
				// don't repair destroyed arms...
				if (((i == MECH_BODY_LOCATION_LARM) || (i == MECH_BODY_LOCATION_RARM)) && (body[i].damageState == IS_DAMAGE_DESTROYED))
					continue;
				if ((i < numBodyLocations) && (body[i].curInternalStructure < body[i].maxInternalStructure))
					repairCount++;
				if (armor[i].curArmor < armor[i].maxArmor)
					repairCount++;
			}
		}

		for (long i = 0; i < numAmmoTypes; i++)
			if (ammoTypeTotal[i].curAmount < ammoTypeTotal[i].maxAmount)
				rearmCount++;

		if (pilot && pilot->getRadio())
			pilot->getRadio()->resetAmmoMessage();

		for (int i = 0; i < numArmorLocations; i++) {
			if ((repairCount == 0) || (refitPoints <= 0.0))
				continue;
			if (((i == MECH_BODY_LOCATION_LARM) || (i == MECH_BODY_LOCATION_RARM)) && (body[i].damageState == IS_DAMAGE_DESTROYED))
				continue;
			if (armor[i].curArmor < armor[i].maxArmor) {
				float pointsToUse = areaMax / repairCount;
				pointsToUse = std::min(pointsToUse, refitPoints);
				float repairValue = pointsToUse * refitCostArray[ARMOR_REFIT_COST][REFIT_VEHICLE];

				if ((armor[i].curArmor + repairValue) > armor[i].maxArmor) {
					repairValue = armor[i].maxArmor - armor[i].curArmor;
					pointsToUse = repairValue / refitCostArray[ARMOR_REFIT_COST][REFIT_VEHICLE];
				}

				armor[i].curArmor += repairValue;
				refitPoints -= pointsToUse;
			}
			if ((i < numBodyLocations) && (body[i].curInternalStructure < body[i].maxInternalStructure)) {
				float pointsToUse = areaMax / repairCount;
				pointsToUse = std::min(pointsToUse, refitPoints);
				float repairValue = pointsToUse * refitCostArray[INTERNAL_REFIT_COST][REFIT_VEHICLE];
				if ((body[i].curInternalStructure + repairValue) > body[i].maxInternalStructure) {
					repairValue = body[i].maxInternalStructure - body[i].curInternalStructure;
					pointsToUse = repairValue / refitCostArray[INTERNAL_REFIT_COST][REFIT_VEHICLE];
				}
				
				body[i].curInternalStructure += repairValue;
				long newDamageState = IS_DAMAGE_NONE;
				if (body[i].curInternalStructure <= 0.0)
					newDamageState = IS_DAMAGE_DESTROYED;
				else if ((body[i].curInternalStructure / body[i].maxInternalStructure) <= 0.5)
					newDamageState = IS_DAMAGE_PARTIAL;

				if ((getObjectClass() == BATTLEMECH) /*&& (newDamageState != body[i].damageState)*/) {
					if ((i == MECH_BODY_LOCATION_LLEG) || (i == MECH_BODY_LOCATION_RLEG))
						(static_cast<BattleMechPtr>(this))->calcLegStatus();
					if (i == MECH_BODY_LOCATION_CTORSO)
						(static_cast<BattleMechPtr>(this))->calcTorsoStatus();
				}
				body[i].damageState = newDamageState;
				refitPoints -= pointsToUse;
			}
		}

		for (int i=0; i < numAmmoTypes; i++) {
			if ((rearmCount > 0) && (refitPoints > 0.0) && (ammoTypeTotal[i].curAmount < ammoTypeTotal[i].maxAmount)) {
				bool recalc = (ammoTypeTotal[i].curAmount == 0);
				float pointsToUse = areaMax / rearmCount;
				pointsToUse = std::min(pointsToUse, refitPoints);
				float repairValue = pointsToUse * refitCostArray[AMMO_REFIT_COST][REFIT_VEHICLE] * MasterComponent::masterList[ammoTypeTotal[i].masterId].getAmmoPerTon();
				if ((ammoTypeTotal[i].curAmount + repairValue) > ammoTypeTotal[i].maxAmount) {
					repairValue = ammoTypeTotal[i].maxAmount - ammoTypeTotal[i].curAmount;
					pointsToUse = repairValue / (refitCostArray[AMMO_REFIT_COST][REFIT_VEHICLE] * MasterComponent::masterList[ammoTypeTotal[i].masterId].getAmmoPerTon());
				}
				ammoTypeTotal[i].curAmount += repairValue;
				refitPoints -= pointsToUse;
				if (recalc) {
					calcWeaponEffectiveness(false);
					calcFireRanges();
				}
			}
		}
		
		rearmCount = 0;
		repairCount = 0;
		if (!ammoOnly) {
			for (int i = 0; i < numArmorLocations; i++) {
				// don't repair destroyed arms
				if (((i == MECH_BODY_LOCATION_LARM) || (i == MECH_BODY_LOCATION_RARM)) && (body[i].damageState == IS_DAMAGE_DESTROYED))
					continue;

				if (((i < numBodyLocations) && (body[i].curInternalStructure < body[i].maxInternalStructure)) || (armor[i].curArmor < armor[i].maxArmor)) {
					repairCount++;
					break;
				}
			}
		}

		if (repairCount == 0)
			for (int i = 0; i < numAmmoTypes; i++)
				if (ammoTypeTotal[i].curAmount < ammoTypeTotal[i].maxAmount) {
					rearmCount++;
					break;
				}

		if ((repairCount == 0) && (rearmCount == 0)) {
			if (getCommanderId() == Commander::home->getId()) 
			{
				getPilot()->radioMessage(RADIO_REFIT_DONE, TRUE);
				soundSystem->playBettySample(BETTY_REPAIR_COMPLETE);
				if (appearance)
					appearance->startSmoking(-1);	//Turn the smoke off if we successfully repaired.
			}
			result = true;
		}
		}
	else {
		if (getCommanderId() == Commander::home->getId())
		{
			getPilot()->radioMessage(RADIO_REFIT_INCOMPLETE, TRUE);
			soundSystem->playBettySample( BETTY_REPAIR_GONE );
		}
		result = true;
	}

	pointsUsed = pointsAvailable - refitPoints;
	// Points used should be rounded to nearest 1/4 point (for consistency in
	// MultiPlayer)...
	if ((pointsUsed > 0.0) && (pointsUsed < 0.25))
		pointsUsed = 0.25;
	pointsUsed = static_cast<float>((static_cast<long>(((pointsUsed + 0.125) / 0.25)))) / 4.0;

	return(result);
}

float Mover::calcRecoverPrice (void) {

	float repairValue = 0;
	for (long i = 0; i < numBodyLocations; i++) {
		if (((i == MECH_BODY_LOCATION_LARM) || (i == MECH_BODY_LOCATION_RARM)) && (body[i].damageState == IS_DAMAGE_DESTROYED))
			continue;
		if ((i < numBodyLocations) && (body[i].curInternalStructure < body[i].maxInternalStructure))
			repairValue += (body[i].maxInternalStructure - body[i].curInternalStructure);
	}
	float pointsToUse = repairValue / recoverCost;

	// Points used should be rounded to nearest 1/4 point (for consistency in
	// MultiPlayer)...
	if ((pointsToUse > 0.0) && (pointsToUse < 0.25))
		pointsToUse = 0.25;
	pointsToUse = static_cast<float>((static_cast<long>(((pointsToUse + 0.125) / 0.25)))) / 4.0;
	return(pointsToUse);
}

bool Mover::recover (void) {

	inRecoverUpdate = true;
	bool result = false;
	float recoverPoints = 100000.0;

	if (recoverPoints > 0.0) 	{
		long repairCount = 0;
		for (long i = 0; i < numBodyLocations; i++) {
			// don't repair destroyed arms...
			if (((i == MECH_BODY_LOCATION_LARM) || (i == MECH_BODY_LOCATION_RARM)) && (body[i].damageState == IS_DAMAGE_DESTROYED))
				continue;
			if ((i < numBodyLocations) && (body[i].curInternalStructure < body[i].maxInternalStructure))
				repairCount++;
		}

		for (int i = 0; i < numBodyLocations; i++) 
		{
			if ((repairCount == 0) || (recoverPoints <= 0.0))
				continue;

			if (((i == MECH_BODY_LOCATION_LARM) || (i == MECH_BODY_LOCATION_RARM)) && (body[i].damageState == IS_DAMAGE_DESTROYED))
				continue;

			if ((i < numBodyLocations) && (body[i].curInternalStructure < body[i].maxInternalStructure)) 
			{
				float pointsToUse = recoverAmount / repairCount;
				pointsToUse = std::min(pointsToUse, recoverPoints);
				float repairValue = pointsToUse * recoverCost;
				if ((body[i].curInternalStructure + repairValue) > body[i].maxInternalStructure) 
				{
					repairValue = body[i].maxInternalStructure - body[i].curInternalStructure;
					pointsToUse = repairValue / recoverCost;
				}
				
				body[i].curInternalStructure += repairValue;
				long newDamageState = IS_DAMAGE_NONE;
				if (body[i].curInternalStructure <= 0.0)
					newDamageState = IS_DAMAGE_DESTROYED;
				else if ((body[i].curInternalStructure / body[i].maxInternalStructure) <= 0.5)
					newDamageState = IS_DAMAGE_PARTIAL;

				if ((getObjectClass() == BATTLEMECH) /*&& (newDamageState != body[i].damageState)*/) 
				{
					if ((i == MECH_BODY_LOCATION_LLEG) || (i == MECH_BODY_LOCATION_RLEG))
						(static_cast<BattleMechPtr>(this))->calcLegStatus();
					if (i == MECH_BODY_LOCATION_CTORSO)
						(static_cast<BattleMechPtr>(this))->calcTorsoStatus();
				}

				body[i].damageState = newDamageState;
				recoverPoints -= pointsToUse;
			}
		}
		
		repairCount = 0;
		for (int i = 0; i < numBodyLocations; i++) 
		{
			// don't repair destroyed arms
			if (((i == MECH_BODY_LOCATION_LARM) || (i == MECH_BODY_LOCATION_RARM)) && (body[i].damageState == IS_DAMAGE_DESTROYED))
				continue;

			if (body[i].curInternalStructure < body[i].maxInternalStructure) {
				repairCount++;
				break;
			}
		}

		if (repairCount == 0) 
		{
			for (long i = 0; i < numOther; i++) 
			{
				MasterComponentPtr component = &MasterComponent::masterList[inventory[i].masterID];
				switch (component->getForm()) {
					case COMPONENT_FORM_WEAPON:
					case COMPONENT_FORM_WEAPON_ENERGY:
					case COMPONENT_FORM_WEAPON_BALLISTIC:
					case COMPONENT_FORM_WEAPON_MISSILE:
						inventory[i].health = component->getHealth();
						inventory[i].disabled = false;
						calcWeaponEffectiveness(false);
						calcFireRanges();
						break;
					default:
						inventory[i].health = component->getHealth();
						inventory[i].disabled = false;
				}
			}

			for (int i = 0; i < numBodyLocations; i++)
				for (long j = 0; j < NumLocationCriticalSpaces[i]; j++)
					if (body[i].criticalSpaces[j].inventoryID < 255)
						body[i].criticalSpaces[j].hit = inventory[body[i].criticalSpaces[j].inventoryID].disabled;

			if (getCommanderId() == Commander::home->getId()) 
			{
				getPilot()->radioMessage(RADIO_REFIT_DONE, TRUE);
				soundSystem->playBettySample(BETTY_REPAIR_COMPLETE);
			}
			result = true;
		}
	}
	else 
	{
		if (getCommanderId() == Commander::home->getId())
		{
			getPilot()->radioMessage(RADIO_REFIT_INCOMPLETE, TRUE);
			soundSystem->playBettySample( BETTY_REPAIR_GONE );
			soundSystem->playBettySample(BETTY_REPAIR_INCOMPLETE);
		}
		result = true;
	}

	inRecoverUpdate = false;
	return(result);
}
