//******************************************************************************************
//	mission.cpp - This file contains the mission class code
//		Missions are what scenarios were in MechCommander 1.
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//----------------------------------------------------------------------------------
// Include Files
#ifndef MCLIB_H
#include"mclib.h"
#include "gos_crashbundle.h"
#include "gos_smoke.h"            // S9D fixed-timestep deterministic smoke clock
#include "cpu_proj_cost_split.h"  // F3 CPU projection cost-baseline (hard_reset)
#define CB_PRINTF(fmt, ...) do { \
    char _cbbuf[256]; \
    snprintf(_cbbuf, sizeof(_cbbuf), fmt, ##__VA_ARGS__); \
    puts(_cbbuf); fflush(stdout); crashbundle_append(_cbbuf); \
} while (0)
#endif

#ifndef MISSION_H
#include"mission.h"
#endif

//#ifndef MOVE_H
#include"move.h"
//#endif

#ifndef MOVEMGR_H
#include"movemgr.h"
#endif

#ifndef GAMESOUND_H
#include"gamesound.h"
#endif

#ifndef SOUNDS_H
#include"sounds.h"
#endif

extern void visualTuning_applyProfile(const char*);  // MISSION-VISUAL-TUNING-1

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
#include "../GameOS/gameos/gpu_cull_substrate.h"  // C0-3: GPU cull substrate init/shutdown
#include "../GameOS/gameos/gpu_cull_compute.h"   // C1a: GPU visibility compute dispatch
#include "../GameOS/gameos/gpu_cull_readback.h"  // C2: async readback ring buffer
#include "../GameOS/gameos/gos_terrain_lighting.h"  // Phase 1: terrain lighting GPU compute

//----------------------------------------------------------------------------------
// Macro Definitions
//#define NUM_FIRERANGES		3

// CP-1: frame counter used in the per-mission shadow reset probe.
// Defined in mclib/tgl.cpp; incremented by GameOS frame-end path.
extern uint32_t g_mc2FrameCounter;

#ifdef LAB_ONLY
// LAB_ONLY timing globals — defined in mission2.cpp / mover.cpp.
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

//----------------------------------------------------------------------------------
// Static globals
float minFrameRate = 4.0;
float minFrameLength = 1.0/4.0;

long MissionStartTime =	0;			//No Idea

Mission *mission = NULL;
unsigned long scenarioResult = mis_PLAYING;
long scenarioEndTurn = -1;

extern long GameDifficulty;
long MechSalvageChance = 100;

long globalPlayerSkills[4];		//Per spec.  Integer value = percentage * 100
long globalPlayerWeapons[4];
long globalEnemySkills[4];
long globalEnemyWeapons[4];

extern float WeaponRanges[NUM_WEAPON_RANGE_TYPES][2];
extern float OptimalRangePoints[NUM_WEAPON_RANGE_TYPES];
extern bool OptimalRangePointInRange[NUM_WEAPON_RANGE_TYPES][3];

float globalMissionValues[MAX_GLOBAL_MISSION_VALUES];

extern Stuff::Vector3D debugMechActorPosition[];
extern float mechDebugAngle[];
extern float torsoDebugAngle[];

extern Stuff::Vector3D debugGVActorPosition;

extern GameObjectFootPrint* tempSpecialAreaFootPrints;
extern long tempNumSpecialAreas;
extern DWORD ServerPlayerNum;

extern bool useNonWeaponEffects;

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
extern bool useShadows;
extern bool inViewMode;
extern unsigned long viewObject;
extern float loadProgress;

extern char TeamRelations[MAX_TEAMS][MAX_TEAMS];

ByteFlag *VisibleBits = NULL;		//What can currently be seen
ByteFlag *SeenBits = NULL;			//What HAS been seen

UserHeapPtr missionHeap = NULL;

unsigned int MultiPlayTeamId = 0xFFFFFFFF;
unsigned int MultiPlayCommanderId = 0xFFFFFFFF;

bool useSensors = true;
bool useCollisions = true;
long missionLineChanged = 0;
bool GeneralAlarm = false;

extern bool KillAmbientLight;

extern GameLog* CombatLog;
#ifndef FINAL
float CheatHitDamage = 0.0f;
#endif

bool neverEndingStory = false;

void GetBlockedDoorCells (int moveLevel, int door, char* openCells);
void PlaceStationaryMovers (MoveMap* map);
void PlaceMovers (void);
//---------------------------------------------------------------------------
void initABL (void);
void closeABL (void);

#define	MAX_DISABLE_AT_START	100
extern long NumDisableAtStart;
extern long DisableAtStart[MAX_DISABLE_AT_START];
bool showFrameRate = false;

bool Mission::terminationCounterStarted = false;
double Mission::missionTerminationTime = -1.0;
unsigned long Mission::terminationResult = mis_PLAYING;

extern float OneOverProcessorSpeed;
extern PriorityQueuePtr	openList;

#include "gos_profiler.h"
#include "../GameAdapters/StaticPropRenderAdapter.h"  // M1 Task 13: firewall bridge (was: gos_static_prop_batcher.h + gos_static_prop_registry.h)
#include "../GameAdapters/MechRenderAdapter.h"          // M2: mech lifecycle adapter
#include "../GameAdapters/VegetationAdapter.h"          // vegetation card system lifecycle
#include "gos_mech_batcher.h"

// Phase-timing hooks implemented in GameOS/gameos/gameosmain.cpp.
extern "C" void mission_phase_begin();
extern "C" void mission_phase_mark(const char* name);

long GameVisibleVertices		= 500;
float BaseHeadShotElevation		= 1.0f;

bool DisplayCameraAngle = false;
extern long MaxResourcePoints;
extern long resolutionX;
extern long resolutionY;
extern long renderer;

bool loadInMissionSave = false;
bool saveInMissionSave = false;

float forcedFrameRate = -1.0f;

extern bool 			invulnerableON;		//Used for tutorials so mechs can take damage, but look like they are taking damage!  Otherwise, I'd just use NOPAIN!!

#define DEFAULT_SKY			1

// ---- MC2_MISSION_SPLIT: wall-ms split of the Mission::update sub-calls -----
// Locate which logic sub-call owns the 1K-map ~50ms (HITCH logic phase). Default
// OFF, zero behavior change. Emits per-call ns totals + averages at exit (stdout).
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
extern int32_t NumStateTransitions;  // ABL-VM-FACTS-1: read at mission-brain fire exit
namespace {
	static const bool s_misSplit = (getenv("MC2_MISSION_SPLIT") != nullptr);
	enum { MS_LAND_UPDATE=0, MS_PATHMGR, MS_CLEAR_BLOCKS, MS_CLEAR_VERTS,
	       MS_TERRAIN_TEX, MS_GEOMETRY, MS_OBJMGR,
	       MS_TOTAL, MS_CLEARARRAYS, MS_INTERFACE, MS_CAMERA, MS_VTOL, MS_WEATHER, MS_WAYPOINTS,
	       MS_COUNT };
	static const char* s_misNames[MS_COUNT] = {
		"land_update","pathmgr","clearBlocks","clearVerts","terrainTex","geometry","objmgr",
		"TOTAL","clearArrays","interface","camera","vtol","weather","waypoints" };
	static unsigned long long s_misNs[MS_COUNT]  = {0};
	static unsigned long long s_misMax[MS_COUNT] = {0};
	static unsigned long long s_misFrames = 0;
	static bool s_misAtexit = false;
	static void misEmit() {
		if (!s_misSplit) return;
		std::printf("[MISSION_SPLIT v1] event=shutdown frames=%llu", s_misFrames);
		for (int i = 0; i < MS_COUNT; i++)
			std::printf(" %s={avg_us:%.1f,max_us:%.1f}", s_misNames[i],
				s_misFrames ? (double)s_misNs[i]/s_misFrames/1000.0 : 0.0,
				(double)s_misMax[i]/1000.0);
		std::printf("\n"); std::fflush(stdout);
	}
	struct MisScope {
		int idx; std::chrono::steady_clock::time_point t0;
		explicit MisScope(int i) : idx(i) {
			if (s_misSplit) t0 = std::chrono::steady_clock::now();
		}
		~MisScope() {
			if (!s_misSplit) return;
			unsigned long long ns = (unsigned long long)
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - t0).count();
			s_misNs[idx] += ns;
			if (ns > s_misMax[idx]) s_misMax[idx] = ns;
		}
	};
}

//----------------------------------------------------------------------------------
// MC2_BRIDGE_MOVER_STATE -- runtime-bridge v0 (mover.state over stdout).
//
// When env MC2_BRIDGE_MOVER_STATE=1, emit one [MOVER v2] line per live mover
// every MC2_BRIDGE_MOVER_PERIOD_SEC seconds (default 2). Strictly READ-ONLY:
// only getters, every deref null-guarded. Zero cost when the env is unset
// (single getenv at startup; the per-frame check is one bool + one float
// compare). Consumed by the editor's GameplayDebugger live table; see
// docs/superpowers/strategy/runtime-bridge-architecture.md Phase 3 sec 11.
//----------------------------------------------------------------------------------
namespace {
	static const bool  s_bridgeMoverState =
		(getenv("MC2_BRIDGE_MOVER_STATE") != nullptr);
	static float s_bridgeNextEmitTime = 0.0f;   // scenarioTime of next burst
	static float bridgeMoverPeriodSec() {
		const char* e = getenv("MC2_BRIDGE_MOVER_PERIOD_SEC");
		float p = e ? (float)atof(e) : 2.0f;
		if (p < 0.25f) p = 0.25f;
		return p;
	}

	// Short name for the warrior status enum (see warrior.h WARRIOR_STATUS_*).
	static const char* bridgeWarriorStatusName(long s) {
		switch (s) {
			case WARRIOR_STATUS_NORMAL:      return "normal";
			case WARRIOR_STATUS_WITHDRAWING: return "withdrawing";
			case WARRIOR_STATUS_WITHDRAWN:   return "withdrawn";
			case WARRIOR_STATUS_EJECTED:     return "ejected";
			case WARRIOR_STATUS_DEAD:        return "dead";
			case WARRIOR_STATUS_MIA:         return "mia";
			case WARRIOR_STATUS_CAPTURED:    return "captured";
			case WARRIOR_STATUS_BASECAMP:    return "basecamp";
			default:                         return "unk";
		}
	}

	// Copy src into dst (cap chars) replacing spaces/'='/control with '_' so the
	// key=value [MOVER v2] line stays parseable even for names with spaces. Always
	// NUL-terminates; emits "?" for null/empty input.
	static void bridgeSanitize(char* dst, size_t cap, const char* src) {
		if (cap == 0) return;
		if (!src || !src[0]) { strncpy(dst, "?", cap - 1); dst[cap - 1] = 0; return; }
		size_t j = 0;
		for (size_t i = 0; src[i] && j + 1 < cap; ++i) {
			char c = src[i];
			if (c == ' ' || c == '=' || c == '\t' || c == '\r' || c == '\n')
				c = '_';
			dst[j++] = c;
		}
		dst[j] = 0;
	}

	// Emit [MOVER v2] lines for up to kCap live movers. Read-only; defensive.
	static void bridgeEmitMoverState(float scenarioTimeNow) {
		if (!s_bridgeMoverState || !ObjectManager)
			return;
		if (scenarioTimeNow < s_bridgeNextEmitTime)
			return;
		s_bridgeNextEmitTime = scenarioTimeNow + bridgeMoverPeriodSec();

		const long kCap = 64;
		long total = ObjectManager->getNumMovers();
		long emitted = 0;
		printf("[MOVER v2 begin] t=%.1f count=%ld\n", scenarioTimeNow, total);
		for (long i = 0; i < total && emitted < kCap; ++i) {
			MoverPtr mover = ObjectManager->getMover(i);
			if (!mover)
				continue;

			long              partId = mover->getPartId();
			char              name[64];
			bridgeSanitize(name, sizeof(name), mover->getName());
			long              team   = mover->getTeamId();
			Stuff::Vector3D   pos    = mover->getPosition();

			// HP as a 0..1 damage-derived fraction (getDamageLevel is the only
			// HP signal reachable without poking per-location body arrays).
			float dmg = mover->getDamageLevel();
			float hpFrac = 1.0f - dmg;
			if (hpFrac < 0.0f) hpFrac = 0.0f;
			if (hpFrac > 1.0f) hpFrac = 1.0f;

			char              pilotName[64];
			bridgeSanitize(pilotName, sizeof(pilotName), 0);   // default "?"
			long              orderVal  = -1;
			const char*       orderName = "none";
			long              targetId  = -1;

			// v2 brain/path/target detail. Defaults are emitted even when the
			// pilot or path is absent so the [MOVER v2] grammar stays fixed-arity
			// and the editor parser never has to handle optional fields.
			long              brainState = -1;   // ABL brain FSM state index (opaque)
			long              moveState  = -1;   // moveOrders.moveState
			Stuff::Vector3D   pgoal;             pgoal.Zero();   // world goal of active path
			long              pCurStep   = -1;   // step index being headed for
			long              pNumSteps  = 0;    // total steps in active path
			long              pCost      = -1;   // A* total path cost
			Stuff::Vector3D   tgtPos;            tgtPos.Zero();  // current target world pos

			MechWarriorPtr    pilot     = mover->getPilot();
			if (pilot) {
				bridgeSanitize(pilotName, sizeof(pilotName), pilot->getName());
				orderVal  = pilot->getStatus();
				orderName = bridgeWarriorStatusName(orderVal);
				brainState = pilot->getBrainState();
				moveState  = pilot->getMoveState();
				// getMovePath(long) is a PURE accessor (returns moveOrders.path[which]);
				// the zero-arg getMovePath() MUTATES (swaps path slots) and is never
				// called here. Null-guarded defensively though the path pair is
				// normally pre-allocated.
				MovePathPtr mp = pilot->getMovePath(0);
				if (mp) {
					pgoal     = mp->goal;
					pCurStep  = mp->curStep;
					pNumSteps = mp->numStepsWhenNotPaused;
					pCost     = mp->cost;
				}
				GameObjectPtr tgt = pilot->getCurrentTarget();
				if (tgt) {
					targetId = tgt->getPartId();
					tgtPos   = tgt->getPosition();
				}
			}

			printf("[MOVER v2] id=%ld name=%s team=%ld pos=%.1f,%.1f,%.1f "
				"hp=%.2f pilot=%s order=%ld/%s target=%ld "
				"brain=%ld mstate=%ld pgoal=%.1f,%.1f,%.1f pstep=%ld/%ld pcost=%ld "
				"tgtpos=%.1f,%.1f,%.1f\n",
				partId, name, team, pos.x, pos.y, pos.z,
				hpFrac, pilotName, orderVal, orderName, targetId,
				brainState, moveState, pgoal.x, pgoal.y, pgoal.z,
				pCurStep, pNumSteps, pCost,
				tgtPos.x, tgtPos.y, tgtPos.z);
			++emitted;
		}
		fflush(stdout);
	}

	// --- CRASH-SOAK harness (MC2_SOAK_AUTOWIN) -------------------------------
	// Auto-win the running mission after MC2_SOAK_WIN_AFTER_SEC of scenario
	// time so a campaign booted via MC2_BOOT_TO_BAY walks itself end-to-end
	// unattended. Gated entirely on the env flag; default OFF = byte-identical.
	static const bool s_soakAutoWin =
		(getenv("MC2_SOAK_AUTOWIN") != nullptr);
	static float soakWinAfterSec() {
		const char* e = getenv("MC2_SOAK_WIN_AFTER_SEC");
		float p = e ? (float)atof(e) : 5.0f;
		if (p < 0.0f) p = 0.0f;
		return p;
	}

	// --- MC2_SOAK_KILL_ENEMY -------------------------------------------------
	// When set, kills one random live enemy BattleMech before autowin fires so
	// the salvage screen has a candidate, and then programmatically salvages it
	// into LogisticsData inventory.  Default OFF = byte-identical when unset.
	static const bool s_soakKillEnemy =
		(getenv("MC2_SOAK_KILL_ENEMY") != nullptr);

	// Stores the variantName of the enemy killed this mission so the next
	// mission can assert it reached the load-bay inventory.
	static char s_soakKilledVariant[64] = {};
	static bool s_soakKillDone = false;      // armed once per mission
	static bool s_soakSalvageDone = false;   // salvage commit fired once per mission
	static bool s_soakInventoryChecked = false; // load-bay check fired once per mission

	// --- MC2_SOAK_PILOT_PROMOTE ----------------------------------------------
	// Grants a deployed pilot enough skill delta to cross the next rank
	// threshold, then runs the same promotePilot() call the UI uses.
	// Throttled to mission-index 2 and 5 within a campaign (1-based).
	// Default OFF = byte-identical when unset.
	static const bool s_soakPilotPromote =
		(getenv("MC2_SOAK_PILOT_PROMOTE") != nullptr);
	static bool s_soakPromoteFired = false;  // armed once per mission
} // namespace

//----------------------------------------------------------------------------------
// class Mission
long Mission::update (void)
{
	ZoneScopedN("Mission::update");
	if (s_misSplit) {
		if (!s_misAtexit) { s_misAtexit = true; std::atexit(misEmit); }
		++s_misFrames;
	}
	MisScope _msTotal(MS_TOTAL);  // whole Mission::update; TOTAL - sum(subcalls) = unbracketed
	if (active)
	{
		turn++;

		memset(ObjectManager->moverLineOfSightTable, -1, ObjectManager->maxMovers*ObjectManager->maxMovers);

#ifdef LAB_ONLY
		MCTimeLOSCalc = 0;
		MCTimeAnimationCalc = 0;
#endif

		if (forcedFrameRate != -1.0f)
			frameLength /= forcedFrameRate;

		if ((missionLineChanged + 50) < turn)
		{
			#ifndef FINAL
			if (userInput->getKeyDown(KEY_X) && userInput->ctrl() && !userInput->alt() && !userInput->shift())
			{
				useCollisions ^= true;
				missionLineChanged = turn;
			}		
		
			if (userInput->getKeyDown(KEY_X) && userInput->ctrl() && userInput->alt() && userInput->shift())
			{
				saveInMissionSave = true;
				missionLineChanged = turn;
			}		

			if (userInput->getKeyDown(KEY_Z) && userInput->ctrl() && userInput->alt() && userInput->shift())
			{
				loadInMissionSave = true;
				missionLineChanged = turn;
			}		

			if (userInput->getKeyDown(KEY_Y) && userInput->ctrl() && !userInput->alt() && !userInput->shift())
			{
				DisplayCameraAngle ^= true;
				missionLineChanged = turn;
			}		

 			if (userInput->getKeyDown(KEY_N) && userInput->ctrl() && !userInput->alt() && !userInput->shift())
			{
				useSensors ^= true;
				missionLineChanged = turn;
			}

			if (userInput->getKeyDown(KEY_P) && userInput->ctrl() && userInput->alt() && userInput->shift())
			{
				LogisticsData::instance->setResourcePoints(100000);
				missionLineChanged = turn;
			}		

			if (userInput->getKeyDown(KEY_R) && userInput->ctrl() && userInput->alt() && userInput->shift())
			{
				if (forcedFrameRate == -1.0f)
					forcedFrameRate = 2.0f;
				else if (forcedFrameRate == 2.0f)
					forcedFrameRate = 5.0f;
				else if (forcedFrameRate == 5.0f)
					forcedFrameRate = 10.0f;
				else if (forcedFrameRate == 10.0f)
					forcedFrameRate = -1.0f;

				missionLineChanged = turn;
			}

			if (userInput->getKeyDown(KEY_C) && userInput->ctrl() && userInput->alt() && userInput->shift())
			{
				neverEndingStory = true;
				missionLineChanged = turn;
			}

			// DEV HOTKEY (Ctrl+Alt+Shift+K): disable ALL enemy mechs on demand, the same
			// way MC2_SOAK_KILL_ENEMY does (OBJECT_STATUS_DISABLED, not destroyed) so they
			// stay salvageable. Lets you grab salvage without a full firefight. Disabled in
			// FINAL builds (this whole block is #ifndef FINAL).
			if (userInput->getKeyDown(KEY_K) && userInput->ctrl() && userInput->alt() && userInput->shift())
			{
				int killed = 0;
				for (int mi = 0; mi < ObjectManager->numMechs; ++mi)
				{
					BattleMech* pM = ObjectManager->getMech(mi);
					if (!pM) continue;
					if (pM->isDisabled() || pM->isDestroyed()) continue;
					if (Team::home && pM->getTeamId() == Team::home->id) continue;  // skip friendlies
					pM->setStatus(OBJECT_STATUS_DISABLED, /*force=*/true);
					++killed;
				}
				printf("[DEVHOTKEY] kill-all-enemy killed=%d (Ctrl+Alt+Shift+K)\n", killed);
				fflush(stdout);
				missionLineChanged = turn;
			}

			// Tessellation/phong debug F-keys removed: F6/F7/F8 now drive the
			// Tactical Overview / sensor / weapon-range views (mechcmd2.cpp), and
			// terrain tuning lives in the imgui overlay. Shadow-softness keys kept.
			{
				// Shadow softness (Poisson disk radius) — edge-triggered (single step
				// per press, not continuous while held) with 0.1 step for fine control.
				if (gos_GetKeyStatus(KEY_LBRACKET) == KEY_PRESSED) {
					float s = max(gos_GetTerrainShadowSoftness() - 0.1f, 0.1f);
					gos_SetTerrainShadowSoftness(s);
					CB_PRINTF("[SHADOW-KEY] [: softness=%.1f", s);
				}
				if (gos_GetKeyStatus(KEY_RBRACKET) == KEY_PRESSED) {
					float s = min(gos_GetTerrainShadowSoftness() + 0.1f, 10.0f);
					gos_SetTerrainShadowSoftness(s);
					CB_PRINTF("[SHADOW-KEY] ]: softness=%.1f", s);
				}
			}
			#endif
		}
	
		//---------------------------------------
		// The game believes we will drop well below par for a frame or two.
		// put the game into slo-mo so we don't lose any frames of animation.
		dynamicFrameTiming = true;
			
		//---------------------------------------
		//Save the game in-Mission if requested
		if (saveInMissionSave)
		{
            // sebi:
			//save("data" PATH_SEPARATOR "savegame" PATH_SEPARATOR "testgame.ims");
            char savegame_path[1024];
            S_snprintf(savegame_path, sizeof(savegame_path)/sizeof(savegame_path[0]), "%s" PATH_SEPARATOR "testgame.ims", savePath);
			save(savegame_path);
			saveInMissionSave = false;
		}

		//--------------------------------------------------
		// Update length of time scenario has been running.
		if (!missionInterface->isPaused() || MPlayer )
		{
			//First Frame we just set LastTimeGetTime.
			// After that, it increments based on System Time.
			// NOT the crazy GameOS frameRate.
			// S9D: under MC2_SMOKE_FIXED_TIMESTEP, substitute a fixed step so
			// frame N reaches an identical sim state every run. Single cached
			// bool check; OFF path is byte-identical to retail below.
			if (SmokeMode::fixedTimestepEnabled())
			{
				float milliseconds = SmokeMode::fixedTimestepMs();   // 1000/30
				const bool firstFrame = (LastTimeGetTime == 0xffffffff);
				// Seed LastTimeGetTime on the first frame so the first fixed
				// step is clean (no huge initial real-time delta); thereafter
				// advance it by exactly one fixed step.
				DWORD currentTimeGetTime =
					firstFrame ? timeGetTime()
					           : (DWORD)(LastTimeGetTime + (DWORD)milliseconds);
				if (!firstFrame)
					scenarioTime += (milliseconds / 1000.0f);
				LastTimeGetTime = currentTimeGetTime;
				// Deterministic clock probe (frames 60/120/180). Counts only
				// frames where the fixed step advanced scenarioTime.
				if (!firstFrame)
					SmokeMode::fixedTimestepOnSimFrame((double)scenarioTime);
			}
			else
			{
				DWORD currentTimeGetTime = timeGetTime();
				if (LastTimeGetTime != 0xffffffff)
				{
					float milliseconds = currentTimeGetTime - LastTimeGetTime;
					scenarioTime += (milliseconds / 1000.0f);
				}
				LastTimeGetTime = currentTimeGetTime;
			}

			soundSystem->clearIsPaused();
		}
		else
		{
			//Keep track of system time.  Just don't add it to scenarioTime!!
			DWORD currentTimeGetTime = timeGetTime();
			LastTimeGetTime = currentTimeGetTime;

			soundSystem->setIsPaused();
		}

		//--------------------------------------------------
		
		//------------------------------------------------------------------------
		// There is a TINYYYYYYYYYY chance this will never go if timeGetTime()
		// happens to return 0 (one millisecond every approx. 49 days). I can live
		// with that...

//		if (MPlayer && (MPlayer->startTime >= 0.0))
//			runningTime = (float)(gos_GetElapsedTime()) - MPlayer->startTime;
//		else if (MPlayer && (scenarioTime > 10.0))
//			Fatal(MissionStartTime, " runningTime is not working...why? ");

//		if (MPlayer)
//			actualTime = runningTime;
//		else
		//Try ALWAYS using SCENARIO Time.  This has been specially coded to be nummies!!
			actualTime = scenarioTime;

#if 0
		static bool tested = false;
		if ((scenarioTime > 15.0) && !tested) {
			tested = true;
			ABLFile* ablSaveFile;
			ABLi_saveEnvironment (ABLFile* ablFile) {

		}
#endif

		ZoneScopedN("GameLogic.Mission.Update");
		{ ZoneScopedN("Mission.clearArrays"); MisScope _ms(MS_CLEARARRAYS); mcTextureManager->clearArrays(); }

		if (missionInterface)
			{ ZoneScopedN("GameLogic.Mission.Interface"); MisScope _ms(MS_INTERFACE); missionInterface->update(); }

		{ ZoneScopedN("GameLogic.Mission.Camera"); MisScope _ms(MS_CAMERA); eye->update(); }

		// [MVP_EARLY v1] MVP-PUBLISH-EARLY-HOIST (gate MC2_MVP_PUBLISH_EARLY,
		// default ON, =0 reverts). Publish THIS frame's world-to-clip matrix here, right
		// after eye->update(), so the terrain compute snapshot taken later in
		// this same UPDATE phase (land->geometry() -> ComputeDispatch reads
		// gos_GetTerrainMVPMat4) sees the current-frame camera instead of the
		// previous frame's. The RENDER-phase publish at gamecam.cpp:282 is KEPT
		// unconditionally; it rewrites the identical matrix this frame so gate
		// OFF is byte-identical to today and A/B stays clean.
		//
		// GUARD: mirror the gamecam.cpp:261 render+publish predicate exactly
		// (eye->active && turn > 1). eye is always the live GameCamera inside
		// Mission::update (SimpleCamera intro/deploy pans and the editor run
		// their OWN update/render loops, NOT this one, and self-publish), so
		// this never clobbers their publish.
		{
			static const bool s_mvpPublishEarly = []() {
				const char* v = getenv("MC2_MVP_PUBLISH_EARLY");
				return !(v && atoi(v) == 0);  // default ON; only =0 reverts
			}();
			if (s_mvpPublishEarly && eye && eye->active && turn > 1)
			{
				gos_SetWorldToClipGL(eye->worldToClipGL());
				// Record the publish-seq for ComputeDispatch's [MVP_EARLY v1]
				// proof line (extern counters in gameos_graphics.cpp).
				extern long g_mvpDiagFrame;
				extern long g_mvpEarlyPublishSeq;
				g_mvpEarlyPublishSeq = g_mvpDiagFrame;
			}
		}

		{ ZoneScopedN("GameLogic.Mission.VTol"); MisScope _ms(MS_VTOL); missionInterface->updateVTol(); }

		{ ZoneScopedN("GameLogic.Mission.Terrain"); MisScope _ms(MS_LAND_UPDATE); land->update(); }

		//ALWAYS update weather AFTER the camera.  May change the lights!
		if (useNonWeaponEffects)
			{ ZoneScopedN("GameLogic.Mission.Weather"); MisScope _ms(MS_WEATHER); weather->update(); }		//Should the rain fall during a pause?

		{ ZoneScopedN("GameLogic.Mission.Waypoints"); MisScope _ms(MS_WAYPOINTS); missionInterface->updateWaypoints(); }

#ifdef USE_PATH_COST_TABLE
		GlobalMoveMap[0]->resetPathCostTable();
#endif
		{ ZoneScopedN("GameLogic.PathManager"); MisScope _ms(MS_PATHMGR); PathManager->update(); }

		if (KillAmbientLight) {
	//		ambientRed<<16)+(ambientGreen<<8)+ambientBlue;
		}
		
		//-----------------------------------------------------------
		// Lastly, process the terrain geometry which loads textures
		// Must do this to keep from Locking during the updateRenders phase
		// Also reset the object flags because we recalc those during geometry!
		{ ZoneScopedN("Mission::update clearObjBlocksActive"); MisScope _ms(MS_CLEAR_BLOCKS); land->clearObjBlocksActive(); }
		{ ZoneScopedN("Mission::update clearObjVerticesActive"); MisScope _ms(MS_CLEAR_VERTS); land->clearObjVerticesActive(); }
		{ ZoneScopedN("Mission::update terrainTextures->update"); MisScope _ms(MS_TERRAIN_TEX); land->terrainTextures->update(); }

		{ ZoneScopedN("GameLogic.Mission.TerrainGeometry"); MisScope _ms(MS_GEOMETRY); land->geometry(); }

		// 2026-05-13: begin GPU cull substrate frame BEFORE the pause-branch.
		// Was inside GameObjectManager::update (objmgr.cpp), which is
		// pause-gated.  Render-time submits (BldgAppearance/TreeAppearance::
		// render and GpuStaticPropRegistry::flush) keep appending substrate
		// records every frame regardless of pause, so this reset must also
		// run every frame to keep the ring slot and per-frame counter in
		// sync.  Calling without isEnabled gating is safe — substrate_frameBegin
		// internally checks isEnabled() and is a no-op when disabled.
		// Fixes the pause-smear bug where paused frames accumulated
		// substrate records across the un-reset ring slot, inflating
		// per-bucket instanceCount in compute cull and causing coalesce
		// multi-draw to render copies of other types' geometry at every
		// prop's origin.
		gpu_cull::substrate_frameBegin();

		if ( missionInterface->isPaused() && !MPlayer )
			ObjectManager->updateAppearancesOnly( true, true, true );
		else
			{ MisScope _ms(MS_OBJMGR); ObjectManager->update(true, true, true); }

		// C1b GPU authority flip: compute_dispatch() has been MOVED to txmmgr.cpp
		// (between GpuStaticPropRegistry::flush() and GpuStaticPropBatcher::flush())
		// so that static prop substrate records appended during registry flush are
		// visible to the cull shader. compute_emitParitySummary() still runs here
		// (no static-prop records yet in the substrate; summary is for dynamic actors
		// from substrate_flushUpload earlier this frame — timing is acceptable).
		if (gpu_cull::compute_isEnabled()) {
			gpu_cull::compute_emitParitySummary();
		}

		{ ZoneScopedN("GameLogic.Mission.Craters"); craterManager->update(); }
		
		//Do not UPDATE the textures during a pause.
		//This uncaches things which only objectManager->update can cache back in!!!!!
		if ( !missionInterface->isPaused() || MPlayer )
			{ ZoneScopedN("GameLogic.Mission.TextureManager"); mcTextureManager->update(); }

		//--------------------------------------
		// update sensor and contact managers...
		if (useSensors && ( !missionInterface->isPaused() || MPlayer ) )
			{ ZoneScopedN("GameLogic.Mission.Sensors"); SensorManager->update(); }

		if (useCollisions && ( !missionInterface->isPaused() || MPlayer ) )
			{ ZoneScopedN("GameLogic.Mission.Collisions"); ObjectManager->updateCollisions(); }

		if (missionBrain)
		{
			if ( !missionInterface->isPaused() || MPlayer )
			{
				// ABL-VM-FACTS-1: time the mission-brain fire; byte-identical when OFF.
				mc2_abl_trace::FireScope ablFire("mission", -1);
				long ablStmts;
				{ ZoneScopedN("GameLogic.AI.BrainExecute"); ablStmts = missionBrain->execute(); }
				ablFire.record(ablStmts, NumStateTransitions);
				long missionResult = missionBrain->getInteger();
				if (missionResult == 9999)
					return(terminationResult = 9999);
				if (!MPlayer)
					terminationResult = missionResult;
			}
		}

		// ABL-VM-FACTS-1: once/frame boundary -- flush aggregated per-frame ABL VM
		// facts (all warrior fires + the mission fire above). No-op when OFF.
		mc2_abl_trace::flushFrame(g_mc2FrameCounter, getBrainTickIndex());

		// Runtime-bridge v0: emit [MOVER v2] mover.state to stdout (env-gated,
		// throttled to MC2_BRIDGE_MOVER_PERIOD_SEC). Read-only; no-op unless
		// MC2_BRIDGE_MOVER_STATE=1.
		if (s_bridgeMoverState)
			bridgeEmitMoverState(scenarioTime);

		//----------------------------------------------------
		// Check is all player forces dead/disabled.
		if (!MPlayer && !terminationCounterStarted)
		{
			bool playerForceAllDead = true;
			for (long i=0;i<ObjectManager->getNumMovers();i++)
			{
				MoverPtr mover = ObjectManager->getMover(i);
				if (mover->getCommanderId() == Commander::home->getId())
					if (!mover->isDisabled() && !mover->isDestroyed() && mover->isOnGUI()) {
						playerForceAllDead = false;
						break;
					}
			}

			if (playerForceAllDead && !neverEndingStory)
			{
				terminationResult = mis_PLAYER_LOST_BIG;
				terminationCounterStarted = true;
				missionTerminationTime = actualTime + 5.0/*seconds*/;
			}
		}

		if (!neverEndingStory && terminationCounterStarted) 
		{
			if (missionTerminationTime <= actualTime) {
				if (ControlGui::instance->resultsDone()) {
					/* if the gui is finished rendering objective results then end it */
					scenarioResult = terminationResult;

					if ( !scenarioResult )
					{
						// mission brain frequently trashes the termination result
						if (!MPlayer) {
							int status = Team::home->objectives.Status();
							if (OS_SUCCESSFUL == status) 
								scenarioResult = mis_PLAYER_WIN_BIG;
							else
								scenarioResult = mis_PLAYER_LOST_BIG;
						}
					}

					// need to reset the termination result
				}
				else if (!MPlayer) {
					/* if all the player's dudes are dead then end it */
					bool playerForceAllDead = true;
					for (long i=0;i<ObjectManager->getNumMovers();i++)
					{
						MoverPtr mover = ObjectManager->getMover(i);
						if (mover->getCommanderId() == Commander::home->getId())
							if (!mover->isDisabled() && !mover->isDestroyed() && mover->isOnGUI()) {
								playerForceAllDead = false;
								break;
							}
					}

					if (playerForceAllDead)
					{
						terminationResult = mis_PLAYER_LOST_BIG;
						terminationCounterStarted = true;
						missionTerminationTime = actualTime + 5.0/*seconds*/;
					}
				}
			}
		} 
		else 
		{
			//As long as the ABL script returns -1, we won't do this.  NEEDED for Tutorials
			if (terminationResult != -1)
			{
				if (MPlayer) 
				{
					if (MPlayer->calcMissionStatus()) 
					{
						terminationCounterStarted = true;
						missionTerminationTime = actualTime + 5.0/*seconds*/;
						if (MPlayer->playerInfo[MPlayer->commanderID].winner)
							terminationResult = mis_PLAYER_WIN_BIG;
						else
							terminationResult = mis_PLAYER_LOST_BIG;

					}
				}
				else if (1 <= Team::home->objectives.Count()) 
				{
					int status = Team::home->objectives.Status();
					if (0 < Team::home->numPrimaryObjectives) 
					{
						if (OS_UNDETERMINED != status) 
						{
							if (!neverEndingStory)
							{
								if (OS_SUCCESSFUL == status) 
								{
									terminationResult = mis_PLAYER_WIN_BIG;
									terminationCounterStarted = true;
									missionTerminationTime = actualTime + 5.0/*seconds*/;
								} 
								else 
								{
									terminationResult = mis_PLAYER_LOST_BIG;
									terminationCounterStarted = true;
									missionTerminationTime = actualTime + 5.0/*seconds*/;
								}
							}
						}
					}
				}
			}
		}

		if (!MPlayer && !terminationCounterStarted)
		{
			if ((m_timeLimit != -1.0f) && (actualTime > m_timeLimit) && !neverEndingStory)
			{
				terminationResult = mis_PLAYER_LOST_BIG;
				terminationCounterStarted = true;
				missionTerminationTime = actualTime + 5.0/*seconds*/;
			}
		}

		if (userInput->getKeyDown(KEY_F) && !userInput->ctrl() && userInput->alt() && !userInput->shift())
		{
			showFrameRate ^= true;
			missionLineChanged = turn;
		}

#ifndef FINAL
		bool cheatWin = (userInput->getKeyDown(KEY_W) && userInput->shift() && userInput->ctrl() && userInput->alt());
		bool cheatLose = (userInput->getKeyDown(KEY_L) && userInput->shift() && userInput->ctrl() && userInput->alt()); 
		if (cheatWin)
			scenarioResult = mis_PLAYER_WIN_BIG;
		else if (cheatLose)
			scenarioResult = mis_PLAYER_LOST_BIG;

		//FORCE these to play to thwart any ABL bugs.
		// OPPOSITE for a reason!!!!!!!!
		// Duane decided to switch the order for our conveniance.  Thanks!!
		if ((terminationResult) && (terminationResult != -1) && (terminationResult < mis_PLAYER_DRAW))
		{
			soundSystem->playDigitalMusic(WIN_TUNE_0);
		}
		else if (terminationResult > mis_PLAYER_DRAW)
		{
			soundSystem->playDigitalMusic(LOSE_TUNE_0);
		}

		if (showFrameRate)
		{
			char text[1024];
			sprintf(text,"FrameRate: %f",1.0f/frameLength);
		
			DWORD width, height;
			Stuff::Vector4D moveHere;
			moveHere.x = 0.0f;
			moveHere.y = 0.0f;
		
			gos_TextSetAttributes (gosFontHandle, 0, gosFontScale, false, true, false, false);
			gos_TextStringLength(&width,&height,text);
		
			moveHere.z = width;
			moveHere.w = height;
		
			globalFloatHelp->setFloatHelp(text,moveHere,SD_GREEN,XP_BLACK,1.0f,true,false,false,false);
		}
#endif

		// CRASH-SOAK auto-win (always compiled; gated on MC2_SOAK_AUTOWIN env).
		// Independent of the #ifndef FINAL cheat path above so the harness works
		// in RelWithDebInfo regardless of FINAL. Fires once per mission after
		// MC2_SOAK_WIN_AFTER_SEC of scenario time. mis_PLAYER_WIN_BIG returned
		// from update() ends the mission instantly (no countdown).
		if (s_soakAutoWin && !scenarioResult)
		{
			static bool s_soakWinEmitted = false;
			const float winAfter = soakWinAfterSec();
			// Re-arm for each new mission: scenarioTime resets to 0 on start().
			if (scenarioTime < winAfter)
				s_soakWinEmitted = false;
			if (!s_soakWinEmitted && scenarioTime > winAfter)
			{
				scenarioResult = mis_PLAYER_WIN_BIG;
				s_soakWinEmitted = true;
				printf("[SOAK] autowin mission=%s t=%.1f\n",
					missionFileName, (float)scenarioTime);
				fflush(stdout);
			}
		}

		// --- MC2_SOAK_KILL_ENEMY ---------------------------------------------
		// Three phases per mission pair.
		// Phase 1 (current mission, t=60% win): disable one enemy mech.
		// Phase 2 (current mission, post-win):  commit it to inventory.
		// Phase 3 (next mission, first frame):  verify it is in load bay.
		// Re-arm clears kill/salvage flags but intentionally does NOT clear
		// s_soakKilledVariant until AFTER Phase 3 has fired, so the cross-
		// mission variant name survives the mission boundary.
		if (s_soakKillEnemy && s_soakAutoWin)
		{
			// PHASE 3 (next mission, first frames): load-bay check using the
			// variant name carried over from last mission.  Must run BEFORE
			// re-arm to avoid s_soakKilledVariant being cleared too early.
			if (!s_soakInventoryChecked && s_soakSalvageDone &&
				s_soakKilledVariant[0] &&
				scenarioTime >= 0.0f && scenarioTime < 1.0f)
			{
				LogisticsVariant* pChkVar =
					LogisticsData::instance
						? LogisticsData::instance->getVariant(s_soakKilledVariant)
						: nullptr;
				int present = 0;
				if (pChkVar && LogisticsData::instance)
					present = LogisticsData::instance->getVariantsInInventory(
						pChkVar, /*bIncludeForceGroup=*/true);
				printf("[SOAK] salvage-in-loadbay name=%s present=%d\n",
					s_soakKilledVariant, present > 0 ? 1 : 0);
				fflush(stdout);
				s_soakInventoryChecked = true;
			}

			// Re-arm each new mission (scenarioTime resets to 0 on start()).
			// Only clears variant name after Phase 3 has had its window.
			if (scenarioTime < soakWinAfterSec() * 0.5f)
			{
				s_soakKillDone = false;
				s_soakSalvageDone = false;
				if (s_soakInventoryChecked)
				{
					s_soakInventoryChecked = false;
					s_soakKilledVariant[0] = '\0';
				}
			}

			// PHASE 1: "cheat mode" — disable EVERY enemy mech at 60% of the win
			// timer so they are all DISABLED (not DESTROYED) and therefore all
			// salvageable. This maximally stresses the salvage system and mech-bay
			// capacity over a campaign. Remember the first variant for the Phase 3
			// load-bay round-trip check.
			const float killAt = soakWinAfterSec() * 0.6f;
			if (!s_soakKillDone && scenarioTime > killAt && !scenarioResult)
			{
				int killed = 0;
				for (int mi = 0; mi < ObjectManager->numMechs; ++mi)
				{
					BattleMech* pM = ObjectManager->getMech(mi);
					if (!pM) continue;
					if (pM->isDisabled() || pM->isDestroyed()) continue;
					// Enemy = any team other than the player home team.
					if (Team::home && pM->getTeamId() == Team::home->id) continue;
					// OBJECT_STATUS_DISABLED: salvage screen keeps it (isDisabled
					// true, isDestroyed false).
					pM->setStatus(OBJECT_STATUS_DISABLED, /*force=*/true);
					if (!s_soakKilledVariant[0])
					{
						strncpy(s_soakKilledVariant, pM->variantName,
							sizeof(s_soakKilledVariant) - 1);
						s_soakKilledVariant[sizeof(s_soakKilledVariant) - 1] = '\0';
					}
					++killed;
				}
				printf("[SOAK] kill-enemy killed=%d first=%s\n",
					killed, s_soakKilledVariant[0] ? s_soakKilledVariant : "(none)");
				fflush(stdout);
				s_soakKillDone = true;
			}

			// PHASE 2: After autowin, salvage ALL disabled enemy mechs into the
			// inventory (mirrors SalvageMechScreen -> addMechToInventory, done for
			// every salvageable mech). This is where mech-bay capacity gets stressed.
			if (!s_soakSalvageDone && scenarioResult)
			{
				int salvCount = 0;
				if (LogisticsData::instance)
				{
					for (int mi = 0; mi < ObjectManager->numMechs; ++mi)
					{
						BattleMech* pM = ObjectManager->getMech(mi);
						if (!pM) continue;
						if (!pM->isDisabled() || pM->isDestroyed()) continue;
						if (Team::home && pM->getTeamId() == Team::home->id) continue;
						LogisticsVariant* pVar =
							LogisticsData::instance->getVariant(pM->variantName);
						if (!pVar) continue;
						LogisticsData::instance->addMechToInventory(
							pVar, (LogisticsPilot*)nullptr, 0, /*bSubtractPts=*/false);
						++salvCount;
					}
				}
				printf("[SOAK] salvage-check items=%d first=%s\n",
					salvCount, s_soakKilledVariant[0] ? s_soakKilledVariant : "(none)");
				fflush(stdout);
				s_soakSalvageDone = true;
			}
		}

		// --- MC2_SOAK_PILOT_PROMOTE ------------------------------------------
		// Fires on campaign mission numbers 2 and 5 (via getCurrentMissionNum).
		// Grants enough gunnery to cross the next rank threshold, then runs
		// promotePilot() exactly as pilotreviewarea.cpp:215 does.
		// Throttle: getCurrentMissionNum() % 10 == 2 or == 5 — roughly 2
		// firings per 10-mission stretch without hammering every mission.
		if (s_soakPilotPromote && s_soakAutoWin)
		{
			// Re-arm at start of each mission, and count missions played once each
			// (edge-triggered) so the throttle does not depend on
			// getCurrentMissionNumber() semantics (not a clean 1..N per campaign).
			static int  s_soakMissionCount   = 0;
			static bool s_soakMissionCounted = false;
			if (scenarioTime < soakWinAfterSec() * 0.5f)
			{
				if (!s_soakMissionCounted) { ++s_soakMissionCount; s_soakMissionCounted = true; }
				s_soakPromoteFired = false;
			}
			else
			{
				s_soakMissionCounted = false;
			}

			// Throttle: ~twice per campaign — the 2nd and 5th missions played.
			const bool throttlePass =
				(s_soakMissionCount == 2 || s_soakMissionCount == 5);

			const float promoteAt = soakWinAfterSec() * 0.7f;
			if (!s_soakPromoteFired && throttlePass &&
				scenarioTime > promoteAt && !scenarioResult)
			{
				// Find first deployed pilot via MechWarrior::warriorList.
				LogisticsPilot* lPilot = nullptr;
				const char* pilotName = "(none)";
				for (int wi = 1; wi <= MechWarrior::numWarriors && !lPilot; ++wi)
				{
					MechWarriorPtr mw = MechWarrior::warriorList[wi];
					if (!mw) continue;  // getPilot() below gates to real deployed pilots
					// Resolve to LogisticsPilot via LogisticsData.
					LogisticsPilot* lp =
						LogisticsData::instance
							? LogisticsData::instance->getPilot(mw->getName())
							: nullptr;
					if (!lp) continue;
					lPilot = lp;
					pilotName = lp->getName();
				}

				if (lPilot)
				{
					const long rankBefore = lPilot->getRank();
					// soakAddGunnery bumps both lifetime gunnery and newGunnery
					// delta so promotePilot() sees a mission gain large enough
					// to cross a rank threshold from any starting rank.
					// 11 pts guarantees crossing regardless of start (worst case
					// GREEN->REGULAR needs avg 50+epsilon; 11 pts covers all).
					lPilot->soakAddGunnery(11.0f);
					bool promoted = lPilot->promotePilot();
					const long rankAfter = lPilot->getRank();
					printf("[SOAK] pilot-promote pilot=%s rankBefore=%ld rankAfter=%ld ok=%d\n",
						pilotName, rankBefore, rankAfter, promoted ? 1 : 0);
					fflush(stdout);
				}
				else
				{
					printf("[SOAK] pilot-promote no-deployed-pilot\n");
					fflush(stdout);
				}
				s_soakPromoteFired = true;
			}
		}

#ifdef LAB_ONLY
		MCTimeMissionTotal 		= MCTimeTerrainUpdate +
									MCTimeCameraUpdate +
									MCTimeWeatherUpdate + 
									MCTimePathManagerUpdate +
									MCTimeRunBrainUpdate +
									MCTimePath1Update + 
									MCTimePath2Update + 
									MCTimePath3Update + 
									MCTimePath4Update + 
									MCTimePath5Update + 
									MCTimeCalcPath1Update +
									MCTimeCalcPath2Update +
									MCTimeCalcPath3Update +
									MCTimeCalcPath4Update +
									MCTimeCalcPath5Update +
									MCTimeCalcGoal1Update + 
									MCTimeCalcGoal2Update + 
									MCTimeCalcGoal3Update + 
									MCTimeCalcGoal4Update + 
									MCTimeCalcGoal5Update + 
									MCTimeCalcGoal6Update + 
									MCTimeTerrainGeometry +
									MCTimeInterfaceUpdate +
									MCTimeCraterUpdate +
									MCTimeTXMManagerUpdate +
									MCTimeSensorUpdate +
									MCTimeLOSUpdate +
									MCTimeCollisionUpdate +
									MCTimeMissionScript+ 
									MCTimeTerrainObjectsUpdate +
									MCTimeMechsUpdate +
									MCTimeVehiclesUpdate +
									MCTimeTurretsUpdate +
									MCTimeAllElseUpdate;
#endif
	}

	return scenarioResult;
}

//----------------------------------------------------------------------------------

long Mission::getStatus (void) {

	if (terminationCounterStarted)
		return(terminationResult);
	return(mis_PLAYING);
}

//----------------------------------------------------------------------------------

long Mission::render (void)
{
	ZoneScopedN("Camera.UpdateRenderers Mission.render");
	if (active)
	{
		unsigned char tempAmbientLight[3];
		if (KillAmbientLight) {
			tempAmbientLight[0] = eye->ambientRed;
			tempAmbientLight[1] = eye->ambientGreen;
			tempAmbientLight[2] = eye->ambientBlue;
			eye->ambientRed = 0xFF;
			eye->ambientGreen = 0xFF;
			eye->ambientBlue = 0xFF;
		}
		{ ZoneScopedN("Camera.UpdateRenderers eye.render"); eye->render(); }
		{ ZoneScopedN("Camera.UpdateRenderers clearCellDebugs"); GameMap->clearCellDebugs(0); }

		//-----------------------------------------------------
		// FOG time.  Set Render state to FOG on!
		DWORD fogColor = eye->fogColor;
		if (useFog)
		{
			gos_SetRenderState( gos_State_Fog, fogColor);
		}
		else
		{
			gos_SetRenderState( gos_State_Fog, 0);
		}

		gos_SetRenderState( gos_State_Fog, 0);

		{ ZoneScopedN("Camera.UpdateRenderers FloatHelp.renderAll"); FloatHelp::renderAll(); }

		currentFloatHelp = 0;

		if (missionInterface)
		{
			ZoneScopedN("Camera.UpdateRenderers missionInterface.render");
			missionInterface->render();
		}

		if (KillAmbientLight) {
			eye->ambientRed = tempAmbientLight[0];
			eye->ambientGreen = tempAmbientLight[1];
			eye->ambientBlue = tempAmbientLight[2];
		}

		//reset the TGL RAM pools.
		{
			ZoneScopedN("Camera.UpdateRenderers tglPools.reset");
			colorPool->reset();
			vertexPool->reset();
			facePool->reset();
			shadowPool->reset();
			trianglePool->reset();
		}
	}

	return scenarioResult;
}

//----------------------------------------------------------------------------------
float applyDifficultySkill (float chance, bool isPlayer)
{
	if (isPlayer)
	{
		switch (GameDifficulty)
		{
			case 0:			//Easy
			case 1:			//Medium
			case 2:			//Hard
			case 3:			//Really Hard
				chance *= (float(globalPlayerSkills[GameDifficulty]) / 100.0);
				return(chance);
				break;

			default:
				return (chance);
				break;
		}
	}
	else
	{
		switch (GameDifficulty)
		{
			case 0:			//Easy
			case 1:			//Medium
			case 2:			//Hard
			case 3:			//Really Hard
				chance *= (float(globalEnemySkills[GameDifficulty]) / 100.0);
				return(chance);
				break;

			default:
				return(chance);
				break;
		}
	}
}

//----------------------------------------------------------------------------------
float applyDifficultyWeapon (float dmg, bool isPlayer)
{
	if (isPlayer)
	{
		switch (GameDifficulty)
		{
			case 0:			//Easy
			case 1:			//Medium -- NO CHANGE!
			case 2:			//Hard
			case 3:			//Very Hard
				dmg *= (float(globalPlayerWeapons[GameDifficulty]) / 100.0);
				break;
				
			default:
				break;
		}
	}
	else
	{
		switch (GameDifficulty)
		{
			case 0:			//Easy
			case 1:			//Medium -- NO CHANGE!
			case 2:			//Hard
			case 3:			//Very Hard
				dmg *= (float(globalEnemyWeapons[GameDifficulty]) / 100.0);
				break;
				
			default:
				break;
		}
	}

	//------------------------------------------------------------------
	// Must keep damage an integral part of .25 or bad things happen
	// In theory only needed for Multiplayer but better safe then...
	if (((float)((long)(dmg / 0.25)) * 0.25) != dmg)
	{
		dmg = ((float)((long)(dmg / 0.25)) * 0.25);
	}

	if (dmg < 0.0)
		dmg = 0.0;

	if (dmg > 255.0)
		dmg = 255.0;
		
	return dmg;
}

//----------------------------------------------------------------------------------
void InitDifficultySettings (FitIniFile *gameSystemFile)
{
	long result = gameSystemFile->seekBlock("DifficultySettings");
	gosASSERT(result == NO_ERR);
	
	result = gameSystemFile->readIdLongArray("PlayerSkills", globalPlayerSkills,4);
	gosASSERT(result == NO_ERR);
	
	result = gameSystemFile->readIdLongArray("EnemySkills",globalEnemySkills,4);
	gosASSERT(result == NO_ERR);
	
	result = gameSystemFile->readIdLongArray("PlayerWeapons",globalPlayerWeapons,4);
	gosASSERT(result == NO_ERR);
	
	result = gameSystemFile->readIdLongArray("EnemyWeapons",globalEnemyWeapons,4);
	gosASSERT(result == NO_ERR);
}	

//--------------------------------------------------
// Game System Constants -- Definitions here.
float maxVisualRange = 0.0;
unsigned long MaxTreeLOSCellBlock = 0;
float MaxVisualRadius = 0.0;
float fireVisualRange = 0.0;
extern float WeaponRange[];						//MOVER
extern float DefaultAttackRange;				//MOVER
float baseSensorRange = 0.0;
extern long visualRangeTable[];

extern unsigned char godMode;		//Can I simply see everything, enemy and friendly?
unsigned char revealTacMap = 0;		//Is Tacmap always revealed?
unsigned char footPrints = 0;		//Do we use/draw footprints?
bool CantTouchThis = false;

long tonnageDivisor = 1.0;			//Amount we divide tons under drop weight by to calc resource bonus
long resourcesPerTonDivided = 0;	//Tons per above divided number

float MineDamage = 0.0;			//Damage mine does per tile
float MineSplashDamage = 0.0;	//Damage mine explosion does to all in tile.
float MineSplashRange = 0.0;	//Range of Splash Damage
long MineExplosion = -1;		//Object ID of mine explosion
long MineLayThrottle = 0;		//Throttle setting of mine Layer
long MineSweepThrottle = 0;		//Throttle setting of mine sweeper
float StrikeWaitTime = 0.0f;	//Time between aerospace Spotter strike calls
float StrikeTimeToImpact = 0.0f;//Time it takes air strike to come in.
float MineWaitTime = 0.0;		//Time Mine Layer must sit in Tile before mine is laid.

long totalSmokeSpheres = 0;			//Maximum number of smoke spheres in world.
long totalSmokeShapeSize = 0;		//Amount we allocate to smoke heap for spheres/textures.

long maxFiresBurning = 0;			//Maximum number of fires allowed in world.
float maxFireBurnTime = 0.0;		//Maximum time a fire can be burning before it dies out.

bool force64MB = TRUE;				//Do we have between 64 and 32 Mb of memory?
bool force32Mb = FALSE;				//Do we have 32Mb of memory only?

float InfluenceRadius = -1.0;		//Capture Radius
float InfluenceTime = 0.0;			//Time inside to Capture

//----------------------------------------------------------------------------------

void Mission::createPartObject (long partIndex, MoverPtr mover) {

	//-----------------------------------
	// All parts are movers in this game!
	
	//------------------------------------------------------------------
	// Create the object -- If object is Not DESTROYED Already OR
	// it doesn't exist yet.  (parts[i].object = NULL
	//
	// This could NEVER have worked.  Destroyeds still need to be created.  Otherwise,
	// why is there code inside this IF which checks if destroyed?  I blame Glenn's Crazy Bracing Style!
	// Create the part just like always, just start it disabled.
	// 
	// If the mech is not on OUR team, you can recover it during the mission.
	// If the mech IS on our team, it starts shutdown and NOT added to Heidi's interface.
	// When we get close enough to it, it adds itself to Heidi's world and we can command it!
	if (parts[partIndex].objectWID == 0)
	{
		ObjectTypePtr objType = ObjectManager->getObjectType(parts[partIndex].objNumber);
		Assert(objType != NULL, partIndex, " Mission.createPartObject: unable to get objType ");
		// Mod-tolerance: if objType is a stub (no appearance name) but the
		// mission provided a CSVFile, back-fill it so BattleMech::init can
		// open the chassis .csv. Without this, stub-only mechs crash at
		// strlen(NULL) inside FullPathFileName::init(csvName).
		if (objType && !objType->getAppearanceTypeName() && parts[partIndex].csvFile[0]) {
			objType->setAppearanceTypeName(parts[partIndex].csvFile);
		}
		parts[partIndex].objectWID = mover->getWatchID();
		mover->init(true, objType);
		mover->setAwake(parts[partIndex].active ? true : false);
		mover->setHandle(ObjectManager->getHandle(mover));

		//----------------------------------------------
		// Load the profile data into the game object...
		// This is MONDO DIFFERENT NOW.
		// Profiles are in the mech's CSV file.  You should just init with mover with
		// the variant number and the rest is done in the mover itself.
		//
		// Heidi, this is what needs to change in the mission.fit files!
		// -fs 12/7/99		A date which will live in Infamy.
		// Must also load Ground Vehicles the OLDen WAY until further notice.
		switch (mover->getObjectClass()) 
		{
			case BATTLEMECH: 
				mover->init(parts[partIndex].variantNum);		
				break;

			case GROUNDVEHICLE:
			case ELEMENTAL:
			{
				if (ObjectTypeManager::objectFile->seekPacket(parts[partIndex].objNumber) == NO_ERR) 
				{
					//--------------------------------------------------------
					// All new code here.  This will ask the objectType it is
					// loading what kind of objectType it is and create it
					// based on that instead of objTypeNum.
					FitIniFile profileFile;
					long result = profileFile.open(ObjectTypeManager::objectFile,ObjectTypeManager::objectFile->getPacketSize());
					if (result != NO_ERR)
						STOP((" Mission.createPartObject: can't open file "));

					if (result == NO_ERR) 
					{
						//-------------------------------------------
						result = mover->init(&profileFile);
						if (result != NO_ERR)
							STOP((" Mission.createPartObject: bad Profile File "));
					}
					
					profileFile.close();
				}
			}
			break;
		}

		mover->setPilotHandle(parts[partIndex].pilot);
		mover->setTeamId(parts[partIndex].teamId, true);
		mover->setSquadId(parts[partIndex].squadId);
		mover->calcWeaponEffectiveness(TRUE);
		mover->calcWeaponEffectiveness(FALSE);
		
		if (parts[partIndex].controlType == CONTROL_PLAYER)
			mover->setControl(CONTROL_PLAYER);
		else if (MPlayer)
			mover->setControl(MPlayer->isServer() ? CONTROL_AI : CONTROL_NET);
		else
			mover->setControl(CONTROL_AI);

		mover->setPosition(parts[partIndex].position);
		mover->setLastValidPosition(parts[partIndex].position);
		mover->setRotation(parts[partIndex].rotation);
		mover->setCommanderId(parts[partIndex].commanderID);

		switch (mover->getObjectClass()) {
			case BATTLEMECH: {
				((BattleMechPtr)mover)->captureable = parts[partIndex].captureable;

				//-----------------------------------------
				// Make sure the pilot paints his Mech!
				// Use editor colors UNLESS this is on Commander::home
				AppearancePtr myActor = mover->getAppearance();

				if (MPlayer)
					myActor->resetPaintScheme( MPlayer->colors[MPlayer->playerInfo[mover->commanderId].stripeColor],
											   MPlayer->colors[MPlayer->playerInfo[mover->commanderId].stripeColor],
											   MPlayer->colors[MPlayer->playerInfo[mover->commanderId].baseColor[BASECOLOR_TEAM]]);
				else {
					if (mover->getCommanderId() == Commander::home->getId())
						myActor->resetPaintScheme(prefs.highlightColor, prefs.highlightColor, prefs.baseColor);
					else
						myActor->resetPaintScheme(parts[partIndex].highlightColor1,
												parts[partIndex].highlightColor2,
												parts[partIndex].baseColor);
					//myActor->setGesture(parts[partIndex].gestureId);  DOn't do this unless you want the mechs to start shutdown and power up!
				}
				}
				break;
			case GROUNDVEHICLE: {
				AppearancePtr myActor = mover->getAppearance();
				if (MPlayer)
					myActor->resetPaintScheme(MPlayer->colors[MPlayer->playerInfo[mover->commanderId].stripeColor],
											  MPlayer->colors[MPlayer->playerInfo[mover->commanderId].stripeColor],
											  MPlayer->colors[MPlayer->playerInfo[mover->commanderId].baseColor[BASECOLOR_TEAM]]);
				else {
					if (mover->getCommanderId() == Commander::home->getId())
						myActor->resetPaintScheme(prefs.highlightColor, prefs.highlightColor, prefs.baseColor);
					else
						myActor->resetPaintScheme(parts[partIndex].highlightColor1,
												  parts[partIndex].highlightColor2,
												  parts[partIndex].baseColor);
					//myActor->setGesture(parts[partIndex].gestureId);  DOn't do this unless you want the mechs to start shutdown and power up!
				}
				}
				break;
			case ELEMENTAL:
				break;
		}

		//-----------------------------------------------------------------
		// Set object Unique ID to be Part Number
		//parts[i].object->setIdNumber(i);

		if (MPlayer) {
			MPlayer->addToMoverRoster((MoverPtr)ObjectManager->getByWatchID(parts[partIndex].objectWID));
			MPlayer->addToPlayerMoverRoster(parts[partIndex].commanderID, (MoverPtr)ObjectManager->getByWatchID(parts[partIndex].objectWID));
			if (parts[partIndex].commanderID == MPlayer->commanderID)
				MPlayer->addToLocalMovers((MoverPtr)ObjectManager->getByWatchID(parts[partIndex].objectWID));
		}

		if (parts[partIndex].exists)
		{
			mover->setExists(true);
		}
		else
			MC2_DESTROY(mover, "mission_load_inactive");

		if (parts[partIndex].destroyed) 
		{
			if (mover->getCommanderId() == Commander::home->getId())
			{
				mover->setStatus(OBJECT_STATUS_SHUTDOWN);
				mover->startDisabled = false;
				mover->setOnGUI(false);		//Even for non-player Team Parts.  It only FALSE if we link up with it later!!!!!
			}
			else
			{
				mover->setStatus(OBJECT_STATUS_DISABLED);
				mover->disable( UNDETERMINED_DEATH );
				mover->startDisabled = true;
				if (mover->sensorSystem)
					mover->sensorSystem->disable();
				mover->setOnGUI(true);		//Even for non-player Team Parts.  It only FALSE if we link up with it later!!!!!
			}
		}
		else
		{
			mover->setOnGUI(true);		//Even for non-player Team Parts.  It only FALSE if we link up with it later!!!!!
		}
	}
}

//---------------------------------------------------------------------------
long Mission::addMover (MoverInitData* moverSpec) {
	
	//--------------------------------------
	// Load the mechwarrior into the mech...
	MechWarriorPtr pilot = MechWarrior::newWarrior();
	if (!pilot)
		STOP(("Too many pilots in this mission!"));

	FullPathFileName pilotFullFileName;
	pilotFullFileName.init(warriorPath, moverSpec->pilotFileName, ".fit");
			
	FitIniFile* pilotFile = new FitIniFile;
	gosASSERT(pilotFile != NULL);
		
	long result = pilotFile->open(pilotFullFileName);
	gosASSERT(result == NO_ERR);
	result = pilot->init(pilotFile);
	gosASSERT(result == NO_ERR);

	pilotFile->close();
	delete pilotFile;
	pilotFile = NULL;

	//Copy logistics data to pilot AFTER loading old data.
	// ONLY if we overrode the data in logistics!!
	if (moverSpec->overrideLoadedPilot)
	{
		pilot->skills[MWS_GUNNERY] = pilot->skillRank[MWS_GUNNERY] = moverSpec->gunnerySkill;
		pilot->skills[MWS_PILOTING] = pilot->skillRank[MWS_PILOTING] = moverSpec->pilotingSkill;
		memcpy(pilot->specialtySkills,moverSpec->specialtySkills,sizeof(bool) * NUM_SPECIALTY_SKILLS);
		pilot->calcRank();
	}

	//*********************
	// NOTE: Need to send packet to other players in MP with new pilot and
	// mover data!
	//*********************

	long numErrors, numLinesProcessed;
	FullPathFileName brainFullFileName;
	if (MPlayer) {
		pilot->setBrainName("pbrain");
		brainFullFileName.init(warriorPath, "pbrain", ".abl");
		}
	else {
		pilot->setBrainName(moverSpec->brainFileName);
		brainFullFileName.init(warriorPath, moverSpec->brainFileName, ".abl");
	}
		
	long moduleHandle = ABLi_preProcess(brainFullFileName, &numErrors, &numLinesProcessed);
	gosASSERT(moduleHandle >= 0);
	pilot->setBrain(moduleHandle);

/*
typedef struct _MoverInitData {
	char			pilotFileName[50];
	char			brainFileName[50];
	char			profileName[50];
	long			objNumber;
	unsigned long	controlType;
	unsigned long	controlDataType;
	unsigned long	variant;
	float			position[2];
	long			rotation;
	char			teamID;
	char			commanderID;
	long			paintScheme;
	bool			active;
	bool			exists;
	char			icon;
	bool			capturable;
} MoverInitData;
*/
			//parts[i].gestureId = 2; // this has never changed
	ObjectTypePtr objType = ObjectManager->loadObjectType(moverSpec->objNumber);
	if (!objType)
		objType = ObjectManager->getObjectType(moverSpec->objNumber);
	if (objType) {
		MoverPtr mover = NULL;
		switch (objType->getObjectTypeClass()) {
			case BATTLEMECH_TYPE:
				mover = (MoverPtr)ObjectManager->newMech();
				break;
			case VEHICLE_TYPE:
				mover = (MoverPtr)ObjectManager->newVehicle();
				break;
		}

		if (mover) 
		{
			mover->init(true, objType);
			mover->setAwake(moverSpec->active);
			mover->setHandle(ObjectManager->getHandle(mover));

			//----------------------------------------------
			// Load the profile data into the game object...
			// This is MONDO DIFFERENT NOW.
			// Profiles are in the mech's CSV file.  You should just init with mover with
			// the variant number and the rest is done in the mover itself.
			//
			// Heidi, this is what needs to change in the mission.fit files!
			// -fs 12/7/99		A date which will live in Infamy.
			// Must also load Ground Vehicles the OLDen WAY until further notice.
			switch (mover->getObjectClass()) {
				case BATTLEMECH: 
					mover->init(moverSpec->variant);		
					break;
				case GROUNDVEHICLE:
				case ELEMENTAL:	
				{
					if (ObjectTypeManager::objectFile->seekPacket(moverSpec->objNumber) == NO_ERR) 
					{
						//--------------------------------------------------------
						// All new code here.  This will ask the objectType it is
						// loading what kind of objectType it is and create it
						// based on that instead of objTypeNum.
						FitIniFile profileFile;
						long result = profileFile.open(ObjectTypeManager::objectFile,ObjectTypeManager::objectFile->getPacketSize());
						if (result != NO_ERR)
							STOP((" Mission.createPartObject: can't open file "));
	
						if (result == NO_ERR) 
						{
							//-------------------------------------------
							result = mover->init(&profileFile);
							if (result != NO_ERR)
								STOP((" Mission.createPartObject: bad Profile File "));
						}
						
						profileFile.close();
					}
				}
				break;
			}

			mover->setPilotHandle(pilot->getIndex());
			mover->setTeamId(moverSpec->teamID, true);
			mover->calcWeaponEffectiveness(true);
			mover->calcWeaponEffectiveness(false);
		
			if (moverSpec->controlType == CONTROL_PLAYER)
				mover->setControl(CONTROL_PLAYER);
			else if (MPlayer)
				mover->setControl(MPlayer->isServer() ? CONTROL_AI : CONTROL_NET);
			else
				mover->setControl(CONTROL_AI);

			mover->setPosition(moverSpec->position);
			mover->setLastValidPosition(moverSpec->position);
			mover->setRotation(moverSpec->rotation);
			mover->setCommanderId(moverSpec->commanderID);

			switch (mover->getObjectClass()) {
				case BATTLEMECH: {
					((BattleMechPtr)mover)->captureable = moverSpec->capturable;
					//-----------------------------------------
					// Make sure the pilot paints his Mech!!			
					AppearancePtr myActor = mover->getAppearance();
					myActor->resetPaintScheme(moverSpec->highlightColor1,
											moverSpec->highlightColor2,
											moverSpec->baseColor);
					//myActor->setGesture(moverSpec->gestureID);  Don't do this unless you want things to start shutdown and power up!
					}
					break;
				case GROUNDVEHICLE:	{
					AppearancePtr myActor = mover->getAppearance();
					myActor->resetPaintScheme(moverSpec->highlightColor1,
											moverSpec->highlightColor2,
											moverSpec->baseColor);
					}
					break;
				case ELEMENTAL:
					break;
			}

			//-----------------------------------------------------------------
			// Set object Unique ID to be Part Number
			//parts[i].object->setIdNumber(i);

			if (MPlayer) {
				MPlayer->addToMoverRoster(mover);
				MPlayer->addToPlayerMoverRoster(moverSpec->commanderID, mover);
				if (moverSpec->commanderID == MPlayer->commanderID)
					MPlayer->addToLocalMovers(mover);
			}

			if (moverSpec->exists)
				mover->setExists(true);
			else
				MC2_DESTROY(mover, "mission_load_inactive");

			//---------------------------------------------------------------------------------
			// If we're not playing multiplayer, make sure all home commander movers have their
			// localMoverId set to 0, so the iface can at least check if a mover is player
			// controlled...
			if (!MPlayer && (Commander::home->getId() == moverSpec->commanderID))
				mover->setLocalMoverId(0);

			ObjectManager->modifyMoverLists(mover, MOVERLIST_ADD);
			Team::teams[moverSpec->teamID]->addToRoster(mover);
			missionInterface->addMover(mover);

			for (long i = 0; i < ObjectManager->getNumMovers(); i++) {
				MoverPtr curMover = ObjectManager->getMover(i);
				if (!curMover->isDisabled()) {
					if (curMover->sensorSystem)
						curMover->sensorSystem->scanMover(mover);
					if ( curMover->getPilot() )
						curMover->getPilot()->triggerAlarm(PILOT_ALARM_NEW_MOVER, mover->getWatchID());
				}
			}
	
			if (moverSpec->numComponents > 0) {
				if (mover->getObjectClass() != BATTLEMECH)
					STOP(("LogisticsMech was not a MECH!!"));
				((BattleMechPtr)mover)->resetComponents(moverSpec->numComponents, moverSpec->components);
			}	

            // sebi: !NB
            // set it so that object wil be drawn!!!
            // currently seems that newly created gvehicl wilnot be drawn because isOnGui set to false
            // (actually it was not initialized at all until I added it to GroundVehicle::init)
            // and if isOnGui == false, then alphaValue will be set to 0x00 (see gvehicl.cpp) and renderer will not render it because
            // there is no case in MC_TextureManager::renderLists() for only MC2_DRAWALPHA flag
            // how it works on Windows... who knows.
			mover->setOnGUI(true);

			return(mover->getHandle());
		}
	}
	else
		return -1;

	return(0);
}

//---------------------------------------------------------------------------

long Mission::addMover (MoverInitData* moveSpec, LogisticsMech* mechData)
{
	//OK, whole new ballgame here.
	// First, convert Heidi data to MoverInitData.
	// Then, create Mech using this data.
	// Then, Replace components to match logisticsMech Components.
	// Then, it should be good to go.
  
	MoverInitData mData;
	memset(&mData,0,sizeof(MoverInitData));
	
	strncpy(mData.pilotFileName,mechData->getPilot()->getFileName(),49);
	strcpy(mData.brainFileName,"pbrain");
	strncpy(mData.csvFileName,mechData->getFileName(),50);
	
	mData.rosterIndex = 255;
	mData.objNumber = mechData->getFitID();
	mData.controlType = moveSpec->controlType;
	mData.controlDataType = moveSpec->controlDataType;
	mData.variant = moveSpec->variant;
	mData.position.x = moveSpec->position.x;
	mData.position.y = moveSpec->position.y; 
	mData.position.z = moveSpec->position.z; 
	mData.rotation = moveSpec->rotation;
	mData.teamID = moveSpec->teamID;
	mData.commanderID = moveSpec->commanderID;
	mData.baseColor = moveSpec->baseColor;
	mData.highlightColor1 = moveSpec->highlightColor1;
	mData.highlightColor2 = moveSpec->highlightColor2;
	mData.gestureID = 2;
	mData.active = true;
	mData.exists = true;
	mData.icon = 0;
	mData.capturable = false;
	mData.overrideLoadedPilot = moveSpec->overrideLoadedPilot;
	mData.gunnerySkill = moveSpec->gunnerySkill;
	mData.pilotingSkill = moveSpec->pilotingSkill;
	memcpy(mData.specialtySkills,moveSpec->specialtySkills,sizeof(bool) * NUM_SPECIALTY_SKILLS);

	long moverHandle = addMover(&mData);

	//Now take the moverHandle and change the component data to match Heidi's passed logisticsMech.
	MoverPtr mMech = (MoverPtr)ObjectManager->get(moverHandle);
	// BATTLEMECH-RESET-KIND-GUARD-1: the customization below (variantName strcpy +
	// resetComponents) is BattleMech-ONLY. A non-BattleMech deployable
	// (GroundVehicle / Elemental — valid MC2X content, e.g. a salvaged Repair Truck
	// fielded via the cheat-mode lance) was only soft-STOP'd here and fell through to
	// a BattleMech downcast: variantName written to the wrong offset, then
	// resetComponents -> calcMaxTargetDamage -> null call / crash. The mover is
	// already fully created+initialised by addMover() above (vehicles via CSV), so
	// return its handle and skip the mech-only step. Uses the runtime object-class
	// discriminator (NOT name/weight). Does not strip or reject the vehicle.
	if (mMech->getObjectClass() != BATTLEMECH)
	{
		static const bool s_deployKindTrace = (getenv("MC2_DEPLOY_KIND_TRACE") != nullptr);
		if (s_deployKindTrace)
		{
			printf("[DEPLOY] non-BattleMech deployable class=%d fielded without mech reset (file=%s)\n",
				(int)mMech->getObjectClass(), mechData ? mechData->getFileName() : "?");
			fflush(stdout);
		}
		return moverHandle;
	}

	strcpy( ((BattleMech*)mMech)->variantName, mechData->getName() );
		
	long totalComponents = mechData->getComponentCount();

	long *componentList = NULL;
	
	if ( totalComponents )
	{
		componentList = (long *)systemHeap->Malloc(sizeof(long) * totalComponents);
	
		long otherCount = totalComponents;
		mechData->getComponents(otherCount, componentList);

		if (otherCount != totalComponents)
			STOP(("Heidi's getComponentCount does not agree with count returned from getComponents"));

	}
	
	
	((BattleMechPtr)mMech)->resetComponents(totalComponents,componentList);
	
	return moverHandle;
}

//---------------------------------------------------------------------------

void DEBUGWINS_removeGameObject (GameObjectPtr obj);

long Mission::removeMover (MoverPtr mover) {

	DEBUGWINS_removeGameObject((GameObjectPtr)mover);
	missionInterface->removeMover(mover);
	if (MPlayer) {
		MPlayer->removeFromLocalMovers(mover);
		MPlayer->removeFromMoverRoster(mover);
	}
	ObjectManager->freeMover(mover);
	return(0);
}

//---------------------------------------------------------------------------

void Mission::tradeMover (MoverPtr mover, long newTeamID, long newCommanderID, char* pilotFileName, const char* brainFileName) {

	missionInterface->removeMover(mover);
	if (MPlayer) {
		MPlayer->removeFromLocalMovers(mover);
		MPlayer->removeFromPlayerMoverRoster(mover);
	}
	if (newCommanderID > -1) 
	{
		char realPilotName[256];
		strcpy(realPilotName,&(pilotFileName[MPlayer ? 4 : 3]));
		LogisticsPilot *lPilot = LogisticsData::instance->getPilot(realPilotName);
		mover->loadPilot(pilotFileName, brainFileName, lPilot);
	}
	ObjectManager->tradeMover(mover, newTeamID, newCommanderID);
	if (mover->sensorSystem)
		mover->sensorSystem->broken = false;
	if (MPlayer && (newCommanderID > -1)) {
		MPlayer->addToPlayerMoverRoster(newCommanderID, mover);
		if (newCommanderID == MPlayer->commanderID)
			MPlayer->addToLocalMovers(mover);
	}

	if (newCommanderID > -1) 
	{
		missionInterface->addMover(mover);
	}
}

//----------------------------------------------------------------------------

bool Mission::calcComplexDropZones (char* missionName, char dropZoneCID[MAX_MC_PLAYERS]) {

	for (long p = 0; p < MAX_MC_PLAYERS; p++)
		dropZoneCID[p] = -1;

	FullPathFileName missionFileName;
	missionFileName.init(missionPath,missionName,".fit");

	FitIniFile* missionFile = new FitIniFile;
	gosASSERT(missionFile != NULL);
	
	long result = missionFile->open(missionFileName);
	if (result != NO_ERR)
		STOP(("Unable to open Mission File %s",missionFileName));

	//------------------------------------------------------------
	// First, let's see how many teams and commanders there are...
	/*long maxTeamID = -1;
	long maxCommanderID = -1;
	for (long i = 0; i < MAX_MC_PLAYERS; i++) {
		if (MPlayer->playerInfo[i].team > maxTeamID)
			maxTeamID = MPlayer->playerInfo[i].team;
		if (MPlayer->playerInfo[i].commanderID > maxCommanderID)
			maxCommanderID = MPlayer->playerInfo[i].commanderID;
	}
	*/
	long dropZoneSetup[MAX_MC_PLAYERS] = {-1, -1, -1, -1, -1 ,-1 ,-1 ,-1};
	result = missionFile->seekBlock("Parts");
	gosASSERT(result == NO_ERR);
	result = missionFile->readIdULong("NumParts",numParts);
	gosASSERT(result == NO_ERR);
	if (numParts)
		for (long i = 1; i < long(numParts + 1); i++) {
			char partName[12];
			sprintf(partName,"Part%d",i);
			
			//------------------------------------------------------------------
			// Find the object to load
			result = missionFile->seekBlock(partName);
			gosASSERT(result == NO_ERR);

			char teamID = -1;
			result = missionFile->readIdChar("TeamId", teamID);
			gosASSERT(result == NO_ERR);
			if ((teamID < 0) || (teamID >= MAX_TEAMS))
				STOP(("Mission.calcComplexDropZones: bad teamID"));

			char commanderID = -1;
			result = missionFile->readIdChar("CommanderId", commanderID);
			if (result != NO_ERR) {
				long cID;
				result = missionFile->readIdLong("CommanderId", cID);
				gosASSERT(result == NO_ERR);
				commanderID = (char)cID;
			}

			if ((commanderID < 0) || (commanderID >= MAX_MC_PLAYERS))
				STOP(("Mission.calcComplexDropZones: bad commanderID"));
			dropZoneSetup[commanderID] = teamID;
	}

	//------------------------------------------------------------------------------------
	// First, let's confirm that we have the correct player/team setup for this mission...
	long teamSize[MAX_TEAMS][2];
	for (long i = 0; i < MAX_TEAMS; i++) {
		teamSize[i][0] = 0;
		teamSize[i][1] = 0;
	}
/*	for (i = 0; i < MAX_MC_PLAYERS; i++)
		if (dropZoneSetup[i] > -1)
			teamSize[dropZoneSetup[i]][0]++;
	for (i = 0; i < MAX_MC_PLAYERS; i++)
		if (MPlayer->playerInfo[i].commanderID > -1)
			teamSize[MPlayer->playerInfo[i].team][1]++;
	for (i = 0; i < MAX_MC_PLAYERS; i++)
		if (teamSize[i][0] != teamSize[i][1])
			return(false);
*/
	//---------------------------------------------------------
	// We know everything's good, so hand out the drop zones...
	for (int i = 0; i < MAX_MC_PLAYERS; i++) {
		long index = RandomNumber(teamSize[MPlayer->playerInfo[i].team][0]--);
		for (long j = 0; j < MAX_MC_PLAYERS; j++)
			if (dropZoneSetup[j] == MPlayer->playerInfo[i].team) {
				if (index == 0) {
					dropZoneSetup[j] = -1;
					dropZoneCID[j] = i;
					break;
				}
				index--;
			}

	}

	missionFile->close();
	delete missionFile;
	missionFile = NULL;


	return(true);
}

//----------------------------------------------------------------------------

bool IsGateDisabled (int objectWID) {

	//--------------------------------------------------------------
	// Actually looks at the object and its parent, if it has one...
	if (objectWID < 1)
		return(false);
	GameObjectPtr obj = ObjectManager->getByWatchID(objectWID);
	if (obj && obj->isDisabled())
		return(true);
	if (obj->isBuilding()) {
		GameObjectPtr parent = ((BuildingPtr)obj)->getParent();
		if (parent)
			return(parent->isDisabled());
	}
	return(false);
}

//----------------------------------------------------------------------------

bool IsGateOpen (int objectWID) {

	//--------------------------------------------------------------
	// Actually looks at the object and its parent, if it has one...
	if (!objectWID)
		return(false);
	GameObjectPtr obj = ObjectManager->getByWatchID(objectWID);
	if (!obj)
		return(false);
	if (obj->getObjectClass() == GATE)
		return(((Gate*)obj)->opened);
	return(false);
}

//----------------------------------------------------------------------------
void Mission::init (const char *missionName, long loadType, long dropZoneID, Stuff::Vector3D* dropZoneList, char commandersToLoad[8][3], long numMoversPerCommander)
{
	extern int g_lightProbeSetupPath; g_lightProbeSetupPath = 1; // [GPUPROPS v1]
	ZoneScopedN("Mission::init");
	mission_phase_begin();
	// F3 CPU projection cost-baseline: flush previous-mission samples and
	// clear ring buffer. No-op when env OFF.
	::mc2_cpu_proj_cost::hard_reset("Mission::init");

	// Reset GPU static-prop batcher state at every map boundary, before any
	// actor registerType() calls happen during actor spawn (Task 6).
	// M1 Task 13 (order-correct split): call order is byte-identical to original:
	//   1. GpuStaticPropBatcher::onMapLoad()   ← beginMissionEarly()
	//   2. GpuMechBatcher::onMapLoad()
	//   3. setMissionForIbl() + Registry::init() + RenderWorld::init() ← beginMissionLate()
	//   4. Mech::beginMission()
	GameAdapters::StaticProp::beginMissionEarly();         // M1 Task 13
	GpuMechBatcher::instance().onMapLoad();
	GameAdapters::StaticProp::beginMissionLate(missionName); // M1 Task 13
	GameAdapters::Mech::beginMission();              // M2: mech lifecycle

	// DETERMINISTIC-RNG-1: per-mission reseed hook. ONLY under MC2_DETERMINISTIC_RNG.
	// Seed = hash(missionName), overridable with MC2_RNG_SEED for manual pinning.
	// Gate OFF: this block is a no-op (no gos_srand call) => OFF byte-identical.
	{
		const char* detEnv = getenv("MC2_DETERMINISTIC_RNG");
		const bool detRng = (detEnv && detEnv[0] != '\0' && detEnv[0] != '0');
		if (detRng) {
			uint32_t seed = mc2_det_rng::hashMissionName(missionName);
			const char* seedEnv = getenv("MC2_RNG_SEED");
			if (seedEnv && seedEnv[0] != '\0')
				seed = (uint32_t)strtoul(seedEnv, nullptr, 0);
			gos_srand(seed);
			std::fprintf(stderr, "[DET_RNG] mission=%s reseed=0x%08X (MC2_DETERMINISTIC_RNG=1)\n",
			             missionName ? missionName : "(null)", seed);
		}
	}

	neverEndingStory = false;
	invulnerableON = false;

	terminationResult = mis_PLAYING; 

	//Start finding the Leaks
	//systemHeap->startHeapMallocLog();
	//systemHeap->dumpRecordLog();

	loadProgress = 0.0f;

	loadProgress = 1.0f;

	if ((loadType == MISSION_LOAD_SP_QUICKSTART) || (loadType == MISSION_LOAD_SP_LOGISTICS)) {
		char teamRelationsForSP[MAX_TEAMS][MAX_TEAMS] = {
			{0, 2, 1, 2, 2, 2, 2, 2},
			{2, 0, 2, 2, 2, 2, 2, 2},
			{1, 2, 0, 2, 2, 2, 2, 2},
			{2, 2, 2, 0, 2, 2, 2, 2},
			{2, 2, 2, 2, 0, 2, 2, 2},
			{2, 2, 2, 2, 2, 0, 2, 2},
			{2, 2, 2, 2, 2, 2, 0, 2},
			{2, 2, 2, 2, 2, 2, 2, 0}
		};
		for (long i = 0; i < MAX_TEAMS; i++)
			for (long j = 0; j < MAX_TEAMS; j++) {
				Team::relations[i][j] = teamRelationsForSP[i][j];
				TeamRelations[i][j] = teamRelationsForSP[i][j];
			}
		}
	else {
		char teamRelationsForMP[MAX_TEAMS][MAX_TEAMS] = {
			{0, 2, 2, 2, 2, 2, 2, 2},
			{2, 0, 2, 2, 2, 2, 2, 2},
			{2, 2, 0, 2, 2, 2, 2, 2},
			{2, 2, 2, 0, 2, 2, 2, 2},
			{2, 2, 2, 2, 0, 2, 2, 2},
			{2, 2, 2, 2, 2, 0, 2, 2},
			{2, 2, 2, 2, 2, 2, 0, 2},
			{2, 2, 2, 2, 2, 2, 2, 0}
		};
		for (long i = 0; i < MAX_TEAMS; i++)
			for (long j = 0; j < MAX_TEAMS; j++) {
				Team::relations[i][j] = teamRelationsForMP[i][j];
				TeamRelations[i][j] = teamRelationsForMP[i][j];
			}
	}
				
	//-------------------------------------------
	// Always reset turn at scenario start
	turn = 0;
	terminationCounterStarted = 0;

	#ifdef LAB_ONLY
	x=GetCycles();
	#endif

	//-----------------------
	// Init the ABL system...
	initABL();

#ifdef LAB_ONLY
	x1=GetCycles();
	MCTimeABLLoad=x1-x;
#endif

	{ ZoneScopedN("Mission::init initBareMinimum"); initBareMinimum(); }
	loadProgress = 4.0f;
	{ ZoneScopedN("Mission::init initTGLForMission"); initTGLForMission(); }
	
	//--------------------------------------------------------------
	// Start the Mission Heap
	missionHeap = new UserHeap;
	gosASSERT(missionHeap != NULL);
	
	missionHeap->init(missionHeapSize,"MISSION");
	
	//--------------------------
	// Load Game System stuff...
	FullPathFileName fullGameSystemName;
	fullGameSystemName.init(missionPath, "gamesys", ".fit");
	
	FitIniFile* gameSystemFile = new FitIniFile;
	gosASSERT(gameSystemFile != NULL);
		
	long result = gameSystemFile->open(fullGameSystemName);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->seekBlock("WeaponRanges");
	gosASSERT(result == NO_ERR);

	float span[2];
	result = gameSystemFile->readIdFloatArray("Short", span, 2);
	gosASSERT(result == NO_ERR);
	WeaponRanges[WEAPON_RANGE_SHORT][0] = span[0];
	WeaponRanges[WEAPON_RANGE_SHORT][1] = span[1];

	result = gameSystemFile->readIdFloatArray("Medium", span, 2);
	gosASSERT(result == NO_ERR);
	WeaponRanges[WEAPON_RANGE_MEDIUM][0] = span[0];
	WeaponRanges[WEAPON_RANGE_MEDIUM][1] = span[1];

	result = gameSystemFile->readIdFloatArray("Long", span, 2);
	gosASSERT(result == NO_ERR);
	WeaponRanges[WEAPON_RANGE_LONG][0] = span[0];
	WeaponRanges[WEAPON_RANGE_LONG][1] = span[1];

	result = gameSystemFile->readIdFloatArray("OptimalRangePoints", OptimalRangePoints, 5);
	gosASSERT(result == NO_ERR);

	for (long i = 0; i < 5; i++)
		for (long j = 0; j < 3; j++) {
			OptimalRangePointInRange[i][j] = false;
			if (OptimalRangePoints[i] > WeaponRanges[j][0])
				if (OptimalRangePoints[i] <= WeaponRanges[j][1])
					OptimalRangePointInRange[i][j] = true;
		}

	result = gameSystemFile->seekBlock("General");
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdFloat("MaxVisualRange",maxVisualRange);
	gosASSERT(result == NO_ERR);
	MaxVisualRadius = maxVisualRange * 1.4142;

	result = gameSystemFile->readIdFloat("FireVisualRange",fireVisualRange);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdULong("MaxTreeLOSBlock",MaxTreeLOSCellBlock);
	if (result != NO_ERR)
		MaxTreeLOSCellBlock = 5;

	result = gameSystemFile->readIdFloatArray("WeaponRange", WeaponRange, NUM_FIRERANGES);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdFloat("DefaultAttackRange", DefaultAttackRange);
	if (result != NO_ERR)
		DefaultAttackRange = 75.0;

	result = gameSystemFile->readIdFloat("BaseSensorRange",baseSensorRange);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdLongArray("VisualRangeTable",visualRangeTable,256);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdFloat("BaseHeadShotElevation",BaseHeadShotElevation);
	if (result != NO_ERR)
		BaseHeadShotElevation = 1.0f;

	long forestMoveCost;
	result = gameSystemFile->readIdLong("ForestMoveCost", forestMoveCost);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdFloat("MaxUnitExtractDistance",MaxExtractUnitDistance);
	if (result != NO_ERR)
		MaxExtractUnitDistance = 1280.0f;	//Ten Tiles away
		
	//----------------------------------------------------------------------
	// Now that we have some base values, load the master component table...
	if (!MasterComponent::masterList) {
		FullPathFileName compFileName;
		compFileName.init(objectPath,"compbas",".csv");
#ifdef _DEBUG
		long loadErr = 
#endif
		    MasterComponent::loadMasterList(compFileName, 255, baseSensorRange);
		gosASSERT(loadErr == NO_ERR);

		// EQUIPMENT-REGISTRY-FACADE-1: build the parallel core:<slug> <-> ordinal
		// registry beside masterList (gated MC2_UNIT_PROFILE_DATA; gate-OFF no-op).
		// Additive -- reads masterList only, changes no gameplay path.
		buildAndDumpEquipmentRegistryIfEnabled();
		// FIRE-EXECUTOR-REGISTRY-1 (E7): gated proof of the fireExecutor lookup
		// over the just-loaded weapon components (no-op OFF; fires nothing).
		dumpFireExecutorProofIfEnabled();
		// ONE-WEAPON-PRESENTATION-PROOF-1 (E8): gated load-time proof that the
		// target weapon's presentation asset resolves from data (no-op OFF;
		// resolves an ASSET id only, fires nothing, changes no firing behavior).
		dumpPresentationProofIfEnabled();
	}

	result = gameSystemFile->readIdUChar("GodMode", godMode);
	if (result != NO_ERR)
		godMode = 0;

	unsigned char revealTacMap;
	result = gameSystemFile->readIdUChar("RevealTacMap", revealTacMap);
	if (result != NO_ERR)
		revealTacMap = 0;
		
	result = gameSystemFile->readIdUChar("FootPrints", footPrints);
	if (result != NO_ERR)
		footPrints = 1;

	result = gameSystemFile->readIdLong("BonusTonnageDivisor",tonnageDivisor);
	gosASSERT(result == NO_ERR);
	
	result = gameSystemFile->readIdLong("BonusPointsPerTon",resourcesPerTonDivided);
	gosASSERT(result == NO_ERR);
	
#ifndef FINAL
 	result = gameSystemFile->readIdFloat("CheatHitDamage",CheatHitDamage);
	if (result != NO_ERR)
		CheatHitDamage = 5.0f;
#endif
	
	//---------------------------------------
	// Read in difficulty here if it exits.
	InitDifficultySettings(gameSystemFile);

	result = Mover::loadGameSystem(gameSystemFile, maxVisualRange);
	gosASSERT(result == NO_ERR);

	//result = loadMultiplayerGameSystem(gameSystemFile);
	//gosASSERT(result == NO_ERR);

	result = BattleMech::loadGameSystem(gameSystemFile);
	gosASSERT(result == NO_ERR);

	//--------------------------------------------------------------------
	result = GroundVehicle::loadGameSystem(gameSystemFile);
	gosASSERT(result == NO_ERR);

#ifdef USE_ELEMENTALS
	result = loadElementalGameSystem(gameSystemFile);
	gosASSERT(result == NO_ERR);
#endif

	result = gameSystemFile->seekBlock("Mine");
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdFloat("BaseDamage", MineDamage);
	gosASSERT(result == NO_ERR);
		
	result = gameSystemFile->readIdFloat("SplashDamage", MineSplashDamage);
	gosASSERT(result == NO_ERR);
		
	result = gameSystemFile->readIdFloat("SplashRange", MineSplashRange);
	gosASSERT(result == NO_ERR);
		
	result = gameSystemFile->readIdLong("Explosion", MineExplosion);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdLong("MineLayThrottle", MineLayThrottle);
	if (result != NO_ERR)
		MineLayThrottle = 50;

	result = gameSystemFile->readIdLong("MineSweepThrottle", MineSweepThrottle);
	if (result != NO_ERR)
		MineSweepThrottle = 50;

	result = gameSystemFile->readIdFloat("MineWaitTime", MineWaitTime);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdFloat("StrikeWaitTime", StrikeWaitTime);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdFloat("StrikeTimeToImpact", StrikeTimeToImpact);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->seekBlock("Smoke");
	gosASSERT(result == NO_ERR);
	
	result = gameSystemFile->readIdLong("MaxSmokeSpheres",totalSmokeSpheres);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdLong("TotalSmokeShapeSize",totalSmokeShapeSize);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->seekBlock("Fire");
	gosASSERT(result == NO_ERR);
	
	result = gameSystemFile->readIdLong("MaxFiresBurning", maxFiresBurning);
	gosASSERT(result == NO_ERR);
	
	result = gameSystemFile->readIdFloat("MaxFireBurnTime", maxFireBurnTime);
	gosASSERT(result == NO_ERR);

	memset(missionFileName,0,80);
	strncpy(missionFileName,missionName,79);

	FullPathFileName missionFileName;
	missionFileName.init(missionPath,missionName,".fit");

	// Activate mod scope for this mission (no-op if it's a base-game mission).
	ActivateModForMission(missionFileName);

	duration = 60;

	missionFile = new FitIniFile;
	gosASSERT(missionFile != NULL);

	result = missionFile->open(missionFileName);
	if (result != NO_ERR)
		STOP(("Unable to open Mission File %s",missionFileName));

	if (!dropZoneList) {
		result = missionFile->seekBlock("Multiplayer");
		if (result == NO_ERR) {
			#if 0
			result = missionFile->readIdULong("TeamId", MultiPlayTeamId);
			gosASSERT(result == NO_ERR);
			result = missionFile->readIdULong("CommanderId", MultiPlayCommanderId);
			gosASSERT(result == NO_ERR);
			char sessionName[128];
			result = missionFile->readIdString("SessionName", sessionName, 127);
			gosASSERT(result == NO_ERR);
			char playerName[128];
			result = missionFile->readIdString("PlayerName", playerName, 127);
			gosASSERT(result == NO_ERR);
			bool isServer = false;
			result = missionFile->readIdBoolean("Server", isServer);
			gosASSERT(result == NO_ERR);
			unsigned long numPlayers;
			result = missionFile->readIdULong("NumPlayers", numPlayers);
			gosASSERT(result == NO_ERR);
			gosASSERT(MPlayer == NULL);
			MPlayer = new MultiPlayer;
			Assert(MPlayer != NULL, 0, " Unable to create MultiPlayer object ");
			MPlayer->setup();
			MPlayer->commanderID = MultiPlayCommanderId;
			//-------------------------------------------
			// If I'm the server, then create the game...
			if (isServer) {
				if (MPlayer->hostGame(sessionName, playerName, numPlayers)) {
					//---------------------------------------------
					//game hosted, so now wait for all check-ins...
					MPlayer->serverCID = MultiPlayCommanderId;//(MultiPlayCommanderId == ServerPlayerNum); //(gos_NetInformation(gos_AmITheServer) == 0);
				}
				}
			else {
				MPlayer->joinGame(NULL, sessionName, playerName);
				//MPlayer->numFitPlayers = numPlayers;
			}
			#endif
		}
	}

#ifdef LAB_ONLY
	x=GetCycles();
	MCTimeMiscToTeamLoad=x-x1;
#endif

	//-----------------------------------
	// Find the SKY Number and save it.
	// If no number, i.e. an old mission file,
	// simply make it 1 until its written out
	// in the magical editor (tm)
	result = missionFile->seekBlock("TheSky");
	if (result != NO_ERR)
		theSkyNumber = DEFAULT_SKY;
	else
	{
		result = missionFile->readIdLong("SkyNumber",theSkyNumber);
		if (result != NO_ERR)
			theSkyNumber = DEFAULT_SKY;
			
		if ((theSkyNumber < 1) || (theSkyNumber > 21))
			theSkyNumber = DEFAULT_SKY;
	}
		
	//-----------------------------------------
	// Begin Setting up Teams and Commanders...
	loadProgress = 10.0f;

	result = missionFile->seekBlock("Teams");
	Assert(result == NO_ERR, result, " Could not find Teams Block ");
	for (int i = 0; i < Team::numTeams; i++)
		if (Team::teams[i]) {
			delete Team::teams[i];
			Team::teams[i] = NULL;
		}
	Team::numTeams = 0;
	for (int i = 0; i < Commander::numCommanders; i++)
		if (Commander::commanders[i]) {
			delete Commander::commanders[i];
			Commander::commanders[i] = NULL;
		}
	Commander::numCommanders = 0;

	//------------------------------------------------------------
	// First, let's see how many teams and commanders there are...
	long maxTeamID = -1;
	long maxCommanderID = -1;
	if (loadType == MISSION_LOAD_MP_LOGISTICS) {
		for (long i = 0; i < MAX_MC_PLAYERS; i++) {
			if (MPlayer->playerInfo[i].team > maxTeamID)
				maxTeamID = MPlayer->playerInfo[i].team;
			if (MPlayer->playerInfo[i].commanderID > maxCommanderID)
				maxCommanderID = MPlayer->playerInfo[i].commanderID;
		}
		}
	else {
		result = missionFile->seekBlock("Parts");
		gosASSERT(result == NO_ERR);
		result = missionFile->readIdULong("NumParts",numParts);
		gosASSERT(result == NO_ERR);
		if (numParts)
			for (int i = 1; i < long(numParts + 1); i++) {
				char partName[12];
				sprintf(partName,"Part%d",i);
				
				//------------------------------------------------------------------
				// Find the object to load
				result = missionFile->seekBlock(partName);
				gosASSERT(result == NO_ERR);

				char teamID = -1;
				result = missionFile->readIdChar("TeamId", teamID);
				gosASSERT(result == NO_ERR);

				char commanderID = -1;
				result = missionFile->readIdChar("CommanderId", commanderID);
				if (result != NO_ERR) {
					long cID;
					result = missionFile->readIdLong("CommanderId", cID);
					gosASSERT(result == NO_ERR);
					commanderID = (char)cID;
				}

				if (MPlayer && dropZoneList) {
					//-------------------------------------------------------------
					// Since dropZoneList is not NULL, we know this was not started
					// from the command-line...
					long origCommanderID = commanderID;
					commanderID = commandersToLoad[origCommanderID][0];
					teamID = commandersToLoad[origCommanderID][1];
				}

				if (commanderID > maxCommanderID)
					maxCommanderID = commanderID;
				if (teamID > maxTeamID)
					maxTeamID = teamID;
			}
	}

	//----------------------------------------------
	// Now, init the teams and commanders we need...
	for (int i = 0; i <= maxTeamID; i++) {
		Team::teams[i] = new Team;
		Team::teams[i]->init(i);
	}
	for (int i = 0; i <= maxCommanderID; i++) {
		Commander::commanders[i] = new Commander;
		Commander::commanders[i]->setId(i);
	}
	
	if (MPlayer) {
		Team::home = Team::teams[MultiPlayTeamId];
		Commander::home = Commander::commanders[MultiPlayCommanderId];
		for (long i = 0; i <= maxCommanderID; i++)
			if (MPlayer->playerInfo[i].commanderID > -1)
				Commander::commanders[MPlayer->playerInfo[i].commanderID]->setTeam(Team::teams[MPlayer->playerInfo[i].team]);
		}
	else {
		Team::home = Team::teams[0];
		Commander::home = Commander::commanders[0];
		for (long i = 0; i <= maxCommanderID; i++) {
			if (commandersToLoad[i][0] > -1)
				Commander::commanders[commandersToLoad[i][0]]->setTeam(Team::teams[commandersToLoad[i][1]]);
		}
		Commander::commanders[0]->setTeam(Team::home);
	}

	//-----------------------------
	// Init Trigger Area Manager...
	if (Mover::triggerAreaMgr) {
		delete Mover::triggerAreaMgr;
		Mover::triggerAreaMgr = NULL;
	}
	Mover::triggerAreaMgr = new TriggerAreaManager;
	Assert(Mover::triggerAreaMgr != NULL, 0, " Mossion.init: unable to init triggerAreaMgr ");

	//-----------------------------------
	// Setup the Sensor System Manager...
	SensorManager = new SensorSystemManager;
	Assert(SensorManager != NULL, 0, " Unable to init sensor system manager ");
	result = SensorManager->init(true);
	Assert(result == NO_ERR, result, " could not start Sensor System Manager ");

	result = missionFile->seekBlock( "DropZone0" );
	if ( result == NO_ERR ) // lets not enforce drop zones for now
	{
		missionFile->readIdFloat( "PositionX", dropZone.x );
		missionFile->readIdFloat( "PositionY", dropZone.y );
	}
	else
	{
		dropZone.x = -1.f;
		dropZone.y = -1.f;
	}

#ifdef LAB_ONLY
	x1=GetCycles();
	MCTimeTeamLoad=x1-x;
#endif

	//-----------------------------------------------------------------
	// Load the names of the scenario tunes.
	//result = missionFile->seekBlock("Music");
	result = missionFile->seekBlock("MissionSettings");
	gosASSERT(result == NO_ERR);
		
	result = missionFile->readIdUChar("scenarioTuneNum",missionTuneNum);
	gosASSERT(result == NO_ERR);

	long numRPoints;
	result = missionFile->readIdLong("ResourcePoints",numRPoints);
	if (MaxResourcePoints > -1)
		numRPoints = MaxResourcePoints;
	if (MPlayer) {
		numRPoints = MPlayer->missionSettings.resourcePoints;
		for (long i = 0; i < MAX_MC_PLAYERS; i++) {
			MPlayer->playerInfo[i].resourcePoints = MPlayer->missionSettings.resourcePoints;
			MPlayer->playerInfo[i].resourcePointsGained = 0;
			MPlayer->playerInfo[i].resourcePointsAtStart = MPlayer->missionSettings.resourcePoints;
		}
	}
	LogisticsData::instance->setResourcePoints(numRPoints);

	if (MPlayer) {
		result = missionFile->readIdULong("NumRandomRPbuildings", MPlayer->numRandomResourceBuildings);
	}

	craterManager = (CraterManagerPtr)missionHeap->Malloc(sizeof(CraterManager));
	gosASSERT(craterManager != NULL);
		
	result = craterManager->init(1000,20479,"feet");
	gosASSERT(result == NO_ERR);
	
	//-----------------------------------------------------------------
	// Start the object system next.	
	ObjectManager = new GameObjectManager;
	if (!ObjectManager)
		Fatal(0, " Mission.init: unable to create ObjectManager ");
	ObjectManager->init("object2", 716799, 3072000);
	gosASSERT(result == NO_ERR);

	
	//-----------------------------------------------------------------
	// Start the collision detection system. -- Doesn't need objects?
	ObjectManager->initCollisionSystem(missionFile);

	//------------------------------------------------------------
	// Start the Terrain System

	FullPathFileName terrainFileName;
	terrainFileName.init( missionPath, missionName, ".pak" ); 

	PacketFile pakFile;
	result = pakFile.open( terrainFileName );
	gosASSERT( result == NO_ERR );

	land = new Terrain;

	land->getColorMapName(missionFile);

	gosASSERT(land != NULL);

	loadProgress = 15.0f;

#ifdef LAB_ONLY
	x=GetCycles();
	MCTimeObjectLoad=x-x1;
#endif

	long terrainInitResult;
	{
		ZoneScopedN("Mission::init land->init");
		terrainInitResult = land->init(&pakFile, 0, GameVisibleVertices, loadProgress, 20.0 );
	}

	if (terrainInitResult != NO_ERR)
	{
		STOP(("Could not load terrain.  Probably size was wrong!"));
	}

	loadProgress = 35.0f;

#ifdef LAB_ONLY
	x1=GetCycles();
	MCTimeTerrainLoad=x1-x;
#endif

	{ ZoneScopedN("Mission::init land->load"); land->load( missionFile ); }

	loadProgress = 36.0f;
	{ ZoneScopedN("Mission::init land->primeMissionTerrainCache"); land->primeMissionTerrainCache(loadProgress, 4.0f); }
	loadProgress = 40.0f;

//	land->recalcWater();		//Should have already been done in the editor

	//----------------------------------------------------
	// Start GameMap for Movement System
	Assert(SimpleMovePathRange > 20, SimpleMovePathRange, " Simple MovePath Range too small ");
	MOVE_init(SimpleMovePathRange);
	if (pakFile.seekPacket(4) == NO_ERR)
	{
		if (pakFile.getPacketSize() != 0)
		{
			MOVE_readData(&pakFile, 4);
			if (GlobalMoveMap[0]->badLoad) {
				PAUSE((" Mission.init: bad/old move data — skipping gate callback wiring; pathfinding degraded "));
				GameMap->placeMoversCallback = PlaceMovers;
			} else {
				GameMap->placeMoversCallback = PlaceMovers;
				GlobalMoveMap[0]->isGateDisabledCallback = IsGateDisabled;
				GlobalMoveMap[1]->isGateDisabledCallback = IsGateDisabled;
				GlobalMoveMap[2]->isGateDisabledCallback = IsGateDisabled;
				GlobalMoveMap[0]->isGateOpenCallback = IsGateOpen;
				GlobalMoveMap[1]->isGateOpenCallback = IsGateOpen;
				GlobalMoveMap[2]->isGateOpenCallback = IsGateOpen;
			}
		}
		else {
			// Packet 4 is empty — mission was saved without MOVE data.
			STOP(("Mission has not movement Data.  QuickSaved Map?"));
			// Blank MOVE synthesized below (shared with the seekPacket-failed path).
		}
	}

	// Safety net: if GameMap is still NULL here the mission will crash in
	// goal.cpp, group.cpp, etc. at frame ~1455 when the AI starts calculating
	// paths (GameMap->width/height accessed without null guard everywhere).
	// Covers three failure modes:
	//   (a) seekPacket(4) returned error — no packet 4 at all
	//   (b) getPacketSize() == 0 — packet reserved but empty
	//   (c) badLoad on GlobalMoveMap[0] left GameMap initialised but
	//       GlobalMoveMap[1/2] == NULL (hover/heli units crash if ever used)
	// Build a blank all-passable MOVE from terrain dimensions.
	// Round moveSide DOWN to the nearest SECTOR_DIM multiple so
	// GlobalMap::build's assertion passes on non-standard map sizes.
	if (!GameMap) {
		int cellSide = Terrain::realVerticesMapSide - 1;
		int moveSide = cellSide * MAPCELL_DIM;
		if (moveSide % SECTOR_DIM != 0)
			moveSide = (moveSide / SECTOR_DIM) * SECTOR_DIM;  // round down
		if (moveSide > MAX_MAP_CELL_WIDTH)
			moveSide = MAX_MAP_CELL_WIDTH;
		if (moveSide > 0) {
			printf("[MISSION] synthesizing blank MOVE: moveSide=%d (terrainVtx=%d)\n",
			       moveSide, (int)Terrain::realVerticesMapSide);
			fflush(stdout);
			MOVE_buildData(moveSide, moveSide, NULL /*all-passable blank*/, 0, NULL);
			if (GameMap)
				GameMap->placeMoversCallback = PlaceMovers;
		}
	}

	PathFindMap[SECTOR_PATHMAP]->blockedDoorCallback = GetBlockedDoorCells;
	PathFindMap[SECTOR_PATHMAP]->placeStationaryMoversCallback = PlaceStationaryMovers;
	PathFindMap[SIMPLE_PATHMAP]->placeStationaryMoversCallback = PlaceStationaryMovers;
	PathFindMap[SECTOR_PATHMAP]->forestCost = forestMoveCost;
	PathFindMap[SIMPLE_PATHMAP]->forestCost = forestMoveCost;
	PathManager = new MovePathManager;

#ifdef LAB_ONLY
	x=GetCycles();
	MCTimeMoveLoad=x-x1;
#endif

	//----------------------
	// Load ABL Libraries...
	long numErrors = 0, numLinesProcessed = 0;
	// ABL-CORE-LIBRARY-MOUNT-1: declarative core-library table (replaces three hardcoded
	// gosASSERT'd loads). orders/miscfunc are universal; corebrain (MCO / wolfman-MC2X) and
	// mc2xcore (cveg / MC2X-R52) are the two core-brain variants — load whichever a mod
	// provides, so MC2X- and MCO-lineage mod missions (e.g. DarkRain "torrin") get their core
	// brain orders + eternals (corePatrol/coreGuard/coreRepair/noAttackCode/wxmCode/…) defined.
	// Absent optional cores are a clean no-op (stock/MCO ship no mc2xcore.abx). Our Enhanced
	// brain (MC2_BRAIN_*) layers on top via the _ai.fit/_specials.fit loader below — this only
	// provides the native ABL base. Soft-fail (log, no crash) so a missing lib degrades rather
	// than asserting. Trace with MC2_ABL_CORE_TRACE=1.
	static const struct AblCoreLib { const char* name; bool required; } kAblCoreLibs[] = {
		{ "orders",    true  },
		{ "miscfunc",  true  },
		{ "corebrain", false },   // MCO / wolfman-MC2X core
		{ "mc2xcore",  false },   // cveg / MC2X-R52 core
	};
	const bool ablCoreTrace = (std::getenv("MC2_ABL_CORE_TRACE") && std::atoi(std::getenv("MC2_ABL_CORE_TRACE")) != 0);
	for (const AblCoreLib& lib : kAblCoreLibs) {
		FullPathFileName libPath;
		libPath.init(missionPath, lib.name, ".abx");
		if (!fileExists(libPath, FILE_ON_DISK | FILE_ON_FST)) {
			if (lib.required)
				std::fprintf(stderr, "[ABL_CORE] WARN: required library %s.abx not found at %s\n", lib.name, missionPath);
			else if (ablCoreTrace)
				std::fprintf(stderr, "[ABL_CORE] optional library %s.abx absent — skipped\n", lib.name);
			continue;
		}
		numErrors = 0;
		ABLModulePtr library = ABLi_loadLibrary(libPath, &numErrors, &numLinesProcessed);
		if (ablCoreTrace || !library || numErrors > 0)
			std::fprintf(stderr, "[ABL_CORE] load %s.abx -> %s (errors=%ld)\n",
			             lib.name, library ? "OK" : "NULL", numErrors);
	}

	//---------------------------
	// Load the mission script...
	//-----------------------------------------------------------------
	// We now read in the mission Script File Name
	result = missionFile->seekBlock("Script");
	gosASSERT(result == NO_ERR);
	
	result = missionFile->readIdString("ScenarioScript",missionScriptName,79);
	gosASSERT(result == NO_ERR);

	// FULL-CAMPAIGN-1: TechScript/Enhanced-converted missions (carver_v_enhanced) leave
	// ScenarioScript empty — the per-warrior brains move to inline Brain{} blocks and mission
	// logic to *_specials.fit — but preserve the original scenario name as LegacyScenarioScript.
	// An empty name builds "<missionPath>/.abl" and ABLi_preProcess STOPs before the mission can
	// load. Fall back to the legacy name so the original scenario ABL (objectives/events) still
	// drives. Gate MC2_BRAIN_INLINE_EMPTY_SKIP (default OFF); stock ScenarioScript is never empty
	// so stock is byte-identical.
	if (missionScriptName[0] == '\0' &&
	    std::getenv("MC2_BRAIN_INLINE_EMPTY_SKIP") &&
	    std::atoi(std::getenv("MC2_BRAIN_INLINE_EMPTY_SKIP")) != 0) {
		char legacyScriptName[80] = {0};
		if (missionFile->readIdString("LegacyScenarioScript", legacyScriptName, 79) == NO_ERR &&
		    legacyScriptName[0] != '\0') {
			strncpy(missionScriptName, legacyScriptName, 79);
			missionScriptName[79] = '\0';
			std::fprintf(stderr, "[MISSIONFIT_OPORD] empty ScenarioScript -> LegacyScenarioScript '%s'\n",
			             missionScriptName);
		}
	}

	visualTuning_applyProfile(missionScriptName);  // MISSION-VISUAL-TUNING-1

	FullPathFileName brainFileName;
	brainFileName.init(missionPath, missionScriptName, ".abl");
	
	missionScriptHandle = ABLi_preProcess(brainFileName, &numErrors, &numLinesProcessed);
	gosASSERT(missionScriptHandle >= 0);
	
	missionBrain = new ABLModule;
	gosASSERT(missionBrain != NULL);
		
#ifdef _DEBUG
	long brainErr = 
#endif
		missionBrain->init(missionScriptHandle);
	gosASSERT(brainErr == NO_ERR);

	missionBrain->setName("Mission");
	//MissionBrain->setStep(TRUE);

	missionBrainParams = new ABLParam;
	gosASSERT(missionBrainParams != NULL);

	missionBrainCallback = missionBrain->findFunction("handlemessage", TRUE);

#ifdef LAB_ONLY
	x1=GetCycles();
	MCTimeMissionABLLoad=x1-x;
#endif

	loadProgress = 41.0f;

	//-------------------------------------------
	// Load all MechWarriors for this mission...
	MechWarrior::setup();

	result = missionFile->seekBlock("Warriors");
	gosASSERT(result == NO_ERR);

	unsigned long numWarriors;
	result = missionFile->readIdULong("NumWarriors",numWarriors);
	gosASSERT(result == NO_ERR);

	bool loadBrainParameters = (result == NO_ERR);
	// CRASH-SOAK harness: tally how many mission AI brains (e.g. "magicx" on
	// MCO missions) load vs fail, so the soak runner can confirm AI is active.
	long soakBrainsLoaded = 0;
	long soakBrainsFailed = 0;
	if (numWarriors)
	{
		for (long i = 1; i <= numWarriors; i++)
		{
			char warriorName[12];
			sprintf(warriorName,"Warrior%d",i);
			
			//-------------------------
			// Find the warrior to load
			result = missionFile->seekBlock(warriorName);
			Assert(result == NO_ERR, i, " Could not find Warrior Number Block ");

			char warriorFile[100];
			result = missionFile->readIdString("Profile", warriorFile, 99);
			Assert(result == NO_ERR, 0, " Could not find Warrior Profile in Warrior Number Block ");

			MechWarriorPtr pilot = MechWarrior::newWarrior();
			if (!pilot)
				STOP(("Too many pilots in this mission!"));
			
			//--------------------------------------
			// Load the mechwarrior into the mech...
			FullPathFileName pilotFullFileName;
			pilotFullFileName.init(warriorPath, warriorFile, ".fit");
			
			FitIniFile* pilotFile = new FitIniFile;
			gosASSERT(pilotFile != NULL);
		
			long result = pilotFile->open(pilotFullFileName);
			gosASSERT(result == NO_ERR);
			result = pilot->init(pilotFile);
			gosASSERT(result == NO_ERR);
			
			pilotFile->close();
			delete pilotFile;
			pilotFile = NULL;
			
			//----------------------------
			// Read in the Brain module...
			char moduleName[128];
			result = missionFile->readIdString("Brain", moduleName, 127);
			gosASSERT(result == NO_ERR);
			
			//------------------------------------------------------------
			// For now, all mplayer brains are pbrain. Need to change when
			// we allow ai brains in mplayer...
			long numErrors, numLinesProcessed;
			FullPathFileName brainFileName;
			if (MPlayer) {
				pilot->setBrainName("pbrain");
				brainFileName.init(warriorPath, "pbrain", ".abl");
				}
			else {
				pilot->setBrainName(moduleName);
				brainFileName.init(warriorPath, moduleName, ".abl");
			}
			
			// BRAIN-EMPTY-BRAIN-SKIP-1: inline Brain{} (TechScript/Enhanced) missions leave the
			// warrior "Brain" field empty — the brain lives in the mission.fit Brain{} block, not a
			// .abl file. Without a guard the empty name builds "warriors/.abl" and ABLi_preProcess
			// STOPs (FATAL: missing ABL), so the mission never reaches the inline-Brain consumers
			// (e.g. BRAIN-MISSIONFIT-OPORD-CONSUMER-1 below). Gate MC2_BRAIN_INLINE_EMPTY_SKIP
			// (default OFF). Stock missions always name a brain, so stock is byte-identical — the
			// guard only fires on an empty, non-MPlayer name.
			static const bool inlineEmptySkip =
				(std::getenv("MC2_BRAIN_INLINE_EMPTY_SKIP") &&
				 std::atoi(std::getenv("MC2_BRAIN_INLINE_EMPTY_SKIP")) != 0);
			if (inlineEmptySkip && !MPlayer && moduleName[0] == '\0')
			{
				// No legacy ABL brain; the inline Brain{} block drives this warrior.
				pilot->setBrainName("");
			}
			else
			{
				long moduleHandle = ABLi_preProcess(brainFileName, &numErrors, &numLinesProcessed);
				gosASSERT(moduleHandle >= 0);
				if (moduleHandle >= 0 && numErrors == 0)
					soakBrainsLoaded++;
				else
				{
					soakBrainsFailed++;
					printf("[SOAK] WARN abl brain load failed name=%s errors=%ld\n",
						moduleName, (long)numErrors);
				}

#ifdef _DEBUG
				long error =
#endif
					pilot->setBrain(moduleHandle);
				gosASSERT(error == 0);
			}
		}
	}
	if (std::getenv("MC2_SOAK_AUTOWIN") != nullptr || soakBrainsFailed > 0)
	{
		printf("[SOAK] abl mission brains loaded=%ld failed=%ld\n",
			soakBrainsLoaded, soakBrainsFailed);
		fflush(stdout);
	}

	if (loadBrainParameters) {
		//---------------------------------------------------------------
		// Load the brain parameter file and load 'em for each warrior...
		// BRAIN-EMPTY-BRAIN-SKIP-1: inline-Brain warriors carry no ABL brain (skipped above),
		// so loadBrainParameters would Fatal(0,"NULL brain"). Skip them under the same gate.
		static const bool inlineEmptySkipParams =
			(std::getenv("MC2_BRAIN_INLINE_EMPTY_SKIP") &&
			 std::atoi(std::getenv("MC2_BRAIN_INLINE_EMPTY_SKIP")) != 0);
		for (unsigned long i = 1; i <= numWarriors; i++) {
			if (inlineEmptySkipParams && MechWarrior::warriorList[i] &&
			    !MechWarrior::warriorList[i]->getBrain())
				continue;
			result = MechWarrior::warriorList[i]->loadBrainParameters(missionFile, i);
			//Assert(result == NO_ERR, result, " Could not load Warrior Brain Parameters ");
		}

	}

	// BRAIN-MISSIONFIT-OPORD-CONSUMER-1: drive declarative patrol from the inline Brain{}
	// PrimaryOPORD blocks in mission.fit (carver_v_enhanced format). FitIniFile cannot see
	// brace-blocks, so raw-scan the mission.fit text via the File class (VFS/mod-overlay aware —
	// NOT fopen). Gate MC2_BRAIN_MISSIONFIT_OPORD (default OFF). Populates the existing
	// MechBrainRuntime patrol structures (BRAIN-OPORD-COREPATROL-1); the brain bundle drives them
	// (the patrol FIELDS persist through later _ai.fit/tactic loaders, which only touch mode).
	if (std::getenv("MC2_BRAIN_MISSIONFIT_OPORD") && std::atoi(std::getenv("MC2_BRAIN_MISSIONFIT_OPORD")) != 0) {
		File mfFile;
		if (mfFile.open(missionFileName) == NO_ERR) {
			unsigned long mfSz = mfFile.getLength();
			char* mfBuf = (char*)malloc(mfSz + 1);
			if (mfBuf) {
				mfFile.read((MemoryPtr)mfBuf, (long)mfSz);
				mfBuf[mfSz] = '\0';
				MissionFitBrain* mfBrains = (MissionFitBrain*)malloc(sizeof(MissionFitBrain) * 128);
				int mfN = mfBrains ? parseMissionFitBrains(mfBuf, mfBrains, 128) : 0;
				int mfPatrol = 0, mfGuard = 0, mfSentry = 0, mfEscort = 0, mfOther = 0;
				for (int r = 0; r < mfN; ++r) {
					const MissionFitBrain& b = mfBrains[r];
					if (b.warriorIndex < 1 || (unsigned long)b.warriorIndex > numWarriors) continue;
					uint8_t pType = brainOpordTypeId(b.primary.type);
					BrainArchetypeDefaults ad; brainArchetypeLookup(b.archetype, ad);  // archetype presets; switches override
					if (ad.playerControlled || pType == 9 || pType == 255) continue;   // player / unknown -> default path
					MechWarriorPtr w = MechWarrior::warriorList[b.warriorIndex];
					if (!w) continue;
					if (!w->getBrainRuntime()) w->setBrainRuntimeMode(BrainRuntimeMode::Enhanced);
					MechBrainRuntime* rt = w->getBrainRuntime();
					if (!rt) continue;
					// EngageRadius = per-unit COMMITMENT radius: the team SHARES the contact (all know the
					// enemy exists) but a unit only attacks once the threat is within its own EngageRadius,
					// so distant units keep patrolling/holding instead of the whole map swarming one sighting.
					rt->engageRadius         = (b.engageRadius >= 0) ? b.engageRadius : ad.engageRadius;
					rt->swAttackerHelpRadius = (b.attackerHelpRadius >= 0) ? b.attackerHelpRadius : ad.attackerHelpRadius;
					rt->swDefenderHelpRadius = (b.defenderHelpRadius >= 0) ? b.defenderHelpRadius : ad.defenderHelpRadius;
					rt->swRequestHelp        = (b.requestHelp  >= 0) ? (int8_t)b.requestHelp  : (int8_t)ad.requestHelp;
					rt->swReturnToPost       = (b.returnToPost >= 0) ? (int8_t)b.returnToPost : (int8_t)ad.returnToPost;
					rt->swWakeOnAttack       = (int8_t)b.wakeOnAttack;
					rt->swPoweredDown        = (int8_t)b.poweredDown;
					rt->opordType[0] = pType;
					rt->opordType[1] = brainOpordTypeId(b.secondary.type);
					rt->opordType[2] = brainOpordTypeId(b.tertiary.type);
					rt->opordCursor  = 0;
					// Load weighted Tactics (MC2_TACTIC_WEIGHTS selects per tick; _WEIGHTS_B applies to attacks).
					for (int ti = 0; ti < b.tacticCount; ++ti) {
						int tidx = brainTacticNameToIndex(b.tacticName[ti]);
						if (tidx >= 0 && tidx < 12) rt->tacticWeights[tidx] = b.tacticWeight[ti];
					}
					if (w->getVehicle()) {
						Stuff::Vector3D vp = w->getVehicle()->getPosition();
						rt->postPos[0] = vp.x; rt->postPos[1] = vp.y; rt->postPos[2] = vp.z; rt->postSet = true;
					}
					switch (pType) {
						case 0: {   // Patrol
							int wc = (b.primary.waypointCount > 8) ? 8 : b.primary.waypointCount;
							if (wc <= 0) { rt->guardHold = true; ++mfGuard; break; }
							for (int wi = 0; wi < wc; ++wi) {
								Stuff::Vector3D wp; wp.x = b.primary.waypoints[wi].x; wp.y = b.primary.waypoints[wi].y; wp.z = 0.0f;
								float z = land ? land->getTerrainElevation(wp) : 0.0f;
								rt->patrolWaypoints[wi][0] = b.primary.waypoints[wi].x;
								rt->patrolWaypoints[wi][1] = b.primary.waypoints[wi].y;
								rt->patrolWaypoints[wi][2] = z;
							}
							rt->patrolWaypointCount = (uint8_t)wc;
							rt->patrolWaypointIndex = 0;
							rt->patrolLoop   = b.primary.loop;
							rt->patrolActive = true;
							++mfPatrol; break;
						}
						case 1:     // Guard
							rt->guardHold = true; ++mfGuard; break;
						case 3:     // Sentry
							rt->guardHold = true; rt->sentryAsleep = (rt->swPoweredDown == 1); ++mfSentry; break;
						case 4: {   // Escort
							int ei = b.primary.escortTargetIndex;
							if (ei >= 1 && (unsigned long)ei <= numWarriors && MechWarrior::warriorList[ei] &&
							    MechWarrior::warriorList[ei]->getVehicle())
								rt->escortTargetWID = MechWarrior::warriorList[ei]->getVehicle()->getWatchID();
							rt->escortMoving = true; rt->guardHold = true; ++mfEscort; break;
						}
						case 2: {   // MoveTo - move to destination once, then hold + engage
							int wc = (b.primary.waypointCount > 8) ? 8 : b.primary.waypointCount;
							if (wc > 0) {
								for (int wi = 0; wi < wc; ++wi) {
									Stuff::Vector3D wp; wp.x = b.primary.waypoints[wi].x; wp.y = b.primary.waypoints[wi].y; wp.z = 0.0f;
									float z = land ? land->getTerrainElevation(wp) : 0.0f;
									rt->patrolWaypoints[wi][0] = b.primary.waypoints[wi].x;
									rt->patrolWaypoints[wi][1] = b.primary.waypoints[wi].y;
									rt->patrolWaypoints[wi][2] = z;
								}
								rt->patrolWaypointCount = (uint8_t)wc;
								rt->patrolWaypointIndex = 0;
								rt->patrolLoop = false;   // move once to destination, do not loop
								rt->patrolActive = true;
							}
							rt->guardHold = true; ++mfOther; break;
						}
						case 7:     // Attack - aggressive: engage any detected enemy (no EngageRadius limit)
							rt->guardHold = true; rt->engageRadius = 0.0f; ++mfOther; break;
						case 8: {   // Withdraw - move to rally point (primary waypoint) once, then hold
							int wc = (b.primary.waypointCount > 8) ? 8 : b.primary.waypointCount;
							if (wc > 0) {
								for (int wi = 0; wi < wc; ++wi) {
									Stuff::Vector3D wp; wp.x = b.primary.waypoints[wi].x; wp.y = b.primary.waypoints[wi].y; wp.z = 0.0f;
									float z = land ? land->getTerrainElevation(wp) : 0.0f;
									rt->patrolWaypoints[wi][0] = b.primary.waypoints[wi].x;
									rt->patrolWaypoints[wi][1] = b.primary.waypoints[wi].y;
									rt->patrolWaypoints[wi][2] = z;
								}
								rt->patrolWaypointCount = (uint8_t)wc;
								rt->patrolWaypointIndex = 0;
								rt->patrolLoop = false;   // move once to destination, do not loop
								rt->patrolActive = true;
							}
							rt->guardHold = true; ++mfOther; break;
						}
						case 5:     // Ambush - hidden until a threat is near, then strike (reuses sentry asleep/woken)
							rt->guardHold = true; rt->sentryAsleep = true; ++mfOther; break;
						case 6: {   // Scout - move the route observing only; never engages
							int wc = (b.primary.waypointCount > 8) ? 8 : b.primary.waypointCount;
							if (wc > 0) {
								for (int wi = 0; wi < wc; ++wi) {
									Stuff::Vector3D wp; wp.x = b.primary.waypoints[wi].x; wp.y = b.primary.waypoints[wi].y; wp.z = 0.0f;
									float z = land ? land->getTerrainElevation(wp) : 0.0f;
									rt->patrolWaypoints[wi][0] = b.primary.waypoints[wi].x;
									rt->patrolWaypoints[wi][1] = b.primary.waypoints[wi].y;
									rt->patrolWaypoints[wi][2] = z;
								}
								rt->patrolWaypointCount = (uint8_t)wc;
								rt->patrolWaypointIndex = 0;
								rt->patrolLoop = b.primary.loop;
								rt->patrolActive = true;
							}
							rt->scoutObserveOnly = true; ++mfOther; break;
						}
						default:    // MoveTo / Attack / Withdraw / Ambush / Scout -> hold + engage (verb wiring TBD)
							rt->guardHold = true; ++mfOther; break;
					}
				}
				std::fprintf(stderr, "[MISSIONFIT_OPORD] parsed %d Brain{} record(s): patrol=%d guard=%d sentry=%d escort=%d other=%d\n",
					mfN, mfPatrol, mfGuard, mfSentry, mfEscort, mfOther);
				if (mfBrains) free(mfBrains);
				free(mfBuf);
			}
			mfFile.close();
		}
	}

	// BRAIN-RUNTIME-1B: load per-unit Brain blocks from <missionName>_ai.fit.
	// Gate: MC2_BRAIN_RUNTIME=1 (default OFF). File absence is silent (no fixture = all Legacy).
	// Single-warrior fixture only: one Brain block keyed "Brain". unitRef "Warrior%d" -> idx.
	// MC2_BRAIN_RUNTIME_FORCE_MODE (if set) overrides all per-unit modes as a post-load global.
	if (std::getenv("MC2_BRAIN_RUNTIME") && std::atoi(std::getenv("MC2_BRAIN_RUNTIME")) != 0) {
		char aiFitName[256];
		// missionName is the raw name like "mc2_01"; missionPath is "data/missions/".
		std::snprintf(aiFitName, sizeof(aiFitName), "%s%s_ai.fit", missionPath, missionName);
		std::fprintf(stderr, "[BRAIN_RT] mission load: MC2_BRAIN_RUNTIME=1 mission=%s seeking %s\n", missionName, aiFitName);
		std::fflush(stderr);
		FitIniFile* aiFit = new FitIniFile;
		if (aiFit && aiFit->open(aiFitName) == NO_ERR) {
			// Seek the single "Brain" block (single-warrior fixture; multi-block not supported this slice).
			if (aiFit->seekBlock("Brain") == NO_ERR) {
				char unitRefBuf[32] = {};
				long modeValLong = 0;
				long unitRefResult  = aiFit->readIdString("unitRef",  unitRefBuf, 31);
				long modeResult     = aiFit->readIdLong("mode", modeValLong);
				if (unitRefResult == NO_ERR && modeResult == NO_ERR) {
					// Parse "Warrior%d" -> 1-based index.
					int warriorIdx = -1;
					std::sscanf(unitRefBuf, "Warrior%d", &warriorIdx);
					if (warriorIdx >= 1 && warriorIdx <= (int)numWarriors) {
						MechWarriorPtr w = MechWarrior::warriorList[warriorIdx];
						if (w) {
							BrainRuntimeMode fitMode = BrainRuntimeMode::Legacy;
							switch (modeValLong) {
								case 1: fitMode = BrainRuntimeMode::Hybrid;   break;
								case 2: fitMode = BrainRuntimeMode::Enhanced; break;
								default: break;
							}
							// setBrainRuntimeMode allocates brainRuntime if needed + sets mode.
							w->setBrainRuntimeMode(fitMode);
							std::fprintf(stderr, "[BRAIN_RT] FIT load warriorIdx=%d unitRef=%s mode=%s\n",
								warriorIdx,
								unitRefBuf,
								MechBrainRuntime::modeString(fitMode));
							std::fflush(stderr);
						}
					} else {
						std::fprintf(stderr, "[BRAIN_RT] WARNING: _ai.fit Brain block unitRef=%s idx=%d out of range [1..%lu]\n",
							unitRefBuf, warriorIdx, (unsigned long)numWarriors);
					}
				}
			}
			aiFit->close();
		}
		// MC2_BRAIN_RUNTIME_FORCE_MODE overrides all per-unit modes (post-load global override).
		const char* forceModeEnv = std::getenv("MC2_BRAIN_RUNTIME_FORCE_MODE");
		if (forceModeEnv) {
			BrainRuntimeMode forced = BrainRuntimeMode::Legacy;
			if      (std::strcmp(forceModeEnv, "hybrid")   == 0) forced = BrainRuntimeMode::Hybrid;
			else if (std::strcmp(forceModeEnv, "enhanced") == 0) forced = BrainRuntimeMode::Enhanced;
			for (unsigned long i = 1; i <= numWarriors; i++) {
				MechWarriorPtr w = MechWarrior::warriorList[i];
				if (w && w->getBrainRuntime())
					w->setBrainRuntimeMode(forced);
			}
		}
		delete aiFit;
	}

	// TACTIC-WEIGHTS-A: load [Tactics] block from <missionName>_ai.fit into tacticWeights[].
	// Gate: MC2_TACTIC_WEIGHTS=1 (default OFF).
	// File absence is silent. Block absence is silent (all weights stay zero).
	// Format: one float entry per tactic by short name, e.g. "f StopAndFire = 0.30"
	// Normalisation is enforced at load time (sum→1); weightsNormalized flag set.
	// Both raw-zero and absent-block warriors keep tacticWeights all-zero (FIT fallback
	// uses uniform weights via the all-zero branch in applyPilotModulation).
	if (std::getenv("MC2_TACTIC_WEIGHTS") && std::atoi(std::getenv("MC2_TACTIC_WEIGHTS")) != 0) {
		char tacFitName[256];
		std::snprintf(tacFitName, sizeof(tacFitName), "%s%s_ai.fit", missionPath, missionName);
		FitIniFile* tacFit = new FitIniFile;
		if (tacFit && tacFit->open(tacFitName) == NO_ERR) {
			if (tacFit->seekBlock("Tactics") == NO_ERR) {
				// Parse known tactic names; missing keys silently default to 0.
				struct { const char* name; int idx; } kTacticKeys[] = {
					{ "None",             TACTIC_NONE             },
					{ "FlankLeft",        TACTIC_FLANK_LEFT       },
					{ "FlankRight",       TACTIC_FLANK_RIGHT      },
					{ "FlankRear",        TACTIC_FLANK_REAR       },
					{ "StopAndFire",      TACTIC_STOP_AND_FIRE    },
					{ "Turret",           TACTIC_TURRET           },
					{ "Joust",            TACTIC_JOUST            },
					{ "IndirectFire",     TACTIC_INDIRECT_FIRE    },
					{ "HullDown",         TACTIC_HULL_DOWN        },
					{ "FightingWithdraw", TACTIC_FIGHTING_WITHDRAW},
					{ "Pursue",           TACTIC_PURSUE           },
					{ "HitAndRun",        TACTIC_HIT_AND_RUN      },
				};
				float loadedWeights[NUM_TACTICS] = {};
				for (auto& kv : kTacticKeys) {
					float val = 0.0f;
					if (tacFit->readIdFloat(kv.name, val) == NO_ERR)
						loadedWeights[kv.idx] = (val < 0.0f) ? 0.0f : val;
				}
				// Normalize
				float sum = 0.0f;
				for (int i = 0; i < NUM_TACTICS; i++) sum += loadedWeights[i];
				if (sum > 0.0f) {
					float inv = 1.0f / sum;
					for (int i = 0; i < NUM_TACTICS; i++) loadedWeights[i] *= inv;
				}
				// Distribute to all warriors that have a brainRuntime
				int loadCount = 0;
				for (unsigned long i = 1; i <= numWarriors; i++) {
					MechWarriorPtr w = MechWarrior::warriorList[i];
					if (w) {
						if (!w->getBrainRuntime()) {
							// Allocate runtime so tactic weights are accessible even for Legacy warriors.
							w->setBrainRuntimeMode(BrainRuntimeMode::Legacy);
						}
						MechBrainRuntime* rt = w->getBrainRuntime();
						if (rt) {
							std::memcpy(rt->tacticWeights, loadedWeights, sizeof(rt->tacticWeights));
							rt->weightsNormalized = 1;
							loadCount++;
						}
					}
				}
				std::fprintf(stderr, "[BRAIN_TACTIC_SELECT] [Tactics] loaded from %s: %d warriors weighted\n",
				             tacFitName, loadCount);
				std::fflush(stderr);
			}
			tacFit->close();
		}
		delete tacFit;
	}

	// TECHSCRIPT-SPECIAL-DISPATCH-1A: load per-warrior BrainSpecial body from <missionName>_specials.fit.
	// Gate: MC2_BRAIN_DISPATCH=1 (default OFF). Requires MC2_BRAIN_RUNTIME=1 + MC2_BRAIN_RUNTIME_APPLY=1.
	// File absence is silent. Only warriors with brainRuntime allocated (Enhanced mode) are loaded.
	//
	// TECHSCRIPT-SPECIAL-DISPATCH-1C: gate MC2_BRAIN_DISPATCH_FSM_TODO=1 (requires MC2_BRAIN_DISPATCH=1).
	// Runs a MISSION-LEVEL raw-text scan of the specials file for
	// "; TODO: manual ABL line: <payload>" comments (stripped by FitIniFile).
	// Emits [BRAIN_DISPATCH_FSM_TODO] summary + detail lines. Information only — no behavior change.
	// Scan is done once per mission (not per-warrior) because the file is mission-level.
	// Gate-OFF: byte-identical to pre-1C; no extra parsing, no extra traces.
	{
		const bool dispatchOn   = (std::getenv("MC2_BRAIN_DISPATCH")         && std::atoi(std::getenv("MC2_BRAIN_DISPATCH"))         != 0);
		const bool fsmTodoOn    = (std::getenv("MC2_BRAIN_DISPATCH_FSM_TODO") && std::atoi(std::getenv("MC2_BRAIN_DISPATCH_FSM_TODO")) != 0);
		const bool dispatchVarOn = (std::getenv("MC2_BRAIN_DISPATCH_VAR")     && std::atoi(std::getenv("MC2_BRAIN_DISPATCH_VAR"))     != 0);
		const bool missionVarOn  = (std::getenv("MC2_BRAIN_VAR_MISSION")      && std::atoi(std::getenv("MC2_BRAIN_VAR_MISSION"))      != 0);

		// GAP-A: reset the mission-level specials cache at every mission load (unconditional;
		// produces no trace, so gate-OFF runs stay byte-identical). Prevents a stale cache
		// from a prior dispatch-ON mission leaking into a later one.
		resetMissionSpecialCache();

		// TECHSCRIPT-DISPATCH-1D-M: reset mission-scope Var store at every mission load.
		// resetMissionVarStore() is always safe to call (no-op if store already empty).
		// Gate: only reset when MC2_BRAIN_VAR_MISSION=1 to avoid noise in gate-OFF runs.
		if (missionVarOn) {
			resetMissionVarStore();
		}

		if (fsmTodoOn && !dispatchOn) {
			std::fprintf(stderr, "[BRAIN_DISPATCH_FSM_TODO] WARN: MC2_BRAIN_DISPATCH_FSM_TODO=1 requires MC2_BRAIN_DISPATCH=1 — inert\n");
			std::fflush(stderr);
		}
		// TECHSCRIPT-DISPATCH-1D: gate-dependency check for DISPATCH_VAR.
		if (dispatchVarOn && !dispatchOn) {
			std::fprintf(stderr, "[BRAIN_DISPATCH_VAR] WARNING: MC2_BRAIN_DISPATCH_VAR=1 requires MC2_BRAIN_DISPATCH=1 -- var handling is INERT\n");
			std::fflush(stderr);
		}

		if (dispatchOn) {
			// DISPATCH-EFFECT-UNITEJECT-1: MC2_BRAIN_SPECIAL_FIT allows test-fixture override.
			// When set, ALL warriors load from <override>_specials.fit instead of <mission>_specials.fit.
			// Intended for smoke gate verification only. Default: missionName (standard behavior).
			const char* specialFitEnv = std::getenv("MC2_BRAIN_SPECIAL_FIT");
			const char* specialFitName = (specialFitEnv && specialFitEnv[0] != '\0') ? specialFitEnv : missionName;

			std::fprintf(stderr, "[BRAIN_DISPATCH] mission load: MC2_BRAIN_DISPATCH=1 mission=%s special_fit=%s\n",
			             missionName, specialFitName);
			std::fflush(stderr);

			// 1A/1B: per-warrior verb parse (only for warriors with brainRuntime allocated).
			// TECHSCRIPT-CALL-CHAIN-1A: pass specialIndex so the raw scanner populates it.
			// The index is per-warrior-runtime (each warrior gets its own copy); content
			// is identical across warriors for the same mission (all read the same file).
			// This is acceptable for 1A: index is small (<10 blocks), mission-ephemeral.
			//
			// GAP-A MULTI-WARRIOR SPECIALS: parse mission-level specials body ONCE into the
			// dispatch cache. Warriors whose brainRuntime is allocated lazily in runBrain
			// (not named in _ai.fit) copy from this cache on first tick, so every warrior
			// dispatches -- not just the _ai.fit-recorded ones.
			cacheMissionSpecialBody(specialFitName);
			for (unsigned long i = 1; i <= numWarriors; i++) {
				MechWarriorPtr w = MechWarrior::warriorList[i];
				if (w && w->getBrainRuntime()) {
					parseBrainSpecialBody(specialFitName, w->getBrainRuntime()->specialBody,
					                      &w->getBrainRuntime()->specialIndex);
				}
			}

			// TECHSCRIPT-SPECIAL-DISPATCH-1C: FSM-TODO mission-level scan + trace.
			// Done ONCE per mission (the specials file is mission-level, not per-warrior).
			// Gate-OFF: this entire block is skipped; no ifstream open, no extra traces.
			if (fsmTodoOn) {
				// Use a temporary BrainSpecialBody for the scan — trace output is mission-level.
				// fsmTodos are INFORMATION ONLY; they are not stored persistently here.
				// (Future per-warrior storage is deferred — the file has one FSM skeleton shared
				// by all warriors; per-warrior active-state gating is out of 1C scope.)
				BrainSpecialBody tmpBody;
				scanFsmTodosFromFile(specialFitName, tmpBody);

				// Count by kind for the summary line.
				int nStateDef = 0, nStateEnd = 0, nTrans = 0, nTransBack = 0, nOther = 0;
				for (const FsmTodoEntry& e : tmpBody.fsmTodos) {
					switch (e.kind) {
						case FsmTodoKind::STATE_DEF:  ++nStateDef;  break;
						case FsmTodoKind::STATE_END:  ++nStateEnd;  break;
						case FsmTodoKind::TRANS:      ++nTrans;     break;
						case FsmTodoKind::TRANS_BACK: ++nTransBack; break;
						case FsmTodoKind::OTHER_TODO: ++nOther;     break;
					}
				}

				// Summary line (always emitted when FSM_TODO gate is on, even if count=0).
				// wid=-1 signals mission-level (no specific warrior context).
				std::fprintf(stderr,
					"[BRAIN_DISPATCH_FSM_TODO] mission=%s wid=-1 stateDefs=%d transitions=%d otherTodos=%d\n",
					missionName, nStateDef, nTrans + nTransBack, nOther);
				std::fflush(stderr);

				// Detail lines: STATE_DEF, TRANS, TRANS_BACK, STATE_END only.
				// OTHER_TODO is counted in summary but NOT detail-traced (noise suppression).
				for (const FsmTodoEntry& e : tmpBody.fsmTodos) {
					switch (e.kind) {
						case FsmTodoKind::STATE_DEF:
							std::fprintf(stderr,
								"[BRAIN_DISPATCH_FSM_TODO] kind=STATE_DEF name=%s wid=-1\n",
								e.name.c_str());
							std::fflush(stderr);
							break;
						case FsmTodoKind::STATE_END:
							std::fprintf(stderr,
								"[BRAIN_DISPATCH_FSM_TODO] kind=STATE_END wid=-1\n");
							std::fflush(stderr);
							break;
						case FsmTodoKind::TRANS:
							std::fprintf(stderr,
								"[BRAIN_DISPATCH_FSM_TODO] kind=TRANS target=%s wid=-1\n",
								e.name.c_str());
							std::fflush(stderr);
							break;
						case FsmTodoKind::TRANS_BACK:
							std::fprintf(stderr,
								"[BRAIN_DISPATCH_FSM_TODO] kind=TRANS_BACK wid=-1\n");
							std::fflush(stderr);
							break;
						case FsmTodoKind::OTHER_TODO:
							// Suppressed from detail trace (counted in summary only).
							break;
					}
				}
			} // fsmTodoOn
		} // dispatchOn
	}

#ifdef LAB_ONLY
	x=GetCycles();
	MCTimeWarriorLoad=x-x1;
#endif

	loadProgress = 43.0f;

	//-----------------------------------------------------------------
  	// All systems are GO if we reach this point.  Now we need to
	// parse the scenario file for the Objects we need for this scenario
	// We then create each object and place it in the world at the 
	// position we read in with the frame we read in.
	result = missionFile->seekBlock("Parts");
	gosASSERT(result == NO_ERR);
		
	result = missionFile->readIdULong("NumParts",numParts);
	gosASSERT(result == NO_ERR);

	//--------------------------------------------------------------------------------
	// IMPORTANT NOTE: mission parts should always start with Part 1.
	// Part 0 is reserved as a "NULL" id for routines that reference the mission
	// parts. AI routines, Brain keywords, etc. use PART ID 0 as an "object not found"
	// error code. DO NOT USE PART 0!!!!!!! Start with Part 1...

#define MAX_SQUADS			256
#define	MAX_ALTERNATIVES	15
#define	USE_ALTERNATES

	loadProgress = 43.5f;

	long numMoversLoaded[MAX_MC_PLAYERS] = {0, 0, 0, 0, 0, 0, 0, 0};
	long numDropZonePositions = 0;
	if (numParts)
	{
		//-----------------------------------------------------
		// Since we leave part 0 unused, malloc numParts + 1...
		parts = (PartPtr)missionHeap->Malloc(sizeof(Part) * (numParts + 1));
		gosASSERT(parts != NULL);
		
		memset(parts,0,sizeof(Part) * (numParts + 1));

#ifdef USE_ALTERNATES
		//------------------------------------------------------------
		// Before we actually read in the parts, do some prep work for
		// determining squad alternatives...
		long numSquads = 0;
		long squadMap[MAX_SQUADS];
		for (long i = 0; i < MAX_SQUADS; i++)
			squadMap[i] = -1;
		long maxAlternatives[MAX_SQUADS];
		long randomAlternative[MAX_SQUADS];
		for (long s = 0; s < MAX_SQUADS; s++) {
			maxAlternatives[s] = 0;
			randomAlternative[s] = -1;
		}

		long bigAlternatives = 0;

		for (int i = 1; i < long(numParts + 1); i++) {
			char partName[12];
			sprintf(partName,"Part%d",i);

			result = missionFile->seekBlock(partName);
			gosASSERT(result == NO_ERR);
			unsigned long squadNum;
			result = missionFile->readIdULong("squadNum", squadNum);
			long squadIndex = 0;
			for (squadIndex = 0; squadIndex < numSquads; squadIndex++)
				if (squadMap[squadIndex] == squadNum)
					break;
			if (squadIndex == numSquads)
				squadMap[numSquads++] = squadNum;

			long alternatives[MAX_ALTERNATIVES];
			result = missionFile->readIdLongArray("IndicesOfAlternatives", alternatives, MAX_ALTERNATIVES);
			long numAlternatives = 0;
			for (numAlternatives = 0; numAlternatives < MAX_ALTERNATIVES; numAlternatives++)
				if (alternatives[numAlternatives] == -1)
					break;
			if (maxAlternatives[squadIndex] < numAlternatives)
				maxAlternatives[squadIndex] = numAlternatives;

			if (numAlternatives > bigAlternatives)
				bigAlternatives = numAlternatives;
		}

		if (MPlayer && (bigAlternatives > 0)) {
			PAUSE(("Mission.init: multiplayer map has random squads"));
			bigAlternatives = 0;
		}

		long alternateChoice = RandomNumber(bigAlternatives + 1);
		for (int s = 0; s < numSquads; s++)
		{
			randomAlternative[s] = alternateChoice;
			if (GameDifficulty >= 2)
			{
				randomAlternative[s]--;
				if (randomAlternative[s] < 1)
					randomAlternative[s] = 1;
			}
		}

		//--------------------------------------------------------
		// This block is optional, and is used for testing only...
		result = missionFile->seekBlock("Squads");
		if (result == NO_ERR)
			for (long i = 0; i < numSquads; i++) {
				char s[128];
				sprintf(s, "Squad%d", i);
				unsigned long alternate = -1;
				result = missionFile->readIdULong(s, alternate);
				if (result == NO_ERR)
					randomAlternative[i] = alternate;
			}
#else
		long i;
#endif

		for (int i = 1; i < long(numParts + 1); i++)
		{
			char partName[12];
			sprintf(partName,"Part%d",i);
			
			//------------------------------------------------------------------
			// Find the object to load
			result = missionFile->seekBlock(partName);
			gosASSERT(result == NO_ERR);

#ifdef USE_ALTERNATES
			//----------------------------------------------------------------------
			// If we have alternatives, choose which one we're taking before we read
			// anything else in...
			bool usingAlternate = false;
			unsigned long realPilot = 0;
			unsigned long squadNum;
			result = missionFile->readIdULong("squadNum", squadNum);
			parts[i].squadId = squadNum;
			long squadIndex = 0;
			for (squadIndex = 0; squadIndex < numSquads; squadIndex++)
				if (squadMap[squadIndex] == squadNum)
					break;

			long alternatives[MAX_ALTERNATIVES];
			result = missionFile->readIdLongArray("IndicesOfAlternatives", alternatives, MAX_ALTERNATIVES);
			gosASSERT(result == NO_ERR);
			if (maxAlternatives[squadIndex]) 
			{
				long partId = i;
				if (randomAlternative[squadIndex] > 0)
					partId = alternatives[randomAlternative[squadIndex] - 1];
				if (partId == -1)
					continue;
				Assert(partId > 0, partId, " Mission.init: Bad Alternate ");

				//MUST save off ORIGINAL Pilot.  WE don't load the alternate pilots!!!!!
				usingAlternate = true;
				result = missionFile->readIdULong("Pilot", realPilot);
				gosASSERT(result == NO_ERR);

				sprintf(partName, "Part%d", partId);
				result = missionFile->seekBlock(partName);
				gosASSERT(result == NO_ERR);
			}
#endif

			//------------------------------------------------------------------
			// Find out what kind of object this is.
			result = missionFile->readIdULong("ObjectNumber",parts[i].objNumber);
			gosASSERT(result == NO_ERR);

			//-------------------------------------------------
			// Read in the data needed to control the object...
			result = missionFile->readIdULong("ControlType", parts[i].controlType);
			gosASSERT(result == NO_ERR);

			result = missionFile->readIdULong("ControlDataType", parts[i].controlDataType);
			gosASSERT(result == NO_ERR);

			result = missionFile->readIdString("ObjectProfile", parts[i].profileName, 9);
			gosASSERT(result == NO_ERR);

			// Mod-tolerance: read CSVFile so we can back-fill the ObjectType
			// appearance name when the pak lookup fails for a high ObjectNumber.
			parts[i].csvFile[0] = 0;
			missionFile->readIdString("CSVFile", parts[i].csvFile, sizeof(parts[i].csvFile) - 1);

			result = missionFile->readIdULong("VariantNumber", parts[i].variantNum);
			if (result != NO_ERR)
				parts[i].variantNum = 0;		//FOR NOW!!!!!!!!!!!!!!!!
												//MAKE a REAL error when Heidi fixes editor.
												//-fs 12/7/99

			if (usingAlternate)
			{
				parts[i].pilot = realPilot;
			}
			else
			{
				result = missionFile->readIdULong("Pilot", parts[i].pilot);
				gosASSERT(result == NO_ERR);
			}
			
			//------------------------------------------------------------------
			// Read the object's position, initial velocity and rotation.
			result = missionFile->readIdFloat("PositionX",parts[i].position.x);
			gosASSERT(result == NO_ERR);
				
			result = missionFile->readIdFloat("PositionY",parts[i].position.y);
			gosASSERT(result == NO_ERR);
				
			parts[i].position.z = -1.0;

			result = missionFile->readIdFloat("Rotation",parts[i].rotation);
			gosASSERT(result == NO_ERR);
				
			result = missionFile->readIdChar("TeamId",parts[i].teamId);
			gosASSERT(result == NO_ERR);
			//--------------------------------------------------------------------------
			// Hack for singleplayer, until editor spits this out properly for allies...
			if (!MPlayer && (parts[i].teamId == 2))
				parts[i].teamId = 0;
			
			if (dropZoneList && (dropZoneID == parts[i].teamId))
				dropZoneList[numDropZonePositions++] = parts[i].position;

			char cmdId = 0;
			result = missionFile->readIdChar("CommanderId", cmdId);
			if (result != NO_ERR)
			{
				result = missionFile->readIdLong("CommanderId", parts[i].commanderID);
				gosASSERT(result == NO_ERR);
			}
			else
			{
				parts[i].commanderID = cmdId;
			}

			if (loadType == MISSION_LOAD_MP_QUICKSTART) {
				long origCommanderID = parts[i].commanderID;
				parts[i].commanderID = commandersToLoad[origCommanderID][0];
				parts[i].teamId = commandersToLoad[origCommanderID][1];
				if (commandersToLoad[origCommanderID][0] > -1) {
					if (numMoversLoaded[commandersToLoad[origCommanderID][0]] == numMoversPerCommander) {
						parts[i].commanderID = -1;
						parts[i].teamId = -1;
						}
					else
						numMoversLoaded[commandersToLoad[origCommanderID][0]]++;
				}
			}

			parts[i].gestureId = 2; // this has never changed
	
			result = missionFile->readIdULong("BaseColor",parts[i].baseColor);
			if (result != NO_ERR || MPlayer )
				parts[i].baseColor = prefs.baseColor;
				
			result = missionFile->readIdULong("HighlightColor1",parts[i].highlightColor1);
			if (result != NO_ERR || MPlayer)
				parts[i].highlightColor1 = prefs.highlightColor;
				
			result = missionFile->readIdULong("HighlightColor2",parts[i].highlightColor2);
			if (result != NO_ERR || MPlayer )
				parts[i].highlightColor2 = prefs.highlightColor;
				
  			parts[i].velocity = 0;
			
			result = missionFile->readIdLong("Active",parts[i].active);
			gosASSERT(result == NO_ERR);

			result = missionFile->readIdLong("Exists",parts[i].exists);
			gosASSERT(result == NO_ERR);

			float fDamage = 0.0f;
			result = missionFile->readIdFloat("Damage",fDamage);
			if (result == NO_ERR) {
				if (fDamage >= 1.0) {
					parts[i].destroyed = true;
				};
			}

			result = missionFile->readIdChar("MyIcon", parts[i].myIcon);
			gosASSERT(result == NO_ERR);

			result = missionFile->readIdChar("MyIcon", parts[i].myIcon);
			gosASSERT(result == NO_ERR);

			result = missionFile->readIdBoolean("Captureable", parts[i].captureable);
			if (result != NO_ERR)
				parts[i].captureable = FALSE;

			float increment = 5.0f/(numParts + 1);
			loadProgress += increment;

		}
	}

	loadProgress = 48.5f;
	//--------------------------------------------------------------------------
	// Now that the parts data has been loaded, let's prep the ObjectManager for
	// the real things. First, count the number of objects we need...
	long numMechs = 0;
	long numVehicles = 0;
	for (int i = 1; i < (numParts + 1); i++) 
	{
		ObjectTypePtr objType = ObjectManager->loadObjectType(parts[i].objNumber);
		if (!objType)
			objType = ObjectManager->getObjectType(parts[i].objNumber);
		if (objType)
			switch (objType->getObjectTypeClass()) 
			{
				case BATTLEMECH_TYPE:
					numMechs++;
					break;
				case VEHICLE_TYPE:
					numVehicles++;
					break;
			}

		float increment = 6.5f/(numParts + 1);
		loadProgress += increment;
	}

	switch (loadType) 
	{
		case MISSION_LOAD_SP_QUICKSTART:
		case MISSION_LOAD_SP_LOGISTICS:
			break;
		case MISSION_LOAD_MP_QUICKSTART:
		case MISSION_LOAD_MP_LOGISTICS:
			numMechs = 64;
			numVehicles = 64;
			break;
	}

	loadProgress = 55.0f;

	pakFile.seekPacket( 1 );
	ObjectManager->countTerrainObjects(&pakFile, (numMechs + MAX_TEAMS * MAX_REINFORCEMENTS_PER_TEAM) + (numVehicles + MAX_TEAMS * MAX_REINFORCEMENTS_PER_TEAM)/* + ObjectManager->maxElementals*/ + 1);
	loadProgress = 58.0f;
	ObjectManager->setNumObjects(numMechs, numVehicles, 0, -1, -1, -1, 100, 50, 0, 130, -1);

	// C0-3 + C1b GPU authority flip: init GPU cull substrate SSBO sized to
	// worst-case per-frame record count. Now includes BOTH dynamic actors
	// (getMaxObjects() + 25% headroom) AND static prop instances (appended by
	// GpuStaticPropRegistry::flush() before compute_dispatch()). Static props
	// at wolfman zoom can reach ~4096+ visible instances; add a flat 8192
	// record headroom beyond the dynamic actor count to cover them safely.
	{
		const uint32_t maxActors = static_cast<uint32_t>(ObjectManager->getMaxObjects());
		const uint32_t staticPropHeadroom = 8192u;  // visible static props at wolfman zoom
		gpu_cull::substrate_init(maxActors + maxActors / 4u + staticPropHeadroom);
	}
	// C1a: init GPU visibility compute pipeline (shadow/diagnostic mode).
	// No-op if MC2_GPU_CULL env var is not set (default off).
	gpu_cull::compute_init();
	frameJobsInit();  // FRAME-JOBS-1: fixed worker pool for parallel CPU prep
	// Phase 1: terrain lighting GPU compute — per-mission init alongside gpu_cull.
	// CRITICAL: use realVerticesMapSide * realVerticesMapSide, NOT getNumVertices()
	// (getNumVertices() returns 0 at this point — set per-frame by makeLists).
	// Terrain::realVerticesMapSide set during land->init() at mission.cpp:2222, before here.
	// No-op if MC2_TERRAIN_LIGHTING_GPU env var is not set (default off at Stage 1).
	gos_terrain_lighting::mission_init(
		static_cast<uint32_t>(Terrain::realVerticesMapSide * Terrain::realVerticesMapSide),
		64u);
	// C2: init async readback ring buffer.
	// Same maxActors sizing as substrate_init — readback slot must hold one flag per actor.
	// No-op if MC2_GPU_CULL_READBACK env var is not set (default off).
	{
		const uint32_t maxActors = static_cast<uint32_t>(ObjectManager->getMaxObjects());
		const uint32_t staticPropHeadroom = 8192u;
		gpu_cull::readback_init(maxActors + maxActors / 4u + staticPropHeadroom);
	}
	// CP-1: new mission - re-prime the world-fixed static shadow map and the
	// static light matrix (both are process-scoped and otherwise carry over
	// the previous mission's state into this mission).
	gos_ResetStaticShadowPriming();
	mc_ResetTerrainShadowPrimed();
	if (getenv("MC2_DECOR_SHADOW_TRACE")) {
		printf("[DECOR_SHADOW v1] event=mission_reset_priming frame=%u\n",
		       g_mc2FrameCounter); fflush(stdout);
	}

	//-------------------------
	// Load the mech objects...
	long curMech = 0;
	long curVehicle = 0;
	for (long t = 0; t < 8; t++)
		for (long i = 1; i < (numParts + 1); i++) 
		{
			bool loadEm = true;
			if (loadType == MISSION_LOAD_MP_LOGISTICS)
				loadEm = false;
			if (loadType == MISSION_LOAD_SP_LOGISTICS)
				if (parts[i].commanderID == 0 && !parts[i].destroyed)
					loadEm = false;
			if (loadType == MISSION_LOAD_MP_QUICKSTART)
				if (parts[i].commanderID == -1)
					loadEm = false;
			if (loadEm) {
				ObjectTypePtr objType = ObjectManager->getObjectType(parts[i].objNumber);
				if (objType)
					switch (objType->getObjectTypeClass()) 
					{
						case BATTLEMECH_TYPE:
							if (parts[i].teamId == t) 
							{
								BattleMechPtr mech = ObjectManager->getMech(curMech++);
								createPartObject(i, mech);
								float increment = 10.0f/(numParts + 1);
								loadProgress += increment;
							}
							break;
						case VEHICLE_TYPE:
							if (parts[i].teamId == t) 
							{
								GroundVehiclePtr vehicle = ObjectManager->getVehicle(curVehicle++);
								createPartObject(i, vehicle);
								float increment = 10.0f/(numParts + 1);
								loadProgress += increment;
							}
							break;
					}
				}
			else 
			{
				MechWarrior::freeWarrior(MechWarrior::warriorList[parts[i].pilot]);
			}
		}

#ifdef LAB_ONLY
	x1=GetCycles();
	MCTimeMoverPartsLoad=x1-x;
#endif

	loadProgress = 68.0f;

	{ ZoneScopedN("Mission::init ObjectManager::loadTerrainObjects"); ObjectManager->loadTerrainObjects(&pakFile, loadProgress, 30); }
	{ ZoneScopedN("Mission::init ObjectManager::primeTerrainObjectsForMissionLoad"); ObjectManager->primeTerrainObjectsForMissionLoad(loadProgress, 2.0f); }
	{ ZoneScopedN("Mission::init Track B static-prop registration walk"); ObjectManager->registerStaticPropsForMissionLoad(); }

	loadProgress = 98.0f;

	ObjectManager->buildMoverLists();

	if (MPlayer)
		MPlayer->initSpecialBuildings(commandersToLoad);

	//----------------------------------------------
	// Read in the Mission Time Limit.
	{
		m_timeLimit = -1.0f	/*seconds  IF -1.0 mission never times out!!! -fs*/;
		long result = 0;
		result = missionFile->seekBlock("MissionSettings");
		if (NO_ERR == result)
		{
			float tmpFloat = 0.0;
			result = missionFile->readIdFloat("TimeLimit", tmpFloat);
			if (NO_ERR == result)
			{
				m_timeLimit = tmpFloat;
			}
		}

		if ( MPlayer )
			m_timeLimit = MPlayer->missionSettings.timeLimit;
	}

	if (NumDisableAtStart) {
		for (long i = 0; i < NumDisableAtStart; i++)
			ObjectManager->getByWatchID(parts[DisableAtStart[i]].objectWID)->setDebugFlag(OBJECT_DFLAG_DISABLE, true);
	}

	//----------------------------------------------
	// Read in the Objectives.  Safe to have none.
	for (int i = 0; i <= maxTeamID; i++)
		Team::teams[i]->loadObjectives(missionFile);

/*	numObjectives = 0; // this refers to the number of *old* objectives
	objectives = 0;

	warning1 = FALSE;
	warning2 = FALSE;

	Team::home->objectives.Clear();
	Team::home->objectives.Read(missionFile);

	Team::home->numPrimaryObjectives = 0;
	CObjectives::EIterator it = Team::home->objectives.Begin();
	while (!it.IsDone())
	{
		if (1 == (*it)->Priority())
		{
			Team::home->numPrimaryObjectives += 1;
		}
		it++;
	}
	ReadNavMarkers(missionFile, Team::home->objectives);
*/
#ifdef LAB_ONLY
	x=GetCycles();
	MCTimeObjectiveLoad=x-x1;
#endif

	//----------------------------
	// Read in Commander Groups...
	for (long curCommanderId = 0; curCommanderId < MAX_MC_PLAYERS; curCommanderId++) {
		long numGroups = 0;
		char headingStr[128];
		
		if (commandersToLoad[curCommanderId][0] == -1)
			continue;

		sprintf(headingStr, "Commander%dGroup:%d", curCommanderId, numGroups);
		result = missionFile->seekBlock(headingStr);
		while (result == NO_ERR) {
			//---------------------------
			// Read in the Group Mates...
			bool pointSet = FALSE;
			long groupMates[MAX_MOVERGROUP_COUNT_START];
			result = missionFile->readIdLongArray("Mates", groupMates, MAX_MOVERGROUP_COUNT_START);
			Assert(result == NO_ERR, result, " could not find Mates in Group in Scenario File ");
			for (long curMate = 0; curMate < MAX_MOVERGROUP_COUNT_START; curMate++) {
				if ((groupMates[curMate] > 0) && parts[groupMates[curMate]].objectWID) {
					ObjectManager->setPartId(ObjectManager->getByWatchID(parts[groupMates[curMate]].objectWID), commandersToLoad[curCommanderId][0], numGroups, curMate);
					Commander::commanders[commandersToLoad[curCommanderId][0]]->getGroup(numGroups)->add((MoverPtr)ObjectManager->getByWatchID(parts[groupMates[curMate]].objectWID));
					if (!pointSet) {
						Commander::commanders[commandersToLoad[curCommanderId][0]]->getGroup(numGroups)->selectPoint(true);
						pointSet = true;
					}
				}
			}
			
			numGroups++;
			sprintf(headingStr, "Commander%dGroup:%d", curCommanderId, numGroups);
			result = missionFile->seekBlock(headingStr);
		}
	}

	//-----------------------------------------------------------------------
	// Now that the parts are loaded, let's build the roster for each team...
	for (int i = 0; i < Team::numTeams; i++)
		Team::teams[i]->buildRoster();

	//---------------------------------------------------------------------------------
	// If we're not playing multiplayer, make sure all home commander movers have their
	// localMoverId set to 0, so the iface can at least check if a mover is player
	// controlled...
	if (!MPlayer)
		Commander::home->setLocalMoverId(0);

#ifdef LAB_ONLY
	x1=GetCycles();
	MCTimeCommanderLoad=x1-x;
#endif

	//-----------------------------------------------------
	// This tracks time since scenario started in seconds.
	LastTimeGetTime = 0xffffffff;
	scenarioTime = 0.0;
	MissionStartTime = 0;
	runningTime = 0.0;
	actualTime = 0.0;

	gameSystemFile->close();
	delete gameSystemFile;
	gameSystemFile = NULL;

	//----------------------------
	// Create and load the Weather
	weather = new Weather;
	weather->init(missionFile);
	
 	//---------------------------------------------------------------
	// Start the Camera and Lists
	eye = new GameCamera;
	eye->init();
	gosASSERT(eye != NULL);

	result = eye->init(missionFile);
	gosASSERT(result == NO_ERR);

	eye->inMovieMode = false;

	loadProgress = 99.0;

#ifdef LAB_ONLY
	x=GetCycles();
	MCTimeMiscLoad=x-x1;
#endif

	//----------------------------------------------------------------------------
	// Start the Mission GUI
	missionInterface = new MissionInterfaceManager;
	gosASSERT(missionInterface != NULL);
	
	missionInterface->initTacMap( &pakFile, 2 );
	
	FullPathFileName missionScreenName;
	missionScreenName.init(artPath,"missionScrn",".fit");
	
	FitIniFile missionLoader;
	result = missionLoader.open(missionScreenName);
	gosASSERT(result == NO_ERR);
	
	missionInterface->init(&missionLoader);
	missionInterface->initMechs();
	missionLoader.close();

	// MISSION-START-HOVER-TARGET-LIFETIME-1: this MissionInterfaceManager is freshly
	// constructed, but the file-scope hover/target caches and the static `target` pointer
	// survive across missions. Clear them now (begin boundary) so the first Mission::update
	// can't pick a stale/freed object from the previous mission. Picking stays SUPPRESSED
	// until Mission::start() arms it (world fully live).
	MissionInterfaceManager::invalidateHoverTarget();
	
	//----------------------------------------------------------------------------
	userInput->setMouseCursor(mState_NORMAL);

	loadProgress = 100.0;

	
	//MechWarrior::initGoalManager(200);

	if (tempSpecialAreaFootPrints) {
		systemHeap->Free(tempSpecialAreaFootPrints);
		tempSpecialAreaFootPrints = NULL;
		tempNumSpecialAreas = 0;
	}

	Mover::initOptimalCells(32);

	//--------------------------------------------------
	// Close all walls and open gates and landbridges...
//	GameObjectPtr wallObjects[MAX_WALL_OBJECTS];
//	long numWalls = ObjectManager->getSpecificObjects(BUILDING, BUILDING_SUBTYPE_WALL, wallObjects, MAX_WALL_OBJECTS);
//	for (i = 0; i < numWalls; i++)
//		((BuildingPtr)wallObjects[i])->closeSubAreas();
//	long numLandBridges = ObjectManager->getSpecificObjects(BUILDING, BUILDING_SUBTYPE_LANDBRIDGE, wallObjects, MAX_WALL_OBJECTS);
//	for (i = 0; i < numLandBridges; i++)
//		((BuildingPtr)wallObjects[i])->openSubAreas();
//	for (i = 0; i < ObjectManager->getNumGates(); i++) {
//		GatePtr gate = ObjectManager->getGate(i);
//		gate->openSubAreas();
//	}

	if (CombatLog)
		MechWarrior::logPilots(CombatLog);

#ifdef LAB_ONLY
	x1=GetCycles();
	MCTimeGUILoad=x1-x;
#endif

#ifdef LAB_ONLY	
	//Add Mission Load statistics to GameOS Debugger screen!
	MCTimeABLLoad       *= OneOverProcessorSpeed;
	MCTimeMiscToTeamLoad*= OneOverProcessorSpeed;
	MCTimeTeamLoad      *= OneOverProcessorSpeed;
	MCTimeObjectLoad    *= OneOverProcessorSpeed;
	MCTimeTerrainLoad   *= OneOverProcessorSpeed;
	MCTimeMoveLoad      *= OneOverProcessorSpeed;
	MCTimeMissionABLLoad*= OneOverProcessorSpeed;
	MCTimeWarriorLoad   *= OneOverProcessorSpeed;
	MCTimeMoverPartsLoad*= OneOverProcessorSpeed;
	MCTimeObjectiveLoad *= OneOverProcessorSpeed;
	MCTimeCommanderLoad *= OneOverProcessorSpeed;
	MCTimeMiscLoad      *= OneOverProcessorSpeed;
	MCTimeGUILoad       *= OneOverProcessorSpeed;
#endif

	missionFile->close();
	delete missionFile;
	missionFile = NULL;

	// All actors have been spawned and have called registerType() into the
	// GPU static-prop batcher. Upload the shared VBO/IBO/VAO now so that
	// submit()/flush() can reference immutable geometry for the rest of the
	// mission. Safe here: GL context is live (textures/shadow FBOs already
	// exist by this point) and this is the unconditional tail of map-load.
	GameAdapters::StaticProp::finalizeGeometry();
	GpuMechBatcher::instance().finalizeGeometry();

	// C1b: build the DrawElementsIndirectCommand buffer now that all static prop
	// types are registered and geometry is finalized. No-op if MC2_GPU_CULL is
	// not set or if there are no static prop types in this mission.
	if (gpu_cull::compute_isEnabled()) {
		gpu_cull::compute_buildIndirectBuffer(GameAdapters::StaticProp::typeCount());
	}

	// STATIC-REG-PREWARM-QUEUE-1: bake light slots for registered static props
	// that are off-screen (never reach render() → never cleared their
	// needsFullBakeNextFrame H4 latch). Must run AFTER finalizeGeometry() (geometry
	// is final) AND after eye->init() (world lights are valid — eye is created at
	// mission.cpp:3589 before we reach here). Guarded by MC2_STATIC_REG_PREWARM=1.
	// updateLights() is called first so activeLights is populated (numActiveLights > 0);
	// without it activeLights is empty at load time and SetLightList gets zero lights.
	{ ZoneScopedN("Mission::init prewarmStaticPropLightBakes");
	  eye->primeActiveLightsForPrewarm();
	  ObjectManager->prewarmStaticPropLightBakes(eye); }

	// Vegetation card system — notify adapter that terrain + move map are ready.
	// land and GameMap are both stable by this point (terrain init + MOVE_readData
	// both completed above). No-op when MC2_VEGETATION_CARDS is unset.
	GameAdapters::Vegetation::missionLoaded(land, GameMap);
}

//----------------------------------------------------------------------------------
// Sets mission active.  Assumes that whatever we needed to do to the mission data
// set for logistis has been done and player is ready to play!
void DEBUGWINS_setGameObject (long debugObj, GameObjectPtr obj);
extern long GameObjectWindowList[3];
extern long NumGameObjectsToDisplay;

void Mission::start (void)
{
	active = true;
	mission_phase_mark("mission_ready");
	// MISSION-START-HOVER-TARGET-LIFETIME-1: mission world is now fully live (objects loaded,
	// active set). Re-enable hover picking that was suppressed since invalidateHoverTarget().
	if (missionInterface)
		MissionInterfaceManager::armHoverTarget();
	gos_SetHudScaleActive(true);  // enable HUD shrink only during mission
	for (long i = 0; i < NumGameObjectsToDisplay; i++)
		DEBUGWINS_setGameObject(-1, ObjectManager->getByWatchID(parts[GameObjectWindowList[i]].objectWID));
}

//----------------------------------------------------------------------------------
void Mission::initTGLForMission()
{
	ZoneScopedN("Mission::initTGLForMission");
	//---------------------------------------------------------
	// End the Tiny Geometry Layer Heap for Logistics
	if (TG_Shape::tglHeap)
	{
		ZoneScopedN("Mission::initTGLForMission shutdown");
		logTglPoolPeaks("mission_unload");
		//Shut down the TGL RAM pools.
		if (colorPool)
		{
			colorPool->destroy();
			delete colorPool;
			colorPool = NULL;
		}
		
		if (vertexPool)
		{
			vertexPool->destroy();
			delete vertexPool;
			vertexPool = NULL;
		}

		if (facePool)
		{
			facePool->destroy();
			delete facePool;
			facePool = NULL;
		}

		if (shadowPool)
		{
			shadowPool->destroy();
			delete shadowPool;
			shadowPool = NULL;
		}

		if (trianglePool)
		{
			trianglePool->destroy();
			delete trianglePool;
			trianglePool = NULL;
		}
		
		TG_Shape::tglHeap->destroy();

		delete TG_Shape::tglHeap;
		TG_Shape::tglHeap = NULL;
	}

	loadProgress += 1.0f;

	//------------------------------------------------------
	// Start the Tiny Geometry Layer Heap for the Mission.
	if ( !TG_Shape::tglHeap )
	{
		ZoneScopedN("Mission::initTGLForMission startup");
		//---------------------------------------------------------
		// Bumped from 40MB to 128MB to accommodate the scaled-up TGL
		// pools below (500K vertex/color/shadow, 200K face/triangle).
		// Total pool footprint is ~60MB; headroom for per-shape
		// allocations via tglHeap->Malloc.
		unsigned long tglHeapSize = 128 * 1024 * 1024;

		TG_Shape::tglHeap = new UserHeap;
		TG_Shape::tglHeap->init(tglHeapSize,"TinyGeom");
		
		//Start up the TGL RAM pools.
		// Sizes scaled up from the original 30K/40K to support the GPU
		// static-prop path (RAlt+0) which forces all buildings/trees/
		// generics in active blocks to TransformShape regardless of
		// inView. At wolfman zoom on large maps the original 30K vertex
		// pool exhausted before mechs allocated, silently dropping every
		// shape whose getVerticesFromPool hit NULL (tgl.h:1022). That
		// manifested as "half the mechs don't render" because mechs
		// iterate AFTER buildings and got the empty pool.
		// MODEL-OVERRIDE: pools sized for modder model overrides. The old
		// 500k/200k caps were CPU-era limits; per-instance vertex/color/shadow
		// storage scales with override mesh complexity x instance count (e.g.
		// ~1000 trees x ~12k verts = ~12M). Static props/trees are GPU-batched,
		// but the per-instance lighting/transform buffers still draw from these
		// pools. Sized to fit a detailed-tree forest with headroom.
		colorPool 		= new TG_VertexPool;
		colorPool->init(32000000);

		vertexPool 		= new TG_GOSVertexPool;
		vertexPool->init(32000000);

		facePool 		= new TG_DWORDPool;
		facePool->init(16000000);

		shadowPool 		= new TG_ShadowPool;
		shadowPool->init(32000000);

		trianglePool 	= new TG_TrianglePool;
		trianglePool->init(16000000);
	}

	loadProgress += 4.0f;

	//Stupid hack for now.  Should really get from prefs!!
	// Needed cause Heidi resets in logistics.
	useFog = true;
	useShadows = prefs.useShadows;
}


//----------------------------------------------------------------------------------
void Mission::destroy (bool initLogistics)
{
	gos_SetHudScaleActive(false);  // back to 100% for menus/logistics

	// C2: release async readback ring buffer at mission teardown.
	gpu_cull::readback_shutdown();
	// C1a: release GPU compute resources at mission teardown.
	frameJobsShutdown();  // FRAME-JOBS-1: join workers before any GPU teardown (LIFO vs init)
	gpu_cull::compute_shutdown();
	// C0-3: release GPU cull substrate SSBO at mission teardown.
	// substrate_init() handles re-init on next mission load (calls shutdown internally).
	gpu_cull::substrate_shutdown();

	// Release GPU resources at mission shutdown.
	// M1 Task 13 (order-correct split): call order is byte-identical to original:
	//   1. GpuStaticPropBatcher::onMapUnload()  ← endMissionEarly()
	//   2. GpuMechBatcher::onMapUnload()
	//   3. GpuStaticPropRegistry::destroy() + RenderWorld::destroy() ← endMissionLate()
	//   4. Mech::endMission()
	GameAdapters::StaticProp::endMissionEarly();   // M1 Task 13
	GpuMechBatcher::instance().onMapUnload();
	GameAdapters::StaticProp::endMissionLate();    // M1 Task 13
	GameAdapters::Mech::endMission();               // M2: mech lifecycle
	GameAdapters::Vegetation::missionUnloaded();    // vegetation card system teardown

	//---------------------------------------------------------------
	// Shutdown the Mission Interface
	if (missionInterface)
	{
		// MISSION-START-HOVER-TARGET-LIFETIME-1: clear the static `target` pointer and the
		// file-scope hover caches at teardown (end boundary). The objects they reference are
		// about to be freed; suppress picking so nothing dereferences them before the next
		// mission arms hover again.
		MissionInterfaceManager::invalidateHoverTarget();
		missionInterface->destroy();

		delete missionInterface;
		missionInterface = NULL;
	}
	
	delete missionBrain;
	missionBrain = NULL;

	delete missionBrainParams;
	missionBrainParams = NULL;

	//Team::home->objectives.Clear();

	//---------------------------------------------------------------
	// End the Camera and Lists
	if (eye)
	{
		eye->destroy();

		delete eye;
		eye = NULL;
		// Null the static TG_Shape camera-matrix aliases that TG_Shape::
		// SetCameraMatrices() (tgl.cpp:1619-1620) populated with raw pointers
		// into the now-freed eye object. Both `s_cameraOrigin` and
		// `s_cameraToClip` outlive their owner; if a subsequent code path
		// (e.g. Mission::load -> MechWarrior::Load -> registerStaticProp ->
		// TG_MultiShape::TransformMultiShape at msl.cpp:1438) dereferences
		// the stale pointer before eye->update() re-primes them, the dereference
		// reads freed heap (latent UB; reliably crashed 2026-05-20 after Block A
		// allocations stomped the freed eye block first). The next eye->update()
		// in Mission::init / Mission::load re-populates both via
		// SetCameraMatrices(). quad.cpp:2008 carries the same dereference and
		// is also covered by this nulling.
		TG_Shape::s_cameraOrigin = NULL;
		TG_Shape::s_cameraToClip = NULL;
	}

	if (PathManager) {
		PathManager->destroy();
		delete PathManager;
		PathManager = NULL;
	}
	MOVE_cleanup();

	MechWarrior::shutdown();
	
	delete weather;
	weather = NULL;

	//------------------------------------------------------------
	// End the Terrain System
	if (land)
	{
		land->destroy();

		delete land;
		land = NULL;
	}

	//--------------------------------------------------------------
	// Shut down the object system
	// CANNOT just delete heaps do to our ASSHOLE MUNGA/ADEPT
	// CODE which cannot be shutdown and restarted more then once.
	//
	// I wish I could just do demos and call that game experience!!!!
	//
	if (ObjectManager)
	{
		ObjectManager->destroy();
		delete ObjectManager;
		ObjectManager = NULL;
	}
	
	if (Mover::sortList)
	{
		delete Mover::sortList;
		Mover::sortList = NULL;
	}

	if (SensorSystem::sortList)
	{
		delete SensorSystem::sortList;
		SensorSystem::sortList = NULL;
	}

	if (GVAppearanceType::SensorTriangleShape)
	{
		delete GVAppearanceType::SensorTriangleShape;
		GVAppearanceType::SensorTriangleShape = NULL;
	}
	
	if (GVAppearanceType::SensorCircleShape)
	{
		delete GVAppearanceType::SensorCircleShape;
		GVAppearanceType::SensorCircleShape = NULL;
	}

	if (Mech3DAppearanceType::SensorSquareShape)
	{
		delete Mech3DAppearanceType::SensorSquareShape;
		Mech3DAppearanceType::SensorSquareShape = NULL;
	}
	
	if (MasterComponent::masterList) 
	{
		MasterComponent::freeMasterList();
	}

	//-----------------------------------
	// Get rid of the Effects manager
	if (weaponEffects)
		weaponEffects->destroy();

	delete weaponEffects;
	weaponEffects = NULL;

	if ( craterManager )
	{
		craterManager->destroy();
		missionHeap->Free( craterManager );
		craterManager = NULL;
	}

	//-----------------------------------
	// Sensors now stored in the missionHeap
	// Which goes away several lines below this
	//if (SensorManager)
	//{
	//	SensorManager->destroy();
	//	delete SensorManager;
	//	SensorManager = NULL;
	//}

	long numt = Team::numTeams;
	for (long i = 0; i < numt; i++)
		if (Team::teams[i]) {
			delete Team::teams[i];
			Team::teams[i] = NULL;
		}
	Team::numTeams = 0;

	long numC = Commander::numCommanders;
	for (int i = 0; i < numC; i++)
		if (Commander::commanders[i]) {
			delete Commander::commanders[i];
			Commander::commanders[i] = NULL;
		}
	Commander::numCommanders = 0;

	if (Mover::triggerAreaMgr) {
		delete Mover::triggerAreaMgr;
		Mover::triggerAreaMgr = NULL;
	}

	if (MoverGroup::goalMap)
	{
		systemHeap->Free(MoverGroup::goalMap);
		MoverGroup::goalMap = NULL;
	}

	if (openList)
	{
		delete openList;
		openList = NULL;
	}

	if (Team::sortList)
	{
		delete Team::sortList;
		Team::sortList = NULL;
	}

	closeABL();

	//------------------------------------------------------------
	// End the Mission Heap
	if (missionHeap)
	{
		missionHeap->destroy();
		
		delete missionHeap;
		missionHeap = NULL;
	}
	
	if ( appearanceTypeList )
	{
		appearanceTypeList->destroy();
		delete appearanceTypeList;
		appearanceTypeList = NULL;
	}
	
	if (initLogistics)
		initTGLForLogistics();
	else
	{
		//---------------------------------------------------------
		// End the Tiny Geometry Layer Heap for Logistics
		if (TG_Shape::tglHeap)
		{
			logTglPoolPeaks("mission_unload");
			//Shut down the TGL RAM pools.
			if (colorPool)
			{
				colorPool->destroy();
				delete colorPool;
				colorPool = NULL;
			}

			if (vertexPool)
			{
				vertexPool->destroy();
				delete vertexPool;
				vertexPool = NULL;
			}

			if (facePool)
			{
				facePool->destroy();
				delete facePool;
				facePool = NULL;
			}

			if (shadowPool)
			{
				shadowPool->destroy();
				delete shadowPool;
				shadowPool = NULL;
			}

			if (trianglePool)
			{
				trianglePool->destroy();
				delete trianglePool;
				trianglePool = NULL;
			}

			TG_Shape::tglHeap->destroy();

			delete TG_Shape::tglHeap;
			TG_Shape::tglHeap = NULL;
		}
	}
	
//	userInput->mouseOff();

	active = FALSE;
	scenarioResult = mis_PLAYING;		//We've completed the mission.  Set it back to playing!
	GeneralAlarm = false;

	EllipseElement::removeTextureHandle();

	//The flush below will remove this one.
	Mover::holdFireIconHandle = 0;

	mcTextureManager->flush();
	mcTextureManager->freeVertices();
	mcTextureManager->freeShapes();

	soundSystem->purgeSoundSystem();

	missionFileName[0] = 0;
	
	if (CObjective::s_markerFont)
	{
		delete CObjective::s_markerFont;
		CObjective::s_markerFont = NULL;
	}

	//Heading back to logistics now.  Change screen back to 800x600
//	if (renderer == 3)
//		gos_SetScreenMode(800,600,16,0,0,0,true,false,0,false,0,renderer);
//	else
//		gos_SetScreenMode(800,600,16,0,0,0,0,false,0,false,0,renderer);

	//Start finding the Leaks
	//systemHeap->dumpRecordLog();
}

//----------------------------------------------------------------------------------

void Mission::handleMultiplayMessage (long code, long param1) {

	if (missionBrainCallback) {
		CurMultiplayCode = code;
		CurMultiplayParam = param1;
		missionBrain->execute(NULL, missionBrainCallback);
		CurMultiplayCode = 0;
		CurMultiplayParam = 0;
	}
}

//----------------------------------------------------------------------------------

void Mission::startObjectiveTimers (void) {

	for (long i=0;i<(long)numObjectives;i++)
	{
		if (objectives[i].timeLeft > 0.0)
		{
			setObjectiveTimer(i,objectives[i].timeLeft);
		}
	}
}

//----------------------------------------------------------------------------------
long Mission::setObjectiveTimer (long objectiveNum, float timeLeft)
{
	gosASSERT((objectiveNum >= 0) && objectiveNum < (long)numObjectives);
		
	long timerNumber = OBJECTIVE_1_TIMER + objectiveNum;
	
	//------------
	// Add Timer.
	TimerPtr timer = timerManager->getTimer(timerNumber);
	gosASSERT(timer != NULL);
	timer->setTimer(timeLeft);

	return(NO_ERR);
}	

//----------------------------------------------------------------------------------
float Mission::checkObjectiveTimer (long objectiveNum)
{
	gosASSERT((objectiveNum >= 0) || objectiveNum < (long)numObjectives);

	long timerNumber = OBJECTIVE_1_TIMER + objectiveNum;
	unsigned long timeLeft = 0;

	TimerPtr timer = timerManager->getTimer(timerNumber);
	gosASSERT(timer != NULL);

	timeLeft = timer->getCurrentTime();
	
	return(timeLeft);
}	

//----------------------------------------------------------------------------------
long Mission::setObjectiveStatus (long objectiveNum, ObjectiveStatus status)
{
	gosASSERT((objectiveNum >= 0) || objectiveNum < (long)numObjectives);
	
	if (objectives)
		objectives[objectiveNum].status = status;
	
	return(NO_ERR);
}	

//----------------------------------------------------------------------------------
bool Mission::checkObjectiveSuccess (void)
{
	//NOW uses NEW(tm) objectives.
	long count = 0;
	bool result = false;
	if (Team::home->objectives.Count())
	{
		for ( CObjectives::EIterator iter = Team::home->objectives.Begin(); !iter.IsDone(); iter++, count++ )
		{
			//Must iterate through ALL objectives and check.
			result |= (*iter)->StatusChangedSuccess();
		}
	}
	
	return result;
}

//----------------------------------------------------------------------------------
bool Mission::checkObjectiveFailed (void)
{
	//NOW uses NEW(tm) objectives.
	long count = 0;
	bool result = false;
	if (Team::home->objectives.Count())
	{
		for ( CObjectives::EIterator iter = Team::home->objectives.Begin(); !iter.IsDone(); iter++, count++ )
		{
			//Must iterate through ALL objectives and check.
			result |= (*iter)->StatusChangedFailed();
		}
	}
	
	return result;
}

//----------------------------------------------------------------------------------
ObjectiveStatus Mission::checkObjectiveStatus (long objectiveNum)
{
	//NOW uses NEW(tm) objectives.
	long count = 0;
	if (Team::home->objectives.Count())
	{
		for ( CObjectives::EIterator iter = Team::home->objectives.Begin(); !iter.IsDone(); iter++, count++ )
		{
			if (count == objectiveNum)
			{
				return (*iter)->Status(Team::home->objectives);
			}
		}
	}
	
	return OS_UNDETERMINED;
}	

//----------------------------------------------------------------------------------
long Mission::setObjectiveType (long objectiveNum, ObjectiveType type)
{
	gosASSERT((objectiveNum >= 0) || objectiveNum < (long)numObjectives);

	if (objectives)
		objectives[objectiveNum].type = type;
	
	return(NO_ERR);
}	

//----------------------------------------------------------------------------------
ObjectiveType Mission::checkObjectiveType (long objectiveNum)
{
	gosASSERT((objectiveNum >= 0) || objectiveNum < (long)numObjectives);
		
	ObjectiveType result = PrimaryGoal;
	
	if (objectives)
		result = objectives[objectiveNum].type;
	
	return(result);
}	

//----------------------------------------------------------------------------------
void Mission::setObjectivePos (long objectiveNum, float realX, float realY, float realZ)
{
	gosASSERT((objectiveNum >= 0) || objectiveNum < (long)numObjectives);
		
	if (objectives)
	{
		objectives[objectiveNum].position.x = realX;
		objectives[objectiveNum].position.y = realY;
		objectives[objectiveNum].position.z = realZ;
	}
}


//---------------------------------------------------------------------------


