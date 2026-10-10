// Shared surface of the MissionInterfaceManager translation units (missiongui*.cpp).
//
// missiongui.cpp keeps the class implementation; the UI-command channel (MC2_UI_COMMAND_FILE verbs) lives in
// missiongui_uicmd*.cpp and shares the include bundle and UiCmdCtx through this header.
#ifndef MISSIONGUI_INTERNAL_H
#define MISSIONGUI_INTERNAL_H

#ifndef MISSIONGUI_H
#include"missiongui.h"
#endif
#include "tacticaloverview.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "gos_postprocess.h"   // VK-GL-LOST-DRAWS tags
#include"terrain_runtime.h"   // TERRAIN-RUNTIME-CONSUMER-WATER-1

// M1.6 + M2.6: IsStaticPropPickEnabled, lookupAtPixel, setLastGameplayPick,
// clearLastGameplayPick, getLastGameplayPick, IsObjectIdBufferEnabled,
// IsMechPickEnabled, IsMechPickDebugEnabled, IsMechPickPierceFogEnabled,
// IsGpuPickHoverEnabled, IsGpuPickHoverTraceEnabled.
#include "../RenderWorld/RenderWorld.h"
#include "../RenderWorld/ScreenPick.h"  // GPU_PICK_HOVER_DYNAMIC-1: screenPickCompute
#include "gameplay_pick.h"  // M2-pre: tryGameplayPick spine + GameplayPickRequest
#include "../GameAdapters/MechRenderAdapter.h"  // M2.6: findMechByHandle (forward-decls BattleMech)
#ifdef MC2_IS_EDITOR
#include "../editor/EditorObjectMgr.h"
#endif
#ifdef MC2_IMGUI
#include "../GuiRuntime/EditorInspector.h"  // Task 7: Ctrl+Shift+LMB inspector bridge
#include "../GameAdapters/StaticPropRenderAdapter.h"  // inspector: getRecipeShapeName
#include "imgui.h"
#endif
#include "../GameOS/gameos/gos_input.h"  // IMGUI-PAUSE-INPUT-FIX-1: input::setGameModalActive
#include "../GameOS/gameos/gos_smoke.h"  // GAMEPLAY-DRIVER-2: SmokeMode::fixedTimestepEnabled
#include "bldg_anim_gate_counters.h"     // ANIMATE-BUILDING-1: animStartInvalidated receipt
// RENDERER-INPUT-READBACK-1 (light_row / draw_path / shadow_feed / ground / pixel):
//   tgl.h      -- TG_HWLightsData + ProbeGatherLightsForList (the real gather)
//   txmmgr.h   -- mcTextureManager::copyLightSlot (the BAKED static light row)
//   probe hdr  -- engine half: scene-FBO texel read + shadow-feed scan. Kept
//                 out of this TU because scripts/check-no-raw-gl-from-game.sh
//                 forbids gl*() under code/.
#include "tgl.h"
#include "txmmgr.h"
#include "../GameOS/gameos/gos_renderer_input_probe.h"

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

// TURRET-LIST-ENUM-1: objmgr.h only forward-declares TurretPtr (via dturret.h),
// but `list buildings` needs the COMPLETE Turret type for the TurretPtr ->
// GameObjectPtr derived-to-base conversion in its third enumeration pass.
// Own guard, NOT nested inside the OBJMGR_H one: if any header above ever
// pulls in objmgr.h first, a nested include would silently vanish and break
// this TU with a confusing incomplete-type error.
#ifndef TURRET_H
#include"turret.h"
#endif

#ifndef GAMESOUND_H
#include"gamesound.h"
#endif

#ifndef SOUNDS_H
#include"sounds.h"
#endif

#ifndef MULTPLYR_H
#include"multplyr.h"
#endif

#ifndef TEAM_H
#include"team.h"
#endif

#ifndef GAMECAM_H
#include"gamecam.h"
#endif

#ifndef VERTEX_H
#include"vertex.h"
#endif

#ifndef MECH_H
#include"mech.h"
#endif

#ifndef ARTLRY_H
#include"artlry.h"
#endif

#ifndef LOGISTICSPILOT_H
#include"logisticspilot.h"
#endif

#ifndef LOGISTICSDATA_H
#include"logisticsdata.h"
#endif

#ifndef MISSION_H
#include"mission.h"
#endif

#ifndef GROUP_H
#include"group.h"
#endif

#ifndef MOVEMGR_H
#include"movemgr.h"
#endif

#ifndef COMNDR_H
#include"comndr.h"
#endif

#ifndef PREFS_H
#include"prefs.h"
#endif

#ifndef KEYBOARDREF_H
#include"keyboardref.h"
#endif

#include "../resource.h"

#include"platform_windows.h"

#include"gvehicl.h" // remove
#include"projectz_trace.h"
#include "gos_visual_diff.h"  // Stage 2.E: gate edge-scroll when capture enabled
#include "gos_profiler.h"     // diag: MIF.* Tracy zones for 20ms spike bisection

// per-element hot-loop probes. Zero chrono calls when both envs unset.
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cmath>     // GPU-GROUND-PICK-PARITY-1: sqrt/fabs for delta magnitude
#include <cstring>   // GPU-GROUND-PICK-PARITY-1: strcmp for lookupFailReason classify
#include "../GameOS/gameos/diagnostic_trace.h"  // CURSOR_TARGET trace (offscreen-pick recon)
#include "pausewindow.h"   // PAUSE-IMGUI-SLICE-1: PauseWindow::imguiPauseEnabled()
#include "logisticsdialog.h"  // UI-COMMAND-CHANNEL-1: LogisticsOKDialog (press dialog.YES/NO)
#include "../GameOS/gameos/gos_dev_shell.h"  // UI-COMMAND-CHANNEL-1: scheduleScreenshot reuse
#include <sys/stat.h>  // UI-COMMAND-CHANNEL-1: cheap per-frame size probe (_stat64)
#include <direct.h>    // UI-COMMAND-CHANNEL-1: _mkdir (dev_shell_out for screenshots)
#include <string>      // UI-COMMAND-CHANNEL-1: command line parsing
#include <vector>      // UI-COMMAND-CHANNEL-1: command line list
// CGui sub-phase per-frame ns accumulators -- DEFINED in controlgui.cpp.

// One UI-command line being executed: the verb groups fill result/note/extra for the ack.
struct UiCmdCtx {
	const std::string& line;
	const char* result = "unknown";
	std::string note;
	std::string extra;   // multi-line ack payload (readback dumps)
};

// DEBUG-VIEW-SWEEP-1: global full-scene debug-view fan-out (gameos_graphics.cpp).
extern "C" void gos_SetGlobalDebugView(int view);
extern "C" int  gos_GetGlobalDebugView();

// Mover-freeze latch (missiongui_uicmd_fixture.cpp); consumed by mission.cpp, mech3d.cpp and gvactor.cpp.
extern "C" long mc2FreezeMoversMissionFrame();
extern "C" void mc2FreezeMoversMissionReset();
extern "C" int  mc2FreezeMoversActive();
extern "C" void mc2FreezeMoversSet(int on, long atFrame);
extern "C" long mc2FreezeMoversArmedAt();
extern "C" long mc2FreezeMoversEngagedAt();

#endif // MISSIONGUI_INTERNAL_H
