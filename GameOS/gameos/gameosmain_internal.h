// Shared surface of the gameosmain*.cpp pieces: the include bundle main needs, the declarations its helpers use,
// and the start-up / per-frame helpers main calls.
#ifndef GAMEOS_GAMEOSMAIN_INTERNAL_H
#define GAMEOS_GAMEOSMAIN_INTERNAL_H

#include <atomic>
#include "gameos.hpp"

// Window audit probes used only by the GameOS frame driver.
bool __stdcall gos_GetHudResClampEnabled();
bool __stdcall gos_ProbeDrawableSize(int* w, int* h);
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "gos_render.h"
#include "render_host_kind.h"
#include "render_frame_driver.h"       // RenderFrameDriver_RenderWorld — Slice 6 shared render seam (gated)
#include "render_snapshot.h"
#include "draw_packet_emitter.h"       // DrawPacket v0
#include "gos_static_prop_batcher.h"   // batcher_getSortedPacketCount — explicit, do not rely on transitive
#include "../../RenderCore/MaterialGpu.h"  // RenderCore::MaterialGpu for g_dpSelProp albedoTex fill
#include "build_identity.h"          // Stable build identity declarations
#include "../../RenderCore/ResolutionContract.h"  // META-FIX design D: MC2_RES_ASSERT audit
#include "vk_shared_gbuffer_gate.h"        // mc2vkgate::umbrellaOn (the VK umbrella adapter)
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <unordered_set>
#include <cstring>
#include <time.h>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#endif

#include <SDL2/SDL.h>
#include "gos_input.h"

#include "utils/color_utils.h"
#include "utils/timing.h"
#include "gos_postprocess.h"
#include "gos_validate.h"
#include "gos_screenshot.h"   // TGA capture of the Vulkan present readback
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
#include "tgl.h"
#include "gos_object_recon_tracy.h"  // [OBJECT_RECON v1] slice-2 recon zero (env-gated)
#include "projectz_trace.h"  // projectz_trace_init/frame_tick/shutdown (PROJECTZ v1)
#include "projectz_overlay.h" // RAlt+P debug overlay (commit 4)
#include "gos_host_vk.h"   // g_vkPresentInitDeferred / initVulkanBackend
#include "render_backend_select.h"  // VK-P13-SELECTOR-1 / T1: MC2_RENDER_BACKEND
#ifdef MC2_VULKAN
#include "vulkan_context.h"         // VK-P13-SELECTOR-1 / T2: shared engine VK device
#include "engine_present_vk.h"      // VK-P13-SELECTOR-1 / T3: GL->VK present intercept
#endif
#include "gos_visual_diff.h"  // Stage 2.E pre-HUD capture + Ctrl+Shift+P record
#include "gos_rdoc_capture.h"  // Tier 5: env-gated in-process RenderDoc capture
#include "gos_terrain_indirect.h"  // [INSTR v1] banner: terrain_indirect{,_parity} fields
#include "gpu_cull_record.h"       // C0-1: GpuActorRecord schema selftest
#include "gpu_cull_readback.h"    // C2: async readback ring buffer selftest
#include "object_admission_predicate.h"  // Track A1: init probe + selftest gate
#include "view_registry.h"
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
extern "C" int mc2_vk_scene_clear(int w, int h);   // vulkan_terrain_island_run.cpp
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
extern "C" uint32_t gos_getMechShadowTypesDrawn();
extern "C" uint32_t gos_getMechShadowInstDrawn();

// VFX-SPINE-0: C-linkage accessors from the particle bridge + batcher TUs.
// Same file-scope rule as the shadow accessors above (extern "C" cannot live
// at block scope). Read-only — these only return file-static counters.
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

extern std::atomic<uint32_t> g_mc2FrameCounter;
extern uint32_t g_mc2RenderFrame;
extern void gos_GetTerrainCameraPos(float* x, float* y, float* z);
extern void mc2_executor_own_begin_postprocess(gosPostProcess* pp);
extern void mc2_executor_own_end_postprocess(gosPostProcess* pp);
extern void gos_CreateRenderer(graphics::RenderWindowHandle win_h, int w, int h);
extern void gos_CreateRendererHeadless(int w, int h);   // --headless: no window or device
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

void startup_phase(std::string_view name);
void process_events( void );

// Helpers of main() (gameosmain_init.cpp / gameosmain_frame.cpp).
void initDpiAwareness();
void initLauncherEnv();
void initInstrBanner();
void initRenderOptionsFromConfig();
void initResolutionFromConfig();
void frameThrottleLogicEvents();
void frameEmitDrawPackets(RenderSnapshot& snap);
void frameScreenshotOracle();
void frameLoadRaceVerdict();
void frameLoadRaceProbes();
void frameCameraPark(graphics::RenderWindowHandle win);
void frameSwapWindow(graphics::RenderWindowHandle win);
void frameFrameCap(bool s_vsync_active, int s_fps_cap_mission, int s_fps_cap_menu, uint64_t start_tick);

#endif  // GAMEOS_GAMEOSMAIN_INTERNAL_H
