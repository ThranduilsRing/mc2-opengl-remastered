#include "GuiRuntime.h"
#include "EditorInspector.h"
#include "GraphicsOptionsWindow.h"
#include "imgui.h"
#include "imgui_internal.h"   // DockBuilder* + ImGuiDockNode (docking branch)
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include <cstdlib>   // getenv
#include <cstdio>    // fprintf
#include <cstring>   // strcmp
#include <cstdint>   // intptr_t (ImTextureID cast)

#include <GL/glew.h>  // glGetError, glGetIntegerv for Render() diagnostics

#include "../GameOS/gameos/gos_render.h"
#include "../GameOS/gameos/gos_input.h"
#ifdef MC2_VULKAN
#include "../GameOS/gameos/engine_present_vk.h"
#include "../GameOS/gameos/render_backend_select.h"
#endif

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#include <SDL2/SDL_syswm.h>
#endif

bool g_imguiInitialized = false;

static bool isEnabled() {
    static int cached = -1;
    if (cached < 0) {
        const char* v = std::getenv("MC2_IMGUI");
        cached = (!v || v[0] != '0') ? 1 : 0;
    }
    return cached == 1;
}

// Editor-mode flag (see header). Default false == GAME: no editor dockspace, so
// ImGui windows float. The editor sets it true before Init().
static bool s_editorMode = false;
void GuiRuntime::SetEditorMode(bool on) { s_editorMode = on; }
bool GuiRuntime::IsEditorMode() { return s_editorMode; }

void GuiRuntime::Init() {
    if (!isEnabled()) return;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // HW-CURSOR-SUPPRESS-1: stop the ImGui SDL backend from managing the OS
    // cursor (it calls SDL_ShowCursor/SDL_SetCursor every frame over its
    // windows). MC2 hides the OS cursor (SDL_ShowCursor(SDL_DISABLE)) and draws
    // its own in-game cursor sprite; without this flag the ImGui menu re-shows
    // the Windows arrow on top of the in-game cursor.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    // Docking (vendored ImGui swapped to v1.91.8-docking): floating editor panels ->
    // real dockable layout, default-docked into a right-side column on first run (see
    // BuildEditorDockspace). Layout persists via imgui.ini. Opt out: MC2_EDITOR_DOCK=0.
    // EDITOR ONLY: docking is an editor affordance. In the GAME (s_editorMode
    // false) we must NOT enable docking -- a docked window cannot be dragged out
    // and the central dockspace is editor UX. Game ImGui windows float.
    if (s_editorMode) {
        const char* d = std::getenv("MC2_EDITOR_DOCK");
        if (!d || strcmp(d, "0") != 0)
            io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    }

    // Auto-dock: make the CODE-defined layout authoritative. With a saved imgui.ini,
    // a window's stale per-window float state (Pos + DockId=0) can survive the
    // DockBuilder rebuild and keep that panel floating (the "Tools floats" symptom).
    // Disabling ini persistence under autodock means every launch starts from the
    // clean built layout with ALL panels docked. (MC2_EDITOR_AUTODOCK=0 keeps the
    // saved layout.) In-session re-docks still work; they just don't persist.
    if (s_editorMode) {
        const char* a = std::getenv("MC2_EDITOR_AUTODOCK");
        if (!a || strcmp(a, "0") != 0)
            io.IniFilename = nullptr;
    }

    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForOpenGL(graphics::getSDLWindow(), graphics::getSDLGLContext());
    ImGui_ImplOpenGL3_Init("#version 430");

    g_imguiInitialized = true;
}

void GuiRuntime::Shutdown() {
    if (!g_imguiInitialized) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    g_imguiInitialized = false;
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
    static int s_on = -1;
    if (s_on < 0) {
        const char* r = std::getenv("MC2_EDITOR_RTT");
        s_on = (r && strcmp(r, "1") == 0) ? 1 : 0;   // default OFF
    }
    return s_on != 0;
}
bool GuiRuntime::AutoDockActive() {
    if (!s_editorMode) return false;   // GAME: nothing auto-docks
    static int s = -1;
    if (s < 0) {
        const char* a = std::getenv("MC2_EDITOR_AUTODOCK");
        s = (a && strcmp(a, "0") == 0) ? 0 : 1;
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
    const char* d = std::getenv("MC2_EDITOR_DOCK");
    return (!d || strcmp(d, "0") != 0);
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
        const char* a = std::getenv("MC2_EDITOR_AUTODOCK");
        s_autodock = (a && strcmp(a, "0") == 0) ? 0 : 1;
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
    ImGui_ImplOpenGL3_NewFrame();
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
                            fprintf(stderr,
                                "[GuiRuntime mouse] client=(%d,%d) cursorClient=(%ld,%ld) "
                                "DisplaySize=(%.0f,%.0f) -> imguiMouse=(%.1f,%.1f)\n",
                                cw, ch, cursorPt.x, cursorPt.y,
                                io.DisplaySize.x, io.DisplaySize.y, mx, my);
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

    // Editor dockspace (right-side panels) + scene-viewport-size feedback. Submitted
    // first so the panel windows dock into it this frame.
    BuildEditorDockspace();

    // Tell the game input layer whether ImGui has claimed the mouse this frame.
    input::setImguiWantsMouse(ImGui::GetIO().WantCaptureMouse);
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
        const char* v = std::getenv("MC2_IMGUI_DEMO");
        cached = (v && v[0] == '1') ? 1 : 0;
    }
    return cached == 1;
}

static bool vkImguiRequested() {
    static int cached = -1;
    if (cached < 0) {
        const char* v = std::getenv("MC2_VK_IMGUI");
        cached = (v && v[0] == '1') ? 1 : 0;
    }
    return cached == 1;
}

static void maybeTraceVkImguiDecline() {
    if (!vkImguiRequested()) return;
    static bool s_traced = false;
    if (s_traced) return;
    s_traced = true;

    int directPresentArmed = 0;
#ifdef MC2_VULKAN
    if (mc2::renderBackend() == mc2::RenderBackend::Vulkan) {
        mc2vk::EnginePresentVk& vkpres = mc2vk::getOrCreateEnginePresentVk();
        directPresentArmed = vkpres.directPresentFrameArmed() ? 1 : 0;
    }
#endif
    std::fprintf(stderr,
        "[VK_IMGUI] requested=1 used_vulkan=0 direct_present_armed=%d "
        "fallback_reason=\"tail_handoff_not_split\"\n",
        directPresentArmed);
    std::fflush(stderr);
}

void GuiRuntime::Render() {
    if (!g_imguiInitialized) return;
    maybeTraceVkImguiDecline();

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

    // Force scale=1 at Render() time too -- some paths between NewFrame and
    // Render can reset it (SDL window event processing, etc.).
    ImGui::GetIO().DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    ImGui::Render();

    // One-shot diagnostic: dump ImDrawData stats and GL state to stderr.
    // editor-stderr.log captures this via run-editor.bat's 2> redirect.
    {
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
                fprintf(stderr,
                    "[GuiRuntime::Render] frame0 CmdLists=%d TotalVtx=%d TotalIdx=%d "
                    "DisplaySize=(%.0f,%.0f) FbScale=(%.3f,%.3f) "
                    "fbo=%d prog=%d vp=(%d,%d,%d,%d) glErr=0x%X\n",
                    dd->CmdListsCount, dd->TotalVtxCount, dd->TotalIdxCount,
                    dd->DisplaySize.x, dd->DisplaySize.y,
                    dd->FramebufferScale.x, dd->FramebufferScale.y,
                    boundFbo, boundProg,
                    vp[0], vp[1], vp[2], vp[3],
                    (unsigned)err);
            } else {
                fprintf(stderr, "[GuiRuntime::Render] frame0 GetDrawData() returned NULL\n");
            }
            fflush(stderr);
        }
    }

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    {
        static bool s_errTraced = false;
        if (!s_errTraced) {
            s_errTraced = true;
            GLenum err = glGetError();
            if (err != GL_NO_ERROR)
                fprintf(stderr, "[GuiRuntime::Render] glError after RenderDrawData: 0x%X\n", (unsigned)err);
            else
                fprintf(stderr, "[GuiRuntime::Render] RenderDrawData completed, no GL error\n");
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

            fprintf(stderr,
                "[GuiRuntime PROBE] BackendRendererName=%s FontTexID=%p "
                "imgui_px(120,19)=RGBA(%d,%d,%d,%d) "
                "terrain_mid_px=RGBA(%d,%d,%d,%d)\n",
                backendName ? backendName : "NULL",
                (void*)(uintptr_t)fontTexID,
                imguiPx[0], imguiPx[1], imguiPx[2], imguiPx[3],
                terrainPx[0], terrainPx[1], terrainPx[2], terrainPx[3]);
            fflush(stderr);
        }
    }
}
