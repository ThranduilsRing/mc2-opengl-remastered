#include "Gates/preferences_legacy.h"
// Shared surface of the GVAppearance translation units (gvactor*.cpp).
//
// gvactor.cpp keeps the type, node lookups, init and setObjStatus plus the lifecycle trace gate; every other gate cache
// stays a file-scope static beside its single user in the render/update files.
#ifndef GVACTOR_INTERNAL_H
#define GVACTOR_INTERNAL_H


#ifndef GVACTOR_H
#include"gvactor.h"
#include "cpu_proj_cost_split.h"  // F3 CPU projection cost-baseline (RAII scope)
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "tgl_load_guard.h"       // META-FIX design B PART 1: required base-shape load guard
#include "spotlight_diag.h"  // T1.16 — (E)-owned slot tagging for per-slot probe
#include "cone_glow.h"       // LIGHT-CONE-MODELS-1 — node aim + cone glow
#include "vis_semantics.h"   // VISIBILITY-SEMANTICS-1 — drawn/known/emits/lit

// FREEZE-MOVERS-1: 0 unless the `freeze_movers` verb / MC2_FREEZE_MOVERS_AT
// armed it. Defined in code/missiongui.cpp next to the command channel.
extern "C" int mc2FreezeMoversActive();
#endif

#ifndef CAMERA_H
#include"camera.h"
#endif

#ifndef DBASEGUI_H
#include"dbasegui.h"
#endif

#ifndef MECH3D_H
#include"mech3d.h"
#endif

#ifndef OBJSTATUS_H
#include"objstatus.h"
#endif

#ifndef CIDENT_H
#include"cident.h"
#endif

#ifndef PATHS_H
#include"paths.h"
#endif

#ifndef USERINPUT_H
#include"userinput.h"
#endif

#ifndef TIMING_H
#include"timing.h"
#endif

#ifndef UTILITIES_H
#include"utilities.h"
#endif

#ifndef CEVFX_H
#include"cevfx.h"
#endif

#ifndef MC2_RENDER_SUBMISSIONS_H
#include"render_submissions.h"
#endif

#ifndef ERR_H
#include"err.h"
#endif

#ifndef WEAPONFX_H
#include"weaponfx.h"
#endif

#ifndef MOVE_H
#include"move.h"
#endif

#include "gos_static_prop_batcher.h"
#include "gos_static_prop_killswitch.h"  // g_useGpuStaticProps
#include "../GameOS/gameos/gpu_cull_readback.h"  // C3: GPU visibility queries
#include "../code/gameobj.h"  // C3: full GameObject definition for obj->getHandle() in init()

#define SPIN_RATE		90.0f

#define EXPAND_FACTOR 		(1.25f)

inline constexpr int MAX_SMOKE_NODES = 2;
#define MAX_WEAPON_NODES	4
inline constexpr int MAX_FOOT_NODES = 2;

// Globals defined in gvactor.cpp (or elsewhere in the engine) that the split pieces share.
extern float	worldUnitsPerMeter;
extern bool 	drawTerrainGrid;
extern bool		useFog;
extern int		ObjectTextureSize;
extern bool reloadBounds;
extern MidLevelRenderer::MLRClipper * theClipper;
extern bool MLRVertexLimitReached;
extern bool InEditor;
extern glm::vec3 debugGVActorPosition;
extern float vehicleDebugAngle;
extern float turretDebugAngle;
extern float debugVelMag;
extern const bool s_gpuCullLifecycle;   // defined (eager typed-gate read) in gvactor.cpp

#endif // GVACTOR_INTERNAL_H
