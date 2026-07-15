# INFO-IMGUI-SLICE-1 — port the in-mission Info window to ImGui

Goal: `MC2_UI_IMGUI_INFO=1` renders the in-mission unit-info panel as ImGui,
matching the legacy panel (`.claude/info_window_legacy_ref.png`: RAZORBACK 30T +
green health bar + mech paperdoll + scrollbar + loadout list). Default legacy.

Worktree `A:/Games/mc2-ui-phase1`, branch `claude/ui-phase1-integration`, deploy
`A:/Games/mc2-opengl/releases/mc2-win64-v0.5.0`. This panel is PROCEDURAL (NOT
tile-stitched) — reproduce the draw, no baked frame. `PauseWindow::renderImGui`
(`code/pausewindow.cpp`) is the structural reference for the gate + delegation.

## Source of truth (read these)

- `code/infowindow.cpp` `InfoWindow::render` (L358) + `drawScrollingStuff` (the
  loadout list) + `drawName`. Layout constants are `InfoWindow::SCROLLBOX*`,
  `HEALTH*`, `NameRect`, loaded from the fit in `InfoWindow::init` (L102) —
  `InfoHeaderBox*`/`ScrollContentBox*`. Authored InfoWindow rect: X=3 Y=434 W=162
  H=163 (`[InfoWindow]` in `.../legacy/game/buttonLayout800.fit`).
- Legacy reference: `.claude/info_window_legacy_ref.png`. Verify fixture:
  `MC2_BOOT_INFO=700` (selects the first home mech + INFO tab headless).

## Slice-1 scope (defer the mech paperdoll icon)

Reproduce, bound to the live `pUnit` (`Mover*`) read EACH frame:
1. **Panel bg + header box** — dark blue (legacy `InfoHeaderBoxColor` 0xff002f55) at
   the header rect; the content box below it.
2. **Unit name** — `pUnit->getIfaceName()` (or `IDS_NOUNIT` when `!pUnit`), the
   `drawName` styling (blue title text).
3. **Health bar** — `pUnit->getAppearance()->barStatus` (clamp ≤1). Color:
   ≥0.5 `SB_GREEN`, >0.2 `SB_YELLOW`, else `SB_RED`. Draw a dim full-width bg
   (`color & 0xff7f7f7f`) then a filled fg to `width*barStatus` (ImGui
   `AddRectFilled` in the window draw list, matching the legacy `HEALTH*` rect).
4. **Loadout list** in an ImGui child region WITH a scrollbar (ImGui's scrollbar
   REPLACES the hand-rolled thumb/scroll-buttons — that's the win). Rows =
   EXACTLY what `drawScrollingStuff` enumerates: iterate `pUnit->inventory` from
   `pUnit->numOther` to `numOther+numWeapons` (+ the component ranges it walks),
   dedup by `masterID` with counts, resolve each name via `cLoadString(
   IDS_COMP_ABBR0 + masterID, ...)`, and color by the same category (`textColors[]`
   = {0xff6E7C00, 0xff005392, 0xffA21600}). To GUARANTEE content parity, FACTOR the
   row-gathering out of `drawScrollingStuff` into a shared helper
   `gatherLoadoutRows(pUnit) -> vector<{std::string text, uint32_t argb}>` and call
   it from BOTH the legacy draw and the ImGui path (do not reimplement the dedup —
   reuse it, or the two paths will drift).
5. **DEFER**: the mech paperdoll icon (`icon->renderUnitIcon`/`renderUnitIconBack`)
   — a 3D/sprite render; leave its area blank in slice-1, note it for slice-2.

Layout: position the ImGui window at the authored InfoWindow rect (3,434,162,163)
mapped through `gos_ComputeHudCanvasBox` (authored*bw/800 + bx). Borderless,
no-decoration, dark bg. Font: ImGui default is fine for slice-1 (legacy uses a
small bitmap font; exact font match is slice-2 polish).

## Wiring

- `code/infowindow.cpp`: add `static bool imguiInfoEnabled()` (env
  `MC2_UI_IMGUI_INFO`, memoized) + `void renderImGui()`. In `InfoWindow::render()`,
  `if (imguiInfoEnabled()) { renderImGui(); return; }` at the top — gate OFF =
  legacy path byte-identical. Includes: `imgui.h`, `gameos.hpp`
  (gos_ComputeHudCanvasBox), whatever `pUnit`/inventory needs (already in the TU).
- Register `MC2_UI_IMGUI_INFO` (Trace) in `RenderCore/RendererFeatureRegistry.h`;
  `bash scripts/check-env-registry.sh` clean.

## Verify (agent, no clicks)

```
MC2_UI_IMGUI_INFO=1 MC2_BOOT_INFO=700 MC2_SCREENSHOT_AT_FRAME=4200 \
MC2_SCREENSHOT_SOURCE=backbuffer MC2_SCREENSHOT_PATH=<abs>.tga \
MC2_NO_LAUNCHER=1 MC2_LOG=1 ./mc2.exe -mission mc2_01
```
Convert TGA→PNG, EYEBALL vs `.claude/info_window_legacy_ref.png`: name, health-bar
color/length, and the loadout rows (names + counts + category colors) should match.
Iterate. Then tier1 smoke (gate OFF, `--exe <install>/mc2.exe`) = mission-stable.
Kill by exact PID the moment the screenshot lands; one game at a time; zero-count
before/after.

## Rules

COMMIT FIRST → build → deploy (0.5.0 lane, `--allow-new-target`,
`--build-dir build64/RelWithDebInfo`); after deploy `cp
.claude/run-with-log.bat.deploy-backup <lane>/run-with-log.bat`. Build = the VS
CMake full path (mc2-main/CLAUDE.md). Gate OFF byte-identical. If you hit a
blocker (helper factoring breaks the legacy draw, a symbol not reachable), STOP
and report the exact error. Report: the info PNG you eyeballed vs the ref, the
smoke result, commit sha(s); append a one-line status to
`.claude/HANDOFF-ui-phase1-bugs-2026-07-05.md`.
