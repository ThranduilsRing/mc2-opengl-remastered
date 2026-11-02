// tools_game_stubs.cpp — link stubs for the offline data tools.
//
// LINK-CONFIG-FIX: makefst / pak / aseconv / makersp link the full engine
// closure (mclib + gameos + renderer libs) purely to satisfy the symbol
// references those libraries make against each other. A handful of game-layer
// globals are *defined* only in code/ translation units (logmain.cpp,
// objmgr.cpp, terrobj.cpp, gameplay_pick.cpp) that the tools deliberately do
// not compile (they would drag in the whole game). The renderer/mclib object
// files still reference them, so the link fails with these few unresolved
// externals.
//
// The tools never exercise the code paths that touch these globals, so trivial
// definitions are sufficient and correct for the tools' purpose. The types are
// forward-declared to reproduce the exact decorated symbol names without
// pulling in any game headers.
//
// If a tool ever genuinely needs one of these at runtime, that is a sign it
// should link the real defining TU instead of this stub.

#include <atomic>

class Camera;
class GameObjectManager;

// aseconv.cpp defines `eye` and `MaxMinUV` itself; compile this TU with
// DATA_TOOLS_STUBS_HAVE_EYE_AND_MAXMINUV defined for that target to avoid a
// duplicate-symbol (LNK2005). The other tools define neither and need both.
#ifndef DATA_TOOLS_STUBS_HAVE_EYE_AND_MAXMINUV
// code/logmain.cpp:103  -> CameraPtr eye = NULL;
Camera* eye = nullptr;

// code/logmain.cpp:73 / code/mechcmd2.cpp:169 -> float MaxMinUV = 8.0f;
float MaxMinUV = 8.0f;
#endif

// code/terrobj.cpp:209 -> std::atomic<unsigned long long> g_tobjAngularCyc{0};
std::atomic<unsigned long long> g_tobjAngularCyc{0ULL};

// code/objmgr.cpp:469 -> GameObjectManagerPtr ObjectManager = NULL;
GameObjectManager* ObjectManager = nullptr;

// code/gameplay_pick.cpp:202 -> void RunGameplayPickSelfTest()
void RunGameplayPickSelfTest() {}

// No gameplay camera runs in offline data tools (code/gamecam.cpp).
unsigned mc2CameraLightChangedCount() { return 0; }

// ---------------------------------------------------------------------------
// Inert definitions for game-layer entry points referenced by gameos.lib /
// mclib.lib (debug_state_dump, gos_visual_capture, mech3d, gvactor). They live
// in heavy game TUs (logmain/mechcmd2, missiongui, sim_identity, unitprofile_fit)
// the offline tools must not link. Never duplicate real logic here: every body
// is a no-op / "feature absent" answer. (mc2UnitProfileDataEnabled and
// theActionExecutorRegistry are NOT stubbed: their real TUs are linked.)
// ---------------------------------------------------------------------------

bool quitGame = false;    // code/logmain.cpp:48
bool gamePaused = false;  // code/logmain.cpp:50

extern "C" {
int  mc2FreezeMoversActive() { return 0; }
int  mc2VisualCaptureGetPauseState() { return -1; }  // -1 = no mission interface
void mc2VisualCaptureSetPaused(int) {}
void mc2VisualCaptureRestorePauseState(int) {}
int  mc2CaptureFocusMech(int, float*) { return 0; }
int  mc2CaptureFocusMechEx(int, int, float*, int*, int*, char*, int*) { return 0; }
int  mc2SimIdentityWriteJson(char* buf, int cap, double, long)
{
    if (buf && cap > 0) buf[0] = '\0';
    return 0;
}

struct UnitProfileWitnessRow;
struct ComponentWitnessRow;
struct UnitPreviewWitnessRow;
int mc2_unitprofile_collect_witness(UnitProfileWitnessRow*, int) { return 0; }
int mc2_component_collect_witness(ComponentWitnessRow*, int) { return 0; }
int mc2_unitpreview_collect_witness(UnitPreviewWitnessRow*, int) { return 0; }
const char* mc2_unitpreview_anim_tag_name(int) { return ""; }
}

// Referenced by gui_runtime (EditorInspector.cpp); real versions live in
// code/gamecam.cpp / game debug tooling. Same no-op shim as editor/EditorGameOS.cpp.
extern "C" {
void mc2_hzb_dump_camera_view(void) {}
void mc2_save_camera_bookmark(char* outPath, int outPathLen)
{
    if (outPath && outPathLen > 0) outPath[0] = '\0';
}
}
