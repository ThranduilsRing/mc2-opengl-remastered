

#include "GuiRuntime_internal.h"
#include <optional>
#include "mc2fmt.h"
#include "../GameOS/gameos/debug_renderer.h"


// Live cliff mesh-decal tuning panel.
// Forward-declared plain API (defined in mclib/cliff_decal_tuning.cpp; resolved at
// mc2 link) so this TU need not pull in Stuff/batcher headers. See that file.
namespace CliffDecalTuning {
    bool cliffDecal_hasDecal();
    void cliffDecal_getKnobs(float* scale, float* offset, float* lateral,
                             float* lift, float* yawDeg, float* pitchDeg);
    void cliffDecal_setKnobsAndApply(float scale, float offset, float lateral,
                                     float lift, float yawDeg, float pitchDeg);
    void cliffDecal_logValues();
}

bool g_imguiInitialized = false;
// The context THIS runtime created. A process can host a second ImGui owner
// (the standalone Viewer creates its own context too); ImGui's "current context"
// is a single global, so we pin our own before any teardown to avoid the backend
// shutdown deref-ing a NULL or foreign context (the on-exit crash in
// ImGui_ImplOpenGL3_Shutdown -> DestroyPlatformWindows). NULL for the main game
// where this is the only context, in which case pinning is a no-op.
static ImGuiContext* s_imguiContext = nullptr;

static std::vector<std::function<void()>> s_postRenderFns;

namespace gui_runtime_detail {

bool s_frameActive = false;
bool s_useSDL2Backend = false;
std::unordered_map<std::string, ImFont*> s_uiFonts;

static bool isEnabled() {
    static int cached = -1;
    if (cached < 0) {
        cached = mc2::gate::get<"MC2_IMGUI">();
    }
    return cached == 1;
}

// ImGui always draws through the Vulkan renderer (the GL ImGui renderer and its MC2_VK_IMGUI kill switch were removed).

static bool traceEnabled()
{
    return mc2::gate::get<"MC2_UI_DEFS_TRACE">();
}

static const char* fileForFamily(std::string_view family)
{
    if (family == "agency")        return "Agency-Regular.ttf";
    if (family == "agency_bold")   return "Agency-Bold.ttf";
    if (family == "impact")        return "Impact.ttf";
    if (family == "blade_runner")  return "BladeRunnerMovieFont.ttf";
    if (family == "cousine")       return "Cousine-Regular.ttf";
    if (family == "roboto")        return "Roboto-Medium.ttf";
    if (family == "liberation_sans_narrow") return "LiberationSansNarrow-Regular.ttf";
    return "LiberationSans-Regular.ttf";
}

static std::filesystem::path executableDirectory()
{
#if defined(_WIN32)
    char buffer[MAX_PATH] = {0};
    const DWORD len = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (len > 0 && len < MAX_PATH)
        return std::filesystem::path(buffer).parent_path();
#else
    char buffer[4096] = {0};
    const ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len > 0) {
        buffer[len] = '\0';
        return std::filesystem::path(buffer).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

static bool regularFile(const std::filesystem::path& p)
{
    std::error_code ec;
    return std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec);
}

static std::optional<std::filesystem::path> resolveFontFile(std::string_view filename)
{
    const std::filesystem::path exe = executableDirectory();
    const std::filesystem::path cwd = std::filesystem::current_path();
    const std::filesystem::path relData     = std::filesystem::path("data") / "fonts" / "ui" / filename;
    const std::filesystem::path relAssets   = std::filesystem::path("assets") / "graphics" / "fonts" / filename;
    const std::filesystem::path relVendored = std::filesystem::path("3rdparty") / "imgui" / "misc" / "fonts" / filename;

    const std::array<std::filesystem::path, 12> candidates = {
        exe / relData,
        cwd / relData,
        exe / relAssets,
        cwd / relAssets,
        exe / relVendored,
        cwd / relVendored,
        cwd.parent_path() / relVendored,
        exe.parent_path() / relVendored,
        cwd.parent_path().parent_path() / relVendored,
        exe.parent_path().parent_path() / relVendored,
        cwd.parent_path().parent_path().parent_path() / relVendored,
        exe.parent_path().parent_path().parent_path() / relVendored,
    };

    for (const std::filesystem::path& p : candidates) {
        if (regularFile(p))
            return p;
    }

    return std::nullopt;
}

static void loadFontFamily(ImGuiIO& io, std::string_view familyName, std::string_view filename)
{
    const auto fontPath = resolveFontFile(filename);
    if (!fontPath) {
        if (traceEnabled())
            mc2::fmt::print_err("[IMGUI_FONT] missing {} for family={}; using ImGui default\n", filename, familyName);
        return;
    }

    const int sizes[] = {8, 10, 12, 14, 16, 17, 18, 20, 22, 24, 28, 32, 36, 48};
    for (int size : sizes) {
        ImFont* font = io.Fonts->AddFontFromFileTTF(fontPath->string().c_str(), static_cast<float>(size));
        if (font)
            s_uiFonts[fontKey(familyName, size)] = font;
    }

    if (traceEnabled())
        mc2::fmt::print_err("[IMGUI_FONT] loaded family={} file={}\n", familyName, fontPath->string().c_str());
}

static void loadUiFonts(ImGuiIO& io)
{
    s_uiFonts.clear();
    io.Fonts->AddFontDefault();
    loadFontFamily(io, "agency",                fileForFamily("agency"));
    loadFontFamily(io, "agency_bold",           fileForFamily("agency_bold"));
    loadFontFamily(io, "impact",                fileForFamily("impact"));
    loadFontFamily(io, "blade_runner",          fileForFamily("blade_runner"));
    loadFontFamily(io, "cousine",               fileForFamily("cousine"));
    loadFontFamily(io, "roboto",                fileForFamily("roboto"));
    loadFontFamily(io, "liberation_sans",       fileForFamily("liberation_sans"));
    loadFontFamily(io, "liberation_sans_narrow",fileForFamily("liberation_sans_narrow"));
}

} // namespace gui_runtime_detail
using namespace gui_runtime_detail;

// Editor-mode flag (see header). Default false == GAME: no editor dockspace, so
// ImGui windows float. The editor sets it true before Init().
static bool s_editorMode = false;
void GuiRuntime::SetEditorMode(bool on) { s_editorMode = on; }
bool GuiRuntime::IsEditorMode() { return s_editorMode; }

void GuiRuntime::NotifyResize(int framebufferW, int framebufferH,
                               int windowW, int windowH)
{
    if (!g_imguiInitialized || !ImGui::GetCurrentContext())
        return;

    ImGuiIO& io = ImGui::GetIO();

    const float fbW = static_cast<float>(framebufferW > 0 ? framebufferW : 1);
    const float fbH = static_cast<float>(framebufferH > 0 ? framebufferH : 1);

    const float wW = static_cast<float>(windowW > 0 ? windowW : framebufferW);
    const float wH = static_cast<float>(windowH > 0 ? windowH : framebufferH);

    io.DisplaySize = ImVec2(wW, wH);

    // FramebufferScale bridges the logical-pixel DisplaySize and the
    // physical-pixel framebuffer for HiDPI displays.  On standard displays
    // this is (1,1); on Retina/HiDPI it is (2,2) or similar.
    io.DisplayFramebufferScale = ImVec2(wW > 0.0f ? fbW / wW : 1.0f,
                                        wH > 0.0f ? fbH / wH : 1.0f);
}

bool GuiRuntime::GetDisplaySize(float& w, float& h)
{
    w = 0.0f;
    h = 0.0f;
    // Prefer
    // Environment.drawableWidth/Height — the values gos updates SYNCHRONOUSLY
    // on every resolution/window change (gos_SetScreenMode handleEvents,
    // SIZE_CHANGED). io.DisplaySize is fed FROM these via
    // NotifyResize but lags during the load-time resolution-transition window
    // (the async load pump renders between the change request and the next
    // ImGui NewFrame), which drew load art / text with two different scales
    // for a frame. One source of truth ends that class. ImGui DisplaySize
    // stays as the fallback for early-boot frames where drawable is not yet
    // populated.
    {
        int dw = 0, dh = 0;
        gos_GetEnvironmentDrawableSize(&dw, &dh);
        if (dw > 0 && dh > 0) {
            w = (float)dw;
            h = (float)dh;
            return true;
        }
    }
    if (!g_imguiInitialized || !ImGui::GetCurrentContext())
        return false;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
        return false;
    w = io.DisplaySize.x;
    h = io.DisplaySize.y;
    return true;
}

void GuiRuntime::Init() {
    if (!isEnabled()) return;
    if (g_imguiInitialized) return;

    IMGUI_CHECKVERSION();
    s_imguiContext = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Stop the ImGui SDL backend from managing the OS
    // cursor (it calls SDL_ShowCursor/SDL_SetCursor every frame over its
    // windows). MC2 hides the OS cursor (SDL_ShowCursor(SDL_DISABLE)) and draws
    // its own in-game cursor sprite; without this flag the ImGui menu re-shows
    // the Windows arrow on top of the in-game cursor.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    // The SDL backend has its own capture/warp path, separate from GameOS.
    if (gos_NoUserPresentRequested()) io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
    // Menus request an ImGui software pointer each frame so it composites
    // above their opaque UI through Vulkan. Gameplay retains the MC2 sprite.
    //
    // Docking (vendored ImGui v1.91.8-docking): floating editor panels ->
    // real dockable layout, default-docked into a right-side column on first run (see
    // BuildEditorDockspace). Layout persists via imgui.ini. Opt out: MC2_EDITOR_DOCK=0.
    // EDITOR ONLY: docking is an editor affordance. In the GAME (s_editorMode
    // false) we must NOT enable docking -- a docked window cannot be dragged out
    // and the central dockspace is editor UX. Game ImGui windows float.
    if (s_editorMode) {
        if (mc2::gate::get<"MC2_EDITOR_DOCK">())
            io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    }

    // Auto-dock: make the CODE-defined layout authoritative. With a saved imgui.ini,
    // a window's stale per-window float state (Pos + DockId=0) can survive the
    // DockBuilder rebuild and keep that panel floating (the "Tools floats" symptom).
    // Disabling ini persistence under autodock means every launch starts from the
    // clean built layout with ALL panels docked. (MC2_EDITOR_AUTODOCK=0 keeps the
    // saved layout.) In-session re-docks still work; they just don't persist.
    if (s_editorMode) {
        if (mc2::gate::get<"MC2_EDITOR_AUTODOCK">())
            io.IniFilename = nullptr;
    }

    loadUiFonts(io);
    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForVulkan(graphics::getSDLWindow());
    io.Fonts->Build();

    g_imguiInitialized = true;
    s_useSDL2Backend = true;
}

void GuiRuntime::Shutdown() {
    if (!g_imguiInitialized) return;
#ifdef MC2_VULKAN
    // VK-IMGUI: shut the Vulkan ImGui renderer down first when it owns the frame.
    mc2vk::getOrCreateEnginePresentVk().shutdownImguiVulkanRenderer();
#endif
    g_imguiInitialized = false;
    s_frameActive = false;
    // Pin OUR context first: a second ImGui owner in the same process (the
    // standalone Viewer) shares ImGui's single global "current context" and may
    // have left a foreign context current -- or already destroyed it, leaving
    // NULL. Running the backend shutdown on a NULL/foreign context is what
    // crashed on exit (ImGui_ImplOpenGL3_Shutdown -> DestroyPlatformWindows).
    if (s_imguiContext)
        ImGui::SetCurrentContext(s_imguiContext);
    if (ImGui::GetCurrentContext() == nullptr) {
        s_imguiContext = nullptr;
        s_uiFonts.clear();
        s_useSDL2Backend = false;
        return;
    }
    // ImGui backend shutdown accesses internal context state (Viewports) that can
    // be corrupted by heap overwrites in the game's TGL/texture teardown paths.
    // On a process-exit path leaking GL objects is harmless; guard with SEH so
    // the crash doesn't surface to the user as a crash-report dialog.
#if defined(_WIN32)
    __try {
#endif
        if (s_useSDL2Backend)
            ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
#if defined(_WIN32)
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
    s_imguiContext = nullptr;
    s_uiFonts.clear();
    s_useSDL2Backend = false;
}

// --- Dockspace -------------------------------------------------------------------
bool GuiRuntime::AutoDockActive() {
    if (!s_editorMode) return false;   // GAME: nothing auto-docks
    static int s = -1;
    if (s < 0) {
        s = mc2::gate::get<"MC2_EDITOR_AUTODOCK">() ? 1 : 0;
    }
    return s != 0;
}







static bool dockEnabled() {
    if (!s_editorMode) return false;   // GAME: no editor dockspace -> windows float
    return mc2::gate::get<"MC2_EDITOR_DOCK">();
}

// Panels overlay the full native scene; the central node passes input through.
static void BuildEditorDockspace() {
    if (!dockEnabled()) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGuiID dockId = ImGui::DockSpaceOverViewport(0, vp, ImGuiDockNodeFlags_PassthruCentralNode);

    // Auto-dock: every launch, force ALL editor panels into a FIXED-width right column
    // so nothing floats. The column width equals the scene's reserved panel strip
    // (full window - fixed scene width) so the panels sit exactly beside the map, not
    // over it. Re-docks on every process start (the user wants a clean docked layout,
    // not scattered floaters); in-session re-docks still work until next launch.
    // MC2_EDITOR_AUTODOCK=0 disables (saved imgui.ini layout wins).
    static int s_autodock = -1;
    if (s_autodock < 0) {
        s_autodock = mc2::gate::get<"MC2_EDITOR_AUTODOCK">() ? 1 : 0;
    }
    static bool s_built = false;
    static int  s_focusToolsFrames = 0;  // >0: re-issue SetWindowFocus("Tools") after autodock build
    if (!s_built) {
        s_built = true;
        if (s_autodock) {
            // Right-column width = the reserved panel strip (vpW - fixed scene width).
            // Falls back to 0.26 of the window until the fixed rect is known (frame 1).
            float vpW = vp->Size.x > 1.0f ? vp->Size.x : 1.0f;
            float panelPx = vpW * 0.26f;
            if (panelPx < 64.0f)        panelPx = 64.0f;
            if (panelPx > vpW - 256.0f) panelPx = vpW - 256.0f;
            float ratio = panelPx / vpW;

            ImGui::DockBuilderRemoveNode(dockId);
            ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
            ImGui::DockBuilderSetNodeSize(dockId, vp->Size);
            ImGuiID rightId = 0, centerId = 0;
            ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Right, ratio, &rightId, &centerId);
            // EXACT ImGui::Begin titles (grep-verified). The HUD overlay (##statushud)
            // and the tiny "MC2 Editor" status line are intentionally left as overlays.
            static const char* kPanels[] = {
                "Tools", "Mission Tools", "Place Tool", "Map Generator", "Objects",
                "Object Inspector", "Inspector", "AI / Brain / Orders",
                "Scene Outliner", "Asset Browser",
                "Debug Overlays", "Task Monitor", "Foliage Detail", "Renderer Features",
                "Mission Save Readiness", "Graphics Options  [Ctrl+Shift+G]"
            };
            for (int i = 0; i < (int)(sizeof(kPanels) / sizeof(kPanels[0])); ++i)
                ImGui::DockBuilderDockWindow(kPanels[i], rightId);
            ImGui::DockBuilderFinish(dockId);

            // All panels share the right-column node, so they render as a tab strip.
            // ImGui's default selected-tab heuristic lands on "Debug Overlays", which
            // confuses users who expect to land on the editing Tools. Force "Tools" to
            // be the active tab on launch (only under autodock so a saved imgui.ini
            // layout with AUTODOCK=0 still wins).
            s_focusToolsFrames = 3;  // reinforce a few frames; panels Begin() after us
        }
    }

    // Apply the deferred "select Tools tab" request. SetWindowFocus(name) is a request
    // resolved when "Tools" next Begin()s (later this frame), so a single call on the
    // build frame can be lost to a panel grabbing focus during its own submission.
    // Re-issue for a few frames after the dock is built, then stop touching focus so
    // the user's tab selection sticks.
    if (s_focusToolsFrames > 0) {
        ImGui::SetWindowFocus("Tools");
        --s_focusToolsFrames;
    }


}

void GuiRuntime::NewFrame() {
    if (!g_imguiInitialized) return;
#ifdef MC2_VULKAN
    mc2vk::getOrCreateEnginePresentVk().prepareImguiVulkanFrame();
#endif
    if (s_useSDL2Backend)
        ImGui_ImplSDL2_NewFrame();

    // The foreign editor child uses physical client pixels at scale one.
    ImGui::GetIO().DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

#ifdef PLATFORM_WINDOWS
    // Win32 input polling
    // The GL child HWND uses a custom WndProc (MC2EditorGLChild) that SDL does
    // not subclass, so SDL never generates SDL_Events for mouse/keyboard input.
    // ImGui_ImplSDL2 therefore cannot feed the ImGui IO queue automatically.
    //
    // Solution: poll Win32 state here, directly before ImGui::NewFrame(), so
    // that when NewFrame runs it has accurate mouse position and can set
    // WantCaptureMouse correctly for this frame's message dispatch.
    if (s_editorMode) {
        ImGuiIO& io = ImGui::GetIO();

        // Mouse position: convert cursor screen coords to GL child HWND client coords.
        // Those client coords ARE the ImGui display coords (1:1 mapping).
        SDL_Window* sdlWin = graphics::getSDLWindow();
        if (sdlWin) {
            SDL_SysWMinfo wmInfo;
            SDL_VERSION(&wmInfo.version);
            if (SDL_GetWindowWMInfo(sdlWin, &wmInfo)) {
                HWND glHwnd = wmInfo.info.win.window;
                POINT cursorPt;
                if (::GetCursorPos(&cursorPt) && ::IsWindow(glHwnd)) {
                    ::ScreenToClient(glHwnd, &cursorPt);
                    // The window client coords from ScreenToClient are in the window's
                    // logical pixel space, but ImGui's DisplaySize is the full
                    // framebuffer (drawable) size -- which differs on HiDPI displays
                    // (e.g. 2x). Scale the cursor into DisplaySize space so ImGui
                    // hit-testing lines up with what is drawn. Without this the mouse
                    // and the highlighted widget are offset by the DPI factor.
                    // (No-op on 1x displays where client size == DisplaySize.)
                    float mx = (float)cursorPt.x;
                    float my = (float)cursorPt.y;
                    RECT cr;
                    if (::GetClientRect(glHwnd, &cr)) {
                        const int cw = cr.right - cr.left;
                        const int ch = cr.bottom - cr.top;
                        if (cw > 0 && ch > 0 && io.DisplaySize.x > 0.f && io.DisplaySize.y > 0.f) {
                            mx *= io.DisplaySize.x / (float)cw;
                            my *= io.DisplaySize.y / (float)ch;
                        }
                        static bool s_traceGuiMouse = false;
                        if (!s_traceGuiMouse) {
                            s_traceGuiMouse = true;
                            mc2::fmt::print_err("[GuiRuntime mouse] client=({:d},{:d}) cursorClient=({:d},{:d}) DisplaySize=({:.0f},{:.0f}) -> imguiMouse=({:.1f},{:.1f})\n", mc2::fmt::as_i32(cw), mc2::fmt::as_i32(ch), mc2::fmt::as_i32(cursorPt.x), mc2::fmt::as_i32(cursorPt.y), io.DisplaySize.x, io.DisplaySize.y, mx, my);
                            fflush(stderr);
                        }
                    }
                    io.AddMousePosEvent(mx, my);
                }
            }
        }

        // Mouse buttons: NOT polled here via GetAsyncKeyState.
        //
        // Polling buttons every frame sets io.MouseDown[n]=true while any button
        // is physically held, which makes ImGui's internal mouse_any_down=true and
        // therefore forces WantCaptureMouse=true for ALL clicks -- including viewport
        // clicks that must reach the GameOS input system for inspector picks etc.
        //
        // Button injection is handled selectively in ForwardMouseToEditor:
        // only when WantCaptureMouse is already true (mouse hovering an ImGui window)
        // are events injected and the WM_BUTTON* message consumed.  Viewport clicks
        // fall through to the MFC/GameOS path unchanged.

        // Modifier keys: inject per-frame from Win32.
        // ImGui deduplicates state-unchanged key events, so unconditional calls
        // per frame are fine and match what the official SDL/Win32 backends do.
        io.AddKeyEvent(ImGuiMod_Ctrl,  (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (::GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0);
        io.AddKeyEvent(ImGuiMod_Alt,   (::GetAsyncKeyState(VK_MENU)    & 0x8000) != 0);

        // Individual keys (edge-detect):
        // F  -> demo window toggle (via ImGui::IsKeyPressed in Render())
        // G  -> Ctrl+Shift+G opens Graphics Options (ImGui shortcut)
        static bool s_F = false, s_G = false;
        bool F_cur = (::GetAsyncKeyState('F') & 0x8000) != 0;
        bool G_cur = (::GetAsyncKeyState('G') & 0x8000) != 0;
        if (F_cur != s_F) { io.AddKeyEvent(ImGuiKey_F, F_cur); s_F = F_cur; }
        if (G_cur != s_G) { io.AddKeyEvent(ImGuiKey_G, G_cur); s_G = G_cur; }
    }
#endif

    ImGui::GetIO().MouseDrawCursor = false;
    ImGui::NewFrame();
    s_frameActive = true;
    DebugRenderer::drawFrozenPrimsToOverlay();

    // Editor dockspace (right-side panels) + scene-viewport-size feedback. Submitted
    // first so the panel windows dock into it this frame.
    BuildEditorDockspace();

    // Tell the game input layer whether ImGui has claimed the mouse this frame.
    input::setImguiWantsMouse(ImGui::GetIO().WantCaptureMouse);
    // Wheel guard: spatial hover only (NOT WantCaptureMouse, which is tainted
    // by mouse_any_down during viewport drags) — cursor over any ImGui window
    // means the wheel scrolls ImGui, not the game camera. See gos_input.h.
    input::setImguiHoversMouse(ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow));
}

// The Dear ImGui demo window is a developer affordance. It must NOT be one
// keypress away in the shipped game (a player hit 'F', opened it, and -- with
// docking on -- could not dismiss it). gui_runtime is a static lib built ONCE
// and linked into both the game and the editor, and MC2_IS_EDITOR is only
// defined on the editor exe target, so a compile macro cannot distinguish the
// two here. Gate the demo behind a runtime env, DEFAULT OFF (opt in with
// MC2_IMGUI_DEMO=1) -- unlike the other MC2_IMGUI* gates which default ON.
static bool demoEnabled() {
    static int cached = -1;
    if (cached < 0) {
        cached = mc2::gate::get<"MC2_IMGUI_DEMO">();
    }
    return cached == 1;
}

// Live cliff mesh-decal placement panel. Renders only when a
// CLIFF_WALL decal was captured this mission (a MarbleCliff was placed). Dragging a
// slider recomputes the decal's face-frame transform and re-uploads it to the
// static-prop registry the same frame — no relaunch. No decal captured => draws nothing.
static void DrawCliffDecalPanel() {
    if (!CliffDecalTuning::cliffDecal_hasDecal()) return;

    if (!ImGui::Begin("Cliff Decal")) { ImGui::End(); return; }

    float scale, offset, lateral, lift, yaw, pitch;
    CliffDecalTuning::cliffDecal_getKnobs(&scale, &offset, &lateral, &lift, &yaw, &pitch);

    ImGui::TextUnformatted("Live CLIFF_WALL mesh-decal placement.");
    ImGui::Separator();

    bool changed = false;
    changed |= ImGui::SliderFloat("Scale",   &scale,    0.5f,   5.0f, "%.3f");
    changed |= ImGui::SliderFloat("Offset",  &offset, -50.0f, 400.0f, "%.1f");
    changed |= ImGui::SliderFloat("Lateral", &lateral,-200.0f,200.0f, "%.1f");
    changed |= ImGui::SliderFloat("Lift",    &lift,   -200.0f,200.0f, "%.1f");
    changed |= ImGui::SliderFloat("Yaw",     &yaw,    -180.0f,180.0f, "%.1f");
    changed |= ImGui::SliderFloat("Pitch",   &pitch,   -90.0f, 90.0f, "%.1f");

    if (changed)
        CliffDecalTuning::cliffDecal_setKnobsAndApply(scale, offset, lateral, lift, yaw, pitch);

    // Shadow-side ambient/fill floor. Drives the batcher's
    // g_terrainDecalFill global, which is uploaded to u_terrainDecalFill every
    // frame and applied per-fragment ONLY to the flag-tagged cliff decal. No
    // transform re-apply needed — just write the value live. 0.0 => shadow side
    // goes black (raw N-L); ~0.20 => dark rock; higher washes the shadow side.
    {
        extern float g_terrainDecalFill;  // gos_static_prop_batcher.cpp
        ImGui::SliderFloat("Fill (shadow side)", &g_terrainDecalFill, 0.0f, 1.0f, "%.3f");
    }

    // RVT-style terrain-colormap blend. Drives the
    // batcher's g_terrainDecalColorBlend global (uploaded to
    // u_terrainDecalColorBlend), applied per-fragment ONLY to the flag-tagged
    // cliff decal: mixes the decal albedo toward the terrain colormap sampled at
    // the decal's world position so the cliff matches its surroundings. 0.0 =>
    // raw marble (byte-identical OFF); higher => more terrain-tinted.
    {
        extern float g_terrainDecalColorBlend;  // gos_static_prop_batcher.cpp
        ImGui::SliderFloat("Color blend (terrain)", &g_terrainDecalColorBlend, 0.0f, 1.0f, "%.3f");
    }

    ImGui::Separator();
    if (ImGui::Button("Copy current values (-> stderr)"))
        CliffDecalTuning::cliffDecal_logValues();
    ImGui::SameLine();
    ImGui::TextDisabled("freeze into CliffDecalTuning::kDefault*");

    ImGui::End();
}

static void maybeTraceVkImguiDecline() {
    static bool s_tracedInitial = false;
    static bool s_tracedArmed = false;

    int directPresentArmed = 0;
#ifdef MC2_VULKAN
    directPresentArmed = mc2vk::getOrCreateEnginePresentVk().directPresentFrameArmed() ? 1 : 0;
#endif
    if (directPresentArmed) {
        if (s_tracedArmed) return;
        s_tracedArmed = true;
    } else {
        if (s_tracedInitial) return;
        s_tracedInitial = true;
    }
    mc2::fmt::print_err("[VK_IMGUI] requested=1 used_vulkan=0 direct_present_armed={:d} fallback_reason=\"vulkan_renderer_not_ready\"\n", mc2::fmt::as_i32(directPresentArmed));
    std::fflush(stderr);
}

void GuiRuntime::Render() {
    if (!g_imguiInitialized || !s_frameActive) return;

    // F toggles the ImGui demo window (dev only -- see demoEnabled()).
    if (demoEnabled()) {
        static bool s_showDemo = false;
        if (ImGui::IsKeyPressed(ImGuiKey_F, /*repeat=*/false))
            s_showDemo = !s_showDemo;
        if (s_showDemo)
            ImGui::ShowDemoWindow(&s_showDemo);
    }

    EditorInspector::drawImGui();
    GraphicsOptionsWindow::draw();
    DrawCliffDecalPanel();

    // When the front-end 16:9
    // canvas is active and smaller than the display, paint the flank pads
    // opaque black in the foreground layer — appended here they sit on top of
    // everything drawn this frame (legacy HUD composites before ImGui; page
    // content never legitimately draws in the pads), so no layer can bleed
    // metal/backdrop into the flanks. Post-ImGui callbacks (cursor) still
    // draw above. Guarantees the "black flanks" contract at any layer's
    // expense instead of per-screen whack-a-mole.
    {
        int bx = 0, by = 0, bw = 0, bh = 0;
        float dw = 0.0f, dh = 0.0f;
        if (GetDisplaySize(dw, dh) &&
            gos_ComputeUiCanvasBox((int)dw, (int)dh, &bx, &by, &bw, &bh))
        {
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            const ImU32 black = IM_COL32(0, 0, 0, 255);
            if (bx > 0) {
                fg->AddRectFilled(ImVec2(0, 0), ImVec2((float)bx, dh), black);
                fg->AddRectFilled(ImVec2((float)(bx + bw), 0), ImVec2(dw, dh), black);
            }
            if (by > 0) {
                fg->AddRectFilled(ImVec2(0, 0), ImVec2(dw, (float)by), black);
                fg->AddRectFilled(ImVec2(0, (float)(by + bh)), ImVec2(dw, dh), black);
            }
        }
    }

    // Force scale=1 at Render() time too -- some paths between NewFrame and
    // Render can reset it (SDL window event processing, etc.).
    ImGui::GetIO().DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    // Vulkan cannot composite the legacy post-ImGui GL cursor callback.
    // Draw above opaque menu windows through the same renderer as the UI.
    ImGui::GetIO().MouseDrawCursor = (ImGui::GetIO().MouseDrawCursor || ImGui::GetIO().WantCaptureMouse)
        && !mc2::gate::get<"MC2_OS_CURSOR">();
    if (ImGui::GetIO().MouseDrawCursor && gos_NoUserPresentRequested()
        && mc2::gate::get<"MC2_VK_PICK_PROBE">()) {
        // Synthetic in-process cursor for unattended menu captures; no OS input.
        auto& io = ImGui::GetIO();
        io.MousePos = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        static bool traced = false;
        if (!traced) {
            traced = true;
            mc2::fmt::print_err("[MENU_CURSOR_PROBE] software=1 pos={:.0f},{:.0f}\n", io.MousePos.x, io.MousePos.y);
        }
    }
    ImGui::Render();
    s_frameActive = false;

    // The Vulkan ImGui renderer draws the frame; when it declines the frame is dropped (and traced once).
#ifdef MC2_VULKAN
    if (mc2vk::getOrCreateEnginePresentVk().renderImguiVulkanDrawData(ImGui::GetDrawData())) {
        static bool s_tracedVkUsed = false;
        if (!s_tracedVkUsed) {
            s_tracedVkUsed = true;
            mc2::fmt::print_err("[VK_IMGUI] requested=1 used_vulkan=1 fallback_reason=\"\"\n");
            std::fflush(stderr);
        }
    } else {
        maybeTraceVkImguiDecline();
    }
#endif
    // The post-ImGui callbacks (cursor re-draw) are GL draws into a buffer the Vulkan present never shows; drop them so the list cannot
    // grow one entry per frame (they were never run under Vulkan either).
    s_postRenderFns.clear();
}

void GuiRuntime::RegisterPostImGuiRender(std::function<void()> fn)
{
    s_postRenderFns.push_back(std::move(fn));
}
