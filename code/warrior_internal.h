// Shared surface of the MechWarrior translation units (warrior*.cpp).
//
// warrior.cpp keeps the data tables, init/clear, the brain-gate statics, runBrain and the decision trees that read
// the brain gates; the rest of the MechWarrior implementation is split by responsibility.
#ifndef WARRIOR_INTERNAL_H
#define WARRIOR_INTERNAL_H


#ifndef MCLIB_H
#include"mclib.h"
#endif
#include "brain_special_dispatch.h"  // TECHSCRIPT-SPECIAL-DISPATCH-1A: executeSpecialBody_TraceOnly
                                     // BRAIN-WORLD-SNAPSHOT-1: s_brainSnapshotEnabled, buildBrainWorldSnapshot

#ifndef WARRIOR_H
#include"warrior.h"
#endif

#ifndef GAMEOBJ_H
#include"gameobj.h"
#endif

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef GROUP_H
#include"group.h"
#endif

#ifndef GAMELOG_H
#include"gamelog.h"
#endif

#ifndef TEAM_H
#include"team.h"
#endif

#ifndef COMNDR_H
#include"comndr.h"
#endif

#ifndef MOVER_H
#include"mover.h"
#endif

#ifndef MOVEMGR_H
#include"movemgr.h"
#endif

#ifndef GOS_PROFILER_H
#include"gos_profiler.h"
#endif

#ifdef USE_MECHS
#ifndef MECH_H
#include"mech.h"
#endif
#endif

#ifndef GVEHICL_H
#include"gvehicl.h"
#endif

#ifndef BLDNG_H
#include"bldng.h"
#endif

#ifndef GATE_H
#include"gate.h"
#endif

#ifndef TURRET_H
#include"turret.h"
#endif

#ifndef TACORDR_H
#include"tacordr.h"
#endif

#ifndef RADIO_H
#include"radio.h"
#endif

#ifndef MULTPLYR_H
#include"multplyr.h"
#endif

#ifndef GOAL_H
#include"goal.h"
#endif

#ifndef TURRET_H
#include"turret.h"
#endif

#ifndef BLDNG_H
#include"bldng.h"
#endif

inline constexpr int TESTING_WITH_PLAYER = 1;

#include"platform_windows.h"
#include "../GameOS/gameos/gos_static_prop_registry.h"  // Task 6: late-spawn registerStaticProp
#include "../GameAdapters/StaticPropRenderAdapter.h"  // M1 Task 12
#include "move_recon.h"  // MC2_MOVE_RECON per-frame pathfinding cost instrumentation
#include "brain_tactic_select.h"  // TACTIC-WEIGHTS-A: Wang-hash deterministic tactic selection
#include"terrain_runtime.h"  // TERRAIN-RUNTIME-CONSUMER-GROUNDING-1
#include "abl_trace.h"  // ABL-VM-FACTS-1: per-frame ABL VM statement/depth/us telemetry
#include "../GameOS/gameos/diagnostic_trace.h"  // ISSUE-17: MC2_FIRE_DECISION_TRACE nofire JSONL events

enum {
	T_A = 0,
	T = T_A,
	T_B,
	U,
	V,
};

inline constexpr int MAXLEN_INPUTSTR = 512;
inline constexpr float CELL_DISTANCE = 42.6667f;

enum MechBodyLocationType {
	MECH_BODY_LOCATION_ANY = -1,
	MECH_BODY_LOCATION_HEAD,
	MECH_BODY_LOCATION_CTORSO,
	MECH_BODY_LOCATION_LTORSO,
	MECH_BODY_LOCATION_RTORSO,
	MECH_BODY_LOCATION_LARM,
	MECH_BODY_LOCATION_RARM,
	MECH_BODY_LOCATION_LLEG,
	MECH_BODY_LOCATION_RLEG,
	NUM_MECH_BODY_LOCATIONS
};

// Globals defined in warrior.cpp (or elsewhere in the engine) that the split pieces share.
extern int32_t NumStateTransitions;
extern uint32_t g_mc2FrameCounter;
extern float scenarioTime;
extern float ThreatRatingEffect[8][3];
extern float BrainUpdateFrequency;
extern float MovementUpdateFrequency;
extern float CombatUpdateFrequency;
extern float CommandUpdateFrequency;
extern float PilotCheckUpdateFrequency;
extern float ThreatAnalysisFrequency;
extern long PilotCheckModifierTable[2];
extern float SkillWeightings[NUM_SKILLS];
extern float WarriorRankScale[NUM_WARRIOR_RANKS];
extern long LastMoveCalcErr;
extern long GroupMoveTrailLen[2];
extern float MoveTimeOut;
extern float MoveYieldTime;
extern float DefaultAttackRadius;
extern QueuedTacOrder TacOrderQueue[MAX_QUEUED_TACORDERS];
extern long TacOrderQueuePos;
extern long	SimpleMovePathRange;
extern long AttitudeEffectOnMovePath[NUM_ATTITUDES][3];
extern char AttitudeEffect[NUM_ATTITUDES][6];
extern MoverGroupPtr CurGroup;
extern GameObjectPtr CurObject;
extern long CurObjectClass;
extern MechWarriorPtr CurWarrior;
extern GameObjectPtr CurContact;
extern long CurAlarm;
extern float MapCellDiagonal;
extern float WeaponRanges[NUM_WEAPON_RANGE_TYPES][2];
extern DebuggerPtr debugger;
extern float MechClassWeights[NUM_MECH_CLASSES];
extern char OverlayIsBridge[NUM_OVERLAY_TYPES];
extern float SkillTry[4];
extern float SkillSuccess[4];
extern float KillSkill[6];
extern float WeaponHit;
extern float SensorSkill;
extern long RamObjectWID;
extern UserHeapPtr missionHeap;
extern GameObjectPtr LastCoreAttackTarget;
extern const char* pilotAlarmFunctionName[NUM_PILOT_ALARMS];
void SeedRandomNumbers (void);
long SignedRandomNumber (long range);
long GetMissionTurn (void);

// Free functions defined in the split pieces.
long compareTacOrderId (long id1, long id2);
long orderOrbitObject (GameObjectPtr target);
long orderUseOrbitRange (long type, float range);
void drawOverviewArrowWorld( const Stuff::Vector3D& from, const Stuff::Vector3D& to, unsigned long color );

#endif // WARRIOR_INTERNAL_H
