// Shared surface of the saveload*.cpp pieces: the include bundle and the file-scope declarations Mission::save and
// the Mission::load phases use.
#ifndef CODE_SAVELOAD_INTERNAL_H
#define CODE_SAVELOAD_INTERNAL_H

//----------------------------------------------------------------------------------
// Include Files
#ifndef MCLIB_H
#include"mclib.h"
#include "cpu_proj_cost_split.h"  // F3 CPU projection cost-baseline (hard_reset)
#endif

#ifndef MISSION_H
#include"mission.h"
#endif

#ifndef MOVEMGR_H
#include"movemgr.h"
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

#ifndef LOADSCREEN_H
#include"loadscreen.h"
#endif

#include "../resource.h"
#include<gameos.hpp>

// VPL-deferred item 11 fix: the in-mission-save load path (Mission::load)
// bypasses Mission::init and must mirror its finalizeGeometry() tail, or
// every mech of any type re-registered on restore stays invisible
// (submitActor fast-rejects while !s_geometryFinalized).
#include "../GameAdapters/StaticPropRenderAdapter.h"  // M1 CI-gate: resetForRestore() + finalizeGeometry()
#include "gos_mech_batcher.h"
#include "../GameOS/gameos/gpu_cull_compute.h"   // Stage 0.5 §0: mirror Init's compute_buildIndirectBuffer tail
#include "../GameOS/gameos/gpu_cull_substrate.h"   // Stage 0.5 §0 (cont'd): mirror Init's per-mission substrate re-init (mission.cpp:2807)
#include "../GameOS/gameos/gpu_cull_readback.h"    // Stage 0.5 §0 (cont'd): mirror Init's per-mission readback re-init (mission.cpp:2826)
#include "../GameOS/gameos/gos_terrain_lighting.h" // Stage 0.5 §0 (cont'd): mirror Init's per-mission terrain-lighting re-init (mission.cpp:2817)
#include "apprtype.h"
#include "Gates/gate.h"
#ifndef LINUX_BUILD
#include<ddraw.h>
#else
struct DDSURFACEDESC2 {
};
#endif

//----------------------------------------------------------------------------------------------------
// Globals
#define DEFAULT_SKY			1
extern float loadProgress;

void MouseTimerInit();
void MouseTimerKill();
void ProgressTimer(	RECT& WinRect,DDSURFACEDESC2& mouseSurfaceDesc );

extern volatile bool mc2IsInDisplayBackBuffer;
extern volatile bool mc2UseAsyncMouse;
extern volatile bool mc2IsInMouseTimer;
extern void (*AsynFunc)(RECT& WinRect,DDSURFACEDESC2& mouseSurfaceDesc );

#define USE_ABL_LOAD

void initABL (void);
void closeABL (void);

extern float WeaponRanges[NUM_WEAPON_RANGE_TYPES][2];
extern float OptimalRangePoints[NUM_WEAPON_RANGE_TYPES];
extern bool OptimalRangePointInRange[NUM_WEAPON_RANGE_TYPES][3];

extern unsigned long missionHeapSize;

//--------------------------------------------------
// Game System Constants -- Declarations here.
#include "mission_globals.h"
extern GameObjectPtr DebugGameObject[];

extern float BaseHeadShotElevation;

#ifndef FINAL
extern float CheatHitDamage;
#endif

void InitDifficultySettings (FitIniFile *gameSystemFile);

extern long GameVisibleVertices;

void GetBlockedDoorCells (int moveLevel, int door, char* openCells);
void PlaceStationaryMovers (MoveMap* map);
void PlaceMovers (void);

extern GameObjectFootPrint* tempSpecialAreaFootPrints;
extern long tempNumSpecialAreas;
extern GameLog* CombatLog;

bool IsGateDisabled (int objectWID);
bool IsGateOpen (int objectWID);

// META-FIX design E: dead, wrong-signature elfHash decl removed (was never
// called from this TU; non-extern-"C" so it never even matched the real
// symbol). The Tier-2 confinement grep (check-path-normalization.sh Rule 1)
// starts from a clean baseline.

extern char versionStamp[];

#endif  // CODE_SAVELOAD_INTERNAL_H
