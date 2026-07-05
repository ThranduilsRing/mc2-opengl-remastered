# ImGui UI port — integration guide (for the UI modder)

State as of 2026-07-05 on `claude/ui-phase1-integration`. This is the map of
every seam the engine now provides for the port, so you can keep converting
screens without re-discovering plumbing. Bug-fix history lives in
`.claude/HANDOFF-ui-phase1-bugs-2026-07-05.md`.

## Transparency: PNG magenta keying is engine-side now

`PNG-MAGENTA-KEY-1` (gameos_graphics_texture.cpp): for **PNG sources only**,
exact-magenta pixels (`FF00FF`) are mapped to transparent black at decode,
before format detection — so a PNG with baked legacy magenta keys out exactly
like the old art, and a PNG with real alpha keeps working. You do NOT need to
convert PNGs to a magenta format or vice versa; export either way.

- Applies to both loose-file and texture-cache (from-memory) loads.
- Stock is untouched by construction (stock ships zero PNGs).
- Killswitch: `MC2_PNG_MAGENTA_KEY=0`.
- Halo note: keyed pixels become transparent BLACK, so bilinear edges halo
  dark, not pink.

## Per-screen migration contract

1. **Auto-load**: a legacy screen picks up your page automatically when
   `data/defs/ui/packages/default/game/<legacy-fit-stem>.fit` exists
   (`UiDefs::replacementPathForLegacyFit`). No code change per screen.
2. **Pin-to-legacy**: unfinished pages are pinned in that same function —
   currently `mcui_mr_layout` (`MC2_UI_DEFS_RESULTS=1` to iterate) and
   `mcl_loadingscreen*` (`MC2_UI_DEFS_LOADSCREEN=1`). When you want to work on
   one, set its env var; when it's done, delete its pin block.
3. **Coverage opt-out (no more invisible widgets)**: with a page active, any
   legacy widget your page does NOT mirror renders automatically through the
   canvas-aware gui bridge on top of the page
   (`LogisticsScreen::renderUncoveredLegacyWidgets`). Coverage = an element
   whose `legacySection` matches the control kind + index — prefix-agnostic
   (`Text13` and `MechBayTextEntry13` both cover text 13). So: mirror a
   control -> it stands down; forget one -> it still shows, correctly scaled,
   instead of vanishing.
4. **Page size**: declare the REAL design space (800x600 for legacy-derived
   pages). The AAR was broken by a converter artifact (`localHeight = 1326`
   taken from one offscreen slide-in element); elements may sit outside the
   declared space (they clip), the declared space itself must be the design
   canvas.

## The 16:9 UI canvas (aspect model)

The approved model: UI lives on a 16:9 canvas. Wider displays: centered +
black flanks (flanks are force-cleared in `GuiRuntime::Render`, nothing can
bleed). Narrower: letterboxed, scaled down. Exactly 16:9: identity.
Killswitch `MC2_UI_ASPECT_ANCHOR=0`.

- Defs pages get this free via `UiDefs` PageScale.
- Legacy/manual compositing: use `aObject::getCanvasTransform(sx, sy, ox, oy)`
  — THE transform (legacy-logical -> display pixels + pad origin). All bridge
  and preview-composite sites already use it; new code should too.
- Bridges: `aObject::beginGuiBridgeCanvas()` (quads),
  `beginTextBridge(sx, sy, fontScale, ox, oy)` (TTF text).
- Element screen rects: `getDefsElementScreenRect(key, ...)` returns TRUE
  display pixels (render offsets + canvas included) — use it to place 3D
  preview composites (`camera.drawPreviewToPanel`), see mechlopedia.cpp.

## In-mission HUD port seam

When you convert an in-mission HUD panel to ImGui:
`gos_ComputeHudCanvasBox(Environment.drawableWidth, Environment.drawableHeight, &bx,&by,&bw,&bh)`
is the exact display-pixel rect legacy chrome is remapped into — lay the panel
out inside it (authored * bw/800 + bx) and mixed legacy+ImGui HUD stays
aligned. Cursor / modal dialogs / world-anchored overlays are full-surface by
design (canvas-exempt); HUD hit-tests go through `userInput->getMouseHudX/Y`.
Full note at the declaration in `GameOS/include/gameos.hpp`.

## Free window resize

Windowed mode is user-resizable (`MC2_WINDOW_RESIZABLE=0` opts out). Every
consumer re-derives from `Environment.drawableWidth/Height` per frame — your
pages automatically re-lay out on live resize. Display size has ONE source of
truth: `GuiRuntime::GetDisplaySize` (drawable-first). Never read
`io.DisplaySize` directly for layout.

## Dev loop (fast iteration, no click-throughs)

- `MC2_DEV_SHELL=1` + `tools/dev_shell/mc2_cmd.py`:
  `ping / screenshot [--source backbuffer] / ui_reload / texture_refresh /
  reload_shaders / framegraph / get_gate / set_gate / quit`.
  `ui_reload` re-reads the active screen's loose .fit live; `texture_refresh
  --name <substr>` re-reads changed pixels live.
- Boot targets: `MC2_BOOT_TO_SCREEN=encyclopedia[_weapons|_vehicles|_buildings|_pilots]`,
  `MC2_BOOT_TO_BAY=<campaign> MC2_BOOT_TO_SCREEN=bay|purchase|loadout|launch`.
- AAR: `MC2_SHOT_RESULTS=1` (+dev shell) auto-captures
  `dev_shell_out/results_salvage.tga` / `results_promote.tga` when those
  screens actually render; reach them with the soak cheats
  (`MC2_SOAK_AUTOWIN MC2_SOAK_AUTO_PURCHASE MC2_SOAK_KILL_ENEMY
  MC2_SOAK_PILOT_PROMOTE`) — salvage lands ~30 s after boot.
  Engine-side hook: `gos_dev_shell::scheduleScreenshot(name, backbuffer)`.
- `tools/dev_shell/shot.py png|crop|diff` — TGA->PNG (+auto-delete), crops,
  pixel-diff with bbox (exit 1 on difference = usable as a gate).

## UI Editor / Viewer hookup points

- **UI Editor -> GOS renderer**: `GuiRuntime::InitEditorOpenGLOnly()` is the
  no-SDL entry (editor WGL path). The dev-shell extension seam
  (`gos_dev_shell::registerCommand`) lets the editor register its own
  commands without gameos linking back at it — same inversion the game uses
  for `ui_reload`.
- **Viewer**: wraps the Mechlopedia; everything above (boot targets, preview
  FBO composite, PNG keying) applies as-is. The preview pipeline is
  `SimpleCamera` -> fixed 800x600 offscreen FBO
  (`gos_BeginCameraPreviewRender`) -> `drawPreviewToPanel` at a defs rect;
  see mechlopedia.cpp MechScreen for the canonical wiring.

## Fonts

TTFs resolve from `assets/graphics/fonts/` (also `data/fonts/ui/`); resolver
in GuiRuntime.cpp `resolveFontFile` — add candidates there if you move them.
