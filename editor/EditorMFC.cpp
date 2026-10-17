/***************************************************************
* FILENAME: EditorMFC.cpp
* DESCRIPTION: Implements the MFC application entry points for the Editor.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Gate MFC startup trace logging behind MC2_EDITOR_TRACE.
****************************************************************/

#include <cstdio>
#include <stdlib.h>
#include <string>
#include <cstring>
#include <vector>
#include "../GameOS/gameos/gos_crashbundle.h"
#include "Gates/gate_cold.h"  // gate reads below, and markMainEntered / invalidateAll / dumpConfig: EditRel has no gameosmain

// ---------------------------------------------------------------------------
// S-CLI: command-line flags for autonomous editor launches.
//
//   -mission PATH.pak       auto-load .pak on first idle after InitGameOS
//   -frames N               PostMessage WM_CLOSE after N rendered frames
//   -exit-on-load-fail      exit nonzero on mission load failure (no modal)
//
// Globals are file-scope here; EditorCamera.h reads g_cliFramesLimit /
// g_cliFrameCounter via extern declarations to drive the WM_CLOSE post.
// ---------------------------------------------------------------------------
std::string g_cliMissionPath;
long        g_cliFramesLimit     = 0;     // 0 = no auto-close
bool        g_cliExitOnLoadFail  = false;
long        g_cliFrameCounter    = 0;     // bumped by EditorCamera::render
bool        g_cliClosePosted     = false; // one-shot guard for WM_CLOSE
bool        g_cliAutoLoadFired   = false; // one-shot guard for auto-load

// --new-map[=mapSize,terrain]: headless repro of the File->New (from-scratch
// generator) path. Mirrors -mission auto-load but calls initTerrainFromTGA
// instead of initTerrainFromPCV. mapSize/terrain are the MapSizeDlg/TerrainDlg
// indices (default 0,0). Diagnostic harness for debugging the generator; not
// surfaced in the UI.
bool        g_cliNewMap          = false;
bool        g_cliGenMap          = false;
int         g_cliNewMapSize      = 0;
int         g_cliNewMapTerrain   = 0;

// -smoke-foliage[=PATH]: after the map auto-loads, exercise the foliage
// preview (FoliageRender::Load + Toggle) under the live frame loop. A missing or
// malformed JSON / missing sprite texture must load to 0 instances WITHOUT
// crashing -- that is the whole point of the foliage smoke. Default path is the
// generator's genmap sidecar. Reported as facts in the [ESMOKE v1] summary; the
// Python runner (run_editor_smoke.py) applies the pass/fail verdict.
bool        g_cliSmokeFoliage    = false;
std::string g_cliSmokeFoliagePath;
bool        g_cliSmokeFoliageFired = false;
int         g_cliSmokeFoliageCount = -1;   // -1 = not attempted, >=0 = Load() ran

// -smoke-exit-sec N: time-based auto-exit. The -frames path (EditorCamera::render
// counter) is unreliable -- that render does not tick every frame and a WM_CLOSE
// can stall on a "save changes?" modal in a headless run. Instead, N seconds after
// the engine is up we emit the [ESMOKE v1] summary and hard exit(0): guaranteed
// termination + rc 0 + a parseable summary, which is all a smoke needs.
int          g_cliExitAfterSec    = 0;     // 0 = disabled
unsigned int g_cliExitDeadlineMs  = 0;     // GetTickCount() target, set on first post-init idle
bool         g_cliEsmokeEmitted   = false;

// Smoke mode suppresses the auto-run-path AfxMessageBox modals (generate/load
// failures) that would otherwise hang a headless run with no one to click OK.
// Read by EditorData.cpp via extern. Derived from the smoke flags at end of parse.
bool         g_cliSuppressModals  = false;

// -smoke-save[=PATH]: after the map auto-generates, save it programmatically to a
// .pak so a second run can load it (the generate->save->load gate, driven through
// the editor). Default path lives under terrain_gen_out so it deploys with tools.
bool         g_cliSmokeSave       = false;
bool         g_cliSmokeWorkflows = false;
bool         g_cliSmokeCatalogReference = false;
std::string  g_cliSmokeSavePath;
int          g_cliSmokeSaveOk     = -1;    // -1 = not attempted, 0/1 = save() result

// -smoke-outliner: after the map auto-loads, exercise the read-only Scene
// Outliner Lite: open the panel, enumerate placed objects (counts), and select
// the first selectable object via the existing EditorObjectMgr path. Read-only
// (no save mutation); reported in the [ESMOKE v1] summary. Pairs with a -gen-map
// (empty map -> counts all zero, selected=0, which is still a valid PASS).
bool         g_cliSmokeOutliner       = false;
bool         g_cliSmokeFrameSelection = false;   // -smoke-frame-selection: frame the camera on the outliner-selected object (capture evidence)
int          g_cliSmokeOutlinerCount  = -1;   // -1 = not attempted, >=0 = total objects
int          g_cliSmokeOutlinerSel    = -1;   // -1 = not attempted, 0/1 = first-object selected

// -smoke-inspector: after the map auto-loads, select the first object (existing
// EditorObjectMgr path) and ask the read-only Inspector for its selection
// summary. Read-only; reported in the [ESMOKE v1] summary. An empty generated
// map yields selected=0 / type=none, which is still a valid crash-free PASS.
bool         g_cliSmokeInspector       = false;
int          g_cliSmokeInspectorSel    = -1;   // -1 = not attempted, 0/1 = had selection
char         g_cliSmokeInspectorType[32] = "none";

// -smoke-validate: after the map auto-loads, open the live Mission Save
// Readiness panel and tally its checks (read-only). Reports issue counts and
// whether a unit-staffing warning fired in the [ESMOKE v1] summary. A freshly
// generated empty map has no units -> expect a staffing warning.
bool         g_cliSmokeValidate         = false;
int          g_cliValidateBlock         = -1;  // -1 = not attempted, >=0 = failing blocking checks
int          g_cliValidateWarn          = -1;  // >=0 = failing warning checks
int          g_cliValidateInfo          = -1;  // >=0 = failing info checks
int          g_cliValidateUnitsWarn     = -1;  // -1 = not attempted, 0/1 = unit-staffing warning

// -smoke-inspector-edit: place a throwaway drop zone, transform it via the
// Inspector's applyObjectTransform path, then undo -- exercising the editable
// Inspector + ModifyBuildingAction undo end to end. Reported in [ESMOKE v1].
bool         g_cliSmokeInspectorEdit     = false;
int          g_cliEditApplied            = -1;  // -1 = not attempted, 0/1 = transform moved object
int          g_cliEditUndo               = -1;  // -1 = not attempted, 0/1 = undo restored position

// -smoke-asset-browser: after the map auto-loads, tally the object catalog and
// activate the first placeable object through the existing placement path.
// Reported in [ESMOKE v1]. (Runtime validation deferred while map-load is broken.)
bool         g_cliSmokeAssetBrowser       = false;
int          g_cliAssetGroups             = -1;  // -1 = not attempted, >=0 = catalog group count
int          g_cliAssetActivated          = -1;  // -1 = not attempted, 0/1 = placement brush activated

// -smoke-gameplay-debugger: open the read-only Gameplay Debugger, select the
// first object, and run its null-guarded selection probe (ImGui-free). The
// editor has no live sim, so this exercises the static-data fallback path only.
// Safe on an empty map (selected=0, type=none). Facts -> [ESMOKE v1] at exit.
bool         g_cliSmokeGameplayDbgr       = false;
int          g_cliGameplayDbgrSel         = -1;   // -1 = not attempted, 0/1 = had selection
char         g_cliGameplayDbgrType[32]    = "none";

// -smoke-undo-history: open the display-only Undo History panel and read the
// ActionUndoMgr accessors (count + cursor) added for it. Read-only. A freshly
// generated map has an empty undo stack (count 0, cursor -1) -> valid PASS.
bool         g_cliSmokeUndoHistory        = false;
int          g_cliUndoCount               = -1;   // -1 = not attempted, >=0 = action count
int          g_cliUndoCursor              = -2;   // -2 = not attempted, >=-1 = cursor position

// -smoke-place-oob: drive a BuildingBrush placement cursor off the map edge to
// exercise the worldToCell OOB clamp (regression guard for the terrainElevation
// 0xC0000005 crash). survived=1 -> the clamp held; a crash here is the bug.
bool         g_cliSmokePlaceOob           = false;
int          g_cliPlaceOobSurvived        = -1;   // -1 = not attempted, 0/1 = survived all updates

// -playtest [-playtest-timeout-sec N]: the END-TO-END playtest smoke. After the
// mission auto-loads, run a few frames then trigger the REAL Playtest path
// (EditorPlaytest::Start(), exactly what the toolbar button does), with the
// launched game in smoke mode so it auto-quits. Poll EditorPlaytest::IsIdle()
// from the main-thread OnIdle until the child completes (PASS) or the timeout
// fires (Stop() + FAIL). On completion emit `[ESMOKE v1] event=playtest ...` and
// exit with 0 iff the child exited 0, else a distinct nonzero code:
//   2 = launch/bridge/save abort (Start() never went Running)
//   3 = child exited nonzero (game crash / content fail)
//   4 = timeout (child never completed within the window)
bool         g_cliPlaytest          = false;
int          g_cliPlaytestTimeout   = 120;  // seconds; -playtest-timeout-sec N
bool         g_cliPlaytestStarted   = false; // one-shot: Start() has been called
unsigned int g_cliPlaytestDeadline  = 0;     // GetTickCount() target once started
int          g_cliPlaytestWarmFrames = 0;    // frames rendered since auto-load done
bool         g_cliPlaytestDone       = false; // verdict emitted; guards the exit

// -capture=<tga path> [-capture-frame=N]: write the shown editor window (viewport + ImGui panels)
// to a TGA after N rendered frames with a mission loaded, then let the smoke exit timer end the run.
// Backend-neutral: the GL backend reads the back buffer, Vulkan asks the present path for its readback.
std::string  g_cliCapturePath;
int          g_cliCaptureFrame       = 150;
const char*  EditorCliCapturePath()  { return g_cliCapturePath.c_str(); }
int          EditorCliCaptureFrame() { return g_cliCaptureFrame; }

// Set true after the auto-load block finishes ALL its work (generate + optional
// foliage + optional save). The smoke exit countdown starts from THIS, not from
// g_cliAutoLoadFired (which is set at block ENTRY) -- otherwise a slow generation
// or save can be cut off mid-flight by the timed ExitProcess.
bool         g_cliSmokeWorkDone   = false;

// -smoke-foliage-menu: drive the foliage MENU commands (Toggle/Clear/Generate)
// via WM_COMMAND -- the actual command path, not just the load hook. Records the
// visibility/count at each step into the [ESMOKE v1] summary for the runner to
// verify. Implies -smoke-foliage (so there is foliage loaded to toggle first).
bool         g_cliSmokeFoliageMenu = false;
void*        g_smokeMainHwnd       = nullptr;   // HWND (typed void* -- declared before windows.h)
int          g_menuVis0 = -1, g_menuVis1 = -1, g_menuVis2 = -1;   // visibility across two toggles
int          g_menuClearCount = -1;          // Count() after Clear (expect 0)
int          g_menuReloadCount = -1;         // Count() after Generate/reload (expect >0)

static void editor_set_default_env_vars()
{
    // Bake editor-preferred defaults so RenderDoc and other tools can launch
    // the exe directly without a .bat. Bat overrides still work because we
    // only set when unset.
    if (!mc2::gate::isSet<"MC2_EDITOR_TRACE">())            _putenv_s("MC2_EDITOR_TRACE",              "1");
    // The editor viewport renders on the same Vulkan path as the game (step 3.6). MC2_RENDER_BACKEND=gl
    // stays as the temporary opt-out; the umbrella arms the VK islands the shared scene driver feeds.
    if (!mc2::gate::isSet<"MC2_RENDER_BACKEND">())          _putenv_s("MC2_RENDER_BACKEND",            "vulkan");
    {
        // The umbrella only makes sense with the Vulkan backend; with the gl opt-out it must stay off or the
        // islands own the scene while nothing presents it.
        const char* backend = getenv("MC2_RENDER_BACKEND");
        const bool gl = backend && (_stricmp(backend, "gl") == 0);
        if (!gl && !getenv("MC2_VK_SHARED_GBUFFER"))        _putenv_s("MC2_VK_SHARED_GBUFFER",         "1");
    }
    // S3 (2026-05-25): MC2_GPU_DRIVEN, MC2_EDITOR_BYPASS_BLDG_CULL, MC2_STATIC_PROP_REGISTRY
    // sidesteps retired -- editor now runs canonical default-ON chain.
}

static void EarlyTrace(const char* msg)
{
	// Always feed crash ring — visible in last_trace.txt on CTD.
	crashbundle_append(msg);

	if (!mc2::gate::get<"MC2_EDITOR_TRACE">())
		return;

    FILE* f = fopen("editor-startup.log", "a");
    if (f)
    {
        fprintf(f, "%s\n", msg);
        fclose(f);
    }
}

static void EarlyTraceBegin()
{
	if (!mc2::gate::get<"MC2_EDITOR_TRACE">())
		return;

    // Open with "w" exactly once at true process start to clear any stale log,
    // then write the sentinel.  All subsequent writes use "a" (append).
    FILE* f = fopen("editor-startup.log", "w");
    if (f)
    {
        fprintf(f, "editor-startup.log begin\n");
        fclose(f);
    }
}

//

#include "stdafx.h"
#include "EditorMFC.h"

#include "MainFrm.h"
#include "editorinterface.h"
#include "EditorVersion.h"
#include "EditorData.h"
#include "FoliageRender.h"
#include "SceneOutliner.h"
#include "InspectorPanel.h"
#include "AssetBrowser.h"
#include "MissionValidation.h"
#include "GameplayDebugger.h"
#include "UndoHistoryPanel.h"
#include "Action.h"            // ActionUndoMgr::instance (undo-history smoke)
#include "ModPicker.h"
#include "EditorModProject.h"
#include "EditorPlaytest.h"   // -playtest end-to-end smoke
#include "EditorTaskRunner.h" // ShutdownKillRunning() on editor exit
#include "resource.h"   // ID_FOLIAGE_* for the -smoke-foliage-menu WM_COMMAND drive

// -- S-CLI parser ------------------------------------------------------------
// Accepts: -mission/--mission PATH (or =PATH), -frames/--frames N (or =N),
// -exit-on-load-fail / --exit-on-load-fail. Tokenizes a Windows-style cmdline
// honoring double-quoted spans (no escape processing beyond quote toggling —
// matches how MFC's CFileDialog hands paths back to itself).
static bool s_cli_flag_match(const char* tok, const char* shortName, const char* longName)
{
	return (strcmp(tok, shortName) == 0) || (strcmp(tok, longName) == 0);
}
static bool s_cli_starts_with(const char* tok, const char* prefix, const char** outRest)
{
	size_t n = strlen(prefix);
	if (strncmp(tok, prefix, n) == 0) { *outRest = tok + n; return true; }
	return false;
}
static void s_cli_parse(const char* cmd)
{
	if (!cmd || !*cmd) return;
	std::vector<std::string> argv;
	{
		std::string cur;
		bool inQuote = false;
		for (const char* p = cmd; *p; ++p)
		{
			char c = *p;
			if (c == '"') { inQuote = !inQuote; continue; }
			if (!inQuote && (c == ' ' || c == '\t'))
			{
				if (!cur.empty()) { argv.push_back(cur); cur.clear(); }
				continue;
			}
			cur.push_back(c);
		}
		if (!cur.empty()) argv.push_back(cur);
	}

	for (size_t i = 0; i < argv.size(); ++i)
	{
		const char* tok = argv[i].c_str();
		const char* rest = nullptr;

		if (s_cli_flag_match(tok, "-mission", "--mission"))
		{
			if (i + 1 < argv.size()) { g_cliMissionPath = argv[++i]; }
		}
		else if (s_cli_starts_with(tok, "-mission=", &rest) ||
		         s_cli_starts_with(tok, "--mission=", &rest))
		{
			g_cliMissionPath = rest;
		}
		else if (s_cli_flag_match(tok, "-frames", "--frames"))
		{
			if (i + 1 < argv.size()) { g_cliFramesLimit = atol(argv[++i].c_str()); }
		}
		else if (s_cli_starts_with(tok, "-frames=", &rest) ||
		         s_cli_starts_with(tok, "--frames=", &rest))
		{
			g_cliFramesLimit = atol(rest);
		}
		else if (s_cli_flag_match(tok, "-exit-on-load-fail", "--exit-on-load-fail"))
		{
			g_cliExitOnLoadFail = true;
		}
		else if (s_cli_flag_match(tok, "-new-map", "--new-map"))
		{
			g_cliNewMap = true;
		}
		else if (s_cli_starts_with(tok, "-new-map=", &rest) ||
		         s_cli_starts_with(tok, "--new-map=", &rest))
		{
			// rest = "size" or "size,terrain"
			g_cliNewMap = true;
			g_cliNewMapSize = atol(rest);
			const char* comma = strchr(rest, ',');
			if (comma) g_cliNewMapTerrain = atol(comma + 1);
		}
		else if (s_cli_flag_match(tok, "-gen-map", "--gen-map"))
		{
			g_cliGenMap = true;
		}
		else if (s_cli_starts_with(tok, "-gen-map=", &rest) ||
		         s_cli_starts_with(tok, "--gen-map=", &rest))
		{
			g_cliGenMap = true;
			g_cliNewMapSize = atol(rest);
			const char* comma = strchr(rest, ',');
			if (comma) g_cliNewMapTerrain = atol(comma + 1);
		}
		else if (s_cli_flag_match(tok, "-smoke-foliage", "--smoke-foliage"))
		{
			g_cliSmokeFoliage = true;
			g_cliSmokeFoliagePath = "terrain_gen_out\\genmap.foliage.json";
		}
		else if (s_cli_starts_with(tok, "-smoke-foliage=", &rest) ||
		         s_cli_starts_with(tok, "--smoke-foliage=", &rest))
		{
			g_cliSmokeFoliage = true;
			g_cliSmokeFoliagePath = rest;
		}
		else if (s_cli_flag_match(tok, "-smoke-exit-sec", "--smoke-exit-sec"))
		{
			if (i + 1 < argv.size()) { g_cliExitAfterSec = atoi(argv[++i].c_str()); }
		}
		else if (s_cli_starts_with(tok, "-smoke-exit-sec=", &rest) ||
		         s_cli_starts_with(tok, "--smoke-exit-sec=", &rest))
		{
			g_cliExitAfterSec = atoi(rest);
		}
		else if (s_cli_flag_match(tok, "-smoke-foliage-menu", "--smoke-foliage-menu"))
		{
			g_cliSmokeFoliageMenu = true;
			g_cliSmokeFoliage = true;   // need foliage loaded before driving the menu
			if (g_cliSmokeFoliagePath.empty())
				g_cliSmokeFoliagePath = "terrain_gen_out\\genmap.foliage.json";
		}
		else if (s_cli_flag_match(tok, "-smoke-workflows", "--smoke-workflows"))
		{
			g_cliSmokeWorkflows = true;
		}
		else if (s_cli_flag_match(tok, "-smoke-catalog-reference", "--smoke-catalog-reference"))
		{
			g_cliSmokeCatalogReference = true;
		}
		else if (s_cli_flag_match(tok, "-smoke-save", "--smoke-save"))
		{
			g_cliSmokeSave = true;
			g_cliSmokeSavePath = "terrain_gen_out\\smoke_saved.pak";
		}
		else if (s_cli_starts_with(tok, "-smoke-save=", &rest) ||
		         s_cli_starts_with(tok, "--smoke-save=", &rest))
		{
			g_cliSmokeSave = true;
			g_cliSmokeSavePath = rest;
		}
		else if (s_cli_flag_match(tok, "-smoke-outliner", "--smoke-outliner"))
		{
			g_cliSmokeOutliner = true;
		}
		else if (s_cli_flag_match(tok, "-smoke-frame-selection", "--smoke-frame-selection"))
		{
			g_cliSmokeFrameSelection = true;
		}
		else if (s_cli_flag_match(tok, "-smoke-inspector", "--smoke-inspector"))
		{
			g_cliSmokeInspector = true;
		}
		else if (s_cli_flag_match(tok, "-smoke-validate", "--smoke-validate"))
		{
			g_cliSmokeValidate = true;
		}
		else if (s_cli_flag_match(tok, "-smoke-inspector-edit", "--smoke-inspector-edit"))
		{
			g_cliSmokeInspectorEdit = true;
		}
		else if (s_cli_flag_match(tok, "-smoke-asset-browser", "--smoke-asset-browser"))
		{
			g_cliSmokeAssetBrowser = true;
		}
		else if (s_cli_flag_match(tok, "-smoke-gameplay-debugger", "--smoke-gameplay-debugger"))
		{
			g_cliSmokeGameplayDbgr = true;
		}
		else if (s_cli_flag_match(tok, "-smoke-undo-history", "--smoke-undo-history"))
		{
			g_cliSmokeUndoHistory = true;
		}
		else if (s_cli_flag_match(tok, "-smoke-place-oob", "--smoke-place-oob"))
		{
			g_cliSmokePlaceOob = true;
		}
		else if (s_cli_flag_match(tok, "-playtest", "--playtest"))
		{
			g_cliPlaytest = true;
		}
		else if (s_cli_starts_with(tok, "-capture=", &rest) || s_cli_starts_with(tok, "--capture=", &rest))
		{
			g_cliCapturePath = rest;
		}
		else if (s_cli_starts_with(tok, "-capture-frame=", &rest) || s_cli_starts_with(tok, "--capture-frame=", &rest))
		{
			g_cliCaptureFrame = atoi(rest);
		}
		else if (s_cli_flag_match(tok, "-playtest-timeout-sec", "--playtest-timeout-sec"))
		{
			if (i + 1 < argv.size()) { g_cliPlaytestTimeout = atoi(argv[++i].c_str()); }
		}
		else if (s_cli_starts_with(tok, "-playtest-timeout-sec=", &rest) ||
		         s_cli_starts_with(tok, "--playtest-timeout-sec=", &rest))
		{
			g_cliPlaytestTimeout = atoi(rest);
		}
	}

	// Any smoke flag implies headless -> suppress the auto-run-path failure modals.
	g_cliSuppressModals = (g_cliExitAfterSec > 0) || g_cliSmokeFoliage || g_cliSmokeSave
	                      || g_cliSmokeOutliner || g_cliSmokeInspector || g_cliSmokeValidate
	                      || g_cliSmokeInspectorEdit || g_cliSmokeAssetBrowser
	                      || g_cliSmokeGameplayDbgr || g_cliSmokeUndoHistory
	                      || g_cliSmokePlaceOob || g_cliPlaytest || g_cliSmokeCatalogReference;
}

// Forward declaration — defined in EditorGameOS.cpp.
// MFCPlatform.hpp is a legacy header that may not be present in all build
// configurations; declare the symbol directly to avoid the dependency.
void __stdcall InitGameOS(HINSTANCE hInstance, HWND hWindow, char* commandLine);

static EditorInterface* g_editorInterfaceWindow = nullptr;


BEGIN_MESSAGE_MAP(EditorMFCApp, CWinApp)
	ON_COMMAND(ID_APP_ABOUT, OnAppAbout)
END_MESSAGE_MAP()


EditorMFCApp::EditorMFCApp()
	: m_bPendingInitGameOS(false)
	, m_hEditorWnd(nullptr)
{
}


EditorMFCApp theApp;


static DWORD WINAPI s_smoke_thread(LPVOID);   // smoke driver: defined below

bool EditorBackgroundLaunch()
{
	// Same truthiness as the engine's window_offscreen_requested(): set and not "0".
	return mc2::gate::get<"MC2_NO_ACTIVATE">() || mc2::gate::get<"MC2_WINDOW_OFFSCREEN">();
}

BOOL EditorMFCApp::InitInstance()
{
	editor_set_default_env_vars();

	// EditRel builds with DISABLE_GAMEOS_MAIN, so none of gameosmain's gate hooks run here.
	mc2::gate::markMainEntered();
	mc2::gate::invalidateAll();
	mc2::gate::dumpConfig(stderr);

	// Mount mod overlays so the editor can load MOD missions + their appearance
	// assets, not just base-game content. mods/<MC2_ACTIVE_MOD>/data/* shadow base
	// data/* for relative reads -- the SAME call the game makes (mechcmd2.cpp:1229).
	// Without it the editor is base-game-only and CTDs loading a mod .pak whose
	// mech/object appearances live under mods/<mod>/data/tgl/ (e.g. chimera.ini).
	// Set MC2_ACTIVE_MOD=<modfolder> before launching to activate a mod.
	extern void InitModSearchPaths(const char* modsRoot);  // mclib/file.h
	InitModSearchPaths("./mods/");

	crashbundle_init();    // install SEH filter; writes crashes/<timestamp>/ on CTD
	EarlyTraceBegin();
	EarlyTrace("InitInstance: enter");
	EarlyTrace(EditorVersion_GetStartupLine());

	// S-CLI: parse before any MFC chrome so flags are stable for the rest of
	// startup. MFC's default arg handling still receives m_lpCmdLine — we
	// don't consume it — but we use only explicit -mission so MFC's implicit
	// "first non-flag = file to shell-open" path stays untouched.
	s_cli_parse(m_lpCmdLine ? (const char*)m_lpCmdLine : "");
	fprintf(stderr, "[EDITOR_CLI v1] event=parsed mission=\"%s\" frames=%ld exit_on_load_fail=%d\n",
		g_cliMissionPath.c_str(), g_cliFramesLimit, g_cliExitOnLoadFail ? 1 : 0);
	fflush(stderr);

	// Empty-install guard: an editor run from an asset-less install (no data/tgl/*.ini)
	// with no mod active will CTD the moment it loads any real mission's mechs/objects.
	// Warn up front instead of letting the user chase a mysterious crash (this exact
	// trap cost a long debug detour: the 0.4c editor install ships 0 tgl appearances).
	{
		// Probe REAL asset resolution rather than just loose data\tgl\*.ini files.
		// Stock installs ship tgl appearances as data/tgl/<name>.tgl packed inside
		// the tgl.fst FastFile (and/or under an active mod overlay), so the loose-dir
		// scan reported a false "no assets" warning even though missions render fine.
		// fileExists() walks the SAME priority chain a real load uses:
		//   active mod  ->  base data/  ->  FastFile (tgl.fst).
		// Use forward slashes -- FST keys are stored slash-normalized (file.cpp:746).
		// 'anubis' is a canonical Clan-mech appearance present in every install.
		extern long fileExists(const char* fName, long destination_mask);
		const long kFileOnDiskOrFst = 1 /*FILE_ON_DISK*/ | 2 /*FILE_ON_FST*/;
		bool canResolveTgl = (fileExists("data/tgl/anubis.tgl", kFileOnDiskOrFst) != 0);

		if (!canResolveTgl)
		{
			EarlyTrace("InitInstance: WARNING tgl appearances unresolvable (no loose data/tgl, no tgl.fst, no mod) -- mission loads will likely CTD");
			// Non-blocking: surface as a persistent dismissable ImGui banner in the
			// editor HUD instead of a modal the user must click OK on every launch.
			extern void EditorSetStartupWarning(const char* text);
			EditorSetStartupWarning(
				"No game assets in data\\tgl -- stock missions will crash. "
				"Open a Mod Project or run from a complete install.");
		}
	}

	// Auto-reopen the last Mod Project (interactive only -- never in a headless smoke, and
	// ReopenLastIfAny opens NO dialogs regardless). Binds save-dir default + remounts the mod.
	if (g_cliExitAfterSec <= 0)
		EditorModProject::ReopenLastIfAny();

	// Start the smoke driver thread NOW (before the blocking init/gen/save on the
	// main thread) so it can auto-dismiss modals throughout and enforce the timed
	// exit. Defined below; forward-declared above InitInstance.
	if (g_cliExitAfterSec > 0)
	{
		EarlyTrace("InitInstance: starting smoke driver thread");
		::CreateThread(nullptr, 0, s_smoke_thread, nullptr, 0, nullptr);
	}

#ifdef _AFXDLL
	// Enable3dControls() is deprecated and no-op on modern MSVC -- removed per C4996
#else
	Enable3dControlsStatic();	// Call this when linking to MFC statically
#endif

	SetRegistryKey(_T("Local AppWizard-Generated Applications"));

	MainFrame* pFrame = new MainFrame;
	m_pMainWnd = pFrame;
	EarlyTrace("InitInstance: m_pMainWnd set");

	// Create the real MFC frame first.  The EditorInterface view is normally
	// constructed from MainFrame::OnCreate; if that route fails, create one here.
	if (!pFrame->LoadFrame(IDR_EDITOR_MENU, WS_OVERLAPPEDWINDOW | FWS_ADDTOTITLE, nullptr, nullptr))
	{
		EarlyTrace("InitInstance: LoadFrame FAILED");
		delete pFrame;
		m_pMainWnd = nullptr;
		return FALSE;
	}
	EarlyTrace("InitInstance: LoadFrame OK");

	HICON editorIco = LoadIcon(IDI_ICON1);
	if (editorIco)
	{
		pFrame->SetIcon(editorIco, TRUE);
		pFrame->SetIcon(editorIco, FALSE);
	}

	// Make sure the frame has a native MFC menu.  Some remastered startup paths
	// leave the frame alive but without a menu attached, which hides the editor UI.
	if (pFrame->GetMenu() == nullptr)
	{
		CMenu* pMenu = new CMenu;
		if (pMenu->LoadMenu(IDR_EDITOR_MENU))
		{
			pFrame->SetMenu(pMenu);
			pMenu->Detach(); // ownership transferred to the frame HWND
			EarlyTrace("InitInstance: menu loaded and attached");
		}
		else
		{
			EarlyTrace("InitInstance: menu LoadMenu FAILED");
		}
		delete pMenu;
	}
	else
	{
		EarlyTrace("InitInstance: frame already has menu");
	}

	EditorInterface* editorInterface = EditorInterface::instance();
	if (!editorInterface)
	{
		EarlyTrace("InitInstance: EditorInterface missing after LoadFrame; creating fallback child");

		CRect clientRect;
		pFrame->GetClientRect(&clientRect);

		g_editorInterfaceWindow = new EditorInterface;
		if (!g_editorInterfaceWindow->Create(
			nullptr,
			_T("EditorInterface"),
			WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
			clientRect,
			pFrame,
			AFX_IDW_PANE_FIRST,
			nullptr))
		{
			EarlyTrace("InitInstance: fallback EditorInterface Create FAILED");
			delete g_editorInterfaceWindow;
			g_editorInterfaceWindow = nullptr;
		}
		else
		{
			EarlyTrace("InitInstance: fallback EditorInterface Create OK");
			g_editorInterfaceWindow->ShowWindow(EditorBackgroundLaunch() ? SW_SHOWNOACTIVATE : SW_MAXIMIZE);
			g_editorInterfaceWindow->UpdateWindow();
		}
	}
	else
	{
		EarlyTrace("InitInstance: EditorInterface exists after LoadFrame");
	}

	if (EditorBackgroundLaunch())
		EarlyTrace("editor: background launch (MC2_NO_ACTIVATE/MC2_WINDOW_OFFSCREEN) -- minimized, no focus");
	pFrame->ShowWindow(EditorBackgroundLaunch() ? SW_SHOWMINNOACTIVE : SW_MAXIMIZE);
	pFrame->UpdateWindow();
	pFrame->DrawMenuBar();

	HWND editorWnd = nullptr;
	editorInterface = EditorInterface::instance();
	if (editorInterface && editorInterface->GetSafeHwnd())
	{
		editorWnd = editorInterface->GetSafeHwnd();
		EarlyTrace("InitInstance: EditorInterface hwnd valid");
	}
	else
	{
		EarlyTrace("InitInstance: EditorInterface hwnd NULL before InitGameOS");
	}

	// SDL_CreateWindow on Windows requires an active Win32 message pump on the
	// calling thread: WGL calls CreateWindowEx internally, which sends WM_CREATE
	// synchronously, and DWM/driver setup needs the pump to dispatch those messages.
	// Calling SDL_CreateWindow here (before CWinApp::Run() enters the pump) causes
	// a deadlock or driver crash that kills the process a few seconds in.
	//
	// Defer InitGameOS to the first OnIdle tick, at which point Run() has entered
	// the pump and the thread is fully message-capable.
	m_hEditorWnd          = editorWnd;
	m_sCmdLine            = m_lpCmdLine ? m_lpCmdLine : _T("");
	m_bPendingInitGameOS  = true;
	EarlyTrace("InitInstance: InitGameOS deferred to first OnIdle");

	if (!EditorBackgroundLaunch())
	{
		if (editorInterface && editorInterface->GetSafeHwnd())
			editorInterface->SetFocus();
		else
			pFrame->SetFocus();
	}

	EarlyTrace("InitInstance: return TRUE");
	return TRUE;
}







class CAboutDlg : public CDialog
{
public:
	CAboutDlg();

	enum { IDD = IDD_FOG };

	protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support

protected:
	DECLARE_MESSAGE_MAP()
};

CAboutDlg::CAboutDlg() : CDialog(CAboutDlg::IDD)
{
}

void CAboutDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
}

BEGIN_MESSAGE_MAP(CAboutDlg, CDialog)
END_MESSAGE_MAP()

void EditorMFCApp::OnAppAbout()
{
	CAboutDlg aboutDlg;
	aboutDlg.DoModal();
}

/////////////////////////////////////////////////////////////////////////////
// EditorMFCApp message handlers


// Emit the machine-readable smoke summary exactly once (idempotent). Written to
// both the inherited stderr pipe and editor-startup.log (the reliable IPC channel
// for a WIN32-subsystem app). The Python runner turns these facts into a verdict.
static void s_emit_esmoke(const char* how)
{
	if (g_cliEsmokeEmitted) return;
	g_cliEsmokeEmitted = true;
	char esmoke[720];
	_snprintf(esmoke, sizeof(esmoke),
		"[ESMOKE v1] event=summary how=%s clean_exit=1 frames=%ld autoload=%d gen_map=%d "
		"foliage_smoke=%d foliage_count=%d saved=%d "
		"outliner_count=%d outliner_selected=%d "
		"inspector_selected=%d inspector_type=%s "
		"validate_blocking=%d validate_warning=%d validate_info=%d validate_units_warn=%d "
		"inspector_edit_applied=%d inspector_edit_undo=%d "
		"asset_groups=%d asset_activated=%d "
		"menu_vis0=%d menu_vis1=%d menu_vis2=%d menu_clear=%d menu_reload=%d "
		"place_oob_survived=%d",
		how, g_cliFrameCounter, g_cliAutoLoadFired ? 1 : 0, g_cliGenMap ? 1 : 0,
		g_cliSmokeFoliageFired ? 1 : 0, g_cliSmokeFoliageCount, g_cliSmokeSaveOk,
		g_cliSmokeOutlinerCount, g_cliSmokeOutlinerSel,
		g_cliSmokeInspectorSel, g_cliSmokeInspectorType,
		g_cliValidateBlock, g_cliValidateWarn, g_cliValidateInfo, g_cliValidateUnitsWarn,
		g_cliEditApplied, g_cliEditUndo,
		g_cliAssetGroups, g_cliAssetActivated,
		g_menuVis0, g_menuVis1, g_menuVis2, g_menuClearCount, g_menuReloadCount,
		g_cliPlaceOobSurvived);
	esmoke[sizeof(esmoke) - 1] = '\0';
	fprintf(stderr, "%s\n", esmoke);
	fflush(stderr);
	EarlyTrace(esmoke);
}

// Both IPC channels carry panel facts because WIN32 apps may have no stderr pipe.
void EditorSmokePanelFact(const char* workflow, const char* panel, bool ok, bool modelOnly)
{
	char line[256];
	std::snprintf(line, sizeof(line), "[EDITOR_PANEL v1] workflow=%s panel=%s coverage=%s reached=%d ok=%d authored=%d undo=%d saved=%d reloaded=%d",
		workflow, panel, modelOnly ? "model-only" : "action-handler", modelOnly ? 0 : 1,
		ok ? 1 : 0, ok ? 1 : 0, ok ? 1 : 0, ok ? 1 : 0, ok ? 1 : 0);
	fprintf(stderr, "%s\n", line);
	fflush(stderr);
	EarlyTrace(line);
}

// Suppressed validation dialogs remain visible to the headless smoke parser.
void EditorSmokeLogSuppressedModal(const char* text, const char* caption)
{
	if (!text) text = "";
	if (!caption) caption = "";

	// Flatten newlines and escape quotes/backslashes into a single-line field.
	char clean[600];
	size_t o = 0;
	for (const char* p = text; *p && o + 2 < sizeof(clean); ++p)
	{
		char c = *p;
		if (c == '\r') continue;
		if (c == '\n' || c == '\t') { clean[o++] = ' '; continue; }
		if (c == '"' || c == '\\') { clean[o++] = '\\'; if (o + 1 >= sizeof(clean)) break; }
		clean[o++] = c;
	}
	clean[o] = '\0';

	char line[760];
	_snprintf(line, sizeof(line),
		"[ESMOKE v1] event=modal caption=\"%s\" text=\"%s\"", caption, clean);
	line[sizeof(line) - 1] = '\0';
	fprintf(stderr, "%s\n", line);
	fflush(stderr);
	EarlyTrace(line);
}

// --- Non-blocking startup warning banner ----------------------------------
// Set once during InitInstance (e.g. empty-install guard). Rendered as a
// dismissable yellow banner in the EditorInterface HUD; cleared for the
// session when the user clicks the banner's "x". Stays set even if the
// underlying condition later resolves (kept simple — dismiss is manual).
static char s_editorStartupWarning[512] = "";

void EditorSetStartupWarning(const char* text)
{
	if (!text) text = "";
	_snprintf(s_editorStartupWarning, sizeof(s_editorStartupWarning), "%s", text);
	s_editorStartupWarning[sizeof(s_editorStartupWarning) - 1] = '\0';
}

// Returns the active warning text, or NULL/empty if none / dismissed.
const char* EditorStartupWarning()
{
	return s_editorStartupWarning[0] ? s_editorStartupWarning : nullptr;
}

void EditorClearStartupWarning()
{
	s_editorStartupWarning[0] = '\0';
}

// Auto-dismiss any modal dialog (#32770 = standard dialog/MessageBox class) owned
// by this process. Headless smoke runs have no one to click OK, and modals appear
// from many scattered places (startup, generate, and especially SAVE validation
// warnings that are built from resource keys and impossible to gate individually).
// We post IDOK + WM_CLOSE so both OK-only and OK/Cancel boxes go away.
// Single smoke driver thread, started at InitInstance. Once the auto-load work
// (generate + optional foliage + optional save) is done, it counts down
// g_cliExitAfterSec, emits the summary, and hard ExitProcess(0)s. A dedicated
// thread is immune to the editor's WM_TIMER render pump starving CWinApp::OnIdle.
// Modals are suppressed AT SOURCE (EMessageBox / EditorData AfxMessageBox gates on
// g_cliSuppressModals), so no fragile external dialog-dismissal is needed -- that
// approach (PostMessage WM_CLOSE) crashed the editor by hitting non-modal panels.
// An absolute backstop fires if the load step never completes.
static DWORD WINAPI s_smoke_thread(LPVOID)
{
	DWORD start = ::GetTickCount();
	DWORD loadTick = 0;
	DWORD sec = (DWORD)(g_cliExitAfterSec > 0 ? g_cliExitAfterSec : 12);
	// -smoke-foliage-menu: once foliage is loaded, drive the real menu commands via
	// WM_COMMAND (the same path a click takes) with renders in between, recording
	// visibility/count at each step. PostMessage from this thread queues onto the
	// main thread, which runs the handler between render frames.
	if (g_cliSmokeFoliageMenu)
	{
		while (!(g_cliSmokeWorkDone && g_smokeMainHwnd) && (::GetTickCount() - start) < 120000u)
			::Sleep(100);
		HWND w = (HWND)g_smokeMainHwnd;
		if (w)
		{
			::Sleep(700);
			g_menuVis0 = FoliageRender::Visible() ? 1 : 0;
			::PostMessageA(w, WM_COMMAND, MAKEWPARAM(ID_FOLIAGE_TOGGLE, 0), 0);  // off
			::Sleep(900);
			g_menuVis1 = FoliageRender::Visible() ? 1 : 0;
			::PostMessageA(w, WM_COMMAND, MAKEWPARAM(ID_FOLIAGE_TOGGLE, 0), 0);  // on
			::Sleep(900);
			g_menuVis2 = FoliageRender::Visible() ? 1 : 0;
			::PostMessageA(w, WM_COMMAND, MAKEWPARAM(ID_FOLIAGE_CLEAR, 0), 0);   // clear
			::Sleep(900);
			g_menuClearCount = FoliageRender::Count();
			::PostMessageA(w, WM_COMMAND, MAKEWPARAM(ID_FOLIAGE_GENERATE, 0), 0); // reload (shells generator)
			// Generate re-runs the generator synchronously on the main thread; poll
			// for the reloaded instance count. The regen time scales with the random
			// map's foliage density (a heavy map can place >20k instances, ~30s+), so
			// the poll window is generous (180 * 500ms = 90s) to avoid a false
			// reload=0 when the synchronous regen simply hasn't returned yet.
			for (int i = 0; i < 180 && FoliageRender::Count() <= 0; ++i)
				::Sleep(500);
			g_menuReloadCount = FoliageRender::Count();
		}
		s_emit_esmoke("foliage_menu");
		::ExitProcess(0);
	}

	for (;;)
	{
		DWORD now = ::GetTickCount();
		if (g_cliSmokeWorkDone && loadTick == 0)
			loadTick = now;
		bool loadedDone = (loadTick != 0 && (now - loadTick) >= sec * 1000u);
		bool absBackstop = (now - start) >= 180000u;   // never-loaded safety exit
		if (loadedDone || absBackstop)
		{
			s_emit_esmoke(loadedDone ? "smoke_thread" : "abs_backstop");
			::ExitProcess(0);
		}
		::Sleep(200);
	}
	return 0;
}

// -playtest driver tick. Called from OnIdle AND from the 8 ms viewport render timer:
// the render WM_TIMER keeps the queue non-empty, so OnIdle alone is starved and the
// driver never reached Start() (process_timeout). Both callers run on the main thread.
void EditorPlaytestSmokeDriverTick()
{
	// -playtest END-TO-END SMOKE driver (main thread). Runs once the auto-load
	// work is done. Warms a few frames, fires the REAL EditorPlaytest::Start()
	// (the toolbar button path) with the child in smoke mode, then polls
	// EditorPlaytest::IsIdle() (the task completion is drained by
	// EditorTaskRunner::PumpMainThread() inside EditorInterface::update()).
	if (g_cliPlaytest && g_cliSmokeWorkDone && !g_cliPlaytestDone)
	{
		if (!g_cliPlaytestStarted)
		{
			// Let a few frames render so GL / asset / texture state settles before
			// the in-place save + child launch.
			if (g_cliPlaytestWarmFrames < 30)
			{
				++g_cliPlaytestWarmFrames;
			}
			else
			{
				int secs = g_cliPlaytestTimeout > 0 ? g_cliPlaytestTimeout : 120;
				// Child auto-quit window: a bit shorter than the editor timeout so a
				// healthy child exits on its own before we declare a timeout.
				int childSecs = secs > 40 ? 30 : (secs > 10 ? secs - 8 : secs);
				EditorPlaytest::SetSmokeChildEnv(true, childSecs);
				EarlyTrace("OnIdle: -playtest Start()");
				EditorPlaytest::Start();
				g_cliPlaytestStarted = true;
				g_cliPlaytestDeadline = ::GetTickCount() + (unsigned int)secs * 1000u;

				if (!EditorPlaytest::IsRunning())
				{
					// Start() aborted pre-launch (no mission / save / bridge / exe).
					g_cliPlaytestDone = true;
					fprintf(stderr,
						"[ESMOKE v1] event=playtest exit=-1 log= mod=%s status=launch_abort\n",
						(EditorPlaytest::LastModId()[0] ? EditorPlaytest::LastModId() : "none"));
					fprintf(stderr, "[EDITOR_CLI v1] event=playtest_abort status=\"%s\"\n",
						EditorPlaytest::StatusLine());
					fflush(stderr);
					EarlyTrace("OnIdle: -playtest launch abort");
					EarlyTrace(EditorPlaytest::StatusLine());
					::ExitProcess(2);
				}
			}
		}
		else
		{
			// Running: wait for the child to complete (IsIdle) or the timeout.
			if (EditorPlaytest::IsIdle())
			{
				g_cliPlaytestDone = true;
				int childExit = EditorPlaytest::LastExitCode();
				const char* logp = EditorPlaytest::LastLogPath();
				const char* modId = EditorPlaytest::LastModId();
				fprintf(stderr,
					"[ESMOKE v1] event=playtest exit=%d log=%s mod=%s\n",
					childExit,
					(logp && logp[0]) ? logp : "(none)",
					(modId && modId[0]) ? modId : "none");
				fprintf(stderr, "[EDITOR_CLI v1] event=playtest_done child_exit=%d\n", childExit);
				fflush(stderr);
				EarlyTrace("OnIdle: -playtest child completed");
				EarlyTrace(EditorPlaytest::StatusLine());
				::ExitProcess(childExit == 0 ? 0 : 3);
			}
			else if (::GetTickCount() >= g_cliPlaytestDeadline)
			{
				g_cliPlaytestDone = true;
				EarlyTrace("OnIdle: -playtest TIMEOUT -> Stop()");
				EditorPlaytest::Stop();
				const char* modId = EditorPlaytest::LastModId();
				fprintf(stderr,
					"[ESMOKE v1] event=playtest exit=-1 log=%s mod=%s status=timeout\n",
					(EditorPlaytest::LastLogPath()[0] ? EditorPlaytest::LastLogPath() : "(none)"),
					(modId && modId[0]) ? modId : "none");
				fflush(stderr);
				::ExitProcess(4);
			}
		}
	}
}

BOOL EditorMFCApp::OnIdle(LONG lCount)
{
	CWinApp::OnIdle(lCount);

	// First-tick deferred initialisation: the MFC message pump is now live so
	// SDL_CreateWindow (which sends WM_CREATE synchronously via CreateWindowEx
	// and needs the pump to dispatch DWM/WGL messages) is safe to call.
	if (m_bPendingInitGameOS)
	{
		m_bPendingInitGameOS = false;
		EarlyTrace("OnIdle: deferred InitGameOS: enter");
		InitGameOS(m_hInstance, m_hEditorWnd, (char*)(LPCSTR)m_sCmdLine);
		EarlyTrace("OnIdle: deferred InitGameOS: complete");
	}

	// S-CLI: auto-load a mission on the FIRST idle after InitGameOS so the GL
	// context, asset system, and texture manager are fully up. One-shot.
	if (!g_cliAutoLoadFired && (!g_cliMissionPath.empty() || g_cliNewMap || g_cliGenMap) && !m_bPendingInitGameOS)
	{
		g_cliAutoLoadFired = true;
		bool ok = false;
		if (g_cliGenMap)
		{
			fprintf(stderr, "[EDITOR_CLI v1] event=gen_map_begin size=%d terrain=%d\n",
				g_cliNewMapSize, g_cliNewMapTerrain);
			fflush(stderr);
			EarlyTrace("OnIdle: S-CLI auto-gen-map: enter");
			ok = EditorData::generateMission(g_cliNewMapSize, g_cliNewMapTerrain, 12345u);
			EarlyTrace(ok ? "OnIdle: S-CLI auto-gen-map: OK" : "OnIdle: S-CLI auto-gen-map: FAILED");
			fprintf(stderr, "[EDITOR_CLI v1] event=gen_map_end ok=%d\n", ok ? 1 : 0);
			fflush(stderr);
		}
		else if (g_cliNewMap)
		{
			fprintf(stderr, "[EDITOR_CLI v1] event=new_map_begin size=%d terrain=%d\n",
				g_cliNewMapSize, g_cliNewMapTerrain);
			fflush(stderr);
			EarlyTrace("OnIdle: S-CLI auto-new-map: enter");
			ok = EditorData::initTerrainFromTGA(g_cliNewMapSize, 0, 0, g_cliNewMapTerrain);
			EarlyTrace(ok ? "OnIdle: S-CLI auto-new-map: OK" : "OnIdle: S-CLI auto-new-map: FAILED");
			fprintf(stderr, "[EDITOR_CLI v1] event=new_map_end ok=%d\n", ok ? 1 : 0);
			fflush(stderr);
		}
		else
		{
			EarlyTrace("OnIdle: S-CLI auto-load: enter");
			ok = EditorData::initTerrainFromPCV(g_cliMissionPath.c_str());
			EarlyTrace(ok ? "OnIdle: S-CLI auto-load: OK" : "OnIdle: S-CLI auto-load: FAILED");
		}
		if (!ok && g_cliExitOnLoadFail)
		{
			fprintf(stderr, "[EDITOR_CLI v1] event=load_fail mission=\"%s\" newmap=%d\n",
				g_cliMissionPath.c_str(), g_cliNewMap ? 1 : 0);
			fflush(stderr);
			exit(1);
		}

		// -smoke-foliage: exercise the foliage preview once the map is up. Load()
		// is failure-tolerant (missing/garbage JSON -> false, 0 instances), so this
		// must never crash; Toggle() flips visibility so the render path runs both
		// states over the remaining frames. Facts only -> [ESMOKE v1] at exit.
		if (g_cliSmokeFoliage)
		{
			bool floaded = FoliageRender::Load(g_cliSmokeFoliagePath.c_str());
			g_cliSmokeFoliageCount = FoliageRender::Count();
			FoliageRender::Toggle();   // exercise hidden state too
			FoliageRender::Toggle();   // ...and back to visible for the frame loop
			g_cliSmokeFoliageFired = true;
			fprintf(stderr, "[EDITOR_CLI v1] event=foliage loaded=%d count=%d path=\"%s\"\n",
				floaded ? 1 : 0, g_cliSmokeFoliageCount, g_cliSmokeFoliagePath.c_str());
			fflush(stderr);
			EarlyTrace(floaded ? "OnIdle: smoke-foliage Load OK" : "OnIdle: smoke-foliage Load returned false (tolerated)");
		}

		if (g_cliSmokeWorkflows && ok && EditorInterface::instance())
			EditorInterface::instance()->runWorkflowSmoke("terrain_gen_out/e0_stock_campaign.fit");
		if (g_cliSmokeCatalogReference && ok && EditorInterface::instance())
			EditorInterface::instance()->runCatalogReferenceSmoke();

		// Explicit destinations preserve stock inputs while exercising the mission writer.
		if (g_cliSmokeSave && ok && EditorData::instance)
		{
			// quickSave=true skips the heavy non-quickSave tail (updateTacMap +
			// MOVE rebuild + tac-map packet) that stalls headless on large maps. The
			// .pak still carries the terrain + object packets, so it reloads fine in
			// the editor (the load-back phase of the smoke).
			bool saved = EditorData::instance->save((char*)g_cliSmokeSavePath.c_str(), true);
			g_cliSmokeSaveOk = saved ? 1 : 0;
			fprintf(stderr, "[EDITOR_CLI v1] event=save ok=%d path=\"%s\"\n",
				saved ? 1 : 0, g_cliSmokeSavePath.c_str());
			fflush(stderr);
			EarlyTrace(saved ? "OnIdle: smoke-save OK" : "OnIdle: smoke-save FAILED");
		}

		// -smoke-outliner: exercise the read-only Scene Outliner. Open the panel,
		// enumerate placed objects (counts), and select the first selectable one
		// through the existing EditorObjectMgr path. All read-only except selection;
		// safe on an empty map (count 0, selected 0). Facts -> [ESMOKE v1] at exit.
		if (g_cliSmokeOutliner)
		{
			SceneOutliner::Open();
			OutlinerCounts oc = SceneOutliner::ComputeCounts();
			g_cliSmokeOutlinerCount = oc.total();
			g_cliSmokeOutlinerSel = SceneOutliner::SelectFirstObject() ? 1 : 0;
			if (g_cliSmokeFrameSelection && g_cliSmokeOutlinerSel)
				EditorInterface::frameSelectedObjects();
			fprintf(stderr, "[EDITOR_CLI v1] event=outliner count=%d selected=%d "
				"units=%d buildings=%d navmarkers=%d dropzones=%d forests=%d\n",
				g_cliSmokeOutlinerCount, g_cliSmokeOutlinerSel,
				oc.units, oc.buildings, oc.navMarkers, oc.dropZones, oc.forests);
			fflush(stderr);
			EarlyTrace("OnIdle: smoke-outliner done");
		}

		// -smoke-inspector: select the first object (existing path) then ask the
		// read-only Inspector for its selection summary. Safe on an empty map
		// (selected=0, type=none). Facts -> [ESMOKE v1] at exit.
		if (g_cliSmokeInspector)
		{
			SceneOutliner::SelectFirstObject();   // no-op/false on empty map
			InspectorPanel::Open();
			bool had = InspectorPanel::GetSelectionSummary(
				g_cliSmokeInspectorType, sizeof(g_cliSmokeInspectorType));
			g_cliSmokeInspectorSel = had ? 1 : 0;
			fprintf(stderr, "[EDITOR_CLI v1] event=inspector selected=%d type=%s\n",
				g_cliSmokeInspectorSel, g_cliSmokeInspectorType);
			fflush(stderr);
			EarlyTrace("OnIdle: smoke-inspector done");
		}

		// -smoke-validate: open the live Mission Save Readiness panel and tally its
		// checks (read-only). A freshly generated empty map has no units, so a
		// unit-staffing warning is expected. Facts -> [ESMOKE v1] at exit.
		if (g_cliSmokeValidate)
		{
			MissionValidator::Open();
			int vb = 0, vw = 0, vi = 0;
			MissionValidator::GetIssueCounts(vb, vw, vi);
			g_cliValidateBlock = vb;
			g_cliValidateWarn  = vw;
			g_cliValidateInfo  = vi;
			g_cliValidateUnitsWarn = MissionValidator::HasUnitStaffingWarning() ? 1 : 0;
			fprintf(stderr, "[EDITOR_CLI v1] event=validate blocking=%d warning=%d info=%d "
				"units_warn=%d\n", vb, vw, vi, g_cliValidateUnitsWarn);
			fflush(stderr);
			EarlyTrace("OnIdle: smoke-validate done");
		}

		// -smoke-inspector-edit: exercise the editable Inspector transform + undo
		// end to end (places a throwaway drop zone, transforms it, undoes). Facts
		// -> [ESMOKE v1] at exit. Requires terrain (gen-map) to place the object.
		if (g_cliSmokeInspectorEdit && EditorInterface::instance())
		{
			int r = EditorInterface::instance()->runInspectorEditSmoke();
			g_cliEditApplied = (r >= 0 && (r & 1)) ? 1 : 0;
			g_cliEditUndo    = (r >= 0 && (r & 2)) ? 1 : 0;
			fprintf(stderr, "[EDITOR_CLI v1] event=inspector_edit setup=%d applied=%d undo=%d\n",
				(r >= 0) ? 1 : 0, g_cliEditApplied, g_cliEditUndo);
			fflush(stderr);
			EarlyTrace("OnIdle: smoke-inspector-edit done");
		}

		// -smoke-asset-browser: tally the existing object catalog and activate the
		// first placeable object through the existing placement path. Facts ->
		// [ESMOKE v1]. (Runtime validation deferred while map-load is broken.)
		if (g_cliSmokeAssetBrowser)
		{
			AssetBrowser::Open();
			g_cliAssetGroups    = AssetBrowser::GroupCount();
			g_cliAssetActivated = AssetBrowser::ActivateFirstObject() ? 1 : 0;
			fprintf(stderr, "[EDITOR_CLI v1] event=asset_browser groups=%d activated=%d\n",
				g_cliAssetGroups, g_cliAssetActivated);
			fflush(stderr);
			EarlyTrace("OnIdle: smoke-asset-browser done");
		}

		// -smoke-gameplay-debugger: open the read-only Gameplay Debugger, select
		// the first object, and run its ImGui-free probe (static-data path; the
		// editor has no live sim). Safe on empty map. Facts -> [ESMOKE v1].
		if (g_cliSmokeGameplayDbgr)
		{
			SceneOutliner::SelectFirstObject();   // no-op/false on empty map
			GameplayDebugger::Open();
			bool had = GameplayDebugger::SmokeProbe(
				g_cliGameplayDbgrType, sizeof(g_cliGameplayDbgrType));
			g_cliGameplayDbgrSel = had ? 1 : 0;
			fprintf(stderr, "[EDITOR_CLI v1] event=gameplay_debugger selected=%d type=%s\n",
				g_cliGameplayDbgrSel, g_cliGameplayDbgrType);
			fflush(stderr);
			EarlyTrace("OnIdle: smoke-gameplay-debugger done");
		}

		// -smoke-undo-history: open the display-only Undo History panel and read
		// the ActionUndoMgr accessors added for it. Read-only; empty stack on a
		// fresh map (count 0, cursor -1). Facts -> [ESMOKE v1].
		if (g_cliSmokeUndoHistory)
		{
			UndoHistoryPanel::Open();
			if (ActionUndoMgr::instance)
			{
				g_cliUndoCount  = ActionUndoMgr::instance->GetActionCount();
				g_cliUndoCursor = ActionUndoMgr::instance->GetCurrentPosition();
			}
			fprintf(stderr, "[EDITOR_CLI v1] event=undo_history count=%d cursor=%d\n",
				g_cliUndoCount, g_cliUndoCursor);
			fflush(stderr);
			EarlyTrace("OnIdle: smoke-undo-history done");
		}

		// -smoke-place-oob: drive a BuildingBrush cursor off the map edge. If the
		// worldToCell OOB clamp is missing this crashes in MapData::terrainElevation;
		// survived=1 proves the regression guard holds. Facts -> [ESMOKE v1].
		if (g_cliSmokePlaceOob && EditorInterface::instance())
		{
			int r = EditorInterface::instance()->runPlaceOobSmoke();
			g_cliPlaceOobSurvived = (r == 1) ? 1 : 0;
			fprintf(stderr, "[EDITOR_CLI v1] event=place_oob setup=%d survived=%d\n",
				(r >= 0) ? 1 : 0, g_cliPlaceOobSurvived);
			fflush(stderr);
			EarlyTrace("OnIdle: smoke-place-oob done");
		}

		// All auto-load work (generate + foliage + save) is complete -> arm the
		// smoke exit countdown from here.
		g_cliSmokeWorkDone = true;
		EarlyTrace("OnIdle: smoke work done");
	}

	EditorInterface* pEditor = EditorInterface::instance();
	if (pEditor && ::IsWindow(pEditor->m_hWnd))
	{
		// The 3D viewport is an embedded child window.  The old foreground-window
		// check only repainted when the MFC frame itself was foreground, which
		// starves the viewport when focus is inside a child/control.
		::InvalidateRect(pEditor->m_hWnd, nullptr, FALSE);
	}

	// Capture the main frame HWND on the main thread so the smoke driver thread can
	// PostMessage WM_COMMAND foliage-menu commands to it (-smoke-foliage-menu).
	if (g_cliSmokeFoliageMenu && g_smokeMainHwnd == nullptr && AfxGetMainWnd())
		g_smokeMainHwnd = AfxGetMainWnd()->GetSafeHwnd();

	EditorPlaytestSmokeDriverTick();

	Sleep(2/*milliseconds*/); /* limits the framerate to 500fps */
	return 1;
}


int EditorMFCApp::ExitInstance()
{
	// Machine-readable smoke summary for the non-smoke-exit path (e.g. -frames or a
	// real user close). Idempotent with the OnIdle deadline emit.
	s_emit_esmoke("exit_instance");

	// Failsafe: kill any still-running playtest / cook child this editor launched so it
	// is not orphaned when the editor exits. Terminates ONLY our own children (by stored
	// HANDLE) -- concurrent smoke / standalone mc2.exe instances are untouched. The Job
	// Object (kill-on-job-close) is the crash-path backstop; this is the clean-exit path.
	EditorTaskRunner::ShutdownKillRunning();

	{
		Environment.TerminateGameEngine();
		gos_PushCurrentHeap(0); // TerminateGameEngine() forgets to do this
	}
	if (false) {
		ExitGameOS();
		if (!EditorInterface::instance()->m_hWnd)
		{
			/* ExitGameOS() shuts down directX which has the side effect of killing the
			EditorInterface window, so we recreate the window here. The editor window
			may not be referenced after this function is executed, but this is not the correct
			place for the EditorInterface window to be destroyed. */
			EditorInterface::instance()->Create(nullptr, nullptr, AFX_WS_DEFAULT_VIEW | WS_VSCROLL | WS_HSCROLL,
				CRect(0, 0, 0, 0), m_pMainWnd, AFX_IDW_PANE_FIRST, nullptr);
		}
	}

	delete m_pMainWnd;
	m_pMainWnd = 0;

	return CWinApp::ExitInstance();
}

