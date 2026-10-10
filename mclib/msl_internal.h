// Shared surface of the msl.cpp translation units (msl.cpp, msl_{load,shape,transform,anim}.cpp).
//
// msl.cpp keeps the include bundle's globals, the line parser, TG_TypeMultiShape::CreateFrom and the allocators; the rest of the
// multi shape layer is split by responsibility (see DOC in each piece).
#ifndef MSL_INTERNAL_H
#define MSL_INTERNAL_H

#ifndef MSL_H
#include"msl.h"
#endif

#include "gos_object_recon_tracy.h"  // [OBJECT_RECON v1] slice-2 recon-zero
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "cpu_proj_cost_split.h"     // F3 CPU projection cost-baseline (RAII scope)
#include "../GameOS/gameos/gos_static_prop_batcher.h"    // Stage 2.D.2 CacheGpuLightData
#include "../GameOS/gameos/gos_static_prop_killswitch.h" // g_useGpuObjects extern
#include "../GameOS/gameos/gos_mech_killswitch.h"        // g_useGpuMechs extern (Slice B1)
#include "../GameOS/gameos/gos_profiler.h"  // PERF DIAGNOSTIC 2026-05-06: ZoneScopedN for CacheGpuLightData breakdown
#include "../code/frame_jobs.h"              // FRAME-JOBS-2D: isFrameJobsWorkerThread()
#include "mc2_audit.h"                       // META-FIX design B PART 0: [ASSET_LOAD v1] loader chokepoint
#include <atomic>

// 2026-05-05: frame-stamp the cache so registry::flush() can skip stale entries
// (actors whose update() was culled this frame; their cached UBO slot index now
// points to a slot whose content was filled by a different actor).
extern uint32_t g_mc2FrameCounter;


#ifndef CIDENT_H
#include"cident.h"
#endif

#ifndef PATHS_H
#include"paths.h"
#endif

#ifndef MATHFUNC_H
#include"mathfunc.h"
#endif

#ifndef CAMERA_H
#include"camera.h"
#endif

#ifndef CELINE_H
#include"celine.h"
#endif

#ifndef DBASEGUI_H
#include"dbasegui.h"
#endif

#ifndef TIMING_H
#include"timing.h"
#endif

#include<toolos.hpp>


#include "../ARM/Microsoft.Xna.Arm.h"
#include "Gates/gate.h"
#include "../RenderCore/axis_convert.h"
using namespace Microsoft::Xna::Arm;

#define CURRENT_SHAPE_VERSION		0xBAFDECAF
#define CURRENT_ANIM_VERSION		0xBADDECAF

// sebi: defined in windows.h
#ifndef LINUX_BUILD
#define MAX_PATH					256
#endif

// Globals defined in msl.cpp (or elsewhere in the engine) that the split pieces share.
extern void GetNumberData (char *rawData, char *result);
extern void GetNameData (char *rawData, char *result);
extern void GetWordData (char *rawData, char *result);
extern int ObjectTextureSize;
extern bool useSlerping;
extern bool useVertexLighting;
extern bool useFaceLighting;
extern bool useShadows;
extern bool silentMode;

extern std::atomic<int> g_workerResubmitCalls;   // defined in msl.cpp; counts worker-thread resubmits
void GetNextLine (char *rawData, char *result);   // defined in msl.cpp

#endif // MSL_INTERNAL_H
