#include "tick_commit.h"
//***************************************************************************
//
//	gameobj.cpp -- File contains the Game Object class code
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef MCLIB_H
#include <atomic>
#include "terrain_part_chunk.h"
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include"mclib.h"
#include "gos_crashbundle.h"
#include"terrain_runtime.h"
#endif
#include "warrior.h"

#ifndef GAMEOBJ_H
#include"gameobj.h"
#endif

#include"appearance_owner.h"
#include"object_zero_new.h"
static_assert(sizeof(std::unique_ptr<Appearance>) == sizeof(Appearance*), "appearanceOwner must use the default deleter");
#include "../GameOS/gameos/utils/mc2log.h"

#ifndef CARNAGE_H
#include"carnage.h"
#endif

#ifndef TEAM_H
#include"team.h"
#endif

#ifndef OBJTYPE_H
#include"objtype.h"
#endif

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef DOBJNUM_H
#include"dobjnum.h"
#endif

#ifndef MOVE_H
#include"move.h"
#endif

#ifndef CMPONENT_H
#include"cmponent.h"
#endif

#ifndef CONTACT_H
#include"contact.h"
#endif

#ifndef UNITDESG_H
#include"unitdesg.h"
#endif

#ifndef MULTPLYR_H
#include"multplyr.h"
#endif

#ifndef MOVER_H
#include"mover.h"
#endif

#ifndef TURRET_H
#include"turret.h"
#endif

#ifndef GAMELOG_H
#include"gamelog.h"
#include"gos_profiler.h"
#endif

#ifndef COMNDR_H
#include"comndr.h"
#endif

#include <stdlib.h>
#include <stdio.h>   // printf, fflush
#include <stdint.h>
#include "../GameOS/gameos/diagnostic_trace.h"   // COMBAT-TRACE-1: weapon-fire JSONL events
#include "Gates/gate.h"
#include "mc2fmt.h"

//---------------------------------------------------------------------------
extern GameLog* CombatLog;

extern MissionMapPtr GameMap;
extern float metersPerWorldUnit;
extern float worldUnitsPerMeter;

extern TeamPtr homeTeam;
extern float MechClassWeights[std::to_underlying(NUM_MECH_CLASSES)];

extern char* ExceptionGameMsg;
char ChunkDebugMsg[5120];

unsigned long GameObject::spanMask = 0;
float GameObject::blockCaptureRange = 0.0;
bool GameObject::initialize = false;

//---------------------------------------------------------------------------
// Tier-1 instrumentation (stability spec §3.1-3.4)
//---------------------------------------------------------------------------
// Defined at file scope in mclib/tgl.cpp:3718.
extern std::atomic<uint32_t> g_mc2FrameCounter;

static const bool s_destroyTrace = mc2::gate::get<"MC2_DESTROY_TRACE">();

// Out-of-line because canBeSeen() needs the full Appearance type.
bool GameObject::canBeSeen_instr (void) const {
	return appearance ? appearance->canBeSeen() : false;
}

const char* getObjectClassName (ObjectClass kind) {
	switch (kind) {
		case INVALID:       return "INVALID";
		case BASEOBJECT:    return "BaseObject";
		case TERRN:         return "Terrn";
		case BATTLEMECH:    return "BattleMech";
		case GROUNDVEHICLE: return "GroundVehicle";
		case ELEMENTAL:     return "Elemental";
		case EXPLOSION:     return "Explosion";
		case FIRE:          return "Fire";
		case ARTILLERY:     return "Artillery";
		case MOVER:         return "Mover";
		case GAMEOBJECT:    return "GameObject";
		case BIGGAMEOBJECT: return "BigGameObject";
		case COMPONENT:     return "Component";
		case WEAPON:        return "Weapon";
		case PROJECTILE:    return "Projectile";
		case LASERWEAPON:   return "LaserWeapon";
		case PPC:           return "PPC";
		case BUILDING:      return "Building";
		case SMOKE:         return "Smoke";
		case BULLET:        return "Bullet";
		case DEBRIS:        return "Debris";
		case MAP_ICON:      return "MapIcon";
		case TREE:          return "Tree";
		case TERRAINOBJECT: return "TerrainObject";
		case MINE:          return "Mine";
		case BRIDGE:        return "Bridge";
		case JET:           return "Jet";
		case PROJLASER:     return "ProjLaser";
		case TREEBUILDING:  return "TreeBuilding";
		case CAMERADRONE:   return "CameraDrone";
		case TRAINCAR:      return "TrainCar";
		case TURRET:        return "Turret";
		case GATE:          return "Gate";
		case KLIEG_LIGHT:   return "KliegLight";
		case WEAPONBOLT:    return "WeaponBolt";
		default:            return "UNKNOWN";
	}
}

void GameObject::destroy_instr (const char* reason, const char* file, int line) {
	// 1. Snapshot all log fields FIRST, before any other logic runs (spec §3.2).
	const int         snap_exists_was      = getExists() ? 1 : 0;
	const ObjectClass snap_kind            = objectClass;
	const int         snap_in_view         = inView_instr()      ? 1 : 0;
	const int         snap_can_be_seen     = canBeSeen_instr()   ? 1 : 0;
	const int         snap_block_active    = blockActive_instr() ? 1 : 0;
	const int         snap_frames_inactive = static_cast<int>(framesSinceActive);
	const int32_t     snap_last_update_ret = lastUpdateRet;

	// 2. Double-destroy: log-and-return without re-calling setExists.
	if (snap_exists_was == 0) {
		if (s_destroyTrace) {
			char _cbbuf[512];
			snprintf(_cbbuf, sizeof(_cbbuf),
				"[DESTROY v1] frame=%u obj=%p kind=%s reason=%s file=%s line=%d exists_was=0 in_view=%d can_be_seen=%d block_active=%d frames_since_active=%d last_update_ret=%d",
				g_mc2FrameCounter.load(std::memory_order_relaxed), static_cast<void*>(this), getObjectClassName(snap_kind), reason,
				file, line, snap_in_view, snap_can_be_seen, snap_block_active,
				snap_frames_inactive, static_cast<int>(snap_last_update_ret));
			puts(_cbbuf);
			crashbundle_append(_cbbuf);
			fflush(stdout);
		}
		return;
	}

	// 3. Real destruction — unchanged existing behavior.
	setExists(false);

	// 4. Env-gated log line.
	if (s_destroyTrace) {
		char _cbbuf[512];
		snprintf(_cbbuf, sizeof(_cbbuf),
			"[DESTROY v1] frame=%u obj=%p kind=%s reason=%s file=%s line=%d exists_was=1 in_view=%d can_be_seen=%d block_active=%d frames_since_active=%d last_update_ret=%d",
			g_mc2FrameCounter.load(std::memory_order_relaxed), static_cast<void*>(this), getObjectClassName(snap_kind), reason,
			file, line, snap_in_view, snap_can_be_seen, snap_block_active,
			snap_frames_inactive, static_cast<int>(snap_last_update_ret));
		puts(_cbbuf);
		crashbundle_append(_cbbuf);
		fflush(stdout);
	}
}

extern float maxVisualRange;
extern long	visualRangeTable[];

float applyDifficultyWeapon (float dmg, bool isPlayer);

//***************************************************************************
// WEAPONFIRECHUNK class
//***************************************************************************

constexpr int WEAPONFIRECHUNK_HIT_BITS = 1;
constexpr int WEAPONFIRECHUNK_WEAPON_BITS = 5;
constexpr int WEAPONFIRECHUNK_TARGETTYPE_BITS = 2;
constexpr int WEAPONFIRECHUNK_ENTRYQUAD_BITS = 2;
constexpr int WEAPONFIRECHUNK_HITLOCATION_BITS = 4;
constexpr int WEAPONFIRECHUNK_MOVERINDEX_BITS = 7;
constexpr int WEAPONFIRECHUNK_CELLPOS_BITS = 10;
constexpr int WEAPONFIRECHUNK_MISSILES_BITS = 4;
constexpr int WEAPONFIRECHUNK_TARGETID_BITS = 20;
constexpr int WEAPONFIRECHUNK_SPECIALTYPE_BITS = 2;
constexpr int WEAPONFIRECHUNK_SPECIALID_BITS = 8;

constexpr int WEAPONFIRECHUNK_HIT_MASK = 0x00000001;
constexpr int WEAPONFIRECHUNK_WEAPON_MASK = 0x0000001F;
constexpr int WEAPONFIRECHUNK_TARGETTYPE_MASK = 0x00000003;
constexpr int WEAPONFIRECHUNK_ENTRYQUAD_MASK = 0x00000003;
constexpr int WEAPONFIRECHUNK_HITLOCATION_MASK = 0x0000000F;
constexpr int WEAPONFIRECHUNK_MOVERINDEX_MASK = 0x0000007F;
constexpr int WEAPONFIRECHUNK_TERRAINBLOCK_MASK = 0x000000FF;
constexpr int WEAPONFIRECHUNK_TERRAINVERTEX_MASK = 0x000001FF;
constexpr int WEAPONFIRECHUNK_TERRAINITEM_MASK = 0x00000007;
constexpr int WEAPONFIRECHUNK_TRAIN_MASK = 0x000000FF;
constexpr int WEAPONFIRECHUNK_TRAINCAR_MASK = 0x000000FF;
constexpr int WEAPONFIRECHUNK_CELLPOS_MASK = 0x000003FF;
constexpr int WEAPONFIRECHUNK_MISSILES_MASK = 0x0000000F;
constexpr int WEAPONFIRECHUNK_TARGETID_MASK = 0x000FFFFF;
constexpr int WEAPONFIRECHUNK_SPECIALTYPE_MASK = 0x00000003;
constexpr int WEAPONFIRECHUNK_SPECIALID_MASK = 0x000000FF;

constexpr int WEAPONFIRECHUNK_TARGET_MOVER = 0;
constexpr int WEAPONFIRECHUNK_TARGET_TERRAIN = 1;
constexpr int WEAPONFIRECHUNK_TARGET_SPECIAL = 2;
constexpr int WEAPONFIRECHUNK_TARGET_LOCATION = 3;

constexpr int WEAPONFIRECHUNK_SPECIAL_CAMERADRONE = 0;

//---------------------------------------------------------------------------

void WeaponShotInfo::init (GameObjectWatchID _attackerWID, long _masterId, float _damage, long _hitLocation, float _entryAngle) {

	attackerWID = _attackerWID;
	masterId = _masterId;
	damage = _damage;
	hitLocation = _hitLocation;
	entryAngle = _entryAngle;
	if (!MPlayer && _attackerWID)		//No Multiplayer skill levels
	{
		//---------------------------------------------------
		// SKill Levels -- Need Attacker Alignment and Class
		GameObjectPtr _attacker = ObjectManager->getByWatchID(_attackerWID);
		if (_attacker)
		{
			ObjectClass objClass = _attacker->getObjectClass();
			long commanderId = _attacker->getCommanderId();
			bool isPlayer =  commanderId == Commander::home->getId();

			if (!isPlayer)		//Only enemy Mechs, vehicles, elementals and turrets get modified
			{
				if ((objClass == BATTLEMECH) ||
					(objClass == GROUNDVEHICLE) ||
					(objClass == TURRET))
				{
					damage = applyDifficultyWeapon(_damage,isPlayer);
				}
			}
			else
			{
				damage = applyDifficultyWeapon(_damage,isPlayer);
			}
		}
	}

	Assert((damage >= 0.0) && (damage <= 255.0), static_cast<long>(damage), " WeaponShotInfo.init: damage out of range ");
	if (MPlayer && MPlayer->isServer()) {
		damage = static_cast<float>((static_cast<unsigned long>((damage * 4.0)))) * 0.25;
		if ((entryAngle >= -45.0) && (entryAngle <= 45.0))
			entryAngle = 0.0; //MECH_HIT_ARC_FRONT;
		else if ((entryAngle  > -135.0) && (entryAngle < -45.0))
			entryAngle = -90.0; //MECH_HIT_ARC_LEFT;
		else if ((entryAngle > 45.0) && (_entryAngle < 135))
			entryAngle = 90.0; //MECH_HIT_ARC_RIGHT;
		else
			entryAngle = 180.0; //MECH_HIT_ARC_REAR;
	}
}

//---------------------------------------------------------------------------

void WeaponShotInfo::setDamage (float _damage) {

	damage = _damage;
	if (MPlayer && MPlayer->isServer())
		damage = static_cast<float>((static_cast<unsigned long>((damage * 4.0)))) * 0.25;
}

//---------------------------------------------------------------------------

void WeaponShotInfo::setEntryAngle (float _entryAngle) {

	entryAngle = _entryAngle;
	if (MPlayer && MPlayer->isServer()) {
		if ((entryAngle >= -45.0) && (entryAngle <= 45.0))
			entryAngle = 0.0; //MECH_HIT_ARC_FRONT;
		else if ((entryAngle  > -135.0) && (entryAngle < -45.0))
			entryAngle = -90.0; //MECH_HIT_ARC_LEFT;
		else if ((entryAngle > 45.0) && (_entryAngle < 135))
			entryAngle = 90.0; //MECH_HIT_ARC_RIGHT;
		else
			entryAngle = 180.0; //MECH_HIT_ARC_REAR;
	}
}

//---------------------------------------------------------------------------



//---------------------------------------------------------------------------


	

//---------------------------------------------------------------------------

void DebugWeaponFireChunk (WeaponFireChunkPtr chunk1, WeaponFireChunkPtr chunk2, GameObjectPtr attacker) {

	ChunkDebugMsg[0] = '\0';

	char outString[512];

	if (attacker) {
		if (attacker->isMover()) {
			(*std::format_to_n(outString, sizeof(outString) - 1, "attacker = {} ({:d})\n", mc2::fmt::safe(attacker->getName()), mc2::fmt::as_i32(attacker->getPartId())).out = '\0');
			strcat(ChunkDebugMsg, outString);
			}
		else {
			(*std::format_to_n(outString, sizeof(outString) - 1, "attacker = objClass {:d} ({:d})\n", mc2::fmt::as_i32(attacker->getObjectClass()), mc2::fmt::as_i32(attacker->getPartId())).out = '\0');
			strcat(ChunkDebugMsg, outString);
		}
		}
	else
		strcat(ChunkDebugMsg, "attacker = ???\n");

	if (chunk1) {
		strcat(ChunkDebugMsg, "\nCHUNK1\n");

		GameObjectPtr target = nullptr;
		glm::vec3 targetPoint;
		targetPoint = glm::vec3(0.0f);
		bool isTargetPoint = false;
		if (chunk1->targetType == WEAPONFIRECHUNK_TARGET_MOVER)
			target = static_cast<GameObjectPtr>(MPlayer->moverRoster[chunk1->targetId]);
		else if (chunk1->targetType == WEAPONFIRECHUNK_TARGET_TERRAIN)
			target = ObjectManager->findByPartId(chunk1->targetId);
		else if (chunk1->targetType == WEAPONFIRECHUNK_TARGET_SPECIAL)
			target = ObjectManager->findByPartId(chunk1->targetId);
		else if (chunk1->targetType == WEAPONFIRECHUNK_TARGET_LOCATION) {
			targetPoint.x = static_cast<float>(chunk1->targetCell[1]) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
			targetPoint.y = (Terrain::worldUnitsMapSide / 2) - (static_cast<float>(chunk1->targetCell[0]) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
			targetPoint.z = TerrainRuntime::groundElevation(targetPoint);
			isTargetPoint = true;
		}

		if (target) {
			if (target->isMover()) {
				(*std::format_to_n(outString, sizeof(outString) - 1, "target = {} ({:d})\n", mc2::fmt::safe(target->getName()), mc2::fmt::as_i32(target->getPartId())).out = '\0');
				strcat(ChunkDebugMsg, outString);
				}
			else {
				(*std::format_to_n(outString, sizeof(outString) - 1, "target = objClass {:d} ({:d})\n", mc2::fmt::as_i32(target->getObjectClass()), mc2::fmt::as_i32(target->getPartId())).out = '\0');
				strcat(ChunkDebugMsg, outString);
			}
			}
		else if (isTargetPoint) {
			(*std::format_to_n(outString, sizeof(outString) - 1, "target point = ({:f}, {:f}, {:f})\n", targetPoint.x, targetPoint.y, targetPoint.z).out = '\0');
			strcat(ChunkDebugMsg, outString);
			}
		else {
			strcat(ChunkDebugMsg, "target = ???\n");
		}

		(*std::format_to_n(outString, sizeof(outString) - 1, "targetType = {:d}\n", mc2::fmt::as_i32(chunk1->targetType)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "targetId = {:d}\n", mc2::fmt::as_i32(chunk1->targetId)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "specialType = {:d}\n", mc2::fmt::as_i32(chunk1->specialType)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "specialId = {:d}\n", mc2::fmt::as_i32(chunk1->specialId)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "targetCell = [{:d}, {:d}]\n", mc2::fmt::as_i32(chunk1->targetCell[0]), mc2::fmt::as_i32(chunk1->targetCell[1])).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "weaponIndex = {:d}\n", mc2::fmt::as_i32(chunk1->weaponIndex)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "hit = {:c}\n", mc2::fmt::as_char(chunk1->hit ? 'T' : 'N')).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "entryAngle = {:d}\n", mc2::fmt::as_i32(chunk1->entryAngle)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "numMissiles = {:d}\n", mc2::fmt::as_i32(chunk1->numMissiles)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "hitLocation = {:d}\n", mc2::fmt::as_i32(chunk1->hitLocation)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "data = {:x}\n", mc2::fmt::as_u32(chunk1->data)).out = '\0');
		strcat(ChunkDebugMsg, outString);
	}

	if (chunk2) {
		strcat(ChunkDebugMsg, "\nCHUNK2\n");

		GameObjectPtr target = nullptr;
		glm::vec3 targetPoint;
		targetPoint = glm::vec3(0.0f);
		bool isTargetPoint = false;
		if (chunk2->targetType == WEAPONFIRECHUNK_TARGET_MOVER)
			target = static_cast<GameObjectPtr>(MPlayer->moverRoster[chunk2->targetId]);
		else if (chunk2->targetType == WEAPONFIRECHUNK_TARGET_TERRAIN)
			target = ObjectManager->findByPartId(chunk2->targetId);
		else if (chunk2->targetType == WEAPONFIRECHUNK_TARGET_SPECIAL)
			target = ObjectManager->findByPartId(chunk2->targetId);
		else if (chunk2->targetType == WEAPONFIRECHUNK_TARGET_LOCATION) {
			targetPoint.x = static_cast<float>(chunk2->targetCell[1]) * Terrain::worldUnitsPerCell + Terrain::worldUnitsPerCell / 2 - Terrain::worldUnitsMapSide / 2;
			targetPoint.y = (Terrain::worldUnitsMapSide / 2) - (static_cast<float>(chunk2->targetCell[0]) * Terrain::worldUnitsPerCell) - Terrain::worldUnitsPerCell / 2;
			targetPoint.z = TerrainRuntime::groundElevation(targetPoint);
			isTargetPoint = true;
		}

		if (target) {
			if (target->isMover()) {
				(*std::format_to_n(outString, sizeof(outString) - 1, "target = {} ({:d})\n", mc2::fmt::safe(target->getName()), mc2::fmt::as_i32(target->getPartId())).out = '\0');
				strcat(ChunkDebugMsg, outString);
				}
			else {
				(*std::format_to_n(outString, sizeof(outString) - 1, "target = objClass {:d} ({:d})\n", mc2::fmt::as_i32(target->getObjectClass()), mc2::fmt::as_i32(target->getPartId())).out = '\0');
				strcat(ChunkDebugMsg, outString);
			}
			}
		else if (isTargetPoint) {
			(*std::format_to_n(outString, sizeof(outString) - 1, "target point = ({:f}, {:f}, {:f})\n", targetPoint.x, targetPoint.y, targetPoint.z).out = '\0');
			strcat(ChunkDebugMsg, outString);
			}
		else {
			strcat(ChunkDebugMsg, "target = ???\n");
		}

		(*std::format_to_n(outString, sizeof(outString) - 1, "targetType = {:d}\n", mc2::fmt::as_i32(chunk2->targetType)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "targetId = {:d}\n", mc2::fmt::as_i32(chunk2->targetId)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "specialType = {:d}\n", mc2::fmt::as_i32(chunk2->specialType)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "specialId = {:d}\n", mc2::fmt::as_i32(chunk2->specialId)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "targetCell = [{:d}, {:d}]\n", mc2::fmt::as_i32(chunk2->targetCell[0]), mc2::fmt::as_i32(chunk1->targetCell[1])).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "weaponIndex = {:d}\n", mc2::fmt::as_i32(chunk2->weaponIndex)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "hit = {:c}\n", mc2::fmt::as_char(chunk2->hit ? 'T' : 'N')).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "entryAngle = {:d}\n", mc2::fmt::as_i32(chunk2->entryAngle)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "numMissiles = {:d}\n", mc2::fmt::as_i32(chunk2->numMissiles)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "hitLocation = {:d}\n", mc2::fmt::as_i32(chunk2->hitLocation)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "data = {:x}\n", mc2::fmt::as_u32(chunk2->data)).out = '\0');
		strcat(ChunkDebugMsg, outString);
	}

	File debugFile;
	debugFile.create("wfchunk.dbg");
	debugFile.writeString(ChunkDebugMsg);
	debugFile.close();

	ExceptionGameMsg = ChunkDebugMsg;
}

//---------------------------------------------------------------------------

void LogWeaponFireChunk (WeaponFireChunkPtr chunk, GameObjectPtr attacker, GameObjectPtr target) {
	// COMBAT-TRACE-1: every weapon fire (mech/vehicle/turret — all 10 fire sites) routes
	// through here, so emitting one diagnostic event captures all combat engagement
	// headlessly: attacker/target watch-IDs + team-IDs. Opt-in via MC2_DIAG_TAGS=...,COMBAT;
	// the tagEnabled() short-circuit makes it zero-cost otherwise. getWatchID(false) avoids
	// the assign side-effect in this trace path (live combatants already have a WID).
	if (mc2_diag::tagEnabled("COMBAT") && attacker && target) {
		char _cbf[160];
		(*std::format_to_n(_cbf, (sizeof(_cbf)) - 1,
			"{{\"ev\":\"fire\",\"a_wid\":{:d},\"a_team\":{:d},\"t_wid\":{:d},\"t_team\":{:d}}}",
			mc2::fmt::as_u32(attacker->getWatchID(false)), mc2::fmt::as_i32(attacker->getTeamId()),
			mc2::fmt::as_u32(target->getWatchID(false)), mc2::fmt::as_i32(target->getTeamId())).out = '\0');
		mc2_diag::writeEvent("COMBAT", 1, g_mc2FrameCounter.load(std::memory_order_relaxed), _cbf);
	}
}

//---------------------------------------------------------------------------

// COMBAT-TRACE-1: emit a death event when an object transitions to DISABLED/DESTROYED.
// Called from GameObject::setStatus (gameobj.h) only on the alive->dead transition, so the
// per-call cost is paid once per kill; tagEnabled() short-circuits when COMBAT is off.
// No attacker context here (setStatus is on the dying object) — wid/team/class + the JSONL
// ts_ms timestamp are enough to count + time kills. {ev:disabled|destroyed, wid, team, class}.
void GameObject::emitCombatDeathTrace (long newStatus)
{
	if (!mc2_diag::tagEnabled("COMBAT"))
		return;
	char _cbf[160];
	(*std::format_to_n(_cbf, (sizeof(_cbf)) - 1,
		"{{\"ev\":\"{}\",\"wid\":{:d},\"team\":{:d},\"class\":{:d}}}",
		mc2::fmt::safe((newStatus == OBJECT_STATUS_DESTROYED) ? "destroyed" : "disabled"),
		mc2::fmt::as_u32(getWatchID(false)), mc2::fmt::as_i32(getTeamId()), mc2::fmt::as_i32(static_cast<int>(getObjectClass()))).out = '\0');
	mc2_diag::writeEvent("COMBAT", 1, 0, _cbf);
}

//---------------------------------------------------------------------------

void WeaponFireChunk::buildMoverTarget (GameObjectPtr target,
										long _weaponIndex,
										bool _hit,
										float _entryAngle,
										long _numMissiles,
										long _hitLocation) {

	targetType = WEAPONFIRECHUNK_TARGET_MOVER;
	targetId = (static_cast<MoverPtr>(target))->getNetRosterIndex();
	weaponIndex = _weaponIndex;
	hit = _hit;
	if ((_entryAngle >= -45.0) && (_entryAngle <= 45.0))
		entryAngle = 0; //MECH_HIT_ARC_FRONT;
	else if ((_entryAngle  > -135.0) && (_entryAngle < -45.0))
		entryAngle = 2; //MECH_HIT_ARC_LEFT;
	else if ((_entryAngle > 45.0) && (_entryAngle < 135))
		entryAngle = 3; //MECH_HIT_ARC_RIGHT;
	else
		entryAngle = 1; //MECH_HIT_ARC_REAR;
	numMissiles = _numMissiles;
	hitLocation = _hitLocation;

	Assert((targetId > -1) && (targetId < MAX_MULTIPLAYER_MOVERS), targetId, " WeaponFireChunk.buildMoverTarget: bad targetId ");
	Assert((weaponIndex < 32), weaponIndex, " WeaponFireChunk.buildMoverTarget: bad weaponIndex ");
	Assert((numMissiles > -1) && (numMissiles < 16), numMissiles, " WeaponFireChunk.buildMoverTarget: bad numMissiles ");
	Assert((hitLocation > -2) && (hitLocation < 12), hitLocation, " WeaponFireChunk.buildMoverTarget: bad hitLocation ");

	data = 0;
}

//---------------------------------------------------------------------------

void WeaponFireChunk::buildTerrainTarget (GameObjectPtr target,
										  long _weaponIndex,
										  bool _hit,
										  long _numMissiles) {

	targetType = WEAPONFIRECHUNK_TARGET_TERRAIN;
	targetId = target->getPartId();

	//target->getCellPosition(targetCell[0], targetCell[1]);

	weaponIndex = _weaponIndex;
	hit = _hit;
	numMissiles = _numMissiles;

	Assert(target->getPartId() != -1, target->getObjectClass(), " WeaponFireChunk.buildTerrainTarget: -1 partId ");

	data = 0;
}

//---------------------------------------------------------------------------

void WeaponFireChunk::buildCameraDroneTarget (GameObjectPtr target,
											  long _weaponIndex,
											  bool _hit,
											  float _entryAngle,
											  long _numMissiles) {

	targetType = WEAPONFIRECHUNK_TARGET_SPECIAL;
	targetId = target->getPartId();

	targetCell[0] = 128;
	targetCell[1] = targetId - MIN_CAMERA_DRONE_ID;

	weaponIndex = _weaponIndex;
	hit = _hit;
	if ((_entryAngle >= -45.0) && (_entryAngle <= 45.0))
		entryAngle = 0; //MECH_HIT_ARC_FRONT;
	else if ((_entryAngle  > -135.0) && (_entryAngle < -45.0))
		entryAngle = 2; //MECH_HIT_ARC_LEFT;
	else if ((_entryAngle > 45.0) && (_entryAngle < 135))
		entryAngle = 3; //MECH_HIT_ARC_RIGHT;
	else
		entryAngle = 1; //MECH_HIT_ARC_REAR;
	numMissiles = _numMissiles;
	data = 0;
}

//---------------------------------------------------------------------------

void WeaponFireChunk::buildLocationTarget (glm::vec3 location,
										   long _weaponIndex,
										   bool _hit,
										   long _numMissiles) {

	targetType = WEAPONFIRECHUNK_TARGET_LOCATION;
	land->worldToCell(location, targetCell[0], targetCell[1]);
	weaponIndex = _weaponIndex;
	hit = _hit;
	numMissiles = _numMissiles;

	data = 0;
}

//---------------------------------------------------------------------------

#ifdef _DEBUG
#define DEBUG_WEAPONFIRECHUNK
#endif

void WeaponFireChunk::pack (GameObjectPtr attacker) {

	data = 0;

	switch (targetType) {
		case WEAPONFIRECHUNK_TARGET_MOVER:
			//------------------------
			// Mover Target...
			// current size is 23 bits
			data |= entryAngle;
			data <<= WEAPONFIRECHUNK_HITLOCATION_BITS;
			data |= (hitLocation + 2);
			data <<= WEAPONFIRECHUNK_MOVERINDEX_BITS;
			data |= targetId;
			if (numMissiles > 0) {
				//------------------------------
				// Weapon is a missile weapon...
				data <<= WEAPONFIRECHUNK_MISSILES_BITS;
				data |= numMissiles;
			}
			data <<= WEAPONFIRECHUNK_HIT_BITS;
			break;
		case WEAPONFIRECHUNK_TARGET_TERRAIN:
			//-------------------------
			// Terrain Object Target...
			// current size is 
			data |= terrain_part_chunk::pack(targetId);
			if (numMissiles > 0) {
				//------------------------------
				// Weapon is a missile weapon...
				data <<= WEAPONFIRECHUNK_MISSILES_BITS;
				data |= numMissiles;
			}
			data <<= WEAPONFIRECHUNK_HIT_BITS;
			break;
		case WEAPONFIRECHUNK_TARGET_SPECIAL:
			data |= entryAngle;
			data <<= WEAPONFIRECHUNK_CELLPOS_BITS;
			data |= targetCell[0];
			data <<= WEAPONFIRECHUNK_CELLPOS_BITS;
			data |= targetCell[1];
			if (numMissiles > 0) {
				//------------------------------
				// Weapon is a missile weapon...
				data <<= WEAPONFIRECHUNK_MISSILES_BITS;
				data |= numMissiles;
			}
			data <<= WEAPONFIRECHUNK_HIT_BITS;
			break;
		case WEAPONFIRECHUNK_TARGET_LOCATION:
			//-----------------------------------------
			// Must be a Location Target (or a Miss)...
			data |= targetCell[0];
			data <<= WEAPONFIRECHUNK_CELLPOS_BITS;
			data |= targetCell[1];
			if (numMissiles > 0) {
				//------------------------------
				// Weapon is a missile weapon...
				data <<= WEAPONFIRECHUNK_MISSILES_BITS;
				data |= numMissiles;
			}
			data <<= WEAPONFIRECHUNK_HIT_BITS;
			break;
	}
	if (hit)
		data |= 1;
	data <<= WEAPONFIRECHUNK_WEAPON_BITS;
	data |= weaponIndex;
	data <<= WEAPONFIRECHUNK_TARGETTYPE_BITS;
	data |= targetType;

#ifdef DEBUG_WEAPONFIRECHUNK
	//-------------------------
	// Lots'a error checking...
	if ((targetType < 0) || (targetType > 3)) {
		DebugWeaponFireChunk(this, nullptr, attacker);
		char errMsg[1024];
		(*std::format_to_n(errMsg, sizeof(errMsg) - 1, " WeaponFireChunk.pack: bad targetType {:d} (save wfchunk.dbg file) ", mc2::fmt::as_i32(targetType)).out = '\0');
		Assert(false, targetType, errMsg);
	}
	if ((weaponIndex < 0) || (weaponIndex > 31)) {
		DebugWeaponFireChunk(this, nullptr, attacker);
		char errMsg[1024];
		(*std::format_to_n(errMsg, sizeof(errMsg) - 1, " WeaponFireChunk.pack: bad weaponIndex {:d} (save wfchunk.dbg file) ", mc2::fmt::as_i32(weaponIndex)).out = '\0');
		Assert(false, weaponIndex, errMsg);
	}
	if ((hitLocation < -1) || (hitLocation >= 12)) {
		DebugWeaponFireChunk(this, nullptr, attacker);
		char errMsg[1024];
		(*std::format_to_n(errMsg, sizeof(errMsg) - 1, " WeaponFireChunk.pack: bad hitLocation {:d} (save wfchunk.dbg file) ", mc2::fmt::as_i32(hitLocation)).out = '\0');
		Assert(false, hitLocation, errMsg);
	}
	if ((entryAngle < 0) || (entryAngle > 3)) {
		DebugWeaponFireChunk(this, nullptr, attacker);
		char errMsg[1024];
		(*std::format_to_n(errMsg, sizeof(errMsg) - 1, " WeaponFireChunk.pack: bad entryAngle {:d} (save wfchunk.dbg file) ", mc2::fmt::as_i32(entryAngle)).out = '\0');
		Assert(false, entryAngle, errMsg);
	}
	if ((numMissiles < 0) || (numMissiles > 15)) {
		DebugWeaponFireChunk(this, nullptr, attacker);
		char errMsg[1024];
		(*std::format_to_n(errMsg, sizeof(errMsg) - 1, " WeaponFireChunk.pack: bad numMissiles {:d} (save wfchunk.dbg file) ", mc2::fmt::as_i32(numMissiles)).out = '\0');
		Assert(false, numMissiles, errMsg);
	}
#endif

}

//---------------------------------------------------------------------------

void WeaponFireChunk::unpack (GameObjectPtr attacker) {

	unsigned long tempData = data;

	targetType = (tempData & WEAPONFIRECHUNK_TARGETTYPE_MASK);
	tempData >>= WEAPONFIRECHUNK_TARGETTYPE_BITS;

	weaponIndex = (tempData & WEAPONFIRECHUNK_WEAPON_MASK);
	tempData >>= WEAPONFIRECHUNK_WEAPON_BITS;

	hit = ((tempData & WEAPONFIRECHUNK_HIT_MASK) != 0) ? true : false;
	tempData >>= WEAPONFIRECHUNK_HIT_BITS;

	bool isMissileWeapon = false;
	if (attacker->isMover()) {
		long itemIndex = (static_cast<MoverPtr>(attacker))->numOther + weaponIndex;
		isMissileWeapon = (static_cast<MoverPtr>(attacker))->isWeaponMissile(itemIndex);
		}
	else if (attacker->getObjectClass() == TURRET) {
		//----------------------
		// Attacker is turret...
		isMissileWeapon = (static_cast<Turret*>(attacker))->isWeaponMissile(weaponIndex);
	}

	switch (targetType) {
		case WEAPONFIRECHUNK_TARGET_MOVER:
			//----------------
			// Mover Target...
			if (isMissileWeapon) {
				numMissiles = (tempData & WEAPONFIRECHUNK_MISSILES_MASK);
				tempData >>= WEAPONFIRECHUNK_MISSILES_BITS;
			}

			targetId = (tempData & WEAPONFIRECHUNK_MOVERINDEX_MASK);
			tempData >>= WEAPONFIRECHUNK_MOVERINDEX_BITS;

			hitLocation = ((tempData & WEAPONFIRECHUNK_HITLOCATION_MASK) - 2);
			tempData >>= WEAPONFIRECHUNK_HITLOCATION_BITS;

			entryAngle = (tempData & WEAPONFIRECHUNK_ENTRYQUAD_MASK);
			break;
		case WEAPONFIRECHUNK_TARGET_TERRAIN:
			//-------------------------
			// Terrain Object Target...
			if (isMissileWeapon) {
				numMissiles = (tempData & WEAPONFIRECHUNK_MISSILES_MASK);
				tempData >>= WEAPONFIRECHUNK_MISSILES_BITS;
			}
			targetId = terrain_part_chunk::unpack(tempData & WEAPONFIRECHUNK_TARGETID_MASK).value_or(-1);
			tempData >>= WEAPONFIRECHUNK_TARGETID_BITS;
			break;
		case WEAPONFIRECHUNK_TARGET_SPECIAL:
			//-----------------------
			// Special (Train, CameraDrone) Object Target...
			if (isMissileWeapon) {
				numMissiles = (tempData & WEAPONFIRECHUNK_MISSILES_MASK);
				tempData >>= WEAPONFIRECHUNK_MISSILES_BITS;
			}

			specialId = (tempData & WEAPONFIRECHUNK_SPECIALID_MASK);
			tempData >>= WEAPONFIRECHUNK_SPECIALID_BITS;

			specialType = (tempData & WEAPONFIRECHUNK_SPECIALTYPE_MASK);
			tempData >>= WEAPONFIRECHUNK_SPECIALTYPE_BITS;

			switch (specialType) {
				case WEAPONFIRECHUNK_SPECIAL_CAMERADRONE:
					targetId = MIN_CAMERA_DRONE_ID + specialId;
					break;
				default:
					Fatal(specialType, " WeaponFireChunk.unpack: bad specialType ");
			}

			entryAngle = (tempData & WEAPONFIRECHUNK_ENTRYQUAD_MASK);
			break;
		case WEAPONFIRECHUNK_TARGET_LOCATION:
			//-----------------------------------------
			// Must be a Location Target (or a Miss)...
			if (isMissileWeapon) {
				numMissiles = (tempData & WEAPONFIRECHUNK_MISSILES_MASK);
				tempData >>= WEAPONFIRECHUNK_MISSILES_BITS;
			}

			targetCell[1] = (tempData & WEAPONFIRECHUNK_CELLPOS_MASK);
			tempData >>= WEAPONFIRECHUNK_CELLPOS_BITS;
					
			targetCell[0] = (tempData & WEAPONFIRECHUNK_CELLPOS_MASK);
			break;
	}

	//-------------------------
	// Lots'a error checking...
#ifdef DEBUG_WEAPONFIRECHUNK
	//-------------------------
	// Lots'a error checking...
	if ((targetType < 0) || (targetType > 3)) {
		DebugWeaponFireChunk(this, nullptr, attacker);
		char errMsg[1024];
		(*std::format_to_n(errMsg, sizeof(errMsg) - 1, " WeaponFireChunk.unpack: bad targetType {:d} (save wfchunk.dbg file) ", mc2::fmt::as_i32(targetType)).out = '\0');
		Assert(false, targetType, errMsg);
	}
	if ((weaponIndex < 0) || (weaponIndex > 31)) {
		DebugWeaponFireChunk(this, nullptr, attacker);
		char errMsg[1024];
		(*std::format_to_n(errMsg, sizeof(errMsg) - 1, " WeaponFireChunk.unpack: bad weaponIndex {:d} (save wfchunk.dbg file) ", mc2::fmt::as_i32(weaponIndex)).out = '\0');
		Assert(false, weaponIndex, errMsg);
	}
	if ((hitLocation < -1) || (hitLocation >= 12)) {
		DebugWeaponFireChunk(this, nullptr, attacker);
		char errMsg[1024];
		(*std::format_to_n(errMsg, sizeof(errMsg) - 1, " WeaponFireChunk.unpack: bad hitLocation {:d} (save wfchunk.dbg file) ", mc2::fmt::as_i32(hitLocation)).out = '\0');
		Assert(false, hitLocation, errMsg);
	}
	if ((entryAngle < 0) || (entryAngle > 3)) {
		DebugWeaponFireChunk(this, nullptr, attacker);
		char errMsg[1024];
		(*std::format_to_n(errMsg, sizeof(errMsg) - 1, " WeaponFireChunk.unpack: bad entryAngle {:d} (save wfchunk.dbg file) ", mc2::fmt::as_i32(entryAngle)).out = '\0');
		Assert(false, entryAngle, errMsg);
	}
	if ((numMissiles < 0) || (numMissiles > 15)) {
		DebugWeaponFireChunk(this, nullptr, attacker);
		char errMsg[1024];
		(*std::format_to_n(errMsg, sizeof(errMsg) - 1, " WeaponFireChunk.unpack: bad numMissiles {:d} (save wfchunk.dbg file) ", mc2::fmt::as_i32(numMissiles)).out = '\0');
		Assert(false, numMissiles, errMsg);
	}
	if (!hit) {
		bool isStreakMissile = false;
		if (attacker->isMover()) {
			long itemIndex = (static_cast<MoverPtr>(attacker))->numOther + weaponIndex;
			isStreakMissile = MasterComponent::masterList[(static_cast<MoverPtr>(attacker))->inventory[itemIndex].masterID].getWeaponStreak();
			}
		else if (attacker->getObjectClass() == TURRET) {
			//----------------------
			// Attacker is turret...
			isStreakMissile = (static_cast<TurretPtr>(attacker))->isWeaponStreak(weaponIndex);
		}
		if (isStreakMissile) {
			DebugWeaponFireChunk(this, nullptr, attacker);
			Assert(false, 0, " WeaponFireChunk.unpack: streak missile missed (save wfchunk.dbg file) ");
		}
	}
	GameObjectPtr target = nullptr;
	if (targetType == 0 /*WEAPONFIRECHUNK_TARGET_MOVER*/) {
		target = static_cast<GameObjectPtr>(MPlayer->moverRoster[targetId]);
		//----------------------------------------------------------------------------
		// Mover targets could be NULL now, since we free them when they're destroyed.
		//if (target == NULL) {
		//	DebugWeaponFireChunk (this, NULL, attacker);
		//	Assert(false, 0, " WeaponFireChunk.unpack: NULL Mover Target (save wfchunk.dbg file) ");
		//}
		}
	else if (targetType == 1 /*WEAPONFIRECHUNK_TARGET_TERRAIN*/) {
		target = ObjectManager->findByPartId(targetId);
		if (target == nullptr) {
			DebugWeaponFireChunk (this, nullptr, attacker);
			Assert(false, 0, " WeaponFireChunk.unpack: NULL Terrain Target (save wfchunk.dbg file) ");
		}
		}
	else if (targetType == 2 /*WEAPONFIRECHUNK_TARGET_TRAIN*/) {
		target = ObjectManager->findByPartId(targetId);
		if (target == nullptr) {
			DebugWeaponFireChunk (this, nullptr, attacker);
			Assert(false, 0, " WeaponFireChunk.unpack: NULL Special Target (save wfchunk.dbg file) ");
		}
		}
	else if (targetType == 3 /*WEAPONFIRECHUNK_TARGET_LOCATION*/) {
		// Do nothing...
	}
#endif
}

//---------------------------------------------------------------------------

bool WeaponFireChunk::equalTo (WeaponFireChunkPtr chunk) {

	if (targetType != chunk->targetType) {
		DebugWeaponFireChunk(this, chunk, nullptr);
		return(false);
	}

	if (targetId != chunk->targetId) {
		DebugWeaponFireChunk(this, chunk, nullptr);
		return(false);
	}

	if (targetCell[0] != chunk->targetCell[0]) {
		DebugWeaponFireChunk(this, chunk, nullptr);
		return(false);
	}

	if (targetCell[1] != chunk->targetCell[1]) {
		DebugWeaponFireChunk(this, chunk, nullptr);
		return(false);
	}

	if (specialType != chunk->specialType) {
		DebugWeaponFireChunk(this, chunk, nullptr);
		return(false);
	}

	if (specialId != chunk->specialId) {
		DebugWeaponFireChunk(this, chunk, nullptr);
		return(false);
	}

	if (weaponIndex != chunk->weaponIndex) {
		DebugWeaponFireChunk(this, chunk, nullptr);
		return(false);
	}

	if (hit != chunk->hit) {
		DebugWeaponFireChunk(this, chunk, nullptr);
		return(false);
	}

	if (entryAngle != chunk->entryAngle) {
		DebugWeaponFireChunk(this, chunk, nullptr);
		return(false);
	}

	if (numMissiles != chunk->numMissiles) {
		DebugWeaponFireChunk(this, chunk, nullptr);
		return(false);
	}

	if (hitLocation != chunk->hitLocation) {
		DebugWeaponFireChunk(this, chunk, nullptr);
		return(false);
	}

	return(true);
}

//***************************************************************************
// WEAPONHITCHUNK class
//***************************************************************************

void DebugWeaponHitChunk (WeaponHitChunkPtr chunk1, WeaponHitChunkPtr chunk2) {

	ChunkDebugMsg[0] = '\0';

	char outString[512];

	if (chunk1) {
		strcat(ChunkDebugMsg, "\nCHUNK1\n");

		GameObjectPtr target = nullptr;
		bool isTargetPoint = false;
		if (chunk1->targetType == WEAPONHITCHUNK_TARGET_MOVER)
			target = static_cast<GameObjectPtr>(MPlayer->moverRoster[chunk1->targetId]);
		else if (chunk1->targetType == WEAPONHITCHUNK_TARGET_TERRAIN)
			target = ObjectManager->findByPartId(chunk1->targetId);
		else if (chunk1->targetType == WEAPONHITCHUNK_TARGET_SPECIAL)
			target = ObjectManager->findByPartId(chunk1->targetId);
		else if (chunk1->targetType == WEAPONHITCHUNK_TARGET_LOCATION)
			isTargetPoint = true;

		if (target) {
			if (target->isMover()) {
				(*std::format_to_n(outString, sizeof(outString) - 1, "target = {} ({:d})\n", mc2::fmt::safe(target->getName()), mc2::fmt::as_i32(target->getPartId())).out = '\0');
				strcat(ChunkDebugMsg, outString);
				}
			else {
				(*std::format_to_n(outString, sizeof(outString) - 1, "target = objClass {:d} ({:d})\n", mc2::fmt::as_i32(target->getObjectClass()), mc2::fmt::as_i32(target->getPartId())).out = '\0');
				strcat(ChunkDebugMsg, outString);
			}
			}
		else if (isTargetPoint)
			strcat(ChunkDebugMsg, "target point\n");
		else
			strcat(ChunkDebugMsg, "target = ???\n");

		(*std::format_to_n(outString, sizeof(outString) - 1, "targetType = {:d}\n", mc2::fmt::as_i32(chunk1->targetType)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "targetId = {:d}\n", mc2::fmt::as_i32(chunk1->targetId)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "specialType = {:d}\n", mc2::fmt::as_i32(chunk1->specialType)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "specialId = {:d}\n", mc2::fmt::as_i32(chunk1->specialId)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "cause = {:d}\n", mc2::fmt::as_i32(chunk1->cause)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "damage = {:f}\n", chunk1->damage).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "hitLocation = {:d}\n", mc2::fmt::as_i32(chunk1->hitLocation)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "entryAngle = {:d}\n", mc2::fmt::as_i32(chunk1->entryAngle)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "refit = {}\n", mc2::fmt::safe(chunk1->refit ? "true" : "false")).out = '\0');
		strcat(ChunkDebugMsg, outString);
	}

	if (chunk2) {
		strcat(ChunkDebugMsg, "\nCHUNK2\n");

		GameObjectPtr target = nullptr;
		bool isTargetPoint = false;
		if (chunk2->targetType == WEAPONHITCHUNK_TARGET_MOVER)
			target = static_cast<GameObjectPtr>(MPlayer->moverRoster[chunk2->targetId]);
		else if (chunk2->targetType == WEAPONHITCHUNK_TARGET_TERRAIN)
			target = ObjectManager->findByPartId(chunk2->targetId);
		else if (chunk2->targetType == WEAPONHITCHUNK_TARGET_SPECIAL)
			target = ObjectManager->findByPartId(chunk2->targetId);
		else if (chunk2->targetType == WEAPONHITCHUNK_TARGET_LOCATION)
			isTargetPoint = true;

		if (target) {
			if (target->isMover()) {
				(*std::format_to_n(outString, sizeof(outString) - 1, "target = {} ({:d})\n", mc2::fmt::safe(target->getName()), mc2::fmt::as_i32(target->getPartId())).out = '\0');
				strcat(ChunkDebugMsg, outString);
				}
			else {
				(*std::format_to_n(outString, sizeof(outString) - 1, "target = objClass {:d} ({:d})\n", mc2::fmt::as_i32(target->getObjectClass()), mc2::fmt::as_i32(target->getPartId())).out = '\0');
				strcat(ChunkDebugMsg, outString);
			}
			}
		else if (isTargetPoint)
			strcat(ChunkDebugMsg, "target point");
		else
			strcat(ChunkDebugMsg, "target = ???\n");

		(*std::format_to_n(outString, sizeof(outString) - 1, "targetType = {:d}\n", mc2::fmt::as_i32(chunk2->targetType)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "targetId = {:d}\n", mc2::fmt::as_i32(chunk2->targetId)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "specialType = {:d}\n", mc2::fmt::as_i32(chunk2->specialType)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "specialId = {:d}\n", mc2::fmt::as_i32(chunk2->specialId)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "cause = {:d}\n", mc2::fmt::as_i32(chunk2->cause)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "damage = {:f}\n", chunk2->damage).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "hitLocation = {:d}\n", mc2::fmt::as_i32(chunk2->hitLocation)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "entryAngle = {:d}\n", mc2::fmt::as_i32(chunk2->entryAngle)).out = '\0');
		strcat(ChunkDebugMsg, outString);

		(*std::format_to_n(outString, sizeof(outString) - 1, "refit = {}\n", mc2::fmt::safe(chunk2->refit ? "true" : "false")).out = '\0');
		strcat(ChunkDebugMsg, outString);
	}

	File debugFile;
	debugFile.create("whchunk.dbg");
	debugFile.writeString(ChunkDebugMsg);
	debugFile.close();

	ExceptionGameMsg = ChunkDebugMsg;
}

//---------------------------------------------------------------------------



//---------------------------------------------------------------------------



//---------------------------------------------------------------------------

void WeaponHitChunk::buildMoverTarget (GameObjectPtr target,
									   long _cause,
									   float _damage,
									   long _hitLocation,
									   float _entryAngle,
									   bool isRefit) {

	targetType = WEAPONHITCHUNK_TARGET_MOVER;
	targetId = (static_cast<MoverPtr>(target))->getNetRosterIndex();
	//Assert((targetId > -1) && (targetId < MPlayer->numMovers), targetId, " WeaponHitChunk.buildMoverTarget: bad targetId ");
	cause = _cause;
	//Assert((cause
	damage = _damage;
	//Assert((damage >= ) && (damage < 1024), damage, " WeaponHitChunk.buildMoverTarget: bad damage ");
	hitLocation = _hitLocation;
	//Assert((hitLocation > -2) && (hitLocation < 12), hitLocation, " WeaponHitChunk.buildMoverTarget: bad hitLocation ");
	if ((_entryAngle >= -45.0) && (_entryAngle <= 45.0))
		entryAngle = 0; //MECH_HIT_ARC_FRONT;
	else if ((_entryAngle  > -135.0) && (_entryAngle < -45.0))
		entryAngle = 2; //MECH_HIT_ARC_LEFT;
	else if ((_entryAngle > 45.0) && (_entryAngle < 135))
		entryAngle = 3; //MECH_HIT_ARC_RIGHT;
	else
		entryAngle = 1; //MECH_HIT_ARC_REAR;
	refit = isRefit;

	data = 0;
}

//---------------------------------------------------------------------------

void WeaponHitChunk::buildTerrainTarget (GameObjectPtr target,
										 float _damage) {

	targetType = WEAPONHITCHUNK_TARGET_TERRAIN;
	targetId = target->getPartId();

	damage = _damage;

	data = 0;
}

//---------------------------------------------------------------------------

void WeaponHitChunk::buildCameraDroneTarget (GameObjectPtr target,
											 float _damage,
											 float _entryAngle) {

	targetType = WEAPONHITCHUNK_TARGET_SPECIAL;
	targetId = target->getPartId();

	specialType = WEAPONFIRECHUNK_SPECIAL_CAMERADRONE;
	specialId = targetId - MIN_CAMERA_DRONE_ID;

	damage = _damage;
	//Assert((damage >= ) && (damage < 1024), damage, " WeaponHitChunk.buildMoverTarget: bad damage ");

	if ((_entryAngle >= -45.0) && (_entryAngle <= 45.0))
		entryAngle = 0; //MECH_HIT_ARC_FRONT;
	else if ((_entryAngle  > -135.0) && (_entryAngle < -45.0))
		entryAngle = 2; //MECH_HIT_ARC_LEFT;
	else if ((_entryAngle > 45.0) && (_entryAngle < 135))
		entryAngle = 3; //MECH_HIT_ARC_RIGHT;
	else
		entryAngle = 1; //MECH_HIT_ARC_REAR;

	data = 0;
}

//---------------------------------------------------------------------------

void WeaponHitChunk::build (GameObjectPtr target, WeaponShotInfoPtr shotInfo, bool isRefit) {

	if (!target)
		Fatal(0, " WeaponHitChunk.build: NULL target ");
	Assert((static_cast<float>((static_cast<unsigned long>((shotInfo->damage * 4.0)))) * 0.25) == shotInfo->damage, 0, " WeaponHitChunk.build: damage round error ");
	if (target->isMover()) {
		//---------------------------------------------------------------
		// HACK fix for ammoExplosions without needing to save the weapon
		// master ID...
		if (shotInfo->masterId > 0) {
			if (MasterComponent::masterList[shotInfo->masterId].getForm() == COMPONENT_FORM_AMMO)
				shotInfo->masterId = -4;
			else
				shotInfo->masterId = 0;
		}
		buildMoverTarget(target,
						 shotInfo->masterId,
						 shotInfo->damage,
						 shotInfo->hitLocation,
						 shotInfo->entryAngle,
						 isRefit);
		}
	else {
		switch (target->getObjectClass()) {
			case BUILDING:
			case TREEBUILDING:
			case TREE:
			case GATE:
			case TURRET:
			case BRIDGE:
				buildTerrainTarget(target, shotInfo->damage);
				break;
			case CAMERADRONE:
				buildCameraDroneTarget(target, shotInfo->damage, shotInfo->entryAngle);
				break;
			default:
				//Fatal(0, " WeaponHitChunk.build: bad target type ");
				break;
		}
	}
}

//---------------------------------------------------------------------------

void WeaponHitChunk::pack (void) {

	data = 0;

	switch (targetType) {
		case WEAPONHITCHUNK_TARGET_MOVER:
			//----------------
			// Mover Target...
			if (refit)
				data |= 1;
			data <<= WEAPONHITCHUNK_ENTRYQUAD_BITS;

			data |= entryAngle;
			data <<= WEAPONHITCHUNK_HITLOCATION_BITS;

			data |= (hitLocation + 2);
			data <<= WEAPONHITCHUNK_CAUSE_BITS;

			data |= (cause + 7);
			data <<= WEAPONHITCHUNK_MOVERINDEX_BITS;

			data |= targetId;
			data <<= WEAPONHITCHUNK_DAMAGE_BITS;
			break;
		case WEAPONHITCHUNK_TARGET_TERRAIN:
			//-------------------------
			// Terrain Object Target...
			data |= terrain_part_chunk::pack(targetId);
			data <<= WEAPONHITCHUNK_DAMAGE_BITS;
			break;
		case WEAPONHITCHUNK_TARGET_SPECIAL:
			data |= entryAngle;
			data <<= WEAPONHITCHUNK_SPECIALTYPE_BITS;

			data |= specialType;
			data <<= WEAPONHITCHUNK_SPECIALID_BITS;

			data |= specialId;
			data <<= WEAPONHITCHUNK_DAMAGE_BITS;
			break;
		default:
			Fatal(0, " Bad WeaponHitChunk Target Type ");
	}

	data |= static_cast<unsigned long>((damage * 4.0));
	data <<= WEAPONHITCHUNK_TARGETTYPE_BITS;

	data |= targetType;
}

//---------------------------------------------------------------------------
		
void WeaponHitChunk::unpack (void) {

	unsigned long tempData = data;

	targetType = (tempData & WEAPONHITCHUNK_TARGETTYPE_MASK);
	tempData >>= WEAPONHITCHUNK_TARGETTYPE_BITS;

	damage = static_cast<float>((tempData & WEAPONHITCHUNK_DAMAGE_MASK)) * 0.25;
	tempData >>= WEAPONHITCHUNK_DAMAGE_BITS;
	Assert((damage >= 0.0) && (damage <= 255.0), 0, " WeaponHitChunk.unpack: bad damage ");

	switch (targetType) {
		case WEAPONHITCHUNK_TARGET_MOVER:
			//----------------
			// Mover Target...
			targetId = (tempData & WEAPONHITCHUNK_MOVERINDEX_MASK);
			tempData >>= WEAPONHITCHUNK_MOVERINDEX_BITS;
			Assert((targetId > -1) && (targetId < MAX_MULTIPLAYER_MOVERS), targetId, " WeaponHitChunk.unpack: bad targetId ");

			cause = ((tempData & WEAPONHITCHUNK_CAUSE_MASK) - 7);
			tempData >>= WEAPONHITCHUNK_CAUSE_BITS;
			Assert((cause >= -7) && (cause <= 0), cause, " WeaponHitChunk.unpack: bad cause ");

			hitLocation = ((tempData & WEAPONHITCHUNK_HITLOCATION_MASK) - 2);
			tempData >>= WEAPONHITCHUNK_HITLOCATION_BITS;
			Assert((hitLocation > -2) && (hitLocation < 12), hitLocation, " WeaponHitChunk.unpack: bad hitLocation ");

			entryAngle = (tempData & WEAPONHITCHUNK_ENTRYQUAD_MASK);
			tempData >>= WEAPONHITCHUNK_ENTRYQUAD_BITS;

			refit = (tempData & WEAPONHITCHUNK_REFIT_MASK) ? true : false;

			break;
		case WEAPONHITCHUNK_TARGET_TERRAIN:
			//-------------------------
			// Terrain Object Target...
			targetId = terrain_part_chunk::unpack(tempData & WEAPONHITCHUNK_TARGETID_MASK).value_or(-1);
			tempData >>= WEAPONHITCHUNK_TARGETID_BITS;
			break;
		case WEAPONHITCHUNK_TARGET_SPECIAL:
			//-----------------------
			// Train Object Target...
			specialId = (tempData & WEAPONHITCHUNK_SPECIALID_MASK);
			tempData >>= WEAPONHITCHUNK_SPECIALID_BITS;

			specialType = (tempData & WEAPONHITCHUNK_SPECIALTYPE_MASK);
			tempData >>= WEAPONHITCHUNK_SPECIALTYPE_BITS;

			switch (specialType) {
				case WEAPONHITCHUNK_SPECIAL_CAMERADRONE:
					targetId = MIN_CAMERA_DRONE_ID + specialId;
					break;
				default:
					Fatal(specialType, " WeaponHitChunk.unpack: bad specialType ");
			}
			entryAngle = (tempData & WEAPONHITCHUNK_ENTRYQUAD_MASK);
			break;
		default:
			DebugWeaponHitChunk(this, nullptr);
			Fatal(0, " Bad WeaponHitChunk Target Type ");
	}
}

//---------------------------------------------------------------------------

bool WeaponHitChunk::equalTo (WeaponHitChunkPtr chunk) {

	if (targetType != chunk->targetType) {
		DebugWeaponHitChunk(this, chunk);
		return(false);
	}

	if (targetId != chunk->targetId) {
		DebugWeaponHitChunk(this, chunk);
		return(false);
	}

	if (specialType != chunk->specialType) {
		DebugWeaponHitChunk(this, chunk);
		return(false);
	}

	if (specialId != chunk->specialId) {
		DebugWeaponHitChunk(this, chunk);
		return(false);
	}

	if (cause != chunk->cause) {
		DebugWeaponHitChunk(this, chunk);
		return(false);
	}

	if (damage != chunk->damage) {
		DebugWeaponHitChunk(this, chunk);
		return(false);
	}

	if (entryAngle != chunk->entryAngle) {
		DebugWeaponHitChunk(this, chunk);
		return(false);
	}

	if (refit != chunk->refit) {
		DebugWeaponHitChunk(this, chunk);
		return(false);
	}

	if (hitLocation != chunk->hitLocation) {
		DebugWeaponHitChunk(this, chunk);
		return(false);
	}

	return(true);
}

//---------------------------------------------------------------------------

bool WeaponHitChunk::valid (long from) {

	if (refit) {
		if (targetType != 0)
			Fatal(0, " Multiplayer.handleAppWeaponHitUpdate: bad targetType for refit ");
		MoverPtr target = MPlayer->moverRoster[targetId];
		if (!target) {
			if (CombatLog)
				CombatLog->write("WeaponHitChunk INVALID: refit has NULL target");
			DebugWeaponHitChunk(this, nullptr);
			return(false);
		}
		}
	else {
		WeaponShotInfo shotInfo;
		shotInfo.attackerWID = 0;
		shotInfo.masterId = cause;
		shotInfo.damage = damage;
		shotInfo.hitLocation = hitLocation;
		//shotInfo.entryAngle = entryQuadTable[entryAngle];
		switch (targetType) {
			case WEAPONHITCHUNK_TARGET_MOVER:
				#ifdef _DEBUG
				//if (WeaponHitLog)
				//	WeaponHitLog->writeString("==>handleAppWeaponHit\n");
				#endif
				//FromMP = true;
				//if (moverRoster[chunk.targetId])
				//	moverRoster[chunk.targetId]->handleWeaponHit(&shotInfo);
				//FromMP = false;
				break;
			case WEAPONHITCHUNK_TARGET_TERRAIN:
			case WEAPONHITCHUNK_TARGET_SPECIAL: {
				GameObjectPtr target = ObjectManager->findByPartId(targetId);
				if (!target) {
					char s[512];
					long r = (targetId - MIN_TERRAIN_PART_ID) / gTerrainPartIdStride;
					long c = targetId - MIN_TERRAIN_PART_ID - (gTerrainPartIdStride * r);
					(*std::format_to_n(s, sizeof(s) - 1, "WeaponHitChunk INVALID: NULL terrain target ({:d}), rc = {:d},{:d}, type = {:d}, damage = {:.2f}, data={:d}", mc2::fmt::as_i32(targetId), mc2::fmt::as_i32(r), mc2::fmt::as_i32(c), mc2::fmt::as_i32(targetType), damage, mc2::fmt::as_i32(data)).out = '\0');
					if (CombatLog) {
						CombatLog->write(s);
						CombatLog->dump();
						//CombatLog->close();
					}
					DebugWeaponHitChunk(this, nullptr);
					return(false);
				}
				}
				break;
			default:
				if (CombatLog) {
					CombatLog->write("WeaponHitChunk INVALID: bad targetType");
					CombatLog->dump();
				}
				DebugWeaponHitChunk(this, nullptr);
				return(false);
		}
	}

	return(true);
}

//---------------------------------------------------------------------------
// class GameObject	
//---------------------------------------------------------------------------

void* GameObject::operator new (size_t ourSize) {

	// B7o.9: the CRT heap (objectCache is gone), zero-filled (G4, kept): object_zero_new.h.
	return objectZeroNew(ourSize);
}

//---------------------------------------------------------------------------

void GameObject::operator delete (void* us) {

	::operator delete(us);
}

//---------------------------------------------------------------------------

void GameObject::init (bool create) {
	if (ObjectManager) ObjectManager->invalidateEntity(handle, this);
	// The draw compares this against its extract stamp (atomic_ref: the sim worker re-inits pooled slots mid-draw).
	std::atomic_ref<uint32_t>(generation).fetch_add(1, std::memory_order_release);

	objectClass = GAMEOBJECT;
	if (initialize) {
		handle = 0;
		adoptAppearance(nullptr);
		threatRating = 0;
	}
	partId = 0;
	watchID = 0;
	typeHandle = 0;
	position = glm::vec3(0.0f);
	cellPositionRow = 0;
	cellPositionCol = 0;
	flags = OBJECT_FLAG_USEME | OBJECT_FLAG_AWAKE;
	debugFlags = 0;
	status = OBJECT_STATUS_NORMAL;
	tonnage = 0.0;
	d_vertexNum = -1;
	//team = 255;
	collisionFreeFromWID = 0;
	collisionFreeTime = 0.0;
	screenPos.x = 0.0f;
	screenPos.y = 0.0f;
	screenPos.z = 0.0f;
	screenPos.w = 0.0f;
	windowsVisible = 0;
	explDamage = 0.0;
	explRadius = 0.0;
	maxCV = 0;
	curCV = 0;
	lastFrameTime = 0.0;
	blipFrame = 0;
	numAttackers = 0;
	rotation = 0.0;
	drawFlags = 0;
}

//---------------------------------------------------------------------------

// B7o.2/B7o.3: the ONLY writers of GameObject::appearance (the census exempts just these two bodies).
// B7o.3: appearanceOwner owns the appearance; `appearance` is a view synced here. Adopting over a live appearance frees the previous
// one (the init replacement paths used to overwrite it). The owner's destructor is a backstop for an actor that is destroyed
// without releasing (destroy() still releases at the same position as before, see G2).
void GameObject::adoptAppearance (std::unique_ptr<Appearance> newAppearance) {

    if (mc2::tick::collecting()) releaseAppearance();
	mc2::adoptOwnedView(appearanceOwner, appearance, newAppearance.release());
}

void GameObject::releaseAppearance (void) {

	verifyAppearanceView("releaseAppearance");
    if (mc2::tick::collecting()) {
        auto* retired = appearanceOwner.release();
        mc2::releaseOwnedView(appearanceOwner, appearance);
        mc2::tick::retireObject(retired);
    } else {
        mc2::releaseOwnedView(appearanceOwner, appearance);
    }
}

void GameObject::verifyAppearanceView (const char* where) {

	if (mc2::ownedViewDiverged(appearanceOwner, appearance))
		MC2_LOG(Sim, Warn, "{}: GameObject appearance view {} diverged from its owner {}", where, static_cast<const void*>(appearance), static_cast<const void*>(appearanceOwner.get()));
}

//---------------------------------------------------------------------------

ObjectTypePtr GameObject::getObjectType (void) {

	return(ObjectManager->getObjectType(typeHandle));
}

//---------------------------------------------------------------------------

void GameObject::init (bool create, ObjectTypePtr _type) {

	typeHandle = _type->whatAmI();
#ifdef _DEBUG
//	id = _type->getName();
#endif
	objectClass = GAMEOBJECT;

	// UNIT-PROFILE-SEAM-1: type now bound; let subclasses seed runtime state.
	onTypeBound();
}

//---------------------------------------------------------------------------

unsigned long GameObject::getWatchID (bool assign) {

	if ((watchID == 0) && assign)
		ObjectManager->setWatchID(this);
	return(watchID);
}

//---------------------------------------------------------------------------

void GameObject::getBlockAndVertexNumber (int &blockNum, int &vertexNum) {

	Assert(Terrain::worldUnitsPerVertex==128,0," Optimizations now broken ");

	// What is our block and vertex number?
	long mx = (float2long(position.x) >> 7) + Terrain::halfVerticesMapSide;
	long blockX = float2long(mx * Terrain::oneOverVerticesBlockSide);

	long my = Terrain::halfVerticesMapSide - ((float2long(position.y) >> 7) + 1);
	long blockY = float2long(my * Terrain::oneOverVerticesBlockSide);

	blockNum = blockX + (blockY * Terrain::blocksMapSide);

	long vertexX = mx - (blockX * Terrain::verticesBlockSide);
	long vertexY = my - (blockY * Terrain::verticesBlockSide);

	vertexNum = vertexX + (vertexY * Terrain::verticesBlockSide);
}	

//---------------------------------------------------------------------------

long GameObject::kill (void) 
{
	//------------------------------------------------------------
	//Once new MC II ObjMgr is up and running, put this back in...
	// DO NOT DO THIS ANYMORE. EACH OBJECT MUST HAVE ITS OWN DESTRUCTION NOW>
	// GOSFX Change 12/15/99 -fs  Code Deleted

	return(OBJECT_DEAD);
}

//---------------------------------------------------------------------------

float GameObject::relFacingTo (glm::vec3 goal, long bodyLocation) {

	glm::vec3 facingVec = getRotationVector();

	glm::vec3 goalVec;
	goalVec = goal - position;

	float angle = angle_from(facingVec, goalVec);

	//--------------------------------
	// Get sign of relative angle.
	float z = (facingVec.x * goalVec.y) - (facingVec.y * goalVec.x);
	if (z > 0.0f)
		angle = -angle;
	
	return(angle);
}

//---------------------------------------------------------------------------

glm::vec3 GameObject::relativePosition (float angle, float distance, unsigned long flags) {

	//--------------------------------------------------------
	// Note that the angle should be -180 <= angle <= 180, and
	// the distance is in meters...

#ifdef USE_ROTATION
	Stuff::Vector2DOf<float> curPos;
	curPos.x = position.x;
	curPos.y = position.y;

	distance *= -worldUnitsPerMeter;
	Stuff::Vector2DOf<float> shiftVect;

	//--------------------------------------------
	// Absolute facing, based upon north facing...
	shiftVect.x = 0.0;
	shiftVect.y = 1.0;

	float tx = shiftVect.x;
	float sine = sin(angle);
	float cosine = cos(angle);

	shiftVect.x *= cosine;
	shiftVect.x += (shiftVect.y * sine);

	shiftVect.y *= cosine;
	shiftVect.y -= (tx * sine);

	//shiftVect.rotate(angle);
	shiftVect *= distance;

	Stuff::Vector2DOf<float> relPos;
	relPos.x = curPos.x + shiftVect.x;
	relPos.y = curPos.y + shiftVect.y;

	Stuff::Vector2DOf<float> start2d;
	Stuff::Vector2DOf<float> goal2d;
	Stuff::Vector2DOf<float> deltaVector;
	if (flags & RELPOS_FLAG_PASSABLE_START) {
		start2d  = curPos;
		goal2d = relPos;
		}
	else {
		start2d = relPos;
		goal2d = curPos;
	}

	deltaVector.x= goal2d.x - start2d.x;
	deltaVector.y = goal2d.y - start2d.y;

	//-------------------------------------------------------------
	// First, we need to calc the delta vector--how much we extend
	// the ray everytime we check the map cell for clear placement.
	deltaVector.normalize();
	float cellLength = Terrain::metersPerVertex / static_cast<float>(MAPCELL_DIM);
	cellLength *= 0.5;
	deltaVector *= cellLength;
	if (deltaVector.magnitude() == 0.0)
		return(curPos);

	//-------------------------------------------------
	// Determine the max length the ray must be cast...
	float maxLength = start2d.distance_from(goal2d);

	//------------------------------------------------------------
	// We'll start at the target, and if it's blocked, we'll move
	// toward our start location, looking for the first valid/open
	// cell...
	Stuff::Vector2DOf<float> curPoint = start2d;
	Stuff::Vector2DOf<float> curRay;
	curRay.zero();
	float rayLength = 0.0;

	long tileR, tileC, cellR, cellC;
	glm::vec3 curPoint3d;
	curPoint3d.init(curPoint.x, curPoint.y, 0.0);
	GameMap->worldToMapPos(curPoint3d, tileR, tileC, cellR, cellC);
	bool cellClear = GameMap->cellPassable(tileR, tileC, cellR, cellC);

	Stuff::Vector2DOf<float> lastGoodPoint = curPoint;
	if (flags & RELPOS_FLAG_PASSABLE_START)
		while (cellClear && (rayLength < maxLength)) {
			lastGoodPoint = curPoint;
			curPoint += deltaVector;
			curRay = curPoint - start2d;
			rayLength = curRay.magnitude();

			curPoint3d.init(curPoint.x, curPoint.y, 0.0);
			GameMap->worldToMapPos(curPoint3d, tileR, tileC, cellR, cellC);
			cellClear = GameMap->cellPassable(tileR, tileC, cellR, cellC);
		}
	else
		while (!cellClear && (rayLength < maxLength)) {
			lastGoodPoint = curPoint;
			curPoint += deltaVector;
			curRay = curPoint - start2d;
			rayLength = curRay.magnitude();

			curPoint3d.init(curPoint.x, curPoint.y, 0.0);
			GameMap->worldToMapPos(curPoint3d, tileR, tileC, cellR, cellC);
			cellClear = GameMap->cellPassable(tileR, tileC, cellR, cellC);
		}

	curPoint3d.init(lastGoodPoint.x, lastGoodPoint.y, 0.0);
	curPoint3d.z = GameMap->getTerrainElevation(curPoint3d);
	return(curPoint3d);
#else
	return(position);
#endif
}

//---------------------------------------------------------------------------

void GameObject::setPosition (const glm::vec3& newPosition, bool calcPositions) {
	if (ObjectManager) ObjectManager->invalidateSpatialGrid(!isMover());

	position = newPosition;

	if (calcPositions) {
		int newCellRow = 0;
		int newCellCol = 0;
		int tileRow = 0;
		int tileCol = 0;

		land->worldToTileCell(position, tileRow, tileCol, newCellRow, newCellCol);
		cellPositionRow = newCellRow + tileRow * MAPCELL_DIM;
		cellPositionCol = newCellCol + tileCol * MAPCELL_DIM;

		d_vertexNum = tileRow * Terrain::realVerticesMapSide + tileCol;
	}

	// GameMap can be NULL when a mission was saved without MOVE data and the
	// blank-MOVE synthesis in Mission::init also failed (oversized map).
	// Skip the bounds check rather than crash; position is still set above.
	//
	// When the map exceeds MAX_MAP_CELL_WIDTH the synthesized MOVE grid is
	// clamped smaller than the terrain cell range, so objects legitimately sit
	// beyond GameMap's extent. The "off map" assert is only meaningful when the
	// grid actually covers the terrain — otherwise it spams every frame.
	if (GameMap) {
		long terrainCells = (Terrain::realVerticesMapSide - 1) * MAPCELL_DIM;
		bool gridCoversTerrain = (GameMap->getHeight() >= terrainCells) &&
		                         (GameMap->getWidth()  >= terrainCells);
		if (gridCoversTerrain) {
			Assert((cellPositionRow >= 0) && (cellPositionRow < GameMap->getHeight()), 0, " Object moved off map ");
			Assert((cellPositionCol >= 0) && (cellPositionCol < GameMap->getWidth()), 0, " Object moved off map ");
		}
	}
}

//---------------------------------------------------------------------------

void GameObject::setExists(bool set) {
	if (getExists() != set) {
		if (mc2::world_trace::enabled())
			mc2::world_trace::event(set ? "spawn" : "remove", this);
		if (ObjectManager) ObjectManager->invalidateEntity(handle, this);
	}
	if (set) flags |= OBJECT_FLAG_EXISTS;
	else flags &= (OBJECT_FLAG_EXISTS ^ 0xFFFFFFFF);
}

float GameObject::distanceFrom (glm::vec3 goal) {

	glm::vec3 result;
	result.x = position.x - goal.x;
	result.y = position.y - goal.y;
	result.z = 0.0;
	return((glm::length(result) * metersPerWorldUnit));
}

//---------------------------------------------------------------------------

long GameObject::cellDistanceFrom (glm::vec3 goal) {

	int cellRow = 0;
	int cellCol = 0;
	land->worldToCell(goal, cellRow, cellCol);

	int rowDelta = 0;
	if (cellPositionRow > cellRow)
		rowDelta = cellPositionRow - cellRow;
	else
		rowDelta = cellRow - cellPositionRow;
	int colDelta = 0;
	if (cellPositionCol > cellCol)
		colDelta = cellPositionCol - cellCol;
	else
		colDelta = cellCol - cellPositionCol;
	return(rowDelta > colDelta ? rowDelta : colDelta);
}

//---------------------------------------------------------------------------

long GameObject::cellDistanceFrom (GameObjectPtr obj) {

	long rowDelta = 0;
	if (cellPositionRow > obj->cellPositionRow)
		rowDelta = cellPositionRow - obj->cellPositionRow;
	else
		rowDelta = obj->cellPositionRow - cellPositionRow;
	long colDelta = 0;
	if (cellPositionCol > obj->cellPositionCol)
		colDelta = cellPositionCol - obj->cellPositionCol;
	else
		colDelta = obj->cellPositionCol - cellPositionCol;
	return(rowDelta > colDelta ? rowDelta : colDelta);
}

//---------------------------------------------------------------------------

long GameObject::getLineOfSightNodes (long eyeCellRow, long eyeCellCol, long* cells) {

	cells[0] = cellPositionRow;
	cells[1] = cellPositionCol;
	return(1);
}

//---------------------------------------------------------------------------

bool GameObject::lineOfSight (glm::vec3 point) {

	glm::vec3 firingPosition = getLOSPosition();
	glm::vec3 targetPosition = point;
	
	bool LOSclear = false;
	if (land->IsGameSelectTerrainPosition(point))
		LOSclear = Team::lineOfSight(firingPosition, targetPosition, getTeamId(), 15.0);
	
	return(LOSclear);
}

//---------------------------------------------------------------------------

// B7o.2: was `inline` here. The virtual's only emitter was the vtable instance gameobj.cpp used to produce through the by-value
// GameObject::set; with that gone the inline body was never instantiated (LNK2001). A virtual defined in a .cpp is an ordinary external function.
float GameObject::lineOfSightRange() {
	//Figure out altitude above minimum terrain altitude and look up in table.
	float baseElevation = MapData::waterDepth;
	if (MapData::waterDepth < Terrain::userMin)
		baseElevation = Terrain::userMin;

	float altitude = position.z - baseElevation;
	float altitudeIntegerRange = (Terrain::userMax - baseElevation) * 0.00390625f;
	long altLevel = 0;
	if (altitudeIntegerRange > Stuff::SMALL)
		altLevel = altitude / altitudeIntegerRange;
	
	if (altLevel < 0)
		altLevel = 0;

	if (altLevel > 255)
		altLevel = 255;

	float radius = visualRangeTable[altLevel];
	
	//Scouting specialty skill.
	if (isMover())
	{
		MoverPtr mover = static_cast<MoverPtr>(this);
		if (mover->pilot && mover->pilot->isScout())
			radius += (radius * 0.2f);
			
		radius *= mover->getLOSFactor();
	}

	return radius * 25.0f * worldUnitsPerMeter;
}

bool GameObject::lineOfSight (GameObjectPtr target, float startExtRad)
{
	ZoneScopedN("GameLogic.LOS.Update");
	if (!target) return false;
	glm::vec3 distance = target->getPosition() - getPosition();
	float dist = glm::length(distance);
	if (dist > lineOfSightRange())
	{
		return false;
	}
		
	//--------------------------------------------------------------------------
	// For now, we hardcode the "height" of the firer and target (to 10 meters).
	// Easily changed when we add a height field to the object class...
	if (target->isMover()) 
	{
		if (isMover() && ObjectManager->useMoverLineOfSightTable) 
		{
			long index = (handle * ObjectManager->maxMovers) + target->handle;
			uint16_t& losCell = ObjectManager->moverLineOfSightTable[index];
			if (mc2::losEpochState(losCell, ObjectManager->moverLosEpoch) == 0)
			{
				bool los = Team::lineOfSight(SensorSystemManager::losPosition(this), SensorSystemManager::losPosition(target), getTeamId(), target->getAppearRadius(), startExtRad);
				if (los) 
				{
					losCell = mc2::losEpochCell(ObjectManager->moverLosEpoch, true);
					//Inverse is NOT always true!!!!
					// I can demonstrate a case!!
					// -fs
				   // ObjectManager->moverLineOfSightTable[(target->handle * ObjectManager->maxMovers) + handle] = 1;
				}
				else 
				{
					losCell = mc2::losEpochCell(ObjectManager->moverLosEpoch, false);
					// ObjectManager->moverLineOfSightTable[(target->handle * ObjectManager->maxMovers) + handle] = 0;
				}
			}
			
			if (mc2::losEpochState(losCell, ObjectManager->moverLosEpoch) == 2)
			{
				return true;
			}
			else
			{
				return false;
			}
		}
		
		if (Team::lineOfSight(getLOSPosition(), target->getLOSPosition(), getTeamId(), target->getAppearRadius(), startExtRad))
		{
			return(true);
		}
		else
		{
			return(false);
		}
	}

	//Try yanking these and just use the location the weapon fire is going to.
	// With the extent radius, like I originally did.
	// -fs
	//long lineOfSightNodes[20];
	//long numNodes = target->getLineOfSightNodes(cellPositionRow, cellPositionCol, lineOfSightNodes);
//	for (long i = 0; i < numNodes; i++)
//	{
//		if (land->IsGameSelectTerrainPosition(getLOSPosition()))
//		{
			float elev = TerrainRuntime::groundElevation(getLOSPosition());
			float localStart = getLOSPosition().z - elev;

			glm::vec3 targetPosition = target->getLOSPosition();
		
			if (Team::lineOfSight(getLOSPosition(), target->getLOSPosition(), getTeamId(), target->getAppearRadius(), startExtRad))
			{
				return(true);
			}
//		}
//	}

	return(false);
}

//---------------------------------------------------------------------------

void GameObject::destroy (void) {
	if (ObjectManager) ObjectManager->invalidateEntity(handle, this);

	//Never need to call this.  Heap destruct will get this!!
//	ObjectManager->removeObjectType(typeHandle);

}

//---------------------------------------------------------------------------

bool GameObject::onScreen (void) {

	//----------------------------------------------------------------------
	// This function is the meat and potatoes of the object cull system.
	// Its job is to determine if the object is on screen or not.
	// It does this by transforming the position for each active camera to
	// its screen coords and saving them.  It then checks each set of coords
	// to see if they are in the viewport of each camera.  Returned value
	// is number of windows that object can be seen in.
	long isVisible = 0;
	if (eye) {
		glm::vec3 objPosition = position;
		// [PROJECTZ:BoolAdmission id=gameobj_visibility_admit]
		PROJECTZ_SITE("gameobj_visibility_admit", "BoolAdmission");
		// [LOW-CAMERA-OBJECT-CULL-1 / FIX-3] sphere-aware admit pads the near
		// plane by the object's bounding radius (gate MC2_LOWCAM_OBJ_NEARPAD;
		// OFF = byte-identical to projectForObjectAdmission). getExtentRadius()
		// is the object-type bounding radius (world units; <=0 falls back).
		isVisible = eye->projectForObjectAdmissionSphere(objPosition, screenPos, getExtentRadius());
	}

	if (isVisible) {
		windowsVisible = turn;
		return(true);
	}
		
	return(false);
}

//---------------------------------------------------------------------------

float GameObject::getExtentRadius (void) {

	//---------------------------------------------------------------------
	// Can be overridden by explosions to return an instance based value.
	ObjectTypePtr objType = ObjectManager->getObjectType(typeHandle);
	if (objType)
		return(objType->getExtentRadius());
	return(-1.0);
}

//---------------------------------------------------------------------------

void GameObject::setExtentRadius (float newRadius) {

	//---------------------------------------------------------------------
	// Can be overridden by explosions to set an instance based value.
	ObjectTypePtr objType = ObjectManager->getObjectType(typeHandle);
	if (objType)
		objType->setExtentRadius(newRadius);
}

//---------------------------------------------------------------------------

MechClass GameObject::getMechClass(void) {

	if (getObjectClass() != BATTLEMECH)
		return(MECH_CLASS_NONE);
	if (tonnage < MechClassWeights[std::to_underlying(MECH_CLASS_LIGHT)])
		return(MECH_CLASS_LIGHT);
	if (tonnage < MechClassWeights[std::to_underlying(MECH_CLASS_MEDIUM)])
		return(MECH_CLASS_MEDIUM);
	if (tonnage < MechClassWeights[std::to_underlying(MECH_CLASS_HEAVY)])
		return(MECH_CLASS_HEAVY);
	return(MECH_CLASS_ASSAULT);
}

//---------------------------------------------------------------------------

long GameObject::getCaptureBlocker (GameObjectPtr capturingMover, GameObjectPtr* blockerList) {

	long numBlockers = 0;
	TeamPtr capturingTeam = capturingMover->getTeam();
	if (!capturingTeam)
		STOP(("GameObject.getCaptureBlocker: NULL capturingTeam"));

	if (distanceFrom(capturingMover->getPosition()) <= 30.0) {
		for (long i = 0; i < ObjectManager->getNumMovers(); i++) {
			MoverPtr mover = ObjectManager->getMover(i);
			if (capturingTeam->isEnemy(mover->getTeam()))
				if (!mover->isMarine() && (mover->numWeapons > 0))
					if ((distanceFrom(mover->getPosition()) < blockCaptureRange) && !mover->isDisabled() && mover->getAwake()) {
						if (blockerList)
							blockerList[numBlockers++] = mover;
						else
							return(1);
                    }
		}
		}
	else {
		for (long i = 0; i < ObjectManager->getNumMovers(); i++) {
			MoverPtr mover = ObjectManager->getMover(i);
			if (!mover->getTeam() || capturingTeam->isEnemy(mover->getTeam()))
				if (!mover->isMarine() && (mover->numWeapons > 0))
					if ((distanceFrom(mover->getPosition()) < blockCaptureRange) && !mover->isDisabled() && mover->getAwake())
						if (capturingMover->getTeam()->isContact(capturingMover, mover, CONTACT_CRITERIA_VISUAL_OR_SENSOR + CONTACT_CRITERIA_ENEMY + CONTACT_CRITERIA_NOT_DISABLED)){
							if (blockerList)
								blockerList[numBlockers++] = mover;
							else
								return(1);
                        }
		}			
	}

	return(numBlockers);
}

//---------------------------------------------------------------------------

bool GameObject::isFriendly (GameObjectPtr obj) {

	TeamPtr myTeam = getTeam();
	TeamPtr objTeam = obj->getTeam();
	if (myTeam && objTeam)
		return(myTeam->isFriendly(objTeam));
	return(false);
}

//---------------------------------------------------------------------------

bool GameObject::isEnemy (GameObjectPtr obj) {
			
	TeamPtr myTeam = getTeam();
	TeamPtr objTeam = obj->getTeam();
	if (myTeam && objTeam)
		return(myTeam->isEnemy(objTeam));
	return(false);
}

//---------------------------------------------------------------------------

bool GameObject::isNeutral (GameObjectPtr obj) {
			
	TeamPtr myTeam = getTeam();
	TeamPtr objTeam = obj->getTeam();
	if (myTeam && objTeam)
		return(myTeam->isNeutral(objTeam));
	return(false);
}

//---------------------------------------------------------------------------
void GameObject::CopyTo (GameObjectData *data)
{
	if (getObjectType())
		data->objectTypeNum = getObjectType()->getObjTypeNum();
	else
		data->objectTypeNum = 0;

	data->objectClass = objectClass;
	data->handle = handle;
	data->partId = partId;
	data->watchID = watchID;

	data->typeHandle = typeHandle;
	data->position = position;
	data->cellPositionRow = cellPositionRow;
	data->cellPositionCol = cellPositionCol;
	data->d_vertexNum = d_vertexNum;
	// Cursor targeting belongs to presentation, not the saved object state.
	// Its timing can differ while a simulation job is in flight.
	data->flags = flags & ~OBJECT_FLAG_TARGETED;
	data->debugFlags = debugFlags;
	data->status = status;

	data->tonnage = tonnage;
	data->rotation = rotation;
	if (getObjectType() && getObjectType()->getAppearanceTypeName())
	{
		if (strlen(getObjectType()->getAppearanceTypeName()) <= 255)
			strcpy(data->appearanceTypeID,getObjectType()->getAppearanceTypeName());
		else
			STOP(("Object Appearance name too long for Save.  %s",getObjectType()->getAppearanceTypeName()));
	}
	else
	{
		strcpy(data->appearanceTypeID,"NONE");
	}

	data->collisionFreeFromWID = collisionFreeFromWID;
	data->collisionFreeTime = collisionFreeTime;
	data->screenPos = screenPos;
	// windowsVisible is the turn the camera last saw the object (stamped by the render pass or a camera bounds test) and
	// Load resets it, so the save carries zero: a saved stamp made the save depend on the camera and on how many sim steps
	// ran since the last drawn frame.
	data->windowsVisible = 0;
	data->explRadius = explRadius;
	data->explDamage = explDamage;
	data->maxCV = maxCV;
	data->curCV = curCV;
	data->threatRating = threatRating;
	data->lastFrameTime = lastFrameTime;
	data->blipFrame = blipFrame;
	data->numAttackers = numAttackers;

	// Rebuild the stable selection overlay; hover bars/text/colour are transient.
	data->drawFlags = (data->flags & OBJECT_FLAG_SELECTED) ? (DRAW_BARS | DRAW_BRACKETS) : DRAW_NORMAL;
}

//---------------------------------------------------------------------------
void GameObject::Save (PacketFilePtr file, long packetNum)
{
	STOP(("Should never save a gameObj!!"));
}

//---------------------------------------------------------------------------
void GameObject::Load (GameObjectData *data)
{
	objectClass = data->objectClass;
	handle = data->handle;
	partId = data->partId;
	if (ObjectManager)
		ObjectManager->notePartId(this, partId);
	watchID = data->watchID;

	position = data->position;
	cellPositionRow = data->cellPositionRow;
	cellPositionCol = data->cellPositionCol;
	d_vertexNum = data->d_vertexNum;
	flags = data->flags;
	debugFlags = data->debugFlags;
	status = data->status;

	tonnage = data->tonnage;
	rotation = data->rotation;

	collisionFreeFromWID = data->collisionFreeFromWID;
	collisionFreeTime = data->collisionFreeTime;
	screenPos = data->screenPos;
	windowsVisible = 0;		//Force back to zero so I don't have to save the turn.
	explRadius = data->explRadius;
	explDamage = data->explDamage;
	maxCV = data->maxCV;
	curCV = data->curCV;
	threatRating = data->threatRating;
	lastFrameTime = data->lastFrameTime;
	blipFrame = data->blipFrame;
	numAttackers = data->numAttackers;

	drawFlags = data->drawFlags;
}

//***************************************************************************




