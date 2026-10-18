// Shared surface of the camera.cpp translation units (camera.cpp, camera_{project,update,daylight,matrix,input}.cpp).
//
// camera.cpp keeps the camera globals, init / destroy, the frustum planes and the save / light-colour accessors; the rest of Camera
// is split by responsibility (see DOC in each piece).
#ifndef CAMERA_INTERNAL_H
#define CAMERA_INTERNAL_H

//---------------------------------------------------------------------------
// Include Files
#ifndef CAMERA_H
#include"camera.h"
#include"camera_frustum_math.h"
#endif

#ifndef TERRAIN_H
#include"terrain.h"
#endif

#ifndef VERTEX_H
#include"vertex.h"
#endif

#ifndef CRATER_H
#include"crater.h"
#endif

#ifndef TERRTXM_H
#include"terrtxm.h"
#endif

#ifndef CIDENT_H
#include"cident.h"
#endif

#ifndef PATHS_H
#include"paths.h"
#endif

#ifndef TIMING_H
#include"timing.h"
#endif

#ifndef USERINPUT_H
#include"userinput.h"
#endif

// [CAMERA_MOTION v1] recon-only motion-fraction tracking — gates whether
// Lifecycle Option C (camera-motion-gated fail-open) is shippable. See
// docs/superpowers/explorations/2026-05-07-lifecycle-normal-zoom-design.md.
#include"../GameOS/gameos/gos_profiler.h"
#include "cpu_proj_cost_split.h"  // F3 CPU projection cost-baseline (RAII scope)
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include <math.h>
#include <cstdlib>
#include <cstdio>   // std::printf for [VPL_PICK v1] lifecycle print
#include <vector>   // T1.15 [SPOT_DIAG v1] camera-overwrite probe state
#include "spotlight_diag.h"  // T1.16 — (E)-owned per-slot probe registry
#include "xform_conventions.h"  // XFORM-CONVENTION-HARNESS-1: lifted clip-space convention matrices
#include "RenderCore/ResolutionContract.h"  // META-FIX design D: MC2_RES_ASSERT audit
#include "Gates/gate.h"
#include "../RenderCore/axis_convert.h"


#pragma warning(disable:4305/*double to float truncation*/)


inline constexpr int TABLE_ENTRY_SIZE = 256;
inline constexpr int SCALE_LEVELS = 7;
inline constexpr int RAMPED_RED = 239;
inline constexpr int RAMPED_GREEN = 248;
inline constexpr int NO_RAM_FOR_SCALE_TABLE = 0x12120001;

#define NIGHT_START_PITCH		(25.0f)
#define NIGHT_LIGHT_PITCH		(10.0f)
inline constexpr float TERRAIN_LIGHTS_ON = 40.0f;

inline constexpr float NORM_PERSPECTIVE = 35.0f;

// Globals defined in camera.cpp (or elsewhere in the engine) that the split pieces share.
extern char WindowTitle[1024];;
extern float s_scrollMomentum;
extern long topCtrlUpd;
extern long scenarioEndTurn;
extern float actualTime;
extern long displayProfileData;
extern bool drawTerrainGrid;
extern long mechElements;
extern long leaveSwoopyOff;
extern long tileCacheReqs;
extern long tileCacheHits;
extern long tileCacheMiss;
extern bool drawCameraCircle;
extern bool g_cameraProjAspectNative;
extern bool gamePaused;
extern bool gameAsked;
extern MemoryPtr pauseShape;
extern MemoryPtr askedShape;
extern bool gRestartRender;
extern bool MaxObjectsDrawn;
extern float elevationAdjustFactor;
extern float zoomMax;
extern float zoomMin;
extern float FOVMax;
extern float FOVMin;
extern bool recalcLight;

// Scroll momentum and the smoothing helpers update and input share (defined in camera.cpp).
extern float s_scrollMomentum;
float camSmoothTau (void);
float camSmoothAlpha (float dt);

// Free functions defined in the split pieces.
bool CameraLineOfSight (glm::vec3 position, glm::vec3 targetPosition);

#endif // CAMERA_INTERNAL_H
