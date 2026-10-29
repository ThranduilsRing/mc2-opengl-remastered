#include "Gates/preferences_legacy.h"
// Shared surface of the mission.cpp translation units (mission.cpp, mission_{update,movers,init}.cpp).
//
// mission.cpp keeps the mission globals, render, difficulty setup, start/destroy and the objective timers; the rest of
// Mission is split by responsibility (see DOC in each piece).
#ifndef MISSION_INTERNAL_H
#define MISSION_INTERNAL_H

// Implemented by missiongui_update.cpp; called by the mission update fixture path.
void updateSaveFixtures();

#ifndef MCLIB_H
#include"mclib.h"
#include <atomic>
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include "gos_crashbundle.h"
#include "gos_smoke.h"            // S9D fixed-timestep deterministic smoke clock
#include "gos_postprocess.h"   // VK-GL-LOST-DRAWS tags
#include "cpu_proj_cost_split.h"  // F3 CPU projection cost-baseline (hard_reset)
#include "sim_identity.h"         // SIM-IDENTITY-HASH-1 per-sim-frame world hash
#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <utility>

namespace {

template <class... Args>
void CB_PRINTF(std::format_string<Args...> fmt, Args&&... args)
{
    crashbundle_printf<256>("", fmt, std::forward<Args>(args)...);
}

}
#endif

#ifndef MISSION_H
#include"mission.h"
#endif

#include"move.h"

#ifndef MOVEMGR_H
#include"movemgr.h"
#endif

#ifndef GAMESOUND_H
#include"gamesound.h"
#endif

#ifndef SOUNDS_H
#include"sounds.h"
#endif
#include "brain_special_dispatch.h"  // TECHSCRIPT-SPECIAL-DISPATCH-1A: parseBrainSpecialBody
#include "brain_missionfit_oporbd.h"  // BRAIN-MISSIONFIT-OPORD-CONSUMER-1: declarative mission.fit OPORD parser
#include "brain_archetype.h"  // BRAIN-ARCHETYPE-1
#include "det_rng.h"  // DETERMINISTIC-RNG-1: hashMissionName for per-mission reseed

#ifndef COLLSN_H
#include"collsn.h"
#endif

#ifndef CMPONENT_H
#include"cmponent.h"
#include"unitprofile.h"		// EQUIPMENT-REGISTRY-FACADE-1: gated registry build hook
#endif

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef MOVER_H
#include"mover.h"
#endif

#ifndef MECH_H
#include"mech.h"
#endif

#ifndef GVEHICL_H
#include"gvehicl.h"
#endif

#ifndef CONTACT_H
#include"contact.h"
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

#ifndef GAMECAM_H
#include"gamecam.h"
#endif

#include "frame_jobs.h"  // FRAME-JOBS-1: fixed worker pool for parallel CPU prep
#include <vector>        // TERRAIN-REAUTH-UNPIN-1 Half B: mover cell scratch
#include "../GameOS/gameos/gos_terrain_lod_chunk.h"  // TERRAIN-REAUTH-UNPIN-1 Half B: visual-damp mover stamps

#ifndef MULTPLYR_H
#include"multplyr.h"
#endif

#ifndef WEAPONFX_H
#include"weaponfx.h"
#endif

#ifndef WEATHER_H
#include"weather.h"
#endif

#ifndef GATE_H
#include"gate.h"
#endif

#ifndef LOGISTICSDATA_H
#include"logisticsdata.h"
#endif

#ifndef LOGISTICSPILOT_H
#include"logisticspilot.h"
#endif

#ifndef GAMELOG_H
#include"gamelog.h"
#endif

#ifndef CELLIP_H
#include"cellip.h"
#endif

#ifndef PREFS_H
#include"prefs.h"
#endif

#include "../resource.h"

#include<gameos.hpp>
#include "mc2_verify.h"  // MC2-VERIFY-LIVE-1: live data-contract guards
#include "mc2_audit.h"   // META-FIX INFRA-1: gated audit emitter (design A/B markers)
#include "../GameOS/gameos/gpu_cull_substrate.h"  // C0-3: GPU cull substrate init/shutdown
#include "../GameOS/gameos/gpu_cull_compute.h"   // C1a: GPU visibility compute dispatch
#include "../GameOS/gameos/gpu_cull_readback.h"  // C2: async readback ring buffer
#include "../GameOS/gameos/gos_terrain_lighting.h"  // Phase 1: terrain lighting GPU compute
#include "gos_profiler.h"
#include "../GameAdapters/StaticPropRenderAdapter.h"  // M1 Task 13: firewall bridge (was: gos_static_prop_batcher.h + gos_static_prop_registry.h)
#include "../GameAdapters/MechRenderAdapter.h"          // M2: mech lifecycle adapter
#include "../GameAdapters/VegetationAdapter.h"          // vegetation card system lifecycle
#include "gos_mech_batcher.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "mech_brain_runtime.h"  // BRAIN-RUNTIME-1B: per-unit mode loading from _ai.fit
#include "brain_task_queue.h"    // BRAIN-RUNTIME-1B: BrainTaskType
#include "brain_tactic_select.h" // TACTIC-WEIGHTS-A: tacticName, applyPilotModulation (no mutation at load)
#include "tacordr.h"             // TACTIC-WEIGHTS-A: TacticType enum + NUM_TACTICS
#include "abl_trace.h"           // ABL-VM-FACTS-1: mission-brain fire + per-frame flush
#include "brain_order_intent.h"  // ABL-VM-FACTS-1: getBrainTickIndex()

inline constexpr int MAX_DISABLE_AT_START = 100;


extern void visualTuning_applyProfile(const char*);
extern void terrainMaterials_apply(const char*);
extern "C" int mc2FreezeMoversActive();
extern "C" void mc2FreezeMoversMissionReset();
extern thread_local float frameLength;
extern std::atomic<uint32_t> g_mc2FrameCounter;
extern float minFrameRate;
extern float minFrameLength;
extern long MissionStartTime;
extern Mission *mission;
extern unsigned long scenarioResult;
extern long scenarioEndTurn;
extern long MechSalvageChance;
extern long globalPlayerSkills[4];;
extern long globalPlayerWeapons[4];
extern long globalEnemySkills[4];
extern long globalEnemyWeapons[4];
extern float WeaponRanges[NUM_WEAPON_RANGE_TYPES][2];
extern float OptimalRangePoints[NUM_WEAPON_RANGE_TYPES];
extern bool OptimalRangePointInRange[NUM_WEAPON_RANGE_TYPES][3];
extern float globalMissionValues[MAX_GLOBAL_MISSION_VALUES];
extern glm::vec3 debugMechActorPosition[];
extern float mechDebugAngle[];
extern float torsoDebugAngle[];
extern glm::vec3 debugGVActorPosition;
extern GameObjectFootPrint* tempSpecialAreaFootPrints;
extern long tempNumSpecialAreas;
extern DWORD ServerPlayerNum;
extern unsigned long elementHeapSize;
extern unsigned long maxElements;
extern unsigned long maxGroups;
extern unsigned long missionHeapSize;
extern unsigned long polyHeapSize;
extern unsigned long spriteDataHeapSize;
extern unsigned long spriteHeapSize;
extern long	CurMultiplayCode;
extern long	CurMultiplayParam;
extern bool quitGame;
extern float MaxExtractUnitDistance;
extern bool useFog;
extern bool inViewMode;
extern unsigned long viewObject;
extern float loadProgress;
extern char TeamRelations[MAX_TEAMS][MAX_TEAMS];
extern unsigned int MultiPlayTeamId;
extern unsigned int MultiPlayCommanderId;
extern bool useSensors;
extern bool useCollisions;
extern long missionLineChanged;
extern bool GeneralAlarm;
extern bool KillAmbientLight;
extern GameLog* CombatLog;
extern bool neverEndingStory;
extern void GetBlockedDoorCells (int moveLevel, int door, char* openCells);
extern void PlaceStationaryMovers (MoveMap* map);
extern void PlaceMovers (void);
extern void initABL (void);
extern void closeABL (void);
extern long NumDisableAtStart;
extern long DisableAtStart[MAX_DISABLE_AT_START];
extern bool showFrameRate;
extern float OneOverProcessorSpeed;
extern "C" void mission_phase_begin();
extern "C" void mission_phase_mark(const char* name);
extern "C" void mission_phase_report();
extern long GameVisibleVertices;
extern float BaseHeadShotElevation;
extern bool DisplayCameraAngle;
extern long MaxResourcePoints;
extern bool loadInMissionSave;
extern bool saveInMissionSave;
extern bool g_resaveAfterQuickload;
extern bool 			invulnerableON;
#include "mission_globals.h"
extern int32_t NumStateTransitions;

void InitDifficultySettings (FitIniFile *gameSystemFile);   // defined in mission.cpp
#ifndef FINAL
extern float CheatHitDamage;   // defined in mission.cpp
#endif

#ifdef LAB_ONLY
extern __int64 x;
extern __int64 x1;
extern __int64 MCTimeABLLoad;
extern __int64 MCTimeMiscToTeamLoad;
extern __int64 MCTimeTeamLoad;
extern __int64 MCTimeObjectLoad;
extern __int64 MCTimeTerrainLoad;
extern __int64 MCTimeMoveLoad;
extern __int64 MCTimeMissionABLLoad;
extern __int64 MCTimeWarriorLoad;
extern __int64 MCTimeMoverPartsLoad;
extern __int64 MCTimeObjectiveLoad;
extern __int64 MCTimeCommanderLoad;
extern __int64 MCTimeMiscLoad;
extern __int64 MCTimeGUILoad;
extern __int64 MCTimeMissionTotal;
extern __int64 MCTimeTerrainUpdate;
extern __int64 MCTimeCameraUpdate;
extern __int64 MCTimeWeatherUpdate;
extern __int64 MCTimePathManagerUpdate;
extern __int64 MCTimeRunBrainUpdate;
extern __int64 MCTimePath1Update;
extern __int64 MCTimePath2Update;
extern __int64 MCTimePath3Update;
extern __int64 MCTimePath4Update;
extern __int64 MCTimePath5Update;
extern __int64 MCTimeCalcPath1Update;
extern __int64 MCTimeCalcPath2Update;
extern __int64 MCTimeCalcPath3Update;
extern __int64 MCTimeCalcPath4Update;
extern __int64 MCTimeCalcPath5Update;
extern __int64 MCTimeCalcGoal1Update;
extern __int64 MCTimeCalcGoal2Update;
extern __int64 MCTimeCalcGoal3Update;
extern __int64 MCTimeCalcGoal4Update;
extern __int64 MCTimeCalcGoal5Update;
extern __int64 MCTimeCalcGoal6Update;
extern __int64 MCTimeTerrainGeometry;
extern __int64 MCTimeInterfaceUpdate;
extern __int64 MCTimeCraterUpdate;
extern __int64 MCTimeTXMManagerUpdate;
extern __int64 MCTimeSensorUpdate;
extern __int64 MCTimeLOSUpdate;
extern __int64 MCTimeCollisionUpdate;
extern __int64 MCTimeMissionScript;
extern __int64 MCTimeTerrainObjectsUpdate;
extern __int64 MCTimeMechsUpdate;
extern __int64 MCTimeVehiclesUpdate;
extern __int64 MCTimeTurretsUpdate;
extern __int64 MCTimeAllElseUpdate;
extern __int64 MCTimeLOSCalc;
extern __int64 MCTimeAnimationCalc;
extern float   OneOverProcessorSpeed;
#endif

bool IsGateDisabled (int objectWID);
bool IsGateOpen (int objectWID);

// Mover-state bridge and coherence audit (mission_update_audit.cpp).
extern const bool s_bridgeMoverState;
void bridgeEmitMoverState(float scenarioTimeNow);
extern const bool s_moverCoherenceAudit;
void auditMoverCoherence(float scenarioTimeNow, MissionInterfaceManagerPtr mi);

#endif // MISSION_INTERNAL_H
