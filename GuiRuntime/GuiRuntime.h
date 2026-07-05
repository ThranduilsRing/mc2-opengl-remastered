// Dear ImGui in-game integration -- wired for MC2 OpenGL port in collaboration with @methuselas.
// Dear ImGui by Omar Cornut et al. -- https://github.com/ocornut/imgui
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace GuiRuntime {
    // Editor-mode opt-in. gui_runtime is ONE static lib linked into both the game
    // and the editor exe, so it cannot tell which it is from a compile macro. The
    // editor calls SetEditorMode(true) ONCE before Init(); the game never does.
    // Editor-only ImGui behavior (the docking dockspace + auto-dock) is gated on
    // this, so in the GAME, ImGui windows (e.g. Graphics Options) FLOAT instead of
    // docking. Must be set before Init() to take effect on DockingEnable.
    void SetEditorMode(bool on);
    bool IsEditorMode();

    void Init();      // call once after GL context created
    void Shutdown();  // call once before context destroyed
    void NewFrame();  // call each frame before game/UI drawing
    void Render();    // call each frame after all game drawing, before swap

    // Editor-only init: skips ImGui_ImplSDL2 (the editor uses a WGL context
    // created via SDL_CreateWindowFrom — no SDL GL context handle exists).
    // Input events are injected manually through GameOSWinProc/SDL_PushEvent.
    void InitEditorOpenGLOnly();  // call once after WGL context is current

    // Called whenever the framebuffer is resized (e.g. gos_SetScreenMode).
    // Updates io.DisplaySize so ImGui layout and hit-testing match the actual
    // drawable area.  framebufferW/H are the drawable (physical pixel) size;
    // windowW/H are the logical SDL window size.  Pass 0/0 for window dims
    // to use the framebuffer dims (non-HiDPI / pre-existing behaviour).
    void NotifyResize(int framebufferW, int framebufferH,
                      int windowW = 0, int windowH = 0);

    // Current ImGui logical display size (io.DisplaySize).  UI Editor FIT
    // pages are authored in a local coordinate space (GuiPage localWidth/
    // localHeight, typically 800x600); the renderer scales them to this
    // size so pages fill the window at any resolution.  Returns false (and
    // zeroes) when no ImGui context/frame is available.
    bool GetDisplaySize(float& w, float& h);

    // Draw data/defs UI text using ImGui's built-in font atlas.  This is the
    // cross-platform font path for UI Editor FITs: no SDL2_ttf, no FreeType DLL,
    // and no GameOS bitmap/D3F font fallback for the new UI layer.
    bool DrawUiText(float x,
                    float y,
                    float w,
                    float h,
                    const char* text,
                    std::uint32_t argb,
                    int fontSize,
                    int align,
                    const char* fontFamily);

    bool IsReadyForUiText();

    // Draw UI Editor FIT rectangles/images through ImGui's draw lists.  The
    // new data/defs UI layer is a HUD layer; GameOS owns the frame/3D
    // renderer, but ImGui owns visible 2D UI composition and text.
    bool DrawUiRect(float x,
                    float y,
                    float w,
                    float h,
                    std::uint32_t argb,
                    bool filled);

    // Draw a filled down-pointing triangle centered in the given rect.
    // Used for combo-box drop arrow indicators.
    bool DrawUiTriangle(float x, float y, float w, float h, std::uint32_t argb);

    // Filled or outlined circle centered at (cx,cy). Radio ("circle check") glyphs.
    bool DrawUiCircle(float cx, float cy, float radius, std::uint32_t argb, bool filled);

    bool DrawUiImage(unsigned int glTextureName,
                     float x,
                     float y,
                     float w,
                     float h,
                     float u1,
                     float v1,
                     float u2,
                     float v2,
                     std::uint32_t argb,
                     bool nearest = false,
                     bool alphaTest = false);

    // Draw an ImGui InputText widget pinned to (x,y,w,h) in display space.
    // buf/bufSize: persistent caller-owned buffer that ImGui edits in place.
    // requestFocus: call SetKeyboardFocusHere this frame (consume once).
    // Returns true while the widget has keyboard focus (IsItemActive).
    bool DrawUiEditBox(const char* id,
                       float x, float y, float w, float h,
                       char* buf, std::size_t bufSize,
                       std::uint32_t fillArgb,
                       std::uint32_t borderArgb,
                       std::uint32_t textArgb,
                       int fontSize,
                       const char* fontFamily,
                       bool requestFocus);

    // Register a one-shot GL callback to run after ImGui::Render() +
    // ImGui_ImplOpenGL3_RenderDrawData(). Use for 3D cameras and direct-GL images
    // that must composite on top of the ImGui overlay rather than underneath it.
    // The callback fires once and is then discarded.
    void RegisterPostImGuiRender(std::function<void()> fn);

    // Size of the dockspace CENTRAL node (the area not covered by docked panels) in
    // pixels. The editor shrinks its scene viewport to this so the map fills only the
    // un-docked region. 0 until the dockspace is laid out (or docking off). Lags the
    // live layout by one frame (built in NewFrame, read at frame top).
    int SceneViewportWidth();
    int SceneViewportHeight();

    // Render-to-texture viewport (editor). When enabled, the editor renders the
    // scene into an offscreen texture and the dockspace paints it into the central
    // node via the background draw list (no ImGui window -> no WantCaptureMouse, so
    // picking still reaches GameOS). EditorGameOS hands the texture id here each
    // frame after compositing+blit. Picking offsets client mouse by the rect origin.
    bool RttEnabled();                              // MC2_EDITOR_RTT (default ON)
    // True when the editor force-docks all panels into the right column each launch
    // (MC2_EDITOR_AUTODOCK != 0). Panel windows must then SKIP SetNextWindowPos before
    // Begin -- an explicit next-window-pos pulls the window out of the dock and floats it.
    bool AutoDockActive();
    void SetViewportTexture(unsigned int glTex);    // GLuint; called pre-Render()
    // Fixed-layout scene rect (client/screen pixels, left-anchored). When set (w>0),
    // the dockspace paints the RTT texture into THIS rect instead of the dynamic
    // central node, and the pick offset uses it. Set each frame by EditorGameOS.
    void SetFixedViewportRect(int x, int y, int w, int h);
    // Central-node screen rect (ImGui DisplaySize space == GL-child client coords).
    int  ViewportRectX();
    int  ViewportRectY();
    int  ViewportRectW();
    int  ViewportRectH();
}

extern bool g_imguiInitialized;
