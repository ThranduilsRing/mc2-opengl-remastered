// gameos_graphics_params.cpp - HUD-scale + UI-canvas parameter state and
// accessors. GAMEOS-GRAPHICS-SPLIT-1 slice 5: moved verbatim from
// gameos_graphics.cpp; the state globals live HERE (extern-declared in
// gameos_graphics_internal.h for flushHUDBatch and the HUD record sites).
// Renderer size reached via gosRendererLogicalWidth/Height shims.

#include "gameos_graphics_internal.h"

#include <cstdlib>

float  s_hud_scale = 0.85f;  // default while iterating; RAlt+5 cycles
bool   s_hud_scale_active = false;  // gated: only shrink during mission
// When set, HUD draw calls recorded while it is on are tagged scaleExempt and
// are NOT shrunk by the bottom-band s_hud_scale transform in flushHUDBatch.
// Bracketed around the mouse cursor sprite and modal dialogs (quit prompt, etc.)
// so the HUD-fit shrink only ever moves the in-game HUD chrome -- never the
// pointer or a popup. Without this the cursor snapped from unscaled to 0.85x as
// it crossed the 60%-height centroid gate (jump-at-center + 0.9x drift) and
// dialogs straddling the gate tore at the seam.
bool   s_hud_scale_exempt = false;

// UI-ASPECT-ANCHOR-1: per-call canvas exemption. Default (-1) FOLLOWS the
// shrink-exempt bracket -- the existing exempt sites are the cursor, modal
// dialogs and world-anchored overlays (selection brackets / health bars /
// sensor rings), all of which must stay in full-surface space. A site that is
// shrink-exempt but WANTS the canvas remap (force-group bar: hit-tests via
// getMouseHudX) overrides with 0 around its draws; 1 forces exemption.
int    s_hud_canvas_exempt_mode = -1;

// UI-ASPECT-ANCHOR-1 canvas flags (defined up here, same reason as s_hud_scale:
// flushHUDBatch references them directly). See gos_SetUiCanvasActive below.
bool s_uiCanvasAssert  = false;  // set by front-end each frame
bool s_uiCanvasLatch   = false;  // last completed frame's assert
bool s_hudCanvasActive = false;  // sticky: mission HUD canvas (mission.cpp)


// ── UI-ASPECT-ANCHOR-1: 16:9 UI canvas ──────────────────────────────────────
// The legacy 800x600 UI historically stretched to the FULL window, distorting
// at non-16:9 aspects. The approved model: the UI lives on a 16:9 canvas (the
// accepted 1080p look). Wider displays center it with black flanks; narrower
// displays scale it down (letterbox). Front-end screens assert the canvas per
// frame (gos_SetUiCanvasActive from MainMenu/MissionBegin render); the flag
// auto-latches at flushHUDBatch so early-frame consumers (mouse normalize)
// use last frame's state, and MISSION frames — which never assert — fall back
// to the legacy full-surface behavior untouched (world pick unaffected).
// Killswitch: MC2_UI_ASPECT_ANCHOR=0 restores full-stretch everywhere.
void __stdcall gos_SetUiCanvasActive(bool on) { s_uiCanvasAssert = on; }
void __stdcall gos_SetHudCanvasActive(bool on) { s_hudCanvasActive = on; }
void __stdcall gos_SetHudCanvasExemptMode(int mode) { s_hud_canvas_exempt_mode = mode; }
int  __stdcall gos_GetHudCanvasExemptMode() { return s_hud_canvas_exempt_mode; }

static bool uiAspectAnchorEnabled()
{
    static const bool s_enabled =
        []{ const char* e = getenv("MC2_UI_ASPECT_ANCHOR"); return !(e && e[0] == '0'); }();
    return s_enabled;
}

// Pure rect math: centered 16:9 canvas inside w x h. Returns true when the
// canvas differs from the full surface.
static bool computeUiCanvas16x9(int w, int h, int* ox, int* oy, int* obw, int* obh)
{
    int cw = w, ch = h;
    if (w > 0 && h > 0)
    {
        cw = (h * 16) / 9;
        ch = h;
        if (cw > w) { cw = w; ch = (w * 9) / 16; }
    }
    if (ox)  *ox  = (w - cw) / 2;
    if (oy)  *oy  = (h - ch) / 2;
    if (obw) *obw = cw;
    if (obh) *obh = ch;
    return (cw != w || ch != h);
}

bool __stdcall gos_ComputeUiCanvasBox(int w, int h, int* ox, int* oy, int* obw, int* obh)
{
    const bool active = uiAspectAnchorEnabled() && (s_uiCanvasAssert || s_uiCanvasLatch);
    if (!active)
    {
        if (ox)  *ox  = 0;
        if (oy)  *oy  = 0;
        if (obw) *obw = w;
        if (obh) *obh = h;
        return false;
    }
    return computeUiCanvas16x9(w, h, ox, oy, obw, obh);
}

// In-mission variant: active while the mission HUD asserts (mission.cpp sets
// gos_SetHudCanvasActive alongside gos_SetHudScaleActive). Kept separate from
// gos_ComputeUiCanvasBox on purpose — the mouse normalize must NOT go
// canvas-relative in mission (world pick spans the full drawable); mission HUD
// hit-tests go through gos_HudInverseMousePoint instead.
bool __stdcall gos_ComputeHudCanvasBox(int w, int h, int* ox, int* oy, int* obw, int* obh)
{
    const bool active = uiAspectAnchorEnabled() && s_hudCanvasActive;
    if (!active)
    {
        if (ox)  *ox  = 0;
        if (oy)  *oy  = 0;
        if (obw) *obw = w;
        if (obh) *obh = h;
        return false;
    }
    return computeUiCanvas16x9(w, h, ox, oy, obw, obh);
}

// HUD scale — clamped to [0.5, 1.0]. 1.0 disables the transform entirely.
// s_hud_scale itself is defined near the top of this file so flushHUDBatch()
// can reference it directly without a forward declaration dance.
void gos_SetHudScale(float s) {
    if (s < 0.5f) s = 0.5f;
    if (s > 1.0f) s = 1.0f;
    s_hud_scale = s;
}
float gos_GetHudScale() { return s_hud_scale; }

void gos_SetHudScaleActive(bool on) { s_hud_scale_active = on; }
bool gos_GetHudScaleActive()        { return s_hud_scale_active; }

// HUD-scale exemption: while set, HUD draw calls recorded are tagged scaleExempt
// and skip the bottom-band shrink in flushHUDBatch. Bracket the cursor sprite
// and modal dialogs so the HUD-fit shrink never moves the pointer or a popup.
void gos_SetHudScaleExempt(bool on) { s_hud_scale_exempt = on; }
bool gos_GetHudScaleExempt()        { return s_hud_scale_exempt; }

void gos_HudInverseMousePoint(float& x, float& y) {
    // UI-ASPECT-ANCHOR-1: invert the mission HUD-canvas remap FIRST (draw order
    // is shrink -> canvas, so the inverse is canvas -> shrink). Maps the
    // full-logical mouse point back into the pre-canvas authored space the
    // band/anchor math below expects. Must stay in sync with the canvas block
    // in gosRenderer::flushHUDBatch().
    if (gosRendererLogicalWidth() > 0) {
        int bx = 0, by = 0, bw = 0, bh = 0;
        if (gos_ComputeHudCanvasBox(Environment.drawableWidth, Environment.drawableHeight,
                                    &bx, &by, &bw, &bh)) {
            const float dw = (float)Environment.drawableWidth;
            const float dh = (float)Environment.drawableHeight;
            const float fx = (float)bw / dw;
            const float fy = (float)bh / dh;
            const float lx = (float)gosRendererLogicalWidth()  * ((float)bx / dw);
            const float ly = (float)gosRendererLogicalHeight() * ((float)by / dh);
            if (fx > 0.0f) x = (x - lx) / fx;
            if (fy > 0.0f) y = (y - ly) / fy;
        }
    }
    // Inverse of the single-anchor bottom-center HUD transform. Must stay in
    // sync with gosRenderer::flushHUDBatch() above.
    const float scale = s_hud_scale;
    if (!s_hud_scale_active || scale > 0.999f || gosRendererLogicalWidth() <= 0) return;
    const float sw = (float)gosRendererLogicalWidth();
    const float sh = (float)gosRendererLogicalHeight();
    const float bottomBand = sh * 0.60f;
    const float renderedBandTop = sh + (bottomBand - sh) * scale;
    if (y < renderedBandTop) return;
    const float ax = sw * 0.5f;
    const float ay = sh;
    x = ax + (x - ax) / scale;
    y = ay + (y - ay) / scale;
}



// LOAD-TRANSITION-DISPLAY-TRUTH-1: tiny accessor so GuiRuntime (which does
// not include gameos.hpp) can read the synchronously-updated drawable size.
void __stdcall gos_GetEnvironmentDrawableSize(int* w, int* h)
{
    if (w) *w = Environment.drawableWidth;
    if (h) *h = Environment.drawableHeight;
}
