# PAUSE-IMGUI-SLICE-2 — wire the baked legacy frame into the ImGui pause page

Goal: the `MC2_UI_IMGUI_PAUSE=1` pause menu must look like the LEGACY pause panel
(hazard-stripe frame + blue metal + blue button slots), not a flat ImGui window.
The offline reconstruction (UI-ART-RECONSTRUCT-1, `tools/ui_art_reconstruct.py`)
bakes the legacy panel's 17 stitched tiles into ONE PNG; this slice samples that
PNG as the pause window's background texture under the buttons.

Worktree `A:/Games/mc2-ui-phase1`, branch `claude/ui-phase1-integration`.
Deploy lane `A:/Games/mc2-opengl/releases/mc2-win64-v0.5.0` (`--allow-new-target`,
`--build-dir build64/RelWithDebInfo`). After EVERY deploy, restore the bat:
`cp .claude/run-with-log.bat.deploy-backup <install>/run-with-log.bat`.
Build: the VS CMake full path (see CLAUDE.md). RULE: commit → build → deploy.

## Step 1 — bake + deploy the frame asset

```
I=A:/Games/mc2-opengl/releases/mc2-win64-v0.5.0
py -3 tools/ui_art_reconstruct.py \
   --fit "$I/data/defs/ui/legacy/game/buttonLayout800.fit" \
   --art-dir "$I/data/art/gui" --panel Pause \
   --out "$I/data/art/gui/mc2_pause_frame.png"      # FULL 800x600 (NOT --crop:
```
Use the FULL (un-cropped) 800x600 PNG so runtime UVs are trivial: the panel
occupies authored rect (638,86)..(800,325). We sample that sub-rect via UVs.
Also drop a copy in the source tree if `data/art/gui/` is git-tracked there so
deploy re-ships it; else the install copy above is the deploy target (like the
run-with-log.bat restore, the asset lives in the install). Confirm the file is
present in the install's `data/art/gui/` before launching.

## Step 2 — load the baked PNG as a GL texture (runtime, once)

In `code/pausewindow.cpp` `renderImGui()`, cache the GL texture id in a static,
loaded on first use (the texture manager is up by the time the pause menu draws):

```cpp
static unsigned s_frameGlTex = 0;
static bool s_frameTried = false;
if (!s_frameTried) {
    s_frameTried = true;
    // loadTexture accepts a .png path; the atlases in the same dir are PNG.
    int mcH = mcTextureManager->loadTexture(
        "data/art/gui/mc2_pause_frame.png", gos_Texture_Alpha, 0, 0, 0);
    if (mcH) {
        DWORD gosID = mcTextureManager->get_gosTextureHandle((DWORD)mcH);
        if (gosID && gosID != 0xffffffff)
            s_frameGlTex = gos_GetGLTextureName(gosID);
    }
}
```
Includes already present via pausewindow.cpp's existing headers; add
`#include "txmmgr.h"` (mcTextureManager) and confirm `gos_GetGLTextureName` is
declared (gameos.hpp, already included this slice). If `mcTextureManager` isn't
reachable from this TU, mirror the include used by `code/controlgui.cpp`.

## Step 3 — draw the frame UNDER the buttons

`GuiRuntime::DrawUiImage` uses the FOREGROUND draw list (draws ON TOP) — wrong for
a background. Instead draw to the WINDOW draw list right after `Begin()`, before the
buttons, and make the ImGui window background transparent:

- Replace the current `ImGuiCol_WindowBg` dark tint push with a fully transparent
  WindowBg (`ImVec4(0,0,0,0)`), keep it a borderless no-decoration window.
- The window is already positioned/sized to the canvas-mapped panel rect
  (`px,py,pw,ph` from `gos_ComputeHudCanvasBox`, authored 636/150 origin — NOTE:
  the current code uses 636,150; the ACTUAL legacy panel bbox is origin (638,86)
  size 162x239 per the fit. RE-DERIVE px/py/pw/ph from (638,86,162,239) so the
  frame and window align exactly).
- After `Begin()`:
```cpp
if (s_frameGlTex) {
    ImVec2 pmin(px, py), pmax(px + pw, py + ph);
    // full-800x600 PNG: UV sub-rect for authored (638,86)..(800,325)
    ImVec2 uv0(638.0f/800.0f, 86.0f/600.0f), uv1(800.0f/800.0f, 325.0f/600.0f);
    ImGui::GetWindowDrawList()->AddImage(
        (ImTextureID)(uintptr_t)s_frameGlTex, pmin, pmax, uv0, uv1);
}
```
(If a baked --crop PNG is used instead, uv0=(0,0) uv1=(1,1) and load that file.)

## Step 4 — buttons transparent over the baked slots + centered labels

The baked frame already contains the 7 blue button slots (normal state). Make the
ImGui buttons invisible (transparent Button/Hovered/Active bg = the slot art shows)
and place them over the slots with the label centered. Button rects (authored, from
`[PauseButtonN]` XLocation/YLocation/Width/Height in the fit): read them from the
fit at build time OR hardcode from a one-time dump. PauseButton0 = (664,116,128,22);
each subsequent button steps down by the fit's spacing (dump all 7). For each:
map authored (bx_i,by_i,bw_i,bh_i) → display px via the same canvas box, position an
ImGui `InvisibleButton` (or a Button with transparent colors) there, and draw the
centered label with `ImGui::GetWindowDrawList()->AddText` (or a Selectable). Keep
the existing `handleClick(id)` call + the disabled-state mirror from the legacy
buttons. Hover feedback: optionally tint the slot on hover via an AddRectFilled
overlay — nice-to-have, not required for slice-2.

Simplify option (acceptable for slice-2 if the above is fiddly): keep the current
real ImGui Buttons but push transparent bg colors so ONLY the label text renders
over the baked slots. Verify the labels land centered in the slots.

## Step 5 — verify (agent, no clicks)

```
MC2_UI_IMGUI_PAUSE=1 MC2_BOOT_PAUSED=600 MC2_SCREENSHOT_AT_FRAME=4200 \
MC2_SCREENSHOT_SOURCE=backbuffer MC2_SCREENSHOT_PATH=.../pause_chrome.tga \
MC2_NO_LAUNCHER=1 MC2_LOG=1 mc2.exe -mission mc2_01
```
Convert TGA→PNG, EYEBALL against `.claude/pause_window_legacy_ref.png`: the frame,
hazard stripes, blue slots, and centered labels should match the legacy panel.
Iterate origin/UV/label-placement until it matches. Kill the process by exact PID
the moment the screenshot lands; zero-count before/after; ONE mission at a time.
Then a tier1 smoke (gate OFF) to confirm mission-stable (`--exe <install>/mc2.exe`).

## Guardrails

- All new behavior under `MC2_UI_IMGUI_PAUSE` — gate OFF = legacy sprite panel,
  byte-identical. The frame-load static must not run when the gate is off.
- Commit the code + a one-line note appended to
  `.claude/HANDOFF-ui-phase1-bugs-2026-07-05.md`. Do NOT commit the baked PNG if
  `data/` is gitignored (it's regenerable via the tool; document the bake command).
- Report back: the pause_chrome PNG you looked at, the smoke result, and any
  origin/UV values you had to correct from this spec.
