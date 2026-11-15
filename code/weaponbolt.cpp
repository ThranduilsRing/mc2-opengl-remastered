#include "tick_commit.h"
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
#include "soft_splash.h"
#include "carnage.h"
#include "gvehicl.h"
#include "warrior.h"
#include "../mclib/cmponent.h"
#include "../mclib/texture_assets.h"
#include "mc2fmt.h"
#include "save_archive.h"
#include <bit>
#include <cstring>
#include "diagnostic_trace.h"
#include "../GameOS/gameos/utils/mc2log.h"


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
	textureName.reset();

	ObjectType::destroy();
}

//---------------------------------------------------------------------------
DWORD bgrTorgb (DWORD frontRGB);
	
//---------------------------------------------------------------------------
long WeaponBoltType::init (FilePtr objFile, unsigned long fileSize)
{
	long result = 0;
	long afterHitReadResult = VARIABLE_NOT_FOUND;
	
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
		
		textureName = std::make_unique<char[]>(strlen(txmName)+1);
		strcpy(textureName.get(),txmName);

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
		afterHitReadResult = result;
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
	initRetirePolicy(afterHitReadResult);
	
	return(result);
}

void WeaponBoltType::initRetirePolicy(long afterHitReadResult)
{
	retirePolicy = mc2::combat::RetirePolicy::fromAfterHit(
		isBeam, afterHitReadResult != VARIABLE_NOT_FOUND, afterHitTime);
	if (!isBeam && afterHitReadResult != NO_ERR && afterHitReadResult != VARIABLE_NOT_FOUND)
		retirePolicy.ticks.reset();
	if (!retirePolicy.ticks)
		MC2_EVENT(Core, "COMBAT_RETIRE", 1, "reject reason=policy type={}", whatAmI());
}
	
//---------------------------------------------------------------------------
bool WeaponBoltType::handleCollision (GameObjectPtr collidee, GameObjectPtr collider)
{
	//-------------------------------------------------------
	// The tick-owned deadline retires the bolt; collision must not remove it.
	
	return(FALSE);
}

//---------------------------------------------------------------------------
bool WeaponBoltType::handleDestruction (GameObjectPtr collidee, GameObjectPtr collider)
{
	//-------------------------------------------------------
	// The tick-owned deadline retires the bolt; collision must not remove it.
	
	return(FALSE);
}

//---------------------------------------------------------------------------
TG_LightPtr WeaponBoltType::getLight (void)
{
	TG_LightPtr pointLight = nullptr;

	if (lightSource)
	{
		pointLight = static_cast<TG_LightPtr>(std::malloc(sizeof(TG_Light)));
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
    void mc2EquipmentProbeEvent(std::string_view event, unsigned long target);
    mc2EquipmentProbeEvent(event, target ? target->getWatchID(false) : 0);
    combatTrace.write("bolt", getHandle(), turn, event, reason, position,
        [this] { return getFlag(OBJECT_FLAG_JUSTCREATED) ? nullptr : &laserPosition; }, target, shot);
}

void WeaponBolt::traceCombatRetire(std::string_view reason)
{
    if (combatTrace.live && mc2_diag::tagEnabled("VFX_COMBAT")) {
        const auto now = mission ? mission->getCombatClock().ticks : 0;
        const auto gap = !retireDeadline.armed ? std::string("null") :
            now >= retireDeadline.tick ? std::format("{}", now - retireDeadline.tick) :
            std::format("-{}", retireDeadline.tick - now);
        traceRetireDeadline("retire", std::format(
            "\"combat_tick\":{},\"armed\":{},\"gap_ticks\":{}", now, retireDeadline.armed, gap));
    }
    traceCombat("retire", reason);
    combatTrace.live = false;
}

void WeaponBolt::traceRetireDeadline(std::string_view reason, std::string_view metadata)
{
    const auto tick = retireDeadline.armed ? std::format("{}", retireDeadline.tick) : std::string("null");
    const auto fields = std::format(
        ",\"deadline_source\":\"{}\",\"deadline_tick\":{},\"deadline_metadata\":{{{}}}",
        retireDeadline.source, tick, metadata);
    combatTrace.write("bolt", getHandle(), turn, "deadline", reason, position,
        [this] { return getFlag(OBJECT_FLAG_JUSTCREATED) ? nullptr : &laserPosition; },
        static_cast<GameObjectPtr>(nullptr), static_cast<const WeaponShotInfo*>(nullptr), fields);
}

void WeaponBolt::armRetireDeadline()
{
    const auto& policy = static_cast<WeaponBoltType*>(getObjectType())->retirePolicy;
    const auto now = mission ? mission->getCombatClock().ticks : 0;
    const auto result = retireDeadline.arm(now, policy);
    if (result == mc2::combat::ArmResult::already_attempted) return;
    if (result != mc2::combat::ArmResult::armed)
        MC2_EVENT(Core, "COMBAT_RETIRE", 1, "reject reason={} type={}",
            result == mc2::combat::ArmResult::policy ? "policy" : "deadline_overflow", getObjectType()->whatAmI());
    if (combatTrace.live && mc2_diag::tagEnabled("VFX_COMBAT"))
        traceRetireDeadline("arm", std::format("\"combat_tick\":{},\"policy_ticks\":{}", now,
            policy.ticks ? std::format("{}", *policy.ticks) : std::string("null")));
}

// Each projectile owns its share of the rack damage and can be intercepted independently.
void WeaponBolt::splitSalvo(GameObjectPtr source, GameObjectPtr dest, long sourceHS, long targetHS)
{
	if (weaponShot.masterId < 0) return;
	auto& component = MasterComponent::masterList[weaponShot.masterId];
	if (component.getForm() != COMPONENT_FORM_WEAPON_MISSILE || component.isDefensiveWeapon()) return;
	const long count = component.getWeaponAmmoAmount();
	if (count <= 1) return;
	weaponShot.damage /= static_cast<float>(count);
	for (long round = 1; round < count; ++round) {
		auto* bolt = ObjectManager->createWeaponBolt(effectId);
		if (!bolt) break;
		if (dest) bolt->connect(source, dest, &weaponShot, sourceHS, targetHS, false);
		else bolt->connect(source, *targetPosition, &weaponShot, sourceHS, false);
	}
}

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

void WeaponBolt::applySoftSplash(const glm::vec3& at, GameObjectWatchID directWID)
{
    const auto* type = static_cast<WeaponBoltType*>(getObjectType());
    if ((MPlayer && !MPlayer->isServer()) || weaponShot.masterId < 0 ||
        !soft_splash::eligible(MasterComponent::masterList[weaponShot.masterId].getForm() ==
            COMPONENT_FORM_WEAPON_MISSILE, type->isBeam, type->areaEffectDmg, type->areaEffectRad)) return;
    auto* attacker = ObjectManager->getByWatchID(ownerWID);
    if (!attacker) return;
    for (long i = 0; i < ObjectManager->getNumMovers(); ++i) {
        auto* target = ObjectManager->getMover(i);
        if (!target || !soft_splash::candidate(target->isMarine(),
            target->isDestroyed() || target->isDisabled(), target->getWatchID(false) == directWID,
            target->getTeamId() == attacker->getTeamId())) continue;
        const auto delta = target->getPosition() - at;
        const float damage = soft_splash::damage(weaponShot.damage, delta.x * delta.x + delta.y * delta.y);
        if (damage == 0.0f) continue;
        auto shot = weaponShot;
        shot.attackerWID = ownerWID;
        shot.damage = damage;
        shot.hitLocation = GROUNDVEHICLE_LOCATION_FRONT; // Fixed: no extra simulation RNG draw.
        shot.entryAngle = target->relFacingTo(at);
        traceCombat("damage", "soft_splash", target, &shot);
        target->handleWeaponHit(&shot, MPlayer != nullptr);
        // The stock kill alarm excludes marine ground vehicles from pilot statistics.
        if ((target->isDisabled() || target->isDestroyed()) && attacker->isMover() && attacker->getPilot()) {
            ++attacker->getPilot()->numMechKills[VEHICLE_CLASS_ELEMENTAL][COMBAT_STAT_MISSION];
            traceCombat("kill", "soft_splash", target, &shot);
        }
    }
}

void WeaponBolt::destroy (void)
{
	traceCombatRetire("teardown");
	hitTarget = false;
	if (targetPosition)
	{
		targetPosition.reset();
		targetPosition = nullptr;
	}

    mc2::tick::releaseLight(pointLight, eye, lightId);
    mc2::tick::releaseEffect(hitEffect);
    mc2::tick::releaseEffect(muzzleEffect);
    mc2::tick::releaseEffect(gosEffect);
    mc2::tick::releaseEffect(missEffect);
    mc2::tick::releaseEffect(waterMissEffect);
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
	retireDeadline = {};
	presentationStarted = false;
	presentationImpactStarted = false;
	GameObject::init(create, _type);

	setFlag(OBJECT_FLAG_JUSTCREATED, true);

	objectClass = WEAPONBOLT;
	
	

	destroy();
	lightId = -9; // reset only after retiring the previous light id
	mcTextureHandle = 0;
	gosTextureHandle = 0xffffffff;

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
void WeaponBolt::setTargetPosition (glm::vec3 pos)
{
	if (!targetPosition)
		targetPosition = std::make_unique_for_overwrite<glm::vec3>();
		
	*targetPosition = pos;
}

//***************************************************************************
#include "save_struct_zero.h"
#include "weaponbolt_value_codec.h"
void WeaponBolt::Save (PacketFilePtr file, long packetNum)
{
	MC2_SAVE_STRUCT(WeaponBoltData, data);
	CopyTo(&data);

	auto bytes = mc2::save::encodeTaggedRecord(mc2::save::WeaponBoltRecord{data}, mc2::save::weaponBoltTag);
	if (bytes.empty()) Fatal(0, "Invalid bolt ObjectSlot");
	file->writePacket(packetNum, bytes.data(), static_cast<unsigned int>(bytes.size()), STORAGE_TYPE_ZLIB);
}

//***************************************************************************
void WeaponBolt::CopyTo (WeaponBoltData *data)
{																	   
	data->ownerWID = ownerWID;
	data->hotSpotNumber = hotSpotNumber;

	data->targetWID = targetWID;
	data->targetHotSpot = targetHotSpot;

	glm::vec3 dmy;
	dmy = glm::vec3(0.0f);

	if (targetPosition)
		data->targetPosition = *targetPosition;
	else
		data->targetPosition = dmy;

	data->distanceToTarget = distanceToTarget;
	data->halfDistanceToTarget = halfDistanceToTarget;

	data->weaponShot = weaponShot;

	data->laserPosition = laserPosition;
	memcpy(data->laserVertices,laserVertices,sizeof(glm::vec3) * 4);
	memcpy(data->laserSide,laserSide,sizeof(glm::vec3) * 4);

	data->effectId = effectId;
	data->hitTarget = hitTarget;

	data->timeLeft = timeLeft;
	data->hsPos = hsPos;

	data->hitLeft = hitLeft;
	data->mcTextureHandle = 0; // Reserved on disk; runtime handles are resolved from the bolt type at init.
	data->gosTextureHandle = 0; // Runtime texture creation order must not affect saves; Load ignores this field.
	data->startUV = startUV;
	data->goalHeight = goalHeight;
	data->retireDeadlineTick = retireDeadline.tick;
	data->retireDeadlineState = retireDeadline.savedState();
	if (mission && combatTrace.live && mc2_diag::tagEnabled("VFX_COMBAT")) {
		const auto& clock = mission->getCombatClock();
		traceRetireDeadline("save", std::format("\"combat_tick\":{},\"fraction_bits\":{},\"armed\":{},\"attempted\":{}",
			clock.ticks, std::bit_cast<std::uint64_t>(clock.frac), retireDeadline.armed, retireDeadline.attempted));
	}

	GameObject::CopyTo(data);
}

//---------------------------------------------------------------------------
void WeaponBolt::Load (WeaponBoltData *data)
{
	// All supported save versions use the same reserved handle slots. Ignore
	// legacy nonzero values; init resolved this bolt type's textures for this run.
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
		targetPosition = std::make_unique_for_overwrite<glm::vec3>();
		*targetPosition = data->targetPosition;
	}

	distanceToTarget = data->distanceToTarget;
	halfDistanceToTarget = data->halfDistanceToTarget;

	weaponShot = data->weaponShot;

	laserPosition = data->laserPosition;
	memcpy(laserVertices,data->laserVertices,sizeof(glm::vec3) * 4);
	memcpy(laserSide,data->laserSide,sizeof(glm::vec3) * 4);

	effectId = data->effectId;
	hitTarget = data->hitTarget;
	retireDeadline = mc2::combat::RetireDeadline::restore(data->retireDeadlineTick,
		data->retireDeadlineState, static_cast<WeaponBoltType*>(getObjectType())->retirePolicy);

	timeLeft = data->timeLeft;
	hsPos = data->hsPos;

	hitLeft = data->hitLeft;
	startUV = data->startUV;
	goalHeight = data->goalHeight;
	combatTrace.effect = effectId;
	traceCombat("restore");
	if (mission && combatTrace.live && mc2_diag::tagEnabled("VFX_COMBAT")) {
		const auto& clock = mission->getCombatClock();
		traceRetireDeadline("restore", std::format("\"combat_tick\":{},\"fraction_bits\":{},\"armed\":{},\"attempted\":{}",
			clock.ticks, std::bit_cast<std::uint64_t>(clock.frac), retireDeadline.armed, retireDeadline.attempted));
	}
	if (!getExists()) traceCombatRetire("loaded_inactive");
}

//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
// Successful gated area-effect aimed shots retain weaponShot.hitLocation in single-player;
// Only that target is excluded from the blast; neighbours keep stock splash.
bool WeaponBolt::deliverAimedAreaHit (CarnagePtr explosion)
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
	// This existing exclusion is saved with the explosion, including its delay.
	if (explosion) explosion->setCollisionFreeFromWID(tgtWID);
	aimedTarget->handleWeaponHit(&weaponShot);
	return true;
}

//---------------------------------------------------------------------------

void WeaponBolt::finishNow (void)
{
	if (!getExists()) { destroy(); return; }
	if (getExists())
	{

        mc2::tick::releaseEffect(hitEffect);
        mc2::tick::releaseEffect(muzzleEffect);
        mc2::tick::releaseEffect(missEffect);
        mc2::tick::releaseEffect(waterMissEffect);
        mc2::tick::releaseEffect(gosEffect);
        mc2::tick::releaseLight(pointLight, eye, lightId);
		GameObjectPtr target = ObjectManager->getByWatchID(targetWID);
		if (target && !((static_cast<WeaponBoltTypePtr>(getObjectType()))->areaEffectDmg > 0.0f &&
			(static_cast<WeaponBoltTypePtr>(getObjectType()))->areaEffectRad > 0.0f))
		{
			submitDamage(target);
		}

		if ((static_cast<WeaponBoltTypePtr>(getObjectType()))->hitEffectObjNum &&
			((static_cast<WeaponBoltTypePtr>(getObjectType()))->areaEffectDmg > 0.0f) &&
			((static_cast<WeaponBoltTypePtr>(getObjectType()))->areaEffectRad > 0.0f))
		{
			// The located target takes one hit; the explosion still damages neighbours.
			float areaDmg = weaponShot.masterId >= 0 && MasterComponent::masterList[weaponShot.masterId].getForm() == COMPONENT_FORM_WEAPON_MISSILE
				? weaponShot.damage : (static_cast<WeaponBoltTypePtr>(getObjectType()))->areaEffectDmg;

			//Create the explosion here.  HIT or MISS, its the same one!!
			// Damage is done by explosion NOT by hit!
			auto* explosion = ObjectManager->createExplosion((static_cast<WeaponBoltTypePtr>(getObjectType()))->hitEffectObjNum,
											nullptr,*targetPosition,
											areaDmg,
											(static_cast<WeaponBoltTypePtr>(getObjectType()))->areaEffectRad);
			deliverAimedAreaHit(explosion);
		}

		applySoftSplash(target ? target->getPosition() : (targetPosition ? *targetPosition : position), targetWID);
		traceCombatRetire("forced_finish");
		MC2_DESTROY(this, "bolt_impact");
	}
}

void WeaponBolt::createPresentationResources()
{
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
	if ((static_cast<WeaponBoltTypePtr>(getObjectType()))->textureName && S_stricmp((static_cast<WeaponBoltTypePtr>(getObjectType()))->textureName.get(),"NONE") != 0)
	{
		char tPath[1024];
		mc2::fmt::to_buf(tPath, "{}128" PATH_SEPARATOR "", mc2::fmt::safe(tglPath));

		FullPathFileName textureName;
		textureName.init(tPath,(static_cast<WeaponBoltTypePtr>(getObjectType()))->textureName.get(),".tga");

		if ((static_cast<WeaponBoltTypePtr>(getObjectType()))->mipTexture)
			mcTextureHandle = mc2::textures::load(textureName,gos_Texture_Alpha,0);
		else
			mcTextureHandle = mc2::textures::load(textureName,gos_Texture_Alpha,gosHint_DisableMipmap);
	}
	else
	{
		mcTextureHandle = 0;
		gosTextureHandle = 0xffffffff;
	}

}

std::uint64_t WeaponBolt::simulationHash()
{
	mc2::save::Archive state;
	state(ownerWID, hotSpotNumber, targetWID, targetHotSpot, aimedTargetWID,
	      distanceToTarget, halfDistanceToTarget, hitTarget, timeLeft, hitLeft, goalHeight,
	      laserPosition.x, laserPosition.y, laserPosition.z,
	      weaponShot.attackerWID, weaponShot.masterId, weaponShot.damage,
	      weaponShot.hitLocation, weaponShot.entryAngle, retireDeadline.tick, retireDeadline.armed);
	if (targetPosition) state(targetPosition->x, targetPosition->y, targetPosition->z);
	std::uint64_t hash = 14695981039346656037ull;
	for (auto byte : state.bytes()) hash = (hash ^ byte) * 1099511628211ull;
	return hash;
}
