// Shared surface of the objmgr.cpp translation units (objmgr.cpp, objmgr_{io,update,pick,misc}.cpp).
//
// objmgr.cpp keeps the manager lifecycle, the object getters and render; the rest of GameObjectManager is split by
// responsibility (see DOC in each piece).
#ifndef OBJMGR_INTERNAL_H
#define OBJMGR_INTERNAL_H

// Include Files

//=============
#ifndef MCLIB_H
#include"mclib.h"
#endif

#ifndef DOBJMGR_H
#include"dobjmgr.h"
#endif

#include <cstdio>
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include <cmath>   // std::abs for clip.w sign-safe frustum test
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <chrono>  // FRAME-JOBS-1: prepass wall-clock timing
#include "../GameOS/gameos/diagnostic_trace.h"  // FRAME-JOBS-1: JSONL trace emit
#include "objmgr_watch_policy.h"  // OBJMGR-WATCH-POLICY-EXTRACT-1: shared watch bounds/index policy
#include "gos_static_prop_killswitch.h"  // g_useGpuStaticProps
#include "terrain_runtime.h"
#include "static_update_counters.h"      // g_staticUpdateRunCount/SkipCount/EmitSummary
#include "../GameOS/gameos/gpu_cull_substrate.h"       // C0: GPU cull substrate SSBO upload
#include "../GameOS/gameos/gpu_cull_parity.h"         // C0-4: AABB parity check
#include "../GameOS/gameos/gpu_cull_readback.h"        // C3: per-actor GPU visibility snapshot
#include "../GameOS/gameos/gos_static_prop_registry.h" // Task 5: mission-load bulk registration
#include "move_recon.h"  // MC2_MOVE_RECON per-frame pathfinding cost instrumentation
#include "vk_umbrella_light.h"  // VK-FIELD-VFX-1: umbrella detection for the VFX HUD-tail route
#include "object_walk_trace.h"  // OBJECT-WALK-FACTS-1 (gated, default-OFF)
#include "frame_jobs.h"  // FRAME-JOBS-1: parallelForRange worker pool
#include "brain_special_dispatch.h"  // BRAIN-COMMIT-PHASE-1: commitAllBrainIntents
#include "warrior.h"                 // BRAIN-COMMIT-PHASE-1: MechWarrior, getBrainRuntime
#include <atomic>
extern std::atomic<int> g_workerResubmitCalls;  // FRAME-JOBS-2D: msl.cpp

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef OBJTYPE_H
#include"objtype.h"
#endif

#ifndef GAMEOBJ_H
#include"gameobj.h"
#endif

#ifndef BLDNG_H
#include"bldng.h"
#endif

#ifndef UNITDESG_H
#include"unitdesg.h"
#endif

#include"gos_profiler.h"

#ifndef ARTLRY_H
#include"artlry.h"
#endif

#ifndef TURRET_H
#include"turret.h"
#endif

#ifndef GROUP_H
#include"group.h"
#endif

#ifndef TEAM_H
#include"team.h"
#endif

#ifndef COMNDR_H
#include"comndr.h"
#endif

#ifndef MECH_H
#include"mech.h"
#endif

#ifndef GVEHICL_H
#include"gvehicl.h"
#endif

#ifndef TERROBJ_H
#include"terrobj.h"
#endif

#ifndef WEAPONBOLT_H
#include"weaponbolt.h"
#endif

#ifndef CARNAGE_H
#include"carnage.h"
#endif

#ifndef LIGHT_H
#include"light.h"
#endif

#ifndef GATE_H
#include"gate.h"
#endif

#ifdef USE_ELEMENTALS
#ifndef ELEMNTL_H
#include"elemntl.h"
#endif
#endif

#ifndef COLLSN_H
#include"collsn.h"
#endif

#ifndef WEAPONFX_H
#include"weaponfx.h"
#endif

#ifndef MISSION_H
#include"mission.h"
#endif

#ifndef GOS_PROFILER_H
#include"gos_profiler.h"
#endif
#include "../RenderCore/axis_convert.h"
#include "Gates/gate.h"

/*
ObjectQueuePtr objectList = NULL;
ObjectQueueNodePtr clanMechList = NULL;
ObjectQueueNodePtr innerSphereMechList = NULL;
ObjectQueueNodePtr iconList = NULL;
ObjectQueueNodePtr weaponList = NULL;
*/

inline constexpr int VISIBLE_THRESHOLD = 1;

// Globals defined in objmgr.cpp (or elsewhere in the engine) that the split pieces share.
extern long* usedBlockList;
extern long* moverBlockList;
extern uint32_t g_mc2FrameCounter;
extern bool updateTerrainObjects;
extern bool	updateObjects;
extern bool	renderTerrainObjects;
extern bool	renderObjects;
extern GameObjectPtr MoverRoster[MAX_MOVER_PART_ID - MIN_MOVER_PART_ID + 1];
extern bool MaxObjectsDrawn;
extern bool drawOldWay;
extern GameObjectFootPrint* tempSpecialAreaFootPrints;
extern long tempNumSpecialAreas;
extern GameObjectManagerPtr ObjectManager;
extern GameObjectPtr* collisionList;
extern long numCollidables;

#endif // OBJMGR_INTERNAL_H
