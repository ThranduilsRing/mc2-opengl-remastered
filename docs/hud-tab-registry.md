<!-- doc-status: current | reviewed:2026-10-11 -->
# HUD tab registry (MC2_HUD_IMGUI_TABS, slice C)

Data-driven command-bar tab strip for the in-mission HUD
(IN-MISSION-HUD-IMGUI-SCOPE-1 slice C). Replaces the hardcoded 3-tab model
(`TACMAP_TAB/INFO_TAB/VEHICLE_TAB` consecutive enum ids + four `TACMAP_TAB+3`
loops + a hand-written cycle table) with `HudTabs::Registry`
(`code/hudtabregistry.{h,cpp}`).

## Gates

- `MC2_HUD_IMGUI_TABS=1` — registry consulted. **OFF (default): every legacy
  branch runs unchanged**; the registry is registered but never read.
- `MC2_HUD_TABS_TEST=1` — additionally registers a 4th "test" witness tab
  (needs the main gate to be visible). Left in deliberately as the worked
  example.
- `MC2_HUD_SIM_SPEED=1` — SIM-SPEED-BUTTON-1: registers a "speed" tab (needs
  the main gate to be visible) with 1x/2x/4x buttons driving the runtime
  sim-speed multiplier behind `MC2_SIM_SPEED` (`simSpeedSetMultiplier`,
  `code/mission.cpp`). Buttons disabled in multiplayer; the multiplier resets
  to the env seed at every `Mission::init`. First real consumer of the
  modder recipe below.
- Pin-to-legacy is shared with slice B (`HudImGui::pinnedToLegacy()`): any
  ImGui failure drops the whole HUD-ImGui family back to legacy for the
  session.

## Sites routed through the registry (gate ON)

All in `code/controlgui.cpp`, each as `if (hudTabsGateOn()) {...} else {legacy verbatim}`:

1. **Render dispatch** (`ControlGui::render`) — `activeTab()->render(this)`.
2. **Top-draw loop** — pressed legacy tab button re-render + ImGui chrome for
   buttonless tabs (`renderHudTabChromeImGui`).
3. **Per-tab update + help text** (`ControlGui::update`) — `activeTab()->update`
   \+ registered `helpTextHeaderID/helpTextID` when the mouse is in the tac area.
4. **`isOverTacMap`** — `activeTab()->countsAsTacMap`.
5. **`handleClick` tab cases** — `setActiveHudTab(indexOfButton(ID), "click")`.
6. **`updateVehicleTab`** — "vehicle tab active" verdict from the registry.
7. **`switchTabs`** — `cycle(direction)`; registration order IS the cycle order
   (tacmap → vehicle → info, matching the legacy table both directions).
8. **Default press** (button-init/`swapResolutions`) — `initHudTabRegistry()`
   re-registers and resets active to index 0 (tacmap), matching the legacy
   `press(TACMAP_TAB)`.

Every tab switch logs `[HUD-TABS v1] dispatch tab '<id>' (index N, via click|cycle|chrome-click)`
to stderr (visible with `MC2_LOG=1`).

## Adding a new tab (modder recipe)

No enum widening, no `+3` loop edits, no button-array surgery. In
`ControlGui::initHudTabRegistry()` (or any code that runs there) add:

```cpp
HudTabs::Tab t;
t.id = "mytab";            // stable string id (string literal)
t.buttonCommand = -1;      // -1 = ImGui-drawn chrome; >=0 = reuse a legacy ControlButton id
t.render = &myRenderThunk; // void(*)(void* user) — user is ControlGui*; draw the tab BODY
t.update = &myUpdateThunk; // optional per-frame update while active (or 0)
t.helpTextHeaderID = IDS_MYTAB;      // rollover help (0 = none)
t.helpTextID = IDS_MYTAB_DESC;
t.countsAsTacMap = false;  // does isOverTacMap() treat this tab as the tac map?
hudTabRegistry.add( t );
```

Rules that keep you honest:

- **Registration order = TAB-hotkey cycle order.** Insert where you want the
  hotkey to reach it.
- **Body drawing**: use `HudImGui::beginPanel(id, l, t, r, b)` over authored
  HUD-space coords (Environment.screenWidth/Height space). For anything in the
  bottom band (command bar / tac area) run the corners through
  `HudImGui::hudShrinkPoint(x, y, rectCentroidY)` FIRST — that models the
  legacy `s_hud_scale` bottom-band shrink (flushHUDBatch) so your chrome stays
  glued to the legacy buttons at any HUD scale.
- **Chrome**: with `buttonCommand = -1` the strip button is drawn for you (an
  ImGui button right of the last legacy tab, shrink+canvas-transformed).
  With a legacy `buttonCommand` the stock ControlButton renders as always.
- **Hit-tests**: never hand-roll rect tests against raw ImGui mouse coords.
  ImGui panels positioned via the helper hit-test natively; legacy-space
  math must use `userInput->getMouseHudX/Y()`.
- **No save/MP surface**: tab selection is presentation-only. Keep it that way.

The double-gated `"test"` tab in `initHudTabRegistry()` (gate
`MC2_HUD_TABS_TEST`, body `hudTabRenderTest`) is the living worked example.

## Tests

`tests/unit/test_hud_tab_registry.cpp` locks registration order, both cycle
directions vs the legacy table, 4th-tab extension, capacity/bounds. Pure
offline (`hudtabregistry.cpp` is a leaf TU in the mc2_tests target).
