// Shared surface of the gos_postprocess translation units (gos_postprocess*.cpp).
//
// gos_postprocess.cpp keeps the singleton, setters, init, scene begin/end and the viewmode/sky accessors; the rest of the
// gosPostProcess implementation is split by responsibility (see DOC in each piece).
#ifndef GOS_POSTPROCESS_INTERNAL_H
#define GOS_POSTPROCESS_INTERNAL_H

#include "gos_postprocess.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "utils/vec.h"
#include "gos_hdri.h"
#include "../../RenderCore/IblHdriRegistry.h"  // HDRI-SKY-NUMBER-1
#include "gos_profiler.h"
#include "gos_validate.h"  // validation entry points
#include "gos_smoke.h"     // S9E: SmokeMode fixed deterministic render-shader clock
#include "vk_shared_gbuffer_gate.h"
#include "gameos.hpp"      // gos_InvalidateRenderStateCache (RENDER_STATES v1)
#include "../../RenderWorld/RenderWorld.h"  // M1.5: IsObjectIdBufferEnabled
#include "../../RenderCore/RenderResourceRegistry.h"
#include "../../RenderCore/EngineView.h"
#include "view_uniforms_gl.h"
#include "../../RenderCore/PipelineRegistry.h"  // POSTPROCESS-COMPOSITE-REGISTRATION-1
#include "render_frame_plan.h"                   // RENDER-FRAME-PLAN-SCAFFOLD-1
#ifdef MC2_VULKAN_ISLAND
#include "vk_shared_gbuffer_gate.h"     // VK-P14 GATE-CONSOLIDATION-1 umbrella resolver
#include "vulkan_shared_scene_depth.h"  // VK-TIER1 SHARED SCENE DEPTH substrate (gated)
#include "vulkan_shared_scene_color.h"  // VK-P11 TIER-1 SHARED SCENE COLOR/NORMAL (gated)
#include "vulkan_shadow_island_live.h"  // VK CSM direct shadow debug overlay view
#include "vulkan_sky_island.h"         // VK-PARITY-F3: HDRI sky VK owner
#endif
#ifdef MC2_VULKAN
#include "engine_present_vk.h"          // VK-P14b: MC2_VK_PRESENT_DIRECT_FBO present-FBO accessor
#endif

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <map>
#include <string>
#include <cstdlib>
#include <SDL2/SDL.h>
#include "../../RenderCore/frame_executor.h"  // FRAME-GRAPH-EXECUTOR-ISLAND-1 IslandContract
#include "Gates/gate_cold.h"   // gate::get<>(), str<>(), raw<>()

// Globals defined in gos_postprocess.cpp (or elsewhere in the engine) that the split pieces share.

// Defined in gos_postprocess.cpp, shared with the FBO piece.
extern gosPostProcess* s_postProcess;


// Free functions defined in the split pieces.
bool mc2ShadowCsmEnabled();
int mc2ShadowCsmCount();
int mc2ShadowMapSize();
bool mc2ShadowFullMapSeparate();
int mc2ShadowFullMapSize();
float mc2ShadowCsmR0();
float mc2ShadowCsmR1();
float mc2ShadowCsmSoftness();
float mc2ShadowDistFadeWu();
float mc2ShadowDistFadeMax();
bool mc2ShadowFadeCont();
bool mc2ShadowFadeRamp0();
bool mc2ShadowAltScale();
float mc2ShadowObjNormalBias();
float mc2ShadowMechSoft();
int mc2PropShadowReceive();
bool mc2ShadowGuardClampedSun();
bool mc2CloudShadowEnabled();
void mc2ClampShadowSunAltitude(float& fx, float& fy, float& fz);

#endif // GOS_POSTPROCESS_INTERNAL_H
