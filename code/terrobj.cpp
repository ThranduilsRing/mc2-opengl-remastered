#include "tick_commit.h"
#include "sim_overlap.h"
#include "Gates/preferences_legacy.h"
//---------------------------------------------------------------------------
//
//	terrobj.cpp -- File contains the misc terrain object code
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include Files

#ifndef MCLIB_H
#include"mclib.h"
#include <atomic>
#endif

#ifndef TERROBJ_H
#include"terrobj.h"
#include "Gates/gate.h"
#endif

#include "gos_static_prop_killswitch.h"  // g_useGpuStaticProps
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "../GameOS/gameos/gpu_cull_readback.h"  // Task 5/6: per-actor GPU visibility readback

#ifndef GAMESOUND_H
#include"gamesound.h"
#endif

#ifndef SOUNDS_H
#include"sounds.h"
#endif

#ifndef MOVE_H
#include"move.h"
#endif

#ifndef TEAM_H
#include"team.h"
#endif

#ifndef COLLSN_H
#include"collsn.h"
#endif

#ifndef MULTPLYR_H
#include"multplyr.h"
#endif

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef CARNAGE_H
#include"carnage.h"
#endif

#ifndef DOBJNUM_H
#include"dobjnum.h"
#endif

#ifndef GOS_PROFILER_H
#include"gos_profiler.h"
#endif

#include"objectappearance.h"
#include "static_update_counters.h"
#include "bldg_anim_gate_counters.h"
#include "frame_jobs.h"  // FRAME-JOBS-1: frameJobsEnabled() gate
#include "../RenderCore/axis_convert.h"
#include "mc2fmt.h"

//---------------------------------------------------------------------------
// Slice 3 Static-Update Bypass instrumentation (worktree CLAUDE.md
// §"Tier-1 Instrumentation Env Vars" pattern; mirrors MC2_DESTROY_TRACE in
// gameobj.cpp:104). Counters defined here because the gate lives in this TU;
// objmgr.cpp reads them via the accessors declared in static_update_counters.h.
//---------------------------------------------------------------------------
extern std::atomic<uint32_t> g_mc2FrameCounter;  // defined in mclib/tgl.cpp:3718

// Std row: unset reads the row default, "0"/"false"/"off"/"no" disable, anything else enables.
// Do NOT regress to a presence test -- that would treat =0 as ENABLED (the GPU_OBJECTS class of bug).
static const bool s_staticUpdateTrace = mc2::gate::get<"MC2_STATIC_UPDATE_TRACE">();

namespace {
struct StaticUpdateCounters {
    uint32_t objects_seen;       // TerrainObject::update() entries with appearance && inView
    uint32_t updates_run;        // appearance->update() actually called
    uint32_t updates_skipped;    // appearance->update() short-circuited by IsStaticNow()
    uint32_t dyn_falling;        // appearance class said static, but OBJECT_FLAG_FALLING set
    uint32_t dyn_other;          // reserved for Stage 3.D building disqualifiers
};
StaticUpdateCounters g_staticUpdateCounters = {};
StaticUpdateCounters g_staticUpdateLastSummary = {};
uint32_t g_staticUpdateLastSummaryFrame = 0;

// [APPEAR_ROUTE v1] Per-appearance-class routing coverage map.
// Bucketed by AppearanceType::getAppearanceClass() (high byte of appearanceNum).
// Tracks how many touch (skip) vs update (full) events each class accumulates.
// Use case: under MC2_STATIC_UPDATE_SKIP=1 + MC2_FORCE_DYNAMIC_TREES=1 +
// MC2_FORCE_DYNAMIC_BUILDINGS=1, this map shows whether the FORCE_DYNAMIC envs
// actually bypassed those classes, AND which OTHER classes are still hitting
// the touch path (potential sources of "still black" symptoms).
// Class IDs: see mclib/daprtype.h (BLDG_TYPE=0x0e, BUILDING_APPR_TYPE=0x10,
// TREE_APPR_TYPE=0x11, GENERIC_APPR_TYPE=0x14, etc.).
// Always-on (cheap: two array increments per gate event); summary emitted by
// g_staticUpdateEmitSummary every 600 frames.
uint64_t g_routeTouchByClass[256]  = {0};
uint64_t g_routeUpdateByClass[256] = {0};
}  // namespace

// External accessors declared in code/static_update_counters.h. Definitions live
// here because the counter state is file-private to this TU.
uint32_t g_staticUpdateRunCount()      { return g_staticUpdateCounters.updates_run; }
uint32_t g_staticUpdateSkipCount()     { return g_staticUpdateCounters.updates_skipped; }
uint32_t g_staticUpdateSeenCount()     { return g_staticUpdateCounters.objects_seen; }
uint32_t g_staticUpdateFallingCount()  { return g_staticUpdateCounters.dyn_falling; }

uint32_t g_staticUpdateLastSummaryFrame_get() { return g_staticUpdateLastSummaryFrame; }

void g_staticUpdateEmitSummary(uint32_t frame) {
    const StaticUpdateCounters& cur  = g_staticUpdateCounters;
    const StaticUpdateCounters& prev = g_staticUpdateLastSummary;
    char buf[256];
    (*std::format_to_n(buf, (sizeof(buf)) - 1,
        "[STATIC_UPDATE v1] frame={:d} seen={:d} run={:d} skip={:d} dyn_falling={:d} dyn_other={:d} delta_seen={:d} delta_run={:d} delta_skip={:d}",
        mc2::fmt::as_u32(frame),
        mc2::fmt::as_u32(cur.objects_seen), mc2::fmt::as_u32(cur.updates_run), mc2::fmt::as_u32(cur.updates_skipped),
        mc2::fmt::as_u32(cur.dyn_falling), mc2::fmt::as_u32(cur.dyn_other),
        mc2::fmt::as_u32(cur.objects_seen   - prev.objects_seen),
        mc2::fmt::as_u32(cur.updates_run    - prev.updates_run),
        mc2::fmt::as_u32(cur.updates_skipped - prev.updates_skipped)).out = '\0');
    MC2_DIAG_PUTS(buf);
    fflush(stdout);

    // [APPEAR_ROUTE v1] Per-class coverage map. One line per non-zero class.
    // class=0x0e (BLDG_TYPE), 0x10 (BUILDING_APPR_TYPE), 0x11 (TREE_APPR_TYPE),
    // 0x12 (VEHICLE_APPR_TYPE), 0x13 (MECH_APPR_TYPE), 0x14 (GENERIC_APPR_TYPE),
    // 0x05 (GV_TYPE), 0x0d (MECH_TYPE), 0xFF (no AppearanceType resolved).
    for (int c = 0; c < 256; ++c) {
        if (g_routeTouchByClass[c] || g_routeUpdateByClass[c]) {
            MC2_DIAGF("[APPEAR_ROUTE v1] frame=%u class=0x%02x touch=%llu update=%llu\n",
                frame, c,
                static_cast<unsigned long long>(g_routeTouchByClass[c]),
                static_cast<unsigned long long>(g_routeUpdateByClass[c]));
        }
    }
    fflush(stdout);

    // [ANIM_GATE v1] BLDG-TYPE-ANIM-GATE-FIX-1 event counters.
    // typeIdleNowStatic_delta: touch() calls for idle registered anim-type buildings;
    //   nonzero proves the fix is exercised.
    // animStartInvalidated_delta: reg-clear events on idle->animated transition;
    //   may be zero in smoke missions if no registered-static building animates.
    // animStateToState_delta: setGesture calls between non-idle states (e.g. gates).
    {
        static uint32_t s_prevTypeIdle  = 0;
        static uint32_t s_prevAnimStart = 0;
        static uint32_t s_prevStoS      = 0;
        const uint32_t curIdle  = g_bldgAnimGate_typeIdleNowStatic();
        const uint32_t curStart = g_bldgAnimGate_animStartInvalidated();
        const uint32_t curStoS  = g_bldgAnimGate_animStateToState();
        MC2_DIAGF("[ANIM_GATE v1] frame=%u typeIdleNowStatic_delta=%u animStartInvalidated_delta=%u animStateToState_delta=%u\n",
               frame,
               curIdle  - s_prevTypeIdle,
               curStart - s_prevAnimStart,
               curStoS  - s_prevStoS);
        fflush(stdout);
        s_prevTypeIdle  = curIdle;
        s_prevAnimStart = curStart;
        s_prevStoS      = curStoS;
    }

    g_staticUpdateLastSummary = cur;
    g_staticUpdateLastSummaryFrame = frame;
}

// [TOBJSPLIT v1] env-gated RDTSC cost split for the recalcBounds slice.
// MC2_TOBJ_COST_SPLIT=1 -> partition GameLogic.Units.TerrainObjects into
// ANGULAR (kept coarse clip) / PROJ (to-delete projection body) / UPDATE
// (appearance->update refill). RDTSC only; chrono per-call is observer-effect
// dominated here (cost_split_instrumentation_is_observer_effect_dominated.md).
// RDTSC per-leaf bracket is a deliberate sanctioned exception to the Tracy
// 100ns-floor prohibition (same class as [SLIMSPLIT v1] in terrain.cpp and
// [LIGHT_COST_SPLIT v1] in tgl.cpp): these per-object hot-loop costs are too
// fine-grained for a Tracy zone but the RDTSC pairs are ~5-10ns and gated.
// Demote-not-delete after the attribution lands (debug_instrumentation_rule.md).
// Accumulators defined here (terrobj.cpp) so objmgr.cpp can call the once-
// per-frame roll via g_tobjSplitRollAndMaybeEmit() (static_update_counters.h).
// Probe points in mclib/bdactor.cpp use extern-declarations to reach these.
#include "platform_intrinsics.h"
#include <stdlib.h>
#include <stdio.h>
static bool s_tobjSplitEnabled = mc2::gate::get<"MC2_TOBJ_COST_SPLIT">();
std::atomic<unsigned long long> g_tobjAngularCyc{0ULL};
std::atomic<unsigned long long> g_tobjProjCyc{0ULL};
unsigned long long g_tobjUpdateCyc  = 0ULL;
static unsigned long long g_tobjFrameCount = 0ULL;

void g_tobjSplitRollAndMaybeEmit() {
    if (!s_tobjSplitEnabled) return;
    // 120-frame interval (not 600): the smoke is hard-capped at 30s; a 600-frame
    // window requires ~10s+ and may not complete in a short mission segment.
    // 120 frames (~2s at 60fps) guarantees several summary lines per 30s run.
    // SLIMSPLIT precedent uses 600 (long uninterrupted missions); here the probe
    // runs under a time-bounded smoke gate so the interval is shortened.
    if (++g_tobjFrameCount % 120ULL == 0ULL) {
        // Per-frame normalization matches SLIMSPLIT (terrain.cpp:1492-1498):
        // raw N-frame integrals are uninterpretable without the window and not
        // comparable across different interval sizes; dividing by the fixed
        // window makes values legible and SLIMSPLIT-comparable. The PROJ/
        // (ANGULAR+PROJ+UPDATE) ratio is interval-invariant so this does NOT
        // alter the Stage-0.5 gate result -- it only makes future captures
        // legible.
        const double f = 120.0;
        fprintf(stderr, "[TOBJSPLIT v1] event=summary frames=120 "
               "angular_cyc_per_frame=%.0f proj_cyc_per_frame=%.0f update_cyc_per_frame=%.0f\n",
               static_cast<double>(g_tobjAngularCyc) / f,
               static_cast<double>(g_tobjProjCyc) / f,
               static_cast<double>(g_tobjUpdateCyc) / f);
        fflush(stderr);
        g_tobjAngularCyc.store(0ULL, std::memory_order_relaxed);
        g_tobjProjCyc.store(0ULL, std::memory_order_relaxed);
        g_tobjUpdateCyc = 0ULL;
    }
}

// [TOBJPARITY v1] env-gated STANDALONE readback-vs-coarse superset-parity
// instrument. NOT a passing gate for any shipped slice -- the readback
// render/shadow repoint was REVERTED (de-risk): Task 7 empirically proved
// the GPU readback is NOT a superset of the coarse visible set (it drops
// 10-60% of in-view terrain statics), so gating render/shadow on it is
// unsound. Render/shadow run on the coarse canBeSeen()/inView path again.
//
// This probe survives, demoted-not-deleted, purely to QUANTIFY that
// readback non-superset for the separately-tracked GPU-path meta-fix
// task (make the GPU cull authoritative and delete the CPU approximation
// gate). MC2_TOBJ_PARITY=1 -> per terrain static per frame, in
// TerrainObject::update(), inlining the per-actor lagged readback
// expression (fail-open when readback disabled) and comparing it to the
// LOCAL coarse `inView` from recalcBounds():
//   samples    = terrain statics where coarse inView is true (denominator).
//   violations = coarseInView && !readbackVisible (the dropped-prop class
//                the deferred GPU-path meta-fix must eliminate).
//
// Legitimate over-inclusion (readbackVisible && !coarseInView) is fine and
// NOT a violation. Counter never chrono; no std::chrono, no RDTSC -- pure
// event count (cost_split_instrumentation_is_observer_effect_dominated.md).
//
// Transitive-dependency note: this probe measures readback-vs-coarse
// (readback >= coarse). Coarse >= projected is independently proven -- the
// deleted projection block (Tasks 2/3) was entirely `if(inView)`-gated and
// could only narrow the visible set, never widen it. The known result here
// is NON-zero violations (readback is a non-superset by Task 7); the probe
// quantifies the magnitude for the meta-fix task. It is NOT a pass/fail
// gate for this de-risked slice.
//
// Demote-not-delete (debug_instrumentation_rule.md).
// Mirrors [TOBJSPLIT v1]: file-static enable flag, same 120-frame roll
// interval, accumulators defined here, roll called from objmgr.cpp.
static bool s_tobjParityEnabled = mc2::gate::get<"MC2_TOBJ_PARITY">();
static unsigned long long g_tobjParitySamples   = 0ULL;
static unsigned long long g_tobjParityViolation = 0ULL;
static unsigned long long g_tobjParityFrameCount = 0ULL;

void g_tobjParityRollAndMaybeEmit() {
    if (!s_tobjParityEnabled) return;
    // 120-frame interval: matches [TOBJSPLIT v1] (see rationale above).
    if (++g_tobjParityFrameCount % 120ULL == 0ULL) {
        fprintf(stderr, "[TOBJPARITY v1] event=summary samples=%llu violations=%llu\n",
                g_tobjParitySamples, g_tobjParityViolation);
        fflush(stderr);
        g_tobjParitySamples   = 0ULL;
        g_tobjParityViolation = 0ULL;
    }
}

extern unsigned long NextIdNumber;
extern float worldUnitsPerMeter;
extern bool drawExtents;
extern bool somethingOnFire;
extern bool	useOldProject;
extern bool MLRVertexLimitReached;

inline float agsqrt( float _a, float _b )
{
	return sqrt(_a*_a + _b*_b);
}

constexpr float TREE_FALL_RATE = 15.0f;
#define TREE_FALL_ACCEL		5.0f;

char lastName[256];

extern MidLevelRenderer::MLRClipper * theClipper;
//---------------------------------------------------------------------------
// class TerrainObjectType
//---------------------------------------------------------------------------

GameObjectPtr TerrainObjectType::createInstance (void) {

	TerrainObjectPtr result = new TerrainObject;
	if (!result)
		return nullptr;

	result->init(true, this);
	//result->setIdNumber(NextIdNumber++);
	
	return(result);
}

//---------------------------------------------------------------------------

void TerrainObjectType::init (void) {
			
	objectTypeClass = TERRAINOBJECT_TYPE;
	objectClass = TERRAINOBJECT;
			
	subType = TERROBJ_NONE;
	damageLevel = 0.0;
	collisionOffsetX = 0;
	collisionOffsetY = 0;
	setImpassable = false;
	xImpasse = 0;
	yImpasse = 0;
	extentRadius = -1.0;
	explDmg = 0.0;
	explRad = 0.0;
	fireTypeHandle = -1;
}

//---------------------------------------------------------------------------

void TerrainObjectType::destroy (void) 
{
	ObjectType::destroy();
}
		
//---------------------------------------------------------------------------

void TerrainObjectType::initMiscTerrObj (long objTypeNum) {

	//---------------------------------------------------------------------
	// This function is here to maintain compatibility with MC1. The values
	// used for the various miscTerrainTypes are hardcoded, based on
	// MC1 ship values. If we need to modify 'em, then try adding new
	// object types to the packet file!
	explosionObject = 0xFFFFFFFF;
	destroyedObject = 0xFFFFFFFF;
	extentRadius = -1.0;
	keepMe = true;
	iconNumber = -1;
	teamId = -1;

	if (objTypeNum == ObjectTypeManager::bridgeTypeHandle) {
		subType = TERROBJ_BRIDGE;
		damageLevel = 100.0;
		}
	else if (objTypeNum == ObjectTypeManager::forestTypeHandle) {
		subType = TERROBJ_FOREST;
		damageLevel = 100.0;
		fireTypeHandle = 1;
		}
	else if (objTypeNum == ObjectTypeManager::wallHeavyTypeHandle) {
		subType = TERROBJ_WALL_HEAVY;
		damageLevel = 100.0;
		}
	else if (objTypeNum == ObjectTypeManager::wallMediumTypeHandle) {
		subType = TERROBJ_WALL_MEDIUM;
		damageLevel = 100.0;
		}
	else if (objTypeNum == ObjectTypeManager::wallLightTypeHandle) {
		subType = TERROBJ_WALL_LIGHT;
		damageLevel = 100.0;
		}
	else
		Fatal(objTypeNum, " TerrainObjectType.init: bad MiscTerrainObj num ");
}

//---------------------------------------------------------------------------

long TerrainObjectType::init (FilePtr objFile, unsigned long fileSize) {

	long result = 0;
	
	FitIniFile bldgFile;
	result = bldgFile.open(objFile, fileSize);
	if (result != NO_ERR)
		return(result);
	
	//----------------------------------------------
	// Read in the data needed for the TerrainObject
	subType = TERROBJ_NONE;
	result = bldgFile.seekBlock("TerrainObjectData");
	if (result != NO_ERR) {
		result = bldgFile.seekBlock("TreeData");
		if (result != NO_ERR)
			return(result);
		subType = TERROBJ_TREE;
		objectClass = TREE;
	}

	unsigned long dmgLevel;
	result = bldgFile.readIdULong("DmgLevel",dmgLevel);
	if (result != NO_ERR)
		return(result);
	damageLevel = static_cast<float>(dmgLevel);
		
	result = bldgFile.readIdLong("CollisionOffsetX",collisionOffsetX);
	if (result != NO_ERR)
		collisionOffsetX = 0;
	
	result = bldgFile.readIdLong("CollisionOffsetY",collisionOffsetY);
	if (result != NO_ERR)	
		collisionOffsetY = 0;

	long setImpass;
	result = bldgFile.readIdLong("SetImpassable",setImpass);
	setImpassable = false;
	if (result == NO_ERR)
		setImpassable = setImpass ? true : false;
		
	result = bldgFile.readIdLong("XImpasse",xImpasse);
	if (result != NO_ERR)
		xImpasse = 0;
	
	result = bldgFile.readIdLong("YImpasse",yImpasse);
	if (result != NO_ERR)	
		yImpasse = 0;

	float realExtent = 0.0;
	result = bldgFile.readIdFloat("ExtentRadius", realExtent);
	if (result != NO_ERR)
		realExtent = -1.0;

	result = bldgFile.readIdFloat("ExplosionRadius",explRad);
	//-----------------------------------------------------------------
	// if this fails, explosion radius is not set and no splash damage.
	if (result != NO_ERR)
		explRad = 0.0;
		
	result = bldgFile.readIdFloat("ExplosionDamage",explDmg);
	// if this fails, explosion damage is not set and no splash damage.
	if (result != NO_ERR)
		explDmg = 0.0;

	//-------------------------------------------------------
	// Initialize the base object Type from the current file.
	result = ObjectType::init(&bldgFile);
	extentRadius = realExtent;

	return(result);
}
	
//---------------------------------------------------------------------------

bool TerrainObjectType::handleCollision (GameObjectPtr collidee, GameObjectPtr collider) {

	if (MPlayer && !MPlayer->isServer())
		return(true);

	switch (subType) {
		case TERROBJ_NONE:
			//-------------------------------------------------------
			// The Building ceases to exist when its effect is done.
			// always return FALSE or the collision will make it
			// go away!  We may want to play a sound effect here.
			switch (collider->getObjectClass()) {
				case EXPLOSION:
				case BATTLEMECH:
				case GROUNDVEHICLE:
				if (!collider->isMover() || (collider->isMover() && (static_cast<MoverPtr>(collider))->pathLocks))
				{
					WeaponShotInfo shot;
					shot.init(0, -1, collidee->getDamageLevel(), 0, 0);
					collidee->handleWeaponHit(&shot, (MPlayer != nullptr));
				}
				break;
			}
			break;
		case TERROBJ_TREE:
			//-------------------------------------------------------
			// Trees are magical.  If a mech hits one, it goes down
			// and is replaced by its last frame.  In other words,
			// play the animation and stop on last frame.
			//
			// When a tree falls, we change its frame_of_ref to match
			// the direction it should fall in from the nominal world frame.
			//
			switch (collider->getObjectClass()) {
				case EXPLOSION:
				case BATTLEMECH:
				case GROUNDVEHICLE:
				if (!collider->isMover() || (collider->isMover() && (static_cast<MoverPtr>(collider))->pathLocks))
				{
					TerrainObjectPtr tree = static_cast<TerrainObjectPtr>(collidee);
					if (!tree->getFlag(OBJECT_FLAG_FALLEN) && !tree->getFlag(OBJECT_FLAG_FALLING)) {
						tree->setFlag(OBJECT_FLAG_FALLING, true);
						
						float fallAngle = collidee->relFacingTo(collider->getPosition());
						collidee->rotate(fallAngle,0.0f);
						
						//------------------------------------------------------
						// Tree has fallen.  You may no longer collide with it.
						tree->setTangible(false);
					}
					}
					break;
			}
			break;
		case TERROBJ_BRIDGE:
		case TERROBJ_FOREST:
		case TERROBJ_WALL_HEAVY:
		case TERROBJ_WALL_MEDIUM:
			break;
		case TERROBJ_WALL_LIGHT:
			switch (collider->getObjectClass()) {
				case BATTLEMECH:
				case GROUNDVEHICLE: {
					WeaponShotInfo shotInfo;
					shotInfo.init(collider->getWatchID(), -1, 250.0, 0, 0);
					if (MPlayer) {
						if (MPlayer->isServer()) {
							collidee->handleWeaponHit(&shotInfo, true);
						}
						}
					else
						collidee->handleWeaponHit(&shotInfo);
					}
					break;
			}
			break;
	}
	
	return(true);
}

//---------------------------------------------------------------------------

bool TerrainObjectType::handleDestruction (GameObjectPtr collidee, GameObjectPtr collider) {

	TerrainObjectPtr me = static_cast<TerrainObjectPtr>(collidee);
	if (me->getObjectType()->getSubType() == TERROBJ_FOREST) {
		me->openSubAreas();
	}
	return(false);
}

//***************************************************************************
// class TerrainObject
//***************************************************************************
void TerrainObject::rotate (float yaw, float pitch)
{
	rotation = yaw;
	pitchAngle = pitch;
}

void TerrainObject::updateDebugWindow (GameDebugWindow* debugWindow) {

	debugWindow->clear();
	char s[128];
	//-------------------------------------------------------
	// For now, show the floating help text if we have one...
	if ((static_cast<ObjectAppearance*>(appearance))->objectNameId != -1) {
		char myName[255];
		cLoadString((static_cast<ObjectAppearance*>(appearance))->objectNameId, myName, 254);
		debugWindow->print(myName);
		}
	else
		debugWindow->print("<no name>");
	(*std::format_to_n(s, sizeof(s) - 1, "team: {:d}, handle: {:d}, partID: {:d} {}", mc2::fmt::as_i32(getTeamId()), mc2::fmt::as_i32(getHandle()), mc2::fmt::as_i32(getPartId()), mc2::fmt::safe(getFlag(OBJECT_FLAG_CAPTURABLE) ? "[C]" : " ")).out = '\0');
	debugWindow->print(s);
	(*std::format_to_n(s, sizeof(s) - 1, "objType: {:d}", mc2::fmt::as_i32(getObjectType()->whatAmI())).out = '\0');
	debugWindow->print(s);
	(*std::format_to_n(s, sizeof(s) - 1, "damage: {:.2f}/{:.2f}", getDamage(), getDamageLevel()).out = '\0');
	debugWindow->print(s);
	(*std::format_to_n(s, sizeof(s) - 1, "pos: [{:d}, {:d}](area = {:d})", mc2::fmt::as_i32(cellPositionRow), mc2::fmt::as_i32(cellPositionCol), mc2::fmt::as_i32(GlobalMoveMap[0]->calcArea(cellPositionRow, cellPositionCol))).out = '\0');
	debugWindow->print(s);
	(*std::format_to_n(s, sizeof(s) - 1, "footprint:[{:d},{:d}]:[{:d},{:d}]", mc2::fmt::as_i32(cellFootprint[0]), mc2::fmt::as_i32(cellFootprint[1]), mc2::fmt::as_i32(cellFootprint[2]), mc2::fmt::as_i32(cellFootprint[3])).out = '\0');
	debugWindow->print(s);
	if (numSubAreas0 > 0) {
		(*std::format_to_n(s, sizeof(s) - 1, "subAreas:").out = '\0');
		for (long i = 0; i < numSubAreas0; i++) {
			char tempStr[15];
			(*std::format_to_n(tempStr, sizeof(tempStr) - 1, " {:d}", mc2::fmt::as_i32(subAreas0[i])).out = '\0');
			strcat(s, tempStr);
		}
		strcat(s, " *");
		for (int i = 0; i < numSubAreas1; i++) {
			char tempStr[15];
			(*std::format_to_n(tempStr, sizeof(tempStr) - 1, " {:d}", mc2::fmt::as_i32(subAreas1[i])).out = '\0');
			strcat(s, tempStr);
		}
		debugWindow->print(s);
	}
}

//---------------------------------------------------------------------------

bool TerrainObject::isVisible (void) {

	//----------------------------------------------------------------------
	// This function is the meat and potatoes of the object cull system.
	// Its job is to determine if the object is on screen or not.
	// It does this by transforming the position for each active camera to
	// its screen coords and saving them.  It then checks each set of coords
	// to see if they are in the viewport of each camera.  Returned value
	// is number of windows that object can be seen in.
	bool isVisible = false; //land->getVertexScreenPos(blockNumber, vertexNumber, screenPos);
	if (appearance)
		isVisible = appearance->recalcBounds();
	
	if (isVisible) {
		windowsVisible = turn;
		return(true);
	}
	return(false);
}

//---------------------------------------------------------------------------

char* TerrainObject::getName (void) {

	/*
	static char* terrainObjectNames[NUM_TERROBJ_SUBTYPES] = {
		"Nothing",
		"Tree",
		"Bridge",
		"Forest",
		"Heavy Wall",
		"Medium Wall",
		"Light Wall"
	};

	TerrainObjectTypePtr type = (TerrainObjectTypePtr)getObjectType();
	return(terrainObjectNames[type->subType]);
	*/

	if ((static_cast<ObjectAppearance*>(appearance))->objectNameId != -1) {
		cLoadString((static_cast<ObjectAppearance*>(appearance))->objectNameId, lastName, 254);
		return(lastName);
	}
	return(nullptr);
}

//---------------------------------------------------------------------------

float TerrainObject::getStatusRating (void) {

	float curDamage = getDamage();
	float maxHealth = getDamageLevel();
	float rating = (maxHealth - curDamage) / maxHealth;
	if (rating < 0.0)
		rating = 0.0;
	return(rating);
}

//---------------------------------------------------------------------------

constexpr int BRIDGE_OFFSET = 60;

static long terrainObjectHomeRelation (long teamId)
{
	if (!Team::home)
		return 0;

	return Team::getRelation(teamId, Team::home->getId());
}

void TerrainObject::primeAppearanceForMissionLoad (void)
{
	// PERF 2026-05-07: stripped high-frequency Tracy zones from TerrainObject:
	// primeAppearanceForMissionLoad, justCreated, appearanceSetup, recalcBounds;
	// update, justCreated, appearanceSetup, recalcBounds, appearanceUpdate, dustEffect.
	if (getFlag(OBJECT_FLAG_JUSTCREATED))
	{
		setFlag(OBJECT_FLAG_JUSTCREATED, false);
		setFlag(OBJECT_FLAG_TILECHANGED, false);

		TerrainObjectTypePtr type = static_cast<TerrainObjectTypePtr>(ObjectManager->getObjectType(typeHandle));

		switch (type->subType) {
			case TERROBJ_NONE:
			case TERROBJ_TREE: 
			{
				setTangible(true);
			}
			break;
			
			case TERROBJ_BRIDGE:
				if (!GameMap->getPassable(cellPositionRow, cellPositionCol)) {
					damage = type->getDamageLevel();
					setStatus(OBJECT_STATUS_DESTROYED);
				}
				break;
			case TERROBJ_FOREST:
			case TERROBJ_WALL_HEAVY:
			case TERROBJ_WALL_MEDIUM:
			case TERROBJ_WALL_LIGHT:
				if (GameMap->getPassable(cellPositionRow, cellPositionCol)) {
					damage = type->getDamageLevel();
					setStatus(OBJECT_STATUS_DESTROYED);
				}
				break;
		}

	}

	//-------------------------------------------
	// Handle power out.
	if (appearance && powerSupply)
	{
		GameObjectPtr powerObject = ObjectManager->getByWatchID(powerSupply);
		if (powerObject && (powerObject->getStatus() == OBJECT_STATUS_DESTROYED))
			appearance->setLightsOut(true);
	}

	if (!appearance)
		return;

	{
		appearance->setObjectParameters(position,rotation,FALSE,getTeamId(),terrainObjectHomeRelation(getTeamId()));
		appearance->setMoverParameters(pitchAngle);
		if (appearance->getAppearanceClass() == TREE_APPR_TYPE)
			static_cast<TreeAppearance*>(appearance)->setShadowMoving(getFlag(OBJECT_FLAG_FALLING));
	}

	bool inView = false;
	{
		inView = appearance->recalcBounds();
	}
	appearance->setInView(inView);
}

// Presentation phase: the draw-side parameters the tick no longer pushes (power-out lights, tree pitch and shadow motion).
// Called for every terrain object the presentation pass visits; idempotent.
void TerrainObject::applyAppearancePush (void)
{
	if (!appearance)
		return;
	if (powerSupply)
	{
		GameObjectPtr powerObject = ObjectManager->getByWatchID(powerSupply);
		if (powerObject && (powerObject->getStatus() == OBJECT_STATUS_DESTROYED))
			appearance->setLightsOut(true);
	}
	if (appearance->getAppearanceClass() == TREE_APPR_TYPE)
	{
		appearance->setMoverParameters(pitchAngle);
		static_cast<TreeAppearance*>(appearance)->setShadowMoving(getFlag(OBJECT_FLAG_FALLING));
	}
}

long TerrainObject::update (void) {

	if (getFlag(OBJECT_FLAG_JUSTCREATED))
	{
		setFlag(OBJECT_FLAG_JUSTCREATED, false);
		setFlag(OBJECT_FLAG_TILECHANGED, false);

		TerrainObjectTypePtr type = static_cast<TerrainObjectTypePtr>(ObjectManager->getObjectType(typeHandle));

		switch (type->subType) {
			case TERROBJ_NONE:
			case TERROBJ_TREE: 
			{
				setTangible(true);
			}
			break;
			
			case TERROBJ_BRIDGE:
				if (!GameMap->getPassable(cellPositionRow, cellPositionCol)) {
					damage = type->getDamageLevel();
					setStatus(OBJECT_STATUS_DESTROYED);
				}
				break;
			case TERROBJ_FOREST:
			case TERROBJ_WALL_HEAVY:
			case TERROBJ_WALL_MEDIUM:
			case TERROBJ_WALL_LIGHT:
				if (GameMap->getPassable(cellPositionRow, cellPositionCol)) {
					damage = type->getDamageLevel();
					setStatus(OBJECT_STATUS_DESTROYED);
				}
				break;
		}

	}

	//-------------------------------------------
	// Handle power out. The tick leaves the lights to applyAppearancePush (presentation); a presentation-pass update() applies them here.
	if (appearance && powerSupply && simPresentationPass)
	{
		GameObjectPtr powerObject = ObjectManager->getByWatchID(powerSupply);
		if (powerObject && (powerObject->getStatus() == OBJECT_STATUS_DESTROYED))
			appearance->setLightsOut(true);
	}

 	if (appearance)
	{
		if (getFlag(OBJECT_FLAG_FALLING))
		{
			if (fallRate == 0.0f)
			{
				if (useSound && soundSystem)
					soundSystem->playDigitalSample(TREEFALL, getPosition(), true);
					
				fallRate = TREE_FALL_RATE;
			}
			else
				fallRate += TREE_FALL_ACCEL;
				
			pitchAngle -= (frameLength * fallRate);
			if (pitchAngle < -85.0f)
			{
				setFlag(OBJECT_FLAG_FALLEN,true);
				setFlag(OBJECT_FLAG_FALLING,false);
			}
		}

		// Falling and terrain-map effects above are simulation. Geometry and
		// dust effects are refreshed by the presentation pass.
		if (!simPresentationPass)
		{
			// The tick feeds only the sim half of a building-class appearance. A tree has no sim state: its draw parameters
			// (pitch, shadow motion) are applied by applyAppearancePush.
			if (appearance->getAppearanceClass() != TREE_APPR_TYPE)
			{
				Appearance::SimInput simInput;
				simInput.position = position;
				simInput.rotation = rotation;
				simInput.torsoRotation = pitchAngle;
				appearance->setSimInput(simInput);
			}
			return 1;
		}
		{
			appearance->setObjectParameters(position,rotation,FALSE,getTeamId(),terrainObjectHomeRelation(getTeamId()));
			appearance->setMoverParameters(pitchAngle);
			if (appearance->getAppearanceClass() == TREE_APPR_TYPE)
				static_cast<TreeAppearance*>(appearance)->setShadowMoving(getFlag(OBJECT_FLAG_FALLING));
		}
		bool inView = false;
		{
			if (gpu_cull::readback_isEnabled()) {
				// PERF-OBJECT-ITER-GPU-PORT-1 (CORRECTED): readback is used as a
				// fast-confirm for VISIBLE objects only — skip recalcBounds when
				// the readback confirms the object IS visible (the common case in
				// a dense scene). When readback says invisible (lagged, 1-frame old),
				// fall through to recalcBounds() so objects entering the view via
				// camera pan don't produce a 1-frame transparent pop.
				// BUG: original used readback as sole authority (set renderVisible=false
				// when lagged-invisible), causing center-screen objects to flicker
				// during camera pans (they were off-screen last frame → readback=0 →
				// invisible this frame). This fix restores the correct conservative
				// direction: readback can confirm visibility but NOT deny it.
				const bool lagVis = gpu_cull::readback_isActorVisibleLagged(
				    static_cast<uint32_t>(getHandle()));
				if (lagVis) {
					// Readback confirms visible: skip expensive coarse sphereclip.
					if (appearance) appearance->setVisibilityGatesFromLegacy(true);
					inView = true;
				} else {
					// Readback uncertain: CPU recalcBounds is authoritative.
					// FRAME-JOBS-1: if workers already computed bounds this frame, use cached result.
					// frameJobsEnabled() is false by default — this branch costs one bool read only.
					if (frameJobsEnabled() && appearance->boundsFrame == mc2::overlap::simulationFrame(g_mc2FrameCounter)) {
						inView = appearance->inView;
					} else {
						inView = appearance->recalcBounds();
					}
				}
			} else {
				// FRAME-JOBS-1: if workers already computed bounds this frame, use cached result.
				// frameJobsEnabled() is false by default — this branch costs one bool read only.
				if (frameJobsEnabled() && appearance->boundsFrame == mc2::overlap::simulationFrame(g_mc2FrameCounter)) {
					inView = appearance->inView;
				} else {
					inView = appearance->recalcBounds();
				}
			}
		}
		// gpuVisible extension: unused when readback is on (inView IS the result).
		const bool gpuVisible = !gpu_cull::readback_isEnabled() && false;
		if (inView || gpuVisible)
		{
			// MOUSE-PICK PATH DEPENDENCY (objmgr consumer). This
			// `windowsVisible = turn;` stamp is read by
			// GameObjectManager::findTerrainObjectByMouse via the
			// `getWindowsVisible() == (turn - VISIBLE_THRESHOLD)`
			// equality (code/objmgr.cpp). Post-Task-2/3 the
			// recalcBounds projection body is DELETED, so `inView`
			// here is now COARSE-ANGULAR-ONLY -- a strict SUPERSET of
			// the old on-screen set. That is intentional and load-
			// bearing: the stamp must still fire for every pick-
			// eligible object so the equality holds; the precise
			// screen-rect filtering moved to a lazy per-click
			// projection + geometry-space PerPolySelect on the
			// consumer side. DO NOT gate this stamp narrower than
			// `inView` (e.g. do not reintroduce a screen-rect or
			// projection test here) -- doing so silently drops
			// pick-eligible buildings/props from selection.
			windowsVisible = turn;
			{
				++g_staticUpdateCounters.objects_seen;

				// Stage 3.B static-update bypass.
				//
				// Composition: appearance->IsStaticNow() (type-time appearance claim)
				// AND !getFlag(OBJECT_FLAG_FALLING) (owner-side instance state).
				//
				// The owner-side check is MANDATORY: OBJECT_FLAG_FALLING is set
				// externally by collision callbacks (terrobj.cpp:352-353 for trees,
				// bldng.cpp:1453-1455 for buildings). A predicate-only gate silently
				// skips the impact frame's fall-animation setup.
				//
				// Stage 3.D will extend this with damage/power/effect checks for
				// BldgAppearance. Keep this composition pattern explicit here.
				// Pending damage/activity can create FX that require the full update.
				// Consume it before asking whether this appearance is still static.
				if (simPresentationPass) appearance->presentEffects();
				const bool appearanceClaimsStatic = appearance->IsStaticNow();
				const bool ownerForcesDynamic     = getFlag(OBJECT_FLAG_FALLING);
				// Renderer-session guard: skip only when a GPU static/object path is
				// enabled. This does not prove this individual appearance was submitted
				// successfully; TreeAppearance::IsStaticNow() also checks
				// needsFullBakeNextFrame so late registration/recovery frames continue
				// to run update().

				// Stage 3.C: clear static registration on dynamic transition so the
				// first post-fall frame gets a full bake and re-registers the final pose.
				if (ownerForcesDynamic)
					appearance->invalidateStaticRegistration();

				// [APPEAR_ROUTE v1] resolve appearance class once per gate event.
				// 0xFF means "no AppearanceType" (shouldn't happen, but harmless).
				unsigned long _apprClass = 0xFF;
				{
					AppearanceTypePtr _aType = appearance->getAppearanceType();
					if (_aType) _apprClass = _aType->getAppearanceClass() & 0xFF;
				}

				if (appearanceClaimsStatic && !ownerForcesDynamic) {
					++g_staticUpdateCounters.updates_skipped;
					++g_routeTouchByClass[_apprClass];
					if (s_staticUpdateTrace) {
						MC2_DIAGF("[STATIC_UPDATE v1] event=skip frame=%u obj=%p class=0x%02lx\n",
							g_mc2FrameCounter.load(std::memory_order_relaxed), static_cast<void*>(this), _apprClass);
						fflush(stdout);
					}
					appearance->touch();  // Stage 3.C: advance lastTurnTransformed
				} else {
					++g_staticUpdateCounters.updates_run;
					++g_routeUpdateByClass[_apprClass];
					if (ownerForcesDynamic && appearanceClaimsStatic)
						++g_staticUpdateCounters.dyn_falling;
					// [TOBJSPLIT v1] UPDATE bracket
					{
						unsigned long long _tsU = s_tobjSplitEnabled ? __rdtsc() : 0ULL;
						appearance->update();
						if (s_tobjSplitEnabled) g_tobjUpdateCyc += __rdtsc() - _tsU;
					}
				}
			}

			if (bldgDustPoofEffect && bldgDustPoofEffect->IsExecuted())
			{
				glm::vec3			actualPosition;
				glm::mat4 	shapeOrigin;
				glm::mat4 	localToWorld;
					
				mc2ToRender(actualPosition, position);
			
 				mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
				mc2::math::buildTranslation(shapeOrigin, actualPosition);
		
				mc2::math::Obb boundingBox;
				gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
		
				bool result = bldgDustPoofEffect->Execute(&info);
				if (!result)
				{
					bldgDustPoofEffect->Kill();
					bldgDustPoofEffect.reset();
				}
			}
		}

		// [TOBJPARITY v1] readback-vs-coarse superset-parity instrument
		// (env-gated MC2_TOBJ_PARITY=1). Render/shadow are NOW gated on
		// readback (Item 3 repoint above). This probe remains to QUANTIFY
		// residual violation rate (coarseInView && !readbackVisible) after
		// the projScale + getRadius() fixes. Violations = objects that the
		// coarse angular cone sees but GPU cull culls; with dilation these
		// should be near zero on a stationary camera and spike only on
		// rapid pan (1-frame readback lag). Demote-don't-delete once the
		// GPU cull is confirmed authoritative for a full release cycle.
		if (s_tobjParityEnabled && inView) {
			bool readbackVisible = gpu_cull::readback_isEnabled()
				? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(getHandle()))
				: true;
			++g_tobjParitySamples;
			if (!readbackVisible)
				++g_tobjParityViolation;
		}
	}

	return(1);
}

//---------------------------------------------------------------------------

// G7b K3: the old render() split at the render extract. The selection bar status is read and the selected/targeted
// reset done here; renderView keeps the main-only parts (GPU readback gate, appearance, dust effect).
void TerrainObject::extractRenderView (mc2::render_view::RenderView& view) {
	GameObject::extractRenderView(view);
	if (getSelected())
	{
		TerrainObjectTypePtr type = static_cast<TerrainObjectTypePtr>(getObjectType());
		float barStatus = 1.0;
		float totalDmgLvl = type->getDamageLevel();
		if (totalDmgLvl > 0.0)
			barStatus -= getDamage() / totalDmgLvl;

		if (barStatus < 0.0)
			barStatus = 0.0;

		view.flags |= mc2::render_view::Bars;
		view.barColor = 0xff7f7f7f;
		view.barStatus = barStatus;
	}
	setSelected(false);		//ALWAYS reset the selected flags.  GUI needs this to work!
	setTargeted( false );	//ALWAYS do it here, too!  Otherwise things may draw FUNNY!
}

void TerrainObject::renderView (const mc2::render_view::RenderView& view) {

	// RENDER-GATE-FIX (mirrors the "CORRECTED" logic in update() line 794-817):
	// Readback can CONFIRM visibility but must NOT DENY it. The readback is lagged
	// (~2 frames); on camera-move frames a center-screen terrain object may have been
	// outside the previous camera frustum, making its lagged result false-invisible.
	// Using readback as sole authority (the pre-fix code above) caused the 1-frame
	// render-skip → visual pop that d65552ab introduced when readback was made default-ON.
	// Fix: OR-merge with canBeSeen() so readback false-invisible never blocks render.
	// If readback says visible → skip canBeSeen (fast path). If readback says invisible
	// → fall through to canBeSeen() (conservative: may over-render, never under-renders).
	bool renderGate;
	if (!gpu_cull::readback_isEnabled()) {
		renderGate = appearance->canBeSeen();
	} else {
		const bool lagVis = gpu_cull::readback_isActorVisibleLagged(
		    static_cast<uint32_t>(getHandle()));
		// Conservative-OR: render if readback OR CPU test says visible.
		renderGate = lagVis || appearance->canBeSeen();
	}
	if (renderGate)
	{
		if (view.has(mc2::render_view::Bars))
		{
			appearance->setBarColor(view.barColor);
			appearance->setBarStatus(view.barStatus);
		}

		//For debug purposes only.  Will crash pause!!	Sorry Heidi!
//		if (windowsVisible != turn)
//			STOP(("Rendering without an update!"));

		appearance->setVisibility(true,true);
		appearance->render();
		
		//------------------------------------------------
		// Render GOSFX
		gosFX::Effect::DrawInfo drawInfo;
		drawInfo.m_clipper = theClipper;
		
		MidLevelRenderer::MLRState mlrState;
		mlrState.SetDitherOn();
		mlrState.SetTextureCorrectionOn();
		mlrState.SetZBufferCompareOn();
		mlrState.SetZBufferWriteOn();

		drawInfo.m_state = mlrState;
		drawInfo.m_clippingFlags = 0x0;
		
		if (bldgDustPoofEffect && bldgDustPoofEffect->IsExecuted())
		{
			glm::vec3			actualPosition;
			glm::mat4 	shapeOrigin;
			glm::mat4	localToWorld;

			mc2ToRender(actualPosition, view.position);

 			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, actualPosition);

			drawInfo.m_parentToWorld = &shapeOrigin;

			if (!MLRVertexLimitReached)
				bldgDustPoofEffect->Draw(&drawInfo);
		}
	}
}

//---------------------------------------------------------------------------
void TerrainObject::renderShadows (void)
{
	if (getFlag(OBJECT_FLAG_FALLING) || getFlag(OBJECT_FLAG_FALLEN))
		return;			//No shadows on fallen trees.
		
	const bool readbackVisible = gpu_cull::readback_isEnabled()
		? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(getHandle()))
		: appearance->canBeSeen();
	if (readbackVisible)
	{
		appearance->renderShadows();
	}
	
	setSelected(false);		//ALWAYS reset the selected flags.  GUI needs this to work!
	setTargeted( false );	//ALWAYS do it here, too!  Otherwise things may draw FUNNY!

}	

//---------------------------------------------------------------------------
void TerrainObject::destroy (void) 
{
	if (cellsCovered) 
	{
		numCellsCovered = 0;
		std::free(cellsCovered);
		cellsCovered = nullptr;
	}

	subAreas0.reset();
	subAreas1.reset();

	//-----------------------------------------------------
	// This will free any memory the Building is using.
	if (appearance) 
	{
		releaseAppearance();
	}
}

//---------------------------------------------------------------------------

void TerrainObject::setDamage (long newDamage) {

	damage = static_cast<float>(newDamage);

	TerrainObjectTypePtr type = static_cast<TerrainObjectTypePtr>(getObjectType());

	switch (type->subType) 
	{
		case TERROBJ_TREE:
		case TERROBJ_NONE:
			
		break;
	}

	//---------------------------------------------
	// Code needs to go in here to fix appearance
	if (damage >= getDamageLevel())
	{
		setStatus(OBJECT_STATUS_DESTROYED);
		if (appearance)
			appearance->setObjStatus(OBJECT_STATUS_DESTROYED);
	}
}
		
//---------------------------------------------------------------------------

void TerrainObject::init (bool create, ObjectTypePtr objType) {

	GameObject::init(true, objType);

	setFlag(OBJECT_FLAG_JUSTCREATED, true);

	if ((static_cast<TerrainObjectTypePtr>(objType))->subType == TERROBJ_TREE)
	{
		//-------------------------------------------------------------
		// The appearance is initialized here using data from the type
		// Need an MLR appearance class
		char *appearanceName = objType->getAppearanceTypeName();

		//--------------------------------------------------------------
		// New code!!!
		// We need to append the sprite type to the appearance num now.
		// The MechEdit tool does not assume a sprite type, nor should it.
		// MechCmdr2 features much simpler objects which only use 1 type of sprite!
		long appearanceType = (TREED_TYPE << 24);

		AppearanceTypePtr terrainObjectAppearanceType = appearanceTypeList->getAppearance(appearanceType,appearanceName);
		if (!terrainObjectAppearanceType)
		{
			//------------------------------------------------------
			// LOAD a dummy appearance until real ones are available
			// for this building!
			terrainObjectAppearanceType = appearanceTypeList->getAppearance(appearanceType,"TREE");
			gosASSERT(terrainObjectAppearanceType != nullptr);
		}
		  
		adoptAppearance(std::make_unique_for_overwrite<TreeAppearance>());
		gosASSERT(appearance != nullptr);

		appearance->init(static_cast<TreeAppearanceType*>(terrainObjectAppearanceType), static_cast<GameObjectPtr>(this));
	}
	else
	{
		//-------------------------------------------------------------
		// The appearance is initialized here using data from the type
		// Need an MLR appearance class
		char *appearanceName = objType->getAppearanceTypeName();

		//--------------------------------------------------------------
		// New code!!!
		// We need to append the sprite type to the appearance num now.
		// The MechEdit tool does not assume a sprite type, nor should it.
		// MechCmdr2 features much simpler objects which only use 1 type of sprite!
		long appearanceType = (BLDG_TYPE << 24);

		AppearanceTypePtr terrainObjectAppearanceType = appearanceTypeList->getAppearance(appearanceType,appearanceName);
		if (!terrainObjectAppearanceType)
		{
			//------------------------------------------------------
			// LOAD a dummy appearance until real ones are available
			// for this building!
			terrainObjectAppearanceType = appearanceTypeList->getAppearance(appearanceType,"TESTOBJ");
		}
	  
		adoptAppearance(std::make_unique_for_overwrite<BldgAppearance>());
		gosASSERT(appearance != nullptr);

		//--------------------------------------------------------------
		// The only appearance type for buildings is MLR_APPEARANCE.
		gosASSERT(terrainObjectAppearanceType->getAppearanceClass() == BLDG_TYPE);
		
		appearance->init(static_cast<BldgAppearanceType*>(terrainObjectAppearanceType), static_cast<GameObjectPtr>(this));
	}

	if (objType->getExtentRadius() > 0.0)
		setTangible(true);

	objectClass = TERRAINOBJECT;
	switch ((static_cast<TerrainObjectTypePtr>(objType))->subType) {
		case TERROBJ_NONE:
			if ((static_cast<TerrainObjectTypePtr>(objType))->getDamageLevel() == 0.0) {
				//--------------------------------------------------------
				// We are already destroyed.  Used for extraction Markers
				setTangible(false);
				setStatus(OBJECT_STATUS_DESTROYED);
			}
			break;
		case TERROBJ_TREE:
			objectClass = TREE;
			setFlag(OBJECT_FLAG_DAMAGED, false);
			break;
		case TERROBJ_BRIDGE:
		case TERROBJ_FOREST:
		case TERROBJ_WALL_HEAVY:
		case TERROBJ_WALL_MEDIUM:
		case TERROBJ_WALL_LIGHT:
			setTangible(false);
			objectClass = BRIDGE;
			break;
	}
}	

//---------------------------------------------------------------------------

void TerrainObject::killFire (void) {

}

//---------------------------------------------------------------------------

void TerrainObject::lightOnFire (float timeToBurn) 
{
}

constexpr int DUST_POOF_ID = 32;
//---------------------------------------------------------------------------
long TerrainObject::handleWeaponHit (WeaponShotInfoPtr shotInfo, bool addMultiplayChunk) {

	if (!shotInfo)
		return(NO_ERR);

	if (addMultiplayChunk)
		MPlayer->addWeaponHitChunk(this, shotInfo);

	if (!getFlag(OBJECT_FLAG_DAMAGED)) 
	{
		float curDamage = getDamage();
		TerrainObjectTypePtr type = static_cast<TerrainObjectTypePtr>(getObjectType());
		switch (type->subType) 
		{
			case TERROBJ_NONE:
				curDamage += shotInfo->damage;
				if (curDamage > type->getDamageLevel())
					curDamage = type->getDamageLevel();
				setDamage(curDamage);
				
				if (curDamage >= type->getDamageLevel())
				{
					setFlag(OBJECT_FLAG_DAMAGED, true);
					curDamage = type->getDamageLevel();
					setStatus(OBJECT_STATUS_DESTROYED);
					setTangible(false);

					appearance->markLOS(true);
					appearance->setObjStatus(OBJECT_STATUS_DESTROYED);
					appearance->setLightsOut(true);
					// Draw bounds and the tree shape swap are presentation work; the sim pose refresh stays here.
					if (appearance->getAppearanceClass() != TREE_APPR_TYPE)
						appearance->update();
					appearance->markLOS();
					
					if (!shotInfo->attackerWID && (shotInfo->masterId == -1))	//Somebody stepped on me.
					{
						//--------------------------------------------
						//Play a Dust Poof.
                        auto dust = [this, position = position, requestedTime = scenarioTime] {
						if (useNonWeaponEffects)
						{
							unsigned flags = gosFX::Effect::ExecuteFlag;
					
							Check_Object(gosFX::EffectLibrary::Instance);
							gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(DUST_POOF_ID));
							
							if (gosEffectSpec)
							{
								bldgDustPoofEffect = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
								gosASSERT(bldgDustPoofEffect != nullptr);
							}
								
							MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
						}
						
						if (bldgDustPoofEffect)
						{
							glm::vec3			actualPosition;
							glm::mat4 	shapeOrigin;
							glm::mat4	localToWorld;
							
							mc2ToRender(actualPosition, position);
							
							mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
							mc2::math::buildTranslation(shapeOrigin, actualPosition);
							
							gosFX::Effect::ExecuteInfo info((Stuff::Time)requestedTime,&shapeOrigin,nullptr);
							bldgDustPoofEffect->Start(&info);
						}
                        };
                        if (!mc2::tick::defer(mc2::tick::Queue::Release, dust)) dust();
					}
				}

			break;

			case TERROBJ_TREE:
				setDamage(curDamage + 1.0);
				
				if (curDamage > type->getDamageLevel())
				{
					setFlag(OBJECT_FLAG_DAMAGED, true);
					curDamage = type->getDamageLevel();
					setStatus(OBJECT_STATUS_DESTROYED);

					appearance->markLOS(true);
					appearance->setObjStatus(OBJECT_STATUS_DESTROYED);
					appearance->setLightsOut(true);
					// Draw bounds and the tree shape swap are presentation work; the sim pose refresh stays here.
					if (appearance->getAppearanceClass() != TREE_APPR_TYPE)
						appearance->update();
					appearance->markLOS();
				}
				break;
			case TERROBJ_FOREST: 
			{
				curDamage += shotInfo->damage;
				if (curDamage >= type->getDamageLevel()) 
				{
					type->createExplosion(position, 0, 0);
					setStatus(OBJECT_STATUS_DESTROYED);
					openSubAreas();
				}
				setDamage(curDamage);
			}
			break;

			case TERROBJ_WALL_HEAVY:
			case TERROBJ_WALL_MEDIUM:
			case TERROBJ_WALL_LIGHT:
				curDamage += shotInfo->damage;
				if (curDamage >= type->getDamageLevel()) 
				{
					type->createExplosion(position, 0, 0);
					setStatus(OBJECT_STATUS_DESTROYED);
					if (type->subType == TERROBJ_WALL_LIGHT)
						soundSystem->playDigitalSample(BREAKINGFENCE, getPosition(), true);
				}
				setDamage(curDamage);
			break;
		}
	}
	
	return(NO_ERR);
}

//---------------------------------------------------------------------------

void TerrainObject::setTerrainPosition (const glm::vec3& position, const Stuff::Vector2DOf<long>& numbers)
{
	setPosition( position );
				
	vertexNumber = numbers.x;
	blockNumber = numbers.y;
}

void TerrainObject::setRotation( float rot )
{
	rotation = rot;
	(static_cast<ObjectAppearance*>(appearance))->rotation = rot;
}

//---------------------------------------------------------------------------

void TerrainObject::calcCellFootprint (glm::vec3& pos) {

	short cellList[MAX_CELL_COORDS];
	cellList[0] = MAX_CELL_COORDS;
	long numCoords = appearance->calcCellsCovered(pos, cellList);
	long minRow = 10000;
	long minCol = 10000;
	long maxRow = 0;
	long maxCol = 0;
	long curCoord = 0;
	while (curCoord < numCoords) {
		if (cellList[curCoord] < minRow)
			minRow = cellList[curCoord];
		if (cellList[curCoord] > maxRow)
			maxRow = cellList[curCoord];
		curCoord++;
		if (cellList[curCoord] < minCol)
			minCol = cellList[curCoord];
		if (cellList[curCoord] > maxCol)
			maxCol = cellList[curCoord];
		curCoord++;
	}
	
	if (numCoords)
	{
		cellFootprint[0] = minRow;
		cellFootprint[1] = minCol;
		cellFootprint[2] = maxRow;
		cellFootprint[3] = maxCol;
		land->cellToWorld(cellFootprint[0], cellFootprint[1], vectorFootprint[0]);
		land->cellToWorld(cellFootprint[0], cellFootprint[3], vectorFootprint[1]);
		land->cellToWorld(cellFootprint[2], cellFootprint[3], vectorFootprint[2]);
		land->cellToWorld(cellFootprint[2], cellFootprint[1], vectorFootprint[3]);
	}
	else
	{
		cellFootprint[0] = 0;
		cellFootprint[1] = 0;
		cellFootprint[2] = 0;
		cellFootprint[3] = 0;

		vectorFootprint[0] = glm::vec3(0.0f);
		vectorFootprint[1] = glm::vec3(0.0f);
		vectorFootprint[2] = glm::vec3(0.0f);
		vectorFootprint[3] = glm::vec3(0.0f);
	}
}

//---------------------------------------------------------------------------

long TerrainObject::getLineOfSightNodes (long eyeCellRow, long eyeCellCol, long* cells) {

	cells[0] = cellFootprint[0];
	cells[1] = cellFootprint[1];

	cells[2] = cellFootprint[0];
	cells[3] = cellFootprint[3];

	cells[4] = cellFootprint[2];
	cells[5] = cellFootprint[3];

	cells[6] = cellFootprint[2];
	cells[7] = cellFootprint[1];

	return(4);
}

//---------------------------------------------------------------------------

void TerrainObject::calcSubAreas (long numCells, short cells[MAX_GAME_OBJECT_CELLS][2]) {

	numCellsCovered = numCells;
	if (numCellsCovered) {
		cellsCovered = static_cast<short*>(std::malloc(4 * numCellsCovered));
		if (cellsCovered) {
			short* curCoord = cellsCovered;
			for (long j = 0; j < numCellsCovered; j++) {
				*curCoord++ = cells[j][0];
				*curCoord++ = cells[j][1];
			}
		}
	
		numSubAreas0 = 0;
		short* curCoord = cellsCovered;
		for (long i = 0; i < numCellsCovered; i++) 
		{
			long r = *curCoord++;
			long c = *curCoord++;
			long area = GlobalMoveMap[0]->calcArea(r, c);
			bool addIt = true;
			for (long j = 0; j < numSubAreas0; j++)
				if (subAreas0[j] == area) 
				{
					addIt = false;
					break;
				}

			if (addIt) 
			{
				if (!subAreas0)
				{
					subAreas0 = std::make_unique<short[]>(MAX_SPECIAL_SUB_AREAS);	// zeroed
				}

				subAreas0[numSubAreas0++] = area;
			}
		}

		numSubAreas1 = 0;
		curCoord = cellsCovered;
		for (int i = 0; i < numCellsCovered; i++) 
		{
			long r = *curCoord++;
			long c = *curCoord++;
			long area = GlobalMoveMap[1]->calcArea(r, c);
			bool addIt = true;
			for (long j = 0; j < numSubAreas1; j++)
				if (subAreas1[j] == area) 
				{
					addIt = false;
					break;
				}

			if (addIt) 
			{
				if (!subAreas1)
				{
					subAreas1 = std::make_unique<short[]>(MAX_SPECIAL_SUB_AREAS);	// zeroed
				}

				subAreas1[numSubAreas1++] = area;
			}
		}

		for (int i = 0; i < numSubAreas0; i++)
			GlobalMoveMap[0]->setAreaOwnerWID(subAreas0[i], getWatchID());

		for (int i = 0; i < numSubAreas1; i++)
			GlobalMoveMap[1]->setAreaOwnerWID(subAreas1[i], getWatchID());
	}
}

//---------------------------------------------------------------------------

void TerrainObject::markMoveMap (bool passable) {
	
	short* curCoord = cellsCovered;
	for (long i = 0; i < numCellsCovered; i++) {
		long r = *curCoord++;
		long c = *curCoord++;
		GameMap->setPassable(r, c, passable);
		if (passable)
			GameMap->setLocalHeight(r, c, 0.0f);
	}
}

//---------------------------------------------------------------------------

void TerrainObject::openSubAreas (void) {

	markMoveMap(true);
	for (int i = 0; i < numSubAreas0; i++)
		GlobalMoveMap[0]->openArea(subAreas0[i]);
	for (int i = 0; i < numSubAreas1; i++)
		GlobalMoveMap[1]->openArea(subAreas1[i]);
}

//---------------------------------------------------------------------------

void TerrainObject::closeSubAreas (void) {

	markMoveMap(false);
	for (int i = 0; i < numSubAreas0; i++)
		GlobalMoveMap[0]->closeArea(subAreas0[i]);
	for (int i = 0; i < numSubAreas1; i++)
		GlobalMoveMap[1]->closeArea(subAreas1[i]);
}

//---------------------------------------------------------------------------

void TerrainObject::setSubAreasTeamId (long id) {

	for (int i = 0; i < numSubAreas0; i++)
		GlobalMoveMap[0]->setAreaTeamID(subAreas0[i], id);
	for (int i = 0; i < numSubAreas1; i++)
		GlobalMoveMap[1]->setAreaTeamID(subAreas1[i], id);
}

//---------------------------------------------------------------------------

bool TerrainObject::calcAdjacentAreaCell (long moveLevel, long areaID, long& adjRow, long& adjCol) {

	if (areaID == -1) {
		short* curCoord = cellsCovered;
		for (int i = 0; i < numCellsCovered; i++) {
			long cellRow = *curCoord++;
			long cellCol = *curCoord++;
			long adjArea = GlobalMoveMap[moveLevel]->calcArea(cellRow - 1, cellCol);
			if (adjArea > -1) {
				adjRow = cellRow - 1;
				adjCol = cellCol;
				return(true);
			}
			adjArea = GlobalMoveMap[moveLevel]->calcArea(cellRow, cellCol + 1);
			if (adjArea > -1) {
				adjRow = cellRow;
				adjCol = cellCol + 1;
				return(true);
			}
			adjArea = GlobalMoveMap[moveLevel]->calcArea(cellRow + 1, cellCol);
			if (adjArea > -1) {
				adjRow = cellRow + 1;
				adjCol = cellCol;
				return(true);
			}
			adjArea = GlobalMoveMap[moveLevel]->calcArea(cellRow, cellCol - 1);
			if (adjArea > -1) {
				adjRow = cellRow;
				adjCol = cellCol - 1;
				return(true);
			}
		}
		}
	else {
		short* curCoord = cellsCovered;
		for (long i = 0; i < numCellsCovered; i++) {
			long cellRow = *curCoord++;
			long cellCol = *curCoord++;
			long adjArea = GlobalMoveMap[moveLevel]->calcArea(cellRow - 1, cellCol);
			if (adjArea == areaID) {
				adjRow = cellRow - 1;
				adjCol = cellCol;
				return(true);
			}
			adjArea = GlobalMoveMap[moveLevel]->calcArea(cellRow, cellCol + 1);
			if (adjArea == areaID) {
				adjRow = cellRow;
				adjCol = cellCol + 1;
				return(true);
			}
			adjArea = GlobalMoveMap[moveLevel]->calcArea(cellRow + 1, cellCol);
			if (adjArea == areaID) {
				adjRow = cellRow + 1;
				adjCol = cellCol;
				return(true);
			}
			adjArea = GlobalMoveMap[moveLevel]->calcArea(cellRow, cellCol - 1);
			if (adjArea == areaID) {
				adjRow = cellRow;
				adjCol = cellCol - 1;
				return(true);
			}
		}
	}
	return(false);
}

//***************************************************************************
#include "save_struct_zero.h"
void TerrainObject::Save (PacketFilePtr file, long packetNum)
{
	MC2_SAVE_STRUCT(TerrainObjectData, data);
	CopyTo(&data);

	//PacketNum incremented in ObjectManager!!
	file->writePacket(packetNum,reinterpret_cast<MemoryPtr>(&data),sizeof(TerrainObjectData),STORAGE_TYPE_ZLIB);
}

//***************************************************************************
void TerrainObject::CopyTo (TerrainObjectData *data)
{
	data->damage = damage;
	data->vertexNumber = vertexNumber;
	data->blockNumber = blockNumber;
	data->pitchAngle = pitchAngle;
	data->fallRate = fallRate;
	data->powerSupply = powerSupply;
	memcpy(data->cellFootprint,cellFootprint,sizeof(short) * 4);
	memcpy(data->vectorFootprint,vectorFootprint,sizeof(glm::vec3) * 4);
	data->numSubAreas0 = numSubAreas0;
	data->numSubAreas1 = numSubAreas1;

	if (subAreas0)
		memcpy(data->subAreas0,subAreas0.get(),sizeof(short) * MAX_SPECIAL_SUB_AREAS);
	else
		memset(data->subAreas0,0,sizeof(short) * MAX_SPECIAL_SUB_AREAS);

	if (subAreas1)
		memcpy(data->subAreas1,subAreas1.get(),sizeof(short) * MAX_SPECIAL_SUB_AREAS);
	else
		memset(data->subAreas1,0,sizeof(short) * MAX_SPECIAL_SUB_AREAS);

	data->listID = listID;

	data->numCellsCovered = numCellsCovered;
	if (numCellsCovered >= 162)
		STOP(("Object %d covers too many cells in Save/Load!!",getObjectType()->getObjTypeNum()));

	memcpy(data->cellsCovered,cellsCovered, sizeof(short) * numCellsCovered * 2);

	GameObject::CopyTo(data);
}

//---------------------------------------------------------------------------
void TerrainObject::Load (TerrainObjectData *data)
{
	GameObject::Load(data); 

	damage = data->damage;
	vertexNumber = data->vertexNumber;
	blockNumber = data->blockNumber;
	pitchAngle = data->pitchAngle;
	fallRate = data->fallRate;
	powerSupply = data->powerSupply;
	memcpy(cellFootprint,data->cellFootprint,sizeof(short) * 4);
	memcpy(vectorFootprint,data->vectorFootprint,sizeof(glm::vec3) * 4);
	numSubAreas0 = data->numSubAreas0;
	numSubAreas1 = data->numSubAreas1;

	if (numSubAreas0)
	{
		subAreas0 = std::make_unique<short[]>(MAX_SPECIAL_SUB_AREAS);
		memcpy(subAreas0.get(),data->subAreas0,sizeof(short) * MAX_SPECIAL_SUB_AREAS);
	}

	if (numSubAreas1)
	{
		subAreas1 = std::make_unique<short[]>(MAX_SPECIAL_SUB_AREAS);
		memcpy(subAreas1.get(),data->subAreas1,sizeof(short) * MAX_SPECIAL_SUB_AREAS);
	}

	listID = data->listID;

	numCellsCovered = data->numCellsCovered;
	if (numCellsCovered)
	{
		cellsCovered = static_cast<short*>(std::malloc(sizeof(short) * numCellsCovered * 2));
		memcpy(cellsCovered, data->cellsCovered,sizeof(short) * numCellsCovered * 2);
	}
}

//***************************************************************************

