

#include "GuiRuntime_internal.h"
#include "mc2fmt.h"
#include "../GameOS/gameos/debug_renderer.h"


// TERRAIN-DECAL-SLICE-0C — live cliff mesh-decal tuning panel.
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
bool s_useSDL2Backend = false;   // false when initialised via InitEditorOpenGLOnly
std::unordered_map<std::string, ImFont*> s_uiFonts;
static bool s_openGlRendererInitialized = false;

static bool isEnabled() {
    static int cached = -1;
    if (cached < 0) {
        cached = mc2::gate::get<"MC2_IMGUI">();
    }
    return cached == 1;
}

static bool vkImguiRequested() {
    // Both inputs are fixed once the engine is up (the gate is cached, the backend is immutable).
    static int cachedGate = -1;
    if (cachedGate < 0) {
#ifdef MC2_VULKAN
        cachedGate = mc2::gate::get<"MC2_VK_IMGUI">() ? 1 : 0;
#else
        cachedGate = 0;
#endif
    }
#ifdef MC2_VULKAN
    return cachedGate == 1;
#else
    return false;
#endif
}

static bool traceEnabled()
{
    return mc2::gate::get<"MC2_UI_DEFS_TRACE">();
}

static const char* fileForFamily(const std::string& family)
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

static std::filesystem::path resolveFontFile(const char* filename)
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

    return std::filesystem::path();
}

static void loadFontFamily(ImGuiIO& io, const char* familyName, const char* filename)
{
    const std::filesystem::path fontPath = resolveFontFile(filename);
    if (fontPath.empty()) {
        if (traceEnabled())
            mc2::fmt::print_err("[IMGUI_FONT] missing {} for family={}; using ImGui default\n", mc2::fmt::safe(filename), mc2::fmt::safe(familyName));
        return;
    }

    const int sizes[] = {8, 10, 12, 14, 16, 17, 18, 20, 22, 24, 28, 32, 36, 48};
    for (int size : sizes) {
        ImFont* font = io.Fonts->AddFontFromFileTTF(fontPath.string().c_str(), static_cast<float>(size));
        if (font)
            s_uiFonts[fontKey(familyName, size)] = font;
    }

    if (traceEnabled())
        mc2::fmt::print_err("[IMGUI_FONT] loaded family={} file={}\n", mc2::fmt::safe(familyName), fontPath.string().c_str());
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
    // LOAD-TRANSITION-DISPLAY-TRUTH-1 (queue #4 loading-flash class): prefer
    // Environment.drawableWidth/Height — the values gos updates SYNCHRONOUSLY
    // on every resolution/window change (gos_SetScreenMode handleEvents,
    // FREE-RESIZE-1 SIZE_CHANGED). io.DisplaySize is fed FROM these via
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

void GuiRuntime::InitEditorOpenGLOnly()
{
    // MERGE-CONFLICT-UI-PHASE1: base had no InitEditorOpenGLOnly(); this is a
    // theirs-only addition (editor WGL path, no SDL GL context). Ours' Init()
    // separately grew editor-mode/dockspace bootstrap that this entry point does
    // NOT run (no SetEditorMode-driven docking flags are set here, no
    // s_editorMode-gated ConfigFlags). If EditRel.exe (GPU-only editor target)
    // ever calls this instead of GuiRuntime::Init(), the dockspace/RTT/viewport
    // machinery added by our in-house tooling (BuildEditorDockspace, DockingEnable,
    // AutoDock) will silently NOT run for that entry point. Currently unclear
    // whether EditRel.exe calls Init() or InitEditorOpenGLOnly() -- verify before
    // relying on docking in that target. Left as-is (additive, theirs' code
    // untouched) since no caller of InitEditorOpenGLOnly() was found repo-wide
    // at merge time; flagging so it isn't silently assumed equivalent to Init().
    if (!isEnabled())
        return;
    if (g_imguiInitialized)
        return;

    IMGUI_CHECKVERSION();
    s_imguiContext = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    loadUiFonts(io);
    ImGui::StyleColorsDark();

    // The editor renders via WGL (SDL_CreateWindowFrom path) — there is no
    // SDL_GLContext handle, so ImGui_ImplSDL2_InitForOpenGL cannot be used.
    // Use the OpenGL3 backend alone; keyboard/mouse state is delivered via
    // SDL_PushEvent in GameOSWinProc so ImGui still receives input events
    // through the normal ImGui_ImplOpenGL3_NewFrame + io.MousePos path.
    ImGui_ImplOpenGL3_Init("#version 430");
    s_openGlRendererInitialized = true;   // MERGE-WAVE-1: keep GL shutdown/frame paths armed for the editor entry point

    g_imguiInitialized = true;
}

void GuiRuntime::Init() {
    if (!isEnabled()) return;
    if (g_imguiInitialized) return;

    IMGUI_CHECKVERSION();
    s_imguiContext = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // HW-CURSOR-SUPPRESS-1: stop the ImGui SDL backend from managing the OS
    // cursor (it calls SDL_ShowCursor/SDL_SetCursor every frame over its
    // windows). MC2 hides the OS cursor (SDL_ShowCursor(SDL_DISABLE)) and draws
    // its own in-game cursor sprite; without this flag the ImGui menu re-shows
    // the Windows arrow on top of the in-game cursor.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    // The MC2R cursor sprite is drawn in the HUD batch that flushes BEFORE
    // GuiRuntime::Render(), so it was hidden behind the new defs/ImGui UI
    // pages (user-confirmed: "cursor draws behind screens, can't see what I
    // click"). First attempt was io.MouseDrawCursor = true here -- WRONG: that
    // draws ImGui's own generic software-arrow cursor on top, not MC2's custom
    // cursor sprite (user-confirmed via screenshot: got the OS-style arrow
    // instead of the custom in-game cursor). Reverted. Real fix is
    // CURSOR-ON-TOP-OF-IMGUI-1 in mechcmd2.cpp: the actual MC2 cursor draw
    // (userInput->render()) is re-issued as a GuiRuntime::RegisterPostImGuiRender
    // callback right after its normal HUD-batch draw, so the SAME custom
    // sprite composites again on top of the ImGui frame instead of being
    // replaced by a different cursor here.
    //
    // Docking (vendored ImGui swapped to v1.91.8-docking): floating editor panels ->
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

    if (vkImguiRequested()) {
        ImGui_ImplSDL2_InitForVulkan(graphics::getSDLWindow());
        io.Fonts->Build();
    } else {
        ImGui_ImplSDL2_InitForOpenGL(graphics::getSDLWindow(), graphics::getSDLGLContext());
        ImGui_ImplOpenGL3_Init("#version 430");
        s_openGlRendererInitialized = true;
    }

    g_imguiInitialized = true;
    s_useSDL2Backend = true;
}

void GuiRuntime::Shutdown() {
    if (!g_imguiInitialized) return;
#ifdef MC2_VULKAN
    // VK-IMGUI: shut the Vulkan ImGui renderer down first when it owns the frame.
    if (vkImguiRequested()) {
        mc2vk::getOrCreateEnginePresentVk().shutdownImguiVulkanRenderer();
    }
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
        if (s_openGlRendererInitialized) {
            ImGui_ImplOpenGL3_Shutdown();
            s_openGlRendererInitialized = false;
        }
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
static int s_sceneViewW = 0;
static int s_sceneViewH = 0;

int GuiRuntime::SceneViewportWidth()  { return s_sceneViewW; }
int GuiRuntime::SceneViewportHeight() { return s_sceneViewH; }

// --- Render-to-texture viewport ---------------------------------------------------
static unsigned int s_rttTex = 0;          // GLuint of the editor present texture
static int s_vpX = 0, s_vpY = 0;           // scene screen rect (client/DisplaySize space)
static int s_vpW = 0, s_vpH = 0;
static int s_fixedX = 0, s_fixedY = 0;     // fixed-layout rect from EditorGameOS
static int s_fixedW = 0, s_fixedH = 0;     // (w>0 => override the dynamic central node)

bool GuiRuntime::RttEnabled() {
    // DEFAULT OFF (2026-06-10): the RTT scene-shrink reproduces a position-dependent
    // pick error (objects parallax + picking drifts toward screen edges) because the
    // shrunk scene projection diverges from the legacy object/pick projection. The
    // full-window scene (RTT off) picks PERFECTLY (user-bisected). Panels overlay the
    // map's right edge and are collapsible instead. Opt back in: MC2_EDITOR_RTT=1.
    // The RTT viewport blits the GL back buffer into a GL texture; under Vulkan the scene lives in the present
    // image, so the full-window scene is the only mode there.
#ifdef MC2_VULKAN
    return false;
#else
    static int s_on = -1;
    if (s_on < 0) {
        s_on = mc2::gate::get<"MC2_EDITOR_RTT">();   // default OFF
    }
    return s_on != 0;
#endif
}
bool GuiRuntime::AutoDockActive() {
    if (!s_editorMode) return false;   // GAME: nothing auto-docks
    static int s = -1;
    if (s < 0) {
        s = mc2::gate::get<"MC2_EDITOR_AUTODOCK">() ? 1 : 0;
    }
    return s != 0;
}
void GuiRuntime::SetViewportTexture(unsigned int glTex) { s_rttTex = glTex; }
void GuiRuntime::SetFixedViewportRect(int x, int y, int w, int h) {
    s_fixedX = x; s_fixedY = y; s_fixedW = w; s_fixedH = h;
}
int  GuiRuntime::ViewportRectX() { return s_vpX; }
int  GuiRuntime::ViewportRectY() { return s_vpY; }
int  GuiRuntime::ViewportRectW() { return s_vpW; }
int  GuiRuntime::ViewportRectH() { return s_vpH; }

static bool dockEnabled() {
    if (!s_editorMode) return false;   // GAME: no editor dockspace -> windows float
    return mc2::gate::get<"MC2_EDITOR_DOCK">();
}

// Build (first run) + drive the editor dockspace each frame. PassthruCentralNode keeps
// the central region transparent + mouse-passthrough so the GL scene + picking work
// there; panels default to a right-side column. Records the central node size so
// EditorGameOS shrinks the scene viewport to it (one-frame lag, harmless), making the
// map fill exactly the un-docked region. Layout (incl. user re-docks) persists in imgui.ini.
static void BuildEditorDockspace() {
    if (!dockEnabled()) { s_sceneViewW = s_sceneViewH = 0; return; }

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
            float panelPx = (s_fixedW > 0) ? (vpW - (float)s_fixedW) : (vpW * 0.26f);
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

    if (ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockId)) {
        s_sceneViewW = (int)central->Size.x;
        s_sceneViewH = (int)central->Size.y;

        // RTT: paint the editor scene texture into the central node via the
        // BACKGROUND draw list. No ImGui window -> WantCaptureMouse stays false
        // over the scene, so picking/camera input still reach the GameOS path
        // (identical to the PassthruCentralNode behavior). The texture is filled
        // by EditorGameOS (composite -> blit) before GuiRuntime::Render samples it.
        // Fixed-layout rect (from EditorGameOS) overrides the dynamic central node so
        // the scene rect, the camera viewport, and the pick offset are ONE known value.
        const bool useFixed = (s_fixedW > 0 && s_fixedH > 0);
        s_vpX = useFixed ? s_fixedX : (int)central->Pos.x;
        s_vpY = useFixed ? s_fixedY : (int)central->Pos.y;
        s_vpW = useFixed ? s_fixedW : (int)central->Size.x;
        s_vpH = useFixed ? s_fixedH : (int)central->Size.y;
        if (GuiRuntime::RttEnabled() && s_rttTex != 0) {
            const ImVec2 p0 = ImVec2((float)s_vpX, (float)s_vpY);
            const ImVec2 p1 = ImVec2((float)(s_vpX + s_vpW),
                                     (float)(s_vpY + s_vpH));
            // GL texture origin is bottom-left; flip V so the scene is upright.
            ImGui::GetBackgroundDrawList()->AddImage(
                (ImTextureID)(intptr_t)s_rttTex, p0, p1,
                ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
        }
    }
}

void GuiRuntime::NewFrame() {
    if (!g_imguiInitialized) return;
    if (!s_openGlRendererInitialized && !vkImguiRequested()) {
        // Init chose the Vulkan renderer, then the session fell back to GL: bring the GL renderer up now.
        ImGui_ImplOpenGL3_Init("#version 430");
        s_openGlRendererInitialized = true;
    }
    if (s_openGlRendererInitialized) {
        ImGui_ImplOpenGL3_NewFrame();
    } else if (vkImguiRequested()) {
#ifdef MC2_VULKAN
        mc2vk::getOrCreateEnginePresentVk().prepareImguiVulkanFrame();
#endif
    }
    if (s_useSDL2Backend)
        ImGui_ImplSDL2_NewFrame();

    // SDL_GL_GetDrawableSize returns 0 for SDL_CreateWindowFrom windows that
    // have no SDL GL context (editor uses WGL, not SDL_GL_CreateContext).
    // ImGui_ImplSDL2_NewFrame sets io.DisplayFramebufferScale = drawable/window,
    // which becomes (0,0) -> ImGui_ImplOpenGL3_RenderDrawData computes fb_width=0
    // and early-returns without issuing any draw calls.  Force scale=1 to fix.
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
    {
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

// TERRAIN-DECAL-SLICE-0C: live cliff mesh-decal placement panel. Renders only when a
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

    // TERRAIN-DECAL-FILL-1: shadow-side ambient/fill floor. Drives the batcher's
    // g_terrainDecalFill global, which is uploaded to u_terrainDecalFill every
    // frame and applied per-fragment ONLY to the flag-tagged cliff decal. No
    // transform re-apply needed — just write the value live. 0.0 => shadow side
    // goes black (raw N-L); ~0.20 => dark rock; higher washes the shadow side.
    {
        extern float g_terrainDecalFill;  // gos_static_prop_batcher.cpp
        ImGui::SliderFloat("Fill (shadow side)", &g_terrainDecalFill, 0.0f, 1.0f, "%.3f");
    }

    // TERRAIN-DECAL-COLORBLEND-1: RVT-style terrain-colormap blend. Drives the
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
    if (!vkImguiRequested()) return;
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

    // CANVAS-FLANK-CLEAR-1 (UI-LAYER-CONTRACT slice): when the front-end 16:9
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

    ImGui::Render();
    s_frameActive = false;

    // One-shot diagnostic: dump ImDrawData stats and GL state to stderr.
    // editor-stderr.log captures this via run-editor.bat's 2> redirect.
    if (s_openGlRendererInitialized) {
        static bool s_traced = false;
        if (!s_traced) {
            s_traced = true;
            ImDrawData* dd = ImGui::GetDrawData();
            GLint boundFbo = -1, boundProg = -1;
            GLint vp[4] = {};
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &boundFbo);
            glGetIntegerv(GL_CURRENT_PROGRAM, &boundProg);
            glGetIntegerv(GL_VIEWPORT, vp);
            GLenum err = glGetError();
            if (dd) {
                mc2::fmt::print_err("[GuiRuntime::Render] frame0 CmdLists={:d} TotalVtx={:d} TotalIdx={:d} DisplaySize=({:.0f},{:.0f}) FbScale=({:.3f},{:.3f}) fbo={:d} prog={:d} vp=({:d},{:d},{:d},{:d}) glErr=0x{:X}\n", mc2::fmt::as_i32(dd->CmdListsCount), mc2::fmt::as_i32(dd->TotalVtxCount), mc2::fmt::as_i32(dd->TotalIdxCount), dd->DisplaySize.x, dd->DisplaySize.y, dd->FramebufferScale.x, dd->FramebufferScale.y, mc2::fmt::as_i32(boundFbo), mc2::fmt::as_i32(boundProg), mc2::fmt::as_i32(vp[0]), mc2::fmt::as_i32(vp[1]), mc2::fmt::as_i32(vp[2]), mc2::fmt::as_i32(vp[3]), mc2::fmt::as_u32((unsigned)err));
            } else {
                mc2::fmt::print_err("[GuiRuntime::Render] frame0 GetDrawData() returned NULL\n");
            }
            fflush(stderr);
        }
    }

    if (!s_openGlRendererInitialized && vkImguiRequested()) {
#ifdef MC2_VULKAN
        if (mc2vk::getOrCreateEnginePresentVk().renderImguiVulkanDrawData(ImGui::GetDrawData())) {
            static bool s_tracedVkUsed = false;
            if (!s_tracedVkUsed) {
                s_tracedVkUsed = true;
                mc2::fmt::print_err("[VK_IMGUI] requested=1 used_vulkan=1 fallback_reason=\"\"\n");
                std::fflush(stderr);
            }
            return;
        }
#endif
        maybeTraceVkImguiDecline();
        return;
    }

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    {
        static bool s_errTraced = false;
        if (!s_errTraced) {
            s_errTraced = true;
            GLenum err = glGetError();
            if (err != GL_NO_ERROR)
                mc2::fmt::print_err("[GuiRuntime::Render] glError after RenderDrawData: 0x{:X}\n", mc2::fmt::as_u32((unsigned)err));
            else
                mc2::fmt::print_err("[GuiRuntime::Render] RenderDrawData completed, no GL error\n");
            fflush(stderr);
        }
    }

    // Pixel probe (fires on 3rd Render call so terrain is in the buffer)
    {
        static int s_renderCallCount = 0;
        ++s_renderCallCount;
        if (s_renderCallCount == 3) {
            // Backend name: non-null only if ImGui_ImplOpenGL3_Init() succeeded
            const char* backendName = ImGui::GetIO().BackendRendererName;
            // Font texture handle: 0 if CreateDeviceObjects never ran
            ImTextureID fontTexID = ImGui::GetIO().Fonts->TexID;

            // Sample a pixel inside the "MC2 Editor" window title bar.
            // Window Pos=(10,10), Size=(220,60). Title bar centre ~ ImGui(120,19).
            // OpenGL origin is bottom-left, so gl_y = fb_height - 1 - imgui_y.
            ImDrawData* dd = ImGui::GetDrawData();
            int fbh = dd ? (int)(dd->DisplaySize.y * dd->FramebufferScale.y) : 0;
            int fbw = dd ? (int)(dd->DisplaySize.x * dd->FramebufferScale.x) : 0;
            GLubyte imguiPx[4] = {};
            GLubyte terrainPx[4] = {};
            if (fbh > 0 && fbw > 0) {
                int gl_y_imgui  = fbh - 1 - 19;   // ImGui y=19 (title bar)
                int gl_y_terrain = fbh / 2;         // screen centre
                glReadPixels(120,       gl_y_imgui,   1, 1, GL_RGBA, GL_UNSIGNED_BYTE, imguiPx);
                glReadPixels(fbw / 2,   gl_y_terrain, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, terrainPx);
            }

            mc2::fmt::print_err("[GuiRuntime PROBE] BackendRendererName={} FontTexID={:016X} imgui_px(120,19)=RGBA({:d},{:d},{:d},{:d}) terrain_mid_px=RGBA({:d},{:d},{:d},{:d})\n", mc2::fmt::safe(backendName ? backendName : "NULL"), mc2::fmt::as_ptr((void*)(uintptr_t)fontTexID), mc2::fmt::as_i32(imguiPx[0]), mc2::fmt::as_i32(imguiPx[1]), mc2::fmt::as_i32(imguiPx[2]), mc2::fmt::as_i32(imguiPx[3]), mc2::fmt::as_i32(terrainPx[0]), mc2::fmt::as_i32(terrainPx[1]), mc2::fmt::as_i32(terrainPx[2]), mc2::fmt::as_i32(terrainPx[3]));
            fflush(stderr);
        }
    }

    // MERGE-CONFLICT-UI-PHASE1: theirs' Render() called gos_InvalidateRenderStateCache()
    // + gos_PrepareForPostImGuiRender() immediately after ImGui_ImplOpenGL3_RenderDrawData(),
    // because ImGui_ImplOpenGL3_RenderDrawData() leaves GL_DEPTH_TEST disabled and
    // glDepthMask(GL_FALSE), which without invalidation makes GOS's render-state cache
    // skip a full re-apply, and post-ImGui 3D camera renders (RegisterPostImGuiRender
    // callbacks below) come out invisible. Ours' Render() has ADDITIONAL diagnostic
    // glReadPixels/glGetIntegerv probes inserted between RenderDrawData and here (see
    // above) that were NOT present when theirs' invalidation call was written; those
    // probes read GL_FRAMEBUFFER_BINDING/GL_CURRENT_PROGRAM/GL_VIEWPORT and do NOT
    // themselves change render state, so ordering vs. the probes should be harmless,
    // but this was never verified against theirs' new UI Editor FIT 3D-camera-in-panel
    // use case. Keeping theirs' invalidation calls (additive, no direct clash with any
    // ours-only code) but flagging because this is the one place Render()'s tail
    // sequencing changed on both sides independently and wasn't validated together.
    gos_InvalidateRenderStateCache();
    gos_PrepareForPostImGuiRender();

    // Post-ImGui GL renders (3D cameras, direct-GL images) fire here so they
    // composite on top of the ImGui overlay rather than underneath it.
    std::vector<std::function<void()>> fns;
    fns.swap(s_postRenderFns);
    static bool s_cbDiagOnce = false;
    if (!s_cbDiagOnce && !fns.empty()) {
        s_cbDiagOnce = true;
        mc2::fmt::print_err("[3DVIEW_DIAG v2] GuiRuntime::Render firing {:d} callbacks\n", mc2::fmt::as_size(fns.size()));
    }
    for (auto& fn : fns)
        fn();
}

void GuiRuntime::RegisterPostImGuiRender(std::function<void()> fn)
{
    s_postRenderFns.push_back(std::move(fn));
}
