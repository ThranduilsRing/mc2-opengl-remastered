// Shared surface of the gameosmain*.cpp pieces: the include bundle main needs, the declarations its helpers use,
// and the start-up / per-frame helpers main calls.
#ifndef GAMEOS_GAMEOSMAIN_INTERNAL_H
#define GAMEOS_GAMEOSMAIN_INTERNAL_H

#include "gameos.hpp"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "gos_render.h"
#include "gos_render_context.h"        // InitializeRenderContextConventions (shared game/editor)
#include "render_frame_driver.h"       // RenderFrameDriver_RenderWorld — Slice 6 shared render seam (gated)
#include "render_snapshot.h"
#include "draw_packet_emitter.h"       // DrawPacket v0
#include "gos_static_prop_batcher.h"   // batcher_getSortedPacketCount — explicit, do not rely on transitive
#include "../../RenderCore/MaterialGpu.h"  // RenderCore::MaterialGpu for g_dpSelProp albedoTex fill
#include "build_fingerprint.h"          // MC2_BUILD_GIT_SHA (real macro; MC2_BUILD_HASH never defined)
#include "../../RenderCore/ResolutionContract.h"  // META-FIX design D: MC2_RES_ASSERT audit
#include "vk_shared_gbuffer_gate.h"        // mc2vkgate::terrainNativeFirstOwnerOn (the VK umbrella adapter)
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <unordered_set>
#include <cstring>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#endif

#include <SDL2/SDL.h>
#include "gos_input.h"

#include "utils/camera.h"
#include "utils/shader_builder.h"
#include "utils/gl_utils.h"
#include "utils/timing.h"
#include "gos_postprocess.h"
#include "gos_validate.h"
#include "gos_screenshot.h"   // deterministic backbuffer->TGA capture (oracle)
#include "gos_dev_shell.h"    // DEV-SHELL-1: localhost dev command socket (MC2_DEV_SHELL)
#include "gos_visual_capture.h"  // [VISUAL_CAPTURE v1] S9 PNG+sidecar + bookmark replay
#include "gos_static_prop_killswitch.h"
#include "gos_static_prop_registry.h"  // Stage 3.C: isEnabled() for [INSTR v1]
#include "../../RenderWorld/RenderWorld.h"  // M1 Task 14
#include "../../mclib/render_contract.h"   // Phase 2 assert init
#include "../../mclib/camera.h"   // CAMERA-PARK-1: extern CameraPtr eye (pose pin fixture)
#include "asset_scale.h"
#include "gos_crashbundle.h"
#include "gos_smoke.h"

#include <signal.h>
#include "gos_profiler.h"
#include "tgl.h"   // drainTglPoolStats / drainTglPoolStatsOnShutdown (Tier-1 instr)
#include "gos_object_recon_tracy.h"  // [OBJECT_RECON v1] slice-2 recon zero (env-gated)
#include "projectz_trace.h"  // projectz_trace_init/frame_tick/shutdown (PROJECTZ v1)
#include "projectz_overlay.h" // RAlt+P debug overlay (commit 4)
#include "render_backend_select.h"  // VK-P13-SELECTOR-1 / T1: MC2_RENDER_BACKEND
#ifdef MC2_VULKAN
#include "vulkan_context.h"         // VK-P13-SELECTOR-1 / T2: shared engine VK device
#include "engine_present_vk.h"      // VK-P13-SELECTOR-1 / T3: GL->VK present intercept
#endif
#include "gos_visual_diff.h"  // Stage 2.E pre-HUD capture + Ctrl+Shift+P record
#include "gos_rdoc_capture.h"  // Tier 5: env-gated in-process RenderDoc capture
#include "gos_terrain_indirect.h"  // [INSTR v1] banner: terrain_indirect{,_parity} fields
#include "terrain_surface_trace.h" // [INSTR v1] banner: terrain_surface_trace field (PR-0)
#include "gpu_cull_record.h"       // C0-1: GpuActorRecord schema selftest
#include "gpu_cull_readback.h"    // C2: async readback ring buffer selftest
#include "object_admission_predicate.h"  // Track A1: init probe + selftest gate
#include "view_uniforms_gl.h"                     // getCurrentView (unconditional: used by debug_state_dump)
#include "debug_state_dump.h"                     // DEBUG-STATE-DUMP-1
// TerrainPatchStream accessors feed the unconditional RenderSnapshot terrain-
// pass fill (TERRAIN-PASS-PACKET-0) below, so this header must NOT be gated on
// MC2_IMGUI — otherwise a non-IMGUI build fails to compile gameosmain.cpp.
#include "gos_terrain_patch_stream.h"             // TERRAIN-SPINE-0
#ifdef MC2_IMGUI
#include "../../GuiRuntime/GuiRuntime.h"
#include "../../GuiRuntime/EditorInspector.h"     // TERRAIN-SPINE-0
#include "imgui_impl_sdl2.h"
#endif
#include "mc2_hitch_trace.h"
#include "gos_render_pass_timer.h"
#include "gos_frame_pass_stats.h"   // [FRAME_PASS_STATS v1] collector
#include "utils/mc2log.h"   // LOGGING-FACADE-P0: mc2::log::init() before the NUL redirect
#include "Gates/gate_cold.h"   // typed gate registry: main-entered marker, invalidateAll, [CONFIG v1]


// SHADOW-SPINE-0: C-linkage accessors from batcher TUs. Declared at file
// scope because `extern "C"` is illegal at function/block scope.
#ifdef MC2_VULKAN_ISLAND
// VK-P9a WATER-INTEROP: water island shutdown teardown (defined in
// vulkan_water_island.cpp under the same #ifdef). Called at engine shutdown before
// the shared VkDevice is destroyed so the island's Vk objects don't leak.
extern "C" void mc2_vk_water_island_destroy();
extern "C" void mc2_vk_vfx_island_destroy();   // VK-FIELD-VFX-1
// VK-P11 STATIC-PROP COLOR: color-prop island shutdown teardown (defined in
// vulkan_color_prop_island.cpp under the same #ifdef). Same ordering rationale as
// the water island (deletes the interop FBO/imported tex + VK objects while both the
// GL context and the shared device are alive). No-op if never born.
extern "C" void mc2_vk_color_prop_island_destroy();
// VK-P10 TERRAIN TIER-1: terrain island shutdown teardown (defined in
// vulkan_terrain_island.cpp under the same #ifdef). Same ordering rationale — frees
// the VK renderpass/framebuffer/pipeline + snapshot buffers while the shared device
// is alive. The shared GBuffer images it borrows are owned by the postprocess
// registries. No-op if never born.
extern "C" void mc2_vk_terrain_island_destroy();
extern "C" void mc2_vk_terrain_set_native_prefix_clear_color(float r, float g, float b, float a);
// VK-P14 MECH TIER-1: mech island shutdown teardown (defined in vulkan_mech_island.cpp
// under the same #ifdef). Frees the VK renderpass/framebuffer/pipeline + instance/bone/
// geometry buffers + copy-once albedo while the shared device is alive. The shared
// GBuffer images it borrows are owned by the postprocess registries. No-op if never born.
extern "C" void mc2_vk_mech_island_destroy();
extern "C" void mc2_vk_legacy_shape_island_destroy();   // VK-PARITY-F1
extern "C" void mc2_vk_sky_island_destroy();            // VK-PARITY-F3
// VK-P8 SHADOW INTEROP: shadow island shutdown teardown (defined in
// vulkan_shadow_island_live.cpp). Frees its VK CSM producer objects + emits the
// [VK_SHADOW_INTEROP] health line (forbidden_* counters). No-op if never born.
extern "C" void mc2_vk_shadow_island_destroy();
#endif
extern "C" uint32_t gos_getMechShadowProgramId();
extern "C" uint32_t gos_getMechShadowTypesDrawn();
extern "C" uint32_t gos_getMechShadowInstDrawn();
extern "C" uint32_t gos_getStaticPropShadowTypesDrawn();
extern "C" uint32_t gos_getStaticPropShadowInstDrawn();

// VFX-SPINE-0: C-linkage accessors from the particle bridge + batcher TUs.
// Same file-scope rule as the shadow accessors above (extern "C" cannot live
// at block scope). Read-only — these only return file-static counters.
extern "C" unsigned int       gos_vfx_getParticleProgramId();
extern "C" unsigned int       gos_vfx_getSsboCapacity();
extern "C" int                gos_vfx_getInitFailed();
extern "C" int                gos_vfx_getCameraSetThisFrame();
extern "C" int                gos_vfx_getDebugMode();
extern "C" int                mc2_vfx_isEnabled();
extern "C" int                mc2_vfx_isLogEnabled();
extern "C" unsigned int       mc2_vfx_getBudget();
extern "C" int                mc2_vfx_getOverflowReported();
extern "C" unsigned long long mc2_vfx_getEmitTotal();
extern "C" unsigned long long mc2_vfx_getFlushTotal();
extern "C" unsigned long long mc2_vfx_getNonemptyFlushTotal();
extern "C" unsigned long long mc2_vfx_getRecordsFlushedTotal();
extern "C" unsigned int       mc2_vfx_getRecordsPerFlushMax();
extern "C" unsigned long long mc2_vfx_getTrailSpawnTotal();
extern "C" unsigned long long mc2_vfx_getTrailHeadTotal();

extern uint32_t g_mc2FrameCounter;
extern void gos_GetTerrainCameraPos(float* x, float* y, float* z);
extern void mc2_executor_own_begin_postprocess(gosPostProcess* pp);
extern void mc2_executor_own_end_postprocess(gosPostProcess* pp);
extern void gos_CreateRenderer(graphics::RenderContextHandle ctx_h, graphics::RenderWindowHandle win_h, int w, int h);
extern void gos_DestroyRenderer();
extern void gos_RendererBeginFrame();
extern void gos_RendererEndFrame();
extern void gos_RendererFlushHUDBatch();
extern void gos_RendererHandleEvents();
extern void gos_RenderUpdateDebugInput();
extern void gos_RenderEnableDebugDrawCalls();
extern bool gos_RenderGetEnableDebugDrawCalls();
extern bool gosExitGameOS();
extern bool gos_CreateAudio();
extern void gos_DestroyAudio();
extern SDL_Window* g_sdl_window;
extern bool g_useGpuStaticProps;

void startup_phase(const char* name);
void process_events( void );
extern bool g_vkPresentInitDeferred;
extern unsigned g_vkPresentRetryFails;

// Helpers of main() (gameosmain_init.cpp / gameosmain_frame.cpp).
void initDpiAwareness();
bool initLauncherBootstrap();
void initLauncherEnv();
void initInstrBanner();
void initResolutionFromConfig();
void initVulkanBackend(graphics::RenderWindowHandle win, int w, int h);
void frameThrottleLogicEvents();
void frameEmitDrawPackets(RenderSnapshot& snap);
void frameScreenshotOracle();
void frameLoadRaceVerdict();
void frameLoadRaceProbes();
void frameCameraPark(graphics::RenderWindowHandle win);
void frameSwapWindow(graphics::RenderWindowHandle win);
void frameFrameCap(bool s_vsync_active, int s_fps_cap_mission, int s_fps_cap_menu, uint64_t start_tick);

#endif  // GAMEOS_GAMEOSMAIN_INTERNAL_H
