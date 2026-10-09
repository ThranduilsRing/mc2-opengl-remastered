// EDITOR-only stub for mc2_unitprofile_collect_witness.
//
// The shared debug-state-dump TU is compiled into the editor lib and references
// mc2_unitprofile_collect_witness (added by the UNIT-PROFILE-SEAM work). The editor does NOT
// need unit-profile witness data (a game-runtime debug feature), and pulling the real
// code/unitprofile_fit.cpp into EditRel cascades engine dependencies. Provide a no-op definition
// so EditRel links. The game target (mc2) links the real definition; this stub is editor-only
// (picked up by the editor/*.cpp glob), so there is exactly one definition per target.
//
// extern "C" linkage => the symbol name is unmangled, so a forward-declared (incomplete) pointer
// parameter matches the call site for linking; we never dereference it.

struct UnitProfileWitnessRow;

extern "C" int mc2_unitprofile_collect_witness(UnitProfileWitnessRow* /*out*/, int /*maxRows*/)
{
    return 0;  // no witness rows in the editor
}

// ---------------------------------------------------------------------------
// Further editor-only link stubs: game-layer entry points referenced by the
// shared gameos_editor.lib / mclib.lib TUs (debug_state_dump, gos_visual_capture,
// mech3d, gvactor). Real definitions live in code/missiongui.cpp, sim_identity.cpp,
// unitprofile_fit.cpp (game-only, heavy). All inert; no real logic duplicated.
// (mc2UnitProfileDataEnabled / theActionExecutorRegistry are linked from their
// real, light TUs via editor/CMakeLists.txt.)
// ---------------------------------------------------------------------------
struct ComponentWitnessRow;
struct UnitPreviewWitnessRow;

extern "C" {
int  mc2FreezeMoversActive() { return 0; }
int  mc2VisualCaptureGetPauseState() { return -1; }  // -1 = no mission interface
void mc2VisualCaptureSetPaused(int) {}
void mc2VisualCaptureRestorePauseState(int) {}
int  mc2SimIdentityWriteJson(char* buf, int cap, double, long)
{
    if (buf && cap > 0) buf[0] = '\0';
    return 0;
}
int mc2_component_collect_witness(ComponentWitnessRow*, int) { return 0; }
int mc2_unitpreview_collect_witness(UnitPreviewWitnessRow*, int) { return 0; }
const char* mc2_unitpreview_anim_tag_name(int) { return ""; }
}
