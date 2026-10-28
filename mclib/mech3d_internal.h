#include "Gates/preferences_legacy.h"
// Shared surface of the mech3d translation units (mech3d*.cpp).
// mech3d.cpp keeps the shared globals, the accessors and destroy/copy code; the rest of the
// Mech3DAppearance implementation is split by responsibility (see DOC in each piece).
#ifndef MECH3D_INTERNAL_H
#define MECH3D_INTERNAL_H

#include "gos_static_prop_killswitch.h"
#include <atomic>
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "../GameOS/gameos/gpu_cull_readback.h"  // C3: GPU visibility queries
#include "../code/gameobj.h"  // C3: full GameObject definition for obj->getHandle() in init()
#include "../GameOS/gameos/gos_mech_batcher.h"
#include "../GameOS/gameos/gos_mech_killswitch.h"
#include "cpu_proj_cost_split.h"  // F3 CPU projection cost-baseline (RAII scope)
#include "anim_override_registry.h"  // ANIM-OVERRIDE-MVP: declarative gesture-clip remap
#include "mech_anim_runtime.h"  // BT2018-SKEL-ENGINE-1B-RUNTIME: imported-mech per-frame re-bake
#include "../GameOS/gameos/diagnostic_trace.h"  // ANIM_CADENCE guard events

extern std::atomic<uint32_t> g_mc2FrameCounter;  // defined in tgl.cpp — gait-advance cadence guard
// VK-MECH-CAPTURE-DETERMINISM-1: capture-mode-only gait-freeze predicate (defined
// gos_visual_capture.cpp). Returns nonzero only while a visual-capture sweep has
// the sim frozen; 0 whenever no MC2_VISUAL_* capture is active -> byte-safe live.
#include "../GameOS/gameos/gos_visual_capture.h"  // mc2CaptureGaitFreezeActive() (extern "C")
// FREEZE-MOVERS-1: 0 unless the `freeze_movers` verb / MC2_FREEZE_MOVERS_AT
// armed it. Kept SEPARATE from mc2CaptureGaitFreezeActive() on purpose: these
// guards must not change what an existing visual-capture run renders, only
// what a freeze_movers run renders. See code/missiongui.cpp.
extern "C" int mc2FreezeMoversActive();
#include "spotlight_diag.h"  // T1.16 — (E)-owned slot tagging for per-slot probe
#include "tgl_load_guard.h"  // META-FIX design B PART 1: required base-shape load guard
#include "cone_glow.h"       // LIGHT-CONE-MODELS-1 — node aim + cone glow
#include "vis_semantics.h"   // VISIBILITY-SEMANTICS-1 — drawn/known/emits/lit
#include <cstdint>  // M2.5 (Q6 amendment 2): uint64_t for MLR mech draw counter
#include <unordered_map>  // ANIM-CADENCE-GUARD-1: per-appearance last-advance frame stamp
#include <vector>  // PRESENTATION-SOCKET-PROFILE-1: DeclaredSocketCheck list
#include <set>      // MECH-LIGHTROW-FRESHNESS-1: one-line-per-mech-type fail-loud dedup
#include <cstdlib>
#include "unitprofile.h"              // PRESENTATION-SOCKET-PROFILE-1: mc2UnitProfileDataEnabled()
#include "unit_profile_validator.h"  // PRESENTATION-SOCKET-PROFILE-1: validateDeclaredSockets()
#include "action_executor_registry.h"  // UNIT-DEFINITION-VALIDATOR-1: theActionExecutorRegistry() for the ONE pass
#include "Gates/gate.h"

#ifndef CAMERA_H
#include"camera.h"
#endif

#ifndef DBASEGUI_H
#include"dbasegui.h"
#endif

#ifndef TERRAIN_H
#include"terrain.h"
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

#ifndef CELINE_H
#include"celine.h"
#endif

#ifndef WEAPONFX_H
#include"weaponfx.h"
#endif


#ifndef GVACTOR_H
#include"gvactor.h"
#endif

#ifndef GOS_PROFILER_H
#include"gos_profiler.h"
#endif

// Globals defined in mech3d.cpp (or elsewhere in the engine) that the split pieces share.
extern const bool s_gpuCullLifecycle;
extern float worldUnitsPerMeter;
extern bool useFog;
extern float metersPerWorldUnit;
extern MidLevelRenderer::MLRClipper * theClipper;
extern bool InEditor;
extern bool MLRVertexLimitReached;
extern char MechStateByGesture[MAX_MECH_ANIMATIONS];
extern bool reloadBounds;
extern int ObjectTextureSize;

// Eager gate caches defined in mech3d.cpp (read by several pieces).
extern const bool s_lcTrace;
extern glm::vec3 debugMechActorPosition[];
extern float mechDebugAngle[];
extern float torsoDebugAngle[];

// mstates.h (included by mech3d.cpp only, it defines the tables) -- what the pieces need from it.
#define MAX_TRANSITION_GESTURES			10
#define GESTURE_OFFSET_SIZE		100
extern char hotSpotFinderArray[28];
extern char mechSpeedStateArray[28];
extern char transitionArray[1000];

#define LC3D_TRACE(fmt, ...) do { if (s_lcTrace) { printf("[GPU_CULL_LIFECYCLE v1] mech3d " fmt "\n", ##__VA_ARGS__); fflush(stdout); } } while(0)

inline constexpr float HELICOPTER_FACTOR = 25.0f;
inline constexpr float idleMAX = 20.0f;
inline constexpr float JUMP_PITCH = 90.0f;

// Free functions defined in the split pieces.
DWORD bgrTorgb (DWORD frontRGB);

#endif // MECH3D_INTERNAL_H
