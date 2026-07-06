# IN-MISSION-HUD-IMGUI-SCOPE-1

Read-only recon. No code changed. Scope doc for porting the legacy GameOS 2D
in-mission HUD (command bar, tac map, info window, message area, force group
bar, objectives, pause menu, hotkeys) to the ImGui defs-page architecture that
already shipped for front-end menus (`claude/ui-phase1-integration`,
`docs/ui-imgui-port-guide.md`).

## 1. INVENTORY

| Component | File(s) / entry points | Draw path | Hit-test path | Readability |
|---|---|---|---|---|
| **Command bar** (mode buttons, range/jump/run/guard, tab strip) | `code/controlgui.cpp` `ControlGui::render` (~L900-1010), `ControlGui::update` (~L1600-1908), `ControlGui::handleClick` (switch at L2230-2300+) | `ControlButton::render()` (sprite/quad-based, `gos_DrawQuads`/UV atlas via `ButtonData::makeUVs` L101), `gos_TextDraw` for labels (L713-716, L804-806, L859-861) | `ControlGui::inRegion` (`controlgui.h` L130), rect tests at `controlgui.cpp` L1858-1861, L1916-1919 vs `userInput->getMouseHudX/Y` | **Marginal** — button art is legacy low-res sprites at 800x600 authoring space, magnified by HUD canvas scale; labels use `gosFontHandle` (global small bitmap font, no literal size param visible at call site — size lives in the font resource, not code) |
| **Tac map** | `code/gametacmap.cpp` (`GameTacMap::render`), embedded via `ControlGui::tacMap` (`controlgui.h` L174) | Texture blit of minimap + `gos_DrawLines`/quads for markers/overview icons (`controlgui.cpp` L916-978 tactical-overview card/marker draws) | `gametacmap.cpp` L77-81 uses `userInput->getMouseHudX/Y` explicitly (comment at L77 warns against raw `getMouseX/Y`) | **Bad (worst-offender candidate for world-anchored complexity)** — world-projected markers + overview cards + objective markers all composited by hand each frame; any misalignment with canvas remap breaks click accuracy (this file has an explicit inline warning about that failure mode) |
| **Force group bar** | `code/forcegroupbar.cpp` `ForceGroupBar::render`/`update`; embedded at `controlgui.h` L175 | Icon/sprite blits per squad, `gos_TextDraw` for numbers | `forcegroupbar.cpp` L540-541 `getMouseHudX/Y`; comments at L170, L346 note it "must live on the 16:9 canvas with the rest of the HUD" | **Marginal** — small icon glyphs, dense at 800x600 scale |
| **Info window** (unit info panel) | `code/infowindow.cpp` `InfoWindow::render` (L358), `code/infowindow.h` | Hand-built scrollbar quad math (L375-423), `drawName`, health-bar quad draw (`barStatus` L437+), `gos_DrawLines` (L959) | `infowindow.cpp` L496-497 `getMouseHudX/Y` | **Bad** — most stateful hand-rolled widget in the HUD (custom scrollbar drag math, no reusable widget abstraction); best "worst offender" candidate together with tac map, but self-contained enough to be a good ImGui port target (single class, single render entry point, no world-projection) |
| **Pause window / menu** | `code/pausewindow.cpp` `PauseWindow::render` (L261), `PauseWindow::update` (L72), `PauseWindow::init` (L295) | Two `drawRect` background quads (L265-266) + button/static loops (L268-276) + one `headerFont.render(...)` call (L280-284) | `pausewindow.cpp` L105-106 `getMouseHudX/Y`, explicit comment (L100-104) explaining why HUD-space hit-test is required | **Marginal, but structurally the simplest HUD panel** — no scrolling, no world anchoring, no per-frame game-state binding beyond a slide-in animation (`moveInfo` table L32-42); ships/quits/aborts are simple state flags |
| **Objectives display** | Marker/blink logic lives in `code/objective.cpp` (`CObjective::RenderMarkers` L269, `RenderOverviewMarker` L321); the actual on-screen objectives-list panel is driven from `code/controlgui.cpp` via `renderObjectives` flag (`ControlGui::startObjectives` L1120, checked at L1258, L1896, L2286-2292) — **the per-item text-list `CObjective::Render` is dead/commented code** (`objective.cpp` L2237-2265, wrapped in `/* ... */`) | Tac-map marker draw only in current code; the flyout list panel is composited through `controlgui.cpp`'s generic static/button render loop, not a dedicated function | Shares `ControlGui::inRegion`/button hit-test | **Bad / unclear** — the commented-out `CObjective::Render` suggests the text-list path was already partially retired or moved; needs a live-game screenshot before design, not just static read (flag in slice 0 recon) |
| **Message area** | Not a separate file — text/log messages are drawn inline within `controlgui.cpp`'s render pass (no dedicated `MessageArea` class found; searched for "MessageArea", "messagearea", "chatEdit" — found `chatEdit`/`personalEdit`/`playerNameEdit` at L3304-3322, which are MP chat edit boxes, not the mission message log) | `gos_TextDraw` inline, same pass as command bar | n/a (display-only) | **Marginal** — smallest/simplest surface, likely foldable into the command-bar slice rather than its own slice |

**Worst offenders, ranked**: (1) **Info window** — hand-rolled scrollbar math with zero reuse, most "why does this feel like Windows 95" energy; (2) **Tac map** — world-projected markers/overview cards are the highest-hazard hit-test surface (file has its own inline warning comment about canvas-remap mouse-space bugs); (3) **Command bar tab strip** — not unreadable so much as *closed* (no data-driven way to add a tab, see Section 2).

## 2. PORT MODEL

### Beyond what front-end pages needed

- **Per-frame game-state binding.** Front-end defs pages bind to static menu data; a HUD page must read live `Mover*`/`CObjective*`/team state every frame (e.g. `InfoWindow::render` reads `pUnit->getAppearance()->barStatus` live, `controlgui.cpp` L974-977 iterates `Team::home->objectives` every frame). An ImGui HUD page needs a per-frame "bind" callback, not a static schema.
- **World-anchored elements.** Tac map markers and the tactical overview cards project world positions through `Camera` math (`controlgui.cpp` L916-977, `objective.cpp` `RenderOverviewMarker` L321) — these are not layout-space widgets and cannot be pure defs-page content; they need a "world overlay" element type that still respects the aspect-anchor canvas for its frame/border chrome while computing marker positions independently.
- **scaleExempt / canvas-exempt interplay.** `gameos.hpp` L2497-2501 documents a **third state** beyond front-end's binary canvas-on/off: `gos_SetHudCanvasExemptMode(mode)` where `-1`=default shrink-exempt bracket (cursor/modal/world-anchored), `0`=force canvas remap (used by the force-group bar, which is shrink-exempt but still hit-tested via `getMouseHudX`), `1`=force exemption. A new ImGui HUD page must pick the correct bracket per element or hit-tests will silently drift on non-16:9 displays — this is the single most likely regression source (see Risk Ladder).
- **Perf cost model.** HUD draws every frame at 100+ fps under Tracy's 100ns-zone-floor discipline (`CLAUDE.md` "Profiling" section); `controlgui.cpp` already wraps sub-panels in `ZoneScopedN`/`CgScope` (e.g. L1892 `"CGui.VehicleTab"`). ImGui itself is cheap per-widget but the HUD's *current* legacy path is a flat vertex-batch flush (`flushHUDBatch`, `gameos_graphics.cpp` L7355) with no per-widget CPU tree-walk; porting to ImGui trades batched-quad throughput for retained-mode immediate-tree overhead — needs its own Tracy zone comparison before/after per slice, not an assumption.

### TAB SEAM

Exact location: **`ControlGui::Commands` enum**, `code/controlgui.h` L179-203:

```cpp
enum Commands {
    ...
    OBJECTIVES_COMMAND,
    SAVE_COMMAND,
    TACMAP_TAB,     // L193
    INFO_TAB,       // L194
    VEHICLE_TAB,    // L195
    ...
};
```

Switching logic:
- **Render dispatch**: `controlgui.cpp` L980-985 — `if (getButton(TACMAP_TAB)->state & PRESSED) tacMap.render(); else if (VEHICLE_TAB pressed) renderVehicleTab(); else renderInfoTab();` — a **linear if/else chain on button press-state**, not a clean enum switch.
- **Click handling**: `controlgui.cpp` L2273-2284 — `handleClick`'s switch `case TACMAP_TAB: case INFO_TAB: case VEHICLE_TAB:` un-presses all three then presses the clicked one (mutual-exclusion via button state, not a mode variable).
- **Tab-cycle (keyboard/hotkey)**: `controlgui.cpp` L3584-3613 — a hand-written next/prev cycle through the 3 IDs.
- Tabs are **hardcoded to exactly 3** (`for (i = TACMAP_TAB; i < TACMAP_TAB + 3; i++)` appears 4 times: L1004, L2276, L3585) — this is the concrete evidence for "closed, not data-driven": adding a 4th tab today means widening every one of these loops plus the enum plus new render/click functions.

**Minimal contract sketch for a data-driven tab** (shape only, not full code):
- `tab_id` (string, not enum ordinal — so mod content doesn't collide with engine IDs)
- `label` / icon asset ref (defs-page element, same PNG-magenta-key convention as front-end)
- `render_callback` — analogous to today's `tacMap.render()` / `renderVehicleTab()` / `renderInfoTab()`, but registered rather than if/else-chained
- `is_active` — replaces `getButton(X)->state & PRESSED` polling; owned by a tab-group controller, not per-button state
- `hit_test_rect` or `defs` element key — for click routing, reusing `getDefsElementScreenRect` from the existing bridge (`docs/ui-imgui-port-guide.md` L58-60) instead of the hand-rolled rect tests at `controlgui.cpp` L1858-1861/1916-1919
- `canvas_exempt_mode` — explicit per-tab value from the 3-state bracket above, defaulting to `-1` (inherit)

This directly extends the existing `UiDefs::replacementPathForLegacyFit` auto-load pattern (`gui/UiDefs.cpp` L1728, declared `gui/UiDefs.h` L28) and the coverage opt-out mechanism (`LogisticsScreen::renderUncoveredLegacyWidgets`, `gui/logisticsscreen.cpp` L481, `gui/logisticsscreen.h` L116) — a HUD tab-group would be a `LogisticsScreen`-shaped construct scoped to the command-bar's screen-space rect instead of the whole window.

## 3. RISK LADDER

**Slice 0 — legacy-path readability quick wins (no ImGui port)**
- Gate: `MC2_HUD_READABILITY_V1` (default OFF)
- Files: font-resource entries reachable via `IDS_*` string table (`strings.res.h` — e.g. `IDS_PAUSEBUTTON800=40031` L660, `IDS_PAUSEDFONT_800`), plus any `.fit`/color constants in `code/controlgui.cpp` (kGreen `0x0000cc00` L797, tower-cyan `0x0033ddff` L463, band colors L725)
- **Feasibility confirmed**: font scale is NOT a hardcoded C++ literal — `gos_Text*` call sites in `controlgui.cpp` universally pass `guiFont.getSize()` / `helpFont.getSize()` (L1071, L1326, L1364, L2826, L2833 etc.), meaning size is already indirected through an `aFont` object initialized from a resource ID. Quick win = swap in a larger `aFont` resource behind the gate (data change, not logic change), plus recolor the darkest text constants found above.
- Proof: dev-shell screenshot diff (`tools/dev_shell/shot.py diff`) before/after, tier1 smoke for regression (readability-only change should have zero smoke impact)
- Hazards: none expected — pure data/constant swap, no layout/hit-test touched

**Slice 1 — pause menu ImGui port (pattern-prover)**
- Gate: `MC2_HUD_IMGUI_PAUSE` (default OFF, legacy-fallback-on-failure)
- Files: `code/pausewindow.cpp`/`.h` (new ImGui page + pin-to-legacy block analogous to `mcui_mr_layout`/`mcl_loadingscreen*` pattern in `UiDefs::replacementPathForLegacyFit`)
- **Justification for going first**: smallest surface (`PauseWindow::render` is 30 lines, L261-293), no scrolling, no world anchoring, no per-frame live game-state read beyond a slide-in float (`moveInfo`, L32-42/L121-130) and two yes/no dialog flags — closest thing to a pure front-end page already living in-mission.
- Proof: `MC2_DEV_SHELL=1` screenshot at pause, tier1 smoke (mc2_01/mc2_24 minimum, since pause is reachable in every mission)
- Hazards: hit-test must go through `getMouseHudX/Y` equivalent (canvas-aware) not raw ImGui io mouse — this is the exact bug class the L100-104 comment in `pausewindow.cpp` warns about for the legacy path; no save/MP impact expected (pause state is presentation-only)

**Slice 2 — info window ImGui port (worst offender)**
- Gate: `MC2_HUD_IMGUI_INFOWINDOW` (default OFF)
- Files: `code/infowindow.cpp`/`.h`
- Proof: dev-shell screenshots across several unit types (health bar variety), tier1 smoke
- Hazards: custom scrollbar math (L373-423) must be replaced by ImGui's native scroll region — behavior parity (drag-to-scroll feel) is a UX judgment call, not just a visual diff, so this slice needs a human confirm step, not just pixel diff. Live per-frame binding to `pUnit` (can change or go null mid-frame per `infoLength < scrollBarLength || !pUnit` check L388) must be preserved exactly.

**Slice 3 — command-bar tabs port (the tab-seam enabler)**
- Gate: `MC2_HUD_IMGUI_TABS` (default OFF)
- Files: `code/controlgui.cpp`/`.h` (the `Commands` enum region L179-203, render dispatch L980-985, `handleClick` L2273-2284, cycle logic L3584-3613)
- This is the point of the whole exercise per the user's ask ("ability to add new tabs"). Implements the minimal contract sketched in Section 2.
- Proof: tier1 smoke (command bar is load-bearing every mission — treat as highest-blast-radius slice), dev-shell screenshot of all 3 existing tabs + hotkey cycle, plus a throwaway 4th test tab added purely to prove the data-driven path works before removing it
- Hazards: **highest risk in the ladder**. (a) Hit-test drift at non-16:9 aspect — the tab buttons currently rely on hand-rolled rects (L1858-1861/1916-1919) that already required careful HUD-space correction once (see `isOverTacMap` comment L1914-1915); a rewrite risks reintroducing the same bug class. (b) `canvas_exempt_mode` interaction — force-group bar already needs the non-default `0` bracket (shrink-exempt but still remapped) per `gameos.hpp` L2499; a new tab-group controller must expose this per-tab, not assume the default. (c) No save/MP impact expected — tab selection is UI-local, not part of mission/save state.

**Slice 4 — tac map port (hardest, do last)**
- Gate: `MC2_HUD_IMGUI_TACMAP` (default OFF)
- Files: `code/gametacmap.cpp`, plus overview-card/marker draw code embedded in `controlgui.cpp` L916-978 and `objective.cpp` `RenderOverviewMarker` L321
- Proof: dev-shell screenshots across multiple missions (marker density varies), tier1 full tier, plus a manual click-accuracy pass at non-16:9 window sizes (resize test, `MC2_WINDOW_RESIZABLE` already supports live resize per the port guide)
- Hazards: world-projected content mixed with canvas-anchored chrome is the hardest combination in the whole HUD; the file already carries an inline comment (L77) warning against raw mouse coords, meaning this exact failure mode has bitten someone before. No save/MP impact expected (map is presentation-only), but multiplayer *shared visibility* logic (fog-of-war, ally markers) must not regress — flag for an MP-specific smoke pass beyond tier1's SP-only missions.

## 4. AGENT-DOABLE VS MODDER-NEEDED

- **Agent-doable solo**: Slice 0 (data/constant swap), Slice 1 (pause menu — fully mechanical, small surface, existing defs-page pattern to copy), Slice 3's *code* plumbing (enum/dispatch refactor) though the new tab's *content* needs an artist-authored icon.
- **Needs human art/authoring input**: Slice 2's scrollbar UX judgment call (feel, not just pixels); any new tab's icon/label art (magenta-keyed PNG per existing convention, `docs/ui-imgui-port-guide.md` L8-20); Slice 4's marker iconography if it changes from the current sprite atlas.
- **`ui_editor/` tool** (found at `A:/Games/mc2-main/ui_editor/`, CMake target confirmed via `ui_editor/CMakeLists.txt` and built artifacts under `build64/out/ui_editor/`; source files `UiEditorMain.cpp`, `UiEditorFitLoader.cpp`, `UiEditorImageCache.cpp`, `UiEditorVersion.cpp`) currently authors static `.fit` defs pages for front-end screens. For HUD pages specifically it would need to additionally support: (a) previewing a page against **live or recorded per-frame game state** (front-end pages are static, HUD pages read `Mover`/`CObjective`/team data every frame — the editor has no such feed today); (b) authoring **world-anchor elements** (tac-map markers) that don't fit the flat defs-page canvas model at all; (c) a way to author/preview the **canvas-exempt bracket** (`-1`/`0`/`1`) per element, which front-end pages never needed since they're always canvas-active. None of this exists yet in the `ui_editor/` source read during this recon — it is a genuine gap, not just an extension.

## Not found / searched but absent

- No dedicated `MessageArea` class — searched `code/controlgui.cpp`, `code/missiongui.cpp` for "MessageArea"/"messagearea"/"message log"; message/log text appears to be drawn inline in the command-bar render pass, not a separate component.
- `docs/active_campaigns.md` and `docs/renderworld_arc_status.md` — grepped for "HUD", "command bar", "tac map", "info window", "pause menu", "objectives"; no hits in either file (searched via Grep tool; both files' content is renderer-arc-focused, not UI). No in-flight duplicate work found there.
