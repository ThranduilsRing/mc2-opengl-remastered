// Shared surface of the Mover translation units (mover*.cpp).
//
// mover.cpp keeps the tuning globals and tables below plus the small Mover accessors; the rest of the
// Mover implementation is split by responsibility (chunks, diag, init, combat, path, attack, misc).
#ifndef MOVER_INTERNAL_H
#define MOVER_INTERNAL_H


#ifndef MCLIB_h
#include"mclib.h"
#endif
#include"terrain_runtime.h"   // TERRAIN-RUNTIME-CONSUMER-WATER-1

#ifndef GAMEOBJ_H
#include"gameobj.h"
#endif

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef MOVER_H
#include"mover.h"
#endif

#ifndef MOVE_H
#include"move.h"
#endif

#ifndef MECH_H
#include"mech.h"
#endif

#ifndef GVEHICL_H
#include"gvehicl.h"
#endif

#ifndef MECH3D_H
#include"mech3d.h"
#endif

#ifndef CMPONENT_H
#include"cmponent.h"
#endif

#ifndef WARRIOR_H
#include"warrior.h"
#endif

#ifndef GOS_PROFILER_H
#include"gos_profiler.h"
#endif

#ifdef USE_DEBRIS
#ifndef DEBRIS_H
#include"debris.h"
#endif
#endif

#ifndef GAMELOG_H
#include"gamelog.h"
#endif

#ifndef TACORDR_H
#include"tacordr.h"
#endif

#ifndef GAMESOUND_H
#include"gamesound.h"
#endif

//#ifndef SOUNDSYS_H
#include"soundsys.h"
//#endif

//#ifndef SOUNDS_H
#include"sounds.h"
//#endif

#ifndef COLLSN_H
#include"collsn.h"
#endif

#ifndef MECHCLASS_H
#include"mechclass.h"
#endif

#ifndef UNITDESG_H
#include"unitdesg.h"
#endif

#ifndef MULTPLYR_H
#include"multplyr.h"
#endif

#ifndef TEAM_H
#include"team.h"
#endif

#ifndef COMNDR_H
#include"comndr.h"
#endif

#ifndef GROUP_H
#include"group.h"
#endif

#ifndef CONTACT_H
#include"contact.h"
#endif

#ifndef TRIGGER_H
#include"trigger.h"
#endif

#ifndef MISSION_H
#include"mission.h"
#endif

#ifndef LOGISTICSPILOT_H
#include"logisticspilot.h"
#endif

#include "move_recon.h"  // MC2_MOVE_RECON per-frame pathfinding cost instrumentation
#include "../GameOS/gameos/diagnostic_trace.h"  // TARGETING-GUARD-1 chokepoint trace
#include <cstdlib>
#include "unitprofile.h"             // LOADOUT-HARDPOINT-JOIN-1: mc2UnitProfileDataEnabled()
#include "unit_profile_validator.h"  // LOADOUT-HARDPOINT-JOIN-1: validateLoadoutHardpointJoins()
#include "hardpoint.h"               // LOADOUT-HARDPOINT-JOIN-1: HardpointProfile / WeaponTag
#include <cmponent.h>                // LOADOUT-HARDPOINT-JOIN-1: ComponentFormType (weapon form)

inline constexpr int GOALMAP_CELL_DIM = 61;

// Globals defined in mover.cpp (or elsewhere in the engine) that the split pieces share.
extern long					SimpleMovePathRange;
extern long					GroupMoveTrailLen[2];
extern long		tileMulMAPCELL_DIM[MAX_MAP_CELL_WIDTH];
extern long					RamObjectWID;
extern long MaxMoveGoalChecks;
extern float DelayedOrderTime;
extern float GroupOrderGoalOffset;
extern MoveMapPtr			PathFindMap[2/*NUM_PATHMAPS*/];
extern long goalMap[GOALMAP_CELL_DIM * GOALMAP_CELL_DIM];
extern long goalMapRowStart[GOALMAP_CELL_DIM];
extern char goalMapRowCol[GOALMAP_CELL_DIM * GOALMAP_CELL_DIM][2];
extern float				DefaultAttackRadius;
extern float WeaponFireModifiers[NUM_WEAPONFIRE_MODIFIERS];
extern void DebugWeaponFireChunk (WeaponFireChunkPtr chunk1, WeaponFireChunkPtr chunk2, GameObjectPtr attacker);
extern unsigned long		NextIdNumber;
extern bool					JumpOnBlocked;
extern bool					FindingEscapePath;
extern float				RankVersusChassisCombatModifier[NUM_WARRIOR_RANKS][NUM_MECH_CLASSES];
extern float				WarriorRankScale[NUM_WARRIOR_RANKS];
extern const char *			SpecialtySkillsTable[NUM_SPECIALTY_SKILLS];
extern float WeaponSpecialistModifier;
extern float				MovementUpdateFrequency;
extern float				CombatUpdateFrequency;
extern float				CommandUpdateFrequency;
extern float				PilotCheckUpdateFrequency;
extern long					PilotCheckModifierTable[2];
extern long					MechSalvageChance;
extern float				MapCellDiagonal;
extern long NumLocationCriticalSpaces[NUM_BODY_LOCATIONS];
extern GlobalMapPtr			GlobalMoveMap[3];
extern long adjCellTable[MAPCELL_DIM * MAPCELL_DIM][8][4];
extern bool useUnlimitedAmmo;
extern bool CantBlowSalvage;
extern long TargetRolo;
extern WeaponFireChunk CurMoverWeaponFireChunk;
extern char* ExceptionGameMsg;
extern char ChunkDebugMsg[5120];
extern char OverlayIsBridge[NUM_OVERLAY_TYPES];
extern float GroundVehiclePivotYawMultiplier;
extern bool friendlyDestroyed;
extern bool enemyDestroyed;
extern GameLog* CombatLog;
extern long StatusChunkUnpackErr;
extern char AttackParameters[14][3];
extern long rearAttackTable[NUM_DIRECTIONS][3][2];
extern float WeaponRange[NUM_FIRERANGES];
extern float WeaponRanges[NUM_WEAPON_RANGE_TYPES][2];
extern float OptimalRangePoints[NUM_WEAPON_RANGE_TYPES];
extern bool OptimalRangePointInRange[NUM_WEAPON_RANGE_TYPES][3];
extern unsigned char OptimalRangeTieTable[32];
extern float DefaultAttackRange;
extern float DisableAttackModifier;
extern float DisableGunneryModifier;
extern float SalvageAttackModifier;
extern float PilotingCheckFactor;
extern long hitLevel[2];
extern long ClusterSizeSRM;
extern long ClusterSizeLRM;
extern long AntiMissileSystemStats[2][2];
extern float PilotCheckHalfRate;
extern float DamageRateFrequency;
extern char AttitudeEffect[NUM_ATTITUDES][6];
extern float SensorBaseChance;
extern float SensorSkillFactor;
extern long DebugMovePathType;
extern float SensorShutDownMechModifier;
extern float SensorRangeModifier[4][2];
extern float SensorSizeModifier[3][2];
extern float SensorBlockingTerrain[2];
extern long AimedFireAbort;
extern long AimedFireHitTable[3];
extern float FireArc[3];
extern const char ProfessionalismOffsetTable[NUM_OFFSET_RANGES][2];
extern const char DecorumOffsetTable[NUM_OFFSET_RANGES][2];
extern const char AmmoConservationModifiers[2][2];
extern float MoveTimeOut;
extern float MoveYieldTime;
extern long DefaultMechJumpCost;
extern char SensorSkillMoveRange[4];
extern float SensorSkillMoveFactor[8];
extern float SkillTry[4];
extern float SkillSuccess[4];
extern float KillSkill[6];
extern float WeaponHit;
extern float SensorSkill;
extern float MaxStationaryTime;
extern float MaxTimeRevealed;
extern float worldUnitsPerMeter;
extern float HeadShotModifier;
extern float ArmShotModifier;
extern float LegShotModifier;
extern bool CalcValidAreaTable;
extern void DEBUGWINS_print (char* s, long window);
extern float SensorBlockingObjectModifier;
extern GameObjectPtr DebugGameObject[3];

// Free functions defined in the split pieces.
void DebugMoveChunk (MoverPtr mover, MoveChunkPtr chunk1, MoveChunkPtr chunk2);
void DebugStatusChunk (MoverPtr mover, StatusChunkPtr chunk1, StatusChunkPtr chunk2);
long calcBestGoalFromTarget (int targetPos[2], int maxPos[2], float minRange, float bestRange, int* bestCell);
void GetBlockedDoorCells (int moveLevel, int door, char* openCells);
void PlaceMovers (void);
void PlaceStationaryMovers (MoveMap* map);

#endif // MOVER_INTERNAL_H
