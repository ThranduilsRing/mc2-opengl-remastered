// Shared surface of the BattleMech translation units (mech*.cpp).
//
// mech.cpp keeps the data tables and the heat/status/speed/Save/Load methods; the rest of the BattleMech
// implementation is split by responsibility (type, init, loadout, move, move_net, update, damage, weapons).
#ifndef MECH_INTERNAL_H
#define MECH_INTERNAL_H


#ifndef MCLIB_H
#include"mclib.h"
#endif

#include"terrain_runtime.h"

#ifndef GAMEOBJ_H
#include"gameobj.h"
#endif

#ifndef MECH_H
#include"mech.h"
#endif

#ifndef GVEHICL_H
#include"gvehicl.h"
#endif

#ifndef MISSION_H
#include"mission.h"
#endif

#ifndef CMPONENT_H
#include"cmponent.h"
#endif

#ifndef WARRIOR_H
#include"warrior.h"
#endif

#ifndef WEAPONBOLT_H
#include"weaponbolt.h"
#endif

#ifdef USE_DEBRIS
#ifndef DEBRIS_H
#include"debris.h"
#endif
#endif

#ifdef USE_JETS
#ifndef JET_H
#include<jet.h>
#endif
#endif

#ifndef TACORDR_H
#include"tacordr.h"
#endif

#ifndef GAMESOUND_H
#include"gamesound.h"
#endif

#ifndef SOUNDS_H
#include"sounds.h"
#endif

#ifndef COLLSN_H
#include"collsn.h"
#endif

#ifndef WEAPONFX_H
#include"weaponfx.h"
#endif

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef MULTPLYR_H
#include"multplyr.h"
#endif

#ifdef OBJTYPE_H
#include"objtype.h"
#endif

#ifndef TEAM_H
#include"team.h"
#endif

#ifndef MECH3D_H
#include"mech3d.h"
#endif

#ifndef GROUP_H
#include"group.h"
#endif

#ifndef UNITDESG_H
#include"unitdesg.h"
#endif

// ONE-WEAPON-PRESENTATION-PROOF-1 (E8): resolveTargetWeaponFireEffectType seam.
#include"unitprofile.h"

#ifdef USE_ELEMENTALS
#ifndef ELEMNTL_H
#include<elemntl.h>
#endif
#endif

#ifndef CONTACT_H
#include"contact.h"
#endif

#ifndef TURRET_h
#include"turret.h"
#endif

#ifndef GAMEOLOG_H
#include"gamelog.h"
#endif

#ifndef COMNDR_H
#include"comndr.h"
#include"tacticaloverview.h"  // maskWorldBars: hide health bars in overview
#endif

#ifndef LOGISTICSDATA_H
#include"logisticsdata.h"
#endif

#include "../resource.h"
#include "../GameOS/gameos/gpu_cull_readback.h"  // C3: GPU visibility queries
#include "../GameAdapters/MechRenderAdapter.h"  // M2: mech spawn/destroy adapter
#include "move_recon.h"  // MC2_MOVE_RECON per-frame pathfinding cost instrumentation
#include "../GameOS/gameos/diagnostic_trace.h"  // COMBAT-TRACE-1: weapon-hit JSONL events
#include "equipment_registry.h"  // LOADOUT-EQUIPMENT-ID-ALIAS-1 (E5): resolveLoadoutEquipmentRef
#include "modder_finding.h"      // MODDER-FIRST-ERROR-SURFACE-1: shared context format
#include "../mclib/vis_semantics.h"  // FORCE-ALL-VISIBLE-1: mc2_vis::forceVisibleOverride

// Globals defined in mech.cpp (or elsewhere in the engine) that the split pieces share.
extern float				MaxVisualRadius;
extern unsigned long		NextIdNumber;
extern float				MetersPerCell;
extern long					AttitudeEffectOnMovePath[NUM_ATTITUDES][3];
extern bool					useSound;
extern bool					friendlyDestroyed;
extern bool					enemyDestroyed;
extern TeamPtr				homeTeam;
extern long					MechSalvageChance;
extern long					woundFatigue[5];
extern float				FireOddsTable[NUM_FIREODDS];
extern float				MovementUpdateFrequency;
extern float				CombatUpdateFrequency;
extern float				CommandUpdateFrequency;
extern float				ContactUpdateFrequency;
extern float				OrderRequestFrequency;
extern float 				elmDamageOnImpact;
extern bool 				drawTerrainGrid;
extern unsigned char		footPrints;
extern float				MaxTimeRevealed;
extern GameLog* CombatLog;
extern float MineDamage;
extern float MineSplashDamage;
extern float MineSplashRange;
extern long MineExplosion;
extern float SkillTry[4];
extern float SkillSuccess[4];
extern void addMoverToList (long blockNum);
extern char mechSpeedStateArray[28];
extern long TargetRolo;
extern float MapCellDiagonal;
extern GlobalMapPtr GlobalMoveMap[3];
extern bool CantTouchThis;
extern WeaponFireChunk CurMoverWeaponFireChunk;
extern void DebugWeaponFireChunk (WeaponFireChunkPtr chunk1, WeaponFireChunkPtr chunk2, GameObjectPtr attacker);
extern long StatusChunkUnpackErr;
extern float worldUnitsPerMeter;
extern float ContactFadeTime;
extern bool invulnerableON;
extern long NumLocationCriticalSpaces[NUM_BODY_LOCATIONS];
extern long MechHitSectionTable[NUM_ATTACKSOURCES];
extern GameObjectPtr mech2;
extern float tonnageTurnRate[110];
extern long stupidNewSageColumns[8];
extern float PilotingCheckFactor;
extern long hitLevel[2];
extern long PilotCheckModifierTable[2];
extern long ClusterSizeSRM;
extern long ClusterSizeLRM;
extern long AntiMissileSystemStats[2][2];
extern float PilotCheckHalfRate;
extern float WeaponRange[NUM_FIRERANGES];
extern float WeaponRanges[NUM_WEAPON_RANGE_TYPES][2];
extern float DisableAttackModifier;
extern float DisableGunneryModifier;
extern float SalvageAttackModifier;
extern long adjClippedCell[8][2];
extern float MechClassScale[NUM_MECH_CLASSES];
extern float RankVersusChassisCombatModifier[NUM_WARRIOR_RANKS][NUM_MECH_CLASSES];
extern float WeaponFireJumpTime;
extern float WeaponFireModifiers[NUM_WEAPONFIRE_MODIFIERS];
extern long AttackerMoveModifier[NUM_MECH_STATES];
extern char CriticalHitTable[4];
extern long TargetMoveModifierTable [5][2];
extern float MechClassWeights[NUM_MECH_CLASSES];
extern char MechTransferHitTable[NUM_MECH_BODY_LOCATIONS];
extern char MechArmorToBodyLocation[NUM_MECH_ARMOR_LOCATIONS];
extern long MechPilotCheckConditions[2];
extern long MechPilotCheckTerrainEffect[64];
extern long AimedFireHitTable[3];
extern float mechCollisionThreshold;
extern float objectCollisionThreshold;
extern float tonnageCollisionThreshold;
extern float treeDeflection;
extern float mechPivotAngle;
extern float mechPivotThrottle;
extern float MaxVelocityMag;
extern float NewRotation;
extern long DefaultMechCrashAvoidSelf;
extern long DefaultMechCrashAvoidPath;
extern long DefaultMechCrashBlockSelf;
extern long DefaultMechCrashBlockPath;
extern float DefaultMechCrashYieldTime;
extern long DefaultMechJumpCost;
extern GameObjectPtr BadGuy;
extern DebuggerPtr debugger;
extern void LogWeaponFireChunk (WeaponFireChunkPtr chunk, GameObjectPtr attacker, GameObjectPtr target);
extern char MechHitLocationTable[NUM_MECH_HIT_SECTIONS * NUM_MECH_HIT_ARCS * NUM_MECH_ARMOR_LOCATIONS];
extern bool ShowMovers;
extern float DistanceToWaypoint;

// MC2_GPU_CULL_LIFECYCLE=1 routes AI canBeSeen() combat gates to GPU-lagged visibility (same env var as objmgr.cpp); read once.
inline bool mech_gpuCullLifecycle() {
	static const bool v = (getenv("MC2_GPU_CULL_LIFECYCLE") != nullptr);
	return v;
}

#endif // MECH_INTERNAL_H
