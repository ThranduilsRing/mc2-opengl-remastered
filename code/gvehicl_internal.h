// Shared surface of the GroundVehicle translation units (gvehicl*.cpp).
//
// gvehicl.cpp keeps the vehicle tables, GroundVehicleType, init/destroy and mineCheck; the rest of the GroundVehicle
// implementation is split by responsibility.
#ifndef GVEHICL_INTERNAL_H
#define GVEHICL_INTERNAL_H

#ifndef MCLIB_H
#include"mclib.h"
#endif

#ifndef GAMEOBJ_H
#include"gameobj.h"
#endif

#ifndef MOVER_H
#include"mover.h"
#endif

#ifndef COLLSN_H
#include"collsn.h"
#endif

#ifndef GVEHICL_H
#include"gvehicl.h"
#endif

#ifndef MECH_H
#include"mech.h"
#endif

#ifndef MOVE_H
#include"move.h"
#endif

#ifdef MISSION_H
#include"mission.h"
#endif

#ifndef CMPONENT_H
#include"cmponent.h"
#endif

#ifndef WARRIOR_H
#include"warrior.h"
#endif

#ifndef WEAPONFX_H
#include"weaponfx.h"
#endif

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef WEAPONBOLT_H
#include"weaponbolt.h"
#endif

#ifndef SOUNDS_H
#include"sounds.h"
#endif


#ifndef MULTPLYR_H
#include"multplyr.h"
#endif

#ifndef GROUP_H
#include"group.h"
#endif

#ifndef UNITDESG_H
#include"unitdesg.h"
#endif

#ifndef GAMESOUND_H
#include"gamesound.h"
#endif

#ifndef TEAM_H
#include"team.h"
#endif

#ifndef CONTACT_H
#include"contact.h"
#endif

#ifndef MISSION_H
#include"mission.h"
#endif

#ifndef GAMELOG_H
#include"gamelog.h"
#endif

#ifndef ARTLRY_H
#include"artlry.h"
#endif

#ifndef LOGISTICSDATA_H
#include"logisticsdata.h"
#endif

#ifndef COMNDR_H
#include"comndr.h"
#include"tacticaloverview.h"  // maskWorldBars: hide health bars in overview
#endif

#include"terrain_runtime.h"
#include "../resource.h"
#include "../GameOS/gameos/gpu_cull_readback.h"  // C3: GPU visibility queries
#include "../GameOS/gameos/gos_profiler.h"  // Tracy sub-zones for vehicles update
#include "../GameOS/gameos/diagnostic_trace.h"  // COMBAT-TRACE-1: weapon-hit JSONL events
#include "../mclib/vis_semantics.h"  // FORCE-ALL-VISIBLE-1: mc2_vis::forceVisibleOverride

#define MAX_GVACTOR_STATES		3

// Globals defined in gvehicl.cpp (or elsewhere in the engine) that the split pieces share.
extern unsigned long		NextIdNumber;
extern float worldUnitsPerMeter;
extern long StatusChunkUnpackErr;
extern TeamPtr				homeTeam;
extern bool					friendlyDestroyed;
extern bool					enemyDestroyed;
extern bool					useSound;
extern bool					useOldProject;
extern bool drawExtents;
extern bool drawTerrainGrid;
extern long adjClippedCell[8][2];
extern void addMoverToList(long blockNum);
extern float MaxVelocityMag;
extern float MineDamage;
extern float MineSplashDamage;
extern float MineSplashRange;
extern long MineExplosion;
extern long MineSweepThrottle;
extern long MineLayThrottle;
extern float MapCellDiagonal;
extern float				MaxTimeRevealed;
extern bool CantTouchThis;
extern bool ShowMovers;
extern WeaponFireChunk CurMoverWeaponFireChunk;
extern void DebugWeaponFireChunk (WeaponFireChunkPtr chunk1, WeaponFireChunkPtr chunk2, GameObjectPtr attacker);
extern void LogWeaponFireChunk (WeaponFireChunkPtr chunk, GameObjectPtr attacker, GameObjectPtr target);
extern long DefaultPilotId;
extern char marineProfileName[80];
extern float vehicleTurnRate[110];
extern long TargetMoveModifierTable[5][2];
extern float WeaponRange[NUM_FIRERANGES];
extern float WeaponRanges[NUM_WEAPON_RANGE_TYPES][2];
extern long GroundVehicleAttackerMoveModifier[MAX_GVACTOR_STATES];
extern long GroundVehicleCriticalHitTable[NUM_GROUNDVEHICLE_CRITICAL_HIT_TYPES];
extern long hitLevel[2];
extern GameLog* CombatLog;
extern long ClusterSizeSRM;
extern long ClusterSizeLRM;
extern float PilotCheckHalfRate;
extern float WeaponFireModifiers[NUM_WEAPONFIRE_MODIFIERS];
extern long AimedFireAbort;
extern long AimedFireHitTable[3];
extern float ContactFadeTime;
extern float gvCollisionThreshold;
extern float gvObjectCollisionThreshold;
extern float gvTonnageCollisionThreshold;
extern float gvTreeDeflection;
extern float gvSweepTime;
extern float gvWalkSpeed;
extern float gvHillSpeedFactor;
extern float InclineThrottleMultiplier[NUM_GROUNDVEHICLE_CHASSIS][2];
extern long DefaultGroundVehicleCrashAvoidSelf;
extern long DefaultGroundVehicleCrashAvoidPath;
extern long DefaultGroundVehicleCrashBlockSelf;
extern long DefaultGroundVehicleCrashBlockPath;
extern float DefaultGroundVehicleCrashYieldTime;
extern long NumMarines;
extern float NewRotation;
extern long TargetRolo;
extern float MineWaitTime;
extern float StrikeWaitTime;
extern float StrikeTimeToImpact;
extern float DistanceToWaypoint;

#endif // GVEHICL_INTERNAL_H
